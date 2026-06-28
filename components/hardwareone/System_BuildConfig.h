#ifndef SYSTEM_BUILDCONFIG_H
#define SYSTEM_BUILDCONFIG_H

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║                     USER CONFIGURATION - EDIT HERE                        ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║  Flags are grouped into five domains:                                     ║
// ║    1. Connectivity         — WiFi / HTTP / ESP-NOW / web pages / MQTT     ║
// ║    2. I/O Hardware         — I2C sensors / display / camera / mic / batt  ║
// ║    3. Wireless Peripherals — BLE radio / G2 Glasses                       ║
// ║    4. On-device Subsystems — Speech / Edge Impulse / LLM / Automation     ║
// ║    5. User-facing Apps     — Games / Maps                                 ║
// ║                                                                           ║
// ║  Flag conventions:                                                        ║
// ║    *_FEATURE_LEVEL — coarse preset (0–4); 4 means "use CUSTOM_* below"    ║
// ║    CUSTOM_ENABLE_* — per-item toggle, only read when level == 4           ║
// ║    ENABLE_*        — top-level boolean, always read directly              ║
// ║                                                                           ║
// ║  All cross-feature dependency overrides (e.g. "WEB_SPEECH off when        ║
// ║  ESP_SR off") live in the DERIVED section near the bottom of this file.   ║
// ║  Edit that section only when adding new derivation rules.                 ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

// ─────────────────────────────────────────────────────────────────────────────
// ⚠️  CMakeLists.txt PARSES THE FOLLOWING FLAGS BY REGEX
// ─────────────────────────────────────────────────────────────────────────────
// These flags are read as plain text by components/hardwareone/CMakeLists.txt
// to decide which .cpp files get compiled (a coarse first-pass filter — the
// per-file `#if` guards remain the fine-grained control inside each .cpp):
//
//     NETWORK_FEATURE_LEVEL, WEB_FEATURE_LEVEL, I2C_FEATURE_LEVEL,
//     DISPLAY_TYPE,
//     ENABLE_MQTT, ENABLE_HTTPS, ENABLE_ONDEVICE_LLM,
//     ENABLE_MAPS, ENABLE_AUTOMATION, ENABLE_EDGE_IMPULSE, ENABLE_GAMES
//
// The regex expects EXACTLY this shape, on a single non-indented line:
//     #define NAME <integer>
//
// Do NOT use parentheses, inline comments, expressions, or indentation on
// these specific lines, e.g. all of the following will silently fall back
// to 0 and strip features out of the build:
//     #define WEB_FEATURE_LEVEL (4)            // parentheses break the match
//     #define WEB_FEATURE_LEVEL 4  // comment  // trailing comment may break
//       #define WEB_FEATURE_LEVEL 4            // indented — not matched
//
// Other flags in this file are only read by the C++ preprocessor and can use
// any valid macro form.
// ─────────────────────────────────────────────────────────────────────────────


// =============================================================================
// 1. CONNECTIVITY  —  WiFi / HTTP / ESP-NOW / web pages
// =============================================================================
// The network stack and any service that runs on top of it. WiFi must be on
// for HTTP / ESP-NOW; the per-page web flags only matter when HTTP_SERVER is
// also on. Disabling a web page removes its .cpp from the binary entirely;
// runtime toggles can't bring it back.

// Network level: WiFi / HTTP / ESP-NOW combinations.
//   0 = DISABLED    - No networking
//   1 = WIFI_ONLY   - WiFi without HTTP server
//   2 = WIFI_HTTP   - WiFi + HTTP server
//   3 = WIFI_ESPNOW - WiFi + HTTP + ESP-NOW mesh
//   4 = CUSTOM      - Use individual CUSTOM_ENABLE_NET_* flags below
#define NETWORK_FEATURE_LEVEL   4

#if NETWORK_FEATURE_LEVEL == 4
  #define CUSTOM_ENABLE_NET_WIFI     1   // WiFi connectivity
  #define CUSTOM_ENABLE_NET_HTTP     1   // HTTP server (web UI)
  #define CUSTOM_ENABLE_NET_ESPNOW   1   // ESP-NOW mesh networking
#endif

// Web level: which feature pages compile in.
//   0 = DISABLED  - No HTTP server, no web UI
//   1 = CORE      - Core UI only (no extra modules)
//   2 = STANDARD  - Core UI + common modules
//   3 = FULL      - Core UI + all modules
//   4 = CUSTOM    - Use individual CUSTOM_ENABLE_WEB_* flags below
#define WEB_FEATURE_LEVEL       4

#if WEB_FEATURE_LEVEL == 4
  // Per-page compile-time gates. Each maps to one WebPage_*.cpp file. A page
  // also auto-disables when its dependency is missing (e.g. WEB_SPEECH needs
  // ENABLE_ESP_SR, WEB_ESPNOW needs ENABLE_ESPNOW) — see DERIVED rules below.
  #define CUSTOM_ENABLE_WEB_SENSORS    1
  #define CUSTOM_ENABLE_WEB_BLUETOOTH  1
  #define CUSTOM_ENABLE_WEB_SPEECH     1
  #define CUSTOM_ENABLE_WEB_ESPNOW     1
  #define CUSTOM_ENABLE_WEB_BOND       1
  #define CUSTOM_ENABLE_WEB_MQTT       0
  #define CUSTOM_ENABLE_WEB_GAMES      0
  #define CUSTOM_ENABLE_WEB_MAPS       0
  #define CUSTOM_ENABLE_WEB_BATTERY    1
#endif

// HTTPS: TLS-encrypted HTTP. Self-signed or uploaded certs in /system/certs/.
// Runtime toggle: gSettings.httpsEnabled (admin can flip via web UI).
// Auto-disabled if HTTP server is off.
#define ENABLE_HTTPS            1

// MQTT: Home Assistant integration via MQTT broker.
// Auto-disabled if WiFi is off.
#define ENABLE_MQTT             0


// =============================================================================
// 2. I/O HARDWARE  —  Sensors, display, camera, mic, battery
// =============================================================================
// On-board peripherals. The I2C system covers the sensor breakouts and the
// OLED; camera, microphone, and battery monitor are independent.

// I2C level: bus + sensor selection.
//   0 = DISABLED   - No I2C (max memory savings)
//   1 = OLED_ONLY  - OLED display only
//   2 = STANDALONE - OLED + Gamepad
//   3 = FULL       - OLED + all sensors
//   4 = CUSTOM     - Use individual CUSTOM_ENABLE_* flags below
// Set to DISABLED for the XIAO ESP32S3 Sense build (camera + mic only,
// no breakout sensors, no OLED) — the Sense expansion has no I2C
// breakouts wired and no on-board display.
#define I2C_FEATURE_LEVEL       4

#if I2C_FEATURE_LEVEL == 4
  // Memory hints (rough — full breakdown in "MEMORY SAVINGS REFERENCE" below).
  #define CUSTOM_ENABLE_OLED        1   // SSD1306 OLED display
  #define CUSTOM_ENABLE_GAMEPAD     1   // Adafruit Seesaw gamepad
  #define CUSTOM_ENABLE_GPS         0   // PA1010D GPS module — disabled 2026-06-07 (not used)
  #define CUSTOM_ENABLE_IMU         0   // BNO055 IMU — not installed
  #define CUSTOM_ENABLE_TOF         0   // VL53L4CX ToF — not installed
  #define CUSTOM_ENABLE_THERMAL     1   // MLX90640 thermal camera
  #define CUSTOM_ENABLE_APDS        0   // APDS9960 gesture/proximity — not installed
  #define CUSTOM_ENABLE_FM_RADIO    0   // RDA5807 FM radio — disabled 2026-06-07 (not used)
  #define CUSTOM_ENABLE_RTC         0   // DS3231 precision RTC — disabled 2026-06-07 (not used)
  #define CUSTOM_ENABLE_PRESENCE    0   // STHS34PF80 IR presence/motion — disabled 2026-06-07 (not used)
  #define CUSTOM_ENABLE_SERVO       0   // PCA9685 servo controller — not installed
#endif

// Display: hardware display selection. 0 forces all OLED_*.cpp out of the
// build via the CMakeLists DISPLAY_TYPE gate.
//   0 = NONE, 1 = SSD1306 (OLED), 2 = ST7789 (TFT), 3 = ILI9341 (TFT)
#define DISPLAY_TYPE            1

// Input device: which physical input controller is wired to the I2C bus.
// Exactly one driver compiles in (mutually exclusive — both share STEMMA QT
// and would collide if both ran). CMakeLists gates the .cpp file just like
// DISPLAY_TYPE does.
//   0 = NONE             (no input device)
//   1 = SEESAW_GAMEPAD   (Adafruit Mini I2C Gamepad, 0x50)
//   2 = ANO_ENCODER      (Adafruit ANO Rotary Encoder breakout, 0x49)
#define INPUT_DEVICE_TYPE       1

// Camera: ESP32-S3 DVP camera (OV2640/OV3660/OV5640). PICO board has none.
// Auto-enabled on the XIAO ESP32S3 Sense, which ships with an OV2640 on its
// expansion board (camera GPIOs are hardcoded for that board in
// System_Camera_DVP.cpp). Still overridable by a pre-define; all other boards
// default off.
#ifndef ENABLE_CAMERA_SENSOR
  #if defined(ARDUINO_XIAO_ESP32S3_SENSE_DEV) || (defined(ARDUINO_XIAO_ESP32S3_DEV) && defined(XIAO_ESP32S3_SENSE_ENABLED))
    #define ENABLE_CAMERA_SENSOR  1
  #else
    #define ENABLE_CAMERA_SENSOR  0
  #endif
#endif

// Microphone: PDM microphone via I2S. PICO board has none. Auto-enabled on the
// XIAO ESP32S3 Sense, which has an onboard PDM mic (CLK=GPIO42, DATA=GPIO41).
// Overridable by a pre-define; all other boards default off.
#ifndef ENABLE_MICROPHONE_SENSOR
  #if defined(ARDUINO_XIAO_ESP32S3_SENSE_DEV) || (defined(ARDUINO_XIAO_ESP32S3_DEV) && defined(XIAO_ESP32S3_SENSE_ENABLED))
    #define ENABLE_MICROPHONE_SENSOR  1
  #else
    #define ENABLE_MICROPHONE_SENSOR  0
  #endif
#endif

// Battery monitor: enables the System_Battery subsystem. The actual backend
// (ADC voltage divider vs. MAX17048G I2C fuel gauge vs. USB-only stub) is
// auto-selected per board further down — see BATTERY_BACKEND_* in the board
// hardware section. Leave commented to auto-default to BATTERY_MONITOR_AVAILABLE
// for the active board; set explicitly to 0 to force-disable on a board that
// HAS the hardware (slim USB-only build), or 1 to force-enable on a board that
// claims it doesn't (e.g., to develop against a wired-up dev rig).
// #define ENABLE_BATTERY_MONITOR 0


// =============================================================================
// 3. WIRELESS PERIPHERALS  —  BLE radio + G2 Glasses
// =============================================================================
// BLE radio and the devices it talks to over GATT.

// Bluetooth: BLE server with GATT services. Required for G2 Glasses below.
//
// **NB:** this flag only gates OUR application code. To actually free the
// ~14 KB IRAM + ~70 KB flash + ~80 KB DRAM that the ESP-IDF Bluedroid
// stack consumes, you also need `CONFIG_BT_ENABLED=n` in sdkconfig.
// (Both flags are kept in sync below — see sdkconfig.defaults.)
#define ENABLE_BLUETOOTH        1

// Even G2 Smart Glasses: BLE client to connect to Even Realities G2 glasses.
// ESP32 acts as BLE central; mutually exclusive with phone BLE at runtime.
// Auto-disabled if ENABLE_BLUETOOTH=0.
#define ENABLE_G2_GLASSES       1

// VFS root for G2 animated icon packs (BMP frames): SD card (`/sd/...`).
// Keeps pack data off LittleFS; requires SD mounted (web + lens picker use VFS).
#define G2_ICON_ANIMATIONS_VFS_PATH "/sd/g2_icon_animations"


// =============================================================================
// 4. ON-DEVICE SUBSYSTEMS  —  Inference, voice, scheduling
// =============================================================================
// Independent subsystems. Some compose with other domains: bonded mode rides
// on ESP-NOW (Connectivity); the speech web page surfaces ESP-SR.

// Speech recognition (ESP-SR): WakeNet wake word + MultiNet command grammar.
// Runtime: srstart / srstop. Web page: CUSTOM_ENABLE_WEB_SPEECH (Connectivity).
#define ENABLE_ESP_SR           0

// Edge Impulse: ML inference engine.
#define ENABLE_EDGE_IMPULSE     0

// On-device LLM: tiny transformer inference (Llama + GPT-2 architectures).
// Requires ESP32-S3 + PSRAM. Models load from LittleFS or SD. FP32 / INT8.
// Typical PSRAM usage: 1–4 MB at runtime.
#define ENABLE_ONDEVICE_LLM     0
#if ENABLE_ONDEVICE_LLM
// Max KV / attention context in tokens (0 = use checkpoint's seq_len only).
// Lower uses less PSRAM; must cover prompt + max generation.
// Typical tiny models: 256–1024.
#define LLM_MAX_CONTEXT_LEN     1024
#endif

// Automation: scheduled tasks + conditional commands.
#define ENABLE_AUTOMATION       1

// Bonded mode: two-device bonded pair via ESP-NOW (master/worker).
// Master shows remote UI for worker features, manifest cached in LittleFS.
// Auto-disabled if ESP-NOW is off.
#define ENABLE_BONDED_MODE      1


// =============================================================================
// 5. USER-FACING APPS  —  Composed features shipped as products
// =============================================================================
// Higher-level features built on top of the subsystems above. Each gates
// its own web page and (where applicable) OLED mode.

// Games: browser-based games web page (served at /games).
//   ENABLE_GAMES is the master switch; then pick exactly ONE game below. Both
//   games are raw-embedded in the firmware image, and shipping both at once
//   exceeds the app partition, so a build-time guard rejects enabling both.
//   Default game is the Tilt Maze (preserves prior behaviour).
//   To ship A Dark Room instead: ENABLE_GAMES 1, ENABLE_WEB_GAME_MAZE 0,
//   ENABLE_WEB_GAME_DARKROOM 1 (and CUSTOM_ENABLE_WEB_GAMES 1 at web level 4).
#define ENABLE_GAMES            0
#define ENABLE_WEB_GAME_MAZE        1   // Tilt Maze (IMU/gamepad prototype)
#define ENABLE_WEB_GAME_DARKROOM    0   // A Dark Room (en/es/fr/zh_cn)

// Maps: offline maps and waypoints web page.
#define ENABLE_MAPS             0


// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║                    END OF USER CONFIGURATION                              ║
// ╚═══════════════════════════════════════════════════════════════════════════╝


// =============================================================================
// FEATURE LEVEL CONSTANTS (do not modify)
// =============================================================================

#define I2C_LEVEL_DISABLED   0
#define I2C_LEVEL_OLED_ONLY  1
#define I2C_LEVEL_STANDALONE 2
#define I2C_LEVEL_FULL       3
#define I2C_LEVEL_CUSTOM     4

#define NET_LEVEL_DISABLED   0
#define NET_LEVEL_WIFI_ONLY  1
#define NET_LEVEL_WIFI_HTTP  2
#define NET_LEVEL_WIFI_ESPNOW 3
#define NET_LEVEL_CUSTOM     4

#define WEB_LEVEL_DISABLED   0
#define WEB_LEVEL_CORE       1
#define WEB_LEVEL_STANDARD   2
#define WEB_LEVEL_FULL       3
#define WEB_LEVEL_CUSTOM     4

#define DISPLAY_TYPE_NONE      0
#define DISPLAY_TYPE_SSD1306   1
#define DISPLAY_TYPE_ST7789    2
#define DISPLAY_TYPE_ILI9341   3

#define INPUT_DEVICE_TYPE_NONE           0
#define INPUT_DEVICE_TYPE_SEESAW_GAMEPAD 1
#define INPUT_DEVICE_TYPE_ANO_ENCODER    2

// =============================================================================
// DERIVED FLAGS (automatically set based on I2C_FEATURE_LEVEL)
// =============================================================================

#if I2C_FEATURE_LEVEL == I2C_LEVEL_DISABLED
  // Level 0: Everything disabled
  #define ENABLE_I2C_SYSTEM       0
  #define ENABLE_OLED_DISPLAY     0
  #define ENABLE_THERMAL_SENSOR   0
  #define ENABLE_TOF_SENSOR       0
  #define ENABLE_IMU_SENSOR       0
  #define ENABLE_GAMEPAD_SENSOR   0
  #define ENABLE_APDS_SENSOR      0
  #define ENABLE_GPS_SENSOR       0
  #define ENABLE_FM_RADIO         0
  #define ENABLE_RTC_SENSOR       0
  #define ENABLE_PRESENCE_SENSOR  0
  #define ENABLE_SERVO            0

#elif I2C_FEATURE_LEVEL == I2C_LEVEL_OLED_ONLY
  // Level 1: OLED only, no sensors
  #define ENABLE_I2C_SYSTEM       1
  #define ENABLE_OLED_DISPLAY     1
  #define ENABLE_THERMAL_SENSOR   0
  #define ENABLE_TOF_SENSOR       0
  #define ENABLE_IMU_SENSOR       0
  #define ENABLE_GAMEPAD_SENSOR   0
  #define ENABLE_APDS_SENSOR      0
  #define ENABLE_GPS_SENSOR       0
  #define ENABLE_FM_RADIO         0
  #define ENABLE_RTC_SENSOR       0
  #define ENABLE_PRESENCE_SENSOR  0
  #define ENABLE_SERVO            0

#elif I2C_FEATURE_LEVEL == I2C_LEVEL_STANDALONE
  // Level 2: OLED + Gamepad for standalone device control
  #define ENABLE_I2C_SYSTEM       1
  #define ENABLE_OLED_DISPLAY     1
  #define ENABLE_THERMAL_SENSOR   0
  #define ENABLE_TOF_SENSOR       0
  #define ENABLE_IMU_SENSOR       0
  #define ENABLE_GAMEPAD_SENSOR   1
  #define ENABLE_APDS_SENSOR      0
  #define ENABLE_GPS_SENSOR       0
  #define ENABLE_FM_RADIO         0
  #define ENABLE_RTC_SENSOR       0
  #define ENABLE_PRESENCE_SENSOR  0
  #define ENABLE_SERVO            0

#elif I2C_FEATURE_LEVEL == I2C_LEVEL_FULL
  // Level 3: Everything enabled
  #define ENABLE_I2C_SYSTEM       1
  #define ENABLE_OLED_DISPLAY     1
  #define ENABLE_THERMAL_SENSOR   1
  #define ENABLE_TOF_SENSOR       1
  #define ENABLE_IMU_SENSOR       1
  #define ENABLE_GAMEPAD_SENSOR   1
  #define ENABLE_APDS_SENSOR      1
  #define ENABLE_GPS_SENSOR       1
  #define ENABLE_FM_RADIO         1
  #define ENABLE_RTC_SENSOR       1
  #define ENABLE_PRESENCE_SENSOR  1
  #define ENABLE_SERVO            1

#else  // I2C_LEVEL_CUSTOM
  // Level 4: Custom sensor selection from user config section
  #define ENABLE_I2C_SYSTEM       1
  #define ENABLE_OLED_DISPLAY     CUSTOM_ENABLE_OLED
  #define ENABLE_THERMAL_SENSOR   CUSTOM_ENABLE_THERMAL
  #define ENABLE_TOF_SENSOR       CUSTOM_ENABLE_TOF
  #define ENABLE_IMU_SENSOR       CUSTOM_ENABLE_IMU
  #define ENABLE_GAMEPAD_SENSOR   CUSTOM_ENABLE_GAMEPAD
  #define ENABLE_APDS_SENSOR      CUSTOM_ENABLE_APDS
  #define ENABLE_GPS_SENSOR       CUSTOM_ENABLE_GPS
  #define ENABLE_FM_RADIO         CUSTOM_ENABLE_FM_RADIO
  #define ENABLE_RTC_SENSOR       CUSTOM_ENABLE_RTC
  #define ENABLE_PRESENCE_SENSOR  CUSTOM_ENABLE_PRESENCE
  #define ENABLE_SERVO            CUSTOM_ENABLE_SERVO

#endif

// Override ENABLE_OLED_DISPLAY if DISPLAY_TYPE is NONE
#if DISPLAY_TYPE == DISPLAY_TYPE_NONE
  #undef ENABLE_OLED_DISPLAY
  #define ENABLE_OLED_DISPLAY 0
#endif

// =============================================================================
// DERIVED INPUT DEVICE FLAGS (based on INPUT_DEVICE_TYPE)
// =============================================================================
// Mutually exclusive: selecting ANO_ENCODER forces ENABLE_GAMEPAD_SENSOR off,
// because both occupy the same role (the OLED input source) and the seesaw
// driver would race the ANO driver for the STEMMA QT bus if both compiled in.
// The CMakeLists gate also skips the disabled driver's .cpp.
#if INPUT_DEVICE_TYPE == INPUT_DEVICE_TYPE_ANO_ENCODER
  #undef  ENABLE_GAMEPAD_SENSOR
  #define ENABLE_GAMEPAD_SENSOR 0
  #define ENABLE_ANO_ENCODER    1
#else
  #define ENABLE_ANO_ENCODER    0
#endif

// Convenience: "is there ANY input device that drives the OLED UI?" Used to
// gate the OLED's input-handling code so it compiles for either driver.
// The two source-specific paths inside each block stay gated on their own
// ENABLE_* flag.
#define ENABLE_OLED_INPUT  (ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER)

// =============================================================================
// DERIVED NETWORK FLAGS (based on NETWORK_FEATURE_LEVEL)
// =============================================================================

#if NETWORK_FEATURE_LEVEL == NET_LEVEL_DISABLED
  #define ENABLE_WIFI         0
  #define ENABLE_ESPNOW       0
#elif NETWORK_FEATURE_LEVEL == NET_LEVEL_WIFI_ONLY
  #define ENABLE_WIFI         1
  #define ENABLE_ESPNOW       0
#elif NETWORK_FEATURE_LEVEL == NET_LEVEL_WIFI_HTTP
  #define ENABLE_WIFI         1
  #define ENABLE_ESPNOW       0
#elif NETWORK_FEATURE_LEVEL == NET_LEVEL_WIFI_ESPNOW
  #define ENABLE_WIFI         1
  #define ENABLE_ESPNOW       1
#else // NET_LEVEL_CUSTOM
  #define ENABLE_WIFI         CUSTOM_ENABLE_NET_WIFI
  #define ENABLE_ESPNOW       CUSTOM_ENABLE_NET_ESPNOW
#endif

// =============================================================================
// DERIVED WEB FLAGS (based on WEB_FEATURE_LEVEL)
// =============================================================================

#undef ENABLE_HTTP_SERVER
#if !ENABLE_WIFI
  #define ENABLE_HTTP_SERVER 0
#elif NETWORK_FEATURE_LEVEL == NET_LEVEL_CUSTOM
  // Custom level: use explicit flag (but still gated by WEB_FEATURE_LEVEL)
  #if !CUSTOM_ENABLE_NET_HTTP || WEB_FEATURE_LEVEL == WEB_LEVEL_DISABLED
    #define ENABLE_HTTP_SERVER 0
  #else
    #define ENABLE_HTTP_SERVER 1
  #endif
#elif NETWORK_FEATURE_LEVEL < NET_LEVEL_WIFI_HTTP
  #define ENABLE_HTTP_SERVER 0
#elif WEB_FEATURE_LEVEL == WEB_LEVEL_DISABLED
  #define ENABLE_HTTP_SERVER 0
#else
  #define ENABLE_HTTP_SERVER 1
#endif

// Migration tool: backup/restore endpoints for the HardwareOne Migration Tool.
// Defaults to whatever ENABLE_HTTP_SERVER is — turning HTTP on/off flips this
// in lockstep, matching the historical single-gate behavior. Set explicitly
// to 1 with ENABLE_HTTP_SERVER=0 to ship a headless build that still offers
// FTS restore-from-backup as a recovery path. The authenticated /api/backup
// endpoint requires ENABLE_HTTP_SERVER=1 (uses auth helpers from
// WebServer_Server); the unauthenticated FTS restore-only server is
// self-contained and works under either configuration.
#ifndef ENABLE_MIGRATION_TOOL
#define ENABLE_MIGRATION_TOOL ENABLE_HTTP_SERVER
#endif

// httpd type stubs — provided whenever the real <esp_http_server.h> isn't
// pulled in by this TU. The typedef alias is compatible with the real
// header (same struct tag), so it's safe even when both are visible.
// Function-level stubs that DO conflict live in System_SensorStubs.h and
// are guarded against ENABLE_MIGRATION_TOOL there.
#if !ENABLE_HTTP_SERVER
  #ifndef HW_HTTPD_TYPES_DEFINED
    #define HW_HTTPD_TYPES_DEFINED 1
    typedef struct httpd_req httpd_req_t;
    typedef void* httpd_handle_t;
  #endif
#endif

#if WEB_FEATURE_LEVEL == WEB_LEVEL_DISABLED
  #define ENABLE_WEB_SENSORS    0
  #define ENABLE_WEB_BLUETOOTH  0
  #define ENABLE_WEB_SPEECH     0
  #define ENABLE_WEB_ESPNOW     0
  #define ENABLE_WEB_BOND       0
  #define ENABLE_WEB_MQTT       0
  #define ENABLE_WEB_GAMES      0
  #define ENABLE_WEB_MAPS       0
  #define ENABLE_WEB_BATTERY    0
#elif WEB_FEATURE_LEVEL == WEB_LEVEL_CORE
  #define ENABLE_WEB_SENSORS    0
  #define ENABLE_WEB_BLUETOOTH  0
  #define ENABLE_WEB_SPEECH     0
  #define ENABLE_WEB_ESPNOW     0
  #define ENABLE_WEB_BOND       0
  #define ENABLE_WEB_MQTT       0
  #define ENABLE_WEB_GAMES      0
  #define ENABLE_WEB_MAPS       0
  #define ENABLE_WEB_BATTERY    0
#elif WEB_FEATURE_LEVEL == WEB_LEVEL_STANDARD
  #define ENABLE_WEB_SENSORS    1
  #define ENABLE_WEB_BLUETOOTH  0
  #define ENABLE_WEB_SPEECH     0
  #define ENABLE_WEB_ESPNOW     1
  #define ENABLE_WEB_BOND       1
  #define ENABLE_WEB_MQTT       1
  #define ENABLE_WEB_GAMES      0
  #define ENABLE_WEB_MAPS       0
  #define ENABLE_WEB_BATTERY    1
#elif WEB_FEATURE_LEVEL == WEB_LEVEL_FULL
  #define ENABLE_WEB_SENSORS    1
  #define ENABLE_WEB_BLUETOOTH  1
  #define ENABLE_WEB_SPEECH     1
  #define ENABLE_WEB_ESPNOW     1
  #define ENABLE_WEB_BOND       1
  #define ENABLE_WEB_MQTT       1
  #define ENABLE_WEB_GAMES      1
  #define ENABLE_WEB_MAPS       1
  #define ENABLE_WEB_BATTERY    1
#else // WEB_LEVEL_CUSTOM
  #define ENABLE_WEB_SENSORS    CUSTOM_ENABLE_WEB_SENSORS
  #define ENABLE_WEB_BLUETOOTH  CUSTOM_ENABLE_WEB_BLUETOOTH
  #define ENABLE_WEB_SPEECH     CUSTOM_ENABLE_WEB_SPEECH
  #define ENABLE_WEB_ESPNOW     CUSTOM_ENABLE_WEB_ESPNOW
  #define ENABLE_WEB_BOND       CUSTOM_ENABLE_WEB_BOND
  #define ENABLE_WEB_MQTT       CUSTOM_ENABLE_WEB_MQTT
  #define ENABLE_WEB_GAMES      CUSTOM_ENABLE_WEB_GAMES
  #define ENABLE_WEB_MAPS       CUSTOM_ENABLE_WEB_MAPS
  #define ENABLE_WEB_BATTERY    CUSTOM_ENABLE_WEB_BATTERY
#endif

// =============================================================================
// CROSS-FEATURE DEPENDENCY OVERRIDES
// =============================================================================
// Force-disable consequents when their prerequisites are missing. These rules
// run AFTER the per-domain derivations above so a CUSTOM-level user choice
// (e.g. CUSTOM_ENABLE_WEB_SPEECH=1) gets transparently zeroed when the
// dependency it needs (ENABLE_ESP_SR) is itself off.
//
// Each block is a single "if prerequisite missing → drop dependents" rule.
// Adding a new rule: pick the right prerequisite, undef + redefine each
// dependent. Don't try to predict combinations — let the rules compose.

// HTTP server gates the entire web surface.
#if !ENABLE_HTTP_SERVER
  #undef ENABLE_WEB_SENSORS
  #undef ENABLE_WEB_BLUETOOTH
  #undef ENABLE_WEB_SPEECH
  #undef ENABLE_WEB_ESPNOW
  #undef ENABLE_WEB_BOND
  #undef ENABLE_WEB_MQTT
  #undef ENABLE_WEB_GAMES
  #undef ENABLE_WEB_MAPS
  #undef ENABLE_WEB_BATTERY
  #define ENABLE_WEB_SENSORS    0
  #define ENABLE_WEB_BLUETOOTH  0
  #define ENABLE_WEB_SPEECH     0
  #define ENABLE_WEB_ESPNOW     0
  #define ENABLE_WEB_BOND       0
  #define ENABLE_WEB_MQTT       0
  #define ENABLE_WEB_GAMES      0
  #define ENABLE_WEB_MAPS       0
  #define ENABLE_WEB_BATTERY    0
#endif

// HTTPS rides on top of HTTP server.
#if !ENABLE_HTTP_SERVER && ENABLE_HTTPS
  #undef ENABLE_HTTPS
  #define ENABLE_HTTPS 0
#endif

// Bluetooth web page needs the BLE radio.
#if !ENABLE_BLUETOOTH
  #undef ENABLE_WEB_BLUETOOTH
  #define ENABLE_WEB_BLUETOOTH 0
#endif

// Speech web page needs the SR engine.
#if !ENABLE_ESP_SR
  #undef ENABLE_WEB_SPEECH
  #define ENABLE_WEB_SPEECH 0
#endif

// ESP-NOW pages (mesh + bonded-mode UI) need the ESP-NOW transport.
#if !ENABLE_ESPNOW
  #undef ENABLE_WEB_ESPNOW
  #undef ENABLE_WEB_BOND
  #define ENABLE_WEB_ESPNOW 0
  #define ENABLE_WEB_BOND 0
#endif

// MQTT web page needs the MQTT subsystem.
#if !ENABLE_MQTT
  #undef ENABLE_WEB_MQTT
  #define ENABLE_WEB_MQTT 0
#endif

// Apps web pages mirror their app flag.
#if !ENABLE_GAMES
  #undef ENABLE_WEB_GAMES
  #define ENABLE_WEB_GAMES 0
#endif

// Per-game selection sits beneath the games web subsystem. If web games are
// off (master off, or the web feature level disables them), force both off.
#if !ENABLE_WEB_GAMES
  #undef ENABLE_WEB_GAME_MAZE
  #undef ENABLE_WEB_GAME_DARKROOM
  #define ENABLE_WEB_GAME_MAZE     0
  #define ENABLE_WEB_GAME_DARKROOM 0
#endif
// Both games are raw-embedded; shipping both overflows the app partition.
#if ENABLE_WEB_GAME_MAZE && ENABLE_WEB_GAME_DARKROOM
  #error "Enable only ONE web game (ENABLE_WEB_GAME_MAZE or ENABLE_WEB_GAME_DARKROOM). Both are raw-embedded and exceed the app partition; gzip-embed if you need both."
#endif
#if !ENABLE_MAPS
  #undef ENABLE_WEB_MAPS
  #define ENABLE_WEB_MAPS 0
#endif

// MQTT subsystem itself requires WiFi (broker is over TCP).
#if !ENABLE_WIFI
  #undef ENABLE_MQTT
  #define ENABLE_MQTT 0
#endif

// Bonded mode rides on ESP-NOW.
#if !ENABLE_ESPNOW
  #undef ENABLE_BONDED_MODE
  #define ENABLE_BONDED_MODE 0
#endif

// Note: ENABLE_ONDEVICE_LLM requires ESP32-S3 with PSRAM.
// CMakeLists.txt handles excluding LLM source files on non-S3 targets.

// =============================================================================
// MEMORY SAVINGS REFERENCE
// =============================================================================
// Level DISABLED (0): Maximum savings
//   - All I2C code excluded (~100KB+ flash, ~50KB+ RAM saved)
//
// Level OLED_ONLY (1): Moderate savings  
//   - OLED display works for boot progress, menus, status
//   - Sensors disabled (~80KB flash, ~45KB RAM saved vs FULL)
//
// Level STANDALONE (2): OLED + Gamepad + Bluetooth
//   - Standalone device control via gamepad
//   - BLE communication with smart glasses
//   - GAMEPAD: +8-12KB flash, +6KB RAM (Seesaw)
//
// Level FULL (3): All features enabled
//   - THERMAL: +20-25KB flash, +15KB RAM (MLX90640)
//   - TOF:     +25-30KB flash, +10KB RAM (VL53L4CX)
//   - IMU:     +12-18KB flash, +8KB RAM  (BNO055)
//   - GAMEPAD: +8-12KB flash,  +6KB RAM  (Seesaw)
//   - APDS:    +6-10KB flash,  +4KB RAM  (APDS9960)
//   - GPS:     +5-8KB flash,   +4KB RAM  (PA1010D)
//   - FM:      +5-8KB flash,   +3KB RAM  (SI4713)
//   - PRESENCE:+4-6KB flash,   +2KB RAM  (STHS34PF80)

// =============================================================================
// BOARD HARDWARE CONFIGURATION
// =============================================================================
// Supported boards are auto-detected via Arduino board defines from menuconfig.
// Each board has specific I2C pins, NeoPixel pins, and battery monitoring.
//
// To add a new board:
//   1. Set CONFIG_ARDUINO_VARIANT in menuconfig (Component config -> Arduino)
//   2. The define is: ARDUINO_<UPPERCASE_VARIANT>_DEV (e.g., adafruit_qtpy_esp32 -> ARDUINO_ADAFRUIT_QTPY_ESP32_DEV)
//   3. Add a new #elif block below with the appropriate pin definitions
//   4. Update the BOARD_NAME string for identification
// =============================================================================

// --- Adafruit QT Py ESP32 (ESP32-PICO) ---
#if defined(ARDUINO_ADAFRUIT_QTPY_ESP32_DEV)
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Adafruit QT Py ESP32"
  
  // I2C Bus Configuration (using Wire1 for sensor breakouts)
  #define I2C_SDA_PIN_DEFAULT   22
  #define I2C_SCL_PIN_DEFAULT   19
  
  // NeoPixel Configuration
  #define NEOPIXEL_PIN_DEFAULT  5
  #define NEOPIXEL_POWER_PIN    8
  #define NEOPIXEL_COUNT_DEFAULT 1
  
  // Battery Monitoring (no built-in battery monitor on QT Py)
  #define BATTERY_ADC_PIN       -1
  #define BATTERY_MONITOR_AVAILABLE 0
  #define BATTERY_BACKEND_ADC        0
  #define BATTERY_BACKEND_FUEL_GAUGE 0

// --- Adafruit Feather ESP32 V2 ---
#elif defined(ARDUINO_ADAFRUIT_FEATHER_ESP32_V2_DEV)
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Adafruit Feather ESP32 V2"
  
  // I2C Bus Configuration (primary Wire bus)
  #define I2C_SDA_PIN_DEFAULT   22
  #define I2C_SCL_PIN_DEFAULT   20
  
  // NeoPixel Configuration
  #define NEOPIXEL_PIN_DEFAULT  0
  #define NEOPIXEL_POWER_PIN    2
  #define NEOPIXEL_COUNT_DEFAULT 1
  
  // Battery Monitoring — Feather V2 has the VBAT/2 ADC divider on GPIO35.
  #define BATTERY_ADC_PIN       35
  #define BATTERY_MONITOR_AVAILABLE 1
  #define BATTERY_BACKEND_ADC        1
  #define BATTERY_BACKEND_FUEL_GAUGE 0

// --- Seeed Studio XIAO ESP32S3 Sense (with camera/mic expansion) ---
// Note: Sense uses same variant as base XIAO ESP32S3, expansion board is add-on hardware
// To enable Sense-specific features, define XIAO_ESP32S3_SENSE_ENABLED in your build
// IMPORTANT: This block MUST come before the base XIAO block so it matches first
#elif defined(ARDUINO_XIAO_ESP32S3_SENSE_DEV) || (defined(ARDUINO_XIAO_ESP32S3_DEV) && defined(XIAO_ESP32S3_SENSE_ENABLED))
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Seeed XIAO ESP32S3 Sense"
  
  // I2C Bus Configuration (same as base XIAO ESP32S3)
  #define I2C_SDA_PIN_DEFAULT   5   // GPIO5 (D4)
  #define I2C_SCL_PIN_DEFAULT   6   // GPIO6 (D5)
  
  // NeoPixel Configuration (no built-in NeoPixel)
  #define NEOPIXEL_PIN_DEFAULT  -1
  #define NEOPIXEL_POWER_PIN    -1
  #define NEOPIXEL_COUNT_DEFAULT 0
  
  // User LED - DISABLED on Sense board (GPIO21 is used for SD_CS)
  // The expansion board SD card takes priority over the base board LED
  #define USER_LED_PIN          -1
  #define USER_LED_ACTIVE_LOW   1
  
  // Battery Monitoring (XIAO Sense has no battery monitoring hardware)
  #define BATTERY_ADC_PIN       -1
  #define BATTERY_MONITOR_AVAILABLE 0
  #define BATTERY_BACKEND_ADC        0
  #define BATTERY_BACKEND_FUEL_GAUGE 0

  // Sense-specific: SD Card (directly on expansion board)
  // Verified working via sddiag: CS=21, SCK=7, MISO=8, MOSI=9
  #define SD_CS_PIN             21  // GPIO21 (directly on expansion board)
  #define SD_SCK_PIN            7   // GPIO7
  #define SD_MISO_PIN           8   // GPIO8
  #define SD_MOSI_PIN           9   // GPIO9
  
  // Sense-specific: Camera (directly on expansion board, I2C on GPIO39/40)
  #define CAMERA_AVAILABLE      1
  
  // Sense-specific: Digital Microphone PDM
  #define MIC_CLK_PIN           42  // GPIO42 (PDM clock)
  #define MIC_DATA_PIN          41  // GPIO41 (PDM data)

// --- Seeed Studio XIAO ESP32S3 (base board without expansion) ---
// Set CONFIG_ARDUINO_VARIANT="XIAO_ESP32S3" in menuconfig
// Note: To use with Sense expansion board, define XIAO_ESP32S3_SENSE_ENABLED
#elif defined(ARDUINO_XIAO_ESP32S3_DEV)
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Seeed XIAO ESP32S3"
  
  // I2C Bus Configuration (D4=SDA, D5=SCL per Seeed pinout)
  #define I2C_SDA_PIN_DEFAULT   5   // GPIO5 (D4)
  #define I2C_SCL_PIN_DEFAULT   6   // GPIO6 (D5)
  
  // NeoPixel Configuration (no built-in NeoPixel)
  #define NEOPIXEL_PIN_DEFAULT  -1
  #define NEOPIXEL_POWER_PIN    -1
  #define NEOPIXEL_COUNT_DEFAULT 0
  
  // User LED (active low on GPIO21)
  #define USER_LED_PIN          21
  #define USER_LED_ACTIVE_LOW   1
  
  // Battery Monitoring (no dedicated ADC pin - requires external wiring)
  #define BATTERY_ADC_PIN       -1
  #define BATTERY_MONITOR_AVAILABLE 0
  #define BATTERY_BACKEND_ADC        0
  #define BATTERY_BACKEND_FUEL_GAUGE 0

// --- Unexpected Maker FeatherS3 / FeatherS3[D] ---
// Set CONFIG_ARDUINO_VARIANT="um_feathers3" in menuconfig.
// Same variant macro covers the original FeatherS3 and the newer Series[D]
// variant (FeatherS3[D]); the pin map below targets the [D] specifically —
// see notes on VBAT/fuel-gauge below.
//
// IMPORTANT — PSRAM mode differs from the XIAO ESP32-S3:
//   FeatherS3 uses QUAD PSRAM, not Octal. Flip
//     CONFIG_SPIRAM_MODE_OCT=y  →  CONFIG_SPIRAM_MODE_QUAD=y
//   in sdkconfig.defaults.esp32s3, then `idf.py fullclean && idf.py build`.
//   Building with the wrong PSRAM mode produces a board that boots fine but
//   has no PSRAM (LLM, large web buffers will OOM).
#elif defined(ARDUINO_UM_FEATHERS3_DEV)
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Unexpected Maker FeatherS3[D]"

  // I2C Bus Configuration — I2C1 (horizontal STEMMA QT, always-on LDO).
  // This is the same bus as the on-board MAX17048G fuel gauge at 0x36.
  // The board also has a second I2C bus (I2C2 on GPIO15/16, vertical
  // STEMMA QT, powered by LDO2) that the current codebase does not use.
  #define I2C_SDA_PIN_DEFAULT   8
  #define I2C_SCL_PIN_DEFAULT   9

  // NeoPixel Configuration — single on-board RGB LED on GPIO40.
  // GPIO39 enables LDO2, which powers the RGB LED (and I2C2). Driving
  // it HIGH (matching System_NeoPixel.cpp behavior) turns the LED on.
  #define NEOPIXEL_PIN_DEFAULT  40
  #define NEOPIXEL_POWER_PIN    39
  #define NEOPIXEL_COUNT_DEFAULT 1

  // Battery Monitoring — the FeatherS3[D] REMOVED the VBAT/2 ADC divider
  // present on the original FeatherS3 and replaced it with the MAX17048G
  // fuel gauge on I2C1 @ 0x36 (same horizontal STEMMA QT bus as the RTC
  // and other always-on sensors). The driver lives in i2csensor_max17048.cpp
  // and is selected via BATTERY_BACKEND_FUEL_GAUGE below; System_Battery
  // dispatches to it instead of the ADC path. Bus assignment defaults to
  // bus 0 (gSettings.fuelGaugeBus) because that's where the chip is wired.
  // (For the original FeatherS3 with VBAT_SENSE on GPIO2, set
  //  BATTERY_ADC_PIN=2, BATTERY_BACKEND_ADC=1, BATTERY_BACKEND_FUEL_GAUGE=0
  //  instead — and BATTERY_MONITOR_AVAILABLE=1.)
  #define BATTERY_ADC_PIN       -1
  #define BATTERY_MONITOR_AVAILABLE 1
  #define BATTERY_BACKEND_ADC        0
  #define BATTERY_BACKEND_FUEL_GAUGE 1

  // VBUS sense — the FeatherS3/[D] routes USB VBUS to GPIO 34 through a
  // voltage divider. Reads HIGH when USB is plugged in, LOW when not.
  // This is the only deterministic "is USB connected?" signal on the board:
  // the MAX17048's CRATE register lags by 30-60s after USB plug/unplug, so
  // relying on it alone makes the OLED stick on "USB" for almost a minute
  // after the user unplugs the cable. With this pin wired, System_Battery
  // overrides the CRATE-based heuristic with a direct GPIO read.
  // (Pin source: components/arduino/variants/um_feathers3/pins_arduino.h.)
  #define BATTERY_VBUS_SENSE_PIN 34

  // Second I2C bus — the FeatherS3[D] exposes the vertical STEMMA QT
  // connector on its own GPIO pair, powered via LDO2. Used by the dual-bus
  // I2C system (see System_I2C_Manager.h) when gSettings.i2c2BusEnabled
  // is set. LDO2 enable pin is GPIO39 (same as NEOPIXEL_POWER_PIN above —
  // driving GPIO39 HIGH powers both the RGB LED and I2C2).
  //
  // PIN MAPPING IS COUNTER-INTUITIVE: SDA = GPIO16, SCL = GPIO15. The
  // pinout image lists IO15/IO16 next to the I2C2 connector but doesn't
  // label which is which; empirical testing on the FeatherS3[D] shows the
  // connector wires SDA → IO16 and SCL → IO15 (opposite of how I2C1 maps
  // numerically). Verified by detecting an Adafruit OLED at 0x3D on the
  // vertical STEMMA QT only after swapping from the obvious 15/16 default.
  #define I2C2_SDA_PIN_DEFAULT  16
  #define I2C2_SCL_PIN_DEFAULT  15

  // I2C2 power gating — the vertical STEMMA QT (I2C2) rides on LDO2, which
  // is enabled by GPIO39. Physically the SAME pin as NEOPIXEL_POWER_PIN
  // above; the duplicate name documents the I2C2 role separately so that
  // (a) bus-1 init can drive the pin itself instead of relying on the
  // NeoPixel driver having run, and (b) future deep-sleep code can find
  // the right pin to drop via I2C2_POWER_PIN regardless of NeoPixel state.
  // Boards without a software-switchable I2C2 rail leave this at -1.
  #define I2C2_POWER_PIN        39

// --- Generic ESP32 (fallback with warning) ---
#elif defined(ARDUINO_ESP32_DEV)
  #define BOARD_SUPPORTED       1
  #define BOARD_NAME            "Generic ESP32"
  #warning "Using generic ESP32 pin configuration. Verify I2C pins match your hardware."
  
  // I2C Bus Configuration (common defaults)
  #define I2C_SDA_PIN_DEFAULT   21
  #define I2C_SCL_PIN_DEFAULT   22
  
  // NeoPixel Configuration
  #define NEOPIXEL_PIN_DEFAULT  -1
  #define NEOPIXEL_POWER_PIN    -1
  #define NEOPIXEL_COUNT_DEFAULT 0
  
  // Battery Monitoring (generic ESP32 — no known battery monitoring)
  #define BATTERY_ADC_PIN       -1
  #define BATTERY_MONITOR_AVAILABLE 0
  #define BATTERY_BACKEND_ADC        0
  #define BATTERY_BACKEND_FUEL_GAUGE 0

// --- Unsupported Board ---
#else
  #define BOARD_SUPPORTED       0
  #define BOARD_NAME            "Unknown/Unsupported"
  #warning "Board not explicitly supported. Using safe defaults. Check pin configuration!"
  
  // Safe fallback defaults (may not work - user should verify)
  #define I2C_SDA_PIN_DEFAULT   21
  #define I2C_SCL_PIN_DEFAULT   22
  #define NEOPIXEL_PIN_DEFAULT  -1
  #define NEOPIXEL_POWER_PIN    -1
  #define NEOPIXEL_COUNT_DEFAULT 0
  #define BATTERY_ADC_PIN       -1
  #define BATTERY_MONITOR_AVAILABLE 0
  #define BATTERY_BACKEND_ADC        0
  #define BATTERY_BACKEND_FUEL_GAUGE 0

#endif

// =============================================================================
// BATTERY MONITOR — derived enable + backend sanity
// =============================================================================
// ENABLE_BATTERY_MONITOR defaults to the active board's BATTERY_MONITOR_AVAILABLE
// flag so adding a new supported board "just works" without touching the user
// config above. A user-set `#define ENABLE_BATTERY_MONITOR <0|1>` at the top of
// this file takes precedence (force-disable on a slim build, or force-enable
// during board bring-up).
#ifndef ENABLE_BATTERY_MONITOR
  #define ENABLE_BATTERY_MONITOR BATTERY_MONITOR_AVAILABLE
#endif

// The battery web page is a consequent of battery-monitor hardware: force it
// off on USB-only / no-battery boards even if the web feature level enabled it.
// (Defined here, after ENABLE_BATTERY_MONITOR, since that's its prerequisite.)
#if !ENABLE_BATTERY_MONITOR
  #ifdef ENABLE_WEB_BATTERY
    #undef ENABLE_WEB_BATTERY
  #endif
  #define ENABLE_WEB_BATTERY 0
#endif

// Sanity: at most one backend selected. If the board didn't define either
// flag (out-of-tree board file), fall back to 0/0 so System_Battery seeds the
// USB-only stub instead of failing to compile.
#ifndef BATTERY_BACKEND_ADC
  #define BATTERY_BACKEND_ADC 0
#endif
#ifndef BATTERY_BACKEND_FUEL_GAUGE
  #define BATTERY_BACKEND_FUEL_GAUGE 0
#endif

// VBUS sense fallback. Boards without a routed VBUS divider leave this at -1
// and System_Battery falls back to the CRATE/voltage heuristic. Boards with
// the pin wired (FeatherS3[D] → GPIO 34) override above and get instant,
// deterministic USB-present detection.
#ifndef BATTERY_VBUS_SENSE_PIN
  #define BATTERY_VBUS_SENSE_PIN -1
#endif
#if BATTERY_BACKEND_ADC && BATTERY_BACKEND_FUEL_GAUGE
  #error "Pick one battery backend per board — BATTERY_BACKEND_ADC and BATTERY_BACKEND_FUEL_GAUGE are mutually exclusive."
#endif
#if BATTERY_BACKEND_FUEL_GAUGE && !ENABLE_I2C_SYSTEM
  #error "BATTERY_BACKEND_FUEL_GAUGE requires the I2C subsystem (I2C_FEATURE_LEVEL > 0). Lower the level or pick a different backend."
#endif
// If the user force-enabled the monitor on a board with no backend hardware
// claimed, the runtime will seed USB-only and report it — no compile error.

// =============================================================================
// I2C2 (second I2C bus) — board-agnostic fallback
// =============================================================================
// Boards that have a second I2C bus (e.g., FeatherS3[D]'s vertical STEMMA QT)
// define I2C2_SDA_PIN_DEFAULT / I2C2_SCL_PIN_DEFAULT in their block above.
// For everyone else, default to -1 so the dual-bus system treats it as
// "unavailable" and skips bus 1 initialization.
#ifndef I2C2_SDA_PIN_DEFAULT
  #define I2C2_SDA_PIN_DEFAULT  -1
#endif
#ifndef I2C2_SCL_PIN_DEFAULT
  #define I2C2_SCL_PIN_DEFAULT  -1
#endif

// Default state for i2c2BusEnabled. Derived from pin availability so the
// secondary bus auto-enables on boards that physically have it (FeatherS3[D]
// and equivalents) while staying off on boards without a second I2C port —
// no risk of a stray init on -1 pins, no misleading "enabled" toggle in the
// settings UI on incompatible boards. A board block above CAN override by
// `#define`ing I2C2_BUS_ENABLED_DEFAULT to 0 to opt out (e.g., for a power-
// sensitive build that doesn't want LDO2 driven at boot).
#ifndef I2C2_BUS_ENABLED_DEFAULT
  #if (I2C2_SDA_PIN_DEFAULT >= 0) && (I2C2_SCL_PIN_DEFAULT >= 0)
    #define I2C2_BUS_ENABLED_DEFAULT 1
  #else
    #define I2C2_BUS_ENABLED_DEFAULT 0
  #endif
#endif

// Fallback for I2C2_POWER_PIN — boards without a software-switchable I2C2
// rail leave this at -1. When valid (>= 0), bus-1 init drives the pin HIGH
// independently of NeoPixel state, and future sleep code can drop it to cut
// power to anything connected to the vertical STEMMA QT.
#ifndef I2C2_POWER_PIN
  #define I2C2_POWER_PIN -1
#endif

// Default bus for the OLED. On boards where I2C2 has its own software-
// controllable power rail (I2C2_POWER_PIN >= 0), the OLED is the prime
// candidate for that bus — putting it there means the display can be
// truly powered off (not just SSD1306-DISPLAYOFF, which leaves the chip
// drawing tens of µA and the panel ready to glow on the next command).
// Everywhere else, default to bus 0 so existing single-bus boards behave
// unchanged. A board block CAN override by `#define`ing OLED_BUS_DEFAULT.
#ifndef OLED_BUS_DEFAULT
  #if (I2C2_POWER_PIN >= 0) && (I2C2_SDA_PIN_DEFAULT >= 0) && (I2C2_SCL_PIN_DEFAULT >= 0)
    #define OLED_BUS_DEFAULT 1
  #else
    #define OLED_BUS_DEFAULT 0
  #endif
#endif

// =============================================================================
// BOARD VALIDATION (compile-time check)
// =============================================================================
// Uncomment to enforce strict board checking (build will fail on unsupported boards)
// #define REQUIRE_SUPPORTED_BOARD 1

#if defined(REQUIRE_SUPPORTED_BOARD) && !BOARD_SUPPORTED
  #error "Unsupported board detected! Please add your board configuration to System_BuildConfig.h or disable REQUIRE_SUPPORTED_BOARD."
#endif

#endif // SYSTEM_BUILDCONFIG_H