// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_DESYNC_MODE_OFF = 0,
    ESP_DESYNC_MODE_SPLIT,
    ESP_DESYNC_MODE_DISORDER,
    ESP_DESYNC_MODE_FAKE,
    ESP_DESYNC_MODE_FAKE_SPLIT,
    ESP_DESYNC_MODE_TLSREC,
} esp_desync_mode_t;

#define ESP_DESYNC_FOOL_NONE      0u
#define ESP_DESYNC_FOOL_TTL       (1u << 0)
#define ESP_DESYNC_FOOL_BADSUM    (1u << 1)
#define ESP_DESYNC_FOOL_BADSEQ    (1u << 2)
#define ESP_DESYNC_FOOL_MD5SIG    (1u << 3)
#define ESP_DESYNC_FOOL_DATANOACK (1u << 4)

typedef struct {
    esp_desync_mode_t mode;
    uint32_t fooling;
    uint8_t  fake_ttl;
    int32_t  badseq_offset;
    const char *fake_sni;
    int16_t  split_pos;
    int16_t  split_pos2;
    uint16_t op_delay_ms;
    uint8_t  repeats;
} esp_desync_config_t;

esp_err_t esp_desync_init(const esp_desync_config_t *cfg);
void esp_desync_get_config(esp_desync_config_t *out);
void esp_desync_set_config(const esp_desync_config_t *cfg);

int esp_desync_connect(const char *host, uint16_t port, int timeout_ms);
/* Resolve host to up to max_addrs IPv4 addresses (network byte order). */
int esp_desync_resolve(const char *host, uint32_t *addrs_be, int max_addrs);
/* Connect to a specific IPv4 address; host is used for logging only, TLS
 * hostname verification is configured by the caller. */
int esp_desync_connect_ip(const char *host, uint32_t ip_be, uint16_t port, int timeout_ms);
ssize_t esp_desync_write(int fd, const void *data, size_t len);
ssize_t esp_desync_read(int fd, void *buf, size_t len);
void esp_desync_close(int fd);

const char *esp_desync_mode_name(esp_desync_mode_t mode);
esp_desync_mode_t esp_desync_mode_from_name(const char *name, bool *ok);

#ifdef __cplusplus
}
#endif
