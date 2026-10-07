// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Anonymous telemetry payload: no identifiers (no device/chat IDs, no SSIDs,
 * no IP addresses). Pure module: no ESP-IDF dependencies, unit-tested on the
 * host. */
typedef struct {
    const char *event;      /* "boot" | "periodic" | "manual" */
    const char *fw;
    const char *target;
    uint32_t uptime_s;
    uint32_t free_heap;
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
} stats_payload_t;

/* Builds a flat JSON object (schema 1). Returns the length (excluding the
 * terminating NUL) or -1 when it would not fit. */
int stats_build_payload(const stats_payload_t *in, char *buf, size_t cap);
