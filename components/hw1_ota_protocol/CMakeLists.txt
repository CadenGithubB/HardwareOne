set(_hw1_ota_embed_args)
if(DEFINED HW1_OTA_PUBLIC_KEY_FILE AND
   NOT "${HW1_OTA_PUBLIC_KEY_FILE}" STREQUAL "")
    list(APPEND _hw1_ota_embed_args
        EMBED_TXTFILES "${HW1_OTA_PUBLIC_KEY_FILE}")
endif()

idf_component_register(
    SRCS
        "hw1_ota_protocol.c"
        "hw1_ota_nvs.c"
        "hw1_ota_idf.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        bootloader_support
        esp_partition
        mbedtls
        nvs_flash
    ${_hw1_ota_embed_args}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Werror
)
