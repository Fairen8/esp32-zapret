// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_desync.h"
#include "desync_scan.h"
#include "app_settings.h"
#include "stats.h"
#include "telegram.h"
#include "fw_version.h"
#include "webui.h"

static const char *TAG = "webui";

#if CONFIG_APP_WEB_UI

static httpd_handle_t s_server;
static bool s_setup;
static volatile bool s_scan_running;

/* ---------------------------------------------------------------- helpers */

static esp_err_t unauthorized(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"esp32-zapret\"");
    return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "auth required");
}

static bool check_auth(httpd_req_t *req)
{
    if (s_setup) {
        return true;
    }
    char creds[192];
    int n = snprintf(creds, sizeof(creds), "admin:%s", app_settings_get()->web_pass);
    unsigned char b64[256];
    size_t blen = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &blen,
                              (const unsigned char *)creds, (size_t)n) != 0) {
        return false;
    }
    char want[320];
    snprintf(want, sizeof(want), "Basic %.*s", (int)blen, (const char *)b64);
    char given[320];
    if (httpd_req_get_hdr_value_str(req, "Authorization", given, sizeof(given)) != ESP_OK) {
        return false;
    }
    return strcmp(given, want) == 0;
}

static esp_err_t bad_request(httpd_req_t *req, const char *why)
{
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, why);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return err;
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2048) {
        return NULL;
    }
    char buf[2048];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            return NULL;
        }
        received += r;
    }
    buf[received] = 0;
    return cJSON_Parse(buf);
}

static const char *jstr(const cJSON *o, const char *key, const char *dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : dflt;
}

static int jint(const cJSON *o, const char *key, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void schedule_reboot(int delay_ms)
{
    static esp_timer_handle_t timer;
    if (timer == NULL) {
        const esp_timer_create_args_t args = { .callback = reboot_cb, .name = "web_reboot" };
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            esp_restart();
        }
    }
    esp_timer_start_once(timer, (uint64_t)delay_ms * 1000);
}

/* ---------------------------------------------------------------- handlers */

static const char INDEX_HTML[] =
"<!doctype html>\n"
"<html lang=\"ru\"><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>esp32-zapret</title><style>\n"
":root{color-scheme:dark}*{box-sizing:border-box}\n"
"body{font:15px/1.45 system-ui,sans-serif;margin:0;background:#111;color:#eee}\n"
"main{max-width:760px;margin:0 auto;padding:16px}\n"
"h1{font-size:20px;margin:8px 0}h2{font-size:16px;margin:0 0 8px;color:#9ad}\n"
".card{background:#1b1b1f;border:1px solid #2a2a30;border-radius:10px;padding:14px;margin:12px 0}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:8px}\n"
"label{display:block;font-size:12px;color:#aaa;margin-bottom:2px}\n"
"input,select{width:100%;padding:8px;border-radius:6px;border:1px solid #333;background:#0e0e12;color:#eee}\n"
"button{padding:9px 14px;border-radius:6px;border:0;background:#2b6cb0;color:#fff;font-weight:600;cursor:pointer}\n"
"button.gray{background:#444}button.red{background:#9b2c2c}\n"
".row{display:flex;gap:8px;flex-wrap:wrap;align-items:end;margin-top:8px}\n"
".kv{display:grid;grid-template-columns:auto 1fr;gap:2px 12px;font-size:13px}\n"
".kv b{color:#9ad;font-weight:600;white-space:nowrap}\n"
"#msg{padding:8px;border-radius:6px;margin:10px 0;display:none}\n"
"#msg.ok{display:block;background:#1c3a24;color:#8f8}\n"
"#msg.err{display:block;background:#3a1c1c;color:#f88}\n"
".chk{display:flex;gap:6px;align-items:center;font-size:13px}.chk input{width:auto}\n"
".hint{font-size:12px;color:#888;margin-top:6px}\n"
"</style></head><body><main>\n"
"<h1>esp32-zapret <span id=\"fw\" class=\"hint\"></span></h1>\n"
"<div id=\"msg\"></div>\n"
"<div id=\"setup\" style=\"display:none\"><div class=\"card\">\n"
"<h2>Первичная настройка</h2>\n"
"<div class=\"grid\"><div><label>Wi-Fi сеть</label><select id=\"wifi_list\"></select></div>\n"
"<div><label>&nbsp;</label><button class=\"gray\" onclick=\"loadWifi()\">Обновить список</button></div></div>\n"
"<div class=\"grid\" style=\"margin-top:8px\">\n"
"<div><label>SSID</label><input id=\"p_ssid\"></div>\n"
"<div><label>Пароль Wi-Fi</label><input id=\"p_pass\" type=\"password\"></div>\n"
"<div><label>Токен бота</label><input id=\"p_token\"></div>\n"
"<div><label>Admin chat id (0 = любой)</label><input id=\"p_admin\" value=\"0\"></div></div>\n"
"<div class=\"grid\" style=\"margin-top:8px\">\n"
"<div><label>MAC ПК</label><input id=\"p_mac\" value=\"AA:BB:CC:DD:EE:FF\"></div>\n"
"<div><label>Broadcast</label><input id=\"p_bcast\" value=\"255.255.255.255\"></div>\n"
"<div><label>WOL порт</label><input id=\"p_port\" value=\"9\"></div>\n"
"<div><label>Пароль веб-интерфейса</label><input id=\"p_webpass\" type=\"password\" placeholder=\"мин. 8 символов\"></div></div>\n"
"<div class=\"row\"><button onclick=\"provision()\">Сохранить и перезагрузить</button></div>\n"
"<div class=\"hint\">В no-bot сборке токен не нужен. Пароль веб-интерфейса = пароль этой точки доступа.</div>\n"
"</div></div>\n"
"<div id=\"panel\" style=\"display:none\">\n"
"<div class=\"card\"><h2>Состояние</h2><div id=\"status\" class=\"kv\"></div></div>\n"
"<div class=\"card\"><h2>Обход DPI</h2><div class=\"grid\">\n"
"<div><label>Режим</label><select id=\"d_mode\">\n"
"<option value=\"off\">off</option><option value=\"split\">split</option>\n"
"<option value=\"disorder\">disorder</option><option value=\"fake\">fake</option>\n"
"<option value=\"fake_split\">fake_split</option><option value=\"tlsrec\">tlsrec</option>\n"
"</select></div><div><label>TTL фейка</label><input id=\"d_ttl\" type=\"number\" min=\"1\" max=\"255\"></div></div>\n"
"<div class=\"row\" id=\"d_fool\">\n"
"<label class=\"chk\"><input type=\"checkbox\" value=\"1\">ttl</label>\n"
"<label class=\"chk\"><input type=\"checkbox\" value=\"2\">badsum</label>\n"
"<label class=\"chk\"><input type=\"checkbox\" value=\"4\">badseq</label>\n"
"<label class=\"chk\"><input type=\"checkbox\" value=\"8\">md5sig</label>\n"
"<label class=\"chk\"><input type=\"checkbox\" value=\"16\">datanoack</label></div>\n"
"<div class=\"row\"><button onclick=\"applyDesync()\">Применить (ручной режим)</button>\n"
"<button class=\"gray\" onclick=\"runScan()\">Автоподбор</button></div></div>\n"
"<div class=\"card\"><h2>Wi-Fi</h2><div class=\"grid\">\n"
"<div><label>SSID</label><input id=\"w_ssid\"></div>\n"
"<div><label>Пароль</label><input id=\"w_pass\" type=\"password\"></div></div>\n"
"<div class=\"row\"><button onclick=\"saveWifi()\">Сохранить (нужна перезагрузка)</button></div></div>\n"
"<div class=\"card\"><h2>Бот</h2><div class=\"grid\">\n"
"<div><label>Токен (пусто = не менять)</label><input id=\"b_token\"></div>\n"
"<div><label>Admin chat id (0 = любой)</label><input id=\"b_admin\" type=\"number\"></div></div>\n"
"<div class=\"row\"><button onclick=\"saveBot()\">Сохранить</button></div></div>\n"
"<div class=\"card\"><h2>Wake-on-LAN</h2><div class=\"grid\">\n"
"<div><label>MAC</label><input id=\"l_mac\"></div>\n"
"<div><label>Broadcast</label><input id=\"l_bcast\"></div>\n"
"<div><label>Порт</label><input id=\"l_port\" type=\"number\"></div></div>\n"
"<div class=\"row\"><button onclick=\"saveWol()\">Сохранить</button></div></div>\n"
"<div class=\"card\"><h2>Прочее</h2>\n"
"<div class=\"row\"><label class=\"chk\"><input type=\"checkbox\" id=\"s_stats\" onchange=\"saveStats()\">анонимная статистика</label></div>\n"
"<div class=\"grid\" style=\"margin-top:8px\"><div><label>Пароль веб-интерфейса</label><input id=\"sec_pass\" type=\"password\" placeholder=\"мин. 8 символов\"></div></div>\n"
"<div class=\"row\"><button onclick=\"saveWebPass()\">Сменить пароль</button>\n"
"<button class=\"gray\" onclick=\"reboot()\">Перезагрузить</button>\n"
"<button class=\"red\" onclick=\"factory()\">Сброс настроек</button></div></div>\n"
"</div>\n"
"<script>\n"
"async function api(p,b){const r=await fetch(p,b?{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)}:{});if(!r.ok)throw new Error(await r.text()||r.status);return r.json()}\n"
"function msg(t,e){const m=document.getElementById('msg');m.textContent=t;m.className=e?'err':'ok';if(!e)setTimeout(()=>{m.className=''},4000)}\n"
"let modeShown=false,busy=false,setupInit=false;\n"
"async function refresh(){if(busy)return;busy=true;try{const s=await api('/api/status');\n"
"document.getElementById('fw').textContent='v'+s.fw+' · '+s.target;\n"
"document.getElementById('setup').style.display=s.setup?'block':'none';\n"
"document.getElementById('panel').style.display=s.setup?'none':'block';\n"
"if(s.setup){if(!document.getElementById('p_ssid').value)document.getElementById('p_ssid').value=s.wifi||'';\n"
"if(!setupInit){setupInit=true;document.getElementById('p_admin').value=s.admin;\n"
"document.getElementById('p_mac').value=s.mac;document.getElementById('p_bcast').value=s.broadcast;document.getElementById('p_port').value=s.port}}\n"
"if(!s.setup){document.getElementById('status').innerHTML=''+\n"
"'<b>Wi-Fi</b><span>'+s.wifi+' ('+s.rssi+' dBm)</span>'+'<b>IP</b><span>'+s.ip+'</span>'+\n"
"'<b>Обход</b><span>'+s.mode+' ttl '+s.ttl+' fool 0x'+s.fool.toString(16)+' ('+(s.manual?'ручной':'авто')+')</span>'+\n"
"'<b>Стратегия</b><span>'+s.strategy+'</span>'+'<b>Пробы</b><span>'+s.probes+' (fails '+s.fails+', scans '+s.scans+')</span>'+\n"
"'<b>Telegram</b><span>'+s.tg+' (HTTP '+s.http+')</span>'+'<b>Heap</b><span>'+s.heap+' B, uptime '+Math.floor(s.uptime/60)+' мин</span>';\n"
"if(!modeShown){modeShown=true;document.getElementById('d_mode').value=s.mode;document.getElementById('d_ttl').value=s.ttl;\n"
"for(const c of document.querySelectorAll('#d_fool input'))c.checked=(s.fool&+c.value)!==0;\n"
"document.getElementById('w_ssid').value=s.wifi;document.getElementById('b_admin').value=s.admin;\n"
"document.getElementById('l_mac').value=s.mac;document.getElementById('l_bcast').value=s.broadcast;document.getElementById('l_port').value=s.port}\n"
"document.getElementById('s_stats').checked=s.stats;}}catch(e){}finally{busy=false}}\n"
"async function loadWifi(){try{const l=await api('/api/wifi/scan');const sel=document.getElementById('wifi_list');\n"
"sel.innerHTML='<option value=\"\">— выберите сеть —</option>'+l.map(a=>'<option>'+a.ssid+'</option>').join('');\n"
"sel.onchange=()=>{document.getElementById('p_ssid').value=sel.value};msg('найдено сетей: '+l.length)}catch(e){msg('скан не удался: '+e,1)}}\n"
"function foolMask(){let m=0;for(const c of document.querySelectorAll('#d_fool input'))if(c.checked)m|=+c.value;return m}\n"
"async function provision(){try{await api('/api/provision',{ssid:document.getElementById('p_ssid').value.trim(),pass:document.getElementById('p_pass').value,token:document.getElementById('p_token').value.trim(),admin:+document.getElementById('p_admin').value||0,mac:document.getElementById('p_mac').value.trim(),broadcast:document.getElementById('p_bcast').value.trim(),port:+document.getElementById('p_port').value||9,webpass:document.getElementById('p_webpass').value||undefined});msg('сохранено, перезагрузка...')}catch(e){msg('ошибка: '+e,1)}}\n"
"async function applyDesync(){try{await api('/api/desync',{mode:document.getElementById('d_mode').value,ttl:+document.getElementById('d_ttl').value,fool:foolMask()});msg('применено')}catch(e){msg('ошибка: '+e,1)}}\n"
"async function runScan(){try{const r=await api('/api/scan',{});msg(r.running?'автоподбор уже идёт':'автоподбор запущен, обновите статус через минуту')}catch(e){msg('ошибка: '+e,1)}}\n"
"async function saveWifi(){try{await api('/api/wifi',{ssid:document.getElementById('w_ssid').value.trim(),pass:document.getElementById('w_pass').value});msg('сохранено, перезагрузите устройство')}catch(e){msg('ошибка: '+e,1)}}\n"
"async function saveBot(){try{await api('/api/bot',{token:document.getElementById('b_token').value.trim(),admin:+document.getElementById('b_admin').value||0});msg('сохранено')}catch(e){msg('ошибка: '+e,1)}}\n"
"async function saveWol(){try{await api('/api/wol',{mac:document.getElementById('l_mac').value.trim(),broadcast:document.getElementById('l_bcast').value.trim(),port:+document.getElementById('l_port').value||9});msg('сохранено')}catch(e){msg('ошибка: '+e,1)}}\n"
"async function saveStats(){try{await api('/api/stats',{on:document.getElementById('s_stats').checked});msg('статистика обновлена')}catch(e){msg('ошибка: '+e,1)}}\n"
"async function saveWebPass(){try{await api('/api/webpass',{pass:document.getElementById('sec_pass').value});msg('пароль сохранён')}catch(e){msg('ошибка: '+e,1)}}\n"
"async function reboot(){if(confirm('Перезагрузить устройство?')){try{await api('/api/reboot',{});msg('перезагрузка...')}catch(e){msg('ошибка: '+e,1)}}}\n"
"async function factory(){if(confirm('Сбросить ВСЕ настройки и включить режим настройки?')){try{await api('/api/factory',{});msg('сброшено, перезагрузка...')}catch(e){msg('ошибка: '+e,1)}}}\n"
"refresh();setInterval(refresh,5000);\n"
"</script></main></body></html>\n";

static esp_err_t h_index(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    const app_settings_t *cfg = app_settings_get();
    esp_desync_config_t dc;
    esp_desync_get_config(&dc);
    scan_status_t st;
    scan_get_status(&st);

    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    esp_wifi_sta_get_ap_info(&ap);

    char ip[16] = "-";
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "setup", s_setup);
    cJSON_AddBoolToObject(r, "provisioned", app_settings_is_provisioned());
    cJSON_AddStringToObject(r, "fw", FW_VERSION);
    cJSON_AddStringToObject(r, "target", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(r, "wifi", ap.ssid[0] ? (const char *)ap.ssid : cfg->wifi_ssid);
    cJSON_AddNumberToObject(r, "rssi", ap.ssid[0] ? ap.rssi : 0);
    cJSON_AddStringToObject(r, "ip", ip);
    cJSON_AddNumberToObject(r, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(r, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(r, "reset", (int)esp_reset_reason());
    cJSON_AddStringToObject(r, "mode", esp_desync_mode_name(dc.mode));
    cJSON_AddNumberToObject(r, "ttl", dc.fake_ttl);
    cJSON_AddNumberToObject(r, "fool", dc.fooling);
    cJSON_AddBoolToObject(r, "manual", st.manual);
    cJSON_AddStringToObject(r, "strategy", st.have ? st.strategy : "");
    cJSON_AddNumberToObject(r, "scans", st.scans);
    cJSON_AddNumberToObject(r, "probes", st.probes);
    cJSON_AddNumberToObject(r, "fails", st.probe_fails);
    cJSON_AddBoolToObject(r, "stats", stats_anon_enabled());
    cJSON_AddStringToObject(r, "tg", tg_last_endpoint());
    cJSON_AddNumberToObject(r, "http", tg_last_http_status());
    cJSON_AddNumberToObject(r, "admin", (double)cfg->tg_admin_id);
    cJSON_AddStringToObject(r, "mac", cfg->wol_mac);
    cJSON_AddStringToObject(r, "broadcast", cfg->wol_broadcast);
    cJSON_AddNumberToObject(r, "port", cfg->wol_port);
    return send_json(req, r);
}

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return bad_request(req, "scan failed");
    }
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        esp_wifi_scan_start(&sc, true);
        esp_wifi_scan_get_ap_num(&count);
    }
    if (count > 32) {
        count = 32;
    }
    wifi_ap_record_t *recs = calloc(count ? count : 1, sizeof(*recs));
    if (recs == NULL) {
        return bad_request(req, "oom");
    }
    uint16_t got = count;
    esp_wifi_scan_get_ap_records(&got, recs);

    cJSON *arr = cJSON_CreateArray();
    const char *prefix = CONFIG_APP_SETUP_AP_PREFIX;
    size_t prefix_len = strlen(prefix);
    for (uint16_t i = 0; i < got; i++) {
        if (recs[i].ssid[0] == 0) {
            continue;
        }
        if (strncmp((const char *)recs[i].ssid, prefix, prefix_len) == 0) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(o, "auth", recs[i].authmode);
        cJSON_AddItemToArray(arr, o);
    }
    free(recs);
    return send_json(req, arr);
}

static esp_err_t h_provision(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    cJSON *b = read_json(req);
    if (b == NULL) {
        return bad_request(req, "bad json");
    }
    const char *ssid = jstr(b, "ssid", NULL);
    const char *webpass = jstr(b, "webpass", NULL);
    esp_err_t err = ESP_OK;

    if (ssid == NULL || ssid[0] == 0) {
        cJSON_Delete(b);
        return bad_request(req, "ssid required");
    }
    if (webpass != NULL && webpass[0] != 0) {
        err = app_settings_set_web_pass(webpass);
        if (err != ESP_OK) {
            cJSON_Delete(b);
            return bad_request(req, "web password must be >= 8 chars");
        }
    }
    app_settings_set_wifi(ssid, jstr(b, "pass", ""));
    const char *token = jstr(b, "token", NULL);
    if (token != NULL && token[0] != 0) {
        app_settings_set_token(token);
    }
    app_settings_set_admin_id(jint(b, "admin", 0));
    const char *mac = jstr(b, "mac", NULL);
    if (mac != NULL && mac[0] != 0) {
        app_settings_set_wol(mac, jstr(b, "broadcast", "255.255.255.255"),
                             (uint16_t)jint(b, "port", 9));
    }
    cJSON_Delete(b);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    esp_err_t send_err = send_json(req, r);
    schedule_reboot(1200);
    return send_err;
}

static esp_err_t h_wifi_save(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    cJSON *b = read_json(req);
    if (b == NULL) {
        return bad_request(req, "bad json");
    }
    const char *ssid = jstr(b, "ssid", NULL);
    if (ssid == NULL || ssid[0] == 0) {
        cJSON_Delete(b);
        return bad_request(req, "ssid required");
    }
    app_settings_set_wifi(ssid, jstr(b, "pass", ""));
    cJSON_Delete(b);
    return send_json(req, cJSON_CreateObject());
}

static esp_err_t h_bot_save(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    cJSON *b = read_json(req);
    if (b == NULL) {
        return bad_request(req, "bad json");
    }
    const char *token = jstr(b, "token", NULL);
    if (token != NULL && token[0] != 0) {
        app_settings_set_token(token);
    }
    app_settings_set_admin_id(jint(b, "admin", 0));
    cJSON_Delete(b);
    return send_json(req, cJSON_CreateObject());
}

static esp_err_t h_wol_save(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    cJSON *b = read_json(req);
    if (b == NULL) {
        return bad_request(req, "bad json");
    }
    const char *mac = jstr(b, "mac", NULL);
    if (mac == NULL || mac[0] == 0) {
        cJSON_Delete(b);
        return bad_request(req, "mac required");
    }
    app_settings_set_wol(mac, jstr(b, "broadcast", "255.255.255.255"),
                         (uint16_t)jint(b, "port", 9));
    cJSON_Delete(b);
    return send_json(req, cJSON_CreateObject());
}

static esp_err_t h_webpass(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    cJSON *b = read_json(req);
    if (b == NULL) {
        return bad_request(req, "bad json");
    }
    const char *pass = jstr(b, "pass", NULL);
    if (pass == NULL || app_settings_set_web_pass(pass) != ESP_OK) {
        cJSON_Delete(b);
        return bad_request(req, "password must be >= 8 chars");
    }
    cJSON_Delete(b);
    return send_json(req, cJSON_CreateObject());
}

static esp_err_t h_desync(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    cJSON *b = read_json(req);
    if (b == NULL) {
        return bad_request(req, "bad json");
    }
    esp_desync_config_t c;
    esp_desync_get_config(&c);

    const char *mode = jstr(b, "mode", NULL);
    if (mode != NULL && mode[0] != 0) {
        bool ok = false;
        esp_desync_mode_t m = esp_desync_mode_from_name(mode, &ok);
        if (!ok) {
            cJSON_Delete(b);
            return bad_request(req, "unknown mode");
        }
        c.mode = m;
    }
    int ttl = jint(b, "ttl", -1);
    if (ttl >= 1 && ttl <= 255) {
        c.fake_ttl = (uint8_t)ttl;
    }
    const cJSON *fool = cJSON_GetObjectItemCaseSensitive(b, "fool");
    if (cJSON_IsNumber(fool) && fool->valueint >= 0) {
        c.fooling = (uint32_t)fool->valueint;
    }
    cJSON_Delete(b);

    esp_desync_set_config(&c);
    scan_set_manual(true);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "mode", esp_desync_mode_name(c.mode));
    cJSON_AddNumberToObject(r, "ttl", c.fake_ttl);
    cJSON_AddNumberToObject(r, "fool", c.fooling);
    return send_json(req, r);
}

static void scan_task(void *arg)
{
    (void)arg;
    scan_find_working();
    s_scan_running = false;
    vTaskDelete(NULL);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    cJSON *r = cJSON_CreateObject();
    if (s_scan_running) {
        cJSON_AddBoolToObject(r, "running", true);
        return send_json(req, r);
    }
    s_scan_running = true;
    if (xTaskCreate(scan_task, "web_scan", 8192, NULL, 5, NULL) != pdPASS) {
        s_scan_running = false;
        return bad_request(req, "no memory for scan task");
    }
    cJSON_AddBoolToObject(r, "running", false);
    cJSON_AddBoolToObject(r, "started", true);
    return send_json(req, r);
}

static esp_err_t h_stats(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    cJSON *b = read_json(req);
    if (b == NULL) {
        return bad_request(req, "bad json");
    }
    bool on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "on"));
    cJSON_Delete(b);
    stats_anon_set_enabled(on);
    if (on) {
        stats_anon_report("manual");
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "on", on);
    return send_json(req, r);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    esp_err_t err = send_json(req, cJSON_CreateObject());
    schedule_reboot(700);
    return err;
}

static esp_err_t h_factory(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return unauthorized(req);
    }
    app_settings_erase();
    scan_erase_saved();
    esp_err_t err = send_json(req, cJSON_CreateObject());
    schedule_reboot(900);
    return err;
}

static const httpd_uri_t URIS[] = {
    { .uri = "/",              .method = HTTP_GET,  .handler = h_index },
    { .uri = "/api/status",    .method = HTTP_GET,  .handler = h_status },
    { .uri = "/api/wifi/scan", .method = HTTP_GET,  .handler = h_wifi_scan },
    { .uri = "/api/provision", .method = HTTP_POST, .handler = h_provision },
    { .uri = "/api/wifi",      .method = HTTP_POST, .handler = h_wifi_save },
    { .uri = "/api/bot",       .method = HTTP_POST, .handler = h_bot_save },
    { .uri = "/api/wol",       .method = HTTP_POST, .handler = h_wol_save },
    { .uri = "/api/webpass",   .method = HTTP_POST, .handler = h_webpass },
    { .uri = "/api/desync",    .method = HTTP_POST, .handler = h_desync },
    { .uri = "/api/scan",      .method = HTTP_POST, .handler = h_scan },
    { .uri = "/api/stats",     .method = HTTP_POST, .handler = h_stats },
    { .uri = "/api/reboot",    .method = HTTP_POST, .handler = h_reboot },
    { .uri = "/api/factory",   .method = HTTP_POST, .handler = h_factory },
};

esp_err_t webui_start(bool setup_mode)
{
    s_setup = setup_mode;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = sizeof(URIS) / sizeof(URIS[0]) + 2;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        httpd_register_uri_handler(s_server, &URIS[i]);
    }
    ESP_LOGI(TAG, "web UI listening on port %d%s", cfg.server_port,
             setup_mode ? " (setup mode, no auth)" : " (Basic auth)");
    return ESP_OK;
}

bool webui_in_setup_mode(void)
{
    return s_setup;
}

#else /* CONFIG_APP_WEB_UI */

esp_err_t webui_start(bool setup_mode)
{
    (void)setup_mode;
    return ESP_ERR_NOT_SUPPORTED;
}

bool webui_in_setup_mode(void)
{
    return false;
}

#endif /* CONFIG_APP_WEB_UI */
