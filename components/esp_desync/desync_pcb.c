// SPDX-License-Identifier: MIT
#include <string.h>
#include "desync_internal.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcpip.h"
#include "lwip/err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    SemaphoreHandle_t done;
    uint16_t lport;
    uint16_t rport;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    bool found;
} pcb_query_t;

static void pcb_query_cb(void *arg)
{
    pcb_query_t *q = (pcb_query_t *)arg;

    for (struct tcp_pcb *pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
        if (pcb->local_port == q->lport && pcb->remote_port == q->rport) {
            q->snd_nxt = pcb->snd_nxt;
            q->rcv_nxt = pcb->rcv_nxt;
            q->found = true;
            break;
        }
    }

    xSemaphoreGive(q->done);
}

int desync_pcb_get_state(uint16_t lport, uint16_t rport, uint32_t *snd_nxt, uint32_t *rcv_nxt)
{
    pcb_query_t q;

    memset(&q, 0, sizeof(q));
    q.lport = lport;
    q.rport = rport;
    q.done = xSemaphoreCreateBinary();
    if (q.done == NULL) {
        return -1;
    }

    if (tcpip_callback(pcb_query_cb, &q) != ERR_OK) {
        vSemaphoreDelete(q.done);
        return -1;
    }

    if (xSemaphoreTake(q.done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        /* The callback may still be queued; leak the semaphore rather than
         * risk a use-after-free. */
        return -1;
    }
    vSemaphoreDelete(q.done);

    if (!q.found) {
        return -1;
    }

    *snd_nxt = q.snd_nxt;
    *rcv_nxt = q.rcv_nxt;
    return 0;
}
