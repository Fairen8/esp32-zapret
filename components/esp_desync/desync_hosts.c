// SPDX-License-Identifier: MIT
#include <string.h>
#include <ctype.h>
#include "desync_hosts.h"

static bool chars_equal(char a, char b)
{
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static bool token_match(const char *host, const char *pat, size_t plen)
{
    if (plen == 1 && pat[0] == '*') {
        return true;
    }
    size_t hlen = strlen(host);
    if (hlen == plen) {
        for (size_t i = 0; i < plen; i++) {
            if (!chars_equal(host[i], pat[i])) {
                return false;
            }
        }
        return true;
    }
    /* Domain suffix: "telegram.org" matches "api.telegram.org" but not
     * "evil-telegram.org". */
    if (hlen > plen && host[hlen - plen - 1] == '.') {
        for (size_t i = 0; i < plen; i++) {
            if (!chars_equal(host[hlen - plen + i], pat[i])) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool desync_host_allowed(const char *host, const char *patterns)
{
    if (host == NULL || patterns == NULL) {
        return false;
    }
    const char *p = patterns;
    while (*p != 0) {
        while (*p == ',' || *p == ' ' || *p == '\t') {
            p++;
        }
        const char *start = p;
        while (*p != 0 && *p != ',') {
            p++;
        }
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        if (end > start && token_match(host, start, (size_t)(end - start))) {
            return true;
        }
    }
    return false;
}
