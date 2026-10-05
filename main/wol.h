// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int wol_send(const char *mac_str, const char *broadcast, uint16_t port);

#ifdef __cplusplus
}
#endif
