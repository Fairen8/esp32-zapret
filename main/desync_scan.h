// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool have;
    char strategy[80];
    uint32_t scans;
    uint32_t probes;
    uint32_t probe_fails;
    int64_t last_ok_ms; /* esp_timer_get_time()/1000 of the last successful probe, 0 = never */
} scan_status_t;

/* Loads the last successful strategy from NVS (if any) and applies it. */
void scan_init(void);

/* Probes the saved strategy first, then scans the candidate list.
 * Returns 0 when a working strategy was found and saved. */
int scan_find_working(void);

/* Periodic health probe of the current strategy; re-scans after
 * CONFIG_APP_HEALTH_FAIL_THRESHOLD consecutive failures. */
int scan_health_check(void);

void scan_get_status(scan_status_t *out);
