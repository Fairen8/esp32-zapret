// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_settings.h"
#include "setup_mode.h"
#include "webui.h"

static const char *TAG = "setup";

static char s_ap_ssid[33];

static esp_err_t start_setup_ap(void)
{
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta(); /* for the Wi-Fi scan in the web UI */

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), CONFIG_APP_SETUP_AP_PREFIX "-%02X%02X",
             mac[4], mac[5]);

    const app_settings_t *cfg = app_settings_get();
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.max_connection = 4;
    if (strlen(cfg->web_pass) >= APP_SETTINGS_WEB_PASS_MIN) {
        strlcpy((char *)ap.ap.password, cfg->web_pass, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGW(TAG, "setup AP \"%s\" up at http://192.168.4.1 (WPA2 password: %s)",
             s_ap_ssid, cfg->web_pass);
    return ESP_OK;
}

/* ------------------------------------------------------------ serial console */

static int cmd_setwifi(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: setwifi <ssid> <password>\n");
        return 1;
    }
    esp_err_t err = app_settings_set_wifi(argv[1], argv[2]);
    if (err != ESP_OK) {
        printf("error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("wifi saved: %s (reboot to apply)\n", argv[1]);
    return 0;
}

static int cmd_settoken(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: settoken <bot-token>\n");
        return 1;
    }
    esp_err_t err = app_settings_set_token(argv[1]);
    if (err != ESP_OK) {
        printf("error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("bot token saved (reboot to apply)\n");
    return 0;
}

static int cmd_setadmin(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: setadmin <chat-id>  (0 = accept anyone)\n");
        return 1;
    }
    char *end = NULL;
    long long id = strtoll(argv[1], &end, 10);
    if (end == NULL || *end != 0) {
        printf("not a number: %s\n", argv[1]);
        return 1;
    }
    app_settings_set_admin_id((int64_t)id);
    printf("admin id saved: %lld\n", id);
    return 0;
}

static int cmd_setmac(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: setmac <AA:BB:CC:DD:EE:FF> [broadcast] [port]\n");
        return 1;
    }
    const char *broadcast = argc >= 3 ? argv[2] : "255.255.255.255";
    int port = argc >= 4 ? atoi(argv[3]) : 9;
    esp_err_t err = app_settings_set_wol(argv[1], broadcast, (uint16_t)port);
    if (err != ESP_OK) {
        printf("error: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("wol saved: %s -> %s:%d\n", argv[1], broadcast, port);
    return 0;
}

static int cmd_setwebpass(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: setwebpass <password>  (min %d chars)\n", APP_SETTINGS_WEB_PASS_MIN);
        return 1;
    }
    esp_err_t err = app_settings_set_web_pass(argv[1]);
    if (err != ESP_OK) {
        printf("password too short (min %d chars)\n", APP_SETTINGS_WEB_PASS_MIN);
        return 1;
    }
    printf("web/setup password saved (reboot to apply)\n");
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const app_settings_t *cfg = app_settings_get();
    printf("setup AP : %s (http://192.168.4.1)\n", s_ap_ssid);
    printf("wifi     : %s (%s)\n", app_settings_has_wifi() ? cfg->wifi_ssid : "-",
           app_settings_has_wifi() ? "set" : "not set");
    printf("bot token: %s\n", app_settings_has_bot_token() ? "set" : "not set");
    printf("admin id : %lld\n", (long long)cfg->tg_admin_id);
    printf("wol      : %s -> %s:%u\n", cfg->wol_mac, cfg->wol_broadcast, cfg->wol_port);
    printf("provisioned: %s\n", app_settings_is_provisioned() ? "yes" : "no");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("rebooting...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static int cmd_erase(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    app_settings_erase();
    printf("settings erased, rebooting...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static void start_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "esp32-zapret>";
    repl_cfg.max_cmdline_length = 256;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
#else
    esp_console_dev_uart_config_t dev_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&dev_cfg, &repl_cfg, &repl);
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "serial console unavailable: %s", esp_err_to_name(err));
        return;
    }

    const esp_console_cmd_t cmds[] = {
        { .command = "setwifi",    .help = "set Wi-Fi: setwifi <ssid> <password>",              .func = &cmd_setwifi },
        { .command = "settoken",   .help = "set bot token: settoken <token>",                    .func = &cmd_settoken },
        { .command = "setadmin",   .help = "set admin chat id: setadmin <id> (0 = anyone)",      .func = &cmd_setadmin },
        { .command = "setmac",     .help = "set WoL target: setmac <mac> [broadcast] [port]",    .func = &cmd_setmac },
        { .command = "setwebpass", .help = "set web/setup password: setwebpass <password>",      .func = &cmd_setwebpass },
        { .command = "status",     .help = "show saved settings",                                .func = &cmd_status },
        { .command = "reboot",     .help = "restart the device",                                 .func = &cmd_reboot },
        { .command = "erase",      .help = "erase all settings and restart",                     .func = &cmd_erase },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
    /* This IDF starts the REPL task inside esp_console_new_repl_xxx(). */
    (void)repl;
}

void setup_mode_run(void)
{
    ESP_LOGW(TAG, "not provisioned: starting setup mode (AP + web UI + serial console)");

    esp_err_t err = start_setup_ap();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup AP failed: %s", esp_err_to_name(err));
    }

#if CONFIG_APP_WEB_UI
    if (webui_start(true) != ESP_OK) {
        ESP_LOGW(TAG, "web UI unavailable, provision over the serial console");
    }
#endif

    start_console();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "setup mode: connect to \"%s\" and open http://192.168.4.1",
                 s_ap_ssid);
    }
}
