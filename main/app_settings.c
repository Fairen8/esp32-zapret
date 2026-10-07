// SPDX-License-Identifier: MIT
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "app_config.h"
#include "app_settings.h"

static const char *TAG = "settings";

#define NVS_NS "app"

static app_settings_t s_cfg;

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t dst_sz,
                     const char *fallback)
{
    size_t sz = dst_sz;
    if (nvs_get_str(h, key, dst, &sz) != ESP_OK) {
        strlcpy(dst, fallback, dst_sz);
    }
}

esp_err_t app_settings_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        load_str(h, "ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), CFG_WIFI_SSID);
        load_str(h, "pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), CFG_WIFI_PASS);
        load_str(h, "token", s_cfg.tg_token, sizeof(s_cfg.tg_token), CFG_TG_TOKEN);
        load_str(h, "mac", s_cfg.wol_mac, sizeof(s_cfg.wol_mac), CFG_WOL_MAC);
        load_str(h, "bcast", s_cfg.wol_broadcast, sizeof(s_cfg.wol_broadcast), CFG_WOL_BROADCAST);

        int64_t admin = CFG_TG_ADMIN_ID;
        nvs_get_i64(h, "admin", &admin);
        s_cfg.tg_admin_id = admin;

        uint16_t port = CFG_WOL_PORT;
        nvs_get_u16(h, "port", &port);
        s_cfg.wol_port = port;

        load_str(h, "webpass", s_cfg.web_pass, sizeof(s_cfg.web_pass),
                 CONFIG_APP_WEB_PASSWORD);

        nvs_close(h);
    } else {
        strlcpy(s_cfg.wifi_ssid, CFG_WIFI_SSID, sizeof(s_cfg.wifi_ssid));
        strlcpy(s_cfg.wifi_pass, CFG_WIFI_PASS, sizeof(s_cfg.wifi_pass));
        strlcpy(s_cfg.tg_token, CFG_TG_TOKEN, sizeof(s_cfg.tg_token));
        strlcpy(s_cfg.wol_mac, CFG_WOL_MAC, sizeof(s_cfg.wol_mac));
        strlcpy(s_cfg.wol_broadcast, CFG_WOL_BROADCAST, sizeof(s_cfg.wol_broadcast));
        strlcpy(s_cfg.web_pass, CONFIG_APP_WEB_PASSWORD, sizeof(s_cfg.web_pass));
        s_cfg.tg_admin_id = CFG_TG_ADMIN_ID;
        s_cfg.wol_port = CFG_WOL_PORT;
    }

    if (strlen(s_cfg.web_pass) < APP_SETTINGS_WEB_PASS_MIN) {
        strlcpy(s_cfg.web_pass, CONFIG_APP_WEB_PASSWORD, sizeof(s_cfg.web_pass));
    }

    ESP_LOGI(TAG, "wifi=%s bot_token=%s admin=%lld (NVS over build defaults)",
             app_settings_has_wifi() ? s_cfg.wifi_ssid : "(not set)",
             app_settings_has_bot_token() ? "set" : "(not set)",
             (long long)s_cfg.tg_admin_id);
    return ESP_OK;
}

const app_settings_t *app_settings_get(void)
{
    return &s_cfg;
}

bool app_settings_has_wifi(void)
{
    return s_cfg.wifi_ssid[0] != 0 &&
           strcmp(s_cfg.wifi_ssid, APP_SETTINGS_PLACEHOLDER_SSID) != 0;
}

bool app_settings_has_bot_token(void)
{
    return s_cfg.tg_token[0] != 0 &&
           strcmp(s_cfg.tg_token, APP_SETTINGS_PLACEHOLDER_TOKEN) != 0;
}

bool app_settings_is_provisioned(void)
{
#if CONFIG_APP_ENABLE_TELEGRAM_BOT
    return app_settings_has_wifi() && app_settings_has_bot_token();
#else
    return app_settings_has_wifi();
#endif
}

static esp_err_t store_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_settings_set_wifi(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid));
    strlcpy(s_cfg.wifi_pass, pass ? pass : "", sizeof(s_cfg.wifi_pass));

    esp_err_t err = store_str("ssid", s_cfg.wifi_ssid);
    if (err != ESP_OK) {
        return err;
    }
    return store_str("pass", s_cfg.wifi_pass);
}

esp_err_t app_settings_set_token(const char *token)
{
    if (token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_cfg.tg_token, token, sizeof(s_cfg.tg_token));
    return store_str("token", s_cfg.tg_token);
}

esp_err_t app_settings_set_admin_id(int64_t admin_id)
{
    s_cfg.tg_admin_id = admin_id;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i64(h, "admin", admin_id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_settings_set_wol(const char *mac, const char *broadcast, uint16_t port)
{
    if (mac == NULL || mac[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_cfg.wol_mac, mac, sizeof(s_cfg.wol_mac));
    strlcpy(s_cfg.wol_broadcast, broadcast ? broadcast : "", sizeof(s_cfg.wol_broadcast));
    s_cfg.wol_port = port;

    esp_err_t err = store_str("mac", s_cfg.wol_mac);
    if (err != ESP_OK) {
        return err;
    }
    err = store_str("bcast", s_cfg.wol_broadcast);
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t h;
    err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(h, "port", port);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_settings_set_web_pass(const char *pass)
{
    if (pass == NULL || strlen(pass) < APP_SETTINGS_WEB_PASS_MIN) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_cfg.web_pass, pass, sizeof(s_cfg.web_pass));
    return store_str("webpass", s_cfg.web_pass);
}

esp_err_t app_settings_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        return app_settings_init();
    }
    return err;
}
