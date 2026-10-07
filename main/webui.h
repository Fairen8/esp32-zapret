// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Starts the HTTP server. In setup mode no HTTP auth is required (the setup
 * access point itself is password protected); in normal mode every request
 * needs HTTP Basic auth (user "admin", password = web password). */
esp_err_t webui_start(bool setup_mode);

bool webui_in_setup_mode(void);
