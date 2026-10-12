// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "lwip/netif.h"
#include "esp_chip_info.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#if defined(CONFIG_SOC_TEMP_SENSOR_SUPPORTED) && CONFIG_SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#include "esp_desync.h"
#include "desync_scan.h"
#include "https_client.h"
#include "doh.h"
#include "fw_version.h"
#include "app_config.h"
#include "stats_payload.h"
#include "stats.h"

static const char *TAG = "stats";

#define NVS_NAMESPACE   "stats"
#define NVS_KEY_ENABLED "enabled"
#define NVS_KEY_LAST    "last"
#define NVS_KEY_SEED    "seed"
#define NVS_KEY_BOOTS   "boots"
#define NVS_KEY_SEQ     "seq"
#define NVS_KEY_RKEY    "rkey"
#define STATS_PATH      "/api/v1/report"
#define BOOT_MIN_GAP_S  3600
#define SEND_TIMEOUT_MS 8000
#define BOOT_STORM_S    600
#define BOOT_STORM_MIN  3
#define BOOT_RING_MAX   8
#define ERR_SLOTS       6
#define ERR_TOP         4
/* Failed sends retry at most every 15 minutes (never on every loop tick). */
#define RETRY_BACKOFF_S 900

typedef struct {
    uint32_t idx;
    uint32_t ts[BOOT_RING_MAX];
} boot_ring_t;

typedef struct {
    uint16_t code;
    uint16_t count;
} err_slot_t;

static bool s_enabled;
static int64_t s_last; /* unix seconds of the last successful report, 0 = never */

static uint32_t s_wifi_disc;
static uint8_t s_wifi_reason;
static uint16_t s_sntp_s = 0xFFFF;
static bool s_sntp_ok;
static bool s_boot_storm;

static err_slot_t s_errs[ERR_SLOTS];

static unsigned char s_seed[16];
static bool s_seed_ok;

/* Reporting key: NVS blob "rkey" (per-device) or CFG_STATS_HMAC_KEY. */
static unsigned char s_key[32];
static bool s_key_ok;
static uint32_t s_seq;
static int64_t s_last_try; /* unix seconds of the last send attempt, 0 = never */
static uint32_t s_last_uptime;

/* Serializes report building/sending: the web UI task can trigger a manual
 * report while the main loop is in a periodic tick. */
static SemaphoreHandle_t s_lock;

#if defined(CONFIG_SOC_TEMP_SENSOR_SUPPORTED) && CONFIG_SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t s_temp;
#endif

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

/* Device-local secret for the rotating pseudonym (never leaves the device). */
static void seed_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    size_t sz = sizeof(s_seed);
    if (nvs_get_blob(h, NVS_KEY_SEED, s_seed, &sz) != ESP_OK || sz != sizeof(s_seed)) {
        for (int i = 0; i < (int)sizeof(s_seed); i += 4) {
            uint32_t r = esp_random();
            memcpy(s_seed + i, &r, 4);
        }
        if (nvs_set_blob(h, NVS_KEY_SEED, s_seed, sizeof(s_seed)) != ESP_OK) {
            nvs_close(h);
            return;
        }
        nvs_commit(h);
    }
    s_seed_ok = true;
    nvs_close(h);
}

/* rid = first 8 bytes of HMAC-SHA256(seed, day) in hex: a pseudonym that
 * rotates daily and cannot be linked across days without the local secret. */
static bool hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen,
                        unsigned char out[32])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return md != NULL && mbedtls_md_hmac(md, key, klen, msg, mlen, out) == 0;
}

static void hex8(const unsigned char *in, char out[17])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[16] = 0;
}

static void rid_build(char out[17])
{
    out[0] = 0;
    if (!s_seed_ok) {
        return;
    }
    char day[16];
    snprintf(day, sizeof(day), "%u", (unsigned)(time(NULL) / 86400));
    unsigned char mac[32];
    if (!hmac_sha256(s_seed, sizeof(s_seed), day, strlen(day), mac)) {
        return;
    }
    hex8(mac, out);
}

/* Stable pseudonym derived from the eFuse MAC: used for per-device keys and
 * revocation; not a secret (the MAC cannot be recovered from it). */
static void dev_build(char out[17])
{
    out[0] = 0;
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return;
    }
    unsigned char h[32];
    static const char ctx[] = "esp32-zapret-device-id";
    if (!hmac_sha256(ctx, sizeof(ctx) - 1, mac, sizeof(mac), h)) {
        return;
    }
    hex8(h, out);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void key_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_key);
        if (nvs_get_blob(h, NVS_KEY_RKEY, s_key, &sz) == ESP_OK && sz == sizeof(s_key)) {
            s_key_ok = true;
        }
        nvs_close(h);
    }
    if (s_key_ok) {
        return;
    }
    const char *hex = CFG_STATS_HMAC_KEY;
    int n = 0;
    while (hex[0] != 0 && hex[1] != 0 && n < (int)sizeof(s_key)) {
        int hi = hexval(hex[0]);
        int lo = hexval(hex[1]);
        if (hi < 0 || lo < 0) {
            break;
        }
        s_key[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    s_key_ok = n >= 16;
}

static void seq_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t v = 0;
    if (nvs_get_u32(h, NVS_KEY_SEQ, &v) == ESP_OK) {
        s_seq = v;
    }
    nvs_close(h);
}

/* Monotonic counter persisted across reboots (anti-replay on the server). */
static uint32_t seq_next(void)
{
    s_seq++;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u32(h, NVS_KEY_SEQ, s_seq) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
    return s_seq;
}

/* Reset-loop detector: 3+ boots within 10 minutes (persisted in NVS). */
static void boot_ring_check(void)
{
    nvs_handle_t h;
    boot_ring_t ring;
    int64_t now = (int64_t)time(NULL);

    memset(&ring, 0, sizeof(ring));
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    size_t sz = sizeof(ring);
    if (nvs_get_blob(h, NVS_KEY_BOOTS, &ring, &sz) != ESP_OK || sz != sizeof(ring)) {
        memset(&ring, 0, sizeof(ring));
    }
    ring.ts[ring.idx % BOOT_RING_MAX] = (uint32_t)now;
    ring.idx++;

    int recent = 0;
    for (int i = 0; i < BOOT_RING_MAX; i++) {
        if (ring.ts[i] != 0 && now >= (int64_t)ring.ts[i] &&
            now - (int64_t)ring.ts[i] <= BOOT_STORM_S) {
            recent++;
        }
    }
    s_boot_storm = recent >= BOOT_STORM_MIN;

    if (nvs_set_blob(h, NVS_KEY_BOOTS, &ring, sizeof(ring)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

void stats_anon_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
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
    seed_init();
    key_init();
    seq_load();
    boot_ring_check();
#if defined(CONFIG_SOC_TEMP_SENSOR_SUPPORTED) && CONFIG_SOC_TEMP_SENSOR_SUPPORTED
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_temp) != ESP_OK) {
        s_temp = NULL;
    } else {
        temperature_sensor_enable(s_temp);
    }
#endif
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

void stats_anon_note_wifi_disconnect(uint8_t reason)
{
    s_wifi_disc++;
    s_wifi_reason = reason;
}

void stats_anon_note_sntp(uint16_t seconds, bool ok)
{
    s_sntp_s = seconds;
    s_sntp_ok = ok;
}

void stats_anon_note_err(uint16_t code)
{
    int free_slot = -1;
    for (int i = 0; i < ERR_SLOTS; i++) {
        if (s_errs[i].count > 0 && s_errs[i].code == code) {
            s_errs[i].count++;
            return;
        }
        if (s_errs[i].count == 0 && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        s_errs[free_slot].code = code;
        s_errs[free_slot].count = 1;
    }
}

static void errs_format(char *buf, size_t cap)
{
    size_t off = 0;
    bool used[ERR_SLOTS] = { false };

    buf[0] = 0;
    for (int k = 0; k < ERR_TOP; k++) {
        int best = -1;
        for (int i = 0; i < ERR_SLOTS; i++) {
            if (!used[i] && s_errs[i].count > 0 &&
                (best < 0 || s_errs[i].count > s_errs[best].count)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        used[best] = true;
        int w = snprintf(buf + off, cap - off, "%s0x%x:%u",
                         off ? "," : "", (unsigned)s_errs[best].code,
                         (unsigned)s_errs[best].count);
        if (w <= 0 || (size_t)w >= cap - off) {
            break;
        }
        off += (size_t)w;
    }
}

static int16_t read_temp_c(void)
{
#if defined(CONFIG_SOC_TEMP_SENSOR_SUPPORTED) && CONFIG_SOC_TEMP_SENSOR_SUPPORTED
    if (s_temp != NULL) {
        float tc = 0;
        if (temperature_sensor_get_celsius(s_temp, &tc) == ESP_OK) {
            return (int16_t)tc;
        }
    }
#endif
    return -128;
}

static uint8_t decoy_sni_cat(const esp_desync_config_t *c)
{
    if (c->rndsni) {
        return 5;
    }
    if (c->fake_sni == NULL || c->fake_sni[0] == 0) {
        return 0;
    }
    if (strcmp(c->fake_sni, "www.iana.org") == 0) {
        return 1;
    }
    if (strcmp(c->fake_sni, "www.yandex.ru") == 0) {
        return 2;
    }
    if (strcmp(c->fake_sni, "mail.ru") == 0) {
        return 3;
    }
    return 4;
}

static uint8_t build_cfg_flags(const esp_desync_config_t *c)
{
    uint8_t cfg = 0;
#if CONFIG_APP_DOH_FALLBACK
    cfg |= 1u << 0;
#endif
#if CONFIG_APP_WEB_UI
    cfg |= 1u << 1;
#endif
#if CONFIG_APP_ENABLE_TELEGRAM_BOT
    cfg |= 1u << 2;
#endif
#if CONFIG_APP_STATS_DEFAULT_ON
    cfg |= 1u << 3;
#endif
    if (c->rndsni) {
        cfg |= 1u << 4;
    }
    if (c->fake_sni != NULL && strcmp(c->fake_sni, "www.iana.org") != 0) {
        cfg |= 1u << 5;
    }
    return cfg;
}

static uint32_t app_partition_free(void)
{
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (part == NULL) {
        return 0;
    }
    esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
    esp_image_metadata_t meta;
    if (esp_image_get_metadata(&pos, &meta) != ESP_OK || meta.image_len >= part->size) {
        return 0;
    }
    return part->size - meta.image_len;
}

static void collect(stats_payload_t *p, const char *event)
{
    /* Statics: the strings referenced by the payload must stay valid until
     * stats_build_payload() runs after collect() returns. */
    static scan_status_t st;
    static scan_telemetry_t tel;
    static char errs_buf[48];
    static char rid[17];
    static char dev[17];

    memset(p, 0, sizeof(*p));
    p->event = event;
    p->fw = FW_VERSION;
    p->target = CONFIG_IDF_TARGET;
    p->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    p->free_heap = esp_get_free_heap_size();
    p->min_heap = esp_get_minimum_free_heap_size();
    p->max_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    p->reset_reason = (uint8_t)esp_reset_reason();

    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        p->rssi = ap.rssi;
    }
    uint8_t ch = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&ch, &second) == ESP_OK) {
        p->channel = ch;
    }

    esp_desync_config_t c;
    esp_desync_get_config(&c);
    p->mode = (uint8_t)c.mode;
    p->fooling = c.fooling;
    p->ttl = c.fake_ttl;
    p->split1 = c.split_pos;
    p->split2 = c.split_pos2;
    p->sni_cat = decoy_sni_cat(&c);
    p->cfg = build_cfg_flags(&c);

    scan_get_status(&st);
    p->have_strategy = st.have;
    p->strategy = st.have ? st.strategy : "";
    p->scans = st.scans;
    p->probes = st.probes;
    p->probe_fails = st.probe_fails;

    scan_get_telemetry(&tel);
    p->tried = tel.tried;
    p->tts_s = tel.tts_s;
    p->strategy_changed = tel.changes;
    p->stage = tel.stage;

    p->wifi_disc = s_wifi_disc;
    p->wifi_reason = s_wifi_reason;
    p->boot_storm = s_boot_storm;
    p->sntp_s = s_sntp_s;
    p->sntp_ok = s_sntp_ok;
    p->doh = doh_get_flags();
    p->temp_c = read_temp_c();

    errs_format(errs_buf, sizeof(errs_buf));
    p->errs = errs_buf;

    if (netif_default != NULL) {
        p->mtu = (uint16_t)netif_default->mtu;
    }

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    p->chip_rev = ci.revision;
    p->cpu_mhz = (uint16_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    p->flash_free = app_partition_free();
    p->psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;

    rid_build(rid);
    p->rid = rid;
    dev_build(dev);
    p->dev = dev;
    p->nonce = esp_random();
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

/* Local sanity invariants: garbage samples are dropped instead of being signed
 * and sent. */
static bool payload_sane(const stats_payload_t *p)
{
    if (p->free_heap == 0 || p->free_heap > 8u * 1024 * 1024) {
        return false;
    }
    if (p->min_heap > p->free_heap) {
        return false;
    }
    if (p->probe_fails > p->probes) {
        return false;
    }
    if (p->ttl == 0) {
        return false;
    }
    if (p->uptime_s + 5 < s_last_uptime) {
        return false; /* uptime must not go backwards within one boot */
    }
    return true;
}

/* Canonical body + `,"sig":"<hex>"}`; without a key the report goes unsigned
 * (the server decides how much to trust it). */
static int sign_body(const char *canonical, char *out, size_t cap)
{
    size_t cl = strlen(canonical);
    if (cl < 2 || canonical[cl - 1] != '}') {
        return -1;
    }
    if (!s_key_ok) {
        strlcpy(out, canonical, cap);
        return (int)strlen(out);
    }
    unsigned char mac[32];
    if (!hmac_sha256(s_key, sizeof(s_key), canonical, cl, mac)) {
        return -1;
    }
    static const char hx[] = "0123456789abcdef";
    char hex[65];
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = hx[mac[i] >> 4];
        hex[i * 2 + 1] = hx[mac[i] & 0x0f];
    }
    hex[64] = 0;
    int n = snprintf(out, cap, "%.*s,\"sig\":\"%s\"}", (int)(cl - 1), canonical, hex);
    if (n <= 0 || n >= (int)cap) {
        return -1;
    }
    return n;
}

static void send_report_locked(const char *event)
{
    static char body[1280];
    static char signed_body[1408];

    stats_payload_t p;
    collect(&p, event);
    if (!payload_sane(&p)) {
        ESP_LOGD(TAG, "report skipped: sanity check failed");
        return;
    }
    s_last_uptime = p.uptime_s;
    p.seq = seq_next();

    if (stats_build_payload(&p, body, sizeof(body)) < 0) {
        return;
    }
    if (sign_body(body, signed_body, sizeof(signed_body)) < 0) {
        return;
    }

    https_conn_t conn;
    if (!connect_stats(&conn)) {
        ESP_LOGD(TAG, "report not sent: endpoint unavailable");
        return;
    }

    int status = 0;
    int r = https_post_json(&conn, STATS_PATH, signed_body, SEND_TIMEOUT_MS, &status);
    https_close(&conn);
    if (r > 0 && status >= 200 && status < 300) {
        last_save((int64_t)time(NULL));
        ESP_LOGD(TAG, "report sent (%s, seq %u)", event, (unsigned)p.seq);
    } else {
        ESP_LOGD(TAG, "report not sent: HTTP %d", status);
    }
}

static void send_report(const char *event)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    send_report_locked(event);
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

void stats_anon_report(const char *event)
{
    if (!s_enabled || event == NULL) {
        return;
    }

    int64_t now = (int64_t)time(NULL);
    if (now > 0) {
        /* Failed attempts retry on the backoff, never on every loop tick. */
        if (strcmp(event, "periodic") == 0) {
            if (s_last > 0 && now - s_last < CONFIG_APP_STATS_INTERVAL_S) {
                return;
            }
            if (s_last_try > 0 && now - s_last_try < RETRY_BACKOFF_S) {
                return;
            }
        }
        if (strcmp(event, "boot") == 0) {
            if (s_last > 0 && now - s_last < BOOT_MIN_GAP_S) {
                return;
            }
            if (s_last_try > 0 && now - s_last_try < RETRY_BACKOFF_S) {
                return;
            }
        }
        s_last_try = now;
    }
    send_report(event);
}

void stats_anon_tick(void)
{
    stats_anon_report("periodic");
}
