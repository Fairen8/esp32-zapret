// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "esp_desync.h"
#include "telegram.h"
#include "app_config.h"

static const char *TAG = "telegram";

#define TG_HOST  "api.telegram.org"
#define RESP_MAX 8192

/* Short per-candidate TCP timeout: dead Telegram IPs usually just swallow SYNs,
 * we must not waste tens of seconds on them. */
#define TG_CONNECT_TIMEOUT_MS 2500

/* Known api.telegram.org addresses; overridden/extended by CFG_TG_API_IPS.
 * Order matters: tried top to bottom, DNS is the last resort. */
static const char TG_DEFAULT_IPS[] =
    "149.154.167.220,149.154.167.191,149.154.175.50,149.154.175.53,149.154.166.110";

static char s_resp[RESP_MAX];
static char s_last_endpoint[16];
static int s_last_status;

typedef struct {
    int fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
} tg_tls_t;

static int parse_ip_list(const char *list, uint32_t *out, int max_addrs)
{
    int n = 0;
    const char *p = list;

    while (p != NULL && *p != '\0' && n < max_addrs) {
        while (*p == ' ' || *p == ',' || *p == ';') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char buf[16];
        size_t i = 0;
        while (*p != '\0' && *p != ',' && *p != ';' && *p != ' ' && i < sizeof(buf) - 1) {
            buf[i++] = *p++;
        }
        buf[i] = '\0';
        struct in_addr a;
        if (i > 0 && inet_pton(AF_INET, buf, &a) == 1) {
            out[n++] = a.s_addr;
        }
    }
    return n;
}

static void set_last_endpoint(uint32_t ip_be)
{
    const uint8_t *b = (const uint8_t *)&ip_be;
    snprintf(s_last_endpoint, sizeof(s_last_endpoint), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* Tries CFG_TG_API_IPS (or the built-in list) first, then DNS. Each candidate
 * gets a short connect timeout; first success wins. */
static int tg_tcp_connect(tg_tls_t *t)
{
    uint32_t addrs[16];
    int n = 0;

    if (CFG_TG_API_IPS[0] != '\0') {
        n = parse_ip_list(CFG_TG_API_IPS, addrs, 16);
    }
    if (n == 0) {
        n = parse_ip_list(TG_DEFAULT_IPS, addrs, 16);
    }

    for (int i = 0; i < n; i++) {
        t->fd = esp_desync_connect_ip(TG_HOST, addrs[i], 443, TG_CONNECT_TIMEOUT_MS);
        if (t->fd >= 0) {
            set_last_endpoint(addrs[i]);
            return 0;
        }
    }

    int rn = esp_desync_resolve(TG_HOST, addrs, 16);
    for (int i = 0; i < rn; i++) {
        t->fd = esp_desync_connect_ip(TG_HOST, addrs[i], 443, TG_CONNECT_TIMEOUT_MS);
        if (t->fd >= 0) {
            set_last_endpoint(addrs[i]);
            return 0;
        }
    }

    ESP_LOGW(TAG, "all telegram endpoints failed (%d pinned, %d dns)", n, rn);
    return -1;
}

static void tls_close(tg_tls_t *t)
{
    if (t->fd >= 0) {
        mbedtls_ssl_close_notify(&t->ssl);
        esp_desync_close(t->fd);
        t->fd = -1;
    }
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->entropy);
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    tg_tls_t *t = (tg_tls_t *)ctx;
    ssize_t n = esp_desync_write(t->fd, buf, len);
    if (n < 0) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    if (n == 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    tg_tls_t *t = (tg_tls_t *)ctx;
    ssize_t n = esp_desync_read(t->fd, buf, len);
    if (n == -2) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (n < 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (n == 0) {
        return MBEDTLS_ERR_NET_CONN_RESET;
    }
    return (int)n;
}

static int tls_open(tg_tls_t *t, int timeout_ms)
{
    memset(t, 0, sizeof(*t));
    t->fd = -1;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_entropy_init(&t->entropy);
    mbedtls_ctr_drbg_init(&t->drbg);

    if (mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, NULL, 0) != 0) {
        goto fail;
    }
    if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        goto fail;
    }
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    if (esp_crt_bundle_attach(&t->conf) != ESP_OK) {
        goto fail;
    }
    if (mbedtls_ssl_setup(&t->ssl, &t->conf) != 0) {
        goto fail;
    }
    if (mbedtls_ssl_set_hostname(&t->ssl, TG_HOST) != 0) {
        goto fail;
    }

    if (tg_tcp_connect(t) != 0) {
        goto fail;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (1) {
        int ret = mbedtls_ssl_handshake(&t->ssl);
        if (ret == 0) {
            break;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (esp_timer_get_time() > deadline) {
                ESP_LOGW(TAG, "handshake timeout via %s", s_last_endpoint);
                goto fail;
            }
            continue;
        }
        ESP_LOGW(TAG, "handshake failed via %s: -0x%04x", s_last_endpoint, (unsigned)-ret);
        goto fail;
    }
    return 0;

fail:
    tls_close(t);
    return -1;
}

static int parse_http_status(const char *r)
{
    if (strncmp(r, "HTTP/", 5) != 0) {
        return 0;
    }
    const char *sp = strchr(r, ' ');
    if (sp == NULL) {
        return 0;
    }
    return atoi(sp + 1);
}

static void log_api_error(int status)
{
    char desc[130] = "";
    const char *p = strstr(s_resp, "\"description\":\"");
    if (p != NULL) {
        p += 15;
        size_t i = 0;
        while (p[i] != '\0' && p[i] != '"' && i < sizeof(desc) - 1) {
            desc[i] = p[i];
            i++;
        }
        desc[i] = '\0';
    }

    if (status == 409) {
        ESP_LOGE(TAG, "HTTP 409 Conflict: another client polls this bot token! "
                      "Only one getUpdates consumer is allowed. (%s)", desc);
    } else if (status == 401) {
        ESP_LOGE(TAG, "HTTP 401: bad bot token. (%s)", desc);
    } else {
        ESP_LOGW(TAG, "HTTP %d via %s: %s", status, s_last_endpoint, desc);
    }
}

static int https_get(const char *path, char *resp, size_t resp_sz, int timeout_ms, int *status)
{
    *status = 0;
    s_last_status = 0;

    tg_tls_t t;
    if (tls_open(&t, timeout_ms) != 0) {
        return -1;
    }

    char req[640];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: " TG_HOST "\r\n"
                      "User-Agent: esp32-zapret/0.1.1\r\n"
                      "Accept: application/json\r\n"
                      "Connection: close\r\n\r\n",
                      path);
    if (rl <= 0 || rl >= (int)sizeof(req)) {
        tls_close(&t);
        return -1;
    }

    size_t off = 0;
    while (off < (size_t)rl) {
        int n = mbedtls_ssl_write(&t.ssl, (const unsigned char *)req + off, (size_t)rl - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        } else {
            ESP_LOGW(TAG, "write failed: -0x%04x", (unsigned)-n);
            tls_close(&t);
            return -1;
        }
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t pos = 0;
    while (pos + 1 < resp_sz) {
        int n = mbedtls_ssl_read(&t.ssl, (unsigned char *)resp + pos, resp_sz - 1 - pos);
        if (n > 0) {
            pos += (size_t)n;
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (esp_timer_get_time() > deadline) {
                break;
            }
            continue;
        }
        break;
    }
    resp[pos] = 0;
    tls_close(&t);

    *status = parse_http_status(resp);
    s_last_status = *status;
    return (int)pos;
}

static size_t utf8_put(char *dst, size_t cap, uint32_t cp)
{
    if (cp < 0x80) {
        if (cap < 1) return 0;
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (cap < 2) return 0;
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        if (cap < 3) return 0;
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        dst[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    if (cap < 4) return 0;
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    dst[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

static void json_parse_text(const char *resp, char *out, size_t out_sz)
{
    out[0] = 0;
    const char *p = strstr(resp, "\"text\":\"");
    if (p == NULL) {
        return;
    }
    p += 8;
    size_t i = 0;

    while (*p && *p != '"' && i + 4 < out_sz) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[i++] = '\n'; p++; break;
            case 't': out[i++] = '\t'; p++; break;
            case 'r': out[i++] = '\r'; p++; break;
            case 'b': out[i++] = '\b'; p++; break;
            case 'f': out[i++] = '\f'; p++; break;
            case '/': out[i++] = '/'; p++; break;
            case '\\': out[i++] = '\\'; p++; break;
            case '"': out[i++] = '"'; p++; break;
            case 'u': {
                int cp = hex4(p + 1);
                if (cp < 0) {
                    p++;
                    break;
                }
                p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    int lo = hex4(p + 2);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                i += utf8_put(out + i, out_sz - i - 1, (uint32_t)cp);
                break;
            }
            default:
                out[i++] = *p++;
                break;
            }
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = 0;
}

int tg_get_updates(tg_update_t *out, int64_t offset, int long_poll_s)
{
    char path[256];
    snprintf(path, sizeof(path),
             "/bot%s/getUpdates?timeout=%d&offset=%lld",
             CFG_TG_TOKEN, long_poll_s, (long long)offset);

    int status = 0;
    int n = https_get(path, s_resp, sizeof(s_resp), long_poll_s * 1000 + 20000, &status);
    if (n <= 0) {
        return TG_RC_TRANSPORT;
    }
    if (status != 200) {
        log_api_error(status);
        return TG_RC_HTTP;
    }

    memset(out, 0, sizeof(*out));

    const char *p = strstr(s_resp, "\"update_id\":");
    if (p == NULL) {
        return TG_RC_NO_UPDATES;
    }
    out->update_id = strtoll(p + 12, NULL, 10);
    out->valid = true;

    p = strstr(s_resp, "\"chat\":");
    if (p != NULL) {
        const char *q = strstr(p, "\"id\":");
        if (q != NULL) {
            out->chat_id = strtoll(q + 5, NULL, 10);
        }
    }

    json_parse_text(s_resp, out->text, sizeof(out->text));

    return TG_RC_UPDATE;
}

static void url_encode(const char *s, char *out, size_t out_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    for (; *s && o + 4 < out_sz; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0f];
        }
    }
    out[o] = 0;
}

int tg_send_message(int64_t chat_id, const char *text)
{
    static char resp[2048];
    char enc[1024];
    char path[1536];
    int status = 0;

    url_encode(text, enc, sizeof(enc));
    snprintf(path, sizeof(path),
             "/bot%s/sendMessage?chat_id=%lld&text=%s",
             CFG_TG_TOKEN, (long long)chat_id, enc);

    int n = https_get(path, resp, sizeof(resp), 20000, &status);
    if (n > 0 && status != 200) {
        ESP_LOGW(TAG, "sendMessage HTTP %d", status);
    }
    return n;
}

const char *tg_last_endpoint(void)
{
    return s_last_endpoint[0] ? s_last_endpoint : "-";
}

int tg_last_http_status(void)
{
    return s_last_status;
}
