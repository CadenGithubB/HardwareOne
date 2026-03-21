# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# ─────────────────────────────────────────────────────────────────────────────
# Auto-select partition table based on ENABLE_ESP_SR in System_BuildConfig.h
# When ESP-SR is enabled, we need a "model" SPIFFS partition for WakeNet/MultiNet.
# When disabled, that space is reclaimed for LittleFS (~924K → ~3.7MB).
# ─────────────────────────────────────────────────────────────────────────────
set(_buildconfig "${CMAKE_CURRENT_SOURCE_DIR}/components/hardwareone/System_BuildConfig.h")
if(EXISTS "${_buildconfig}")
    file(READ "${_buildconfig}" _bc_contents)
    string(REGEX MATCH "#define ENABLE_ESP_SR[ \t]+([0-9]+)" _match "${_bc_contents}")
    set(_espsr_val "${CMAKE_MATCH_1}")
    if(_espsr_val STREQUAL "1")
        set(_partitions_src "${CMAKE_CURRENT_SOURCE_DIR}/partitions_sr.csv")
        message(STATUS "ENABLE_ESP_SR=1 → using partitions_sr.csv (with model partition)")
    else()
        set(_partitions_src "${CMAKE_CURRENT_SOURCE_DIR}/partitions_no_sr.csv")
        message(STATUS "ENABLE_ESP_SR=0 → using partitions_no_sr.csv (maximized LittleFS)")
    endif()
    # Copy to partitions.csv (the file sdkconfig references) if content differs
    set(_partitions_dst "${CMAKE_CURRENT_SOURCE_DIR}/partitions.csv")
    if(EXISTS "${_partitions_src}")
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
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(PROJECT_VER "0.9")
project(hardwareone-idf)

# Add board-specific defines based on target (must be AFTER project())
if(CONFIG_IDF_TARGET_ESP32S3)
    add_compile_definitions(XIAO_ESP32S3_SENSE_ENABLED)
endif()
