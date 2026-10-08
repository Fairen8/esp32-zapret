// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Runtime settings. Compile-time values from secrets.h are the defaults;
 * values saved in NVS (serial console, web UI or bot) take precedence, so a
 * prebuilt firmware image can be configured without rebuilding. */
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char tg_token[128];
    int64_t tg_admin_id;
    char wol_mac[24];
    char wol_broadcast[24];
    uint16_t wol_port;
    char web_pass[33];
    /* BSSID pinning: AP that last handed out a DHCP lease and the channel
     * hint, so the next boot connects to the same radio without a full scan. */
    uint8_t wifi_bssid[6];
    bool wifi_bssid_set;
    uint8_t wifi_channel;
    /* Optional static IPv4 (DHCP disabled when enabled). */
    bool net_static;
    char static_ip[16];
    char static_gw[16];
    char static_mask[16];
} app_settings_t;

/* Loads NVS values over the build-time defaults. Call after nvs_flash_init(). */
esp_err_t app_settings_init(void);

const app_settings_t *app_settings_get(void);

/* Placeholders from secrets_example.h count as "not configured". */
bool app_settings_has_wifi(void);
bool app_settings_has_bot_token(void);
bool app_settings_is_provisioned(void);

esp_err_t app_settings_set_wifi(const char *ssid, const char *pass);
esp_err_t app_settings_set_token(const char *token);
esp_err_t app_settings_set_admin_id(int64_t admin_id);
esp_err_t app_settings_set_wol(const char *mac, const char *broadcast, uint16_t port);

/* Password for the setup AP and the HTTP Basic auth of the web UI.
 * Must be at least APP_SETTINGS_WEB_PASS_MIN characters. */
esp_err_t app_settings_set_web_pass(const char *pass);

/* Remembers the access point that successfully provided a lease. */
void app_settings_set_bssid(const uint8_t bssid[6], uint8_t channel);
void app_settings_clear_bssid(void);

/* Static IPv4 fallback; when enabled, DHCP is not used. */
esp_err_t app_settings_set_static(bool enable, const char *ip, const char *gw,
                                  const char *mask);

/* Wipes all runtime settings (build-time defaults apply again). */
esp_err_t app_settings_erase(void);

#define APP_SETTINGS_PLACEHOLDER_SSID  "your-ssid"
#define APP_SETTINGS_PLACEHOLDER_TOKEN "123456789:PUT_YOUR_TOKEN_HERE"
#define APP_SETTINGS_WEB_PASS_MIN 8
