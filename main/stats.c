// SPDX-License-Identifier: MIT
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "esp_desync.h"
#include "desync_scan.h"
#include "https_client.h"
#include "doh.h"
#include "fw_version.h"
#include "stats_payload.h"
#include "stats.h"

static const char *TAG = "stats";

#define NVS_NAMESPACE   "stats"
#define NVS_KEY_ENABLED "enabled"
#define NVS_KEY_LAST    "last"
#define STATS_PATH      "/api/v1/report"
#define BOOT_MIN_GAP_S  3600
#define SEND_TIMEOUT_MS 8000

static bool s_enabled;
static int64_t s_last; /* unix seconds of the last successful report, 0 = never */

static void last_save(int64_t ts)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        s_last = ts;
        return;
    }
    if (nvs_set_i64(h, NVS_KEY_LAST, ts) == ESP_OK) {
        nvs_commit(h);
        s_last = ts;
    }
    nvs_close(h);
}

static void last_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int64_t ts = 0;
    if (nvs_get_i64(h, NVS_KEY_LAST, &ts) == ESP_OK) {
        s_last = ts;
    }
    nvs_close(h);
}

void stats_anon_init(void)
{
    nvs_handle_t h;
    bool have_pref = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_ENABLED, &v) == ESP_OK) {
            s_enabled = v != 0;
            have_pref = true;
        }
        nvs_close(h);
    }
    if (!have_pref) {
#if CONFIG_APP_STATS_DEFAULT_ON
        s_enabled = true;
#endif
    }
    last_load();
    if (s_enabled) {
        ESP_LOGI(TAG, "anonymous statistics: on");
    }
}

bool stats_anon_enabled(void)
{
    return s_enabled;
}

void stats_anon_set_enabled(bool on)
{
    s_enabled = on;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(h, NVS_KEY_ENABLED, on ? 1 : 0) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static void collect(stats_payload_t *p, const char *event)
{
    memset(p, 0, sizeof(*p));
    p->event = event;
    p->fw = FW_VERSION;
    p->target = CONFIG_IDF_TARGET;
    p->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    p->free_heap = esp_get_free_heap_size();
    p->reset_reason = (uint8_t)esp_reset_reason();

    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        p->rssi = ap.rssi;
    }

    esp_desync_config_t c;
    esp_desync_get_config(&c);
    p->mode = (uint8_t)c.mode;
    p->fooling = c.fooling;
    p->ttl = c.fake_ttl;
    p->split1 = c.split_pos;
    p->split2 = c.split_pos2;

    scan_status_t st;
    scan_get_status(&st);
    p->have_strategy = st.have;
    p->strategy = st.have ? st.strategy : "";
    p->scans = st.scans;
    p->probes = st.probes;
    p->probe_fails = st.probe_fails;
}

static bool connect_stats(https_conn_t *conn)
{
    if (https_connect(conn, CONFIG_APP_STATS_HOST, NULL, 0, 443, 2500, 6000, true) == 0) {
        return true;
    }
#if CONFIG_APP_DOH_FALLBACK
    uint32_t ips[4];
    int n = doh_resolve(CONFIG_APP_STATS_HOST, ips, 4);
    if (n > 0 &&
        https_connect(conn, CONFIG_APP_STATS_HOST, ips, n, 443, 2500, 6000, false) == 0) {
        return true;
    }
#endif
    return false;
}

static void send_report(const char *event)
{
    stats_payload_t p;
    collect(&p, event);

    char body[512];
    if (stats_build_payload(&p, body, sizeof(body)) < 0) {
        return;
    }

    https_conn_t conn;
    if (!connect_stats(&conn)) {
        ESP_LOGD(TAG, "report not sent: endpoint unavailable");
        return;
    }

    int status = 0;
    int r = https_post_json(&conn, STATS_PATH, body, SEND_TIMEOUT_MS, &status);
    https_close(&conn);
    if (r > 0 && status >= 200 && status < 300) {
        last_save((int64_t)time(NULL));
        ESP_LOGD(TAG, "report sent (%s)", event);
    } else {
        ESP_LOGD(TAG, "report not sent: HTTP %d", status);
    }
}

void stats_anon_report(const char *event)
{
    if (!s_enabled || event == NULL) {
        return;
    }

    int64_t now = (int64_t)time(NULL);
    if (now > 0 && s_last > 0) {
        if (strcmp(event, "periodic") == 0 && now - s_last < CONFIG_APP_STATS_INTERVAL_S) {
            return;
        }
        if (strcmp(event, "boot") == 0 && now - s_last < BOOT_MIN_GAP_S) {
            return;
        }
    }
    send_report(event);
}

void stats_anon_tick(void)
{
    stats_anon_report("periodic");
}

