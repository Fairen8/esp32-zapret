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
#include "net_scan.h"
#include "net_utils.h"

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

/* DHCP-failure AP search: with multi-radio SSIDs the driver may associate to
 * a repeater that never relays DHCP. After repeated timeouts the device scans
 * for other radios of the same SSID and tries them one by one. */
#define WIFI_BSSID_CAND_MAX 8
typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
} wifi_bssid_cand_t;

static uint8_t s_dhcp_fails;
static bool s_bssid_scan_running;
static bool s_ap_search;
static wifi_bssid_cand_t s_candidates[WIFI_BSSID_CAND_MAX];
static int s_cand_count;
static int s_cand_idx;
static uint8_t s_last_bssid[6];
static bool s_last_bssid_valid;
static int64_t s_last_scan_us;

static void sntp_start(void);

static void wifi_reconnect_cb(void *arg)
{
    (void)arg;
    if (s_bssid_scan_running) {
        /* The AP scan task resumes the connection attempts when done. */
        return;
    }
    esp_wifi_connect();
}

/* Blocking scan must run in its own task: from the esp_timer callback
 * esp_wifi_scan_start(..., true) fails with ESP_ERR_WIFI_TIMEOUT. */
static void bssid_scan_task(void *arg)
{
    (void)arg;
    /* Let esp_wifi_disconnect() settle: scanning while still associated
     * fails with ESP_ERR_WIFI_STATE. */
    vTaskDelay(pdMS_TO_TICKS(300));

    wifi_scan_config_t sc = {0};
    sc.ssid = s_sta_cfg.sta.ssid;
    sc.show_hidden = false;

    wifi_ap_record_t recs[WIFI_BSSID_CAND_MAX];
    uint16_t want = WIFI_BSSID_CAND_MAX;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK &&
        esp_wifi_scan_get_ap_records(&want, recs) == ESP_OK) {
        s_cand_count = 0;
        for (int i = 0; i < want && s_cand_count < WIFI_BSSID_CAND_MAX; i++) {
            if (s_last_bssid_valid && memcmp(recs[i].bssid, s_last_bssid, 6) == 0) {
                continue; /* the radio that just failed DHCP */
            }
            memcpy(s_candidates[s_cand_count].bssid, recs[i].bssid, 6);
            s_candidates[s_cand_count].channel = recs[i].primary;
            s_cand_count++;
        }
        s_cand_idx = 0;
        ESP_LOGI(TAG, "AP scan for %s: %d alternate radio(s)",
                 (const char *)s_sta_cfg.sta.ssid, s_cand_count);
    } else {
        ESP_LOGW(TAG, "AP scan failed");
    }
    s_bssid_scan_running = false;

    /* Connection attempts were suppressed while the scan was running. */
    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, 300 * 1000);
    } else {
        esp_wifi_connect();
    }
    vTaskDelete(NULL);
}

static void dhcp_timeout_cb(void *arg)
{
    (void)arg;
    s_dhcp_fails++;
    ESP_LOGW(TAG, "DHCP timed out (%u in a row), reconnecting", (unsigned)s_dhcp_fails);

    if (s_cand_idx < s_cand_count) {
        const wifi_bssid_cand_t *cand = &s_candidates[s_cand_idx++];
        memcpy(s_sta_cfg.sta.bssid, cand->bssid, sizeof(s_sta_cfg.sta.bssid));
        s_sta_cfg.sta.bssid_set = true;
        s_sta_cfg.sta.channel = cand->channel;
        esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg);
        ESP_LOGW(TAG, "trying alternate AP %02x:%02x:%02x:%02x:%02x:%02x (ch %u)",
                 cand->bssid[0], cand->bssid[1], cand->bssid[2],
                 cand->bssid[3], cand->bssid[4], cand->bssid[5],
                 (unsigned)cand->channel);
    } else if (s_dhcp_fails >= 2 && !s_bssid_scan_running &&
               (s_last_scan_us == 0 ||
                esp_timer_get_time() - s_last_scan_us > 60 * 1000000)) {
        s_ap_search = true;
        s_bssid_scan_running = true;
        s_last_scan_us = esp_timer_get_time();
        if (xTaskCreate(bssid_scan_task, "bssid_scan", 4096, NULL, 4, NULL) != pdPASS) {
            s_bssid_scan_running = false;
            ESP_LOGW(TAG, "cannot start AP scan task");
        }
    }
    esp_wifi_disconnect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_ap_record_t ap;
        memset(&ap, 0, sizeof(ap));
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(s_last_bssid, ap.bssid, sizeof(s_last_bssid));
            s_last_bssid_valid = true;
        } else if (s_sta_cfg.sta.bssid_set) {
            memcpy(s_last_bssid, s_sta_cfg.sta.bssid, sizeof(s_last_bssid));
            s_last_bssid_valid = true;
        }
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
        if (s_disconnects >= 3 && app_settings_get()->wifi_bssid_set && !s_ap_search) {
            /* The pinned radio is not working (moved AP, multi-radio SSID,
             * DHCP broken): drop the pin and let the driver pick again.
             * While the AP-search is running the device manages the BSSID
             * itself, do not fight it. */
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
        s_dhcp_fails = 0;
        s_cand_count = 0;
        s_cand_idx = 0;
        s_ap_search = false;
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

/* ---- Bot UI: inline keyboards and screens ---------------------------- */

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

static void esc_html(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *p = src; *p != 0 && o + 6 < cap; p++) {
        if (*p == '<') {
            memcpy(dst + o, "&lt;", 4);
            o += 4;
        } else if (*p == '>') {
            memcpy(dst + o, "&gt;", 4);
            o += 4;
        } else if (*p == '&') {
            memcpy(dst + o, "&amp;", 5);
            o += 5;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = 0;
}

static void fool_str(uint32_t f, char *out, size_t cap)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } T[] = {
        { ESP_DESYNC_FOOL_TTL, "ttl" },
        { ESP_DESYNC_FOOL_MD5SIG, "md5sig" },
        { ESP_DESYNC_FOOL_BADSUM, "badsum" },
        { ESP_DESYNC_FOOL_BADSEQ, "badseq" },
        { ESP_DESYNC_FOOL_DATANOACK, "datanoack" },
        { ESP_DESYNC_FOOL_TS, "ts" },
    };
    out[0] = 0;
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        if (f & T[i].bit) {
            if (out[0]) {
                strlcat(out, "|", cap);
            }
            strlcat(out, T[i].name, cap);
        }
    }
    if (out[0] == 0) {
        strlcpy(out, "none", cap);
    }
}

#define KB_MAIN "{\"inline_keyboard\":[[" \
    "{\"text\":\"📊 Статус\",\"callback_data\":\"st\"}," \
    "{\"text\":\"🔁 Скан\",\"callback_data\":\"sc\"}],[" \
    "{\"text\":\"🧭 Стратегия\",\"callback_data\":\"strat\"}," \
    "{\"text\":\"🌐 Сеть\",\"callback_data\":\"net\"}],[" \
    "{\"text\":\"🧠 Память\",\"callback_data\":\"heap\"}," \
    "{\"text\":\"💤 Wake\",\"callback_data\":\"wr\"}],[" \
    "{\"text\":\"⚙️ Настройки\",\"callback_data\":\"set\"}," \
    "{\"text\":\"🔌 Перезагрузка\",\"callback_data\":\"rb\"}]]}"

#define KB_BACK "{\"inline_keyboard\":[[{\"text\":\"🔙 Меню\",\"callback_data\":\"m\"}]]}"

#define KB_STATUS "{\"inline_keyboard\":[[" \
    "{\"text\":\"🔄 Обновить\",\"callback_data\":\"st\"}," \
    "{\"text\":\"⚙️ Настройки\",\"callback_data\":\"set\"}],[" \
    "{\"text\":\"🔙 Меню\",\"callback_data\":\"m\"}]]}"

#define KB_STRAT "{\"inline_keyboard\":[[" \
    "{\"text\":\"🔁 Скан\",\"callback_data\":\"sc\"}," \
    "{\"text\":\"🔄 Обновить\",\"callback_data\":\"strat\"}],[" \
    "{\"text\":\"🔙 Меню\",\"callback_data\":\"m\"}]]}"

#define KB_SET "{\"inline_keyboard\":[[" \
    "{\"text\":\"off\",\"callback_data\":\"ds:off\"}," \
    "{\"text\":\"split\",\"callback_data\":\"ds:split\"}," \
    "{\"text\":\"disorder\",\"callback_data\":\"ds:disorder\"}],[" \
    "{\"text\":\"fake\",\"callback_data\":\"ds:fake\"}," \
    "{\"text\":\"fake_split\",\"callback_data\":\"ds:fake_split\"}," \
    "{\"text\":\"tlsrec\",\"callback_data\":\"ds:tlsrec\"}," \
    "{\"text\":\"seqovl\",\"callback_data\":\"ds:seqovl\"}],[" \
    "{\"text\":\"TTL 3\",\"callback_data\":\"ttl:3\"}," \
    "{\"text\":\"TTL 5\",\"callback_data\":\"ttl:5\"}," \
    "{\"text\":\"TTL 8\",\"callback_data\":\"ttl:8\"}," \
    "{\"text\":\"TTL 12\",\"callback_data\":\"ttl:12\"}],[" \
    "{\"text\":\"TTL -1\",\"callback_data\":\"ttl:-1\"}," \
    "{\"text\":\"TTL +1\",\"callback_data\":\"ttl:+1\"}," \
    "{\"text\":\"🎲 rndsni\",\"callback_data\":\"rnd:t\"}," \
    "{\"text\":\"📊 статистика\",\"callback_data\":\"stat:t\"}],[" \
    "{\"text\":\"🤡 Фулинг\",\"callback_data\":\"fool\"}," \
    "{\"text\":\"🔙 Меню\",\"callback_data\":\"m\"}]]}"

#define KB_FOOL "{\"inline_keyboard\":[[" \
    "{\"text\":\"ttl\",\"callback_data\":\"fl:ttl\"}," \
    "{\"text\":\"md5sig\",\"callback_data\":\"fl:md5sig\"}," \
    "{\"text\":\"badsum\",\"callback_data\":\"fl:badsum\"}],[" \
    "{\"text\":\"badseq\",\"callback_data\":\"fl:badseq\"}," \
    "{\"text\":\"datanoack\",\"callback_data\":\"fl:datanoack\"}," \
    "{\"text\":\"ts\",\"callback_data\":\"fl:ts\"}," \
    "{\"text\":\"none\",\"callback_data\":\"fl:none\"}],[" \
    "{\"text\":\"🔙 Настройки\",\"callback_data\":\"set\"}]]}"

#define KB_REBOOT "{\"inline_keyboard\":[[" \
    "{\"text\":\"✅ Перезагрузить\",\"callback_data\":\"rb:y\"}," \
    "{\"text\":\"❌ Отмена\",\"callback_data\":\"m\"}]]}"

static void bot_screen(const tg_update_t *u, const char *text, const char *kb)
{
    if (u->is_callback && u->message_id != 0) {
        if (tg_edit_menu(u->chat_id, u->message_id, text, kb) == 0) {
            return;
        }
    }
    tg_send_menu(u->chat_id, text, kb);
}

static void scr_menu(const tg_update_t *u)
{
    esp_desync_config_t c;
    scan_status_t st;
    esp_desync_get_config(&c);
    scan_get_status(&st);
    char strategy[84];
    esc_html(st.have ? st.strategy : "—", strategy, sizeof(strategy));
    char text[512];
    snprintf(text, sizeof(text),
             "🛰 <b>esp32-zapret</b> — обход блокировок Telegram\n"
             "\n"
             "🎛 Режим: <code>%s</code> · ⏳ TTL <code>%u</code>\n"
             "🧭 Стратегия: <code>%s</code> (%s)\n"
             "\n"
             "Всё управление — кнопками ниже.\n"
             "Команды тоже работают: /status, /scan, /help.",
             esp_desync_mode_name(c.mode), (unsigned)c.fake_ttl,
             strategy, st.manual ? "ручная" : "авто");
    bot_screen(u, text, KB_MAIN);
}

static void scr_status(const tg_update_t *u)
{
    wifi_ap_record_t ap;
    esp_desync_config_t c;
    scan_status_t st;
    memset(&ap, 0, sizeof(ap));
    esp_wifi_sta_get_ap_info(&ap);
    esp_desync_get_config(&c);
    scan_get_status(&st);

    char ips[16] = "—", gws[16] = "—";
    esp_netif_ip_info_t ipi;
    memset(&ipi, 0, sizeof(ipi));
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif != NULL && esp_netif_get_ip_info(nif, &ipi) == ESP_OK) {
        snprintf(ips, sizeof(ips), IPSTR, IP2STR(&ipi.ip));
        snprintf(gws, sizeof(gws), IPSTR, IP2STR(&ipi.gw));
    }
    char ssid[80];
    esc_html(ap.ssid[0] ? (const char *)ap.ssid : "—", ssid, sizeof(ssid));
    char strategy[84], fool[48];
    esc_html(st.have ? st.strategy : "—", strategy, sizeof(strategy));
    fool_str(c.fooling, fool, sizeof(fool));

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    char text[768];
    snprintf(text, sizeof(text),
             "📊 <b>Состояние</b>\n"
             "\n"
             "🌐 <code>%s</code> · шлюз <code>%s</code>\n"
             "📶 %s · %d dBm\n"
             "🕓 %uч %uм · 🧠 heap %u КБ\n"
             "🎛 <code>%s</code> · TTL <code>%u</code> · фулинг <code>%s</code> · rndsni %s\n"
             "🧭 <code>%s</code> (%s) · переборов %u, проб %u, сбоев %u\n"
             "📡 telegram <code>%s</code> · HTTP %d",
             ips, gws, ssid, ap.rssi,
             (unsigned)(up / 3600), (unsigned)((up / 60) % 60),
             (unsigned)(esp_get_free_heap_size() / 1024),
             esp_desync_mode_name(c.mode), (unsigned)c.fake_ttl, fool,
             c.rndsni ? "вкл" : "выкл",
             strategy, st.manual ? "ручная" : "авто",
             (unsigned)st.scans, (unsigned)st.probes, (unsigned)st.probe_fails,
             tg_last_endpoint(), tg_last_http_status());
    bot_screen(u, text, KB_STATUS);
}

static void scr_strategy(const tg_update_t *u)
{
    scan_status_t st;
    scan_get_status(&st);
    char strategy[84];
    esc_html(st.have ? st.strategy : "не найдена", strategy, sizeof(strategy));
    char text[400];
    snprintf(text, sizeof(text),
             "🧭 <b>Стратегия обхода</b>\n"
             "\n"
             "Режим: <code>%s</code> (%s)\n"
             "Переборов: %u · проб: %u · сбоев: %u",
             strategy, st.manual ? "ручная" : "авто",
             (unsigned)st.scans, (unsigned)st.probes, (unsigned)st.probe_fails);
    bot_screen(u, text, KB_STRAT);
}

static void scr_net(const tg_update_t *u)
{
    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    esp_wifi_sta_get_ap_info(&ap);
    char ips[16] = "—", gws[16] = "—";
    esp_netif_ip_info_t ipi;
    memset(&ipi, 0, sizeof(ipi));
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif != NULL && esp_netif_get_ip_info(nif, &ipi) == ESP_OK) {
        snprintf(ips, sizeof(ips), IPSTR, IP2STR(&ipi.ip));
        snprintf(gws, sizeof(gws), IPSTR, IP2STR(&ipi.gw));
    }
    char ssid[80];
    esc_html(ap.ssid[0] ? (const char *)ap.ssid : "—", ssid, sizeof(ssid));
    uint8_t ch = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&ch, &second);
    char text[320];
    snprintf(text, sizeof(text),
             "🌐 <b>Сеть</b>\n"
             "\n"
             "IP <code>%s</code>\n"
             "шлюз <code>%s</code>\n"
             "Wi-Fi %s · %d dBm · канал %u",
             ips, gws, ssid, ap.rssi, (unsigned)ch);
    bot_screen(u, text, KB_BACK);
}

static void scr_heap(const tg_update_t *u)
{
    char text[256];
    snprintf(text, sizeof(text),
             "🧠 <b>Память</b>\n"
             "\n"
             "свободно <code>%u</code> Б\n"
             "минимум <code>%u</code> Б\n"
             "крупнейший блок <code>%u</code> Б",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    bot_screen(u, text, KB_BACK);
}

static void scr_settings(const tg_update_t *u)
{
    esp_desync_config_t c;
    esp_desync_get_config(&c);
    char fool[48];
    fool_str(c.fooling, fool, sizeof(fool));
    char text[512];
    snprintf(text, sizeof(text),
             "⚙️ <b>Настройки обхода</b>\n"
             "\n"
             "🎛 Режим <code>%s</code> · ⏳ TTL <code>%u</code>\n"
             "🤡 Фулинг <code>%s</code>\n"
             "🎲 rndsni %s · 📊 статистика %s\n"
             "\n"
             "Тапни, чтобы изменить:",
             esp_desync_mode_name(c.mode), (unsigned)c.fake_ttl, fool,
             c.rndsni ? "вкл" : "выкл",
             stats_anon_enabled() ? "вкл" : "выкл");
    bot_screen(u, text, KB_SET);
}

static void scr_fool(const tg_update_t *u)
{
    esp_desync_config_t c;
    esp_desync_get_config(&c);
    char fool[48];
    fool_str(c.fooling, fool, sizeof(fool));
    char text[320];
    snprintf(text, sizeof(text),
             "🤡 <b>Фулинг фейка</b>\n"
             "\n"
             "Сейчас: <code>%s</code>\n"
             "\n"
             "TTL — самый универсальный; md5sig/badsum/badseq/ts — когда TTL не "
             "подходит; none — без защиты от «долёта» фейка.",
             fool);
    bot_screen(u, text, KB_FOOL);
}

static void do_scan(const tg_update_t *u)
{
    bot_screen(u, "⏳ <b>Перебираю стратегии…</b>\nОбычно 10–60 секунд.", KB_STATUS);
    int rc = scan_find_working();
    scan_status_t st;
    scan_get_status(&st);
    char strategy[84];
    esc_html(st.have ? st.strategy : "—", strategy, sizeof(strategy));
    char text[384];
    snprintf(text, sizeof(text),
             rc == 0 ? "✅ <b>Готово</b>\nСтратегия: <code>%s</code>"
                     : "⚠️ <b>Рабочая стратегия не найдена</b>\nОстаюсь на <code>%s</code>",
             strategy);
    bot_screen(u, text, KB_STATUS);
}

#define KB_WAKE "{\"inline_keyboard\":[[" \
    "{\"text\":\"📤 Отправить\",\"callback_data\":\"wk\"}," \
    "{\"text\":\"🔍 Найти в сети\",\"callback_data\":\"ws\"}],[" \
    "{\"text\":\"🔙 Меню\",\"callback_data\":\"m\"}]]}"

static net_host_t s_arp_hosts[NET_SCAN_MAX_HOSTS];
static int s_arp_count;
static char s_hosts_kb[1200];

static void mac_to_str(const uint8_t *mac, char *buf, size_t cap)
{
    snprintf(buf, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void scr_wake(const tg_update_t *u)
{
    const app_settings_t *s = app_settings_get();
    char mac[32], bcast[32];
    esc_html(s->wol_mac[0] ? s->wol_mac : "не задан", mac, sizeof(mac));
    esc_html(s->wol_broadcast[0] ? s->wol_broadcast : "255.255.255.255", bcast, sizeof(bcast));
    char text[560];
    snprintf(text, sizeof(text),
             "💤 <b>Wake-on-LAN</b>\n"
             "\n"
             "Цель: <code>%s</code>\n"
             "Broadcast: <code>%s</code> · порт %u\n"
             "\n"
             "• «Найти в сети» — ARP-скан включённых устройств: выбери своё, "
             "MAC запомнится.\n"
             "• Или пришли <code>/wake AA:BB:CC:DD:EE:FF</code> — MAC тоже "
             "запомнится навсегда.",
             mac, bcast, (unsigned)(s->wol_port ? s->wol_port : 9));
    bot_screen(u, text, KB_WAKE);
}

static void do_pick_host(const tg_update_t *u, int idx)
{
    if (idx < 0 || idx >= s_arp_count) {
        tg_answer_callback(u->callback_id, "Список устарел, запусти поиск заново");
        scr_wake(u);
        return;
    }
    char mac[18];
    mac_to_str(s_arp_hosts[idx].mac, mac, sizeof(mac));
    const app_settings_t *s = app_settings_get();
    bool saved = app_settings_set_wol(mac, s->wol_broadcast, s->wol_port) == ESP_OK;
    bool sent = wol_send(mac, s->wol_broadcast, s->wol_port) == 0;
    char text[448];
    snprintf(text, sizeof(text),
             "💤 <b>Wake-on-LAN</b>\n"
             "\n"
             "MAC: <code>%s</code>%s\n"
             "\n"
             "%s",
             mac,
             saved ? "\n💾 сохранён для следующих запусков" : "",
             sent ? "✅ Пакет отправлен." : "⚠️ Не удалось отправить пакет.");
    bot_screen(u, text, KB_WAKE);
}

static void do_arp_scan(const tg_update_t *u)
{
    bot_screen(u, "⏳ <b>Ищу устройства в сети…</b>\nПара секунд.", KB_WAKE);
    s_arp_count = net_scan_arp(s_arp_hosts, NET_SCAN_MAX_HOSTS);
    if (s_arp_count == 0) {
        bot_screen(u,
                   "🔍 <b>Никого не нашёл</b>\n"
                   "\n"
                   "Спящий ПК не отвечает на ARP — включи его или пришли "
                   "<code>/wake AA:BB:CC:DD:EE:FF</code>, чтобы задать MAC вручную.",
                   KB_WAKE);
        return;
    }

    char text[768];
    size_t off = snprintf(text, sizeof(text), "🔍 <b>Найдено устройств: %d</b>\n\n", s_arp_count);
    size_t k = 0;
    k += snprintf(s_hosts_kb + k, sizeof(s_hosts_kb) - k, "{\"inline_keyboard\":[");
    for (int i = 0; i < s_arp_count && off + 80 < sizeof(text) && k + 80 < sizeof(s_hosts_kb); i++) {
        char mstr[18], ips[16];
        mac_to_str(s_arp_hosts[i].mac, mstr, sizeof(mstr));
        net_format_ip(s_arp_hosts[i].ip_be, ips, sizeof(ips));
        off += snprintf(text + off, sizeof(text) - off, "• <code>%s</code> · %s\n", mstr, ips);
        k += snprintf(s_hosts_kb + k, sizeof(s_hosts_kb) - k,
                      "[{\"text\":\"%s · %s\",\"callback_data\":\"wm:%d\"}],",
                      mstr, ips, i);
    }
    snprintf(text + off, sizeof(text) - off,
             "\nВыбери устройство — MAC запомнится и сразу уйдёт WoL.");
    snprintf(s_hosts_kb + k, sizeof(s_hosts_kb) - k,
             "[{\"text\":\"🔙 Меню\",\"callback_data\":\"m\"}]]}");
    bot_screen(u, text, s_hosts_kb);
}

static void do_wake(const tg_update_t *u, const char *mac_arg)
{
    const app_settings_t *settings = app_settings_get();
    char macbuf[24];
    strlcpy(macbuf, settings->wol_mac, sizeof(macbuf));
    if (mac_arg != NULL && mac_arg[0]) {
        copy_token(mac_arg, macbuf, sizeof(macbuf));
    }
    if (macbuf[0] == 0) {
        scr_wake(u);
        return;
    }

    bool saved = false;
    if (mac_arg != NULL && mac_arg[0] &&
        app_settings_set_wol(macbuf, settings->wol_broadcast, settings->wol_port) == ESP_OK) {
        saved = true;
    }

    char mac_esc[32];
    esc_html(macbuf, mac_esc, sizeof(mac_esc));
    bool sent = wol_send(macbuf, settings->wol_broadcast, settings->wol_port) == 0;
    char text[448];
    snprintf(text, sizeof(text),
             "💤 <b>Wake-on-LAN</b>\n"
             "\n"
             "MAC: <code>%s</code>%s\n"
             "\n"
             "%s",
             mac_esc,
             saved ? "\n💾 сохранён для следующих запусков" : "",
             sent ? "✅ Пакет отправлен." : "⚠️ Не удалось отправить: проверь MAC.");
    bot_screen(u, text, KB_WAKE);
}

static void handle_callback(const tg_update_t *u)
{
    const char *d = u->callback_data;
    tg_answer_callback(u->callback_id, NULL);

    if (strcmp(d, "m") == 0) {
        scr_menu(u);
    } else if (strcmp(d, "st") == 0) {
        scr_status(u);
    } else if (strcmp(d, "strat") == 0) {
        scr_strategy(u);
    } else if (strcmp(d, "net") == 0) {
        scr_net(u);
    } else if (strcmp(d, "heap") == 0) {
        scr_heap(u);
    } else if (strcmp(d, "wr") == 0 || strcmp(d, "wake") == 0) {
        scr_wake(u);
    } else if (strcmp(d, "wk") == 0) {
        do_wake(u, NULL);
    } else if (strcmp(d, "ws") == 0) {
        do_arp_scan(u);
    } else if (strncmp(d, "wm:", 3) == 0) {
        do_pick_host(u, atoi(d + 3));
    } else if (strcmp(d, "sc") == 0) {
        do_scan(u);
    } else if (strcmp(d, "set") == 0) {
        scr_settings(u);
    } else if (strcmp(d, "fool") == 0) {
        scr_fool(u);
    } else if (strcmp(d, "rb") == 0) {
        bot_screen(u, "🔌 <b>Перезагрузить устройство?</b>", KB_REBOOT);
    } else if (strcmp(d, "rb:y") == 0) {
        tg_send_message(u->chat_id, "🔌 Перезагружаюсь…");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (strncmp(d, "ds:", 3) == 0) {
        bool ok = false;
        esp_desync_mode_t m = esp_desync_mode_from_name(d + 3, &ok);
        if (ok) {
            esp_desync_config_t c;
            esp_desync_get_config(&c);
            c.mode = m;
            esp_desync_set_config(&c);
            scan_set_manual(true);
        }
        scr_settings(u);
    } else if (strncmp(d, "ttl:", 4) == 0) {
        esp_desync_config_t c;
        esp_desync_get_config(&c);
        int v = c.fake_ttl;
        if (d[4] == '+') {
            v++;
        } else if (d[4] == '-') {
            v--;
        } else {
            v = atoi(d + 4);
        }
        if (v < 1) {
            v = 1;
        }
        if (v > 255) {
            v = 255;
        }
        c.fake_ttl = (uint8_t)v;
        esp_desync_set_config(&c);
        scan_set_manual(true);
        scr_settings(u);
    } else if (strncmp(d, "fl:", 3) == 0) {
        const char *name = d + 3;
        uint32_t f = ESP_DESYNC_FOOL_NONE;
        if (strcmp(name, "ttl") == 0) f = ESP_DESYNC_FOOL_TTL;
        else if (strcmp(name, "md5sig") == 0) f = ESP_DESYNC_FOOL_MD5SIG;
        else if (strcmp(name, "badsum") == 0) f = ESP_DESYNC_FOOL_BADSUM;
        else if (strcmp(name, "badseq") == 0) f = ESP_DESYNC_FOOL_BADSEQ;
        else if (strcmp(name, "datanoack") == 0) f = ESP_DESYNC_FOOL_DATANOACK;
        else if (strcmp(name, "ts") == 0) f = ESP_DESYNC_FOOL_TS;
        esp_desync_config_t c;
        esp_desync_get_config(&c);
        c.fooling = f;
        esp_desync_set_config(&c);
        scan_set_manual(true);
        scr_fool(u);
    } else if (strcmp(d, "rnd:t") == 0) {
        esp_desync_config_t c;
        esp_desync_get_config(&c);
        c.rndsni = !c.rndsni;
        esp_desync_set_config(&c);
        scan_set_manual(true);
        scr_settings(u);
    } else if (strcmp(d, "stat:t") == 0) {
        stats_anon_set_enabled(!stats_anon_enabled());
        scr_settings(u);
    }
}

static void handle_update(const tg_update_t *u)
{
    const app_settings_t *settings = app_settings_get();

    if (settings->tg_admin_id != 0 && u->chat_id != settings->tg_admin_id) {
        tg_send_message(u->chat_id, "🚫 Доступ запрещён");
        return;
    }
    if (!rate_limit_ok()) {
        tg_send_message(u->chat_id, "⏳ Слишком много команд, подождите минуту");
        return;
    }
    if (u->is_callback) {
        handle_callback(u);
        return;
    }

    char reply[512];
    const char *text = u->text;
    const char *args;

    if (cmd_args(text, "start") != NULL || cmd_args(text, "help") != NULL ||
        cmd_args(text, "menu") != NULL) {
        scr_menu(u);
    } else if (cmd_args(text, "status") != NULL) {
        scr_status(u);
    } else if (cmd_args(text, "strategy") != NULL) {
        scr_strategy(u);
    } else if (cmd_args(text, "settings") != NULL) {
        scr_settings(u);
    } else if (cmd_args(text, "heap") != NULL) {
        scr_heap(u);
    } else if (cmd_args(text, "ip") != NULL) {
        scr_net(u);
    } else if (cmd_args(text, "scan") != NULL) {
        do_scan(u);
    } else if ((args = cmd_args(text, "wake")) != NULL ||
               (args = cmd_args(text, "wol")) != NULL) {
        if (args[0]) {
            do_wake(u, args);
        } else {
            scr_wake(u);
        }
    } else if (cmd_args(text, "reboot") != NULL) {
        bot_screen(u, "🔌 <b>Перезагрузить устройство?</b>", KB_REBOOT);
    } else if ((args = cmd_args(text, "desync")) != NULL) {
        char name[24] = {0};
        if (args[0]) {
            copy_token(args, name, sizeof(name));
        }
        if (name[0] == 0) {
            scr_settings(u);
            return;
        }
        if (strcmp(name, "none") == 0) {
            strlcpy(name, "off", sizeof(name));
        }
        bool ok = false;
        esp_desync_mode_t m = esp_desync_mode_from_name(name, &ok);
        if (ok) {
            esp_desync_config_t c;
            esp_desync_get_config(&c);
            c.mode = m;
            esp_desync_set_config(&c);
            scan_set_manual(true);
            snprintf(reply, sizeof(reply), "🎛 Режим: <code>%s</code>", esp_desync_mode_name(m));
        } else {
            strlcpy(reply, "⚠️ Неизвестный режим. Открой /settings.", sizeof(reply));
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
            snprintf(reply, sizeof(reply), "⏳ TTL = <code>%d</code>", v);
        } else {
            snprintf(reply, sizeof(reply), "⚠️ Использование: /ttl 1..255 (обычно 3..8)");
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
            char fool[48];
            fool_str(f, fool, sizeof(fool));
            snprintf(reply, sizeof(reply), "🤡 Фулинг: <code>%s</code>", fool);
        } else {
            snprintf(reply, sizeof(reply),
                     "⚠️ Использование: /fool ttl|md5sig|badsum|badseq|datanoack|ts|none");
        }
        tg_send_message(u->chat_id, reply);
    } else if ((args = cmd_args(text, "rndsni")) != NULL) {
        char name[8] = {0};
        if (args[0]) {
            copy_token(args, name, sizeof(name));
        }
        esp_desync_config_t c;
        esp_desync_get_config(&c);
        if (name[0] == 0) {
            snprintf(reply, sizeof(reply), "🎲 rndsni: %s", c.rndsni ? "вкл" : "выкл");
        } else if (strcmp(name, "on") == 0 || strcmp(name, "off") == 0) {
            c.rndsni = strcmp(name, "on") == 0;
            esp_desync_set_config(&c);
            scan_set_manual(true);
            snprintf(reply, sizeof(reply), "🎲 rndsni: %s", c.rndsni ? "вкл" : "выкл");
        } else {
            snprintf(reply, sizeof(reply), "⚠️ Использование: /rndsni on|off");
        }
        tg_send_message(u->chat_id, reply);
    } else if ((args = cmd_args(text, "stats")) != NULL) {
        char arg[8] = {0};
        if (args[0]) {
            copy_token(args, arg, sizeof(arg));
        }
        if (arg[0] == 0) {
            snprintf(reply, sizeof(reply), "📊 Анонимная статистика: %s",
                     stats_anon_enabled() ? "вкл" : "выкл");
        } else if (strcmp(arg, "on") == 0) {
            stats_anon_set_enabled(true);
            stats_anon_report("manual");
            snprintf(reply, sizeof(reply), "📊 Анонимная статистика: вкл");
        } else if (strcmp(arg, "off") == 0) {
            stats_anon_set_enabled(false);
            snprintf(reply, sizeof(reply), "📊 Анонимная статистика: выкл");
        } else {
            snprintf(reply, sizeof(reply), "⚠️ Использование: /stats on|off");
        }
        tg_send_message(u->chat_id, reply);
    } else {
        scr_menu(u);
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

