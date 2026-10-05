# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS
        "main.c"
        "net_utils.c"
        "telegram.c"
        "wol.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_desync
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        mbedtls
)
