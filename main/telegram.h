// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TG_RC_UPDATE      1
#define TG_RC_NO_UPDATES  0
#define TG_RC_TRANSPORT  -1
#define TG_RC_HTTP       -2

typedef struct {
    bool valid;
    int64_t update_id;
    int64_t chat_id;
    char text[192];
} tg_update_t;

/* Returns one of TG_RC_*. */
int tg_get_updates(tg_update_t *out, int64_t offset, int long_poll_s);

/* TLS connectivity self-test (no bot token required): returns 0 on success. */
int tg_selftest(void);

/* Lightweight connect+TLS probe used by the strategy scanner. */
int tg_probe(int timeout_ms);

/* Returns: response length (>0) or -1 on error */
int tg_send_message(int64_t chat_id, const char *text);

/* Diagnostics: last endpoint the request actually used and last HTTP status. */
const char *tg_last_endpoint(void);
int tg_last_http_status(void);

#ifdef __cplusplus
}
#endif
