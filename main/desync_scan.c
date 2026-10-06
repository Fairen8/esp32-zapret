// SPDX-License-Identifier: MIT
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_desync.h"
#include "telegram.h"
#include "scan_candidates.h"
#include "desync_scan.h"

static const char *TAG = "scan";

#define NVS_NAMESPACE    "desync"
#define NVS_KEY          "strategy"
#define PROBE_TIMEOUT_MS 8000
#define PROBE_SETTLE_MS  300

static scan_candidate_t s_current;
static bool s_have;
static int s_fail_streak;
static uint32_t s_scans;
static uint32_t s_probes;
static uint32_t s_fails;
static int64_t s_last_ok_ms;

static void apply_candidate(const scan_candidate_t *c)
{
    esp_desync_config_t cfg;
    esp_desync_get_config(&cfg);
    cfg.mode = c->mode;
    cfg.fooling = c->fooling;
    cfg.fake_ttl = c->ttl ? c->ttl : 64;
    cfg.fake_sni = (c->sni[0] != 0) ? c->sni : NULL;
    esp_desync_set_config(&cfg);
}

static bool load_saved(scan_candidate_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sz = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, out, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(*out) || out->mode > ESP_DESYNC_MODE_TLSREC) {
        return false;
    }
    out->sni[SCAN_SNI_MAX - 1] = 0;
    return true;
}

static void save_current(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, NVS_KEY, &s_current, sizeof(s_current)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static int probe_with(const scan_candidate_t *c)
{
    apply_candidate(c);
    vTaskDelay(pdMS_TO_TICKS(PROBE_SETTLE_MS));
    s_probes++;
    if (tg_probe(PROBE_TIMEOUT_MS) == 0) {
        s_last_ok_ms = esp_timer_get_time() / 1000;
        s_fail_streak = 0;
        return 0;
    }
    s_fails++;
    return -1;
}

void scan_init(void)
{
    if (!load_saved(&s_current)) {
        ESP_LOGI(TAG, "no saved strategy");
        return;
    }
    s_have = true;
    apply_candidate(&s_current);
    char buf[80];
    scan_format(&s_current, buf, sizeof(buf));
    ESP_LOGI(TAG, "saved strategy: %s", buf);
}

int scan_find_working(void)
{
    s_scans++;

    if (s_have) {
        char buf[80];
        scan_format(&s_current, buf, sizeof(buf));
        ESP_LOGI(TAG, "probing saved strategy: %s", buf);
        if (probe_with(&s_current) == 0) {
            return 0;
        }
    }

    scan_candidate_t cands[SCAN_MAX_CANDIDATES];
    int n = scan_build_candidates(cands, SCAN_MAX_CANDIDATES);
    for (int i = 0; i < n; i++) {
        char buf[80];
        scan_format(&cands[i], buf, sizeof(buf));
        ESP_LOGI(TAG, "scan %d/%d: %s", i + 1, n, buf);
        if (probe_with(&cands[i]) == 0) {
            s_current = cands[i];
            s_have = true;
            save_current();
            ESP_LOGI(TAG, "selected strategy: %s", buf);
            return 0;
        }
    }

    ESP_LOGE(TAG, "no working strategy among %d candidates", n);
    s_have = false;
    return -1;
}

int scan_health_check(void)
{
    if (!s_have) {
        return scan_find_working();
    }
    if (probe_with(&s_current) == 0) {
        return 0;
    }
    s_fail_streak++;
    ESP_LOGW(TAG, "health probe failed (%d in a row)", s_fail_streak);
    if (s_fail_streak >= CONFIG_APP_HEALTH_FAIL_THRESHOLD) {
        s_fail_streak = 0;
        return scan_find_working();
    }
    return -1;
}

void scan_get_status(scan_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->have = s_have;
    out->scans = s_scans;
    out->probes = s_probes;
    out->probe_fails = s_fails;
    out->last_ok_ms = s_last_ok_ms;
    if (s_have) {
        scan_format(&s_current, out->strategy, sizeof(out->strategy));
    } else {
        strlcpy(out->strategy, "(not detected)", sizeof(out->strategy));
    }
}
