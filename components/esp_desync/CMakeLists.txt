# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS
        "esp_desync.c"
        "desync_hosts.c"
        "desync_tls.c"
        "desync_inject.c"
        "desync_pcb.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        lwip
)
