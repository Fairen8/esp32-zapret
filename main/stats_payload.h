// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Anonymous telemetry payload (schema 2). No stable identifiers are sent: the
 * only pseudonym is `rid`, a rotating value (HMAC of a device-local secret and
 * the current day) that cannot be linked across days without the local secret.
 * Pure module: no ESP-IDF dependencies, unit-tested on the host. */
typedef struct {
    const char *event;      /* "boot" | "periodic" | "manual" */
    const char *fw;
    const char *target;
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t min_heap;      /* minimum free heap since boot (watermark) */
    uint32_t max_block;     /* largest free heap block (fragmentation) */
    int32_t  rssi;
    uint8_t  reset_reason;
    uint8_t  mode;
    uint32_t fooling;
    uint8_t  ttl;
    int16_t  split1;
    int16_t  split2;
    bool     have_strategy;
    const char *strategy;
    uint32_t scans;
    uint32_t probes;
    uint32_t probe_fails;

    /* schema 2 */
    const char *tried;         /* "mode:fool,..." tried before success */
    uint16_t tts_s;            /* seconds to a working strategy; 0xFFFF = none */
    uint16_t strategy_changed; /* strategy switches since boot */
    uint32_t wifi_disc;        /* Wi-Fi disconnects since boot */
    uint8_t  wifi_reason;      /* last disconnect reason (esp_wifi code) */
    bool     boot_storm;       /* 3+ boots within 10 minutes */
    int16_t  temp_c;           /* chip temperature; -128 = unknown */
    const char *errs;          /* top error codes, "0x104:5,0x7f:2" */
    uint16_t mtu;
    uint16_t sntp_s;           /* seconds to a usable clock; 0xFFFF = n/a */
    bool     sntp_ok;          /* true = NTP sync, false = build-time fallback */
    uint8_t  doh;              /* bit0 tried, bit1 ok */
    uint8_t  channel;          /* Wi-Fi primary channel (2.4 GHz) */
    uint8_t  stage;            /* last failure stage: 1 tcp, 2 tls, 3 rst, 4 timeout */
    uint8_t  cfg;              /* bit0 doh, bit1 webui, bit2 bot, bit3 stats default,
                                * bit4 rndsni, bit5 non-default decoy SNI */
    uint16_t cpu_mhz;
    uint32_t flash_free;       /* free bytes in the running app partition */
    bool     psram;
    uint16_t chip_rev;         /* MXX: major * 100 + minor */
    uint8_t  sni_cat;          /* decoy SNI category: 1 iana, 2 yandex, 3 mailru,
                                * 4 custom, 5 random */
    const char *rid;           /* rotating id: HMAC(local secret, day), 16 hex */
} stats_payload_t;

/* Builds a flat JSON object (schema 2). Returns the length (excluding the
 * terminating NUL) or -1 when it would not fit. */
int stats_build_payload(const stats_payload_t *in, char *buf, size_t cap);
