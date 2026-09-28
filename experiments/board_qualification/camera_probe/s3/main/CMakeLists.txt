idf_component_register(SRCS "probe.c" INCLUDE_DIRS "." REQUIRES espressif__esp32-camera espressif__esp_jpeg esp_timer)
idf_component_get_property(camera_library espressif__esp32-camera COMPONENT_LIB)
target_link_libraries(${camera_library} PRIVATE idf::espressif__esp_jpeg)
