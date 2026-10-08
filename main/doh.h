// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

/* Telemetry flags (stats schema 2 `doh` field). */
#define DOH_FLAG_TRIED 0x01u
#define DOH_FLAG_OK    0x02u

/* Resolves host via DNS-over-HTTPS (Cloudflare 1.1.1.1, Google 8.8.8.8).
 * Returns the number of IPv4 addresses in network byte order (0 on failure).
 * Used as a fallback when DNS is poisoned or blocked by the provider. */
int doh_resolve(const char *host, uint32_t *addrs_be, int max_addrs);

/* Usage flags since boot: DOH_FLAG_TRIED | DOH_FLAG_OK. */
uint8_t doh_get_flags(void);
