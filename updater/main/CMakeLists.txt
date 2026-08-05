idf_component_register(
    SRCS
        "updater_main.c"
        "updater_preflight.c"
        "recovery_network.c"
        "updater_power.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        bootloader_support
        driver
        esp_app_format
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        hw1_ota_protocol
        json
        joltwallet__littlefs
        mbedtls
        nvs_flash
    EMBED_TXTFILES
        "${_ota_public_key}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Werror
)
