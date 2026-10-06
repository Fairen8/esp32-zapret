// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

/* Resolves host via DNS-over-HTTPS (Cloudflare 1.1.1.1, Google 8.8.8.8).
 * Returns the number of IPv4 addresses in network byte order (0 on failure).
 * Used as a fallback when DNS is poisoned or blocked by the provider. */
int doh_resolve(const char *host, uint32_t *addrs_be, int max_addrs);
