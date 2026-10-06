// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "mbedtls/net_sockets.h"
#include "esp_desync.h"
#include "net_utils.h"
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

int https_connect(https_conn_t *c, const char *host, const uint32_t *ips_be, int n_ips,
                  uint16_t port, int connect_timeout_ms, int handshake_timeout_ms,
                  bool dns_fallback)
{
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
                goto fail;
            }
            continue;
        }
        ESP_LOGW(TAG, "handshake failed via %s: -0x%04x", c->endpoint, (unsigned)-ret);
        goto fail;
    }
    return 0;

fail:
    https_close(c);
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
                      "User-Agent: esp32-zapret/1.0.0\r\n"
                      "Accept: %s\r\n"
                      "Connection: close\r\n\r\n",
                      path, c->host, accept ? accept : "application/json");
    if (rl <= 0 || rl >= (int)sizeof(req)) {
        return -1;
    }

    size_t off = 0;
    while (off < (size_t)rl) {
        int n = mbedtls_ssl_write(&c->ssl, (const unsigned char *)req + off, (size_t)rl - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        } else {
            ESP_LOGW(TAG, "write failed: -0x%04x", (unsigned)-n);
            return -1;
        }
    }

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

const char *https_endpoint(const https_conn_t *c)
{
    return c->endpoint[0] ? c->endpoint : "-";
}
