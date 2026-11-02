#pragma once
// Waveshare LCD-5: HX8394 display and ES8311 speaker support.
// ES7210 onboard microphone capture; optional OV5647 is enabled by a saved profile.
// Optional application choices belong in the saved bring-up configuration.
#undef HW_BOARD_WAVESHARE_P4_TOUCH
#define HW_BOARD_WAVESHARE_P4_TOUCH 1
#undef DISPLAY_TYPE
#define DISPLAY_TYPE 5
#undef INPUT_DEVICE_TYPE
#define INPUT_DEVICE_TYPE 4
#undef I2C_FEATURE_LEVEL
#define I2C_FEATURE_LEVEL 1
#undef XIAO_ESP32S3_SENSE_ENABLED
#define XIAO_ESP32S3_SENSE_ENABLED 0
#undef ENABLE_NEOPIXEL
#define ENABLE_NEOPIXEL 0
#undef ENABLE_CAMERA_SENSOR
#define ENABLE_CAMERA_SENSOR 0
#undef ENABLE_MICROPHONE_SENSOR
#define ENABLE_MICROPHONE_SENSOR 1
#undef ENABLE_BATTERY_MONITOR
#define ENABLE_BATTERY_MONITOR 1
#undef ENABLE_SDMMC_CARD
#define ENABLE_SDMMC_CARD 1
#undef HW1_SR_MODELS_IN_FILESYSTEM
#define HW1_SR_MODELS_IN_FILESYSTEM 1

#undef ENABLE_SPEAKER
#define ENABLE_SPEAKER 1
