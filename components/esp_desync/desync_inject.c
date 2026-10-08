// SPDX-License-Identifier: MIT
#include <string.h>
#include <stdlib.h>
#include "desync_internal.h"
#include "lwip/pbuf.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/err.h"
#include "lwip/prot/ip.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TCP_HLEN_BASE   20
#define TCP_OPT_MD5_LEN 20
#define TCP_OPT_TS_LEN  12

static uint32_t cksum_add(const uint8_t *d, size_t len, uint32_t sum)
{
    while (len > 1) {
        sum += (uint32_t)((d[0] << 8) | d[1]);
        d += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint32_t)(d[0] << 8);
    }
    return sum;
}

static uint16_t cksum_finish(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

typedef struct {
    SemaphoreHandle_t done;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t ttl;
    uint8_t tcp_hlen;
    bool md5sig;
    bool badsum;
    bool datanoack;
    bool ts;
    uint16_t payload_len;
    uint8_t payload[DESYNC_FAKE_MAX];
    uint8_t seg[DESYNC_SEG_MAX];
    int result;
} inject_arg_t;

static void inject_cb(void *arg)
{
    inject_arg_t *a = (inject_arg_t *)arg;
    a->result = -1;

    struct netif *netif = netif_default;
    if (netif == NULL || !netif_is_up(netif)) {
        xSemaphoreGive(a->done);
        return;
    }

    const ip4_addr_t *src = netif_ip4_addr(netif);
    uint8_t *seg = a->seg;
    uint16_t hlen = a->tcp_hlen;
    uint16_t total = (uint16_t)(hlen + a->payload_len);

    memset(seg, 0, hlen);
    seg[0] = (uint8_t)(a->src_port >> 8);
    seg[1] = (uint8_t)(a->src_port & 0xff);
    seg[2] = (uint8_t)(a->dst_port >> 8);
    seg[3] = (uint8_t)(a->dst_port & 0xff);
    seg[4] = (uint8_t)(a->seq >> 24);
    seg[5] = (uint8_t)(a->seq >> 16);
    seg[6] = (uint8_t)(a->seq >> 8);
    seg[7] = (uint8_t)(a->seq & 0xff);
    seg[8] = (uint8_t)(a->ack >> 24);
    seg[9] = (uint8_t)(a->ack >> 16);
    seg[10] = (uint8_t)(a->ack >> 8);
    seg[11] = (uint8_t)(a->ack & 0xff);
    seg[12] = (uint8_t)((hlen / 4) << 4);
    seg[13] = (uint8_t)(a->datanoack ? 0x08 : 0x18);
    seg[14] = 0xff;
    seg[15] = 0xff;

    if (a->md5sig) {
        seg[20] = 19;
        seg[21] = 18;
        for (int i = 0; i < 16; i += 4) {
            uint32_t r = esp_random();
            memcpy(seg + 22 + i, &r, 4);
        }
        seg[38] = 1;
        seg[39] = 1;
    }

    uint16_t opt = TCP_HLEN_BASE + (a->md5sig ? TCP_OPT_MD5_LEN : 0);
    if (a->ts) {
        seg[opt + 0] = 8;  /* TCP option: timestamps */
        seg[opt + 1] = 10;
        uint32_t tsval = esp_random();
        seg[opt + 2] = (uint8_t)(tsval >> 24);
        seg[opt + 3] = (uint8_t)(tsval >> 16);
        seg[opt + 4] = (uint8_t)(tsval >> 8);
        seg[opt + 5] = (uint8_t)(tsval & 0xff);
        seg[opt + 6] = (uint8_t)(a->ack >> 24);
        seg[opt + 7] = (uint8_t)(a->ack >> 16);
        seg[opt + 8] = (uint8_t)(a->ack >> 8);
        seg[opt + 9] = (uint8_t)(a->ack & 0xff);
        seg[opt + 10] = 1; /* NOP padding to a 4-byte boundary */
        seg[opt + 11] = 1;
    }

    memcpy(seg + hlen, a->payload, a->payload_len);

    uint32_t sum = 0;
    sum = cksum_add((const uint8_t *)src, 4, sum);
    sum = cksum_add((const uint8_t *)&a->dst_ip, 4, sum);
    sum += IP_PROTO_TCP;
    sum += total;
    sum = cksum_add(seg, total, sum);

    uint16_t ck = cksum_finish(sum);
    if (a->badsum) {
        ck = (uint16_t)(0xB000u | (esp_random() & 0x0fffu));
    }
    seg[16] = (uint8_t)(ck >> 8);
    seg[17] = (uint8_t)(ck & 0xff);

    struct pbuf *p = pbuf_alloc(PBUF_IP, total, PBUF_RAM);
    if (p != NULL) {
        memcpy(p->payload, seg, total);
        err_t err = ip4_output_if(p, src, (const ip4_addr_t *)&a->dst_ip,
                                  a->ttl, 0, IP_PROTO_TCP, netif);
        pbuf_free(p);
        a->result = (err == ERR_OK) ? 0 : -1;
    }

    xSemaphoreGive(a->done);
}

int desync_inject_tcp(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                      uint32_t seq, uint32_t ack, const uint8_t *payload, size_t payload_len,
                      uint32_t fooling, uint8_t ttl, int32_t badseq_offset)
{
    if (payload_len > DESYNC_FAKE_MAX) {
        return -1;
    }

    inject_arg_t *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return -1;
    }
    a->dst_ip = dst_ip;
    a->src_port = src_port;
    a->dst_port = dst_port;
    a->seq = seq;
    if (fooling & ESP_DESYNC_FOOL_BADSEQ) {
        a->seq = (uint32_t)(a->seq + (uint32_t)badseq_offset);
    }
    a->ack = ack;
    a->ttl = (fooling & ESP_DESYNC_FOOL_TTL) ? ttl : 64;
    a->md5sig = (fooling & ESP_DESYNC_FOOL_MD5SIG) != 0;
    a->badsum = (fooling & ESP_DESYNC_FOOL_BADSUM) != 0;
    a->datanoack = (fooling & ESP_DESYNC_FOOL_DATANOACK) != 0;
    a->ts = (fooling & ESP_DESYNC_FOOL_TS) != 0;
    a->tcp_hlen = TCP_HLEN_BASE + (a->md5sig ? TCP_OPT_MD5_LEN : 0) +
                  (a->ts ? TCP_OPT_TS_LEN : 0);
    a->payload_len = (uint16_t)payload_len;
    memcpy(a->payload, payload, payload_len);

    a->done = xSemaphoreCreateBinary();
    if (a->done == NULL) {
        free(a);
        return -1;
    }

    if (tcpip_callback(inject_cb, a) != ERR_OK) {
        vSemaphoreDelete(a->done);
        free(a);
        return -1;
    }

    /* The argument lives on the heap and is freed only after the callback
     * signals completion. Waiting without a timeout is safe (the callback
     * runs in the tcpip task, which this wait does not block) and rules out
     * a use-after-free when the callback is queued for a long time. */
    xSemaphoreTake(a->done, portMAX_DELAY);

    int result = a->result;
    vSemaphoreDelete(a->done);
    free(a);
    return result;
}
