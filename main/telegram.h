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
    /* Inline-keyboard callback (button press). */
    bool is_callback;
    char callback_id[48];
    char callback_data[64];
    int64_t message_id;
} tg_update_t;

/* Returns one of TG_RC_*. */
int tg_get_updates(tg_update_t *out, int64_t offset, int long_poll_s);

/* TLS connectivity self-test (no bot token required): returns 0 on success. */
int tg_selftest(void);

/* Lightweight connect+TLS probe used by the strategy scanner. */
int tg_probe(int timeout_ms);

/* Same, but reports the failure stage (HTTPS_STAGE_* from https_client.h)
 * for the scanner telemetry. */
int tg_probe_ex(int timeout_ms, int *stage);

/* Sends a message (HTML parse mode). Returns: response length (>0) or -1. */
int tg_send_message(int64_t chat_id, const char *text);

/* Sends a message with an inline keyboard (reply_markup JSON object). */
int tg_send_menu(int64_t chat_id, const char *text, const char *reply_markup);

/* Edits a message sent by the bot (keeps the keyboard up to date).
 * Returns 0 on success (incl. "message is not modified"), -1 on failure. */
int tg_edit_menu(int64_t chat_id, int64_t message_id, const char *text,
                 const char *reply_markup);

/* Stops the button spinner; text may be NULL. Best effort. */
void tg_answer_callback(const char *callback_id, const char *text);

/* Diagnostics: last endpoint the request actually used and last HTTP status. */
const char *tg_last_endpoint(void);
int tg_last_http_status(void);

#ifdef __cplusplus
}
#endif
