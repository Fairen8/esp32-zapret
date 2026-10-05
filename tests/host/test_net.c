// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "net_utils.h"

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

static void test_ip_list(void)
{
    uint32_t a[8];

    int n = net_parse_ip_list("149.154.167.220, 149.154.167.191;91.108.56.1", a, 8);
    CHECK(n == 3);
    CHECK(a[0] == ip4(149, 154, 167, 220));
    CHECK(a[1] == ip4(149, 154, 167, 191));
    CHECK(a[2] == ip4(91, 108, 56, 1));

    n = net_parse_ip_list("300.1.1.1, 1.2.3, 1.2.3.4.5, ok, 10.0.0.1", a, 8);
    CHECK(n == 1);
    CHECK(a[0] == ip4(10, 0, 0, 1));

    n = net_parse_ip_list("", a, 8);
    CHECK(n == 0);
    n = net_parse_ip_list(NULL, a, 8);
    CHECK(n == 0);

    n = net_parse_ip_list("1.1.1.1,2.2.2.2,3.3.3.3,4.4.4.4", a, 2);
    CHECK(n == 2);
    CHECK(a[1] == ip4(2, 2, 2, 2));
}

static void test_format_ip(void)
{
    char buf[16];
    net_format_ip(ip4(149, 154, 167, 220), buf, sizeof(buf));
    CHECK(strcmp(buf, "149.154.167.220") == 0);
}

static void test_mac(void)
{
    uint8_t m[6];

    CHECK(net_parse_mac("AA:BB:CC:DD:EE:FF", m) && m[0] == 0xaa && m[5] == 0xff);
    CHECK(net_parse_mac("aabbccddeeff", m) && m[0] == 0xaa && m[2] == 0xcc);
    CHECK(net_parse_mac("aa-bb-cc-dd-ee-ff", m) && m[5] == 0xff);
    CHECK(!net_parse_mac("AA:BB:CC:DD:EE", m));
    CHECK(!net_parse_mac("AA:BB:CC:DD:EE:FF:00", m));
    CHECK(!net_parse_mac("GG:BB:CC:DD:EE:FF", m));
    CHECK(!net_parse_mac("AA:BB:CC:DD:EE:F", m));
    CHECK(!net_parse_mac("", m));
    CHECK(!net_parse_mac(NULL, m));
}

static void test_url_encode(void)
{
    char out[64];

    net_url_encode("hello world", out, sizeof(out));
    CHECK(strcmp(out, "hello%20world") == 0);

    net_url_encode("a-b_c.d~e", out, sizeof(out));
    CHECK(strcmp(out, "a-b_c.d~e") == 0);

    net_url_encode("&=?", out, sizeof(out));
    CHECK(strcmp(out, "%26%3D%3F") == 0);

    /* UTF-8 Cyrillic "yo": d1 91 */
    net_url_encode("\xd1\x91", out, sizeof(out));
    CHECK(strcmp(out, "%D1%91") == 0);
}

int main(void)
{
    test_ip_list();
    test_format_ip();
    test_mac();
    test_url_encode();

    if (g_fail) {
        printf("NET TESTS FAILED: %d\n", g_fail);
        return 1;
    }
    printf("NET tests: OK\n");
    return 0;
}
