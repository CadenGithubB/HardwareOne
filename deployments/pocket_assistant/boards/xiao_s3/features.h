#pragma once

// Pocket Assistant deployment policy for the Seeed XIAO ESP32-S3.
//
// System_BuildConfig.h includes this file after its editable defaults and
// before any derived rules.  Keep values as plain integer literals: the root
// build and the component CMake read the same file as TEXT, before the C
// preprocessor runs, to decide which translation units enter the build.  A
// computed expression here would be read as its literal and silently
// disagree with what the compiler resolves.
//
// This is intentionally a complete profile rather than an include of the
// headless one plus deltas, for the same reason the FeatherS3 headless board
// carries its own copy: each board must present one unambiguous literal for
// every source-list gate.
//
// The profile is "Headless Node service set, plus the wearable link":
//   * kept from headless — Wi-Fi, HTTP web UI, ESP-NOW mesh, BLE, automations,
//     serial CLI, auth, LittleFS, remote management, signed recovery OTA
//   * added here        — G2 glasses, R1 ring health, and the authenticated
//                         CM5/Pi UART host link (power, fan, LLM source)
//   * still excluded    — I2C entirely, displays, local input, MQTT, HTTPS,
//                         camera, on-board PDM mic, ESP-SR, on-device LLM,
//                         maps, games, bonded mode

#undef NETWORK_FEATURE_LEVEL
#define NETWORK_FEATURE_LEVEL 3

#undef WEB_FEATURE_LEVEL
#define WEB_FEATURE_LEVEL 4

// Web surface: the transports this deployment actually owns (BLE, ESP-NOW),
// the R1 vitals page, and the CM5 host-power page.  No sensors page — there
// is no I2C bus behind it.  No battery page — the XIAO has no battery
// telemetry hardware at all.  Automations has its own ENABLE_AUTOMATION gate.
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
#define CUSTOM_ENABLE_WEB_BATTERY 0
#undef CUSTOM_ENABLE_WEB_R1_HEALTH
#define CUSTOM_ENABLE_WEB_R1_HEALTH 1
#undef CUSTOM_ENABLE_WEB_POWER
#define CUSTOM_ENABLE_WEB_POWER 1

#undef ENABLE_HTTPS
#define ENABLE_HTTPS 0

#undef ENABLE_MQTT
#define ENABLE_MQTT 0

// No I2C at all.  The XIAO has no on-board I2C infrastructure this deployment
// needs — no fuel gauge (unlike FeatherS3[D]), no display, no input device —
// so level 0 is honest rather than a custom level with everything zeroed.
// Level 0 also zeroes every ENABLE_*_SENSOR and ENABLE_OLED_DISPLAY on its
// own; the two selections below are still explicit because HAL_Display.cpp and
// the input drivers are gated on DISPLAY_TYPE / INPUT_DEVICE_TYPE directly.
#undef I2C_FEATURE_LEVEL
#define I2C_FEATURE_LEVEL 0

#undef DISPLAY_TYPE
#define DISPLAY_TYPE 0

#undef INPUT_DEVICE_TYPE
#define INPUT_DEVICE_TYPE 0

// Deliberately LEFT AT 1, unlike the headless profiles.
//
// On this board the flag does not mean "compile the camera and mic in" — it
// selects which XIAO pin block System_BuildConfig.h's board chain matches.
// The Sense block is the one that defines SD_CS_PIN/SD_SCK_PIN/SD_MISO_PIN/
// SD_MOSI_PIN and the UART_LINK_* pins this deployment depends on; the base
// XIAO block defines no SD card.  Setting it to 0 here would take the microSD
// card away from the G2 icon packs and capture tree for no benefit.
//
// The camera and PDM mic are compiled out below by their own flags instead,
// which is what those flags are for.
#undef XIAO_ESP32S3_SENSE_ENABLED
#define XIAO_ESP32S3_SENSE_ENABLED 1

// On-board Sense silicon is NOT part of this deployment.  Both auto-enable
// from the board chain above, so both must be explicitly undone here — this
// file is included after that #ifndef-guarded auto-detection has run.
//
// This does not remove the microphone FEATURE: ENABLE_MICROPHONE is
// (ENABLE_MICROPHONE_SENSOR || (ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)) and
// both of those are 1 below, so the source-agnostic mic layer (HAL_Audio,
// System_Microphone, live audio, dictation) still compiles and the G2 glasses
// microphone is the audio source.  ENABLE_MICROPHONE_SENSOR stays strictly
// "on-board PDM silicon is driven", which here it is not.
#undef ENABLE_CAMERA_SENSOR
#define ENABLE_CAMERA_SENSOR 0

#undef ENABLE_MICROPHONE_SENSOR
#define ENABLE_MICROPHONE_SENSOR 0

// The XIAO defines BATTERY_MONITOR_AVAILABLE 0 — no connector, no gauge, no
// VBAT divider.  Nothing to sample, so the subsystem stays out.
#undef ENABLE_BATTERY_MONITOR
#define ENABLE_BATTERY_MONITOR 0

// No on-board addressable pixel (NEOPIXEL_PIN_DEFAULT is -1); a 1 here is a
// compile error by design.
#undef ENABLE_NEOPIXEL
#define ENABLE_NEOPIXEL 0

// --- The wearable link: BLE radio, G2 glasses, R1 ring ---
//
// A 1 here is only a REQUEST; the derived rules force it to 0 unless the
// active sdkconfig actually carries Bluedroid.  boards/xiao_s3.defaults sets
// CONFIG_BT_ENABLED=n, so this deployment's sdkconfig.defaults overlay turns
// the stack back on.  Without that overlay every flag below silently
// collapses to 0 and the image builds green with no glasses support at all.
#undef ENABLE_BLUETOOTH
#define ENABLE_BLUETOOTH 1

#undef ENABLE_G2_GLASSES
#define ENABLE_G2_GLASSES 1

#undef ENABLE_R1_HEALTH
#define ENABLE_R1_HEALTH 1

// The on-glasses transport test bench is a development tool, not part of the
// product surface, and it is expensive: ~25 KB for the page plus ~42 KB of
// Q-series image probes whose only caller is its dispatch table.
#undef ENABLE_G2_TESTSUITE
#define ENABLE_G2_TESTSUITE 0

#undef ENABLE_ESP_SR
#define ENABLE_ESP_SR 0

#undef ENABLE_EDGE_IMPULSE
#define ENABLE_EDGE_IMPULSE 0

#undef ENABLE_AUTOMATION
#define ENABLE_AUTOMATION 1

// --- The CM5 / Pi 5 co-processor link ---
//
// UART0 on the XIAO's D6/D7 pads (GPIO43/44), which is what the carrier wires
// to the Pi's uart2.  Compile-time only: the runtime switch is
// gSettings.uartLinkEnabled and it still defaults off.
#undef ENABLE_UART_HOST_LINK
#define ENABLE_UART_HOST_LINK 1

#undef ENABLE_RASPBERRY_PI_HOST_POWER
#define ENABLE_RASPBERRY_PI_HOST_POWER 1

#undef ENABLE_RASPBERRY_PI_HOST_FAN
#define ENABLE_RASPBERRY_PI_HOST_FAN 1

#undef ENABLE_BONDED_MODE
#define ENABLE_BONDED_MODE 0

#undef ENABLE_GAMES
#define ENABLE_GAMES 0

#undef ENABLE_WEB_GAME_MAZE
#define ENABLE_WEB_GAME_MAZE 0

#undef ENABLE_WEB_GAME_DARKROOM
#define ENABLE_WEB_GAME_DARKROOM 0

// The assistant answers through the CM5, which is the whole point of the UART
// link.  The on-device engine is deliberately NOT a source here: it is by far
// the largest single contributor to the image, and 8 MB of flash already has
// to hold Bluedroid plus the G2/R1 stack.  Enabling it needs a bigger board,
// not a bigger partition.
#undef ENABLE_LLM_BACKEND
#define ENABLE_LLM_BACKEND 1

#undef ENABLE_LLM_SOURCE_ONBOARD
#define ENABLE_LLM_SOURCE_ONBOARD 0

#undef ENABLE_LLM_SOURCE_CM5
#define ENABLE_LLM_SOURCE_CM5 1

#undef ENABLE_MAPS
#define ENABLE_MAPS 0
