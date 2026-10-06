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

/* Connects to host:port. Tries the given IPv4 candidates first (network byte
 * order), then DNS when dns_fallback is true. Returns 0 on success (TCP + TLS
 * handshake). */
int https_connect(https_conn_t *c, const char *host, const uint32_t *ips_be, int n_ips,
                  uint16_t port, int connect_timeout_ms, int handshake_timeout_ms,
                  bool dns_fallback);

/* HTTP/1.1 GET (Connection: close). resp is NUL-terminated on success.
 * Returns the response length (>0) or -1; fills http_status when non-NULL. */
int https_request(https_conn_t *c, const char *path, const char *accept,
                  char *resp, size_t resp_sz, int timeout_ms, int *http_status);

void https_close(https_conn_t *c);

/* IPv4 address (dotted quad) the last connection actually used, "-" if none. */
const char *https_endpoint(const https_conn_t *c);
