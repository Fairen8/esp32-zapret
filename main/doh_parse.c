// SPDX-License-Identifier: MIT
#include "doh_parse.h"
#include <string.h>
#include <stdbool.h>

static bool parse_ipv4(const char *s, size_t len, uint32_t *out_be)
{
    uint8_t octets[4];
    int n = 0;
    size_t i = 0;

    while (i < len && n < 4) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        unsigned v = 0;
        size_t digits = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            v = v * 10u + (unsigned)(s[i] - '0');
            if (v > 255) {
                return false;
            }
            i++;
            digits++;
        }
        if (digits == 0 || digits > 3) {
            return false;
        }
        octets[n++] = (uint8_t)v;
        if (n < 4) {
            if (i >= len || s[i] != '.') {
                return false;
            }
            i++;
        }
    }

    if (n != 4 || i != len) {
        return false;
    }

    uint8_t *b = (uint8_t *)out_be;
    b[0] = octets[0];
    b[1] = octets[1];
    b[2] = octets[2];
    b[3] = octets[3];
    return true;
}

int doh_parse_a_records(const char *json, uint32_t *out_be, int max_addrs)
{
    if (json == NULL || out_be == NULL || max_addrs <= 0) {
        return 0;
    }

    int n = 0;
    const char *p = json;

    while (n < max_addrs) {
        p = strstr(p, "\"data\":\"");
        if (p == NULL) {
            break;
        }
        p += 8;
        const char *end = strchr(p, '"');
        if (end == NULL) {
            break;
        }
        uint32_t ip;
        if (parse_ipv4(p, (size_t)(end - p), &ip)) {
            bool dup = false;
            for (int i = 0; i < n; i++) {
                if (out_be[i] == ip) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                out_be[n++] = ip;
            }
        }
        p = end + 1;
    }

    return n;
}
