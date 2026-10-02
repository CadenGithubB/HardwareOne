# ESP-NOW API for a P4 host whose radio is an ESP-Hosted C6 companion. Native
# targets use the SoC's own ESP-NOW driver, so this component is empty there.
# The C6 must run Hosted firmware with the matching slave bridge; see README.md.
if(IDF_TARGET STREQUAL "esp32p4")
    idf_component_register(
        SRCS "esp_now_hosted_host.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_hosted esp_wifi freertos esp_hw_support)
else()
    idf_component_register()
endif()
