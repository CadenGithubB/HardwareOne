cmake_minimum_required(VERSION 3.16)

# Standalone, pure ESP-IDF factory recovery application. It intentionally does
# not link Arduino or the HardwareOne application component. The updater's
# managed dependencies are declared by main/idf_component.yml; borrowing the
# root project's managed_components directory makes a clean updater-first build
# fail because that directory has not been populated yet.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/hw1_ota_protocol")

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/HW1Deployment.cmake")
set(_hw_deployment "$ENV{HW_DEPLOYMENT}")
hw1_load_deployment("${CMAKE_CURRENT_LIST_DIR}/.." "${_hw_deployment}")

set(_hw_board "$ENV{HW_BOARD}")
if(HW1_DEPLOYMENT_ACTIVE)
    if(_hw_board AND NOT _hw_board STREQUAL HW1_DEPLOYMENT_BOARD)
        message(FATAL_ERROR
            "HW_DEPLOYMENT=${_hw_deployment} is for board "
            "${HW1_DEPLOYMENT_BOARD}, but HW_BOARD=${_hw_board}")
    endif()
    set(_hw_board "${HW1_DEPLOYMENT_BOARD}")
endif()
if(NOT _hw_board)
    set(_hw_board "feathers3")
endif()

set(_board_defaults "${CMAKE_CURRENT_LIST_DIR}/boards/${_hw_board}.defaults")
if(NOT EXISTS "${_board_defaults}")
    file(GLOB _updater_boards
         RELATIVE "${CMAKE_CURRENT_LIST_DIR}/boards"
         "${CMAKE_CURRENT_LIST_DIR}/boards/*.defaults")
    list(TRANSFORM _updater_boards REPLACE "\\.defaults$" "")
    string(JOIN ", " _updater_boards_str ${_updater_boards})
    message(FATAL_ERROR
        "Unsupported updater HW_BOARD='${_hw_board}'. Supported boards: "
        "${_updater_boards_str}")
endif()

# The chip comes from the board file's marker rather than a literal, so the
# updater and the main application cannot be built for different targets from
# the same HW_BOARD. A hard-coded set(IDF_TARGET "esp32s3") here silently
# produced an S3 recovery image for any board name you passed.
file(STRINGS "${_board_defaults}" _updater_target_line REGEX "^# *HW_TARGET:")
if(NOT _updater_target_line)
    message(FATAL_ERROR
        "updater/boards/${_hw_board}.defaults has no '# HW_TARGET: <chip>' marker")
endif()
string(REGEX REPLACE "^# *HW_TARGET:[ \t]*([A-Za-z0-9]+).*" "\\1"
       _updater_target "${_updater_target_line}")
set(IDF_TARGET "${_updater_target}")

# Board registry. These identities are matched byte-for-byte against the main
# application's by tools/ota/check_ota_builds.py, and the strings themselves
# are declared once in components/hw1_ota_protocol/include/hw1_ota_protocol.h.
if(_hw_board STREQUAL "feathers3")
    set(_layout_id "hw1-f3-ota-v1")
    set(_version_suffix "f3o1")
elseif(_hw_board STREQUAL "feathers3_fe")
    set(_layout_id "hw1-f3fe-ota-v1")
    set(_version_suffix "f3feo1")
elseif(_hw_board STREQUAL "feather_esp32_v2")
    set(_layout_id "hw1-fv2-ota-v1")
    set(_version_suffix "fv2o1")
elseif(_hw_board STREQUAL "qtpy_esp32")
    set(_layout_id "hw1-qtpy-ota-v1")
    set(_version_suffix "qtpyo1")
elseif(_hw_board STREQUAL "xiao_s3")
    # Deployment-only board. Unlike every row above it, the XIAO has NO
    # board-only recovery layout: its identity and its partition geometry come
    # from a deployment contract, and HW1_DEPLOYMENT_ACTIVE overwrites both
    # values below.
    #
    # Refuse the bare build rather than assigning defaults. Without this the
    # partition selection further down would fall through to the shared
    # partitions_ota_no_sr_8mb.csv, whose ota_0 and littlefs sit at different
    # offsets from any XIAO deployment table -- producing an updater that
    # builds green, claims a layout id it does not implement, and writes
    # firmware to the wrong address on a provisioned device.
    if(NOT HW1_DEPLOYMENT_ACTIVE)
        message(FATAL_ERROR
            "HW_BOARD=xiao_s3 has no board-only recovery layout. Build its "
            "updater through a deployment, e.g. "
            "HW_DEPLOYMENT=pocket_assistant/xiao_s3, or use "
            "tools/build_deployment.sh pocket_assistant xiao_s3.")
    endif()
    set(_layout_id "${HW1_DEPLOYMENT_LAYOUT_ID}")
    set(_version_suffix "${HW1_DEPLOYMENT_VERSION_SUFFIX}")
else()
    message(FATAL_ERROR
        "updater/boards/${_hw_board}.defaults exists but the board has no "
        "layout-id/version-suffix row in this file. Add it here AND in the "
        "recovery-OTA registry in ../CMakeLists.txt; the two are compared by "
        "tools/ota/check_ota_builds.py.")
endif()
if(HW1_DEPLOYMENT_ACTIVE)
    if(NOT _updater_target STREQUAL HW1_DEPLOYMENT_TARGET)
        message(FATAL_ERROR
            "Deployment ${_hw_deployment} target ${HW1_DEPLOYMENT_TARGET} "
            "does not match updater board target ${_updater_target}")
    endif()
    set(_layout_id "${HW1_DEPLOYMENT_LAYOUT_ID}")
    set(_version_suffix "${HW1_DEPLOYMENT_VERSION_SUFFIX}")
endif()
set(_project_version "1.0.0+${_version_suffix}")

# Never let a configured updater build directory change recovery identity.
# Board-only and deployment layouts can target the same physical board while
# assigning incompatible partition geometry and signed layout ids.
if(HW1_DEPLOYMENT_ACTIVE)
    set(_updater_layout_mode
        "deployment:${_hw_deployment}:${_layout_id}")
else()
    set(_updater_layout_mode "legacy:${_hw_board}:${_layout_id}")
endif()
set(_updater_layout_mode_file "${CMAKE_BINARY_DIR}/hw1-layout-mode.txt")
if(EXISTS "${_updater_layout_mode_file}")
    file(READ "${_updater_layout_mode_file}" _updater_cached_layout_mode)
    string(STRIP "${_updater_cached_layout_mode}"
        _updater_cached_layout_mode)
    if(NOT _updater_cached_layout_mode STREQUAL _updater_layout_mode)
        message(FATAL_ERROR
            "This updater build directory was configured for "
            "'${_updater_cached_layout_mode}' and cannot be reused for "
            "'${_updater_layout_mode}'. Use a fresh -B directory; recovery "
            "identities and partition layouts are intentionally isolated.")
    endif()
else()
    file(WRITE "${_updater_layout_mode_file}"
        "${_updater_layout_mode}\n")
endif()

# --- Partition table: shared with the main application, never copied ---
#
# The updater used to point CONFIG_PARTITION_TABLE_CUSTOM_FILENAME at a local
# "partitions.csv". Two things were wrong with that. First, the root
# .gitignore matches `partitions.csv`, so that file was never tracked -- a
# fresh clone had no updater partition table at all and the build could not be
# configured. Second, the stale local copy that did exist had drifted: it said
# ota_0 0x5A0000 / littlefs 0x700000 while the root OTA CSV had grown ota_0 by
# 512K and moved littlefs to 0x780000. Both projects emit a partition table to
# the same flash offset, so whichever was flashed last decided where LittleFS
# lived.
#
# Pointing both builds at ONE tracked file removes both problems. The SR axis
# is read from the same header the root picker reads, for the same reason.
set(_updater_buildconfig
    "${CMAKE_CURRENT_LIST_DIR}/../components/hardwareone/System_BuildConfig.h")
if(NOT EXISTS "${_updater_buildconfig}")
    message(FATAL_ERROR
        "Cannot resolve the partition layout: ${_updater_buildconfig} is missing")
endif()
file(READ "${_updater_buildconfig}" _updater_bc_contents)
string(REGEX MATCH "#define ENABLE_ESP_SR[ \t]+([0-9]+)" _ "${_updater_bc_contents}")
if(CMAKE_MATCH_1 STREQUAL "1")
    set(_updater_sr_flag "sr")
else()
    set(_updater_sr_flag "no_sr")
endif()
file(STRINGS "${_board_defaults}" _updater_fs_line
     REGEX "^CONFIG_ESPTOOLPY_FLASHSIZE_[0-9]+MB=y")
if(NOT _updater_fs_line)
    message(FATAL_ERROR
        "updater/boards/${_hw_board}.defaults must declare "
        "CONFIG_ESPTOOLPY_FLASHSIZE_<n>MB=y so the partition table can be picked")
endif()
string(REGEX MATCH "FLASHSIZE_([0-9]+MB)" _ "${_updater_fs_line}")
string(TOLOWER "${CMAKE_MATCH_1}" _updater_flash_size)
if(HW1_DEPLOYMENT_ACTIVE)
    if(NOT _updater_flash_size STREQUAL HW1_DEPLOYMENT_FLASH_SIZE)
        message(FATAL_ERROR
            "Deployment ${_hw_deployment} flash ${HW1_DEPLOYMENT_FLASH_SIZE} "
            "does not match updater board flash ${_updater_flash_size}")
    endif()
    set(_updater_partitions "${HW1_DEPLOYMENT_PARTITION_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${HW1_DEPLOYMENT_CONTRACT_FILE}"
        "${HW1_DEPLOYMENT_PARTITION_FILE}")
else()
    set(_updater_partitions
        "${CMAKE_CURRENT_LIST_DIR}/../partitions/partitions_ota_${_updater_sr_flag}_${_updater_flash_size}.csv")
endif()
get_filename_component(_updater_partitions "${_updater_partitions}" ABSOLUTE)
if(NOT EXISTS "${_updater_partitions}")
    message(FATAL_ERROR
        "Expected shared partition file ${_updater_partitions} does not exist "
        "(resolved from board ${_hw_board}: ${_updater_flash_size}, "
        "${_updater_sr_flag}).")
endif()
message(STATUS
    "Updater board=${_hw_board} target=${_updater_target} "
    "flash=${_updater_flash_size} → ${_updater_partitions}")
set(_updater_partition_defaults
    "${CMAKE_BINARY_DIR}/hw1_updater_partitions.defaults")
file(WRITE "${_updater_partition_defaults}"
    "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${_updater_partitions}\"\n"
    "CONFIG_PARTITION_TABLE_FILENAME=\"${_updater_partitions}\"\n")

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

# Updater settings are build products, not source policy.  A build-local
# sdkconfig avoids legacy/deployment collisions, stale partition paths, and
# rewriting tracked files with workstation-specific signing-key paths.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig" CACHE FILEPATH
    "HardwareOne updater saved sdkconfig" FORCE)
# Kconfig defaults never override an existing sdkconfig value. Remove only the
# stale partition/key values so this invocation's selected contract and
# HW1_OTA_SIGNING_KEY are authoritative.
if(EXISTS "${SDKCONFIG}")
    file(READ "${SDKCONFIG}" _updater_sdkconfig_contents)
    set(_updater_sdkconfig_original "${_updater_sdkconfig_contents}")
    file(STRINGS "${SDKCONFIG}" _updater_partition_configured_lines
        REGEX "^CONFIG_PARTITION_TABLE_(CUSTOM_FILENAME|FILENAME)=")
    set(_updater_partition_expected_custom
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${_updater_partitions}\"")
    set(_updater_partition_expected_filename
        "CONFIG_PARTITION_TABLE_FILENAME=\"${_updater_partitions}\"")
    list(FIND _updater_partition_configured_lines
        "${_updater_partition_expected_custom}" _updater_partition_custom_index)
    list(FIND _updater_partition_configured_lines
        "${_updater_partition_expected_filename}" _updater_partition_filename_index)
    if(_updater_partition_custom_index EQUAL -1 OR
       _updater_partition_filename_index EQUAL -1)
        foreach(_updater_partition_key IN ITEMS
                CONFIG_PARTITION_TABLE_CUSTOM_FILENAME
                CONFIG_PARTITION_TABLE_FILENAME)
            string(REGEX REPLACE
                "(^|\n)${_updater_partition_key}=[^\n]*"
                "\\1" _updater_sdkconfig_contents
                "${_updater_sdkconfig_contents}")
            string(REGEX REPLACE
                "(^|\n)# ${_updater_partition_key} is not set"
                "\\1" _updater_sdkconfig_contents
                "${_updater_sdkconfig_contents}")
        endforeach()
    endif()
    string(REGEX REPLACE
        "(^|\n)CONFIG_SECURE_BOOT_SIGNING_KEY=[^\n]*"
        "\\1" _updater_sdkconfig_contents "${_updater_sdkconfig_contents}")
    string(REGEX REPLACE
        "(^|\n)# CONFIG_SECURE_BOOT_SIGNING_KEY is not set"
        "\\1" _updater_sdkconfig_contents "${_updater_sdkconfig_contents}")
    if(NOT _updater_sdkconfig_contents STREQUAL _updater_sdkconfig_original)
        file(WRITE "${SDKCONFIG}" "${_updater_sdkconfig_contents}")
        message(STATUS
            "Cleared stale updater partition/key settings; current contract and key will apply")
    endif()
endif()
set(_ota_key_defaults "${CMAKE_BINARY_DIR}/hw1_ota_signing_key.defaults")
file(WRITE "${_ota_key_defaults}"
    "CONFIG_SECURE_BOOT_SIGNING_KEY=\"${_ota_signing_key}\"\n")
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/../config/sdkconfig.ota.defaults"
    "${_board_defaults}"
    "${_updater_partition_defaults}"
    "${_ota_key_defaults}")

# --- Runtime layout gate constants, generated from the SAME csv ---
#
# updater_preflight.c refuses to start the recovery SoftAP unless the flash it
# booted from matches the expected partition geometry. That check is the reason
# a mis-flashed device stays dark instead of writing an image into the wrong
# slot, so it has to be right.
#
# It used to compare against eight literals in updater_preflight.c/.h holding
# 16 MB FeatherS3 values. Three of them (ota_0 size, littlefs offset, littlefs
# size) were stale even for that board -- they matched the deleted
# updater/partitions.csv rather than the real one, so the S3 recovery SoftAP
# had been refusing to start since the partitions grew on 2026-08-16. The
# failure is invisible until someone actually needs recovery, which is the
# worst possible time to discover it.
#
# Deriving them from the csv this firmware is built against keeps the check
# meaningful -- it still validates real flash against a declared layout -- while
# making it impossible for the declaration and the layout to drift apart.
file(STRINGS "${_updater_partitions}"
     _hw1_csv_rows REGEX "^[A-Za-z_][A-Za-z0-9_]*[ \t]*,")
macro(hw1_partition_geometry _pname _off_var _size_var)
    set(${_off_var} "")
    set(${_size_var} "")
    foreach(_row IN LISTS _hw1_csv_rows)
        if(_row MATCHES "^${_pname}[ \t]*,[^,]*,[^,]*,[ \t]*(0[xX][0-9A-Fa-f]+)[ \t]*,[ \t]*(0[xX][0-9A-Fa-f]+)")
            set(${_off_var} "${CMAKE_MATCH_1}")
            set(${_size_var} "${CMAKE_MATCH_2}")
        endif()
    endforeach()
    if("${${_off_var}}" STREQUAL "" OR "${${_size_var}}" STREQUAL "")
        message(FATAL_ERROR
            "Could not read the '${_pname}' offset/size from "
            "${_updater_partitions}; the recovery layout gate cannot be built.")
    endif()
endmacro()
hw1_partition_geometry(factory  _hw1_fac_off  _hw1_fac_size)
hw1_partition_geometry(otadata  _hw1_otad_off _hw1_otad_size)
hw1_partition_geometry(ota_0    _hw1_ota0_off _hw1_ota0_size)
hw1_partition_geometry(littlefs _hw1_lfs_off  _hw1_lfs_size)
message(STATUS
    "Updater layout gate: factory ${_hw1_fac_off}/${_hw1_fac_size} "
    "otadata ${_hw1_otad_off}/${_hw1_otad_size} "
    "ota_0 ${_hw1_ota0_off}/${_hw1_ota0_size} "
    "littlefs ${_hw1_lfs_off}/${_hw1_lfs_size}")

add_compile_definitions(
    HW1_FACTORY_OFFSET=${_hw1_fac_off}u
    HW1_FACTORY_SIZE=${_hw1_fac_size}u
    HW1_OTADATA_OFFSET=${_hw1_otad_off}u
    HW1_OTADATA_SIZE=${_hw1_otad_size}u
    HW1_OTA0_OFFSET=${_hw1_ota0_off}u
    HW1_UPDATER_OTA0_SIZE=${_hw1_ota0_size}u
    HW1_LITTLEFS_OFFSET=${_hw1_lfs_off}u
    HW1_LITTLEFS_SIZE=${_hw1_lfs_size}u
    HW1_OTA_BOARD_ID="${_hw_board}"
    HW1_OTA_LAYOUT_ID="${_layout_id}"
    HW1_OTA_VERSION_SUFFIX="+${_version_suffix}"
    HW1_OTA_UPDATER_VERSION="${_project_version}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# One dependency lock per chip.
#
# The lock file records the target it was solved for. With a single shared
# updater/dependencies.lock, building the classic-ESP32 recovery image after
# the ESP32-S3 one rewrote the tracked file (`target: esp32s3` -> `esp32`) and
# forced a full re-solve, then the next S3 build rewrote it back. Endless churn
# in a tracked file, and a slow re-solve on every board switch. IDF supports a
# per-target path for exactly this case -- see
# tools/test_build_system/test_component_manager.py::test_dependency_lock.
idf_build_set_property(DEPENDENCIES_LOCK
    "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")

set(PROJECT_VER "${_project_version}")
project(hw1-updater)
