#pragma once

// Headless Node policy for FeatherS3[D].
//
// This is intentionally a complete profile rather than a conditional branch
// in deployments/headless/features.h. Component CMake reads the selected file
// as plain text before the C preprocessor runs, so each board must present one
// unambiguous literal value for every source-list gate.

#undef NETWORK_FEATURE_LEVEL
#define NETWORK_FEATURE_LEVEL 3

#undef WEB_FEATURE_LEVEL
#define WEB_FEATURE_LEVEL 4

#undef CUSTOM_ENABLE_WEB_SENSORS
#define CUSTOM_ENABLE_WEB_SENSORS 0
#undef CUSTOM_ENABLE_WEB_BLUETOOTH
#define CUSTOM_ENABLE_WEB_BLUETOOTH 1
#undef CUSTOM_ENABLE_WEB_SPEECH
#define CUSTOM_ENABLE_WEB_SPEECH 0
#undef CUSTOM_ENABLE_WEB_ESPNOW
#define CUSTOM_ENABLE_WEB_ESPNOW 1
#undef CUSTOM_ENABLE_WEB_BOND
#define CUSTOM_ENABLE_WEB_BOND 0
#undef CUSTOM_ENABLE_WEB_MQTT
#define CUSTOM_ENABLE_WEB_MQTT 0
#undef CUSTOM_ENABLE_WEB_GAMES
#define CUSTOM_ENABLE_WEB_GAMES 0
#undef CUSTOM_ENABLE_WEB_MAPS
#define CUSTOM_ENABLE_WEB_MAPS 0
#undef CUSTOM_ENABLE_WEB_BATTERY
#define CUSTOM_ENABLE_WEB_BATTERY 1
#undef CUSTOM_ENABLE_WEB_R1_HEALTH
#define CUSTOM_ENABLE_WEB_R1_HEALTH 0

#undef ENABLE_HTTPS
#define ENABLE_HTTPS 0

#undef ENABLE_MQTT
#define ENABLE_MQTT 0

// The MAX17048 is board infrastructure on the primary bus (SDA 8 / SCL 9),
// not an optional sensor module. Custom level keeps the I2C core available to
// the battery backend while explicitly compiling out every selectable device.
#undef I2C_FEATURE_LEVEL
#define I2C_FEATURE_LEVEL 4

#undef CUSTOM_ENABLE_OLED
#define CUSTOM_ENABLE_OLED 0
#undef CUSTOM_ENABLE_GAMEPAD
#define CUSTOM_ENABLE_GAMEPAD 0
#undef CUSTOM_ENABLE_GPS
#define CUSTOM_ENABLE_GPS 0
#undef CUSTOM_ENABLE_IMU
#define CUSTOM_ENABLE_IMU 0
#undef CUSTOM_ENABLE_TOF
#define CUSTOM_ENABLE_TOF 0
#undef CUSTOM_ENABLE_THERMAL
#define CUSTOM_ENABLE_THERMAL 0
#undef CUSTOM_ENABLE_APDS
#define CUSTOM_ENABLE_APDS 0
#undef CUSTOM_ENABLE_FM_RADIO
#define CUSTOM_ENABLE_FM_RADIO 0
#undef CUSTOM_ENABLE_RTC
#define CUSTOM_ENABLE_RTC 0
#undef CUSTOM_ENABLE_PRESENCE
#define CUSTOM_ENABLE_PRESENCE 0
#undef CUSTOM_ENABLE_SERVO
#define CUSTOM_ENABLE_SERVO 0
#undef CUSTOM_ENABLE_LED_MATRIX
#define CUSTOM_ENABLE_LED_MATRIX 0

#undef DISPLAY_TYPE
#define DISPLAY_TYPE 0

#undef INPUT_DEVICE_TYPE
#define INPUT_DEVICE_TYPE 0

// The gauge is fixed on primary bus 0. Keep the unused vertical STEMMA QT bus
// disabled by default so this deployment does not allocate a second I2C driver
// or scan it. It remains available as an explicit runtime setting.
#undef I2C2_BUS_ENABLED_DEFAULT
#define I2C2_BUS_ENABLED_DEFAULT 0

#undef XIAO_ESP32S3_SENSE_ENABLED
#define XIAO_ESP32S3_SENSE_ENABLED 0

#undef ENABLE_CAMERA_SENSOR
#define ENABLE_CAMERA_SENSOR 0

#undef ENABLE_MICROPHONE_SENSOR
#define ENABLE_MICROPHONE_SENSOR 0

#undef ENABLE_BATTERY_MONITOR
#define ENABLE_BATTERY_MONITOR 1

#undef ENABLE_NEOPIXEL
#define ENABLE_NEOPIXEL 0

#undef ENABLE_BLUETOOTH
#define ENABLE_BLUETOOTH 1

#undef ENABLE_G2_GLASSES
#define ENABLE_G2_GLASSES 0

#undef ENABLE_R1_HEALTH
#define ENABLE_R1_HEALTH 0

#undef ENABLE_G2_TESTSUITE
#define ENABLE_G2_TESTSUITE 0

#undef ENABLE_ESP_SR
#define ENABLE_ESP_SR 0

#undef ENABLE_EDGE_IMPULSE
#define ENABLE_EDGE_IMPULSE 0

#undef ENABLE_AUTOMATION
#define ENABLE_AUTOMATION 1

#undef ENABLE_UART_HOST_LINK
#define ENABLE_UART_HOST_LINK 0

#undef ENABLE_RASPBERRY_PI_HOST_POWER
#define ENABLE_RASPBERRY_PI_HOST_POWER 0

#undef ENABLE_RASPBERRY_PI_HOST_FAN
#define ENABLE_RASPBERRY_PI_HOST_FAN 0

#undef ENABLE_BONDED_MODE
#define ENABLE_BONDED_MODE 0

#undef ENABLE_GAMES
#define ENABLE_GAMES 0

#undef ENABLE_WEB_GAME_MAZE
#define ENABLE_WEB_GAME_MAZE 0

#undef ENABLE_WEB_GAME_DARKROOM
#define ENABLE_WEB_GAME_DARKROOM 0

#undef ENABLE_LLM_BACKEND
#define ENABLE_LLM_BACKEND 0

#undef ENABLE_LLM_SOURCE_ONBOARD
#define ENABLE_LLM_SOURCE_ONBOARD 0

#undef ENABLE_LLM_SOURCE_CM5
#define ENABLE_LLM_SOURCE_CM5 0

#undef ENABLE_MAPS
#define ENABLE_MAPS 0
