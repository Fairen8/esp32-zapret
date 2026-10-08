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
    p.fw = "1.2.0";
    p.target = "esp32";
    p.uptime_s = 42;
    p.free_heap = 123456;
    p.min_heap = 98765;
    p.max_block = 65536;
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
    p.tried = "4:1,1:0";
    p.tts_s = 17;
    p.strategy_changed = 2;
    p.wifi_disc = 1;
    p.wifi_reason = 200;
    p.boot_storm = false;
    p.temp_c = 47;
    p.errs = "0x6a80:3,0x0001:1";
    p.mtu = 1500;
    p.sntp_s = 3;
    p.sntp_ok = true;
    p.doh = 0;
    p.channel = 6;
    p.stage = 0;
    p.cfg = 0x0f;
    p.cpu_mhz = 160;
    p.flash_free = 630784;
    p.psram = false;
    p.chip_rev = 301;
    p.sni_cat = 1;
    p.rid = "0123456789abcdef";

    char buf[1024];
    int n = stats_build_payload(&p, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK((size_t)n == strlen(buf));

    const char *expect =
        "{\"schema\":2,\"event\":\"boot\",\"fw\":\"1.2.0\",\"target\":\"esp32\","
        "\"uptime_s\":42,\"heap\":123456,\"min_heap\":98765,\"max_block\":65536,"
        "\"rssi\":-55,\"reset\":3,"
        "\"mode\":5,\"fool\":1,\"ttl\":5,\"split1\":-1,\"split2\":-1,"
        "\"have\":true,\"strategy\":\"fake_split fool=ttl ttl=5 sni=www.iana.org\","
        "\"scans\":2,\"probes\":14,\"fails\":3,"
        "\"tried\":\"4:1,1:0\",\"tts_s\":17,\"strategy_changed\":2,"
        "\"wifi_disc\":1,\"wifi_reason\":200,\"boot_storm\":false,"
        "\"temp_c\":47,\"errs\":\"0x6a80:3,0x0001:1\","
        "\"mtu\":1500,\"sntp_s\":3,\"sntp_ok\":true,"
        "\"doh\":0,\"ch\":6,\"stage\":0,\"cfg\":15,"
        "\"cpu_mhz\":160,\"flash_free\":630784,\"psram\":false,"
        "\"chip_rev\":301,\"sni_cat\":1,\"rid\":\"0123456789abcdef\"}";
    CHECK(strcmp(buf, expect) == 0);
    if (strcmp(buf, expect) != 0) {
        printf("got: %s\n", buf);
    }

    /* No strategy / failure diagnostics. */
    p.have_strategy = false;
    p.strategy = "";
    p.tried = "";
    p.errs = "";
    p.rid = "";
    p.temp_c = -128;
    p.stage = 4;
    p.boot_storm = true;
    n = stats_build_payload(&p, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "\"have\":false") != NULL);
    CHECK(strstr(buf, "\"strategy\":\"\"") != NULL);
    CHECK(strstr(buf, "\"tried\":\"\"") != NULL);
    CHECK(strstr(buf, "\"temp_c\":-128") != NULL);
    CHECK(strstr(buf, "\"stage\":4") != NULL);
    CHECK(strstr(buf, "\"boot_storm\":true") != NULL);

    /* NULL safety and truncation. */
    CHECK(stats_build_payload(NULL, buf, sizeof(buf)) == -1);
    CHECK(stats_build_payload(&p, NULL, 0) == -1);
    CHECK(stats_build_payload(&p, buf, 16) == -1);

    char full_buf[1024];
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
