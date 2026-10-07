// SPDX-License-Identifier: MIT
#include <stdio.h>
#include "stats_payload.h"

#define STATS_SCHEMA 1

int stats_build_payload(const stats_payload_t *in, char *buf, size_t cap)
{
    if (in == NULL || buf == NULL || cap == 0) {
        return -1;
    }

    int n = snprintf(buf, cap,
                     "{\"schema\":%d,\"event\":\"%s\",\"fw\":\"%s\",\"target\":\"%s\","
                     "\"uptime_s\":%u,\"heap\":%u,\"rssi\":%d,\"reset\":%u,"
                     "\"mode\":%u,\"fool\":%u,\"ttl\":%u,\"split1\":%d,\"split2\":%d,"
                     "\"have\":%s,\"strategy\":\"%s\","
                     "\"scans\":%u,\"probes\":%u,\"fails\":%u}",
                     STATS_SCHEMA,
                     in->event ? in->event : "",
                     in->fw ? in->fw : "",
                     in->target ? in->target : "",
                     (unsigned)in->uptime_s,
                     (unsigned)in->free_heap,
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
                     (unsigned)in->probe_fails);
    if (n <= 0 || n >= (int)cap) {
        return -1;
    }
    return n;
}
