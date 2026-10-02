// Handheld deployment policy for the Espressif ESP32-P4X-EYE.
//
// System_BuildConfig.h includes this file after its editable defaults and the
// board profile (boards/p4x_eye.features.h), before any derived rules. Keep
// values as plain integer literals: the root build and the component CMake
// read the same file as TEXT, before the C preprocessor runs, to decide which
// translation units enter the build. A computed expression here would be read
// as its literal and silently disagree with what the compiler resolves.
//
// This is intentionally a complete profile rather than an include of the board
// profile plus deltas, for the same reason the other deployments carry their
// own copies: a release must present one unambiguous literal for every
// source-list gate, independent of what a developer later changes in the
// board profile. It mirrors the configuration qualified on hardware on
// 2026-09-29 (experiments/stt_p4 and its predecessors).
//
// The profile is the "Standard Handheld" product on this board:
//   * display, wheel and buttons, MIPI camera, PDM microphone, battery
//   * Wi-Fi/web, BLE with G2 glasses and R1 ring, ESP-NOW mesh through the
//     onboard ESP32-C6 (ESP-Hosted), ESP-SR wake word, fully local STT
//   * excluded: I2C, HTTPS, MQTT, bonded mode, automations, Pi UART link,
//     games, maps, on-device LLM, Edge Impulse, and the microSD card until it
//     is qualified with a card fitted
//
// Factory-only release: there is no recovery updater for this board yet, so
// the image is flashed over USB (see MIGRATION.md).
#pragma once


// Board identity: pulls in System_Board_P4X_EYE.h wiring.
#undef HW_BOARD_P4X_EYE
#define HW_BOARD_P4X_EYE 1

// --- Onboard peripherals -----------------------------------------------------
// 240x240 ST7789 SPI LCD rendering the monochrome UI.
#undef DISPLAY_TYPE
#define DISPLAY_TYPE 4
// Clickable encoder wheel plus the three user buttons.
#undef INPUT_DEVICE_TYPE
#define INPUT_DEVICE_TYPE 3
#undef ENABLE_GPIO_ENCODER_BUTTONS
#define ENABLE_GPIO_ENCODER_BUTTONS 1
// No I2C sensors or I2C display on this board.
#undef I2C_FEATURE_LEVEL
#define I2C_FEATURE_LEVEL 0
#undef XIAO_ESP32S3_SENSE_ENABLED
#define XIAO_ESP32S3_SENSE_ENABLED 0
#undef ENABLE_NEOPIXEL
#define ENABLE_NEOPIXEL 0

#undef ENABLE_CAMERA_SENSOR
#define ENABLE_CAMERA_SENSOR 1
#undef ENABLE_MICROPHONE_SENSOR
#define ENABLE_MICROPHONE_SENSOR 1
#undef ENABLE_BATTERY_MONITOR
#define ENABLE_BATTERY_MONITOR 1

// microSD (SDMMC slot 0) is implemented but not yet qualified with a card on
// this board. Set both to 1 to try it; see docs/P4X_EYE_PERIPHERALS.md.
#undef ENABLE_SDMMC_CARD
#define ENABLE_SDMMC_CARD 0
#undef ENABLE_SD_CARD
#define ENABLE_SD_CARD 0

// --- Radio (onboard ESP32-C6 through ESP-Hosted) -----------------------------
#undef NETWORK_FEATURE_LEVEL
#define NETWORK_FEATURE_LEVEL 3
#undef ENABLE_BLUETOOTH
#define ENABLE_BLUETOOTH 1
#undef ENABLE_BLE_IDF_EXPERIMENTAL
#define ENABLE_BLE_IDF_EXPERIMENTAL 0
#undef ENABLE_G2_GLASSES
#define ENABLE_G2_GLASSES 1
#undef ENABLE_R1_HEALTH
#define ENABLE_R1_HEALTH 1
#undef ENABLE_G2_TESTSUITE
#define ENABLE_G2_TESTSUITE 0
#undef ENABLE_HTTPS
#define ENABLE_HTTPS 0
#undef ENABLE_MQTT
#define ENABLE_MQTT 0
#undef ENABLE_BONDED_MODE
#define ENABLE_BONDED_MODE 0

// --- Web UI ------------------------------------------------------------------
#undef WEB_FEATURE_LEVEL
#define WEB_FEATURE_LEVEL 4
#undef CUSTOM_ENABLE_WEB_SENSORS
#define CUSTOM_ENABLE_WEB_SENSORS 1
#undef CUSTOM_ENABLE_WEB_BLUETOOTH
#define CUSTOM_ENABLE_WEB_BLUETOOTH 1
#undef CUSTOM_ENABLE_WEB_SPEECH
#define CUSTOM_ENABLE_WEB_SPEECH 1
#undef CUSTOM_ENABLE_WEB_ESPNOW
#define CUSTOM_ENABLE_WEB_ESPNOW 1
#undef CUSTOM_ENABLE_WEB_BATTERY
#define CUSTOM_ENABLE_WEB_BATTERY 1
#undef CUSTOM_ENABLE_WEB_BOND
#define CUSTOM_ENABLE_WEB_BOND 0
#undef CUSTOM_ENABLE_WEB_MQTT
#define CUSTOM_ENABLE_WEB_MQTT 0
#undef CUSTOM_ENABLE_WEB_GAMES
#define CUSTOM_ENABLE_WEB_GAMES 0
#undef CUSTOM_ENABLE_WEB_MAPS
#define CUSTOM_ENABLE_WEB_MAPS 0
// Omitted to fit local speech-to-text; ring support itself stays enabled.
#undef CUSTOM_ENABLE_WEB_R1_HEALTH
#define CUSTOM_ENABLE_WEB_R1_HEALTH 0

// --- Speech ------------------------------------------------------------------
#undef ENABLE_ESP_SR
#define ENABLE_ESP_SR 1
// ESP-SR models load from LittleFS, keeping the no_sr partition layout.
#undef HW1_SR_MODELS_IN_FILESYSTEM
#define HW1_SR_MODELS_IN_FILESYSTEM 1
// Fully local transcription on the P4 (no Pi/UART, Wi-Fi or cloud).
#undef ENABLE_LOCAL_STT
#define ENABLE_LOCAL_STT 1

// --- Not used on this board --------------------------------------------------
#undef ENABLE_EDGE_IMPULSE
#define ENABLE_EDGE_IMPULSE 0
#undef ENABLE_AUTOMATION
#define ENABLE_AUTOMATION 0
#undef ENABLE_UART_HOST_LINK
#define ENABLE_UART_HOST_LINK 0
#undef ENABLE_RASPBERRY_PI_HOST_POWER
#define ENABLE_RASPBERRY_PI_HOST_POWER 0
#undef ENABLE_RASPBERRY_PI_HOST_FAN
#define ENABLE_RASPBERRY_PI_HOST_FAN 0
#undef ENABLE_GAMES
#define ENABLE_GAMES 0
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
