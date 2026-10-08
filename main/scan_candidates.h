// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_desync.h"

#define SCAN_SNI_MAX 40
#define SCAN_MAX_CANDIDATES 24

/* One candidate desync strategy for the boot-time auto-scan. Pure module:
 * no ESP-IDF dependencies besides the public esp_desync types, so it is
 * unit-tested on the host. */
typedef struct {
    esp_desync_mode_t mode;
    uint32_t fooling;
    uint8_t ttl;
    bool rndsni;
    char sni[SCAN_SNI_MAX];
} scan_candidate_t;

/* Builds the ordered candidate list (first entry: no desync). */
int scan_build_candidates(scan_candidate_t *out, int max);

/* Human-readable description, e.g. "fake_split fool=ttl ttl=5 sni=www.iana.org". */
void scan_format(const scan_candidate_t *c, char *buf, size_t buf_sz);
