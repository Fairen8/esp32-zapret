// SPDX-License-Identifier: MIT
#include <string.h>
#include "desync_tls.h"
#include "esp_random.h"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

size_t desync_tls_random_sni(char *buf, size_t buf_sz)
{
    static const char alnum[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static const char *const tlds[] = { "com", "org", "net", "ru" };

    if (buf == NULL || buf_sz < 16) {
        return 0;
    }
    size_t label = 6 + (esp_random() % 13); /* 6..18 characters */
    if (label + 6 >= buf_sz) {
        label = buf_sz - 6;
    }

    size_t p = 0;
    for (size_t i = 0; i < label; i++) {
        buf[p++] = alnum[esp_random() % (sizeof(alnum) - 1)];
    }
    buf[p++] = '.';
    const char *tld = tlds[esp_random() % (sizeof(tlds) / sizeof(tlds[0]))];
    size_t tld_len = strlen(tld);
    memcpy(buf + p, tld, tld_len);
    p += tld_len;
    buf[p] = 0;
    return p;
}

int desync_tls_find_sni(const uint8_t *buf, size_t len, size_t *sni_off, size_t *sni_len)
{
    *sni_off = 0;
    *sni_len = 0;

    if (len < 44 || buf[0] != 0x16 || buf[1] != 0x03) {
        return -1;
    }

    size_t rec_len = rd16(buf + 3);
    if (rec_len + 5 > len) {
        rec_len = len - 5;
    }

    const uint8_t *p = buf + 5;
    size_t rem = rec_len;
    if (rem < 4 || p[0] != 0x01) {
        return -1;
    }

    size_t hs_len = rd24(p + 1);
    if (hs_len + 4 > rem) {
        hs_len = rem - 4;
    }
    p += 4;
    rem = hs_len;

    if (rem < 2 + 32 + 1) {
        return -1;
    }

    size_t pos = 2 + 32;
    size_t sid_len = p[pos++];
    if (pos + sid_len + 2 > rem) {
        return -1;
    }
    pos += sid_len;

    size_t cs_len = rd16(p + pos);
    pos += 2;
    if (pos + cs_len + 1 > rem) {
        return -1;
    }
    pos += cs_len;

    size_t comp_len = p[pos++];
    if (pos + comp_len > rem) {
        return -1;
    }
    pos += comp_len;

    if (pos + 2 > rem) {
        return -1;
    }
    size_t ext_total = rd16(p + pos);
    pos += 2;
    size_t ext_end = pos + ext_total;
    if (ext_end > rem) {
        ext_end = rem;
    }

    while (pos + 4 <= ext_end) {
        uint16_t etype = rd16(p + pos);
        uint16_t elen = rd16(p + pos + 2);
        size_t body = pos + 4;
        if (body + elen > ext_end) {
            break;
        }
        if (etype == 0x0000 && elen >= 5) {
            size_t nlen = rd16(p + body + 3);
            if (p[body + 2] == 0x00 && 5 + nlen <= elen && nlen > 0) {
                *sni_off = (size_t)(p + body + 5 - buf);
                *sni_len = nlen;
                return 0;
            }
        }
        pos = body + elen;
    }

    return -1;
}

static size_t put_ext(uint8_t *b, size_t p, uint16_t type, const uint8_t *data, size_t dlen)
{
    b[p++] = (uint8_t)(type >> 8);
    b[p++] = (uint8_t)(type & 0xff);
    b[p++] = (uint8_t)(dlen >> 8);
    b[p++] = (uint8_t)(dlen & 0xff);
    if (dlen) {
        memcpy(b + p, data, dlen);
    }
    return p + dlen;
}

size_t desync_tls_build_fake(uint8_t *out, size_t out_sz, const char *sni,
                             const uint8_t *real, size_t real_len, bool clone_random)
{
    if (sni == NULL || sni[0] == '\0') {
        sni = "www.iana.org";
    }
    size_t sni_len = strlen(sni);
    if (sni_len > 200) {
        sni_len = 200;
    }

    uint8_t body[DESYNC_FAKE_MAX];
    size_t p = 0;

    body[p++] = 0x03;
    body[p++] = 0x03;

    if (clone_random && real != NULL && real_len >= 43 && real[0] == 0x16) {
        memcpy(body + p, real + 11, 32);
    } else {
        for (int i = 0; i < 32; i += 4) {
            uint32_t r = esp_random();
            memcpy(body + p + i, &r, 4);
        }
    }
    p += 32;

    body[p++] = 0x00;

    static const uint8_t suites[] = {0x13, 0x01, 0x13, 0x02, 0x13, 0x03, 0xc0, 0x2b};
    body[p++] = 0x00;
    body[p++] = sizeof(suites);
    memcpy(body + p, suites, sizeof(suites));
    p += sizeof(suites);

    body[p++] = 0x01;
    body[p++] = 0x00;

    size_t ext_len_pos = p;
    p += 2;

    {
        uint8_t d[5 + 200];
        size_t dl = 0;
        size_t l = 1 + 2 + sni_len;
        d[dl++] = (uint8_t)(l >> 8);
        d[dl++] = (uint8_t)(l & 0xff);
        d[dl++] = 0x00;
        d[dl++] = (uint8_t)(sni_len >> 8);
        d[dl++] = (uint8_t)(sni_len & 0xff);
        memcpy(d + dl, sni, sni_len);
        dl += sni_len;
        p = put_ext(body, p, 0x0000, d, dl);
    }
    {
        static const uint8_t d[] = {0x00, 0x04, 0x00, 0x1d, 0x00, 0x17};
        p = put_ext(body, p, 0x000a, d, sizeof(d));
    }
    {
        static const uint8_t d[] = {0x01, 0x00};
        p = put_ext(body, p, 0x000b, d, sizeof(d));
    }
    {
        static const uint8_t d[] = {0x00, 0x04, 0x04, 0x03, 0x08, 0x04};
        p = put_ext(body, p, 0x000d, d, sizeof(d));
    }
    {
        static const uint8_t d[] = {0x02, 0x03, 0x04};
        p = put_ext(body, p, 0x002b, d, sizeof(d));
    }

    size_t ext_len = p - ext_len_pos - 2;
    body[ext_len_pos] = (uint8_t)(ext_len >> 8);
    body[ext_len_pos + 1] = (uint8_t)(ext_len & 0xff);

    size_t hs_len = p;        /* handshake body length (no 4-byte header) */
    size_t rec_len = p + 4;   /* TLS record payload: handshake header + body */
    if (out_sz < 5 + rec_len) {
        return 0;
    }

    size_t q = 0;
    out[q++] = 0x16;
    out[q++] = 0x03;
    out[q++] = 0x01;
    out[q++] = (uint8_t)(rec_len >> 8);
    out[q++] = (uint8_t)(rec_len & 0xff);
    out[q++] = 0x01;
    out[q++] = (uint8_t)(hs_len >> 16);
    out[q++] = (uint8_t)(hs_len >> 8);
    out[q++] = (uint8_t)(hs_len & 0xff);
    memcpy(out + q, body, p);
    q += p;

    return q;
}
