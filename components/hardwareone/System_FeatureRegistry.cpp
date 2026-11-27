/**
 * System Feature Registry Implementation
 * 
 * Centralized registry of all system features with heap cost estimates
 */

#include "System_FeatureRegistry.h"
#include <esp_attr.h>
#include <stdarg.h>
#include <string.h>
#include "System_Settings.h"
#include "System_Command.h"
#include "System_Debug.h"        // emitListingTrailer() for agent-facing listing trailer
#include "System_MemUtil.h"
#include "System_Utils.h"
#include "System_SetupWizard.h"
#include "System_AuthIdentity.h"  // currentAuthContext() for cmd_featuresetup transport check
#include "System_User.h"          // CommandSource enum
#include "System_SetupWizardMode.h"  // Phase 5: new CLIMode-based wizard for cmd_featuresetup
#include "System_CommandRegistry.h"
// Live state for the "run" column of `features` (featureRunningNow below).
// Each header has a branch for a build without its feature.
#include "System_RamFlush.h"      // ramFlushReadLive()
#include "BLE_Server.h"           // bleSubsystemActive()
#include "System_I2C.h"           // gI2cBusRunning
#include "i2csensor_encoder.h"     // gEncoderInst: the encoder row's switch
#include "System_EdgeImpulse.h"   // isContinuousInferenceRunning()

// External settings

// Compile-time feature checks
static bool isWifiCompiled() {
#if ENABLE_WIFI
  return true;
#else
  return false;
#endif
}

static bool isBluetoothCompiled() {
#if ENABLE_BLUETOOTH
  return true;
#else
  return false;
#endif
}

static bool isHttpCompiled() {
#if ENABLE_HTTP_SERVER
  return true;
#else
  return false;
#endif
}

static bool isEspNowCompiled() {
#if ENABLE_ESPNOW
  return true;
#else
  return false;
#endif
}

static bool isMqttCompiled() {
#if ENABLE_MQTT
  return true;
#else
  return false;
#endif
}

static bool isOledCompiled() {
#if ENABLE_OLED_DISPLAY
  return true;
#else
  return false;
#endif
}

static bool isNeoPixelCompiled() {
  // The feature flag (defaults to pin+count presence; user-overridable) is the
  // single source of truth now — see ENABLE_NEOPIXEL in System_BuildConfig.h.
#if ENABLE_NEOPIXEL
  return true;
#else
  return false;
#endif
}

static bool isThermalCompiled() {
#if ENABLE_THERMAL_SENSOR
  return true;
#else
  return false;
#endif
}

static bool isI2cCompiled() {
#if ENABLE_I2C_SYSTEM
  return true;
#else
  return false;
#endif
}

static bool isTofCompiled() {
#if ENABLE_TOF_SENSOR
  return true;
#else
  return false;
#endif
}

static bool isImuCompiled() {
#if ENABLE_IMU_SENSOR
  return true;
#else
  return false;
#endif
}

static bool isGpsCompiled() {
#if ENABLE_GPS_SENSOR
  return true;
#else
  return false;
#endif
}

static bool isFmRadioCompiled() {
#if ENABLE_FM_RADIO
  return true;
#else
  return false;
#endif
}

static bool isCameraCompiled() {
#if ENABLE_CAMERA_SENSOR
  return true;
#else
  return false;
#endif
}

static bool isMicrophoneCompiled() {
#if ENABLE_MICROPHONE
  // The mic feature is present if EITHER onboard PDM silicon OR a G2-capable
  // build — runtime availability (PDM present / glasses connected) is reported
  // separately by the sensor "connected" callback.
  return true;
#else
  return false;
#endif
}

static bool isApdsCompiled() {
#if ENABLE_APDS_SENSOR
  return true;
#else
  return false;
#endif
}

static bool isInputCompiled() {
#if ENABLE_OLED_INPUT
  return true;
#else
  return false;
#endif
}

static bool isRtcCompiled() {
#if ENABLE_RTC_SENSOR
  return true;
#else
  return false;
#endif
}

static bool isPresenceCompiled() {
#if ENABLE_PRESENCE_SENSOR
  return true;
#else
  return false;
#endif
}

static bool isAutomationCompiled() {
#if ENABLE_AUTOMATION
  return true;
#else
  return false;
#endif
}

static bool isEspSrCompiled() {
#if ENABLE_ESP_SR
  return true;
#else
  return false;
#endif
}

static bool isEdgeImpulseCompiled() {
#if ENABLE_EDGE_IMPULSE
  return true;
#else
  return false;
#endif
}

// Does the LLM FEATURE exist (registry, chat layer, commands, surfaces)? This
// is deliberately the backend flag, not a source flag — a CM5-only build has a
// working assistant with no on-device engine. Anything that needs "is there a
// local inference engine" must test ENABLE_LLM_SOURCE_ONBOARD instead; reading
// this as "on-device" is what mislabelled the wizard's feature row.
static bool isLlmCompiled() {
#if ENABLE_LLM_BACKEND
  return true;
#else
  return false;
#endif
}

static bool isMapsCompiled() {
#if ENABLE_MAPS
  return true;
#else
  return false;
#endif
}

static bool isGamesCompiled() {
#if ENABLE_WEB_GAME_DARKROOM
  return true;
#else
  return false;
#endif
}

static bool isBatteryCompiled() {
#if ENABLE_BATTERY_MONITOR
  return true;
#else
  return false;
#endif
}

// NOTE: ENABLE_BONDED_MODE is force-set to 0 when ESP-NOW is compiled out
// (System_BuildConfig.h "Bonded mode rides on ESP-NOW"), so this predicate
// reports false on an ESP-NOW-less build without any extra check here.
static bool isBondedModeCompiled() {
#if ENABLE_BONDED_MODE
  return true;
#else
  return false;
#endif
}

static bool isG2Compiled() {
#if ENABLE_G2_GLASSES
  return true;
#else
  return false;
#endif
}

static bool isServoCompiled() {
#if ENABLE_SERVO
  return true;
#else
  return false;
#endif
}

static bool isMatrixCompiled() { return ENABLE_LED_MATRIX; }
#if ENABLE_ENCODER
static bool isEncoderCompiled() { return true; }
#endif

// NOTE: ENABLE_R1_HEALTH is force-set to 0 unless BOTH Bluetooth and G2 are
// compiled (the ring rides the G2 BLE transport — System_BuildConfig.h "R1
// Health needs BT + G2"), so this predicate already reflects those
// prerequisites without repeating them here.
static bool isR1HealthCompiled() {
#if ENABLE_R1_HEALTH
  return true;
#else
  return false;
#endif
}

// ============================================================================
// Feature Registry - All System Features
// ============================================================================
// Heap estimates are approximate and include:
// - Task stack (typically 4-8KB per task)
// - Driver/library buffers
// - Runtime data structures

// Columns: id, displayName, category, heapEstimateKB, flags, settingPtr, isCompiledFn, description
static constexpr FeatureEntry featureRegistry[] = {
  // === NETWORK FEATURES ===
  { "wifi", "WiFi", FEATURE_CAT_NETWORK, 24,
    FEATURE_FLAG_RUNTIME_TOGGLE | FEATURE_FLAG_REQUIRES_REBOOT,
    &gSettings.wifiAutoStart, isWifiCompiled,
    "WiFi connectivity and network stack" },
    
  { "http", "HTTP Server", FEATURE_CAT_NETWORK, 18,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.httpAutoStart, isHttpCompiled,
    "Web interface and REST API" },
    
  { "bluetooth", "Bluetooth", FEATURE_CAT_NETWORK, 12,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.bleAutoStart, isBluetoothCompiled,
    "Bluetooth in the selected Server, G2 client or phone client mode" },
    
  { "espnow", "ESP-NOW", FEATURE_CAT_NETWORK, 8,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.espnowAutoStart, isEspNowCompiled,
    "Device-to-device mesh communication" },

  // Bonded mode sits right after espnow because it rides it: BuildConfig
  // force-clears ENABLE_BONDED_MODE whenever ESP-NOW is off. That coupling is
  // exactly why this row has to exist — an ESP-NOW-less build silently loses
  // every bond* command (bondstatus / bondrole / bondconnect / bondstream*,
  // System_Utils.cpp + System_ESPNow.cpp, all under #if ENABLE_BONDED_MODE)
  // and the /api/bond/* routes, while the espnow row itself may still read
  // compiled:false and the bluetooth row compiled:true. A client cannot infer
  // "no bond" from any other row, and it must never be inferred from a MISSING
  // row either: a missing id means "assume present" (so that older firmware
  // doesn't hide half the UI), which is precisely why this row is
  // unconditional and NOT #if'd. #if-wrapping it would make it
  // indistinguishable from old firmware and it would gate nothing.
  //
  // Heap 2 KB is a bookkeeping floor, not a measured cost: bond spawns no task
  // of its own (it runs on the ESP-NOW task, already counted above) and makes
  // no internal-heap allocation — its peer/session/manifest tables are
  // EXT_RAM_BSS_ATTR or MALLOC_CAP_SPIRAM, i.e. PSRAM, not this column.
  { "bond", "Bonded Mode", FEATURE_CAT_NETWORK, 2,
    FEATURE_FLAG_COMPILE_TIME,
    nullptr, isBondedModeCompiled,
    "Paired-device bond channel (bondstatus, bondrole, bondstream)" },


  { "mqtt", "MQTT", FEATURE_CAT_NETWORK, 6,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.mqttAutoStart, isMqttCompiled,
    "Home Assistant integration via MQTT broker" },

  // === DISPLAY FEATURES ===
  { "oled", "Local Display", FEATURE_CAT_DISPLAY, 4,
    FEATURE_FLAG_RUNTIME_TOGGLE | FEATURE_FLAG_REQUIRES_REBOOT,
    &gSettings.oledAutoStart, isOledCompiled,
    "Local display interface (selected display backend)" },
    
  { "led", "Onboard LED", FEATURE_CAT_DISPLAY, 2,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.ledStartupEnabled, isNeoPixelCompiled,
    "RGB LED strip/ring effects" },

  // Filed under Display because the lens HUD is the user-facing surface, even
  // though the transport is BLE. BuildConfig force-clears ENABLE_G2_GLASSES
  // when Bluetooth is compiled out, so a `bluetooth` row reading compiled:true
  // says nothing about whether the ~60 g2* commands exist — this row is the
  // only way to tell. Unconditional for the same reason as every row here.
  // Heap 10 KB: the persistent g2_owner task (8 KB stack, G2_Glasses.cpp)
  // plus its TCB and BLE-central client state. The 5 KB g2_ble_connect worker
  // is transient (it exits once the link is up) so it is not counted.
  { "g2", "G2 Glasses", FEATURE_CAT_DISPLAY, 10,
    FEATURE_FLAG_COMPILE_TIME,
    nullptr, isG2Compiled,
    "Even Realities G2 smart glasses over BLE (g2status, g2show)" },

  // === SENSOR FEATURES ===
  // Unified input device entry — either driver (Seesaw gamepad or ANO encoder)
  // exposes itself here. Wizard / settings UI / feature list all use this one
  // entry; the active driver disambiguates at runtime.
  { "input",
#if ENABLE_GT911_TOUCH
    "Single Touchscreen",
#elif ENABLE_GPIO_ENCODER
    "GPIO Rotary Encoder",
#elif ENABLE_IMU_INPUT
    "IMU Input",  // short: the features json size check below counts every name
#elif ENABLE_ANO_ENCODER
    "ANO Encoder",
#else
    "Gamepad",
#endif
    FEATURE_CAT_SENSOR, 2,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.inputAutoStart, isInputCompiled,
#if ENABLE_GT911_TOUCH
    "GT911 single-finger touch controls"
#elif ENABLE_GPIO_ENCODER
    "GPIO quadrature wheel and click gestures for navigation"
#elif ENABLE_IMU_INPUT
    "Motion sensor as the input: taps are buttons, tilt is the joystick"
#elif ENABLE_ANO_ENCODER
    "ANO rotary encoder + 5-button D-pad for navigation"
#else
    "Seesaw gamepad for navigation"
#endif
  },
    
  { "thermal", "Thermal Camera", FEATURE_CAT_SENSOR, 32,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.thermalAutoStart, isThermalCompiled,
    "MLX90640 32x24 thermal imaging" },
    
  { "tof", "ToF Distance", FEATURE_CAT_SENSOR, 8,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.tofAutoStart, isTofCompiled,
    "VL53L4CX time-of-flight ranging" },
    
  { "imu", "IMU", FEATURE_CAT_SENSOR, 12,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.imuAutoStart, isImuCompiled,
    "Orientation and motion sensing" },
    
  { "gps", "GPS", FEATURE_CAT_SENSOR, 4,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.gpsAutoStart, isGpsCompiled,
    "PA1010D GPS location tracking" },
    
  { "fmradio", "FM Radio", FEATURE_CAT_SENSOR, 2,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.fmRadioAutoStart, isFmRadioCompiled,
    "RDA5807 FM receiver" },
    
  { "apds", "APDS Gesture", FEATURE_CAT_SENSOR, 4,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.apdsAutoStart, isApdsCompiled,
    "APDS9960 gesture/color/proximity" },

  { "rtc", "RTC Clock", FEATURE_CAT_SENSOR, 2,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.rtcAutoStart, isRtcCompiled,
    "DS3231 precision real-time clock" },

  { "presence", "Presence Sensor", FEATURE_CAT_SENSOR, 2,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.presenceAutoStart, isPresenceCompiled,
    "STHS34PF80 IR presence/motion detection" },

  // The one I2C device that had no row at all — thermal, tof, imu, gps,
  // fmradio, apds, rtc and presence all have one, so its absence was a plain
  // inconsistency. Gates servo / pwm / servoprofile / servolist /
  // servocalibrate (i2csensor_pca9685.cpp). Compile-time only: ENABLE_SERVO is
  // set per I2C feature level, and there is no servo enable setting. Heap ~0
  // (one small Adafruit_PWMServoDriver object, no task); 1 is the floor.
  { "servo", "Servo Controller", FEATURE_CAT_SENSOR, 1,
    FEATURE_FLAG_COMPILE_TIME,
    nullptr, isServoCompiled,
    "PCA9685 16-channel servo/PWM controller" },

  { "matrix", "LED Matrix", FEATURE_CAT_SENSOR, 1,
    FEATURE_FLAG_COMPILE_TIME, nullptr, isMatrixCompiled,
    "HT16K33 monochrome 16x8 LED matrix" },

#if ENABLE_ENCODER
  // Output only, no task: the main loop ticks its frames (heap ~0, 1 is the
  // floor). Listed only in a build that has it: this row spends the last of
  // the `features json` runway (the static_assert below; 2026-11-21: a build
  // with it and the ANO input keeps 1 byte, with the gamepad 5; with the P4
  // boards' touch or wheel input it would not fit, and the build catalogue
  // offers the device on the five boards with an I2C port only). The next row
  // needs the dedicated endpoint the assert names.
  { "encoder", "Encoder", FEATURE_CAT_SENSOR, 1,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gEncoderInst[0].autoStart, isEncoderCompiled,  // instance 1's switch (i2csensor_encoder.h)
    "Adafruit seesaw encoder board (5752 or 4991): its RGB pixels" },
#endif

  // Battery monitor has no enable/AutoStart setting of its own — the board's
  // BATTERY_MONITOR_AVAILABLE decides, so it's compile-time only (nullptr
  // setting => enabled==compiled, toggleable false). Listed unconditionally
  // like "llm" below so a USB-only board reports compiled:false instead of
  // going silent: on such a build batterystatus / batterycalibrate /
  // batterylog are not registered at all (System_CommandRegistry.cpp #if
  // ENABLE_BATTERY_MONITOR), so without this row the app's only way to find
  // out is to send batterystatus and get "Unknown command". Heap is ~0 (a 36 B
  // ADC-calibration struct, or nothing at all on a fuel-gauge board — it's
  // polled from the main loop, not a task); 1 is the rounded floor.
  { "battery", "Battery Monitor", FEATURE_CAT_SENSOR, 1,
    FEATURE_FLAG_COMPILE_TIME,
    nullptr, isBatteryCompiled,
    "Battery voltage/charge monitoring (batterystatus, batterylog)" },

  // R1 ring health. Added ahead of any client screen existing, precisely so the
  // client can gate on it the day one does — this row is the only advance
  // signal, because the health commands do NOT disappear when the feature is
  // compiled out (see below), they answer with an error string.
  //
  // Compile-time only. gSettings.healthLoggingEnabled is unconditional and
  // would compile here, but do NOT wire it up: it means "keep LOG_R1 on and
  // resume at boot", i.e. a CSV-logging toggle, not "is R1 Health available".
  // Pointing this row at it would make a client's on/off switch silently start
  // and stop logging. Same call as the `battery` row above vs batteryLogEnabled.
  //
  // Heap 1 is a rounded floor: G2_Health.cpp spawns no task and makes no
  // internal-heap allocation — its series, trend, history-day and tile-grid
  // state is all EXT_RAM_BSS_ATTR (PSRAM), and the history itself is on flash
  // (R1_HEALTH_HISTORY_MAX_FILES 96 x up to 64 KB). Note the r1_owner task
  // (6 KB) is NOT counted here: G2_Ring.cpp is gated on BLUETOOTH && G2, not on
  // ENABLE_R1_HEALTH, so that stack belongs to the `g2` row's budget.
  { "r1health", "R1 Ring Health", FEATURE_CAT_SENSOR, 1,
    FEATURE_FLAG_COMPILE_TIME,
    nullptr, isR1HealthCompiled,
    "R1 ring vitals: HR, HRV, SpO2, temp (ring health, healthlogging)" },

  { "camera", "Camera", FEATURE_CAT_SENSOR, 18,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.cameraAutoStart, isCameraCompiled,
    "ESP32-S3 camera sensor (XIAO ESP32S3 Sense)" },

  { "microphone", "Microphone", FEATURE_CAT_SENSOR, 4,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.micAutoStart, isMicrophoneCompiled,
    "Microphone: onboard PDM (XIAO Sense) or G2 glasses mic" },

  { "espsr", "Speech Recognition", FEATURE_CAT_SENSOR, 48,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.srAutoStart, isEspSrCompiled,
    "ESP-SR voice commands (requires microphone)" },

  { "edgeimpulse", "Edge Impulse ML", FEATURE_CAT_SENSOR, 32,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.eiAutoStart, isEdgeImpulseCompiled,
    "ML inference for object detection (requires camera)" },

  // On-device LLM is pure compute (no device/sensor), so it's filed under
  // System rather than Sensor like its mic/camera-driven ML cousins above.
  // Always listed (NOT #if'd) so `features json` reports compiled:false on
  // builds without it — discoverable like a disconnected sensor. The runtime
  // toggle (gSettings.llmAutoStart) controls auto-loading the default model at
  // boot, matching every other feature's AutoStart setting.
  //
  // Name/cost/description follow the compiled SOURCE, not the feature flag.
  // ENABLE_LLM_BACKEND only says the feature exists; it says nothing about
  // where answers come from. A CM5-only build has no on-device engine, no
  // model file and no PSRAM cost, so calling it "On-Device LLM ~24KB" offered
  // the wizard a checkbox for hardware that build does not contain. The
  // setting itself stays shared and is meaningful either way: onboard uses it
  // to auto-load a local model (HardwareOne.cpp), CM5 uses it for the deferred
  // remote autostart of a `cm5:` default (System_LLMBackend.cpp).
  //
  // This #if picks BETWEEN two rows, it does not wrap one: id "llm" is present
  // in every build, so the "never #if-wrap a registry row" rule still holds —
  // a consumer never sees the id go missing and assume-present.
#if ENABLE_LLM_SOURCE_ONBOARD
  { "llm", "On-Device LLM", FEATURE_CAT_SYSTEM, 24,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.llmAutoStart, isLlmCompiled,
    "Tiny transformer text generation (ESP32-S3 + PSRAM)" },
#else
  { "llm", "LLM Assistant", FEATURE_CAT_SYSTEM, 0,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.llmAutoStart, isLlmCompiled,
    "Text generation via the CM5 co-processor (no on-device engine)" },
#endif

  // === CORE HARDWARE FEATURES ===
  { "i2c", "I2C Bus", FEATURE_CAT_CORE, 4,
    FEATURE_FLAG_RUNTIME_TOGGLE | FEATURE_FLAG_REQUIRES_REBOOT,
    &gSettings.i2cEnabled, isI2cCompiled,
    "I2C hardware bus (required for OLED and sensors)" },
    
  // Unconditional (was #if ENABLE_AUTOMATION): an #if'd row vanishes on a
  // build without the feature, and a consumer treats a missing id as "assume
  // present" so that older firmware doesn't hide half the UI. That made the
  // wrapped row indistinguishable from old firmware, so it gated nothing on
  // exactly the builds it needed to. gSettings.automationAutoStart sits
  // outside any #if in System_Settings.h so this row compiles in any build.
  { "automation", "Automations", FEATURE_CAT_SYSTEM, 8,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.automationAutoStart, isAutomationCompiled,
    "Scheduled tasks and conditional logic" },

  // === USER-FACING APPS ===
  // Filed under System to match their siblings "automation" and "llm" — the
  // registry has no App category, and these gate composed features rather than
  // a display or a device. Both are compile-time only (no enable setting
  // exists for either), and both are listed unconditionally, NOT #if'd, so
  // `features json` reports compiled:false on a build without them. That is
  // the whole point of these two rows: with ENABLE_MAPS 0 the map commands
  // (mapzoom / maplayers / mapcachekb / maporganize), the /maps page and the
  // OLED Maps mode all disappear, and the app previously had no way to learn
  // that except by issuing a map command and getting "Unknown command".
  //
  // Maps heap ~12 KB: the lazily-created, never-torn-down mapRender task
  // (8 KB stack + ~0.7 KB TCB, OLED_Mode_Map.cpp) plus ~4.6 KB of newlib
  // stdio + littlefs cache for the map file held open in _currentMap.mapFile.
  // The tile cache is much larger (mapcachekb, default 1280 KB) but is PSRAM,
  // so it is deliberately NOT in this number — this column is internal heap.
  { "maps", "Maps", FEATURE_CAT_SYSTEM, 12,
    FEATURE_FLAG_COMPILE_TIME,
    nullptr, isMapsCompiled,
    "Offline map tiles, waypoints, and the OLED Maps mode" },

  // A Dark Room costs ~0 internal heap: its raw string literals are streamed
  // straight out of .rodata by static httpd handlers, with no task, timer or
  // device-side state. Saves live in browser localStorage; the cost is flash.
  { "games", "Games", FEATURE_CAT_SYSTEM, 0,
    FEATURE_FLAG_COMPILE_TIME,
    nullptr, isGamesCompiled,
    "A Dark Room browser game at /games" },
};

static const size_t kFeatureRegistryCount = sizeof(featureRegistry) / sizeof(featureRegistry[0]);

// Must match CMD_RESULT_MAX (System_CommandTypes.h). That header is deliberately
// NOT #included here — see the note at cmd_features()'s jbuf for why neither this
// nor CMD_RESULT_MAX may be raised.
static constexpr size_t kFeaturesJsonBuf = 4096;

// ============================================================================
// Feature switches: what each row runs, and which commands switch it
// ============================================================================
// `features` only lists (owner decision 2026-11-20). Switching stays with each
// feature's own commands: open<x>/close<x> (now) and <x>autostart (at boot).
// Features have no "allowed" switch (owner decision 2026-11-21); only the I2C
// bus keeps one (i2cbusenabled), because it is hardware the board may not
// wire. This table names them, so the listing can show the states and a
// `features <id> on|off` refusal can name the right command.
//
// A parallel table, not new FeatureEntry columns: FeatureEntry::enabledSetting is
// read and written by the setup wizard, detect apply, sensorautostart and the G2
// sensors page, and must keep pointing where it points today. Flash only
// (constexpr .rodata, about 32 B a row); no internal RAM.
// `live`: a RamFlushFeatureId (System_RamFlush.h), or one of the values below.
enum : uint8_t {
  FEATURE_LIVE_BLE  = 0xF0,  // bleSubsystemActive(): server OR G2/phone client
  FEATURE_LIVE_I2C  = 0xF1,  // gI2cBusRunning
  FEATURE_LIVE_EI   = 0xF2,  // continuous Edge Impulse inference
  FEATURE_LIVE_NONE = 0xFF,  // the row has no runtime state ("-")
};
static_assert((unsigned)RF_FEATURE_COUNT < (unsigned)FEATURE_LIVE_BLE, "RamFlushFeatureId ran into the FEATURE_LIVE_* values");

struct FeatureSwitches {
  const char* id;          // equals featureRegistry[i].id (checked below)
  const bool* allowed;     // bus allowed (i2c only); nullptr = the row has none
  const bool* autoStart;   // start-at-boot flag; nullptr = none
  uint8_t live;            // where "running now" comes from (see above)
  const char* nowCmd;      // starts/stops it now; nullptr = none
  const char* bootCmd;     // sets start at boot; nullptr = none
  const char* allowCmd;    // sets allowed; nullptr = none
  // allowCmd only saves the flag and acts at the next boot (i2cbusenabled
  // replies "reboot required").
  bool allowAtReboot = false;
};

#define FEATURE_SENSOR_SWITCHES(id, field, rf) \
  { id, nullptr, &gSettings.field##AutoStart, rf, \
    "open" id " / close" id, id "autostart <0|1>", nullptr }

// Same order as featureRegistry[]. Every command named here is checked against
// the command tables by test_features_list_only.py.
static constexpr FeatureSwitches kFeatureSwitches[] = {
  { "wifi", nullptr, &gSettings.wifiAutoStart, RF_WIFI,
    "openwifi / closewifi", "wifiautostart <0|1>", nullptr },
  { "http", nullptr, &gSettings.httpAutoStart, RF_HTTP,
    "openhttp / closehttp", "httpautostart <0|1>", nullptr },
  { "bluetooth", nullptr, &gSettings.bleAutoStart, FEATURE_LIVE_BLE,
    "openble / closeble", "bleautostart <0|1>", nullptr },
  { "espnow", nullptr, &gSettings.espnowAutoStart, RF_ESPNOW,
    "openespnow / closeespnow", "espnowautostart <0|1>", nullptr },
  { "bond", nullptr, nullptr, FEATURE_LIVE_NONE, nullptr, nullptr, nullptr },
  { "mqtt", nullptr, &gSettings.mqttAutoStart, RF_MQTT,
    "openmqtt / closemqtt", "mqttautostart <0|1>", nullptr },
  { "oled", nullptr, &gSettings.oledAutoStart, RF_OLED,
    "openoled / closeoled", "oledautostart <0|1>", nullptr },
  // The start-up effect only; the LED has no master switch and no running state.
  { "led", nullptr, &gSettings.ledStartupEnabled, FEATURE_LIVE_NONE,
    nullptr, "ledstartupenabled <0|1>", nullptr },
  { "g2", nullptr, nullptr, FEATURE_LIVE_NONE, nullptr, nullptr, nullptr },
  FEATURE_SENSOR_SWITCHES("input",    input,    RF_INPUT),
  FEATURE_SENSOR_SWITCHES("thermal",  thermal,  RF_THERMAL),
  FEATURE_SENSOR_SWITCHES("tof",      tof,      RF_TOF),
  FEATURE_SENSOR_SWITCHES("imu",      imu,      RF_IMU),
  FEATURE_SENSOR_SWITCHES("gps",      gps,      RF_GPS),
  FEATURE_SENSOR_SWITCHES("fmradio",  fmRadio,  RF_FMRADIO),
  FEATURE_SENSOR_SWITCHES("apds",     apds,     RF_APDS),
  FEATURE_SENSOR_SWITCHES("rtc",      rtc,      RF_RTC),
  FEATURE_SENSOR_SWITCHES("presence", presence, RF_PRESENCE),
  { "servo", nullptr, nullptr, FEATURE_LIVE_NONE, nullptr, nullptr, nullptr },
  { "matrix", nullptr, nullptr, FEATURE_LIVE_NONE, nullptr, nullptr, nullptr },
#if ENABLE_ENCODER
  // Instance 1's switch: the encoder's settings live in gEncoderInst.
  { "encoder", nullptr, &gEncoderInst[0].autoStart, RF_ENCODER,
    "openencoder / closeencoder", "encoderautostart <0|1>", nullptr },
#endif
  { "battery", nullptr, nullptr, FEATURE_LIVE_NONE, nullptr, nullptr, nullptr },
  { "r1health", nullptr, nullptr, FEATURE_LIVE_NONE, nullptr, nullptr, nullptr },
  FEATURE_SENSOR_SWITCHES("camera",   camera,   RF_CAMERA),
  { "microphone", nullptr, &gSettings.micAutoStart, RF_MICROPHONE,
    "openmic / closemic", "micautostart <0|1>", nullptr },
  { "espsr", nullptr, &gSettings.srAutoStart, RF_SR,
    "opensr / closesr", "srautostart <0|1>", nullptr },
  { "edgeimpulse", nullptr, &gSettings.eiAutoStart, FEATURE_LIVE_EI,
    "eicontinuous <0|1>", "eiautostart <0|1>", nullptr },
  { "llm", nullptr, &gSettings.llmAutoStart, RF_LLM,
    "llmload / llmunload", "llmautostart <0|1>", nullptr },
  // Bus 0 has no start-at-boot flag and no open/close: the bus comes up at boot
  // when it is allowed.
  { "i2c", &gSettings.i2cEnabled, nullptr, FEATURE_LIVE_I2C,
    nullptr, nullptr, "i2cbusenabled <0|1>", true },
  { "automation", nullptr, &gSettings.automationAutoStart, RF_AUTOMATION,
    "automation system enable|disable", "automationautostart <0|1>", nullptr },
  { "maps", nullptr, nullptr, FEATURE_LIVE_NONE, nullptr, nullptr, nullptr },
  { "games", nullptr, nullptr, FEATURE_LIVE_NONE, nullptr, nullptr, nullptr },
};
#undef FEATURE_SENSOR_SWITCHES

constexpr bool ceStrEq(const char* a, const char* b) {
  size_t i = 0;
  while (a[i] && a[i] == b[i]) ++i;
  return a[i] == b[i];
}
constexpr bool kFeatureSwitchesMatchRegistry() {
  if (sizeof(kFeatureSwitches) / sizeof(kFeatureSwitches[0]) != kFeatureRegistryCount) return false;
  for (size_t i = 0; i < kFeatureRegistryCount; i++) {
    if (!ceStrEq(kFeatureSwitches[i].id, featureRegistry[i].id)) return false;
    // A row with a saved switch (RUNTIME_TOGGLE) names at least one command;
    // a compile-time row names none.
    const bool named = kFeatureSwitches[i].nowCmd || kFeatureSwitches[i].bootCmd ||
                       kFeatureSwitches[i].allowCmd;
    if (named != ((featureRegistry[i].flags & FEATURE_FLAG_RUNTIME_TOGGLE) != 0)) return false;
    if (kFeatureSwitches[i].allowAtReboot && !kFeatureSwitches[i].allowCmd) return false;
  }
  return true;
}
static_assert(kFeatureSwitchesMatchRegistry(),
  "kFeatureSwitches must list every featureRegistry row, in the same order, and "
  "name a command for every runtime-switchable row (and an allowed command for "
  "every allowAtReboot row)");

static const FeatureSwitches* featureSwitchesFor(const FeatureEntry* f) {
  if (!f || f < featureRegistry || f >= featureRegistry + kFeatureRegistryCount) return nullptr;
  return &kFeatureSwitches[f - featureRegistry];
}

// 1 = running now, 0 = not running, -1 = the row has no runtime state.
static int featureRunningNow(const FeatureSwitches* s) {
  if (!s || s->live == FEATURE_LIVE_NONE) return -1;
  if (s->live < (unsigned)RF_FEATURE_COUNT) return ramFlushReadLive((RamFlushFeatureId)s->live) ? 1 : 0;
  switch (s->live) {
    case FEATURE_LIVE_BLE: return bleSubsystemActive() ? 1 : 0;
#if ENABLE_I2C_SYSTEM
    case FEATURE_LIVE_I2C: return gI2cBusRunning ? 1 : 0;
#endif
#if ENABLE_EDGE_IMPULSE
    case FEATURE_LIVE_EI:  return isContinuousInferenceRunning() ? 1 : 0;
#endif
    default:               return 0;
  }
}

// ============================================================================
// Registry Access Functions
// ============================================================================

void initFeatureRegistry() {
  // Nothing to init currently - registry is static
}

size_t getFeatureCount() {
  return kFeatureRegistryCount;
}

const FeatureEntry* getFeatureByIndex(size_t index) {
  if (index >= kFeatureRegistryCount) return nullptr;
  return &featureRegistry[index];
}

const FeatureEntry* getFeatureById(const char* id) {
  for (size_t i = 0; i < kFeatureRegistryCount; i++) {
    if (strcmp(featureRegistry[i].id, id) == 0) {
      return &featureRegistry[i];
    }
  }
  return nullptr;
}

// ============================================================================
// Feature Status Helpers
// ============================================================================

bool isFeatureCompiled(const FeatureEntry* feature) {
  if (!feature) return false;
  if (!feature->isCompileEnabled) return true;  // No check = always compiled
  return feature->isCompileEnabled();
}

bool isFeatureEnabled(const FeatureEntry* feature) {
  if (!feature) return false;
  if (!isFeatureCompiled(feature)) return false;
  if (!feature->enabledSetting) return true;  // No setting = always enabled if compiled
  return *feature->enabledSetting;
}

bool canToggleFeature(const FeatureEntry* feature) {
  if (!feature) return false;
  if (!isFeatureCompiled(feature)) return false;
  // RUNTIME_TOGGLE is the authority. It agrees with the enabledSetting check
  // below on every row (COMPILE_TIME <=> enabledSetting == nullptr), so this
  // cannot diverge — but reading the positive bit is what lets the JSON
  // `toggleable` field be the same expression instead of a second opinion.
  if (!(feature->flags & FEATURE_FLAG_RUNTIME_TOGGLE)) return false;
  if (!feature->enabledSetting) return false;
  return true;
}

// ============================================================================
// Heap Estimation Functions
// ============================================================================

uint32_t getEnabledFeaturesHeapEstimate() {
  uint32_t total = 0;
  for (size_t i = 0; i < kFeatureRegistryCount; i++) {
    if (isFeatureEnabled(&featureRegistry[i])) {
      total += featureRegistry[i].heapCostKb;
    }
  }
  return total;
}

uint32_t getTotalPossibleHeapCost() {
  uint32_t total = 0;
  for (size_t i = 0; i < kFeatureRegistryCount; i++) {
    if (isFeatureCompiled(&featureRegistry[i])) {
      total += featureRegistry[i].heapCostKb;
    }
  }
  return total;
}

uint32_t getCategoryHeapEstimate(FeatureCategory cat) {
  uint32_t total = 0;
  for (size_t i = 0; i < kFeatureRegistryCount; i++) {
    if (featureRegistry[i].category == cat && isFeatureEnabled(&featureRegistry[i])) {
      total += featureRegistry[i].heapCostKb;
    }
  }
  return total;
}

// ============================================================================
// CLI Command: features
// ============================================================================

static constexpr const char* getCategoryName(FeatureCategory cat) {
  switch (cat) {
    case FEATURE_CAT_CORE:    return "Core";
    case FEATURE_CAT_NETWORK: return "Network";
    case FEATURE_CAT_DISPLAY: return "Display";
    case FEATURE_CAT_SENSOR:  return "Sensors";
    case FEATURE_CAT_SYSTEM:  return "System";
    default: return "Unknown";
  }
}

// Heap cost AS REPORTED. A feature that is not compiled cannot consume heap, so
// it reports 0. `compiled` travels in the same object/record, so 0 is never
// ambiguous — and every functional reader in the firmware already gates on
// compiled before spending this number. Before this, summing the per-row column
// counted features that are not in the binary.
static inline uint16_t reportedHeapKb(const FeatureEntry* f) {
  return (f && isFeatureCompiled(f)) ? f->heapCostKb : 0;
}

// ---------------------------------------------------------------------------
// Compile-time overflow tripwire for `features json`.
//
// The list is ONE round trip by contract: a client treats a missing id as
// "assume present", so a dropped page or a truncated document is a silent false
// capability claim. That makes the buffer a hard ceiling rather than a tuning
// knob, and it means the failure must be caught at BUILD time, not in the field.
// Same shape as the DBG_FLAG_COUNT drift assert in System_DebugFlags.h.
// ---------------------------------------------------------------------------
constexpr size_t ceSlen(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
constexpr size_t ceDigits(uint32_t v)  { size_t n = 1; while (v >= 10) { v /= 10; ++n; } return n; }

constexpr size_t kFeaturesJsonWorstCase() {
  size_t n = ceSlen("{\"schema\":1,\"features\":[]}");
  for (size_t i = 0; i < kFeatureRegistryCount; i++) {
    if (i) n += 1;   // inter-object comma
    // Field skeleton. EDIT THIS STRING IF YOU ADD OR REMOVE A FIELD.
    n += ceSlen("{\"id\":\"\",\"name\":\"\",\"category\":\"\",\"heapKB\":,"
                "\"compiled\":,\"enabled\":,\"toggleable\":}");
    n += ceSlen(featureRegistry[i].id)
       + ceSlen(featureRegistry[i].name)
       + ceSlen(getCategoryName(featureRegistry[i].category));
    // Take the max of both shapes: `false` is a byte longer than `true`, but a
    // compiled row spends digits(heapKb) where a compiled-out row spends "0".
    // Today the not-compiled shape wins; a 5-digit heapCostKb would flip it.
    const bool tog = (featureRegistry[i].flags & FEATURE_FLAG_RUNTIME_TOGGLE) != 0;
    const size_t notCompiled = 5 + 5 + 5 + 1;                       // false,false,false,"0"
    const size_t compiled    = 4 + 5 + (tog ? 4 : 5)
                             + ceDigits(featureRegistry[i].heapCostKb);
    n += (notCompiled > compiled) ? notCompiled : compiled;
  }
  return n;
}

// The runtime guard in cmd_features() rejects len >= kFeaturesJsonBuf - 1, so the
// largest ACCEPTED document is kFeaturesJsonBuf - 2. The 256 B reserve is runway
// on purpose: the BUILD breaks while roughly two rows of DELIVERABLE headroom
// still exist, so the fix can land before the payload becomes undeliverable.
static_assert(kFeaturesJsonWorstCase() + 256 <= kFeaturesJsonBuf - 2,
  "features json worst case no longer leaves runway. Do NOT raise this buffer or "
  "CMD_RESULT_MAX, and do NOT page the list: a client reads a missing id as "
  "\"assume present\", so a dropped page is a silent false capability claim. Move "
  "the list behind a dedicated endpoint instead - see /api/devices -> "
  "buildDeviceRegistryJson for the shape.");

// ---------------------------------------------------------------------------
// `features` text views. They only read: the command writes no setting, posts
// no event and starts or stops nothing (owner decision 2026-11-20).
// ---------------------------------------------------------------------------
static bool featuresAppend(char* buf, size_t size, size_t& pos, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));
static bool featuresAppend(char* buf, size_t size, size_t& pos, const char* fmt, ...) {
  if (pos >= size) return false;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf + pos, size - pos, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= size - pos) {
    pos = size;
    return false;
  }
  pos += (size_t)n;
  return true;
}

static const char* featureOnOff(const bool* flag) {
  return flag ? (*flag ? "on" : "off") : "-";
}

// `features`: one row per feature. "-" = the row has no such switch or state.
static const char* featuresListing() {
  PSRAM_STATIC_BUF(buf, 3072);
  size_t pos = 0;
  uint32_t runningKb = 0;
  bool ok = featuresAppend(buf, buf_SIZE, pos,
    "[Features] built in, running now, start at boot, allowed (heap estimates)\n"
    "═══════════════════════════════════════════\n"
    " id           heap   built run  boot allow\n");

  int lastCat = -1;
  for (size_t i = 0; ok && i < kFeatureRegistryCount; i++) {
    const FeatureEntry* f = &featureRegistry[i];
    if (f->category != lastCat) {
      lastCat = f->category;
      ok = featuresAppend(buf, buf_SIZE, pos, "\n[%s]\n", getCategoryName(f->category));
    }
    const bool built = isFeatureCompiled(f);
    const FeatureSwitches* s = built ? featureSwitchesFor(f) : nullptr;
    const int running = s ? featureRunningNow(s) : -1;
    // A built-in row with no runtime state (bond, led, maps, ...) is counted as
    // present: there is nothing to stop.
    if (built && running != 0) runningKb += f->heapCostKb;
    ok = ok && featuresAppend(buf, buf_SIZE, pos, " %-12s ~%2uKB  %-5s %-4s %-4s %s\n",
      f->id, (unsigned)reportedHeapKb(f), built ? "yes" : "no",
      running < 0 ? "-" : (running ? "yes" : "no"),
      s ? featureOnOff(s->autoStart) : "-",
      s ? featureOnOff(s->allowed) : "-");
  }

  ok = ok && featuresAppend(buf, buf_SIZE, pos,
    "═══════════════════════════════════════════\n"
    "Running now: ~%luKB | Free: %luKB | All built in: ~%luKB\n"
    "'-' = no such switch or state. 'features <id>' names the commands that switch it.",
    (unsigned long)runningKb, (unsigned long)(hw1InternalFreeBytes() / 1024),
    (unsigned long)getTotalPossibleHeapCost());
  if (!ok) return "Error: feature list outgrew the response buffer";

  emitListingTrailer("compiled-in features: built in, running now, start at boot, allowed",
    "sensor/battery DATA: use the read command ('batterystatus', or 'open<sensor>' then '<sensor>read'); to switch a feature: 'features <id>' names its commands");
  return buf;
}

// One "Start at boot" / "Allowed" line: the state and the command that sets it.
static bool featureSwitchLine(char* buf, size_t size, size_t& pos, const char* label,
                              const bool* flag, const char* cmd, const char* note) {
  if (!flag) return featuresAppend(buf, size, pos, "%s: -\n", label);
  return featuresAppend(buf, size, pos, "%s: %s%s%s%s\n", label, featureOnOff(flag),
                        cmd ? " - " : "", cmd ? cmd : "", note);
}

// `features <id>`.
static const char* featureDetail(const FeatureEntry* f, char* buf, size_t size) {
  const bool built = isFeatureCompiled(f);
  // A compiled-out feature reports 0 KB (it cannot consume heap), but the
  // build-planning number is still useful — keep it as prose, not as the field.
  char heapNote[40];
  if (built) heapNote[0] = '\0';
  else snprintf(heapNote, sizeof(heapNote), " (~%uKB if built in)", (unsigned)f->heapCostKb);

  size_t pos = 0;
  bool ok = featuresAppend(buf, size, pos,
    "[%s] %s\nCategory: %s\nHeap cost: ~%uKB%s\nBuilt in: %s\n",
    f->id, f->name, getCategoryName(f->category),
    (unsigned)reportedHeapKb(f), heapNote, built ? "yes" : "no");

  const FeatureSwitches* s = built ? featureSwitchesFor(f) : nullptr;
  if (built && (!s || !canToggleFeature(f))) {
    ok = ok && featuresAppend(buf, size, pos, "Switch: none, fixed when the firmware is built\n");
  } else if (s) {
    const int running = featureRunningNow(s);
    const char* reboot = s->allowAtReboot ? " (takes effect after a reboot)" : "";
    ok = ok && featuresAppend(buf, size, pos, "Running now: %s\n",
                              running < 0 ? "-" : (running ? "yes" : "no"));
    ok = ok && featureSwitchLine(buf, size, pos, "Start at boot", s->autoStart, s->bootCmd, "");
    ok = ok && featureSwitchLine(buf, size, pos, "Allowed", s->allowed, s->allowCmd, reboot);
    ok = ok && featuresAppend(buf, size, pos, "Start/stop now: %s\n", s->nowCmd ? s->nowCmd : "-");
  }
  ok = ok && featuresAppend(buf, size, pos, "%s", f->description);
  if (!ok) return "Error: feature details outgrew the response buffer";
  return buf;
}

// `features <id> <anything>`: the two-argument form that used to write a setting.
// It writes nothing, posts no event and calls no owner; the reply names the
// commands that do switch the row.
static const char* featureSwitchRefusal(const CommandArgs& a, char* buf, size_t size) {
  if (a.count() > 2 || a.unterminatedQuote()) {
    return "Error: Usage: features [<id> | json]. The features command only lists.";
  }
  String id = a.arg(0);
  id.toLowerCase();
  const FeatureEntry* f = getFeatureById(id.c_str());
  if (!f) return "Error: Unknown feature. Run 'features' to see list.";
  if (!isFeatureCompiled(f)) {
    snprintf(buf, size, "Error: features only lists, and %s is not built into this firmware.", f->id);
    return buf;
  }
  const FeatureSwitches* s = featureSwitchesFor(f);
  if (!s || !canToggleFeature(f)) {
    snprintf(buf, size, "Error: features only lists, and %s has no switch: it is fixed when "
             "the firmware is built.", f->id);
    return buf;
  }

  size_t pos = 0;
  bool ok = featuresAppend(buf, size, pos, "Error: features only lists. Switch %s with:", f->id);
  const char* sep = " ";
  if (s->nowCmd) {
    ok = ok && featuresAppend(buf, size, pos, "%s%s (now)", sep, s->nowCmd);
    sep = ", ";
  }
  if (s->bootCmd) {
    ok = ok && featuresAppend(buf, size, pos, "%s%s (at boot)", sep, s->bootCmd);
    sep = ", ";
  }
  if (s->allowCmd) {
    ok = ok && featuresAppend(buf, size, pos, "%s%s (allowed%s)", sep, s->allowCmd,
      s->allowAtReboot ? ", after a reboot" : "");
  }
  ok = ok && featuresAppend(buf, size, pos, ".");
  if (!ok) return "Error: features only lists. 'features <id>' names the commands that switch it.";
  return buf;
}

const char* cmd_features(const String& argsInput) {
  // `features` only lists (owner decision 2026-11-20). A second argument is
  // refused before the validate return, so `automation add` rejects a saved
  // `features <id> on` step when it is saved rather than when it runs.
  const bool wantsJson = argWantsJson(argsInput);
  CommandArgs a(argsInput);
  PSRAM_STATIC_BUF(text, 512);
  if (!wantsJson && (a.count() >= 2 || a.unterminatedQuote())) {
    return featureSwitchRefusal(a, text, text_SIZE);
  }
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Structured path: full capability list (what this build supports), one
  // verbatim JSON blob via the return value. No broadcastOutput. Lets the app
  // gate UI on what the device actually has.
  // Schema: {"schema":1,"features":[{"id","name","category","heapKb","compiled",
  //          "enabled","toggleable"}, ...]}
  // Kept byte for byte when `features` became list-only: `enabled` is the
  // row's enabledSetting (the start-at-boot switch; the bus switch for i2c,
  // the start-up effect for led) and `toggleable` is canToggleFeature (the row
  // has a saved switch). The text views show the states apart; there is no
  // room here for more keys (see the static_assert above).
  //
  // No "hint" field here on purpose. It used to carry 161 B of agent-facing
  // prose ("...use the read command..."), which is dead weight in a machine
  // payload the client parses for `compiled`. The identical string still goes
  // out on the text path below via emitListingTrailer(), so a human or agent
  // running plain `features` is unaffected. Removing a field nothing reads does
  // NOT bump `schema`: a client pinned to schema==1 would reject the bump and
  // lose the whole capability list, which is a far worse outcome than a field
  // it was ignoring. Reserve the bump for changes that alter what survives.
  if (wantsJson) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray arr = doc["features"].to<JsonArray>();
    for (size_t i = 0; i < kFeatureRegistryCount; i++) {
      const FeatureEntry* f = &featureRegistry[i];
      JsonObject o = arr.add<JsonObject>();
      o["id"]         = f->id;
      o["name"]       = f->name;
      o["category"]   = getCategoryName(f->category);
      o["heapKB"]     = reportedHeapKb(f);
      o["compiled"]   = isFeatureCompiled(f);
      o["enabled"]    = isFeatureEnabled(f);
      // Same expression the text detail and refusal paths use, so they never drift.
      // NOT the raw RUNTIME_TOGGLE bit: that ignores whether the feature is even
      // compiled, so it used to answer "toggleable":true for features that
      // reply "Feature not compiled in this build."
      o["toggleable"] = canToggleFeature(f);
    }
    // Stays at 4096 to MATCH CMD_RESULT_MAX, and must not be raised past it.
    // 4 KB is not a local choice: executeCommand() copies every handler result
    // into a `char out[CMD_RESULT_MAX]` and rejects resultLen >= outSize
    // ("result too large for this transport"), so a bigger buffer here buys
    // zero deliverable bytes — it only moves the failure one layer out and
    // replaces the precise message below with a generic one. Worse for a
    // capability payload specifically: the caller would get a non-JSON error
    // string instead of a list, and a consumer that treats a missing id as
    // "assume present" then degrades to assuming EVERY feature is present.
    // (One path is tighter still: the help-exit `char out[2048]` in
    // HardwareOne.cpp, deliberately not raised.)
    //
    // At 29 rows the worst case is ~3.6 KB, leaving room for roughly 3 more.
    // Note the worst case is the build where NOTHING is compiled (`false`
    // serializes a byte longer than `true`), so the margin is thinnest on
    // exactly the minimal builds this endpoint exists to describe. When it runs
    // out, the documented remedy is a dedicated endpoint (see CMD_RESULT_MAX's
    // own comment, and /api/devices -> buildDeviceRegistryJson for the shape),
    // NOT a bigger buffer and NOT paging: a partially-fetched list is
    // indistinguishable from "those features exist", so a dropped page becomes
    // a silent false capability claim.
    PSRAM_STATIC_BUF(jbuf, kFeaturesJsonBuf);
    // serializeJson truncates in silence when it runs out of room, producing a
    // half-sentence that still looks like data — and ArduinoJson's
    // StaticStringWriter does not null-terminate when the output exactly fills
    // the buffer, so an overflow could also hand back an unterminated string.
    // Fail loudly instead; a truncated list reads as "feature present".
    size_t len = serializeJson(doc, jbuf, jbuf_SIZE);
    if (len == 0 || len >= jbuf_SIZE - 1) return "Error: feature list outgrew the response buffer";
    return jbuf;
  }

  if (a.count() == 0) return featuresListing();

  String id = a.arg(0);
  id.toLowerCase();
  const FeatureEntry* f = getFeatureById(id.c_str());
  if (!f) return "Error: Unknown feature. Run 'features' to see list.";
  return featureDetail(f, text, text_SIZE);
}

// ============================================================================
// Feature Setup Wizard Command
// ============================================================================

static const char* cmd_featuresetup(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;

  // Phase 5: the wizard now runs as a CliMode state machine. Each user
  // input is one short handler call that mutates persistent state and
  // returns; cmd_exec is free between keystrokes. The transport guard
  // and blocking-read timeout from the prior implementation are replaced by
  // exact session ownership plus a monotonic idle timeout:
  //   - The new wizard reads input via the CLI dispatcher (NOT directly
  //     from Serial), from the same stateful session that opened it: serial,
  //     the human web CLI, or OLED. Machine/stateless transports bypass it.
  //   - cmd_exec isn't parked in a blocking read, so other commands keep
  //     working while the wizard is open. A user who walks away mid-
  //     wizard can resume from that same session, or `cancel` to exit cleanly.
  //
  // (FTS-at-boot continues to use the legacy runAndApplyFeatureWizard()
  // because that path runs on the main task BEFORE cmd_exec exists,
  // and the OLED + joystick integration there is already correct.)
  if (!setupWizardMode_start()) {
    return "Error: another interactive mode is active. Type 'exit' or 'cancel' first.";
  }
  return "Wizard started. Use 'n' (next), 'b' (back), or numbers to navigate. "
         "Type 'cancel' at any time to abort.";
}

// ============================================================================
// Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
extern const CommandEntry featureCommands[] = {
  { "features", "List system features: built in, running now, start at boot, allowed.", true, cmd_features,
    "features              - List all features with heap estimates\n"
    "features <id>         - Show one feature and the commands that switch it\n"
    "features json         - JSON capability list" },
  { "featuresetup", "Run the interactive feature configuration wizard.", true, cmd_featuresetup,
    "featuresetup  - Launch the feature config wizard (any CLI transport; navigate with n/b/numbers, 'cancel' to abort)" }
};

extern const size_t featureCommandsCount = sizeof(featureCommands) / sizeof(featureCommands[0]);
