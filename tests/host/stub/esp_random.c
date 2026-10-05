// SPDX-License-Identifier: MIT
#include "esp_random.h"

static uint32_t s_state = 0x12345678u;

uint32_t esp_random(void)
{
    s_state = s_state * 1103515245u + 12345u;
    return (s_state >> 8) ^ (s_state << 13);
}
