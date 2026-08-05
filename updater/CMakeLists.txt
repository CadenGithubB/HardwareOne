cmake_minimum_required(VERSION 3.16)

# Standalone, pure ESP-IDF factory recovery application. It intentionally does
# not link Arduino or the HardwareOne application component.
set(IDF_TARGET "esp32s3")
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/hw1_ota_protocol"
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/joltwallet__littlefs")

set(_hw_board "$ENV{HW_BOARD}")
if(NOT _hw_board)
    set(_hw_board "feathers3")
endif()

set(_board_defaults "${CMAKE_CURRENT_LIST_DIR}/boards/${_hw_board}.defaults")
if(NOT EXISTS "${_board_defaults}")
    message(FATAL_ERROR
        "Unsupported updater HW_BOARD='${_hw_board}'. Supported boards: "
        "feathers3, feathers3_fe")
endif()

if(_hw_board STREQUAL "feathers3")
    set(_layout_id "hw1-f3-ota-v1")
    set(_version_suffix "f3o1")
elseif(_hw_board STREQUAL "feathers3_fe")
    set(_layout_id "hw1-f3fe-ota-v1")
    set(_version_suffix "f3feo1")
endif()
set(_project_version "1.0.0+${_version_suffix}")

# A deployable updater and every accepted main image use the same RSA-3072
# private key. Only the derived public key is embedded in the updater.
if(DEFINED ENV{HW1_OTA_SIGNING_KEY} AND
   NOT "$ENV{HW1_OTA_SIGNING_KEY}" STREQUAL "")
    get_filename_component(_ota_signing_key
        "$ENV{HW1_OTA_SIGNING_KEY}" ABSOLUTE)
else()
    set(_ota_signing_key "${CMAKE_CURRENT_LIST_DIR}/ota_signing_key.pem")
endif()
if(NOT EXISTS "${_ota_signing_key}")
    message(FATAL_ERROR
        "Updater builds require an RSA-3072 signing key. Set "
        "HW1_OTA_SIGNING_KEY or create ignored updater/ota_signing_key.pem. "
        "Never commit the private key. Missing: ${_ota_signing_key}")
endif()

find_program(_ota_openssl openssl)
if(NOT _ota_openssl)
    message(FATAL_ERROR "openssl is required for updater builds")
endif()
execute_process(
    COMMAND "${_ota_openssl}" pkey -in "${_ota_signing_key}" -text -noout
    RESULT_VARIABLE _ota_key_check_result
    OUTPUT_VARIABLE _ota_key_description
    ERROR_VARIABLE _ota_key_check_error)
if(NOT _ota_key_check_result EQUAL 0 OR
   NOT _ota_key_description MATCHES "3072 bit")
    message(FATAL_ERROR
        "HW1_OTA_SIGNING_KEY must be a readable RSA-3072 private key: "
        "${_ota_key_check_error}")
endif()

set(_ota_public_key "${CMAKE_BINARY_DIR}/hw1_ota_public_key.pem")
execute_process(
    COMMAND "${_ota_openssl}" pkey -in "${_ota_signing_key}" -pubout
            -out "${_ota_public_key}"
    RESULT_VARIABLE _ota_public_key_result
    ERROR_VARIABLE _ota_public_key_error)
if(NOT _ota_public_key_result EQUAL 0)
    message(FATAL_ERROR
        "Could not derive the OTA public key: ${_ota_public_key_error}")
endif()

set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${_hw_board}")
# Kconfig defaults never override an existing sdkconfig value. Remove only the
# workstation-specific key path so this invocation's HW1_OTA_SIGNING_KEY is
# authoritative for both app signing and the embedded manifest public key.
if(EXISTS "${SDKCONFIG}")
    file(READ "${SDKCONFIG}" _updater_sdkconfig_contents)
    set(_updater_sdkconfig_original "${_updater_sdkconfig_contents}")
    string(REGEX REPLACE
        "(^|\n)CONFIG_SECURE_BOOT_SIGNING_KEY=[^\n]*"
        "\\1" _updater_sdkconfig_contents "${_updater_sdkconfig_contents}")
    string(REGEX REPLACE
        "(^|\n)# CONFIG_SECURE_BOOT_SIGNING_KEY is not set"
        "\\1" _updater_sdkconfig_contents "${_updater_sdkconfig_contents}")
    if(NOT _updater_sdkconfig_contents STREQUAL _updater_sdkconfig_original)
        file(WRITE "${SDKCONFIG}" "${_updater_sdkconfig_contents}")
        message(STATUS
            "Cleared stale updater signing-key path; current HW1_OTA_SIGNING_KEY will apply")
    endif()
endif()
set(_ota_key_defaults "${CMAKE_BINARY_DIR}/hw1_ota_signing_key.defaults")
file(WRITE "${_ota_key_defaults}"
    "CONFIG_SECURE_BOOT_SIGNING_KEY=\"${_ota_signing_key}\"\n")
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.ota.defaults"
    "${_board_defaults}"
    "${_ota_key_defaults}")

add_compile_definitions(
    HW1_OTA_BOARD_ID="${_hw_board}"
    HW1_OTA_LAYOUT_ID="${_layout_id}"
    HW1_OTA_VERSION_SUFFIX="+${_version_suffix}"
    HW1_OTA_UPDATER_VERSION="${_project_version}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "${_project_version}")
project(hw1-updater)
