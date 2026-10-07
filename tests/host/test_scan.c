// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "scan_candidates.h"

static int g_fail;

#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail++; \
        } \
    } while (0)

int main(void)
{
    scan_candidate_t c[SCAN_MAX_CANDIDATES];
    int n = scan_build_candidates(c, SCAN_MAX_CANDIDATES);

    CHECK(n > 8);
    CHECK(n <= SCAN_MAX_CANDIDATES);

    /* The first candidate must be plain (no desync): optimal when unfiltered. */
    CHECK(c[0].mode == ESP_DESYNC_MODE_OFF);

    /* Ascending TTL fake_split entries follow (lowest working TTL wins). */
    CHECK(c[1].mode == ESP_DESYNC_MODE_FAKE_SPLIT);
    CHECK(c[1].fooling == ESP_DESYNC_FOOL_TTL);
    CHECK(c[1].ttl == 3);
    CHECK(strcmp(c[1].sni, "www.iana.org") == 0);
    CHECK(c[2].ttl == 5);
    CHECK(c[3].ttl == 8);
    CHECK(c[4].ttl == 12);

    /* Candidate sanity. */
    for (int i = 0; i < n; i++) {
        CHECK(c[i].mode <= ESP_DESYNC_MODE_SEQOVL);
        if (c[i].mode != ESP_DESYNC_MODE_OFF) {
            CHECK(c[i].sni[0] != 0);
            CHECK(strlen(c[i].sni) < SCAN_SNI_MAX);
        }
    }

    /* Must cover alternative decoys and no-fake methods. */
    bool yandex = false;
    bool split = false;
    bool tlsrec = false;
    bool rndsni = false;
    bool seqovl = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(c[i].sni, "www.yandex.ru") == 0) {
            yandex = true;
        }
        if (c[i].mode == ESP_DESYNC_MODE_SPLIT) {
            split = true;
        }
        if (c[i].mode == ESP_DESYNC_MODE_TLSREC) {
            tlsrec = true;
        }
        if (c[i].mode == ESP_DESYNC_MODE_SEQOVL) {
            seqovl = true;
        }
        if (c[i].rndsni) {
            rndsni = true;
            char rbuf[96];
            scan_format(&c[i], rbuf, sizeof(rbuf));
            CHECK(strstr(rbuf, "rndsni") != NULL);
        }
    }
    CHECK(yandex);
    CHECK(split);
    CHECK(tlsrec);
    CHECK(rndsni);
    CHECK(seqovl);

    /* Formatting. */
    char buf[96];
    scan_format(&c[0], buf, sizeof(buf));
    CHECK(strcmp(buf, "off") == 0);
    scan_format(&c[1], buf, sizeof(buf));
    CHECK(strstr(buf, "fake_split") != NULL);
    CHECK(strstr(buf, "ttl=3") != NULL);
    CHECK(strstr(buf, "www.iana.org") != NULL);

    /* Overflow safety. */
    scan_candidate_t small[2];
    int m = scan_build_candidates(small, 2);
    CHECK(m == 2);

    if (g_fail) {
        printf("SCAN TESTS FAILED: %d\n", g_fail);
        return 1;
    }
    printf("SCAN tests: OK\n");
    return 0;
}
