// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

/* Extracts A-record IPv4 addresses from a DoH JSON response
 * (Cloudflare / Google "application/dns-json" format).
 * Addresses are written in network byte order; duplicates are skipped.
 * Returns the number of parsed addresses (never more than max_addrs). */
int doh_parse_a_records(const char *json, uint32_t *out_be, int max_addrs);
