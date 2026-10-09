// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_desync.h"
#include "https_client.h"
#include "net_utils.h"
#include "telegram.h"
#include "app_config.h"
#include "app_settings.h"
#if CONFIG_APP_DOH_FALLBACK
#include "doh.h"
#endif

static const char *TAG = "telegram";

#define TG_HOST  "api.telegram.org"
#define RESP_MAX 8192

/* Short per-candidate TCP timeout: dead Telegram IPs usually just swallow SYNs,
 * we must not waste tens of seconds on them. */
#define TG_CONNECT_TIMEOUT_MS 2500

/* Quick-probe connect timeout: the scanner checks many strategies, so each
 * probe must fail fast when the pinned addresses are dead. */
#define TG_PROBE_CONNECT_TIMEOUT_MS 1200

/* Known api.telegram.org addresses; overridden/extended by CFG_TG_API_IPS.
 * Order matters: tried top to bottom, DNS is the last resort. */
static const char TG_DEFAULT_IPS[] =
    "149.154.167.220,149.154.167.191,149.154.175.50,149.154.175.53,149.154.166.110";

static char s_resp[RESP_MAX];
static char s_last_endpoint[16];
static int s_last_status;

/* Persistent keep-alive session: without it every poll and every reply paid a
 * full TCP+TLS handshake (1-3 s on classic ESP32). */
static https_conn_t s_sess;
static bool s_sess_open;
static bool s_cmd_menu_done;

static int tg_connect(https_conn_t *c, int timeout_ms)
{
    uint32_t ips[16];
    int n = 0;

    if (CFG_TG_API_IPS[0] != '\0') {
        n = net_parse_ip_list(CFG_TG_API_IPS, ips, 16);
    }
    if (n == 0) {
        n = net_parse_ip_list(TG_DEFAULT_IPS, ips, 16);
    }

    if (https_connect(c, TG_HOST, ips, n, 443, TG_CONNECT_TIMEOUT_MS, timeout_ms, true) == 0) {
        strlcpy(s_last_endpoint, https_endpoint(c), sizeof(s_last_endpoint));
        return 0;
    }

#if CONFIG_APP_DOH_FALLBACK
    uint32_t resolved[8];
    int rn = doh_resolve(TG_HOST, resolved, 8);
    if (rn > 0 &&
        https_connect(c, TG_HOST, resolved, rn, 443, TG_CONNECT_TIMEOUT_MS, timeout_ms, true) == 0) {
        strlcpy(s_last_endpoint, https_endpoint(c), sizeof(s_last_endpoint));
        return 0;
    }
#endif

    ESP_LOGW(TAG, "all telegram endpoints failed (%d pinned)", n);
    strlcpy(s_last_endpoint, "-", sizeof(s_last_endpoint));
    return -1;
}

static void tg_session_close(void)
{
    if (s_sess_open) {
        https_close(&s_sess);
        s_sess_open = false;
    }
}

/* Registers the command menu once per boot (cosmetic, best effort). */
static void tg_set_commands_once(void)
{
    if (s_cmd_menu_done) {
        return;
    }
    s_cmd_menu_done = true;

    static const char menu[] =
        "{\"commands\":["
        "{\"command\":\"status\",\"description\":\"состояние устройства\"},"
        "{\"command\":\"scan\",\"description\":\"перебрать стратегии заново\"},"
        "{\"command\":\"strategy\",\"description\":\"текущая стратегия и статистика\"},"
        "{\"command\":\"wake\",\"description\":\"Wake-on-LAN пакет\"},"
        "{\"command\":\"desync\",\"description\":\"режим обхода\"},"
        "{\"command\":\"ttl\",\"description\":\"TTL фейка\"},"
        "{\"command\":\"fool\",\"description\":\"метод фулинга\"},"
        "{\"command\":\"rndsni\",\"description\":\"случайный decoy SNI\"},"
        "{\"command\":\"heap\",\"description\":\"память (heap)\"},"
        "{\"command\":\"ip\",\"description\":\"IP, шлюз, Wi-Fi\"},"
        "{\"command\":\"stats\",\"description\":\"анонимная статистика\"},"
        "{\"command\":\"reboot\",\"description\":\"перезагрузить\"},"
        "{\"command\":\"help\",\"description\":\"справка\"}"
        "]}";

    const app_settings_t *settings = app_settings_get();
    char path[192];
    snprintf(path, sizeof(path), "/bot%s/setMyCommands", settings->tg_token);

    https_conn_t conn;
    if (tg_connect(&conn, 8000) == 0) {
        int status = 0;
        https_post_json(&conn, path, menu, 8000, &status);
        https_close(&conn);
        ESP_LOGD(TAG, "setMyCommands HTTP %d", status);
    }
}

/* Sends a keep-alive request over the session, reconnecting once if the
 * connection died. Returns body bytes or -1. */
static int tg_session_get(const char *path, char *resp, size_t resp_sz,
                          int timeout_ms, int *status)
{
    if (s_sess_open || tg_connect(&s_sess, 8000) == 0) {
        s_sess_open = true;
    } else {
        s_last_status = 0;
        return -1;
    }

    bool alive = false;
    int n = https_request_ka(&s_sess, path, "application/json", resp, resp_sz,
                             timeout_ms, status, &alive);
    s_last_status = status ? *status : 0;
    if (n < 0) {
        tg_session_close();
        /* One retry on a fresh connection (the peer may have closed an idle
         * keep-alive socket while the device was busy scanning). */
        if (tg_connect(&s_sess, 8000) != 0) {
            s_last_status = 0;
            return -1;
        }
        s_sess_open = true;
        alive = false;
        n = https_request_ka(&s_sess, path, "application/json", resp, resp_sz,
                             timeout_ms, status, &alive);
        s_last_status = status ? *status : 0;
    }
    if (n < 0 || !alive) {
        tg_session_close();
    }
    return n;
}

static void log_api_error(int status)
{
    char desc[130] = "";
    const char *p = strstr(s_resp, "\"description\":\"");
    if (p != NULL) {
        p += 15;
        size_t i = 0;
        while (p[i] != '\0' && p[i] != '"' && i < sizeof(desc) - 1) {
            desc[i] = p[i];
            i++;
        }
        desc[i] = '\0';
    }

    if (status == 409) {
        ESP_LOGE(TAG, "HTTP 409 Conflict: another client polls this bot token! "
                      "Only one getUpdates consumer is allowed. (%s)", desc);
    } else if (status == 401) {
        ESP_LOGE(TAG, "HTTP 401: bad bot token. (%s)", desc);
    } else {
        ESP_LOGW(TAG, "HTTP %d via %s: %s", status, s_last_endpoint, desc);
    }
}

static size_t utf8_put(char *dst, size_t cap, uint32_t cp)
{
    if (cp < 0x80) {
        if (cap < 1) return 0;
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (cap < 2) return 0;
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        if (cap < 3) return 0;
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        dst[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    if (cap < 4) return 0;
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    dst[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

static void json_parse_text(const char *resp, char *out, size_t out_sz)
{
    out[0] = 0;
    const char *p = strstr(resp, "\"text\":\"");
    if (p == NULL) {
        return;
    }
    p += 8;
    size_t i = 0;

    while (*p && *p != '"' && i + 4 < out_sz) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[i++] = '\n'; p++; break;
            case 't': out[i++] = '\t'; p++; break;
            case 'r': out[i++] = '\r'; p++; break;
            case 'b': out[i++] = '\b'; p++; break;
            case 'f': out[i++] = '\f'; p++; break;
            case '/': out[i++] = '/'; p++; break;
            case '\\': out[i++] = '\\'; p++; break;
            case '"': out[i++] = '"'; p++; break;
            case 'u': {
                int cp = hex4(p + 1);
                if (cp < 0) {
                    p++;
                    break;
                }
                p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    int lo = hex4(p + 2);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                i += utf8_put(out + i, out_sz - i - 1, (uint32_t)cp);
                break;
            }
            default:
                out[i++] = *p++;
                break;
            }
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = 0;
}

int tg_get_updates(tg_update_t *out, int64_t offset, int long_poll_s)
{
    const app_settings_t *settings = app_settings_get();
    char path[256];
    snprintf(path, sizeof(path),
             "/bot%s/getUpdates?timeout=%d&offset=%lld"
             "&allowed_updates=%%5B%%22message%%22%%5D",
             settings->tg_token, long_poll_s, (long long)offset);

    int status = 0;
    int n = tg_session_get(path, s_resp, sizeof(s_resp), long_poll_s * 1000 + 20000, &status);
    if (n <= 0) {
        return TG_RC_TRANSPORT;
    }
    if (status != 200) {
        log_api_error(status);
        return TG_RC_HTTP;
    }

    tg_set_commands_once();

    memset(out, 0, sizeof(*out));

    const char *p = strstr(s_resp, "\"update_id\":");
    if (p == NULL) {
        return TG_RC_NO_UPDATES;
    }
    out->update_id = strtoll(p + 12, NULL, 10);
    out->valid = true;

    p = strstr(s_resp, "\"chat\":");
    if (p != NULL) {
        const char *q = strstr(p, "\"id\":");
        if (q != NULL) {
            out->chat_id = strtoll(q + 5, NULL, 10);
        }
    }

    json_parse_text(s_resp, out->text, sizeof(out->text));

    return TG_RC_UPDATE;
}

int tg_send_message(int64_t chat_id, const char *text)
{
    const app_settings_t *settings = app_settings_get();
    static char resp[2048];
    char enc[1024];
    char path[1536];
    int status = 0;

    net_url_encode(text, enc, sizeof(enc));
    snprintf(path, sizeof(path),
             "/bot%s/sendMessage?chat_id=%lld&text=%s",
             settings->tg_token, (long long)chat_id, enc);

    int n = tg_session_get(path, resp, sizeof(resp), 20000, &status);
    if (n > 0 && status != 200) {
        ESP_LOGW(TAG, "sendMessage HTTP %d", status);
    }
    return n;
}

int tg_probe_ex(int timeout_ms, int *stage)
{
    static char resp[256];
    uint32_t ips[4];
    int n = 0;
    int fstage = HTTPS_STAGE_TLS;

    if (stage != NULL) {
        *stage = HTTPS_STAGE_OK;
    }

    if (CFG_TG_API_IPS[0] != '\0') {
        n = net_parse_ip_list(CFG_TG_API_IPS, ips, 4);
    }
    if (n == 0) {
        n = net_parse_ip_list(TG_DEFAULT_IPS, ips, 4);
    }

    https_conn_t conn;
    if (https_connect_ex(&conn, TG_HOST, ips, n, 443, TG_PROBE_CONNECT_TIMEOUT_MS,
                         timeout_ms, false, &fstage) != 0) {
        /* Pinned list is stale: fall back to the full path (DNS/DoH). */
        if (tg_connect(&conn, timeout_ms) != 0) {
            if (stage != NULL) {
                *stage = fstage;
            }
            return -1;
        }
        fstage = HTTPS_STAGE_TLS;
    }

    int status = 0;
    int r = https_request(&conn, "/", "application/json", resp, sizeof(resp), timeout_ms, &status);
    strlcpy(s_last_endpoint, https_endpoint(&conn), sizeof(s_last_endpoint));
    https_close(&conn);
    if (r <= 0) {
        if (stage != NULL) {
            *stage = HTTPS_STAGE_RST;
        }
        return -1;
    }
    return 0;
}

int tg_probe(int timeout_ms)
{
    return tg_probe_ex(timeout_ms, NULL);
}

int tg_selftest(void)
{
    int64_t start = esp_timer_get_time();
    int rc = tg_probe(20000);
    int64_t ms = (esp_timer_get_time() - start) / 1000;
    if (rc == 0) {
        ESP_LOGI(TAG, "selftest ok: %lld ms, endpoint %s", (long long)ms, s_last_endpoint);
    } else {
        ESP_LOGW(TAG, "selftest failed (endpoint %s)", s_last_endpoint);
    }
    return rc;
}

const char *tg_last_endpoint(void)
{
    return s_last_endpoint[0] ? s_last_endpoint : "-";
}

int tg_last_http_status(void)
{
    return s_last_status;
}
