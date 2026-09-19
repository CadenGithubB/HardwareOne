#pragma once

// Headless Node deployment policy.
//
// System_BuildConfig.h includes this file after its editable defaults and before
// any derived rules.  Keep values as plain integer literals: component CMake
// reads the same file to decide which translation units enter the build.

#undef NETWORK_FEATURE_LEVEL
#define NETWORK_FEATURE_LEVEL 3

#undef WEB_FEATURE_LEVEL
#define WEB_FEATURE_LEVEL 4

// Keep the headless web surface deliberately narrow.  The ESP-NOW and BLE
// transports are part of this deployment, so expose their management pages;
// retain the Feather's ADC-backed battery page without pulling in the generic
// sensor/I2C surface.  Automations has its own ENABLE_AUTOMATION gate below.
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

#undef I2C_FEATURE_LEVEL
#define I2C_FEATURE_LEVEL 0

#undef DISPLAY_TYPE
#define DISPLAY_TYPE 0

#undef INPUT_DEVICE_TYPE
#define INPUT_DEVICE_TYPE 0

#undef XIAO_ESP32S3_SENSE_ENABLED
#define XIAO_ESP32S3_SENSE_ENABLED 0

#undef ENABLE_CAMERA_SENSOR
#define ENABLE_CAMERA_SENSOR 0

#undef ENABLE_MICROPHONE_SENSOR
#define ENABLE_MICROPHONE_SENSOR 0

// Feather ESP32 V2 uses the VBAT/2 divider on GPIO35. This is independent of
// I2C and deliberately remains part of the headless hardware contract. The
// FeatherS3[D] deployment has its own profile because its MAX17048 backend
// requires the I2C core while keeping every optional sensor disabled.
#undef ENABLE_BATTERY_MONITOR
#define ENABLE_BATTERY_MONITOR 1

// The on-board status pixel is local UI, not part of the Headless Node
// service contract.
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
