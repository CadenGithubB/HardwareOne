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
// board profile.
//
// The profile is the "Standard Handheld" product on this board. It contains:
//   * display, wheel and buttons, MIPI camera, PDM microphone, battery
//   * the microSD card (SDMMC slot 0)
//   * Wi-Fi/web, BLE with G2 glasses and R1 ring, ESP-NOW mesh through the
//     onboard ESP32-C6 (ESP-Hosted)
//   * speech: the ESP-SR wake word, and fully local speech-to-text with the
//     Citrinet-384 model read from the card ("/sd/STT Models/"), its
//     language-model decoder, session speaker labels and QuickNote
//   * excluded: I2C, HTTPS, MQTT, bonded mode, automations, Pi UART link,
//     games, maps, the assistant (LLM), Edge Impulse, the phone link
//
// microSD and local speech-to-text were switched on here on 2026-11-20. The
// parts run daily on this board in the saved configuration `p4-portable`
// (card, local STT, glasses, ring; no camera, no wake word). This exact
// selection, with the camera and the wake word next to local STT, has not
// been built or run on hardware as one image: its size against the release
// gate (contract.conf) and a wake-word plus dictation session, which share
// the PDM microphone, are checked before a release is made from it.
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

// microSD in SDMMC slot 0 (docs/P4X_EYE_PERIPHERALS.md). Both switches are
// written: this profile does not rely on System_BuildConfig.h deriving the
// second from the first. Local speech-to-text below needs the card.
#undef ENABLE_SDMMC_CARD
#define ENABLE_SDMMC_CARD 1
#undef ENABLE_SD_CARD
#define ENABLE_SD_CARD 1

// --- Radio (onboard ESP32-C6 through ESP-Hosted) -----------------------------
#undef NETWORK_FEATURE_LEVEL
#define NETWORK_FEATURE_LEVEL 3
#undef ENABLE_BLUETOOTH
#define ENABLE_BLUETOOTH 1
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
// Fully local transcription on the P4 (no Pi/UART, Wi-Fi or cloud). It runs
// its acoustic model from the microSD card, so it needs the card switches
// above (the build refuses local STT without them). One model per build: this
// deployment builds Citrinet-384 ("/sd/STT Models/citrinet384-2000h-c90.p4.stt"),
// the model the portable runs. CMake reads these switches as text to pick the
// runtime sources, so each is a bare literal here. A QuartzNet build would
// set ENABLE_STT_MODEL_QUARTZNET 1, ENABLE_STT_MODEL_CITRINET 0 and
// ENABLE_STT_MODEL_15X5_GEN2 1 instead.
#undef ENABLE_LOCAL_STT
#define ENABLE_LOCAL_STT 1
#undef ENABLE_STT_MODEL_CITRINET
#define ENABLE_STT_MODEL_CITRINET 1
#undef ENABLE_STT_MODEL_QUARTZNET
#define ENABLE_STT_MODEL_QUARTZNET 0
#undef ENABLE_STT_MODEL_15X5_GEN2
#define ENABLE_STT_MODEL_15X5_GEN2 0
// Session speaker labels: saved transcript lines become "[Person N] text" when
// "/sd/STT Models/speaker.spk" is on the card (without it nothing changes).
// The build needs the ESP-IDF PIE context-switch fix (MIGRATION.md).
#undef ENABLE_STT_SPEAKER_LABELS
#define ENABLE_STT_SPEAKER_LABELS 1
// Private spoken notes (ring hold on a HW1 lens page, Notes page, `note` CLI),
// transcribed locally and saved under /stt/u<id>/notes/.
#undef ENABLE_QUICKNOTE
#define ENABLE_QUICKNOTE 1

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
