// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "esp_idf_version.h"
#include "sdkconfig.h"
#include "esp_desync.h"
#include "desync_scan.h"
#include "app_config.h"
#include "app_settings.h"
#include "setup_mode.h"
#include "webui.h"
#include "stats.h"
#include "telegram.h"
#include "wol.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 1, 0)
#error "esp32-zapret requires ESP-IDF v5.1 or newer"
#endif

static const char *TAG = "bot";

static volatile bool s_connected;
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_dhcp_timer;
static int s_disconnects;
static int64_t s_sntp_start_us;
static wifi_config_t s_sta_cfg;

static void sntp_start(void);

static void wifi_reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void dhcp_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "DHCP timed out, reconnecting");
    esp_wifi_disconnect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        if (!app_settings_get()->net_static && s_dhcp_timer != NULL) {
            esp_timer_stop(s_dhcp_timer);
            esp_timer_start_once(s_dhcp_timer, 15 * 1000 * 1000);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        stats_anon_note_wifi_disconnect(e->reason);
        s_connected = false;
        if (s_dhcp_timer != NULL) {
            esp_timer_stop(s_dhcp_timer);
        }
        s_disconnects++;
        if (s_disconnects >= 3 && app_settings_get()->wifi_bssid_set) {
            /* The pinned radio is not working (moved AP, multi-radio SSID,
             * DHCP broken): drop the pin and let the driver pick again. */
            ESP_LOGW(TAG, "connection keeps failing, clearing BSSID pin");
            app_settings_clear_bssid();
            s_sta_cfg.sta.bssid_set = false;
            s_sta_cfg.sta.channel = 0;
            esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg);
        }
        ESP_LOGW(TAG, "wifi disconnected, retrying in 2 s (failure %d)", s_disconnects);
        /* Never sleep inside the event loop task: defer the reconnect. */
        if (s_reconnect_timer != NULL) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, 2 * 1000 * 1000);
        } else {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_disconnects = 0;
        if (s_dhcp_timer != NULL) {
            esp_timer_stop(s_dhcp_timer);
        }
        /* Remember the radio that worked so the next boot connects faster. */
        wifi_ap_record_t ap;
        uint8_t channel = 0;
        wifi_second_chan_t second;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
            esp_wifi_get_channel(&channel, &second) == ESP_OK) {
            app_settings_set_bssid(ap.bssid, channel);
        }
        if (s_sntp_start_us == 0) {
            s_sntp_start_us = esp_timer_get_time();
        }
        sntp_start();
    }
}

static bool time_is_valid(void)
{
    time_t now = 0;
    struct tm tm_buf;
    time(&now);
    localtime_r(&now, &tm_buf);
    return (tm_buf.tm_year + 1900) >= 2025;
}

/* lwIP stores SNTP server names by pointer (it does not copy them), so the
 * parsed tokens must point into a static buffer that lives on. */
static char s_sntp_server_buf[128];

static void sntp_start(void)
{
    if (esp_sntp_enabled()) {
        return;
    }
    strlcpy(s_sntp_server_buf, CONFIG_APP_SNTP_SERVERS, sizeof(s_sntp_server_buf));
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    int idx = 0;
    char *save = NULL;
    for (char *tok = strtok_r(s_sntp_server_buf, ",", &save);
         tok != NULL && idx < CONFIG_LWIP_SNTP_MAX_SERVERS;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') {
            tok++;
        }
        if (*tok != 0) {
            esp_sntp_setservername((u8_t)idx++, tok);
        }
    }
    if (idx == 0) {
        esp_sntp_setservername(0, "pool.ntp.org");
    }
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (%d server(s))", idx > 0 ? idx : 1);
}

/* Last-resort clock: the firmware build timestamp (__DATE__/__TIME__).
 * TLS needs an approximately valid time for certificate validation, so a
 * device that cannot reach any NTP server still gets a usable clock. */
static bool time_set_from_build(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    static const char *months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (desc == NULL) {
        return false;
    }
    char mon[4] = {0};
    int day = 0, year = 0, hour = 0, min = 0, sec = 0;
    if (sscanf(desc->date, "%3s %d %d", mon, &day, &year) != 3 ||
        sscanf(desc->time, "%d:%d:%d", &hour, &min, &sec) != 3) {
        return false;
    }
    int mi = -1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon, months[i]) == 0) {
            mi = i;
            break;
        }
    }
    if (mi < 0 || year < 2025) {
        return false;
    }
    struct tm tm_build = {0};
    tm_build.tm_year = year - 1900;
    tm_build.tm_mon = mi;
    tm_build.tm_mday = day;
    tm_build.tm_hour = hour;
    tm_build.tm_min = min;
    tm_build.tm_sec = sec;
    time_t t = mktime(&tm_build); /* ESP-IDF keeps TZ=UTC by default */
    if (t <= 0) {
        return false;
    }
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    return time_is_valid();
}

/* Lightweight liveness mark so a silent console is distinguishable from a
 * stuck application (field debugging on USB-only boards). */
static void heartbeat_log(void)
{
    static int64_t next_us;
    int64_t now = esp_timer_get_time();
    if (now < next_us) {
        return;
    }
    next_us = now + (int64_t)5 * 60 * 1000000;
    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    esp_wifi_sta_get_ap_info(&ap);
    ESP_LOGI(TAG, "heartbeat: uptime %llds, heap %u, rssi %d",
             (long long)(now / 1000000), (unsigned)esp_get_free_heap_size(), ap.rssi);
}

#if CONFIG_APP_ENABLE_TELEGRAM_BOT
static void copy_token(const char *src, char *dst, size_t dst_sz)
{
    size_t i = 0;
    while (src[i] && src[i] != ' ' && i < dst_sz - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* Matches exactly "/name" or "/name@botname" (with optional arguments) and
 * returns the arguments; NULL otherwise, so "/wakeXYZ" is not "/wake". */
static const char *cmd_args(const char *text, const char *name)
{
    if (text[0] != '/') {
        return NULL;
    }
    size_t n = strlen(name);
    if (strncmp(text + 1, name, n) != 0) {
        return NULL;
    }
    const char *q = text + 1 + n;
    if (*q == '@') {
        while (*q != 0 && *q != ' ') {
            q++;
        }
    }
    if (*q != 0 && *q != ' ') {
        return NULL;
    }
    while (*q == ' ') {
        q++;
    }
    return q;
}

/* Simple global command rate limit: one chat cannot flood the device. */
#define CMD_RATE_LIMIT_PER_MIN 20

static bool rate_limit_ok(void)
{
    static int64_t window_start;
    static int count;
    int64_t now = esp_timer_get_time();
    if (now - window_start > 60 * 1000000) {
        window_start = now;
        count = 0;
    }
    return ++count <= CMD_RATE_LIMIT_PER_MIN;
}

static void handle_update(const tg_update_t *u)
{
    const app_settings_t *settings = app_settings_get();

    if (settings->tg_admin_id != 0 && u->chat_id != settings->tg_admin_id) {
        tg_send_message(u->chat_id, "access denied");
        return;
    }
    if (!rate_limit_ok()) {
        tg_send_message(u->chat_id, "too many commands, try again in a minute");
        return;
    }

    char reply[512];
    const char *text = u->text;
    const char *args;

    if ((args = cmd_args(text, "wake")) != NULL || (args = cmd_args(text, "wol")) != NULL) {
        char macbuf[24];
        strlcpy(macbuf, settings->wol_mac, sizeof(macbuf));
        if (args[0]) {
            copy_token(args, macbuf, sizeof(macbuf));
        }
        if (wol_send(macbuf, settings->wol_broadcast, settings->wol_port) == 0) {
            snprintf(reply, sizeof(reply), "magic packet sent to %s", macbuf);
        } else {
            snprintf(reply, sizeof(reply), "WoL failed, bad MAC? %s", macbuf);
        }
        tg_send_message(u->chat_id, reply);

    } else if ((args = cmd_args(text, "desync")) != NULL) {
        char name[24] = {0};
        if (args[0]) {
            copy_token(args, name, sizeof(name));
        }
        if (name[0]) {
            bool ok = false;
            esp_desync_mode_t m = esp_desync_mode_from_name(name, &ok);
            if (ok) {
                esp_desync_config_t c;
                esp_desync_get_config(&c);
                c.mode = m;
                esp_desync_set_config(&c);
                scan_set_manual(true);
                snprintf(reply, sizeof(reply), "desync mode: %s (manual)", esp_desync_mode_name(m));
            } else {
                snprintf(reply, sizeof(reply), "unknown mode, use: off split disorder fake fake_split tlsrec seqovl");
            }
        } else {
            snprintf(reply, sizeof(reply), "usage: /desync <off|split|disorder|fake|fake_split|tlsrec|seqovl>");
        }
        tg_send_message(u->chat_id, reply);

    } else if ((args = cmd_args(text, "ttl")) != NULL) {
        int v = args[0] ? atoi(args) : 0;
        if (v >= 1 && v <= 255) {
            esp_desync_config_t c;
            esp_desync_get_config(&c);
            c.fake_ttl = (uint8_t)v;
            esp_desync_set_config(&c);
            scan_set_manual(true);
            snprintf(reply, sizeof(reply), "fake TTL = %d (manual)", v);
        } else {
            snprintf(reply, sizeof(reply), "usage: /ttl <1..255>, try 3..8");
        }
        tg_send_message(u->chat_id, reply);

    } else if ((args = cmd_args(text, "fool")) != NULL) {
        char name[16] = {0};
        uint32_t f = 0;
        bool ok = true;
        if (args[0]) {
            copy_token(args, name, sizeof(name));
        }
        if (name[0] == 0 || strcmp(name, "ttl") == 0) f = ESP_DESYNC_FOOL_TTL;
        else if (strcmp(name, "md5sig") == 0) f = ESP_DESYNC_FOOL_MD5SIG;
        else if (strcmp(name, "badsum") == 0) f = ESP_DESYNC_FOOL_BADSUM;
        else if (strcmp(name, "badseq") == 0) f = ESP_DESYNC_FOOL_BADSEQ;
        else if (strcmp(name, "datanoack") == 0) f = ESP_DESYNC_FOOL_DATANOACK;
        else if (strcmp(name, "ts") == 0) f = ESP_DESYNC_FOOL_TS;
        else if (strcmp(name, "none") == 0) f = ESP_DESYNC_FOOL_NONE;
        else ok = false;

        if (ok) {
            esp_desync_config_t c;
            esp_desync_get_config(&c);
            c.fooling = f;
            esp_desync_set_config(&c);
            scan_set_manual(true);
            snprintf(reply, sizeof(reply), "fooling: %s (manual)", name[0] ? name : "ttl");
        } else {
            snprintf(reply, sizeof(reply), "usage: /fool ttl|md5sig|badsum|badseq|datanoack|ts|none");
        }
        tg_send_message(u->chat_id, reply);

    } else if (cmd_args(text, "heap") != NULL) {
        snprintf(reply, sizeof(reply), "heap free %u, min ever %u, largest block %u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        tg_send_message(u->chat_id, reply);

    } else if (cmd_args(text, "ip") != NULL) {
        esp_netif_ip_info_t ip_info;
        char ips[16] = "-";
        char gws[16] = "-";
        memset(&ip_info, 0, sizeof(ip_info));
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(ips, sizeof(ips), IPSTR, IP2STR(&ip_info.ip));
            snprintf(gws, sizeof(gws), IPSTR, IP2STR(&ip_info.gw));
        }
        wifi_ap_record_t ap;
        memset(&ap, 0, sizeof(ap));
        esp_wifi_sta_get_ap_info(&ap);
        snprintf(reply, sizeof(reply), "ip %s, gw %s\nssid %s, rssi %d",
                 ips, gws, ap.ssid[0] ? (const char *)ap.ssid : "-", ap.rssi);
        tg_send_message(u->chat_id, reply);

    } else if (cmd_args(text, "reboot") != NULL) {
        tg_send_message(u->chat_id, "rebooting...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();

    } else if ((args = cmd_args(text, "rndsni")) != NULL) {
        char name[8] = {0};
        if (args[0]) {
            copy_token(args, name, sizeof(name));
        }
        esp_desync_config_t c;
        esp_desync_get_config(&c);
        if (name[0] == 0) {
            snprintf(reply, sizeof(reply), "rndsni: %s", c.rndsni ? "on" : "off");
        } else if (strcmp(name, "on") == 0 || strcmp(name, "off") == 0) {
            c.rndsni = strcmp(name, "on") == 0;
            esp_desync_set_config(&c);
            scan_set_manual(true);
            snprintf(reply, sizeof(reply), "rndsni: %s (manual)", name);
        } else {
            snprintf(reply, sizeof(reply), "usage: /rndsni on|off");
        }
        tg_send_message(u->chat_id, reply);

    } else if (cmd_args(text, "scan") != NULL) {
        tg_send_message(u->chat_id, "scanning strategies, up to a minute...");
        int rc = scan_find_working();
        scan_status_t st;
        scan_get_status(&st);
        snprintf(reply, sizeof(reply), rc == 0 ? "strategy: %s (auto)" : "scan failed, staying on: %s",
                 st.strategy);
        tg_send_message(u->chat_id, reply);

    } else if (cmd_args(text, "strategy") != NULL) {
        scan_status_t st;
        scan_get_status(&st);
        snprintf(reply, sizeof(reply),
                 "strategy %s%s\nscans %u, probes %u, fails %u",
                 st.have ? st.strategy : "(none)",
                 st.manual ? " (manual)" : " (auto)",
                 (unsigned)st.scans, (unsigned)st.probes, (unsigned)st.probe_fails);
        tg_send_message(u->chat_id, reply);

    } else if ((args = cmd_args(text, "stats")) != NULL) {
        char arg[8] = {0};
        if (args[0]) {
            copy_token(args, arg, sizeof(arg));
        }
        if (arg[0] == 0) {
            snprintf(reply, sizeof(reply), "anonymous statistics: %s",
                     stats_anon_enabled() ? "on" : "off");
        } else if (strcmp(arg, "on") == 0) {
            stats_anon_set_enabled(true);
            stats_anon_report("manual");
            snprintf(reply, sizeof(reply), "anonymous statistics: on");
        } else if (strcmp(arg, "off") == 0) {
            stats_anon_set_enabled(false);
            snprintf(reply, sizeof(reply), "anonymous statistics: off");
        } else {
            snprintf(reply, sizeof(reply), "usage: /stats on|off");
        }
        tg_send_message(u->chat_id, reply);

    } else if (cmd_args(text, "status") != NULL) {
        wifi_ap_record_t ap;
        esp_desync_config_t c;
        scan_status_t st;
        memset(&ap, 0, sizeof(ap));
        esp_wifi_sta_get_ap_info(&ap);
        esp_desync_get_config(&c);
        scan_get_status(&st);
        snprintf(reply, sizeof(reply),
                 "uptime %llds, heap %u, rssi %d\nmode %s, ttl %u, fool 0x%x, rndsni %s\nbypass %s (%s)\ntg %s (last HTTP %d)",
                 (long long)(esp_timer_get_time() / 1000000),
                 (unsigned)esp_get_free_heap_size(), ap.rssi,
                 esp_desync_mode_name(c.mode), (unsigned)c.fake_ttl, (unsigned)c.fooling,
                 c.rndsni ? "on" : "off",
                 st.have ? st.strategy : "(none)", st.manual ? "manual" : "auto",
                 tg_last_endpoint(), tg_last_http_status());
        tg_send_message(u->chat_id, reply);

    } else {
        tg_send_message(u->chat_id,
                        "esp32-zapret\n"
                        "/wake [mac] - send Wake-on-LAN magic packet\n"
                        "/status - device state\n"
                        "/desync <mode> - off|split|disorder|fake|fake_split|tlsrec|seqovl\n"
                        "/ttl <n> - fake packet TTL (tune 3..8)\n"
                        "/fool <mode> - ttl|md5sig|badsum|badseq|datanoack|ts|none\n"
                        "/rndsni on|off - random decoy SNI for every fake\n"
                        "/heap - free/min/largest heap block\n"
                        "/ip - current IP, gateway, SSID and RSSI\n"
                        "/reboot - restart the device\n"
                        "/scan - re-run strategy auto-detection (drops manual tuning)\n"
                        "/strategy - show current strategy and stats\n"
                        "/stats on|off - anonymous statistics (optional)\n"
                        "manual /desync,/ttl,/fool persist until /scan or repeated failures");
    }
}
#endif /* CONFIG_APP_ENABLE_TELEGRAM_BOT */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(app_settings_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Prebuilt firmware has placeholder credentials: instead of hanging,
     * run first-boot provisioning (setup AP + web UI + serial console). */
    if (!app_settings_is_provisioned()) {
        setup_mode_run(); /* never returns */
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    const esp_timer_create_args_t reconnect_args = {
        .callback = wifi_reconnect_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &s_reconnect_timer));

    const esp_timer_create_args_t dhcp_args = {
        .callback = dhcp_timeout_cb,
        .name = "dhcp_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&dhcp_args, &s_dhcp_timer));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    const app_settings_t *settings = app_settings_get();
    memset(&s_sta_cfg, 0, sizeof(s_sta_cfg));
    strlcpy((char *)s_sta_cfg.sta.ssid, settings->wifi_ssid, sizeof(s_sta_cfg.sta.ssid));
    strlcpy((char *)s_sta_cfg.sta.password, settings->wifi_pass, sizeof(s_sta_cfg.sta.password));
    if (settings->wifi_bssid_set) {
        memcpy(s_sta_cfg.sta.bssid, settings->wifi_bssid, sizeof(s_sta_cfg.sta.bssid));
        s_sta_cfg.sta.bssid_set = true;
        s_sta_cfg.sta.channel = settings->wifi_channel;
        ESP_LOGI(TAG, "pinned to AP %02x:%02x:%02x:%02x:%02x:%02x (channel %u)",
                 settings->wifi_bssid[0], settings->wifi_bssid[1], settings->wifi_bssid[2],
                 settings->wifi_bssid[3], settings->wifi_bssid[4], settings->wifi_bssid[5],
                 (unsigned)settings->wifi_channel);
    }

    if (settings->net_static && settings->static_ip[0]) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_str_to_ip4(settings->static_ip, &ip_info.ip) == ESP_OK &&
            esp_netif_str_to_ip4(settings->static_gw, &ip_info.gw) == ESP_OK &&
            esp_netif_str_to_ip4(settings->static_mask, &ip_info.netmask) == ESP_OK) {
            esp_netif_dhcpc_stop(sta_netif);
            if (esp_netif_set_ip_info(sta_netif, &ip_info) == ESP_OK) {
                esp_netif_dns_info_t dns = {0};
                dns.ip.type = ESP_IPADDR_TYPE_V4;
                dns.ip.u_addr.ip4 = ip_info.gw;
                esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns);
                ESP_LOGI(TAG, "static ip %s gw %s mask %s", settings->static_ip,
                         settings->static_gw, settings->static_mask);
            } else {
                ESP_LOGW(TAG, "failed to apply static ip, falling back to DHCP");
                esp_netif_dhcpc_start(sta_netif);
            }
        } else {
            ESP_LOGW(TAG, "invalid static ip config, using DHCP");
        }
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_ERROR_CHECK(esp_desync_init(NULL));

    ESP_LOGI(TAG, "waiting for network...");
    int64_t last_warn_us = 0;
    while (!s_connected) {
        int64_t now = esp_timer_get_time();
        if (now - last_warn_us > 30 * 1000000) {
            ESP_LOGW(TAG, "no IP address yet (check Wi-Fi credentials/AP)");
            last_warn_us = now;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "waiting for time (SNTP, up to %d s)...", CONFIG_APP_SNTP_TIMEOUT_S);
    int64_t time_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_APP_SNTP_TIMEOUT_S * 1000000;
    bool fallback_done = false;
    last_warn_us = 0;
    while (!time_is_valid()) {
        int64_t now = esp_timer_get_time();
        if (!fallback_done && now > time_deadline_us) {
            fallback_done = true;
            if (time_set_from_build()) {
                ESP_LOGW(TAG, "SNTP did not sync in %d s, using build timestamp",
                         CONFIG_APP_SNTP_TIMEOUT_S);
            } else {
                ESP_LOGE(TAG, "SNTP did not sync and the build timestamp is unusable");
            }
        }
        if (now - last_warn_us > 30 * 1000000) {
            ESP_LOGW(TAG, "time is not set yet, TLS cannot work without it");
            last_warn_us = now;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (s_sntp_start_us > 0) {
        stats_anon_note_sntp((uint16_t)((esp_timer_get_time() - s_sntp_start_us) / 1000000),
                             !fallback_done);
    }

    scan_init();
#if CONFIG_APP_SCAN_ON_BOOT
    ESP_LOGI(TAG, "auto-detecting optimal desync strategy...");
    if (scan_find_working() != 0) {
        ESP_LOGW(TAG, "no working strategy detected yet; will retry periodically");
    }
#endif

#if CONFIG_APP_WEB_UI
    if (webui_start(false) != ESP_OK) {
        ESP_LOGW(TAG, "web UI unavailable");
    }
#endif

    stats_anon_init();
    stats_anon_report("boot");

#if CONFIG_APP_ENABLE_TELEGRAM_BOT
    ESP_LOGI(TAG, "ready, starting telegram long-poll");

    int64_t offset = 0;
    int errs = 0;
    int64_t next_health = esp_timer_get_time() + (int64_t)CONFIG_APP_HEALTH_CHECK_INTERVAL_S * 1000000;
    tg_update_t upd;

    while (1) {
        if (!s_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (esp_timer_get_time() >= next_health) {
            scan_health_check();
            next_health = esp_timer_get_time() + (int64_t)CONFIG_APP_HEALTH_CHECK_INTERVAL_S * 1000000;
        }

        stats_anon_tick();
        heartbeat_log();

        int r = tg_get_updates(&upd, offset, 25);
        if (r > 0) {
            errs = 0;
            offset = upd.update_id + 1;
            if (upd.text[0]) {
                ESP_LOGI(TAG, "cmd from %lld: %s", (long long)upd.chat_id, upd.text);
                handle_update(&upd);
            }
        } else if (r == TG_RC_HTTP) {
            /* e.g. 409 Conflict: another getUpdates consumer. Do not hammer. */
            vTaskDelay(pdMS_TO_TICKS(15000));
        } else if (r < 0) {
            errs++;
            if (errs >= 3) {
                errs = 0;
                ESP_LOGW(TAG, "repeated transport errors, checking bypass");
                scan_health_check();
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
#else
    ESP_LOGI(TAG, "ready (no-bot build): periodic TLS self-test through esp_desync");

    while (1) {
        if (!s_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (tg_selftest() != 0) {
            scan_health_check();
        }
        stats_anon_tick();
        heartbeat_log();
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
#endif
}

