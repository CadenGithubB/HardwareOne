get_filename_component(repo_dir "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(codec_dir "${repo_dir}/components/hardwareone")
set(generated_header "${CMAKE_CURRENT_BINARY_DIR}/jpeg_fixtures.h")
if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(GLOB fixture_files CONFIGURE_DEPENDS "${codec_dir}/test/host/jpeg_fixtures/*.jpg")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${fixture_files} "${CMAKE_CURRENT_LIST_DIR}/../embed_fixtures.py")
endif()
execute_process(
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../embed_fixtures.py"
        "${codec_dir}/test/host/jpeg_fixtures" "${generated_header}"
    RESULT_VARIABLE fixture_result)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR "Could not embed the JPEG fixture corpus")
endif()
set(jpeg_requires espressif__esp_jpeg espressif__esp32-camera esp_timer)
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND jpeg_requires esp_driver_jpeg)
endif()
idf_component_register(
    SRCS "probe.cpp" "${codec_dir}/HAL_JPEG.cpp"
        "${codec_dir}/HAL_JPEG_Software.cpp" "${codec_dir}/HAL_JPEG_P4.cpp"
    INCLUDE_DIRS "${codec_dir}" "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES ${jpeg_requires})

# Component Manager is disabled for this self-contained probe. Reproduce the
# camera manifest's JPEG dependency without editing its verified source copy.
idf_component_get_property(camera_library espressif__esp32-camera COMPONENT_LIB)
target_link_libraries(${camera_library} PRIVATE idf::espressif__esp_jpeg)
