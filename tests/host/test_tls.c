// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "desync_tls.h"

static int g_fail;

#define CHECK(cond) do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail++; \
        } \
    } while (0)

/* Independent minimal TLS 1.2 ClientHello fixture: version, random, one
 * cipher suite, no compression methods beyond null, a dummy supported_groups
 * extension and the SNI extension. Records the SNI offset for assertions. */
static size_t make_hello(uint8_t *out, size_t cap, const char *sni,
                         const uint8_t random32[32], size_t *sni_off)
{
    size_t n = strlen(sni);
    if (cap < 160 + n) {
        return 0;
    }

    size_t p = 5;                     /* record header is written at the end */
    out[p++] = 0x01;                  /* handshake: ClientHello */
    size_t hs_len_pos = p;
    p += 3;

    out[p++] = 0x03;
    out[p++] = 0x03;                  /* client version TLS 1.2 */
    for (int i = 0; i < 32; i++) {
        out[p++] = random32 ? random32[i] : (uint8_t)(0x11 + i);
    }
    out[p++] = 0x00;                  /* session id length */
    out[p++] = 0x00;
    out[p++] = 0x02;                  /* cipher suites length */
    out[p++] = 0xc0;
    out[p++] = 0x2b;                  /* one ECDHE suite */
    out[p++] = 0x01;
    out[p++] = 0x00;                  /* compression: null */

    size_t ext_len_pos = p;
    p += 2;                           /* extensions length placeholder */

    out[p++] = 0x00;
    out[p++] = 0x0a;                  /* dummy supported_groups */
    out[p++] = 0x00;
    out[p++] = 0x02;
    out[p++] = 0x00;
    out[p++] = 0x17;

    out[p++] = 0x00;
    out[p++] = 0x00;                  /* SNI extension type */
    out[p++] = (uint8_t)((n + 5) >> 8);
    out[p++] = (uint8_t)((n + 5) & 0xff);
    out[p++] = (uint8_t)((n + 3) >> 8);
    out[p++] = (uint8_t)((n + 3) & 0xff);
    out[p++] = 0x00;                  /* name type: host_name */
    out[p++] = (uint8_t)(n >> 8);
    out[p++] = (uint8_t)(n & 0xff);
    if (sni_off) {
        *sni_off = p;
    }
    memcpy(out + p, sni, n);
    p += n;

    size_t ext_len = p - ext_len_pos - 2;
    out[ext_len_pos] = (uint8_t)(ext_len >> 8);
    out[ext_len_pos + 1] = (uint8_t)(ext_len & 0xff);

    size_t body_len = p - (hs_len_pos + 3);
    out[hs_len_pos] = (uint8_t)(body_len >> 16);
    out[hs_len_pos + 1] = (uint8_t)(body_len >> 8);
    out[hs_len_pos + 2] = (uint8_t)(body_len & 0xff);

    size_t rec_len = 4 + body_len;
    out[0] = 0x16;
    out[1] = 0x03;
    out[2] = 0x01;
    out[3] = (uint8_t)(rec_len >> 8);
    out[4] = (uint8_t)(rec_len & 0xff);
    return p;
}

static size_t make_hello_no_sni(uint8_t *out, size_t cap)
{
    if (cap < 64) {
        return 0;
    }
    size_t p = 5;
    out[p++] = 0x01;
    size_t hs_len_pos = p;
    p += 3;
    out[p++] = 0x03;
    out[p++] = 0x03;
    for (int i = 0; i < 32; i++) {
        out[p++] = 0x22;
    }
    out[p++] = 0x00;
    out[p++] = 0x00;
    out[p++] = 0x02;
    out[p++] = 0xc0;
    out[p++] = 0x2b;
    out[p++] = 0x01;
    out[p++] = 0x00;
    out[p++] = 0x00;
    out[p++] = 0x00;                  /* empty extensions */

    size_t body_len = p - (hs_len_pos + 3);
    out[hs_len_pos] = (uint8_t)(body_len >> 16);
    out[hs_len_pos + 1] = (uint8_t)(body_len >> 8);
    out[hs_len_pos + 2] = (uint8_t)(body_len & 0xff);

    size_t rec_len = 4 + body_len;
    out[0] = 0x16;
    out[1] = 0x03;
    out[2] = 0x01;
    out[3] = (uint8_t)(rec_len >> 8);
    out[4] = (uint8_t)(rec_len & 0xff);
    return p;
}

static void test_find_sni_basic(void)
{
    uint8_t buf[256];
    size_t off = 0;
    size_t n = make_hello(buf, sizeof(buf), "example.com", NULL, &off);
    CHECK(n > 0);

    size_t so = 0;
    size_t sl = 0;
    CHECK(desync_tls_find_sni(buf, n, &so, &sl) == 0);
    CHECK(so == off);
    CHECK(sl == strlen("example.com"));
    CHECK(memcmp(buf + so, "example.com", sl) == 0);
}

static void test_find_sni_no_ext_and_garbage(void)
{
    uint8_t buf[256];
    size_t n = make_hello_no_sni(buf, sizeof(buf));
    CHECK(n > 0);
    size_t so = 0;
    size_t sl = 0;
    CHECK(desync_tls_find_sni(buf, n, &so, &sl) == -1);

    memset(buf, 0xff, sizeof(buf));
    CHECK(desync_tls_find_sni(buf, sizeof(buf), &so, &sl) == -1);

    n = make_hello(buf, sizeof(buf), "example.com", NULL, NULL);
    CHECK(n > 0);
    CHECK(desync_tls_find_sni(buf, 20, &so, &sl) == -1);
}

static void test_build_fake(void)
{
    uint8_t fake[DESYNC_FAKE_MAX];
    size_t n = desync_tls_build_fake(fake, sizeof(fake), "www.iana.org", NULL, 0, false);
    CHECK(n > 44);
    CHECK(fake[0] == 0x16);
    CHECK(fake[1] == 0x03);
    CHECK(fake[5] == 0x01);
    size_t rec_len = (size_t)((fake[3] << 8) | fake[4]);
    size_t hs_len = ((size_t)fake[6] << 16) | ((size_t)fake[7] << 8) | fake[8];
    CHECK(rec_len + 5 == n);
    /* The 3-byte handshake length covers the body only; writing the record
     * length there (off by 4) makes strict DPIs drop the fake as malformed. */
    CHECK(hs_len == rec_len - 4);
    CHECK(hs_len + 4 + 5 == n);

    size_t so = 0;
    size_t sl = 0;
    CHECK(desync_tls_find_sni(fake, n, &so, &sl) == 0);
    CHECK(sl == strlen("www.iana.org"));
    CHECK(memcmp(fake + so, "www.iana.org", sl) == 0);
}

static void test_build_fake_default_sni(void)
{
    uint8_t fake[DESYNC_FAKE_MAX];
    size_t n = desync_tls_build_fake(fake, sizeof(fake), NULL, NULL, 0, false);
    CHECK(n > 44);
    size_t so = 0;
    size_t sl = 0;
    CHECK(desync_tls_find_sni(fake, n, &so, &sl) == 0);
    CHECK(sl == strlen("www.iana.org"));
}

static void test_build_fake_clone_random(void)
{
    uint8_t buf[256];
    uint8_t rnd[32];
    for (int i = 0; i < 32; i++) {
        rnd[i] = (uint8_t)(0xa0 + i);
    }
    size_t hn = make_hello(buf, sizeof(buf), "mail.example.org", rnd, NULL);
    CHECK(hn > 0);

    uint8_t fake[DESYNC_FAKE_MAX];
    size_t n = desync_tls_build_fake(fake, sizeof(fake), "www.iana.org", buf, hn, true);
    CHECK(n > 0);
    CHECK(memcmp(fake + 11, rnd, 32) == 0);
}

static void test_build_fake_small_and_long(void)
{
    uint8_t small[16];
    CHECK(desync_tls_build_fake(small, sizeof(small), "www.iana.org", NULL, 0, false) == 0);

    char long_sni[301];
    memset(long_sni, 'a', sizeof(long_sni) - 1);
    long_sni[0] = 'x';
    long_sni[299] = 'z';
    long_sni[300] = '\0';

    uint8_t fake[DESYNC_FAKE_MAX];
    size_t n = desync_tls_build_fake(fake, sizeof(fake), long_sni, NULL, 0, false);
    CHECK(n > 0);
    size_t so = 0;
    size_t sl = 0;
    CHECK(desync_tls_find_sni(fake, n, &so, &sl) == 0);
    CHECK(sl == 200);
    CHECK(fake[so] == 'x');
    CHECK(fake[so + 199] == 'a');
}

static void test_random_sni(void)
{
    static const char *tlds[] = { "com", "org", "net", "ru" };
    char buf[32];

    for (int i = 0; i < 200; i++) {
        size_t n = desync_tls_random_sni(buf, sizeof(buf));
        CHECK(n >= 9 && n <= 24);
        CHECK(strlen(buf) == n);

        const char *dot = strchr(buf, '.');
        CHECK(dot != NULL && dot > buf && dot[1] != 0);

        bool tld_ok = false;
        for (int t = 0; t < 4; t++) {
            if (strcmp(dot + 1, tlds[t]) == 0) {
                tld_ok = true;
            }
        }
        CHECK(tld_ok);
        for (const char *p = buf; p < dot; p++) {
            CHECK((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'));
        }

        uint8_t fake[DESYNC_FAKE_MAX];
        size_t fl = desync_tls_build_fake(fake, sizeof(fake), buf, NULL, 0, false);
        CHECK(fl > 0);
        size_t so = 0;
        size_t sl = 0;
        CHECK(desync_tls_find_sni(fake, fl, &so, &sl) == 0);
        CHECK(sl == strlen(buf));
    }

    CHECK(desync_tls_random_sni(buf, 8) == 0);
}

int main(void)
{
    test_find_sni_basic();
    test_find_sni_no_ext_and_garbage();
    test_build_fake();
    test_build_fake_default_sni();
    test_build_fake_clone_random();
    test_build_fake_small_and_long();
    test_random_sni();

    if (g_fail) {
        printf("TLS TESTS FAILED: %d\n", g_fail);
        return 1;
    }
    printf("TLS tests: OK\n");
    return 0;
}
