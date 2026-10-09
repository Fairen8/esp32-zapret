// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

#define NET_SCAN_MAX_HOSTS 12

typedef struct {
    uint8_t mac[6];
    uint32_t ip_be; /* network byte order */
} net_host_t;

/* ARP-scans the local subnet (up to 254 hosts) and returns the stable ARP
 * entries found, gateway excluded. Blocking, takes ~2-3 seconds. Only devices
 * that are awake answer ARP (a sleeping PC has to be added manually). */
int net_scan_arp(net_host_t *out, int max);
