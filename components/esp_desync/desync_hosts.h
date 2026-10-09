// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>

/* Host allowlist for the desync engine: desync is applied only to matching
 * destinations, everything else connects as plain TLS (statistics, DoH and
 * other services keep working even when the bypass strategy would interfere
 * with them on the local network).
 *
 * `patterns` is a comma-separated list; whitespace around entries is ignored;
 * "*" matches any host; an entry matches the host exactly or as a domain
 * suffix ("telegram.org" matches "api.telegram.org"). Matching is
 * case-insensitive.
 *
 * Pure module: no ESP-IDF dependencies, unit-tested on the host. */
bool desync_host_allowed(const char *host, const char *patterns);
