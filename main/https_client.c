// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/net_sockets.h"
#include "esp_desync.h"
#include "net_utils.h"
#include "fw_version.h"
#include "stats.h"
#include "https_client.h"

static const char *TAG = "https";

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    https_conn_t *c = (https_conn_t *)ctx;
    ssize_t n = esp_desync_write(c->fd, buf, len);
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
    https_conn_t *c = (https_conn_t *)ctx;
    ssize_t n = esp_desync_read(c->fd, buf, len);
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

int https_connect_ex(https_conn_t *c, const char *host, const uint32_t *ips_be, int n_ips,
                     uint16_t port, int connect_timeout_ms, int handshake_timeout_ms,
                     bool dns_fallback, int *fail_stage)
{
    int stage = HTTPS_STAGE_TLS;
    if (fail_stage) {
        *fail_stage = HTTPS_STAGE_OK;
    }

    memset(c, 0, sizeof(*c));
    c->fd = -1;
    strlcpy(c->host, host, sizeof(c->host));
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);

    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy, NULL, 0) != 0) {
        goto fail;
    }
    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        goto fail;
    }
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    if (esp_crt_bundle_attach(&c->conf) != ESP_OK) {
        goto fail;
    }
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) {
        goto fail;
    }
    if (mbedtls_ssl_set_hostname(&c->ssl, host) != 0) {
        goto fail;
    }

    for (int i = 0; i < n_ips && c->fd < 0; i++) {
        c->fd = esp_desync_connect_ip(host, ips_be[i], port, connect_timeout_ms);
        if (c->fd >= 0) {
            net_format_ip(ips_be[i], c->endpoint, sizeof(c->endpoint));
        }
    }
    if (c->fd < 0 && dns_fallback) {
        uint32_t resolved[8];
        int rn = esp_desync_resolve(host, resolved, 8);
        for (int i = 0; i < rn && c->fd < 0; i++) {
            c->fd = esp_desync_connect_ip(host, resolved[i], port, connect_timeout_ms);
            if (c->fd >= 0) {
                net_format_ip(resolved[i], c->endpoint, sizeof(c->endpoint));
            }
        }
    }
    if (c->fd < 0) {
        ESP_LOGW(TAG, "tcp connect failed: %s", host);
        stage = HTTPS_STAGE_TCP;
        stats_anon_note_err(0x0001);
        goto fail;
    }

    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);

    int64_t deadline = esp_timer_get_time() + (int64_t)handshake_timeout_ms * 1000;
    while (1) {
        int ret = mbedtls_ssl_handshake(&c->ssl);
        if (ret == 0) {
            break;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (esp_timer_get_time() > deadline) {
                ESP_LOGW(TAG, "handshake timeout via %s", c->endpoint);
                stage = HTTPS_STAGE_TIMEOUT;
                stats_anon_note_err(0x0002);
                goto fail;
            }
            vTaskDelay(1);
            continue;
        }
        ESP_LOGW(TAG, "handshake failed via %s: -0x%04x", c->endpoint, (unsigned)-ret);
        stage = (ret == MBEDTLS_ERR_NET_RECV_FAILED || ret == MBEDTLS_ERR_NET_CONN_RESET)
                    ? HTTPS_STAGE_RST : HTTPS_STAGE_TLS;
        stats_anon_note_err((uint16_t)(-ret));
        goto fail;
    }
    return 0;

fail:
    if (fail_stage) {
        *fail_stage = stage;
    }
    https_close(c);
    return -1;
}

int https_connect(https_conn_t *c, const char *host, const uint32_t *ips_be, int n_ips,
                  uint16_t port, int connect_timeout_ms, int handshake_timeout_ms,
                  bool dns_fallback)
{
    return https_connect_ex(c, host, ips_be, n_ips, port, connect_timeout_ms,
                            handshake_timeout_ms, dns_fallback, NULL);
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

/* Writes the whole buffer, but never spins forever: a peer that keeps the
 * window closed or a stuck TCP connection aborts the request on timeout. */
static int ssl_write_all(mbedtls_ssl_context *ssl, const char *data, size_t len, int timeout_ms)
{
    size_t off = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (off < len) {
        int n = mbedtls_ssl_write(ssl, (const unsigned char *)data + off, len - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (esp_timer_get_time() > deadline) {
                ESP_LOGW(TAG, "write timeout after %d ms", timeout_ms);
                return -1;
            }
            vTaskDelay(1);
            continue;
        } else {
            ESP_LOGW(TAG, "write failed: -0x%04x", (unsigned)-n);
            return -1;
        }
    }
    return 0;
}

static int read_response(https_conn_t *c, char *resp, size_t resp_sz, int timeout_ms,
                         int *http_status)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t pos = 0;
    while (pos + 1 < resp_sz) {
        int n = mbedtls_ssl_read(&c->ssl, (unsigned char *)resp + pos, resp_sz - 1 - pos);
        if (n > 0) {
            pos += (size_t)n;
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (esp_timer_get_time() > deadline) {
                break;
            }
            vTaskDelay(1); /* do not spin the CPU while the peer is idle */
            continue;
        }
        break;
    }
    resp[pos] = 0;
    if (http_status) {
        *http_status = parse_http_status(resp);
    }
    return (int)pos;
}

int https_request(https_conn_t *c, const char *path, const char *accept,
                  char *resp, size_t resp_sz, int timeout_ms, int *http_status)
{
    if (http_status) {
        *http_status = 0;
    }
    if (resp == NULL || resp_sz == 0) {
        return -1;
    }

    char req[640];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: " FW_USER_AGENT "\r\n"
                      "Accept: %s\r\n"
                      "Connection: close\r\n\r\n",
                      path, c->host, accept ? accept : "application/json");
    if (rl <= 0 || rl >= (int)sizeof(req)) {
        return -1;
    }
    if (ssl_write_all(&c->ssl, req, (size_t)rl, timeout_ms) != 0) {
        return -1;
    }
    return read_response(c, resp, resp_sz, timeout_ms, http_status);
}

int https_post_json(https_conn_t *c, const char *path, const char *json,
                    int timeout_ms, int *http_status)
{
    if (http_status) {
        *http_status = 0;
    }
    if (json == NULL) {
        return -1;
    }

    size_t body_len = strlen(json);
    char hdr[320];
    int hl = snprintf(hdr, sizeof(hdr),
                      "POST %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: " FW_USER_AGENT "\r\n"
                      "Accept: application/json\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %u\r\n"
                      "Connection: close\r\n\r\n",
                      path, c->host, (unsigned)body_len);
    if (hl <= 0 || hl >= (int)sizeof(hdr)) {
        return -1;
    }
    if (ssl_write_all(&c->ssl, hdr, (size_t)hl, timeout_ms) != 0 ||
        ssl_write_all(&c->ssl, json, body_len, timeout_ms) != 0) {
        return -1;
    }

    char discard[64];
    return read_response(c, discard, sizeof(discard), timeout_ms, http_status);
}

void https_close(https_conn_t *c)
{
    if (c->fd >= 0) {
        mbedtls_ssl_close_notify(&c->ssl);
        esp_desync_close(c->fd);
        c->fd = -1;
    }
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
}

/* --- Keep-alive session reader (Telegram) --- */

typedef struct {
    https_conn_t *c;
    unsigned char carry[512];
    size_t carry_len;
    size_t carry_pos;
    int64_t deadline;
    bool peer_closed;
} ka_reader_t;

/* Case-insensitive substring search (newlib may lack strcasestr). */
static const char *ci_find(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] != 0 &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) {
            return p;
        }
    }
    return NULL;
}

/* Returns buffered bytes; 0 = nothing yet (already delayed), -1 = error. */
static int ka_read(ka_reader_t *r, char *dst, size_t len)
{
    if (r->carry_pos < r->carry_len) {
        size_t c = r->carry_len - r->carry_pos;
        if (c > len) {
            c = len;
        }
        memcpy(dst, (char *)r->carry + r->carry_pos, c);
        r->carry_pos += c;
        return (int)c;
    }
    int n = mbedtls_ssl_read(&r->c->ssl, r->carry, sizeof(r->carry));
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
        if (esp_timer_get_time() > r->deadline) {
            return -1; /* timeout: caller drops the session */
        }
        vTaskDelay(1); /* do not spin the CPU while the peer is idle */
        return 0;
    }
    if (n <= 0) {
        r->peer_closed = true;
        return -1;
    }
    r->carry_len = (size_t)n;
    r->carry_pos = 0;
    size_t c = (size_t)n > len ? len : (size_t)n;
    memcpy(dst, r->carry, c);
    r->carry_pos = c;
    return (int)c;
}

static int ka_read_exact(ka_reader_t *r, char *dst, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = ka_read(r, dst + got, len - got);
        if (n > 0) {
            got += (size_t)n;
        } else if (n < 0) {
            return -1;
        }
    }
    return 0;
}

static int ka_read_byte(ka_reader_t *r)
{
    char ch;
    while (1) {
        int n = ka_read(r, &ch, 1);
        if (n == 1) {
            return (unsigned char)ch;
        }
        if (n < 0) {
            return -1;
        }
    }
}

static long ka_header_long(const char *hdr, const char *name)
{
    const char *p = ci_find(hdr, name);
    if (p == NULL) {
        return -1;
    }
    p += strlen(name);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return strtol(p, NULL, 10);
}

static int ka_read_response(https_conn_t *c, char *resp, size_t resp_sz, int timeout_ms,
                            int *http_status, bool *conn_alive);

int https_request_ka(https_conn_t *c, const char *path, const char *accept,
                     char *resp, size_t resp_sz, int timeout_ms,
                     int *http_status, bool *conn_alive)
{
    if (http_status) {
        *http_status = 0;
    }
    if (conn_alive) {
        *conn_alive = false;
    }
    if (resp == NULL || resp_sz == 0) {
        return -1;
    }

    char req[1600];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: " FW_USER_AGENT "\r\n"
                      "Accept: %s\r\n"
                      "Connection: keep-alive\r\n\r\n",
                      path, c->host, accept ? accept : "application/json");
    if (rl <= 0 || rl >= (int)sizeof(req)) {
        return -1;
    }
    if (ssl_write_all(&c->ssl, req, (size_t)rl, timeout_ms) != 0) {
        return -1;
    }
    return ka_read_response(c, resp, resp_sz, timeout_ms, http_status, conn_alive);
}

int https_post_json_ka(https_conn_t *c, const char *path, const char *json,
                       int timeout_ms, int *http_status, bool *conn_alive)
{
    if (http_status) {
        *http_status = 0;
    }
    if (conn_alive) {
        *conn_alive = false;
    }
    if (json == NULL) {
        return -1;
    }

    size_t body_len = strlen(json);
    char hdr[320];
    int hl = snprintf(hdr, sizeof(hdr),
                      "POST %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: " FW_USER_AGENT "\r\n"
                      "Accept: application/json\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %u\r\n"
                      "Connection: keep-alive\r\n\r\n",
                      path, c->host, (unsigned)body_len);
    if (hl <= 0 || hl >= (int)sizeof(hdr)) {
        return -1;
    }
    if (ssl_write_all(&c->ssl, hdr, (size_t)hl, timeout_ms) != 0 ||
        ssl_write_all(&c->ssl, json, body_len, timeout_ms) != 0) {
        return -1;
    }

    char discard[64];
    return ka_read_response(c, discard, sizeof(discard), timeout_ms, http_status, conn_alive);
}

static int ka_read_response(https_conn_t *c, char *resp, size_t resp_sz, int timeout_ms,
                            int *http_status, bool *conn_alive)
{
    ka_reader_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.c = c;
    rd.deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    /* Response head. */
    char hdr[640];
    size_t hn = 0;
    while (hn < sizeof(hdr) - 1) {
        int ch = ka_read_byte(&rd);
        if (ch < 0) {
            return -1;
        }
        hdr[hn++] = (char)ch;
        if (hn >= 4 && hdr[hn - 4] == '\r' && hdr[hn - 3] == '\n' &&
            hdr[hn - 2] == '\r' && hdr[hn - 1] == '\n') {
            break;
        }
    }
    hdr[hn] = 0;

    int status = parse_http_status(hdr);
    if (http_status) {
        *http_status = status;
    }

    bool close_hdr = ci_find(hdr, "\r\nconnection: close") != NULL;
    bool chunked = ci_find(hdr, "\r\ntransfer-encoding: chunked") != NULL;
    long clen = ka_header_long(hdr, "\r\ncontent-length:");

    size_t pos = 0;
    char sink[64];

    if (chunked) {
        while (1) {
            char line[24];
            size_t ln = 0;
            int ch;
            while ((ch = ka_read_byte(&rd)) >= 0 && ch != '\n') {
                if (ch != '\r' && ln < sizeof(line) - 1) {
                    line[ln++] = (char)ch;
                }
            }
            if (ch < 0) {
                return -1;
            }
            line[ln] = 0;
            unsigned long sz = strtoul(line, NULL, 16);
            if (sz == 0) {
                /* Trailer section: read lines until an empty one. */
                while (1) {
                    char tline[4];
                    size_t tl = 0;
                    int tc;
                    while ((tc = ka_read_byte(&rd)) >= 0 && tc != '\n') {
                        if (tc != '\r' && tl < sizeof(tline) - 1) {
                            tline[tl++] = (char)tc;
                        }
                    }
                    if (tc < 0) {
                        return -1;
                    }
                    if (tl == 0) {
                        break;
                    }
                }
                break;
            }
            size_t take = sz;
            if (take > resp_sz - 1 - pos) {
                take = resp_sz - 1 - pos;
            }
            if (take && ka_read_exact(&rd, resp + pos, take) != 0) {
                return -1;
            }
            pos += take;
            size_t rest = sz - take;
            while (rest) {
                size_t cc = rest > sizeof(sink) ? sizeof(sink) : rest;
                if (ka_read_exact(&rd, sink, cc) != 0) {
                    return -1;
                }
                rest -= cc;
            }
            char crlf[2];
            if (ka_read_exact(&rd, crlf, 2) != 0) {
                return -1;
            }
        }
    } else if (clen >= 0) {
        size_t total = (size_t)clen;
        size_t take = total > resp_sz - 1 ? resp_sz - 1 : total;
        if (take && ka_read_exact(&rd, resp, take) != 0) {
            return -1;
        }
        pos = take;
        size_t rest = total - take;
        while (rest) {
            size_t cc = rest > sizeof(sink) ? sizeof(sink) : rest;
            if (ka_read_exact(&rd, sink, cc) != 0) {
                return -1;
            }
            rest -= cc;
        }
    } else {
        /* No framing headers: read until the peer closes (drop the session). */
        close_hdr = true;
        while (pos + 1 < resp_sz) {
            int n = ka_read(&rd, resp + pos, resp_sz - 1 - pos);
            if (n > 0) {
                pos += (size_t)n;
                continue;
            }
            if (n == 0) {
                continue; /* waiting for data */
            }
            break;
        }
    }

    resp[pos] = 0;
    if (conn_alive) {
        *conn_alive = !close_hdr && !rd.peer_closed;
    }
    return (int)pos;
}

const char *https_endpoint(const https_conn_t *c)
{
    return c->endpoint[0] ? c->endpoint : "-";
}
