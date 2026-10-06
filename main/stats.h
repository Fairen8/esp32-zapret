// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>

/* Voluntary anonymous statistics (statistics.fairen8.ru). Disabled unless the
 * owner opts in with the /stats bot command (or CONFIG_APP_STATS_DEFAULT_ON).
 * All network activity is best-effort: failures are ignored and never reported
 * to the user. No identifiers are sent (see stats_payload.h).
 *
 * Note: lwIP (pulled in via esp_wifi) defines a `stats_init()` macro, hence
 * the `stats_anon_` prefix. */

void stats_anon_init(void);

bool stats_anon_enabled(void);
void stats_anon_set_enabled(bool on);

/* Sends a report if enabled and the event throttling allows it.
 * event: "boot" | "periodic" | "manual". Fails silently. */
void stats_anon_report(const char *event);

/* Periodic hook for the main loop (throttled to CONFIG_APP_STATS_INTERVAL_S). */
void stats_anon_tick(void);
