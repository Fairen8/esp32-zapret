// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "esp_log.h"
#include "net_utils.h"
#include "wol.h"

static const char *TAG = "wol";

int wol_send(const char *mac_str, const char *broadcast, uint16_t port)
{
    uint8_t mac[6];
    if (!net_parse_mac(mac_str, mac)) {
        ESP_LOGE(TAG, "bad MAC: %s", mac_str ? mac_str : "(null)");
        return -1;
    }

    uint8_t pkt[102];
    memset(pkt, 0xff, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(pkt + 6 + i * 6, mac, 6);
    }

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }

    int bc = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port ? port : 9);
    if (broadcast == NULL || inet_pton(AF_INET, broadcast, &dst.sin_addr) != 1) {
        dst.sin_addr.s_addr = htonl(0xffffffffUL);
    }

    int sent = 0;
    for (int i = 0; i < 3; i++) {
        if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst)) == (int)sizeof(pkt)) {
            sent++;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    close(fd);
    ESP_LOGI(TAG, "magic packet %s port %u sent=%d/3", mac_str, (unsigned)port, sent);
    return sent > 0 ? 0 : -1;
}
