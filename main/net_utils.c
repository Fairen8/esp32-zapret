// SPDX-License-Identifier: MIT
#include "net_utils.h"
#include <string.h>
#include <stdio.h>

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

int net_parse_ip_list(const char *list, uint32_t *out_be, int max_addrs)
{
    if (list == NULL || out_be == NULL || max_addrs <= 0) {
        return 0;
    }

    int n = 0;
    const char *p = list;

    while (*p != '\0' && n < max_addrs) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != ',' && *p != ';') {
            p++;
        }
        if (parse_ipv4(start, (size_t)(p - start), &out_be[n])) {
            n++;
        }
    }

    return n;
}

void net_format_ip(uint32_t ip_be, char *buf, size_t buf_sz)
{
    if (buf == NULL || buf_sz == 0) {
        return;
    }
    const uint8_t *b = (const uint8_t *)&ip_be;
    snprintf(buf, buf_sz, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool net_parse_mac(const char *s, uint8_t out[6])
{
    if (s == NULL || out == NULL) {
        return false;
    }

    uint8_t bytes[6];
    int n = 0;
    int nibbles = 0;
    uint8_t cur = 0;
    bool seen_any = false;

    for (const char *p = s; *p != '\0'; p++) {
        int h = hexval(*p);
        if (h >= 0) {
            cur = (uint8_t)((cur << 4) | (unsigned)h);
            nibbles++;
            seen_any = true;
            if (nibbles == 2) {
                if (n >= 6) {
                    return false;
                }
                bytes[n++] = cur;
                cur = 0;
                nibbles = 0;
            }
        } else if (*p == ':' || *p == '-' || *p == '.') {
            if (nibbles != 0 || !seen_any) {
                return false;
            }
        } else {
            return false;
        }
    }

    if (n != 6 || nibbles != 0) {
        return false;
    }

    memcpy(out, bytes, 6);
    return true;
}

void net_url_encode(const char *s, char *out, size_t out_sz)
{
    static const char hex[] = "0123456789ABCDEF";

    if (out == NULL || out_sz == 0) {
        return;
    }

    size_t o = 0;
    for (; *s != '\0' && o + 4 < out_sz; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0f];
        }
    }
    out[o] = '\0';
}
