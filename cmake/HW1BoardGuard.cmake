# Refuse a configure that would pair one board's saved sdkconfig with another
# board's defaults. Kept independent of ESP-IDF so it can be tested with
# cmake -P (components/hardwareone/test/host/test_bare_idf_guard.py).
#
# A bare `idf.py build` (no HW_BOARD, no deployment) defaults the board per
# target (feathers3 on esp32s3), but IDF keeps every value already saved in the
# sdkconfig: the tracked config/sdkconfig is a XIAO (8 MB) configuration, so the
# result combined XIAO settings with the FeatherS3 16 MB partition table and
# rewrote the tracked file. Supported builds go through tools/build_board.sh or
# tools/build_deployment.sh, which always name the board and use a per-build
# sdkconfig.
#
#   _repo            project source directory
#   _sdkconfig       absolute path of the sdkconfig this configure will use
#   _board_explicit  TRUE when HW_BOARD was given or a deployment chose the board
#   _board           the board in effect (possibly defaulted), may be empty
#   _board_file      boards/<board>.defaults, may be empty
function(hw1_check_board_selection _repo _sdkconfig _board_explicit _board _board_file)
    if(_board_explicit)
        return()
    endif()
    set(_hint
        "Build with the per-board wrapper instead, which names the board and "
        "keeps its own sdkconfig:\n"
        "    tools/build_board.sh <board> [idf.py actions]\n"
        "(boards: the names of boards/*.defaults), or set HW_BOARD explicitly.")
    get_filename_component(_tracked "${_repo}/config/sdkconfig" ABSOLUTE)
    get_filename_component(_used "${_sdkconfig}" ABSOLUTE)
    if(_used STREQUAL _tracked)
        message(FATAL_ERROR
            "A bare idf.py configure (no HW_BOARD) would use and rewrite the "
            "tracked config/sdkconfig and default the board to '${_board}'. "
            ${_hint})
    endif()
    if(NOT _board_file OR NOT EXISTS "${_board_file}" OR NOT EXISTS "${_used}")
        return()
    endif()
    foreach(_key IN ITEMS VARIANT FLASH)
        if(_key STREQUAL "VARIANT")
            set(_regex "^CONFIG_ARDUINO_VARIANT=\"[^\"]*\"")
        else()
            set(_regex "^CONFIG_ESPTOOLPY_FLASHSIZE_[0-9]+MB=y")
        endif()
        file(STRINGS "${_board_file}" _want REGEX "${_regex}")
        file(STRINGS "${_used}" _have REGEX "${_regex}")
        if(_want AND _have AND NOT _want STREQUAL _have)
            message(FATAL_ERROR
                "HW_BOARD is unset, so the board defaulted to '${_board}' "
                "(${_want}), but ${_used} was saved for a different board "
                "(${_have}). " ${_hint})
        endif()
    endforeach()
endfunction()
