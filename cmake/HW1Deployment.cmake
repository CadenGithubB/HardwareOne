# Shared deployment-profile loader for the main application and factory updater.
#
# A deployment is selected with:
#
#   HW_DEPLOYMENT=<family>/<board>
#
# and resolves to:
#
#   deployments/<family>/boards/<board>/contract.conf
#
# contract.conf is deliberately a small KEY=VALUE file.  CMake, Python release
# tooling, and shell wrappers can all consume the same source of truth without
# maintaining another board/layout registry in each language.

function(_hw1_contract_value _contents _key _output)
    string(REGEX MATCH "(^|\n)${_key}=([^\n\r]+)" _match "${_contents}")
    if(NOT _match)
        message(FATAL_ERROR
            "Deployment contract ${HW1_DEPLOYMENT_CONTRACT_FILE} is missing ${_key}=...")
    endif()
    set(${_output} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

# Same reader for a key a contract MAY omit.  Absent yields an empty string
# rather than a fatal error, so existing contracts stay valid unchanged.
function(_hw1_contract_optional _contents _key _output)
    string(REGEX MATCH "(^|\n)${_key}=([^\n\r]+)" _match "${_contents}")
    if(_match)
        set(${_output} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    else()
        set(${_output} "" PARENT_SCOPE)
    endif()
endfunction()

function(hw1_load_deployment _repository_root _selector)
    if("${_selector}" STREQUAL "")
        set(HW1_DEPLOYMENT_ACTIVE FALSE PARENT_SCOPE)
        set(HW1_DEPLOYMENT_SELECTOR "" PARENT_SCOPE)
        set(HW1_DEPLOYMENT_FEATURE_FILE "" PARENT_SCOPE)
        set(HW1_DEPLOYMENT_SDKCONFIG_FILE "" PARENT_SCOPE)
        set(HW1_DEPLOYMENT_PARTITION_FILE "" PARENT_SCOPE)
        return()
    endif()

    if(NOT "${_selector}" MATCHES "^([a-z0-9_]+)/([a-z0-9_]+)$")
        message(FATAL_ERROR
            "Invalid HW_DEPLOYMENT='${_selector}'. Expected <family>/<board> "
            "using lowercase letters, numbers, and underscores.")
    endif()
    set(_selector_family "${CMAKE_MATCH_1}")
    set(_selector_board "${CMAKE_MATCH_2}")
    set(_contract
        "${_repository_root}/deployments/${_selector_family}/boards/${_selector_board}/contract.conf")
    if(NOT EXISTS "${_contract}")
        message(FATAL_ERROR
            "Unknown HW_DEPLOYMENT='${_selector}': ${_contract} does not exist")
    endif()

    file(READ "${_contract}" _contract_contents)
    set(HW1_DEPLOYMENT_CONTRACT_FILE "${_contract}")
    _hw1_contract_value("${_contract_contents}" "DEPLOYMENT_ID" _deployment_id)
    _hw1_contract_value("${_contract_contents}" "BOARD_ID" _board_id)
    _hw1_contract_value("${_contract_contents}" "TARGET" _target)
    _hw1_contract_value("${_contract_contents}" "FLASH_SIZE" _flash_size)
    _hw1_contract_value("${_contract_contents}" "OTA_LAYOUT" _ota_layout)
    _hw1_contract_value("${_contract_contents}" "OTA_LAYOUT_ID" _layout_id)
    _hw1_contract_value("${_contract_contents}" "OTA_VERSION_SUFFIX" _version_suffix)
    _hw1_contract_value("${_contract_contents}" "PARTITION_CSV" _partition_csv)
    _hw1_contract_value("${_contract_contents}" "FEATURE_HEADER" _feature_header)
    _hw1_contract_value("${_contract_contents}" "MAIN_RELEASE_MAX" _main_release_max)
    _hw1_contract_value("${_contract_contents}" "UPDATER_RELEASE_MAX" _updater_release_max)
    _hw1_contract_value("${_contract_contents}" "FLASH_ENCRYPTION" _flash_encryption)
    # OPTIONAL.  A deployment whose feature policy cannot be expressed in
    # features.h alone -- because it must contradict a value in
    # boards/<board>.defaults -- names an sdkconfig fragment here.  It is
    # layered last for the main application only; see the root CMakeLists.
    _hw1_contract_optional("${_contract_contents}" "SDKCONFIG_DEFAULTS" _sdkconfig_defaults)

    if(NOT _deployment_id STREQUAL _selector_family OR
       NOT _board_id STREQUAL _selector_board)
        message(FATAL_ERROR
            "Deployment selector '${_selector}' disagrees with ${_contract}: "
            "DEPLOYMENT_ID=${_deployment_id}, BOARD_ID=${_board_id}")
    endif()
    if(NOT _ota_layout STREQUAL "1")
        message(FATAL_ERROR
            "Deployment ${_selector} must declare OTA_LAYOUT=1; non-OTA "
            "deployment contracts are not implemented yet")
    endif()
    if(NOT _flash_encryption MATCHES "^[01]$")
        message(FATAL_ERROR
            "Deployment ${_selector} FLASH_ENCRYPTION must be 0 or 1")
    endif()
    if(NOT _main_release_max MATCHES "^0[xX][0-9A-Fa-f]+$" OR
       NOT _updater_release_max MATCHES "^0[xX][0-9A-Fa-f]+$")
        message(FATAL_ERROR
            "Deployment ${_selector} release limits must be hexadecimal")
    endif()

    set(_sdkconfig_file "")
    if(NOT "${_sdkconfig_defaults}" STREQUAL "")
        get_filename_component(_sdkconfig_file
            "${_repository_root}/${_sdkconfig_defaults}" ABSOLUTE)
        if(NOT EXISTS "${_sdkconfig_file}")
            message(FATAL_ERROR
                "Deployment ${_selector} references missing file: ${_sdkconfig_file}")
        endif()
    endif()

    get_filename_component(_feature_file
        "${_repository_root}/${_feature_header}" ABSOLUTE)
    get_filename_component(_partition_file
        "${_repository_root}/${_partition_csv}" ABSOLUTE)
    foreach(_required_file IN ITEMS "${_feature_file}" "${_partition_file}")
        if(NOT EXISTS "${_required_file}")
            message(FATAL_ERROR
                "Deployment ${_selector} references missing file: ${_required_file}")
        endif()
    endforeach()

    string(TOLOWER "${_flash_size}" _flash_size_lower)
    set(HW1_DEPLOYMENT_ACTIVE TRUE PARENT_SCOPE)
    set(HW1_DEPLOYMENT_SELECTOR "${_selector}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_ID "${_deployment_id}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_BOARD "${_board_id}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_TARGET "${_target}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_FLASH_SIZE "${_flash_size_lower}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_OTA_LAYOUT TRUE PARENT_SCOPE)
    set(HW1_DEPLOYMENT_LAYOUT_ID "${_layout_id}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_VERSION_SUFFIX "${_version_suffix}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_FEATURE_FILE "${_feature_file}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_SDKCONFIG_FILE "${_sdkconfig_file}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_PARTITION_FILE "${_partition_file}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_MAIN_RELEASE_MAX "${_main_release_max}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_UPDATER_RELEASE_MAX "${_updater_release_max}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_FLASH_ENCRYPTION "${_flash_encryption}" PARENT_SCOPE)
    set(HW1_DEPLOYMENT_CONTRACT_FILE "${_contract}" PARENT_SCOPE)
endfunction()
