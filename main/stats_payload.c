// SPDX-License-Identifier: MIT
#include <stdio.h>
#include "stats_payload.h"

#define STATS_SCHEMA 2

int stats_build_payload(const stats_payload_t *in, char *buf, size_t cap)
{
    if (in == NULL || buf == NULL || cap == 0) {
        return -1;
    }

    int n = snprintf(buf, cap,
                     "{\"schema\":%d,\"event\":\"%s\",\"fw\":\"%s\",\"target\":\"%s\","
                     "\"uptime_s\":%u,\"heap\":%u,\"min_heap\":%u,\"max_block\":%u,"
                     "\"rssi\":%d,\"reset\":%u,"
                     "\"mode\":%u,\"fool\":%u,\"ttl\":%u,\"split1\":%d,\"split2\":%d,"
                     "\"have\":%s,\"strategy\":\"%s\","
                     "\"scans\":%u,\"probes\":%u,\"fails\":%u,"
                     "\"tried\":\"%s\",\"tts_s\":%u,\"strategy_changed\":%u,"
                     "\"wifi_disc\":%u,\"wifi_reason\":%u,\"boot_storm\":%s,"
                     "\"temp_c\":%d,\"errs\":\"%s\","
                     "\"mtu\":%u,\"sntp_s\":%u,\"sntp_ok\":%s,"
                     "\"doh\":%u,\"ch\":%u,\"stage\":%u,\"cfg\":%u,"
                     "\"cpu_mhz\":%u,\"flash_free\":%u,\"psram\":%s,"
                     "\"chip_rev\":%u,\"sni_cat\":%u,\"rid\":\"%s\"}",
                     STATS_SCHEMA,
                     in->event ? in->event : "",
                     in->fw ? in->fw : "",
                     in->target ? in->target : "",
                     (unsigned)in->uptime_s,
                     (unsigned)in->free_heap,
                     (unsigned)in->min_heap,
                     (unsigned)in->max_block,
                     (int)in->rssi,
                     (unsigned)in->reset_reason,
                     (unsigned)in->mode,
                     (unsigned)in->fooling,
                     (unsigned)in->ttl,
                     (int)in->split1,
                     (int)in->split2,
                     in->have_strategy ? "true" : "false",
                     in->strategy ? in->strategy : "",
                     (unsigned)in->scans,
                     (unsigned)in->probes,
                     (unsigned)in->probe_fails,
                     in->tried ? in->tried : "",
                     (unsigned)in->tts_s,
                     (unsigned)in->strategy_changed,
                     (unsigned)in->wifi_disc,
                     (unsigned)in->wifi_reason,
                     in->boot_storm ? "true" : "false",
                     (int)in->temp_c,
                     in->errs ? in->errs : "",
                     (unsigned)in->mtu,
                     (unsigned)in->sntp_s,
                     in->sntp_ok ? "true" : "false",
                     (unsigned)in->doh,
                     (unsigned)in->channel,
                     (unsigned)in->stage,
                     (unsigned)in->cfg,
                     (unsigned)in->cpu_mhz,
                     (unsigned)in->flash_free,
                     in->psram ? "true" : "false",
                     (unsigned)in->chip_rev,
                     (unsigned)in->sni_cat,
                     in->rid ? in->rid : "");
    if (n <= 0 || n >= (int)cap) {
        return -1;
    }
    return n;
}
