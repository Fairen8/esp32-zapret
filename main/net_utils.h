// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Parse a list of IPv4 addresses separated by ',', ';', spaces or tabs.
 * Invalid entries are skipped. Addresses are stored in network byte order.
 * Returns the number of parsed addresses (never more than max_addrs). */
int net_parse_ip_list(const char *list, uint32_t *out_be, int max_addrs);

/* Format an IPv4 address (network byte order) as dotted quad. */
void net_format_ip(uint32_t ip_be, char *buf, size_t buf_sz);

/* Parse a MAC address: "AA:BB:CC:DD:EE:FF", "aa-bb-cc-dd-ee-ff" or
 * "aabbccddeeff". Returns false on any syntax error. */
bool net_parse_mac(const char *s, uint8_t out[6]);

/* Percent-encode a string for use in a URL query parameter. */
void net_url_encode(const char *s, char *out, size_t out_sz);
