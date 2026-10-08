# SPDX-License-Identifier: MIT

# Firmware version: single source of truth is the repo-root VERSION file.
set(_version_file "${CMAKE_CURRENT_LIST_DIR}/../VERSION")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_version_file}")
file(READ "${_version_file}" _fw_version)
string(STRIP "${_fw_version}" _fw_version)

idf_component_register(
    SRCS
        "main.c"
        "app_settings.c"
        "desync_scan.c"
        "setup_mode.c"
        "webui.c"
        "doh.c"
        "doh_parse.c"
        "https_client.c"
        "net_utils.c"
        "scan_candidates.c"
        "stats.c"
        "stats_payload.c"
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
        esp_app_format
        esp_http_server
        console
        json
        mbedtls
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_VERSION="${_fw_version}")
