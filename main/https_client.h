// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

/* Minimal HTTPS client over esp_desync: certificate verification is mandatory
 * and the TLS hostname is always the logical host, even when connecting to a
 * pinned IP address. Reused by the Telegram client and the DoH resolver. */

typedef struct {
    int fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    char host[64];
    char endpoint[16];
} https_conn_t;

/* Failure stages reported by https_connect_ex (used by the scanner `stage`
 * telemetry field): 1 = no TCP, 2 = TLS handshake, 3 = connection reset,
 * 4 = timeout. */
#define HTTPS_STAGE_OK      0
#define HTTPS_STAGE_TCP     1
#define HTTPS_STAGE_TLS     2
#define HTTPS_STAGE_RST     3
#define HTTPS_STAGE_TIMEOUT 4

/* Connects to host:port. Tries the given IPv4 candidates first (network byte
 * order), then DNS when dns_fallback is true. Returns 0 on success (TCP + TLS
 * handshake). When fail_stage is non-NULL it receives one of HTTPS_STAGE_*. */
int https_connect_ex(https_conn_t *c, const char *host, const uint32_t *ips_be, int n_ips,
                     uint16_t port, int connect_timeout_ms, int handshake_timeout_ms,
                     bool dns_fallback, int *fail_stage);

int https_connect(https_conn_t *c, const char *host, const uint32_t *ips_be, int n_ips,
                  uint16_t port, int connect_timeout_ms, int handshake_timeout_ms,
                  bool dns_fallback);

/* HTTP/1.1 GET (Connection: close). resp is NUL-terminated on success.
 * Returns the response length (>0) or -1; fills http_status when non-NULL. */
int https_request(https_conn_t *c, const char *path, const char *accept,
                  char *resp, size_t resp_sz, int timeout_ms, int *http_status);

/* HTTP/1.1 POST with an application/json body (Connection: close). Returns the
 * response length (>0) or -1; fills http_status when non-NULL. The response
 * body is only used to parse the status line, so a small buffer is enough. */
int https_post_json(https_conn_t *c, const char *path, const char *json,
                    int timeout_ms, int *http_status);

void https_close(https_conn_t *c);

/* IPv4 address (dotted quad) the last connection actually used, "-" if none. */
const char *https_endpoint(const https_conn_t *c);
