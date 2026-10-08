// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Anonymous statistics (statistics.fairen8.ru). Enabled unless the owner
 * disables it with the /stats bot command, the web UI toggle or
 * CONFIG_APP_STATS_DEFAULT_ON=n. All network activity is best-effort: failures
 * are ignored and never reported to the user. No stable identifiers are sent;
 * see stats_payload.h for the full field list and the rotating pseudonym.
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

/* Diagnostics collected for the next report. All of these are safe to call
 * before stats_anon_init(). */
void stats_anon_note_wifi_disconnect(uint8_t reason);
void stats_anon_note_sntp(uint16_t seconds, bool ok);
/* code: mbedTLS error codes are positive (0x6xxx-0x7xxx); 0x0001 no TCP,
 * 0x0002 timeout, 0x0003 connection reset; esp_err values as-is. */
void stats_anon_note_err(uint16_t code);
