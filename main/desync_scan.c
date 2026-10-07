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
#define NVS_KEY_MANUAL   "manual"
#define PROBE_TIMEOUT_MS 8000
#define PROBE_SETTLE_MS  300

static scan_candidate_t s_current;
static bool s_have;
static bool s_manual;
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
    cfg.rndsni = c->rndsni;
    esp_desync_set_config(&cfg);
}

static bool load_blob(const char *key, scan_candidate_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sz = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, key, out, &sz);
    nvs_close(h);
    return err == ESP_OK && sz == sizeof(*out);
}

/* Rejects or repairs fields that may contain garbage (old firmware wrote
 * this blob without validation). Returns false only for a bad mode. */
static bool candidate_valid(scan_candidate_t *c)
{
    const uint32_t fool_mask = ESP_DESYNC_FOOL_TTL | ESP_DESYNC_FOOL_BADSUM |
                               ESP_DESYNC_FOOL_BADSEQ | ESP_DESYNC_FOOL_MD5SIG |
                               ESP_DESYNC_FOOL_DATANOACK;
    if (c->mode > ESP_DESYNC_MODE_TLSREC) {
        return false;
    }
    c->fooling &= fool_mask;
    if (c->ttl == 0) {
        c->ttl = 64;
    }
    c->sni[SCAN_SNI_MAX - 1] = 0;
    return true;
}

static bool load_saved(scan_candidate_t *out)
{
    return load_blob(NVS_KEY, out) && candidate_valid(out);
}

static void capture_current(scan_candidate_t *out)
{
    esp_desync_config_t cfg;
    esp_desync_get_config(&cfg);
    memset(out, 0, sizeof(*out));
    out->mode = cfg.mode;
    out->fooling = cfg.fooling;
    out->ttl = cfg.fake_ttl;
    out->rndsni = cfg.rndsni;
    if (cfg.fake_sni != NULL) {
        strlcpy(out->sni, cfg.fake_sni, sizeof(out->sni));
    }
}

static void save_manual(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (on) {
        scan_candidate_t c;
        capture_current(&c);
        if (nvs_set_blob(h, NVS_KEY_MANUAL, &c, sizeof(c)) != ESP_OK) {
            nvs_close(h);
            return;
        }
    } else {
        nvs_erase_key(h, NVS_KEY_MANUAL);
    }
    nvs_commit(h);
    nvs_close(h);
}

void scan_set_manual(bool on)
{
    s_manual = on;
    s_fail_streak = 0;
    save_manual(on);
    ESP_LOGI(TAG, "manual override %s", on ? "enabled" : "disabled");
}

bool scan_is_manual(void)
{
    return s_manual;
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

static int probe_once(void)
{
    s_probes++;
    if (tg_probe(PROBE_TIMEOUT_MS) == 0) {
        s_last_ok_ms = esp_timer_get_time() / 1000;
        s_fail_streak = 0;
        return 0;
    }
    s_fails++;
    return -1;
}

static int probe_with(const scan_candidate_t *c)
{
    apply_candidate(c);
    vTaskDelay(pdMS_TO_TICKS(PROBE_SETTLE_MS));
    return probe_once();
}

void scan_init(void)
{
    if (load_saved(&s_current)) {
        s_have = true;
        apply_candidate(&s_current);
        char buf[80];
        scan_format(&s_current, buf, sizeof(buf));
        ESP_LOGI(TAG, "saved strategy: %s", buf);
    } else {
        ESP_LOGI(TAG, "no saved strategy");
    }

    /* Manual settings restored from NVS win over the auto strategy. */
    scan_candidate_t manual;
    if (load_blob(NVS_KEY_MANUAL, &manual) && candidate_valid(&manual)) {
        s_manual = true;
        apply_candidate(&manual);
        char buf[80];
        scan_format(&manual, buf, sizeof(buf));
        ESP_LOGI(TAG, "manual override restored: %s", buf);
    }
}

int scan_find_working(void)
{
    if (s_manual) {
        /* An explicit re-scan always drops the manual override. */
        scan_set_manual(false);
    }
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
    if (s_manual) {
        /* Probe the user's settings as-is; only give up on them when they
         * keep failing, so a single network hiccup does not reset them. */
        if (probe_once() == 0) {
            return 0;
        }
        s_fail_streak++;
        ESP_LOGW(TAG, "health probe failed with manual settings (%d in a row)", s_fail_streak);
        if (s_fail_streak >= CONFIG_APP_HEALTH_FAIL_THRESHOLD) {
            s_fail_streak = 0;
            ESP_LOGW(TAG, "manual settings keep failing, re-running auto-detection");
            return scan_find_working();
        }
        return -1;
    }

    if (!s_have) {
        return scan_find_working();
    }
    /* Probe the current config as-is: re-applying the saved candidate here
     * would silently overwrite changes made by the user. */
    if (probe_once() == 0) {
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

void scan_erase_saved(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY);
        nvs_erase_key(h, NVS_KEY_MANUAL);
        nvs_commit(h);
        nvs_close(h);
    }
    memset(&s_current, 0, sizeof(s_current));
    s_have = false;
    s_manual = false;
    s_fail_streak = 0;
}

void scan_get_status(scan_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->have = s_have;
    out->manual = s_manual;
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
