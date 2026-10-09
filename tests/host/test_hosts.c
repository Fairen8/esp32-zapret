// SPDX-License-Identifier: MIT
#include <stdio.h>
#include "desync_hosts.h"

static int g_fail;

#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail++; \
        } \
    } while (0)

int main(void)
{
    /* Exact and case-insensitive. */
    CHECK(desync_host_allowed("api.telegram.org", "api.telegram.org"));
    CHECK(desync_host_allowed("API.Telegram.ORG", "api.telegram.org"));

    /* Domain suffix and list whitespace. */
    CHECK(desync_host_allowed("api.telegram.org", " telegram.org "));
    CHECK(desync_host_allowed("api.telegram.org", "example.com, telegram.org , foo.bar"));

    /* No false positives. */
    CHECK(!desync_host_allowed("api.telegram.org", "telegram.com"));
    CHECK(!desync_host_allowed("evil-telegram.org", "telegram.org"));
    CHECK(!desync_host_allowed("notapi.telegram.org", "api.telegram.org"));

    /* Wildcard and empty lists. */
    CHECK(desync_host_allowed("anything.example", "*"));
    CHECK(desync_host_allowed("anything.example", "x.com,*"));
    CHECK(!desync_host_allowed("anything.example", ""));
    CHECK(!desync_host_allowed("anything.example", " , , "));

    /* NULL safety. */
    CHECK(!desync_host_allowed(NULL, "*"));
    CHECK(!desync_host_allowed("api.telegram.org", NULL));

    if (g_fail == 0) {
        printf("HOSTS tests: OK\n");
        return 0;
    }
    printf("HOSTS tests: %d failure(s)\n", g_fail);
    return 1;
}
