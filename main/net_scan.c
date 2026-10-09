// SPDX-License-Identifier: MIT
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "esp_log.h"
#include "net_scan.h"

static const char *TAG = "netscan";

/* Copies new stable ARP entries (except skip_ip) into out[]. */
static void harvest(struct netif *nif, uint32_t skip_ip, net_host_t *out, int max, int *found)
{
    for (size_t i = 0; i < ARP_TABLE_SIZE && *found < max; i++) {
        ip4_addr_t *ip = NULL;
        struct netif *owner = NULL;
        struct eth_addr *eth = NULL;
        if (!etharp_get_entry(i, &ip, &owner, &eth) || ip == NULL || eth == NULL || owner != nif) {
            continue;
        }
        if (ip->addr == skip_ip) {
            continue;
        }
        bool dup = false;
        for (int k = 0; k < *found; k++) {
            if (out[k].ip_be == ip->addr) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        memcpy(out[*found].mac, eth->addr, 6);
        out[*found].ip_be = ip->addr;
        (*found)++;
    }
}

int net_scan_arp(net_host_t *out, int max)
{
    struct netif *nif = netif_default;
    if (nif == NULL || out == NULL || max <= 0) {
        return 0;
    }
    if (max > NET_SCAN_MAX_HOSTS) {
        max = NET_SCAN_MAX_HOSTS;
    }

    const ip4_addr_t *my = netif_ip4_addr(nif);
    const ip4_addr_t *mask = netif_ip4_netmask(nif);
    const ip4_addr_t *gw = netif_ip4_gw(nif);
    if (my == NULL || mask == NULL || my->addr == 0 || mask->addr == 0) {
        return 0;
    }

    uint32_t net = my->addr & mask->addr;
    uint32_t bcast = net | ~mask->addr;
    uint32_t my_ip = my->addr;
    uint32_t gw_ip = gw ? gw->addr : 0;

    int found = 0;
    uint32_t probes = 0;
    for (uint32_t a = net + 1; a < bcast && found < max && probes < 254; a++) {
        if (a == my_ip || a == gw_ip) {
            continue;
        }
        ip4_addr_t target;
        target.addr = a;
        etharp_request(nif, &target);
        probes++;
        if ((probes & 15) == 0) {
            vTaskDelay(pdMS_TO_TICKS(120));
            harvest(nif, gw_ip, out, max, &found);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(400));
    harvest(nif, gw_ip, out, max, &found);

    ESP_LOGI(TAG, "ARP scan: %u probes, %d host(s)", (unsigned)probes, found);
    return found;
}
