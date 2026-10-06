// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include "stats_payload.h"

static int g_fail;

#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail++; \
        } \
    } while (0)

int main(void)
{
    stats_payload_t p = {0};
    p.event = "boot";
    p.fw = "1.0.1";
    p.target = "esp32";
    p.uptime_s = 42;
    p.free_heap = 123456;
    p.rssi = -55;
    p.reset_reason = 3;
    p.mode = 5;
    p.fooling = 1;
    p.ttl = 5;
    p.split1 = -1;
    p.split2 = -1;
    p.have_strategy = true;
    p.strategy = "fake_split fool=ttl ttl=5 sni=www.iana.org";
    p.scans = 2;
    p.probes = 14;
    p.probe_fails = 3;

    char buf[512];
    int n = stats_build_payload(&p, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK((size_t)n == strlen(buf));

    const char *expect =
        "{\"schema\":1,\"event\":\"boot\",\"fw\":\"1.0.1\",\"target\":\"esp32\","
        "\"uptime_s\":42,\"heap\":123456,\"rssi\":-55,\"reset\":3,"
        "\"mode\":5,\"fool\":1,\"ttl\":5,\"split1\":-1,\"split2\":-1,"
        "\"have\":true,\"strategy\":\"fake_split fool=ttl ttl=5 sni=www.iana.org\","
        "\"scans\":2,\"probes\":14,\"fails\":3}";
    CHECK(strcmp(buf, expect) == 0);
    if (strcmp(buf, expect) != 0) {
        printf("got: %s\n", buf);
    }

    p.have_strategy = false;
    p.strategy = "";
    n = stats_build_payload(&p, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\"have\":false") != NULL);
    CHECK(strstr(buf, "\"strategy\":\"\"") != NULL);

    CHECK(stats_build_payload(NULL, buf, sizeof(buf)) == -1);
    CHECK(stats_build_payload(&p, NULL, 0) == -1);
    CHECK(stats_build_payload(&p, buf, 16) == -1);

    char full_buf[512];
    int full = stats_build_payload(&p, full_buf, sizeof(full_buf));
    CHECK(full > 0);
    CHECK(stats_build_payload(&p, buf, (size_t)full + 1) == full);
    CHECK(stats_build_payload(&p, buf, (size_t)full) == -1);

    if (g_fail == 0) {
        printf("STATS tests: OK\n");
        return 0;
    }
    printf("STATS tests: %d failure(s)\n", g_fail);
    return 1;
}
