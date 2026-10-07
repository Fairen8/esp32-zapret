// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include "scan_candidates.h"

/* Decoy SNIs: the first is zapret's built-in default; the others are widely
 * allowed domestic domains useful on some providers. */
static const char *DECOYS[] = {
    "www.iana.org",
    "www.yandex.ru",
    "mail.ru",
};

static int add(scan_candidate_t *out, int n, int max, esp_desync_mode_t mode,
               uint32_t fooling, uint8_t ttl, const char *sni, bool rndsni)
{
    if (n >= max) {
        return n;
    }
    out[n].mode = mode;
    out[n].fooling = fooling;
    out[n].ttl = ttl;
    out[n].rndsni = rndsni;
    out[n].sni[0] = 0;
    if (sni != NULL) {
        size_t len = strlen(sni);
        if (len > SCAN_SNI_MAX - 1) {
            len = SCAN_SNI_MAX - 1;
        }
        memcpy(out[n].sni, sni, len);
        out[n].sni[len] = 0;
    }
    return n + 1;
}

int scan_build_candidates(scan_candidate_t *out, int max)
{
    int n = 0;

    /* 1) no desync: the optimal choice when the network is not filtered */
    n = add(out, n, max, ESP_DESYNC_MODE_OFF, ESP_DESYNC_FOOL_NONE, 64, NULL, false);

    /* 2) fake+split with ascending TTL: the first success is the lowest
     *    working TTL, i.e. the closest to the DPI hop (zapret methodology) */
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE_SPLIT, ESP_DESYNC_FOOL_TTL, 3, DECOYS[0], false);
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE_SPLIT, ESP_DESYNC_FOOL_TTL, 5, DECOYS[0], false);
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE_SPLIT, ESP_DESYNC_FOOL_TTL, 8, DECOYS[0], false);
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE_SPLIT, ESP_DESYNC_FOOL_TTL, 12, DECOYS[0], false);

    /* 3) alternative decoy SNIs */
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE_SPLIT, ESP_DESYNC_FOOL_TTL, 5, DECOYS[1], false);
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE_SPLIT, ESP_DESYNC_FOOL_TTL, 5, DECOYS[2], false);
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE, ESP_DESYNC_FOOL_TTL, 3, DECOYS[0], false);

    /* 4) randomized SNI: every fake has a different hostname and size, which
     *    defeats DPI that fingerprints the decoy or the packet length */
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE_SPLIT, ESP_DESYNC_FOOL_TTL, 5, DECOYS[0], true);
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE, ESP_DESYNC_FOOL_TTL, 5, DECOYS[0], true);

    /* 5) fooling variants for providers where TTL tuning is fragile */
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE, ESP_DESYNC_FOOL_MD5SIG, 64, DECOYS[0], false);
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE_SPLIT, ESP_DESYNC_FOOL_BADSEQ, 64, DECOYS[0], false);
    n = add(out, n, max, ESP_DESYNC_MODE_FAKE, ESP_DESYNC_FOOL_DATANOACK, 64, DECOYS[0], false);

    /* 6) no-fake methods */
    n = add(out, n, max, ESP_DESYNC_MODE_SPLIT, ESP_DESYNC_FOOL_NONE, 64, DECOYS[0], false);
    n = add(out, n, max, ESP_DESYNC_MODE_DISORDER, ESP_DESYNC_FOOL_NONE, 64, DECOYS[0], false);
    n = add(out, n, max, ESP_DESYNC_MODE_TLSREC, ESP_DESYNC_FOOL_NONE, 64, DECOYS[0], false);

    return n;
}

static const char *mode_name(esp_desync_mode_t mode)
{
    switch (mode) {
    case ESP_DESYNC_MODE_OFF: return "off";
    case ESP_DESYNC_MODE_SPLIT: return "split";
    case ESP_DESYNC_MODE_DISORDER: return "disorder";
    case ESP_DESYNC_MODE_FAKE: return "fake";
    case ESP_DESYNC_MODE_FAKE_SPLIT: return "fake_split";
    case ESP_DESYNC_MODE_TLSREC: return "tlsrec";
    default: return "?";
    }
}

static const char *fool_name(uint32_t fooling)
{
    if (fooling & ESP_DESYNC_FOOL_TTL) return "ttl";
    if (fooling & ESP_DESYNC_FOOL_MD5SIG) return "md5sig";
    if (fooling & ESP_DESYNC_FOOL_BADSUM) return "badsum";
    if (fooling & ESP_DESYNC_FOOL_BADSEQ) return "badseq";
    if (fooling & ESP_DESYNC_FOOL_DATANOACK) return "datanoack";
    return "none";
}

void scan_format(const scan_candidate_t *c, char *buf, size_t buf_sz)
{
    if (c->mode == ESP_DESYNC_MODE_OFF || c->sni[0] == 0) {
        snprintf(buf, buf_sz, "%s", mode_name(c->mode));
        return;
    }
    snprintf(buf, buf_sz, "%s fool=%s ttl=%u sni=%s%s",
             mode_name(c->mode), fool_name(c->fooling), (unsigned)c->ttl, c->sni,
             c->rndsni ? " rndsni" : "");
}
