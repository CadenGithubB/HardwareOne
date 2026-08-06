# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# ─────────────────────────────────────────────────────────────────────────────
# Pre-project configuration: target detection, board selection, partition
# table selection, SDKCONFIG_DEFAULTS layering, and sdkconfig auto-recovery.
#
# Order matters: target → board → flash size → partition table → defaults →
# auto-recovery, since each step's output feeds the next.
# ─────────────────────────────────────────────────────────────────────────────

# --- 1. Target detection ---
# Precedence: IDF_TARGET env var (set by `idf.py set-target` and by idf.py
# during normal builds) → existing sdkconfig → S3 fallback for a totally
# fresh checkout (project is S3-first).
# `idf.py set-target <chip>` and normal builds pass the target as a CMake cache
# variable (-DIDF_TARGET=<chip>). Prefer it so board/target validation compares
# against the target being configured RIGHT NOW, not a stale sdkconfig. Without
# this, switching to an ESP32 board while an ESP32-S3 build exists (or vice
# versa) trips the mismatch guard below during set-target's own reconfigure and
# aborts the switch (the reconfigure deletes/replaces sdkconfig, so reading it
# yields the old chip or the S3 fallback). Env var and sdkconfig remain fallbacks.
set(_inferred_target "")
if(DEFINED IDF_TARGET AND NOT "${IDF_TARGET}" STREQUAL "")
    set(_inferred_target "${IDF_TARGET}")
endif()
if(NOT _inferred_target)
    set(_inferred_target "$ENV{IDF_TARGET}")
endif()
set(_sdkconfig "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig")
if(DEFINED SDKCONFIG AND NOT "${SDKCONFIG}" STREQUAL "")
    if(IS_ABSOLUTE "${SDKCONFIG}")
        set(_sdkconfig "${SDKCONFIG}")
    else()
        set(_sdkconfig "${CMAKE_CURRENT_SOURCE_DIR}/${SDKCONFIG}")
    endif()
endif()
if(NOT _inferred_target)
    if(EXISTS "${_sdkconfig}")
        file(STRINGS "${_sdkconfig}" _target_line REGEX "^CONFIG_IDF_TARGET=\"[^\"]+\"")
        if(_target_line)
            string(REGEX REPLACE "^CONFIG_IDF_TARGET=\"([^\"]+)\".*" "\\1"
                   _inferred_target "${_target_line}")
        endif()
    endif()
endif()
if(NOT _inferred_target)
    set(_inferred_target "esp32s3")
endif()

# --- 2. Board selection (HW_BOARD, all targets) ---
#
# Each file in boards/ holds the settings that distinguish one board from the
# others in its chip family (Arduino variant, PSRAM mode/CS, flash size, …).
# The chip-family defaults (sdkconfig.defaults.<target>) are auto-loaded by IDF
# based on target; the board file is layered on top so its values override.
#
# Each board file declares which chip it targets with a "# HW_TARGET: <chip>"
# marker. The active target (step 1) is chosen by `idf.py set-target`; HW_BOARD
# only LAYERS board deltas on top of it — it does not switch chips — so the two
# must agree. We validate that below and fail with a fix-it hint rather than
# silently mis-building (e.g. an S3 PSRAM mode baked onto an ESP32 bootloader).
#
# Usage:
#   # ESP32-S3 boards (default feathers3):
#   HW_BOARD=xiao_s3          idf.py set-target esp32s3 && idf.py fullclean && idf.py build
#   # ESP32 boards (default qtpy_esp32):
#   HW_BOARD=feather_esp32_v2 idf.py set-target esp32   && idf.py fullclean && idf.py build
#
# Default board per target: esp32s3 → feathers3, esp32 → qtpy_esp32. Any other
# target (or no boards/ file) skips layering and uses CONFIG_ARDUINO_VARIANT
# straight from sdkconfig.defaults.<target>.
set(_hw_board "$ENV{HW_BOARD}")
set(_board_file "")
set(_flash_size "8mb")   # used by the partition picker; overridden from the board file below
if(NOT _hw_board)
    if(_inferred_target STREQUAL "esp32s3")
        set(_hw_board "feathers3")
    elseif(_inferred_target STREQUAL "esp32")
        set(_hw_board "qtpy_esp32")
    endif()
endif()
if(_hw_board)
    set(_board_file "${CMAKE_CURRENT_SOURCE_DIR}/boards/${_hw_board}.defaults")
    if(NOT EXISTS "${_board_file}")
        file(GLOB _available_boards
             RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/boards"
             "${CMAKE_CURRENT_SOURCE_DIR}/boards/*.defaults")
        list(TRANSFORM _available_boards REPLACE "\\.defaults$" "")
        string(JOIN ", " _available_str ${_available_boards})
        message(FATAL_ERROR
            "HW_BOARD=${_hw_board} but boards/${_hw_board}.defaults does not exist. "
            "Available: ${_available_str}")
    endif()
    # Validate the board's declared chip matches the active target. HW_BOARD
    # layers settings; it cannot change the target, so a mismatch is always a
    # mistake — point the user at set-target instead of producing a bad image.
    file(STRINGS "${_board_file}" _bt_line REGEX "^# *HW_TARGET:")
    if(_bt_line)
        string(REGEX REPLACE "^# *HW_TARGET:[ \t]*([A-Za-z0-9]+).*" "\\1" _board_target "${_bt_line}")
        if(NOT _board_target STREQUAL _inferred_target)
            message(FATAL_ERROR
                "HW_BOARD=${_hw_board} targets '${_board_target}', but the active chip is "
                "'${_inferred_target}'. HW_BOARD only layers board settings — it does not "
                "switch chips. Run:\n"
                "    idf.py set-target ${_board_target} && idf.py fullclean\n"
                "then rebuild with HW_BOARD=${_hw_board}.")
        endif()
    endif()
    # Flash size comes from the board file's CONFIG_ESPTOOLPY_FLASHSIZE_*MB
    # marker. The partition picker below uses it to pick partitions_*_<size>.csv.
    file(STRINGS "${_board_file}" _fs_line REGEX "^CONFIG_ESPTOOLPY_FLASHSIZE_[0-9]+MB=y")
    if(_fs_line)
        string(REGEX MATCH "FLASHSIZE_([0-9]+MB)" _ "${_fs_line}")
        string(TOLOWER "${CMAKE_MATCH_1}" _flash_size)
    endif()
    message(STATUS "HW_BOARD=${_hw_board} (target=${_inferred_target}, flash=${_flash_size}) → layering ${_board_file}")
else()
    message(STATUS "Target=${_inferred_target}: no boards/ mapping → layering skipped; "
                   "using CONFIG_ARDUINO_VARIANT from sdkconfig.defaults.${_inferred_target}")
endif()

# --- 3. Partition table selection ---
#
# Two axes: SR vs no_sr (from #define ENABLE_ESP_SR in System_BuildConfig.h)
# and flash size (from the active board file, defaulted to 8mb for ESP32
# boards and as a fallback). The four combinations are:
#   partitions_no_sr_8mb.csv    factory 4948K + littlefs 3180K
#   partitions_no_sr_16mb.csv   factory 4948K + littlefs 11628K  (FeatherS3[D])
#   partitions_sr_8mb.csv       factory 4992K + model 3008K + littlefs 128K
#   partitions_sr_16mb.csv      factory 4992K + model 3008K + littlefs 8320K
#
# When ENABLE_ESP_SR is enabled, we need a "model" SPIFFS partition for
# WakeNet/MultiNet; when disabled, that space is reclaimed for LittleFS.
set(_ota_layout FALSE)
if(DEFINED ENV{HW_OTA_LAYOUT})
    string(TOLOWER "$ENV{HW_OTA_LAYOUT}" _ota_layout_env)
    if(_ota_layout_env STREQUAL "1" OR
       _ota_layout_env STREQUAL "on" OR
       _ota_layout_env STREQUAL "true" OR
       _ota_layout_env STREQUAL "yes")
        set(_ota_layout TRUE)
    endif()
endif()

if(_ota_layout AND
   (NOT _inferred_target STREQUAL "esp32s3" OR
    NOT _flash_size STREQUAL "16mb" OR
    NOT (_hw_board STREQUAL "feathers3" OR _hw_board STREQUAL "feathers3_fe")))
    message(FATAL_ERROR
        "HW_OTA_LAYOUT=1 currently supports only 16 MB FeatherS3 boards "
        "(HW_BOARD=feathers3 or feathers3_fe, target esp32s3).")
endif()

# A CMake build directory must never change between the ordinary and recovery-
# OTA layouts.  The two modes use different partition tables, application
# destinations, rollback policy, and sdkconfig files; silently reusing cached
# state across them is a deployment hazard even when the source still builds.
set(_hw1_layout_mode "ordinary")
if(_ota_layout)
    set(_hw1_layout_mode "recovery-ota")
endif()
set(_hw1_layout_mode_file "${CMAKE_BINARY_DIR}/hw1-layout-mode.txt")
if(EXISTS "${_hw1_layout_mode_file}")
    file(READ "${_hw1_layout_mode_file}" _hw1_cached_layout_mode)
    string(STRIP "${_hw1_cached_layout_mode}" _hw1_cached_layout_mode)
    if(NOT _hw1_cached_layout_mode STREQUAL _hw1_layout_mode)
        message(FATAL_ERROR
            "This build directory was configured for '${_hw1_cached_layout_mode}' "
            "and cannot be reused for '${_hw1_layout_mode}'. Use a fresh -B "
            "directory; OTA and ordinary build caches are intentionally isolated.")
    endif()
else()
    file(WRITE "${_hw1_layout_mode_file}" "${_hw1_layout_mode}\n")
endif()

if(_ota_layout)
    # Keep OTA configuration isolated from the ordinary tracked sdkconfig. A
    # cached default SDKCONFIG from an older ordinary configure must be replaced
    # too; otherwise explicit `# ... is not set` lines defeat OTA defaults.
    get_filename_component(_ordinary_sdkconfig
        "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig" ABSOLUTE)
    get_filename_component(_active_sdkconfig "${_sdkconfig}" ABSOLUTE)
    if(NOT DEFINED SDKCONFIG OR "${SDKCONFIG}" STREQUAL "" OR
       "${_active_sdkconfig}" STREQUAL "${_ordinary_sdkconfig}")
        set(SDKCONFIG "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.ota" CACHE FILEPATH
            "HardwareOne OTA sdkconfig" FORCE)
        set(_sdkconfig "${SDKCONFIG}")
    else()
        message(STATUS "Recovery OTA build uses isolated SDKCONFIG=${_active_sdkconfig}")
    endif()
endif()

set(_buildconfig "${CMAKE_CURRENT_SOURCE_DIR}/components/hardwareone/System_BuildConfig.h")
if(EXISTS "${_buildconfig}")
    file(READ "${_buildconfig}" _bc_contents)
    string(REGEX MATCH "#define ENABLE_ESP_SR[ \t]+([0-9]+)" _match "${_bc_contents}")
    set(_espsr_val "${CMAKE_MATCH_1}")
    if(_espsr_val STREQUAL "1")
        set(_sr_flag "sr")
    else()
        set(_sr_flag "no_sr")
    endif()
    if(_ota_layout)
        set(_partitions_src
            "${CMAKE_CURRENT_SOURCE_DIR}/partitions_ota_${_sr_flag}_${_flash_size}.csv")
    else()
        set(_partitions_src
            "${CMAKE_CURRENT_SOURCE_DIR}/partitions_${_sr_flag}_${_flash_size}.csv")
    endif()
    if(NOT EXISTS "${_partitions_src}")
        message(FATAL_ERROR
            "Expected partition file ${_partitions_src} does not exist. "
            "Add it (and any sibling _8mb / _16mb file you may have missed).")
    endif()
    message(STATUS "ENABLE_ESP_SR=${_espsr_val}, flash=${_flash_size} → ${_partitions_src}")
    set(_partitions_dst "${CMAKE_CURRENT_SOURCE_DIR}/partitions.csv")
    file(READ "${_partitions_src}" _src_content)
    set(_dst_content "")
    if(EXISTS "${_partitions_dst}")
        file(READ "${_partitions_dst}" _dst_content)
    endif()
    if(NOT "${_src_content}" STREQUAL "${_dst_content}")
        file(WRITE "${_partitions_dst}" "${_src_content}")
        message(STATUS "Updated partitions.csv from ${_partitions_src}")
    endif()
endif()

# --- 4. SDKCONFIG_DEFAULTS layering + sdkconfig auto-recovery ---
# Runs whenever a board file was selected in step 2 (any target).
if(_board_file)
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults"
        "${_board_file}")
    if(_ota_layout)
        list(APPEND SDKCONFIG_DEFAULTS
            "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.ota.defaults")

        # Keep workstation-specific key paths out of tracked configuration.
        # A conventional ignored local filename is supported for convenience;
        # HW1_OTA_SIGNING_KEY is preferable when the key lives in backed-up
        # secret storage.  OTA builds fail at configure time if neither exists.
        if(DEFINED ENV{HW1_OTA_SIGNING_KEY} AND
           NOT "$ENV{HW1_OTA_SIGNING_KEY}" STREQUAL "")
            get_filename_component(_ota_signing_key
                "$ENV{HW1_OTA_SIGNING_KEY}" ABSOLUTE)
        else()
            set(_ota_signing_key
                "${CMAKE_CURRENT_SOURCE_DIR}/ota_signing_key.pem")
        endif()
        if(NOT EXISTS "${_ota_signing_key}")
            message(FATAL_ERROR
                "Recovery OTA builds require an RSA-3072 signing key. Set "
                "HW1_OTA_SIGNING_KEY or create the ignored local file "
                "ota_signing_key.pem; never commit the private key. Missing: "
                "${_ota_signing_key}")
        endif()

        find_program(_ota_openssl openssl)
        if(NOT _ota_openssl)
            message(FATAL_ERROR "openssl is required for recovery OTA builds")
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

        # Generate the public half before project() discovers components. The
        # protocol component embeds this file from its own directory scope;
        # doing target_add_binary_data() here after project() makes modern
        # CMake reject the cross-directory generated .S source.
        set(HW1_OTA_PUBLIC_KEY_FILE
            "${CMAKE_BINARY_DIR}/hw1_ota_public_key.pem")
        execute_process(
            COMMAND "${_ota_openssl}" pkey -in "${_ota_signing_key}" -pubout
                    -out "${HW1_OTA_PUBLIC_KEY_FILE}"
            RESULT_VARIABLE _ota_public_key_result
            ERROR_VARIABLE _ota_public_key_error)
        if(NOT _ota_public_key_result EQUAL 0)
            message(FATAL_ERROR
                "Could not derive the OTA public key: ${_ota_public_key_error}")
        endif()

        # Defaults do not override an existing sdkconfig value. Remove only a
        # stale signing-key line so the current, explicitly supplied key cannot
        # diverge from the public key embedded below.
        if(EXISTS "${_sdkconfig}")
            file(READ "${_sdkconfig}" _ota_sdk_contents)
            file(STRINGS "${_sdkconfig}" _ota_configured_key_lines
                REGEX "^CONFIG_SECURE_BOOT_SIGNING_KEY=")
            set(_ota_expected_key_line
                "CONFIG_SECURE_BOOT_SIGNING_KEY=\"${_ota_signing_key}\"")
            list(FIND _ota_configured_key_lines "${_ota_expected_key_line}"
                _ota_expected_key_index)
            if(_ota_expected_key_index EQUAL -1)
                string(REGEX REPLACE
                    "(^|\n)CONFIG_SECURE_BOOT_SIGNING_KEY=\"[^\"]*\""
                    "\\1" _ota_sdk_contents "${_ota_sdk_contents}")
                string(REGEX REPLACE
                    "(^|\n)# CONFIG_SECURE_BOOT_SIGNING_KEY is not set"
                    "\\1" _ota_sdk_contents "${_ota_sdk_contents}")
                file(WRITE "${_sdkconfig}" "${_ota_sdk_contents}")
                message(STATUS
                    "Cleared stale OTA signing-key setting so the explicit key reapplies")
            endif()
        endif()
        set(_ota_key_defaults
            "${CMAKE_BINARY_DIR}/hw1_ota_signing_key.defaults")
        file(WRITE "${_ota_key_defaults}"
            "CONFIG_SECURE_BOOT_SIGNING_KEY=\"${_ota_signing_key}\"\n")
        list(APPEND SDKCONFIG_DEFAULTS "${_ota_key_defaults}")
    endif()

    # Auto-recovery: if HW_BOARD is explicitly set, ensure sdkconfig honors
    # the board file's settings. IDF's defaults-files only apply to UNSET
    # options, so a stale sdkconfig (e.g. restored by `set-target` from a
    # sdkconfig.<target> snapshot of a different board) sticks. We surgically
    # remove ONLY the keys the board file controls — Kconfig then re-applies
    # them from the board file as defaults. Everything else the user has
    # tuned in sdkconfig is preserved.
    set(_hw_board_explicit FALSE)
    if(DEFINED ENV{HW_BOARD} AND NOT "$ENV{HW_BOARD}" STREQUAL "")
        set(_hw_board_explicit TRUE)
    endif()
    if(_hw_board_explicit AND EXISTS "${_sdkconfig}")
        file(READ "${_sdkconfig}" _sdk_contents)
        file(STRINGS "${_board_file}" _board_settings REGEX "^CONFIG_[A-Z0-9_]+=")
        set(_modified FALSE)
        set(_changed_keys "")

        foreach(_setting IN LISTS _board_settings)
            string(REGEX MATCH "^(CONFIG_[A-Z0-9_]+)" _ "${_setting}")
            set(_key "${CMAKE_MATCH_1}")

            # Skip if sdkconfig already has the exact expected line.
            if(_sdk_contents MATCHES "(^|\n)${_setting}(\n|$)")
                continue()
            endif()

            # A board-file "KEY=n" is satisfied by Kconfig's "# KEY is not set"
            # form too - without this, an =n guard line would strip-and-rewrite
            # sdkconfig on every configure.
            if(_setting MATCHES "=n$" AND _sdk_contents MATCHES "(^|\n)# ${_key} is not set(\n|$)")
                continue()
            endif()

            # Strip existing value/notset forms of this key.
            string(REGEX REPLACE "(^|\n)${_key}=[^\n]*" "\\1" _sdk_contents "${_sdk_contents}")
            string(REGEX REPLACE "(^|\n)# ${_key} is not set" "\\1" _sdk_contents "${_sdk_contents}")
            list(APPEND _changed_keys "${_key}")
            set(_modified TRUE)

            # Choice groups: when one key is set, the others must be unset, or
            # Kconfig will keep the old active choice and ignore the board-file
            # default. We strip ALL siblings of any matched group so Kconfig
            # re-resolves the choice from the board file.
            if(_key STREQUAL "CONFIG_SPIRAM_MODE_OCT" OR _key STREQUAL "CONFIG_SPIRAM_MODE_QUAD")
                string(REGEX REPLACE "(^|\n)CONFIG_SPIRAM_MODE_(OCT|QUAD)=[^\n]*" "\\1" _sdk_contents "${_sdk_contents}")
                string(REGEX REPLACE "(^|\n)# CONFIG_SPIRAM_MODE_(OCT|QUAD) is not set" "\\1" _sdk_contents "${_sdk_contents}")
            elseif(_key MATCHES "^CONFIG_ESPTOOLPY_FLASHSIZE_[0-9]+MB$")
                # Flash size choice: strip every _<n>MB sibling and the
                # derived CONFIG_ESPTOOLPY_FLASHSIZE="<n>MB" string (which
                # Kconfig recomputes from whichever boolean ends up active).
                string(REGEX REPLACE "(^|\n)CONFIG_ESPTOOLPY_FLASHSIZE_[0-9]+MB=[^\n]*" "\\1" _sdk_contents "${_sdk_contents}")
                string(REGEX REPLACE "(^|\n)# CONFIG_ESPTOOLPY_FLASHSIZE_[0-9]+MB is not set" "\\1" _sdk_contents "${_sdk_contents}")
                string(REGEX REPLACE "(^|\n)CONFIG_ESPTOOLPY_FLASHSIZE=\"[^\"]*\"" "\\1" _sdk_contents "${_sdk_contents}")
            endif()
        endforeach()

        if(_modified)
            # Collapse blank lines left behind by the surgical removals.
            string(REGEX REPLACE "\n\n+" "\n" _sdk_contents "${_sdk_contents}")
            file(WRITE "${_sdkconfig}" "${_sdk_contents}")
            string(JOIN ", " _changed_str ${_changed_keys})
            message(STATUS
                "HW_BOARD=${_hw_board}: cleared stale [${_changed_str}] from sdkconfig "
                "so board file values re-apply (other settings preserved)")
        endif()
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(_ota_layout)
    if(_hw_board STREQUAL "feathers3_fe")
        set(PROJECT_VER "0.99.82+f3feo1")
    else()
        set(PROJECT_VER "0.99.82+f3o1")
    endif()
else()
    set(PROJECT_VER "0.99.82")
endif()
project(hardwareone-idf)

# ESP-IDF creates component targets while project() runs. Directory-level
# add_compile_definitions() calls made below therefore do not retroactively
# reach first-party component sources. Bind load-bearing layout definitions to
# the component target itself; otherwise System_OTA.cpp silently takes its
# HW1_OTA_LAYOUT=0 fallback even though the OTA partition/build gates are active.
idf_component_get_property(_hw1_app_component hardwareone COMPONENT_LIB)
if(NOT TARGET ${_hw1_app_component})
    message(FATAL_ERROR
        "Could not resolve the hardwareone component target for OTA definitions")
endif()

if(_ota_layout)
    # These are load-bearing OTA safety settings, not best-effort defaults.
    # Assert them during configure so a custom sdkconfig cannot produce an
    # unsafe OTA binary and rely on a later flash-time audit to notice.
    foreach(_ota_required_option IN ITEMS
            CONFIG_PARTITION_TABLE_CUSTOM
            CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
            CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT
            CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME
            CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
            CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)
        if(NOT ${_ota_required_option})
            message(FATAL_ERROR
                "Recovery OTA requires ${_ota_required_option}=y. Use an "
                "isolated OTA sdkconfig/build directory and reconfigure.")
        endif()
    endforeach()
    foreach(_ota_forbidden_option IN ITEMS
            CONFIG_SECURE_BOOT
            CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
            CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
            CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON
            CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS)
        if(${_ota_forbidden_option})
            message(FATAL_ERROR
                "Recovery OTA requires ${_ota_forbidden_option}=n.")
        endif()
    endforeach()
    if(NOT CONFIG_IDF_TARGET STREQUAL "esp32s3" OR
       NOT CONFIG_ESPTOOLPY_FLASHSIZE STREQUAL "16MB" OR
       NOT CONFIG_PARTITION_TABLE_OFFSET STREQUAL "0x9000")
        message(FATAL_ERROR
            "Recovery OTA requires ESP32-S3, 16MB flash, and partition table "
            "offset 0x9000; the active sdkconfig does not match.")
    endif()
    if(_hw_board STREQUAL "feathers3_fe")
        if(NOT CONFIG_SECURE_FLASH_ENC_ENABLED OR
           NOT CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT OR
           NOT CONFIG_SECURE_FLASH_ENCRYPTION_AES128 OR
           NOT CONFIG_NVS_ENCRYPTION OR
           NOT CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC)
            message(FATAL_ERROR
                "feathers3_fe recovery OTA requires Development-mode flash "
                "encryption and flash-encryption-protected NVS keys.")
        endif()
    elseif(CONFIG_SECURE_FLASH_ENC_ENABLED OR CONFIG_NVS_ENCRYPTION)
        message(FATAL_ERROR
            "Plain feathers3 recovery OTA must not inherit flash/NVS encryption.")
    endif()

    if(_hw_board STREQUAL "feathers3_fe")
        set(_ota_layout_id "hw1-f3fe-ota-v1")
    else()
        set(_ota_layout_id "hw1-f3-ota-v1")
    endif()
    add_compile_definitions(
        HW1_OTA_LAYOUT=1
        HW1_OTA_BOARD_ID="${_hw_board}"
        HW1_OTA_LAYOUT_ID="${_ota_layout_id}"
    )
    target_compile_definitions(${_hw1_app_component} PRIVATE
        HW1_OTA_LAYOUT=1
        HW1_OTA_BOARD_ID="${_hw_board}"
        HW1_OTA_LAYOUT_ID="${_ota_layout_id}"
    )
    get_target_property(_hw1_app_compile_definitions
        ${_hw1_app_component} COMPILE_DEFINITIONS)
    foreach(_hw1_required_definition IN ITEMS
            "HW1_OTA_LAYOUT=1"
            "HW1_OTA_BOARD_ID=\"${_hw_board}\""
            "HW1_OTA_LAYOUT_ID=\"${_ota_layout_id}\"")
        list(FIND _hw1_app_compile_definitions
            "${_hw1_required_definition}" _hw1_definition_index)
        if(_hw1_definition_index EQUAL -1)
            message(FATAL_ERROR
                "Recovery OTA definition did not reach hardwareone target: "
                "${_hw1_required_definition}")
        endif()
    endforeach()
    message(STATUS
        "Recovery OTA layout ENABLED: board=${_hw_board}, layout=${_ota_layout_id}. "
        "Stock ESP-IDF flash targets are disabled; use guarded HardwareOne targets.")

    # The protocol component embedded HW1_OTA_PUBLIC_KEY_FILE during project()
    # discovery. The private key remains outside the source tree/device image.

    # Stock IDF app-flash chooses the factory partition when one exists. Here
    # that would place a ~5 MiB main image in a 1.3 MiB recovery slot and write
    # across otadata into ota_0. Refuse every stock mutation target before it
    # opens a serial port; only the guarded targets below are valid.
    idf_build_get_property(_ota_build_dir BUILD_DIR)
    idf_component_get_property(_ota_flash_main_args esptool_py FLASH_ARGS)
    idf_component_get_property(_ota_flash_sub_args esptool_py FLASH_SUB_ARGS)

    # Name the port before writing it.
    #
    # `idf.py -p <port> ota0-flash` accepts -p and then throws it away. The port
    # is a global click option (serial_ext.py) that the BUILT-IN flash actions
    # forward via run_target(..., {'ESPPORT': args.port}); every target below is
    # a plain CMake custom target, so idf.py routes it through fallback_target()
    # (core_ext.py), which calls run_target() with no environment. esptool then
    # scans and takes whichever device answers first. One board, harmless. Two
    # boards, it writes the wrong one and says nothing.
    #
    # The guard refuses only genuine ambiguity, so the single-board bench case
    # is unchanged apart from a line stating the port. ESPPORT always wins.
    function(hw1_require_flash_port _program_target)
        set(_guard "hw1-flash-port-${_program_target}")
        if(NOT TARGET ${_guard})
            idf_build_get_property(_guard_python PYTHON)
            add_custom_target(${_guard}
                COMMAND "${_guard_python}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ota/require_flash_port.py"
                    "${_program_target}"
                USES_TERMINAL
                VERBATIM)
        endif()
        add_dependencies(${_program_target} ${_guard})
    endfunction()
    add_custom_target(hw1-stock-flash-refused
        COMMAND ${CMAKE_COMMAND} -E echo
            "Refusing stock ESP-IDF flash/erase target for a recovery-OTA layout. Use migration-flash or ota0-flash. On feathers3_fe use the encrypted variants."
        COMMAND ${CMAKE_COMMAND} -E false
        USES_TERMINAL
        VERBATIM)
    foreach(_unsafe_ota_target IN ITEMS
            flash encrypted-flash app-flash encrypted-app-flash
            bootloader-flash encrypted-bootloader-flash
            partition-table-flash encrypted-partition-table-flash
            partition_table-flash otadata-flash encrypted-otadata-flash
            erase_flash erase-otadata erase_otadata dfu-flash)
        if(TARGET ${_unsafe_ota_target})
            add_dependencies(${_unsafe_ota_target} hw1-stock-flash-refused)
        endif()
    endforeach()

    # A direct ota_0 cable write is slot-safe but can still strand recovery if
    # the main and factory updater use different signing keys or layouts. Make
    # the same paired artifact audit mandatory for every safe flash target.
    set(_ota_updater_bin "")
    if(DEFINED ENV{HW1_UPDATER_BIN} AND
       NOT "$ENV{HW1_UPDATER_BIN}" STREQUAL "")
        get_filename_component(_ota_updater_bin
            "$ENV{HW1_UPDATER_BIN}" ABSOLUTE)
        if(NOT EXISTS "${_ota_updater_bin}")
            message(FATAL_ERROR
                "HW1_UPDATER_BIN does not exist: ${_ota_updater_bin}")
        endif()
        file(SIZE "${_ota_updater_bin}" _ota_updater_size)
        if(_ota_updater_size GREATER 1205862)
            message(FATAL_ERROR
                "Updater exceeds the 1.15 MiB release gate: "
                "${_ota_updater_size} bytes")
        endif()
        get_filename_component(_ota_updater_build
            "${_ota_updater_bin}" DIRECTORY)
        idf_build_get_property(_ota_python PYTHON)
        add_custom_target(hw1-ota-pair-audit
            COMMAND "${_ota_python}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/ota/check_ota_builds.py"
                --board "${_hw_board}"
                --main-build "${_ota_build_dir}"
                --updater-build "${_ota_updater_build}"
            DEPENDS gen_project_binary "${_ota_updater_bin}"
            USES_TERMINAL
            VERBATIM)
        set(_ota_pair_gate hw1-ota-pair-audit)
    else()
        add_custom_target(hw1-ota-updater-required
            COMMAND ${CMAKE_COMMAND} -E echo
                "Refusing OTA cable write without HW1_UPDATER_BIN=/absolute/path/to/matched/updater-build/hw1-updater.bin"
            COMMAND ${CMAKE_COMMAND} -E false
            USES_TERMINAL)
        set(_ota_pair_gate hw1-ota-updater-required)
    endif()

    # The HardwareOne application belongs in ota_0. This generated target also
    # handles Development-mode flash encryption and is paired-audit gated.
    esptool_py_flash_target(hw1-ota0-program
        "${_ota_flash_main_args}" "${_ota_flash_sub_args}")
    esptool_py_flash_to_partition(hw1-ota0-program "ota_0"
        "${_ota_build_dir}/${PROJECT_BIN}")
    add_dependencies(hw1-ota0-program gen_project_binary ${_ota_pair_gate})
    hw1_require_flash_port(hw1-ota0-program)
    if(CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT)
        add_dependencies(encrypted-hw1-ota0-program
            gen_project_binary ${_ota_pair_gate})
        hw1_require_flash_port(encrypted-hw1-ota0-program)
        add_custom_target(encrypted-ota0-flash
            DEPENDS encrypted-hw1-ota0-program)
        add_custom_target(hw1-plaintext-ota0-refused
            COMMAND ${CMAKE_COMMAND} -E echo
                "Refusing plaintext ota_0 programming on feathers3_fe"
            COMMAND ${CMAKE_COMMAND} -E false
            USES_TERMINAL)
        add_dependencies(hw1-ota0-program hw1-plaintext-ota0-refused)
        add_custom_target(ota0-flash
            COMMAND ${CMAKE_COMMAND} -E echo
                "Use encrypted-ota0-flash for feathers3_fe"
            COMMAND ${CMAKE_COMMAND} -E false
            USES_TERMINAL)
    else()
        add_custom_target(ota0-flash DEPENDS hw1-ota0-program)
    endif()

    # A blank LittleFS image, built on the host and flashed as part of the
    # one-time migration.  This is what lets NO firmware carry the ability to
    # erase its own storage: previously the relocated partition could only be
    # made mountable by a migration-only updater build that shipped a
    # destructive `formatfs confirm` console command, which then had to be
    # replaced by a second build and a second migration flash.  Generating the
    # empty filesystem here removes that build variant, that console command,
    # and that second flash.
    #
    # The seed directory is created in the build tree rather than tracked, so
    # no placeholder file ends up on the device's filesystem.
    #
    # Side effect worth knowing: this also defines a `littlefs-flash` target
    # that writes the same blank image on its own.  That is the supported way
    # to wipe a device's filesystem - the capability now lives in the host
    # toolchain instead of inside firmware sitting on the device.
    set(_ota_blank_fs_dir "${CMAKE_BINARY_DIR}/littlefs_blank")
    file(MAKE_DIRECTORY "${_ota_blank_fs_dir}")
    littlefs_create_partition_image(littlefs "${_ota_blank_fs_dir}")

    # littlefs_create_partition_image() defines a plaintext `littlefs-flash`
    # unconditionally. esptool_py_flash_target_image() decides encryption solely
    # from CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT - it never consults
    # the partition CSV - so on feathers3_fe the plaintext target would write an
    # unencrypted image into a partition the device reads through the flash
    # encryption engine, i.e. silent corruption. Refuse it there, exactly as the
    # ota0 and migration targets already do. The migration path itself is
    # already safe: it reaches littlefs through esptool_py_flash_to_partition,
    # whose plaintext parent target is refused on this board.
    if(CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT)
        add_custom_target(hw1-plaintext-littlefs-refused
            COMMAND ${CMAKE_COMMAND} -E echo
                "Refusing plaintext littlefs programming on feathers3_fe; use encrypted-littlefs-flash"
            COMMAND ${CMAKE_COMMAND} -E false
            USES_TERMINAL)
        add_dependencies(littlefs-flash hw1-plaintext-littlefs-refused)
        hw1_require_flash_port(encrypted-littlefs-flash)
    endif()
    hw1_require_flash_port(littlefs-flash)

    # One-time layout installation is intentionally unavailable unless the
    # separately-built recovery image is named explicitly. This target writes
    # bootloader/table/factory/ota_0/otadata AND a blank littlefs: it is
    # first-time provisioning and it DOES destroy the filesystem. It still
    # preserves NVS. To replace only the recovery updater on a provisioned
    # device, use `factory-flash`, which touches nothing else.
    if(NOT "${_ota_updater_bin}" STREQUAL "")
        add_custom_target(hw1-ota-migration-preflight
            COMMAND "${_ota_python}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/ota/confirm_migration.py"
                --board "${_hw_board}"
            DEPENDS
                hw1-ota-pair-audit
                bootloader
                partition_table_bin
                blank_ota_data
            USES_TERMINAL
            VERBATIM)

        # Keep the low-level generated target private so the FE variant cannot
        # accidentally invoke its plaintext sibling by muscle memory.
        esptool_py_flash_target(hw1-layout-program
            "${_ota_flash_main_args}" "${_ota_flash_sub_args}")
        esptool_py_flash_target_image(hw1-layout-program bootloader
            "${CONFIG_BOOTLOADER_OFFSET_IN_FLASH}"
            "${_ota_build_dir}/bootloader/bootloader.bin")
        esptool_py_flash_target_image(hw1-layout-program partition-table
            "${CONFIG_PARTITION_TABLE_OFFSET}"
            "${_ota_build_dir}/partition_table/partition-table.bin")
        esptool_py_flash_to_partition(hw1-layout-program "factory"
            "${_ota_updater_bin}")
        esptool_py_flash_to_partition(hw1-layout-program "ota_0"
            "${_ota_build_dir}/${PROJECT_BIN}")
        esptool_py_flash_to_partition(hw1-layout-program "otadata"
            "${_ota_build_dir}/ota_data_initial.bin")
        esptool_py_flash_to_partition(hw1-layout-program "littlefs"
            "${_ota_build_dir}/littlefs.bin")
        add_dependencies(hw1-layout-program hw1-ota-migration-preflight
            littlefs_littlefs_bin)
        hw1_require_flash_port(hw1-layout-program)
        if(CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT)
            add_dependencies(encrypted-hw1-layout-program
                hw1-ota-migration-preflight)
            hw1_require_flash_port(encrypted-hw1-layout-program)
            add_custom_target(hw1-plaintext-migration-refused
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Refusing plaintext layout programming on feathers3_fe"
                COMMAND ${CMAKE_COMMAND} -E false
                USES_TERMINAL)
            add_dependencies(hw1-layout-program
                hw1-plaintext-migration-refused)
            add_custom_target(encrypted-migration-flash
                DEPENDS encrypted-hw1-layout-program)
            add_custom_target(migration-flash
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Refusing plaintext migration for feathers3_fe; use encrypted-migration-flash"
                COMMAND ${CMAKE_COMMAND} -E false
                USES_TERMINAL)
        else()
            add_custom_target(migration-flash DEPENDS hw1-layout-program)
        endif()

        # Replace ONLY the recovery updater. Before this existed, updating the
        # factory slot meant re-running the whole migration, which is why the
        # procedure used to end by destroying and rebuilding the filesystem it
        # had just set up. Writes one partition; leaves ota_0, otadata, NVS and
        # littlefs untouched. Still behind the paired-build audit, so a
        # mismatched key or layout cannot reach the device.
        esptool_py_flash_target(hw1-factory-program
            "${_ota_flash_main_args}" "${_ota_flash_sub_args}")
        esptool_py_flash_to_partition(hw1-factory-program "factory"
            "${_ota_updater_bin}")
        add_dependencies(hw1-factory-program ${_ota_pair_gate})
        hw1_require_flash_port(hw1-factory-program)
        if(CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT)
            add_dependencies(encrypted-hw1-factory-program ${_ota_pair_gate})
            hw1_require_flash_port(encrypted-hw1-factory-program)
            add_custom_target(encrypted-factory-flash
                DEPENDS encrypted-hw1-factory-program)
            add_custom_target(hw1-plaintext-factory-refused
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Refusing plaintext factory programming on feathers3_fe"
                COMMAND ${CMAKE_COMMAND} -E false
                USES_TERMINAL)
            add_dependencies(hw1-factory-program
                hw1-plaintext-factory-refused)
            add_custom_target(factory-flash
                COMMAND ${CMAKE_COMMAND} -E echo
                    "Refusing plaintext factory flash for feathers3_fe; use encrypted-factory-flash"
                COMMAND ${CMAKE_COMMAND} -E false
                USES_TERMINAL)
        else()
            add_custom_target(factory-flash DEPENDS hw1-factory-program)
        endif()

        message(WARNING
            "One-time migration target enabled. It now writes a BLANK "
            "littlefs and destroys filesystem contents; back up first. Use "
            "'idf.py migration-flash' (feathers3) or "
            "'idf.py encrypted-migration-flash' (feathers3_fe). To replace "
            "only the recovery updater on a provisioned device use "
            "'idf.py factory-flash'.")
    endif()
else()
    add_compile_definitions(HW1_OTA_LAYOUT=0)
    target_compile_definitions(${_hw1_app_component} PRIVATE HW1_OTA_LAYOUT=0)
endif()

# Add board-specific defines based on target (must be AFTER project())
if(CONFIG_IDF_TARGET_ESP32S3)
    add_compile_definitions(XIAO_ESP32S3_SENSE_ENABLED)
endif()

# arduino_events task stack, in BYTES (ESP-IDF xTaskCreate takes bytes, not
# words). The vendored components/arduino is gitignored, so this tracked
# override is the reviewable source of truth and survives a vendor resync
# (NetworkEvents.cpp wraps its default in #ifndef). History: a local cut to
# 2560 believed the unit was words ("2560 words = 10 KB") — the 4x footgun —
# leaving 2.5 KB, which the disconnect-while-connected handler chain
# (wifiEventLogger -> logSystemEvent -> systemEventPost) overflowed on every
# wifi teardown (panic observed 2026-07-20). 4096 restores the upstream size.
add_compile_definitions(ARDUINO_NETWORK_EVENT_TASK_STACK_SIZE=4096)
