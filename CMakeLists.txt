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
    set(_partitions_src
        "${CMAKE_CURRENT_SOURCE_DIR}/partitions_${_sr_flag}_${_flash_size}.csv")
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

set(PROJECT_VER "0.98.4")
project(hardwareone-idf)

# Add board-specific defines based on target (must be AFTER project())
if(CONFIG_IDF_TARGET_ESP32S3)
    add_compile_definitions(XIAO_ESP32S3_SENSE_ENABLED)
endif()
