// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_desync.h"
#include "desync_tls.h"
#include "lwip/opt.h"
#include "lwip/err.h"
#include "lwip/tcpip.h"

#define DESYNC_SEG_MAX (40 + DESYNC_FAKE_MAX)

typedef struct {
    uint32_t dst_ip;   /* lwip/network byte order */
    uint16_t src_port; /* host order */
    uint16_t dst_port; /* host order */
} desync_flow_t;

/* TCP state introspection (snd_nxt / rcv_nxt of the socket's PCB) */
int desync_pcb_get_state(uint16_t lport, uint16_t rport, uint32_t *snd_nxt, uint32_t *rcv_nxt);

/* Raw TCP segment injection through the normal LwIP output path */
int desync_inject_tcp(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                      uint32_t seq, uint32_t ack, const uint8_t *payload, size_t payload_len,
                      uint32_t fooling, uint8_t ttl, int32_t badseq_offset);
