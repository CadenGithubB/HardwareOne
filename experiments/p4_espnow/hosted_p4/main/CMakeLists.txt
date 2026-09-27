idf_component_register(
    SRCS "main.c" "../../bridge/esp_now_hosted_host.c"
    INCLUDE_DIRS "." "../../bridge" "../../native_s3/main"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer esp_driver_gpio)
