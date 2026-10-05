// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "esp_idf_version.h"
#include "esp_desync.h"
#include "app_config.h"
#include "telegram.h"
#include "wol.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 1, 0)
#error "esp32-zapret requires ESP-IDF v5.1 or newer"
#endif

static const char *TAG = "bot";

static volatile bool s_connected;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "wifi disconnected, retrying");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        if (!esp_sntp_enabled()) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
        }
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

static void copy_token(const char *src, char *dst, size_t dst_sz)
{
    size_t i = 0;
    while (src[i] && src[i] != ' ' && i < dst_sz - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void handle_update(const tg_update_t *u)
{
    if (CFG_TG_ADMIN_ID != 0 && u->chat_id != (int64_t)CFG_TG_ADMIN_ID) {
        tg_send_message(u->chat_id, "access denied");
        return;
    }

    char reply[512];
    const char *text = u->text;
    const char *sp = strchr(text, ' ');

    if (strncmp(text, "/wake", 5) == 0 || strncmp(text, "/wol", 4) == 0) {
        char macbuf[24] = CFG_WOL_MAC;
        if (sp && sp[1]) {
            copy_token(sp + 1, macbuf, sizeof(macbuf));
        }
        if (wol_send(macbuf, CFG_WOL_BROADCAST, CFG_WOL_PORT) == 0) {
            snprintf(reply, sizeof(reply), "magic packet sent to %s", macbuf);
        } else {
            snprintf(reply, sizeof(reply), "WoL failed, bad MAC? %s", macbuf);
        }
        tg_send_message(u->chat_id, reply);

    } else if (strncmp(text, "/desync", 7) == 0) {
        char name[24] = {0};
        if (sp && sp[1]) {
            copy_token(sp + 1, name, sizeof(name));
        }
        if (name[0]) {
            bool ok = false;
            esp_desync_mode_t m = esp_desync_mode_from_name(name, &ok);
            if (ok) {
                esp_desync_config_t c;
                esp_desync_get_config(&c);
                c.mode = m;
                esp_desync_set_config(&c);
                snprintf(reply, sizeof(reply), "desync mode: %s", esp_desync_mode_name(m));
            } else {
                snprintf(reply, sizeof(reply), "unknown mode, use: off split disorder fake fake_split tlsrec");
            }
        } else {
            snprintf(reply, sizeof(reply), "usage: /desync <off|split|disorder|fake|fake_split|tlsrec>");
        }
        tg_send_message(u->chat_id, reply);

    } else if (strncmp(text, "/ttl", 4) == 0) {
        int v = (sp && sp[1]) ? atoi(sp + 1) : 0;
        if (v >= 1 && v <= 255) {
            esp_desync_config_t c;
            esp_desync_get_config(&c);
            c.fake_ttl = (uint8_t)v;
            esp_desync_set_config(&c);
            snprintf(reply, sizeof(reply), "fake TTL = %d", v);
        } else {
            snprintf(reply, sizeof(reply), "usage: /ttl <1..255>, try 3..8");
        }
        tg_send_message(u->chat_id, reply);

    } else if (strncmp(text, "/fool", 5) == 0) {
        char name[16] = {0};
        uint32_t f = 0;
        bool ok = true;
        if (sp && sp[1]) {
            copy_token(sp + 1, name, sizeof(name));
        }
        if (name[0] == 0 || strcmp(name, "ttl") == 0) f = ESP_DESYNC_FOOL_TTL;
        else if (strcmp(name, "md5sig") == 0) f = ESP_DESYNC_FOOL_MD5SIG;
        else if (strcmp(name, "badsum") == 0) f = ESP_DESYNC_FOOL_BADSUM;
        else if (strcmp(name, "badseq") == 0) f = ESP_DESYNC_FOOL_BADSEQ;
        else if (strcmp(name, "none") == 0) f = ESP_DESYNC_FOOL_NONE;
        else ok = false;

        if (ok) {
            esp_desync_config_t c;
            esp_desync_get_config(&c);
            c.fooling = f;
            esp_desync_set_config(&c);
            snprintf(reply, sizeof(reply), "fooling: %s", name[0] ? name : "ttl");
        } else {
            snprintf(reply, sizeof(reply), "usage: /fool ttl|md5sig|badsum|badseq|none");
        }
        tg_send_message(u->chat_id, reply);

    } else if (strncmp(text, "/status", 7) == 0) {
        wifi_ap_record_t ap;
        esp_desync_config_t c;
        memset(&ap, 0, sizeof(ap));
        esp_wifi_sta_get_ap_info(&ap);
        esp_desync_get_config(&c);
        snprintf(reply, sizeof(reply),
                 "uptime %llds, heap %u, rssi %d\nmode %s, ttl %u, fool 0x%x\ntg %s (last HTTP %d)",
                 (long long)(esp_timer_get_time() / 1000000),
                 (unsigned)esp_get_free_heap_size(), ap.rssi,
                 esp_desync_mode_name(c.mode), (unsigned)c.fake_ttl, (unsigned)c.fooling,
                 tg_last_endpoint(), tg_last_http_status());
        tg_send_message(u->chat_id, reply);

    } else {
        tg_send_message(u->chat_id,
                        "esp32-zapret\n"
                        "/wake [mac] - send Wake-on-LAN magic packet\n"
                        "/status - device state\n"
                        "/desync <mode> - off|split|disorder|fake|fake_split|tlsrec\n"
                        "/ttl <n> - fake packet TTL (tune 3..8)\n"
                        "/fool <mode> - ttl|md5sig|badsum|badseq|none");
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, CFG_WIFI_SSID, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, CFG_WIFI_PASS, sizeof(sta.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_ERROR_CHECK(esp_desync_init(NULL));

    ESP_LOGI(TAG, "waiting for network and time...");
    bool warned = false;
    while (!s_connected || !time_is_valid()) {
        if (!warned && s_connected) {
            ESP_LOGW(TAG, "waiting for SNTP sync...");
            warned = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "ready, starting telegram long-poll");

    int64_t offset = 0;
    tg_update_t upd;

    while (1) {
        if (!s_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int r = tg_get_updates(&upd, offset, 25);
        if (r > 0) {
            offset = upd.update_id + 1;
            if (upd.text[0]) {
                ESP_LOGI(TAG, "cmd from %lld: %s", (long long)upd.chat_id, upd.text);
                handle_update(&upd);
            }
        } else if (r == TG_RC_HTTP) {
            /* e.g. 409 Conflict: another getUpdates consumer. Do not hammer. */
            vTaskDelay(pdMS_TO_TICKS(15000));
        } else if (r < 0) {
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}
