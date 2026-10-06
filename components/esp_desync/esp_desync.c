// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "desync_internal.h"

static const char *TAG = "desync";

#ifndef CONFIG_ESP_DESYNC_FAKE_SNI
#define CONFIG_ESP_DESYNC_FAKE_SNI "www.iana.org"
#endif
#ifndef CONFIG_ESP_DESYNC_FAKE_TTL
#define CONFIG_ESP_DESYNC_FAKE_TTL 3
#endif
#ifndef CONFIG_ESP_DESYNC_FAKE_REPEATS
#define CONFIG_ESP_DESYNC_FAKE_REPEATS 1
#endif
#ifndef CONFIG_ESP_DESYNC_OP_DELAY_MS
#define CONFIG_ESP_DESYNC_OP_DELAY_MS 20
#endif
#ifndef CONFIG_ESP_DESYNC_SPLIT_POS
#define CONFIG_ESP_DESYNC_SPLIT_POS (-1)
#endif
#ifndef CONFIG_ESP_DESYNC_BADSEQ_OFFSET
#define CONFIG_ESP_DESYNC_BADSEQ_OFFSET (-10000)
#endif

#if defined(CONFIG_ESP_DESYNC_MODE_OFF)
#define DESYNC_DEF_MODE ESP_DESYNC_MODE_OFF
#elif defined(CONFIG_ESP_DESYNC_MODE_SPLIT)
#define DESYNC_DEF_MODE ESP_DESYNC_MODE_SPLIT
#elif defined(CONFIG_ESP_DESYNC_MODE_DISORDER)
#define DESYNC_DEF_MODE ESP_DESYNC_MODE_DISORDER
#elif defined(CONFIG_ESP_DESYNC_MODE_FAKE)
#define DESYNC_DEF_MODE ESP_DESYNC_MODE_FAKE
#elif defined(CONFIG_ESP_DESYNC_MODE_TLSREC)
#define DESYNC_DEF_MODE ESP_DESYNC_MODE_TLSREC
#else
#define DESYNC_DEF_MODE ESP_DESYNC_MODE_FAKE_SPLIT
#endif

#if defined(CONFIG_ESP_DESYNC_FOOLING_MD5SIG)
#define DESYNC_DEF_FOOLING (ESP_DESYNC_FOOL_MD5SIG)
#elif defined(CONFIG_ESP_DESYNC_FOOLING_BADSUM)
#define DESYNC_DEF_FOOLING (ESP_DESYNC_FOOL_BADSUM)
#elif defined(CONFIG_ESP_DESYNC_FOOLING_BADSEQ)
#define DESYNC_DEF_FOOLING (ESP_DESYNC_FOOL_BADSEQ)
#else
#define DESYNC_DEF_FOOLING (ESP_DESYNC_FOOL_TTL)
#endif

static const esp_desync_config_t s_default = {
    .mode = DESYNC_DEF_MODE,
    .fooling = DESYNC_DEF_FOOLING,
    .fake_ttl = CONFIG_ESP_DESYNC_FAKE_TTL,
    .badseq_offset = CONFIG_ESP_DESYNC_BADSEQ_OFFSET,
    .fake_sni = CONFIG_ESP_DESYNC_FAKE_SNI,
    .split_pos = CONFIG_ESP_DESYNC_SPLIT_POS,
    .split_pos2 = -1,
    .op_delay_ms = CONFIG_ESP_DESYNC_OP_DELAY_MS,
    .repeats = CONFIG_ESP_DESYNC_FAKE_REPEATS,
};

static esp_desync_config_t s_cfg;
static bool s_inited;
static desync_flow_t s_flow;
static int s_pending_fd = -1;

static ssize_t send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(1);
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return (ssize_t)len;
}

static ssize_t send_seg(int fd, const uint8_t *buf, size_t len, int p1, int p2, unsigned delay_ms)
{
    if (p1 <= 0 || (size_t)p1 >= len) {
        return send_all(fd, buf, len);
    }

    if (send_all(fd, buf, (size_t)p1) < 0) {
        return -1;
    }
    if (delay_ms) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    if (p2 > p1 && (size_t)p2 < len) {
        if (send_all(fd, buf + p1, (size_t)(p2 - p1)) < 0) {
            return -1;
        }
        if (delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        if (send_all(fd, buf + p2, len - (size_t)p2) < 0) {
            return -1;
        }
    } else {
        if (send_all(fd, buf + p1, len - (size_t)p1) < 0) {
            return -1;
        }
    }
    return (ssize_t)len;
}

static ssize_t tlsrec_send(int fd, const uint8_t *hello, size_t len, int p1, unsigned delay_ms)
{
    uint8_t *tmp = malloc(len + 5);
    if (tmp == NULL) {
        return send_seg(fd, hello, len, p1, -1, delay_ms);
    }

    memcpy(tmp, hello, (size_t)p1);
    tmp[3] = (uint8_t)((p1 - 5) >> 8);
    tmp[4] = (uint8_t)((p1 - 5) & 0xff);

    size_t q = (size_t)p1;
    size_t rest = len - (size_t)p1;
    tmp[q++] = 0x16;
    tmp[q++] = hello[1];
    tmp[q++] = hello[2];
    tmp[q++] = (uint8_t)(rest >> 8);
    tmp[q++] = (uint8_t)(rest & 0xff);
    memcpy(tmp + q, hello + p1, rest);
    q += rest;

    ssize_t r = send_seg(fd, tmp, q, p1, -1, delay_ms);
    free(tmp);
    if (r < 0) {
        return -1;
    }
    return (ssize_t)len;
}

static ssize_t apply_desync(int fd, const uint8_t *hello, size_t len)
{
    const esp_desync_config_t *c = &s_cfg;
    size_t sni_off = 0;
    size_t sni_len = 0;
    bool have_sni = desync_tls_find_sni(hello, len, &sni_off, &sni_len) == 0;

    int p1 = c->split_pos;
    if (p1 < 0) {
        p1 = have_sni ? (int)sni_off + 1 : 5;
    }
    if (p1 <= 0 || (size_t)p1 >= len) {
        p1 = (int)(len / 2);
    }
    if (p1 <= 0) {
        return send_all(fd, hello, len);
    }

    /* Multisplit: when the user did not pin the second position, cut again in
     * the middle of the SNI (zapret's split2 behaviour). */
    int p2 = c->split_pos2;
    if (p2 < 0 && have_sni && sni_len >= 4 &&
        (c->mode == ESP_DESYNC_MODE_SPLIT || c->mode == ESP_DESYNC_MODE_FAKE_SPLIT)) {
        p2 = (int)(sni_off + sni_len / 2);
    }

    switch (c->mode) {
    case ESP_DESYNC_MODE_OFF:
        return send_all(fd, hello, len);

    case ESP_DESYNC_MODE_SPLIT:
        return send_seg(fd, hello, len, p1, p2, c->op_delay_ms);

    case ESP_DESYNC_MODE_TLSREC:
        if (p1 <= 5) {
            return send_seg(fd, hello, len, p1, c->split_pos2, c->op_delay_ms);
        }
        return tlsrec_send(fd, hello, len, p1, c->op_delay_ms);

    case ESP_DESYNC_MODE_FAKE:
    case ESP_DESYNC_MODE_FAKE_SPLIT: {
        uint8_t fake[DESYNC_FAKE_MAX];
        size_t fl = desync_tls_build_fake(fake, sizeof(fake), c->fake_sni, hello, len, true);
        uint32_t snd = 0;
        uint32_t rcv = 0;
        if (fl > 0 && desync_pcb_get_state(s_flow.src_port, s_flow.dst_port, &snd, &rcv) == 0) {
            for (uint8_t i = 0; i < c->repeats; i++) {
                if (desync_inject_tcp(s_flow.dst_ip, s_flow.src_port, s_flow.dst_port,
                                      snd, rcv, fake, fl, c->fooling, c->fake_ttl,
                                      c->badseq_offset) != 0) {
                    ESP_LOGW(TAG, "fake injection failed");
                    break;
                }
            }
            ESP_LOGI(TAG, "fake sent: sni=%s len=%u ttl=%u fool=0x%x",
                     c->fake_sni, (unsigned)fl,
                     (unsigned)((c->fooling & ESP_DESYNC_FOOL_TTL) ? c->fake_ttl : 64),
                     (unsigned)c->fooling);
            if (c->op_delay_ms) {
                vTaskDelay(pdMS_TO_TICKS(c->op_delay_ms));
            }
        } else {
            ESP_LOGW(TAG, "no PCB state for fake, skipping");
        }
        if (c->mode == ESP_DESYNC_MODE_FAKE) {
            return send_all(fd, hello, len);
        }
        return send_seg(fd, hello, len, p1, p2, c->op_delay_ms);
    }

    case ESP_DESYNC_MODE_DISORDER: {
        uint32_t snd = 0;
        uint32_t rcv = 0;
        if (desync_pcb_get_state(s_flow.src_port, s_flow.dst_port, &snd, &rcv) == 0) {
            size_t tail = len - (size_t)p1;
            if (desync_inject_tcp(s_flow.dst_ip, s_flow.src_port, s_flow.dst_port,
                                  snd + (uint32_t)p1, rcv, hello + p1, tail,
                                  ESP_DESYNC_FOOL_NONE, 64, 0) == 0) {
                if (c->op_delay_ms) {
                    vTaskDelay(pdMS_TO_TICKS(c->op_delay_ms));
                }
                /* head via socket, then the tail is pushed again through the
                 * normal stream so that LwIP sequencing stays consistent.
                 * The server drops the duplicate, DPI sees the tail first. */
                if (send_seg(fd, hello, len, p1, -1, c->op_delay_ms) < 0) {
                    return -1;
                }
                return (ssize_t)len;
            }
            ESP_LOGW(TAG, "disorder injection failed, fallback split");
        }
        return send_seg(fd, hello, len, p1, c->split_pos2, c->op_delay_ms);
    }
    }

    return send_all(fd, hello, len);
}

esp_err_t esp_desync_init(const esp_desync_config_t *cfg)
{
    s_cfg = cfg ? *cfg : s_default;
    if (s_cfg.fake_sni == NULL) {
        s_cfg.fake_sni = s_default.fake_sni;
    }
    s_flow.dst_ip = 0;
    s_flow.src_port = 0;
    s_flow.dst_port = 0;
    s_pending_fd = -1;
    s_inited = true;

    ESP_LOGI(TAG, "init: mode=%s sni=%s ttl=%u fool=0x%x delay=%ums",
             esp_desync_mode_name(s_cfg.mode), s_cfg.fake_sni, (unsigned)s_cfg.fake_ttl,
             (unsigned)s_cfg.fooling, (unsigned)s_cfg.op_delay_ms);
    return ESP_OK;
}

void esp_desync_get_config(esp_desync_config_t *out)
{
    if (out) {
        *out = s_cfg;
    }
}

void esp_desync_set_config(const esp_desync_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    esp_desync_config_t c = *cfg;
    if ((int)c.mode < 0 || c.mode > ESP_DESYNC_MODE_TLSREC) {
        c.mode = s_cfg.mode;
    }
    if (c.fake_sni == NULL) {
        c.fake_sni = s_cfg.fake_sni;
    }
    if (c.repeats < 1) {
        c.repeats = 1;
    }
    if (c.fake_ttl < 1) {
        c.fake_ttl = 1;
    }
    if (c.op_delay_ms > 1000) {
        c.op_delay_ms = 1000;
    }
    if (c.split_pos < -1 || c.split_pos == 0) {
        c.split_pos = -1;
    }
    if (c.split_pos2 < -1 || c.split_pos2 == 0) {
        c.split_pos2 = -1;
    }
    s_cfg = c;
    ESP_LOGI(TAG, "config updated: mode=%s ttl=%u", esp_desync_mode_name(s_cfg.mode), (unsigned)s_cfg.fake_ttl);
}

static int connect_sockaddr(const char *host, const struct sockaddr_in *dst, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int r = connect(fd, (const struct sockaddr *)dst, sizeof(*dst));
    if (r != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    r = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (r <= 0) {
        close(fd);
        return -1;
    }

    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
    if (soerr != 0) {
        close(fd);
        return -1;
    }

    fcntl(fd, F_SETFL, flags);

    struct sockaddr_in local;
    socklen_t llen = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &llen) != 0) {
        close(fd);
        return -1;
    }

    s_flow.dst_ip = dst->sin_addr.s_addr;
    s_flow.dst_port = ntohs(dst->sin_port);
    s_flow.src_port = ntohs(local.sin_port);
    s_pending_fd = fd;

    ESP_LOGD(TAG, "connected %s:%u lport=%u", host, (unsigned)s_flow.dst_port,
             (unsigned)s_flow.src_port);
    return fd;
}

int esp_desync_connect_ip(const char *host, uint32_t ip_be, uint16_t port, int timeout_ms)
{
    if (!s_inited) {
        esp_desync_init(NULL);
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = ip_be;

    return connect_sockaddr(host, &dst, timeout_ms);
}

int esp_desync_resolve(const char *host, uint32_t *addrs_be, int max_addrs)
{
    if (max_addrs <= 0) {
        return 0;
    }

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        if (res) {
            freeaddrinfo(res);
        }
        return 0;
    }

    int n = 0;
    for (struct addrinfo *ai = res; ai != NULL && n < max_addrs; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET || ai->ai_addr == NULL) {
            continue;
        }
        uint32_t ip = ((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr;
        bool dup = false;
        for (int i = 0; i < n; i++) {
            if (addrs_be[i] == ip) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            addrs_be[n++] = ip;
        }
    }

    freeaddrinfo(res);
    return n;
}

int esp_desync_connect(const char *host, uint16_t port, int timeout_ms)
{
    uint32_t addrs[8];
    int n = esp_desync_resolve(host, addrs, 8);
    if (n <= 0) {
        ESP_LOGW(TAG, "DNS lookup failed: %s", host);
        return -1;
    }
    return esp_desync_connect_ip(host, addrs[0], port, timeout_ms);
}

ssize_t esp_desync_write(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    if (fd == s_pending_fd && len >= 44 && p[0] == 0x16 && p[1] == 0x03 && p[5] == 0x01) {
        s_pending_fd = -1;
        ssize_t r = apply_desync(fd, p, len);
        if (r >= 0) {
            return r;
        }
        ESP_LOGW(TAG, "desync failed, sending plain");
    }

    return send_all(fd, p, len);
}

ssize_t esp_desync_read(int fd, void *buf, size_t len)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r == 0) {
        return -2;
    }
    if (r < 0) {
        return -1;
    }

    int n = recv(fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -2;
        }
        return -1;
    }
    return n;
}

void esp_desync_close(int fd)
{
    if (fd == s_pending_fd) {
        s_pending_fd = -1;
    }
    close(fd);
}

const char *esp_desync_mode_name(esp_desync_mode_t mode)
{
    switch (mode) {
    case ESP_DESYNC_MODE_OFF: return "off";
    case ESP_DESYNC_MODE_SPLIT: return "split";
    case ESP_DESYNC_MODE_DISORDER: return "disorder";
    case ESP_DESYNC_MODE_FAKE: return "fake";
    case ESP_DESYNC_MODE_FAKE_SPLIT: return "fake_split";
    case ESP_DESYNC_MODE_TLSREC: return "tlsrec";
    default: return "?";
    }
}

esp_desync_mode_t esp_desync_mode_from_name(const char *name, bool *ok)
{
    if (ok) {
        *ok = true;
    }
    if (name == NULL) {
        if (ok) {
            *ok = false;
        }
        return ESP_DESYNC_MODE_OFF;
    }
    if (strcmp(name, "off") == 0) return ESP_DESYNC_MODE_OFF;
    if (strcmp(name, "split") == 0) return ESP_DESYNC_MODE_SPLIT;
    if (strcmp(name, "disorder") == 0) return ESP_DESYNC_MODE_DISORDER;
    if (strcmp(name, "fake") == 0) return ESP_DESYNC_MODE_FAKE;
    if (strcmp(name, "fake_split") == 0) return ESP_DESYNC_MODE_FAKE_SPLIT;
    if (strcmp(name, "tlsrec") == 0) return ESP_DESYNC_MODE_TLSREC;
    if (ok) {
        *ok = false;
    }
    return ESP_DESYNC_MODE_OFF;
}
