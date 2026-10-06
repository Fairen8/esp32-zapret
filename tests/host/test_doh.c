// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "doh_parse.h"

static int g_fail;

#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail++; \
        } \
    } while (0)

static uint32_t ip4(unsigned a, unsigned b, unsigned c, unsigned d)
{
    uint32_t v;
    uint8_t *p = (uint8_t *)&v;
    p[0] = (uint8_t)a;
    p[1] = (uint8_t)b;
    p[2] = (uint8_t)c;
    p[3] = (uint8_t)d;
    return v;
}

int main(void)
{
    const char *cf =
        "{"
        "\"Status\":0,\"Answer\":["
        "{\"name\":\"api.telegram.org\",\"type\":5,\"data\":\"cdn.example.net\"},"
        "{\"name\":\"api.telegram.org\",\"type\":1,\"data\":\"149.154.167.220\"},"
        "{\"name\":\"api.telegram.org\",\"type\":1,\"data\":\"149.154.167.191\"},"
        "{\"name\":\"api.telegram.org\",\"type\":1,\"data\":\"149.154.167.220\"}"
        "]}";

    uint32_t a[8];

    int n = doh_parse_a_records(cf, a, 8);
    CHECK(n == 2);
    CHECK(a[0] == ip4(149, 154, 167, 220));
    CHECK(a[1] == ip4(149, 154, 167, 191));

    /* Respects the output cap. */
    n = doh_parse_a_records(cf, a, 1);
    CHECK(n == 1);

    /* Invalid data. */
    n = doh_parse_a_records("{\"Answer\":[{\"type\":1,\"data\":\"not-an-ip\"}]}", a, 8);
    CHECK(n == 0);
    n = doh_parse_a_records("", a, 8);
    CHECK(n == 0);
    n = doh_parse_a_records(NULL, a, 8);
    CHECK(n == 0);
    n = doh_parse_a_records(cf, a, 0);
    CHECK(n == 0);

    /* CNAME-only answer. */
    n = doh_parse_a_records("{\"Answer\":[{\"type\":5,\"data\":\"x.example.org\"}]}", a, 8);
    CHECK(n == 0);

    /* Out-of-range octets are rejected. */
    n = doh_parse_a_records("{\"data\":\"300.1.1.1\"}", a, 8);
    CHECK(n == 0);

    if (g_fail) {
        printf("DOH TESTS FAILED: %d\n", g_fail);
        return 1;
    }
    printf("DOH tests: OK\n");
    return 0;
}
