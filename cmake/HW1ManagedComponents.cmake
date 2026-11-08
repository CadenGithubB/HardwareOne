# Keep every build directory of one checkout in step with the shared
# managed_components/ directory. Kept independent of ESP-IDF so it can be tested
# with cmake -P (components/hardwareone/test/host/test_managed_components_stamp.py).
#
# The component manager syncs <project>/managed_components at configure time to
# the build's DEPENDENCIES_LOCK, and IDF 5.5.5 cannot move that directory per
# build. The per-target locks pin different versions (the S3 and classic-ESP32
# locks take libsodium 1.0.22~1, the P4 lock 1.0.20~4, and so on), so a P4
# configure rewrites the directory under every S3 build graph. Neither the lock
# nor managed_components is a configure dependency, so the next incremental S3
# build used to skip configure and fail in ninja on files the P4 sync removed
# (2026-11-08: espressif__libsodium/.../keccak1600.c).
#
# The stamp records the SHA-256 of the lock that last populated the directory.
# Every build directory lists the stamp (and its own lock) as configure
# dependencies, and the stamp is rewritten only when its content changes, so:
#   - a configure that syncs a different lock touches the stamp, and every other
#     build directory reconfigures (and resyncs to its own lock) on its next
#     build, before ninja looks at any managed source;
#   - builds that share a lock (all S3 boards, or two P4 build directories of
#     one checkout) never touch it, so incremental builds stay incremental.
# It is written before the sync as well as after, so a configure that fails
# after the component manager has changed the directory still invalidates the
# other build directories.
#
# PRE_SYNC also takes <stamp>.lock until POST_SYNC (or the end of the cmake
# process), so two configures of one checkout never sync at the same time.
# Same-target builds may run together; tools/checkout_build_lock.sh refuses a
# concurrent build of another target, whose sync would change sources under a
# running compile.
#
#   _lock   absolute path of the DEPENDENCIES_LOCK this configure uses (it may
#           not exist yet: a first solve creates it)
#   _stamp  absolute path of the checkout's stamp file
#   _phase  PRE_SYNC (before project()) or POST_SYNC (after it)
function(hw1_managed_components_stamp _lock _stamp _phase)
    if(NOT _phase STREQUAL "PRE_SYNC" AND NOT _phase STREQUAL "POST_SYNC")
        message(FATAL_ERROR "hw1_managed_components_stamp: unknown phase '${_phase}'")
    endif()
    get_filename_component(_stamp_dir "${_stamp}" DIRECTORY)
    file(MAKE_DIRECTORY "${_stamp_dir}")
    if(_phase STREQUAL "PRE_SYNC")
        file(LOCK "${_stamp}.lock" GUARD PROCESS TIMEOUT 0 RESULT_VARIABLE _locked)
        if(NOT _locked EQUAL 0)
            message(STATUS "Waiting for another configure of this checkout to finish syncing managed_components")
            file(LOCK "${_stamp}.lock" GUARD PROCESS TIMEOUT 1800 RESULT_VARIABLE _locked)
            if(NOT _locked EQUAL 0)
                message(FATAL_ERROR "Could not lock ${_stamp}.lock: ${_locked}")
            endif()
        endif()
    endif()

    if(EXISTS "${_lock}")
        file(SHA256 "${_lock}" _key)
    else()
        # A first solve will create the lock; the POST_SYNC call records it.
        set(_key "unsolved")
    endif()
    set(_content "${_key}\n")
    set(_previous "")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _previous)
    endif()
    if(NOT _previous STREQUAL _content)
        file(WRITE "${_stamp}" "${_content}")
    endif()

    if(_phase STREQUAL "POST_SYNC")
        file(LOCK "${_stamp}.lock" RELEASE RESULT_VARIABLE _released)
        # A changed lock (a pull, or a re-solve in another build directory
        # that shares it) also means a resync: IDF does not list it as an input.
        if(NOT CMAKE_SCRIPT_MODE_FILE)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_stamp}")
            if(EXISTS "${_lock}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_lock}")
            endif()
        endif()
    endif()
endfunction()
