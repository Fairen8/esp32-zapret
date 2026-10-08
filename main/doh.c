// SPDX-License-Identifier: MIT
#include <stdio.h>
#include "esp_log.h"
#include "https_client.h"
#include "doh.h"
#include "doh_parse.h"

static const char *TAG = "doh";

/* Telemetry flags: bit0 = attempted, bit1 = succeeded (see doh.h). */
static uint8_t s_telemetry;

uint8_t doh_get_flags(void)
{
    return s_telemetry;
}

typedef struct {
    const char *sni;  /* TLS hostname / Host header */
    uint32_t ip_be;   /* network byte order */
} doh_server_t;

static const doh_server_t DOH_SERVERS[] = {
    { "cloudflare-dns.com", 0x01010101u }, /* 1.1.1.1 */
    { "dns.google",         0x08080808u }, /* 8.8.8.8 */
};

int doh_resolve(const char *host, uint32_t *addrs_be, int max_addrs)
{
    static char resp[2048];
    char path[192];

    s_telemetry |= DOH_FLAG_TRIED;
    snprintf(path, sizeof(path), "/dns-query?name=%s&type=A", host);

    for (size_t i = 0; i < sizeof(DOH_SERVERS) / sizeof(DOH_SERVERS[0]); i++) {
        const doh_server_t *srv = &DOH_SERVERS[i];
        https_conn_t conn;

        if (https_connect(&conn, srv->sni, &srv->ip_be, 1, 443, 4000, 8000, false) != 0) {
            continue;
        }
        int status = 0;
        int n = https_request(&conn, path, "application/dns-json", resp, sizeof(resp), 10000, &status);
        https_close(&conn);

        if (n > 0 && status == 200) {
            int cnt = doh_parse_a_records(resp, addrs_be, max_addrs);
            if (cnt > 0) {
                s_telemetry |= DOH_FLAG_OK;
                ESP_LOGI(TAG, "%s -> %d address(es)", srv->sni, cnt);
                return cnt;
            }
        }
        ESP_LOGW(TAG, "%s failed (http %d, %d bytes)", srv->sni, status, n);
    }

    ESP_LOGW(TAG, "DoH resolution failed for %s", host);
    return 0;
}
