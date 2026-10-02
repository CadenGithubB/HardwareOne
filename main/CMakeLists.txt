# main requires every component (no REQUIRES list), so P4-only components such
# as esp_now_hosted are visible here without naming them.
idf_component_register(
    SRCS "hardwareone-idf.cpp"
    INCLUDE_DIRS "."
)
