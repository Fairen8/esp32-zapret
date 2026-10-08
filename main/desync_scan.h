// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool have;
    bool manual;   /* true while /desync,/ttl,/fool user settings are in effect */
    char strategy[80];
    uint32_t scans;
    uint32_t probes;
    uint32_t probe_fails;
    int64_t last_ok_ms; /* esp_timer_get_time()/1000 of the last successful probe, 0 = never */
} scan_status_t;

/* Scanner telemetry for the anonymous statistics (stats schema 2). */
#define SCAN_TRIED_MAX 48

typedef struct {
    char tried[SCAN_TRIED_MAX]; /* "mode:fool,..." tried before success */
    uint16_t tts_s;             /* seconds to a working strategy; 0xFFFF = none */
    uint16_t changes;           /* strategy switches since boot */
    uint8_t stage;              /* last probe failure stage (HTTPS_STAGE_*) */
} scan_telemetry_t;

/* Loads the last successful strategy from NVS (if any) and applies it. */
void scan_init(void);

/* Probes the saved strategy first, then scans the candidate list.
 * Returns 0 when a working strategy was found and saved.
 * Clears a manual override (explicit user re-scan). */
int scan_find_working(void);

/* Periodic health probe of the current strategy; re-scans after
 * CONFIG_APP_HEALTH_FAIL_THRESHOLD consecutive failures.
 * A manual override is probed as-is and only dropped after the same number
 * of consecutive failures. */
int scan_health_check(void);

/* Marks (and persists) that the current desync config was set by hand:
 * health checks then probe it without re-applying the saved auto strategy. */
void scan_set_manual(bool on);

bool scan_is_manual(void);

/* Drops the saved strategy and any manual override (factory reset). */
void scan_erase_saved(void);

void scan_get_status(scan_status_t *out);

/* Fills scan_telemetry_t (tried list, time-to-strategy, switch count, last
 * failure stage) for the statistics payload. */
void scan_get_telemetry(scan_telemetry_t *out);
