# Task-watchdog soak. These guards do not need ESP-IDF, so
# components/hardwareone/test/host/test_wdt_soak_config.py runs them with
# cmake -P.

# Kconfig values stick in a build directory's sdkconfig, so the soak must never
# share one with an ordinary build. Refuse it unless the build directory name
# ends in -wdt-soak and the sdkconfig lives inside that directory.
function(hw1_wdt_soak_check_build_dir _binary_dir _sdkconfig _defaults)
    get_filename_component(_dir_name "${_binary_dir}" NAME)
    if(NOT _dir_name MATCHES "-wdt-soak$")
        message(FATAL_ERROR
            "HW1_WDT_SOAK needs its own build directory (name ending in -wdt-soak) "
            "so an ordinary build never inherits the watchdog panic. Use "
            "tools/build_board.sh <board> [--config <name>] --wdt-soak.")
    endif()
    # The shared config/sdkconfig (or config/sdkconfig.ota) would keep the
    # panic after the soak; only a build-local sdkconfig is allowed.
    get_filename_component(_sdk_dir "${_sdkconfig}" DIRECTORY)
    get_filename_component(_sdk_dir "${_sdk_dir}" REALPATH)
    get_filename_component(_bin_dir "${_binary_dir}" REALPATH)
    if(NOT _sdk_dir STREQUAL _bin_dir)
        message(FATAL_ERROR
            "HW1_WDT_SOAK needs SDKCONFIG inside its build directory, not ${_sdkconfig}. "
            "Use tools/build_board.sh <board> [--config <name>] --wdt-soak.")
    endif()
    if(NOT EXISTS "${_defaults}")
        message(FATAL_ERROR "Missing watchdog soak overlay: ${_defaults}")
    endif()
endfunction()

# After project(): a stale sdkconfig must not give an app that subscribes the
# loop while the watchdog only prints, or a timeout other than the overlay's.
function(hw1_wdt_soak_check_resolved _defaults _panic _timeout)
    file(STRINGS "${_defaults}" _timeout_line
        REGEX "^CONFIG_ESP_TASK_WDT_TIMEOUT_S=[0-9]+$")
    string(REGEX REPLACE "^.*=" "" _want_timeout "${_timeout_line}")
    if("${_want_timeout}" STREQUAL "")
        message(FATAL_ERROR "Watchdog soak overlay sets no timeout: ${_defaults}")
    endif()
    if(NOT "${_panic}" STREQUAL "y" OR NOT "${_timeout}" STREQUAL "${_want_timeout}")
        message(FATAL_ERROR
            "Watchdog soak build resolved PANIC='${_panic}', TIMEOUT_S='${_timeout}' "
            "(overlay asks for y, ${_want_timeout}); run reconfigure in this "
            "-wdt-soak directory")
    endif()
endfunction()
