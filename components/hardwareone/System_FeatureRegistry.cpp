/**
 * System Feature Registry Implementation
 * 
 * Centralized registry of all system features with heap cost estimates
 */

#include "System_FeatureRegistry.h"
#include <esp_attr.h>
#include "System_Settings.h"
#include "System_Command.h"
#include "System_Debug.h"        // emitListingTrailer() for agent-facing listing trailer
#include "System_MemUtil.h"
#include "System_Utils.h"
#include "System_SetupWizard.h"
#include "System_AuthIdentity.h"  // currentAuthContext() for cmd_featuresetup transport check
#include "System_User.h"          // CommandSource enum
#include "System_SetupWizardMode.h"  // Phase 5: new CLIMode-based wizard for cmd_featuresetup
#include "System_Events.h"        // systemEventPost() — SYSEVT_FEATURE_TOGGLED
#include <esp_heap_caps.h>

// External settings

// Compile-time feature checks
static bool isWiFiCompiled() {
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

static bool isI2CCompiled() {
#if ENABLE_I2C_SYSTEM
  return true;
#else
  return false;
#endif
}

static bool isToFCompiled() {
#if ENABLE_TOF_SENSOR
  return true;
#else
  return false;
#endif
}

static bool isIMUCompiled() {
#if ENABLE_IMU_SENSOR
  return true;
#else
  return false;
#endif
}

static bool isGPSCompiled() {
#if ENABLE_GPS_SENSOR
  return true;
#else
  return false;
#endif
}

static bool isFMRadioCompiled() {
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

static bool isAPDSCompiled() {
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

static bool isRTCCompiled() {
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

static bool isESPSRCompiled() {
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
static bool isLLMCompiled() {
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
#if ENABLE_GAMES
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
static const FeatureEntry featureRegistry[] = {
  // === NETWORK FEATURES ===
  { "wifi", "WiFi", FEATURE_CAT_NETWORK, 24,
    FEATURE_FLAG_REQUIRES_REBOOT,
    &gSettings.wifiEnabled, isWiFiCompiled,
    "WiFi connectivity and network stack" },
    
  { "http", "HTTP Server", FEATURE_CAT_NETWORK, 18,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.httpAutoStart, isHttpCompiled,
    "Web interface and REST API" },
    
  { "bluetooth", "Bluetooth", FEATURE_CAT_NETWORK, 12,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.bleAutoStart, isBluetoothCompiled,
    "BLE server for remote control" },
    
  { "espnow", "ESP-NOW", FEATURE_CAT_NETWORK, 8,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.espnowEnabled, isEspNowCompiled,
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
    &gSettings.mqttEnabled, isMqttCompiled,
    "Home Assistant integration via MQTT broker" },

  // === DISPLAY FEATURES ===
  { "oled", "OLED Display", FEATURE_CAT_DISPLAY, 4,
    FEATURE_FLAG_REQUIRES_REBOOT,
    &gSettings.oledEnabled, isOledCompiled,
    "128x64 OLED display interface" },
    
  { "led", "NeoPixel LED", FEATURE_CAT_DISPLAY, 2,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.ledStartupEnabled, isNeoPixelCompiled,
    "RGB LED strip/ring effects" },

  // Filed under Display because the lens HUD is the user-facing surface, even
  // though the transport is BLE. BuildConfig force-clears ENABLE_G2_GLASSES
  // when Bluetooth is compiled out, so a `bluetooth` row reading compiled:true
  // says nothing about whether the ~60 g2* commands exist — this row is the
  // only way to tell. Unconditional for the same reason as every row here.
  // Heap 8 KB: the persistent g2_ctrl_owner task (6 KB stack, G2_Glasses.cpp)
  // plus its TCB and BLE-central client state. The 5 KB g2_ble_connect worker
  // is transient (it exits once the link is up) so it is not counted.
  { "g2", "G2 Glasses", FEATURE_CAT_DISPLAY, 8,
    FEATURE_FLAG_COMPILE_TIME,
    nullptr, isG2Compiled,
    "Even Realities G2 smart glasses over BLE (g2status, g2show)" },

  // === SENSOR FEATURES ===
  // Unified input device entry — either driver (Seesaw gamepad or ANO encoder)
  // exposes itself here. Wizard / settings UI / feature list all use this one
  // entry; the active driver disambiguates at runtime.
  { "input",
#if ENABLE_ANO_ENCODER
    "ANO Encoder",
#else
    "Gamepad",
#endif
    FEATURE_CAT_SENSOR, 2,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.inputAutoStart, isInputCompiled,
#if ENABLE_ANO_ENCODER
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
    &gSettings.tofAutoStart, isToFCompiled,
    "VL53L4CX time-of-flight ranging" },
    
  { "imu", "IMU", FEATURE_CAT_SENSOR, 12,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.imuAutoStart, isIMUCompiled,
    "BNO055 orientation/motion sensing" },
    
  { "gps", "GPS", FEATURE_CAT_SENSOR, 4,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.gpsAutoStart, isGPSCompiled,
    "PA1010D GPS location tracking" },
    
  { "fmradio", "FM Radio", FEATURE_CAT_SENSOR, 2,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.fmRadioAutoStart, isFMRadioCompiled,
    "RDA5807 FM receiver" },
    
  { "apds", "APDS Gesture", FEATURE_CAT_SENSOR, 4,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.apdsAutoStart, isAPDSCompiled,
    "APDS9960 gesture/color/proximity" },

  { "rtc", "RTC Clock", FEATURE_CAT_SENSOR, 2,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.rtcAutoStart, isRTCCompiled,
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

  // Battery monitor has no enable/AutoStart setting of its own — the board's
  // BATTERY_MONITOR_AVAILABLE decides, so it's compile-time only (nullptr
  // setting => enabled==compiled, toggleable false). Listed unconditionally
  // like "llm" below so a USB-only board reports compiled:false instead of
  // going silent: on such a build batterystatus / batterycalibrate /
  // batterylog are not registered at all (System_Utils.cpp #if
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
    "R1 ring vitals: HR, HRV, SpO2, temp (healthstatus, healthlogging)" },

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
    &gSettings.srAutoStart, isESPSRCompiled,
    "ESP-SR voice commands (requires microphone)" },

  { "edgeimpulse", "Edge Impulse ML", FEATURE_CAT_SENSOR, 32,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.eiEnabled, isEdgeImpulseCompiled,
    "ML inference for object detection (requires camera)" },

  // On-device LLM is pure compute (no device/sensor), so it's filed under
  // System rather than Sensor like its mic/camera-driven ML cousins above.
  // Always listed (NOT #if'd) so `features json` reports compiled:false on
  // builds without it — discoverable like a disconnected sensor. The runtime
  // toggle (gSettings.llmAutoStart) controls auto-loading the default model at
  // boot, matching every other feature's AutoStart/Enabled setting.
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
    &gSettings.llmAutoStart, isLLMCompiled,
    "Tiny transformer text generation (ESP32-S3 + PSRAM)" },
#else
  { "llm", "LLM Assistant", FEATURE_CAT_SYSTEM, 0,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.llmAutoStart, isLLMCompiled,
    "Text generation via the CM5 co-processor (no on-device engine)" },
#endif

  // === HARDWARE FEATURES (shown on first page) ===
  { "i2c", "I2C Bus", FEATURE_CAT_NETWORK, 4,
    FEATURE_FLAG_REQUIRES_REBOOT,
    &gSettings.i2cEnabled, isI2CCompiled,
    "I2C hardware bus (required for OLED and sensors)" },
    
  // Unconditional (was #if ENABLE_AUTOMATION): an #if'd row vanishes on a
  // build without the feature, and a consumer treats a missing id as "assume
  // present" so that older firmware doesn't hide half the UI. That made the
  // wrapped row indistinguishable from old firmware, so it gated nothing on
  // exactly the builds it needed to. gSettings.automationEnabled is hoisted
  // out of its own #if in System_Settings.h to let this row compile in any
  // build — same treatment as ledStartupEnabled and llmAutoStart.
  { "automation", "Automations", FEATURE_CAT_SYSTEM, 8,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.automationEnabled, isAutomationCompiled,
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

  // Games cost ~0 internal heap: both games are raw string literals streamed
  // straight out of .rodata by static httpd handlers, with no task, timer or
  // device-side state (the maze polls /api/sensors from browser JS; A Dark
  // Room saves to browser localStorage). The real cost is flash — ~0.7-1 MB,
  // which is why BuildConfig #errors if both games are enabled at once.
  { "games", "Games", FEATURE_CAT_SYSTEM, 0,
    FEATURE_FLAG_COMPILE_TIME,
    nullptr, isGamesCompiled,
    "Browser games at /games (tilt maze or A Dark Room)" },
};

static const size_t featureRegistryCount = sizeof(featureRegistry) / sizeof(featureRegistry[0]);

// ============================================================================
// Registry Access Functions
// ============================================================================

void initFeatureRegistry() {
  // Nothing to init currently - registry is static
}

size_t getFeatureCount() {
  return featureRegistryCount;
}

const FeatureEntry* getFeatureByIndex(size_t index) {
  if (index >= featureRegistryCount) return nullptr;
  return &featureRegistry[index];
}

const FeatureEntry* getFeatureById(const char* id) {
  for (size_t i = 0; i < featureRegistryCount; i++) {
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
  if (feature->flags & FEATURE_FLAG_COMPILE_TIME) return false;
  if (!feature->enabledSetting) return false;
  return true;
}

// ============================================================================
// Heap Estimation Functions
// ============================================================================

uint32_t getEnabledFeaturesHeapEstimate() {
  uint32_t total = 0;
  for (size_t i = 0; i < featureRegistryCount; i++) {
    if (isFeatureEnabled(&featureRegistry[i])) {
      total += featureRegistry[i].heapCostKB;
    }
  }
  return total;
}

uint32_t getTotalPossibleHeapCost() {
  uint32_t total = 0;
  for (size_t i = 0; i < featureRegistryCount; i++) {
    if (isFeatureCompiled(&featureRegistry[i])) {
      total += featureRegistry[i].heapCostKB;
    }
  }
  return total;
}

uint32_t getCategoryHeapEstimate(FeatureCategory cat) {
  uint32_t total = 0;
  for (size_t i = 0; i < featureRegistryCount; i++) {
    if (featureRegistry[i].category == cat && isFeatureEnabled(&featureRegistry[i])) {
      total += featureRegistry[i].heapCostKB;
    }
  }
  return total;
}

// ============================================================================
// CLI Command: features
// ============================================================================

static const char* getCategoryName(FeatureCategory cat) {
  switch (cat) {
    case FEATURE_CAT_CORE:    return "Core";
    case FEATURE_CAT_NETWORK: return "Network";
    case FEATURE_CAT_DISPLAY: return "Display";
    case FEATURE_CAT_SENSOR:  return "Sensors";
    case FEATURE_CAT_SYSTEM:  return "System";
    default: return "Unknown";
  }
}

const char* cmd_features(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Structured path: full capability list (what this build supports + runtime
  // state), one verbatim JSON blob via the return value. No broadcastOutput.
  // Lets the app gate UI on what the device actually has.
  // Schema: {"schema":1,"features":[{"id","name","category","heapKB","compiled",
  //          "enabled","toggleable"}, ...]}
  //
  // No "hint" field here on purpose. It used to carry 161 B of agent-facing
  // prose ("...use the read command..."), which is dead weight in a machine
  // payload the client parses for `compiled`. The identical string still goes
  // out on the text path below via emitListingTrailer(), so a human or agent
  // running plain `features` is unaffected. Removing a field nothing reads does
  // NOT bump `schema`: a client pinned to schema==1 would reject the bump and
  // lose the whole capability list, which is a far worse outcome than a field
  // it was ignoring. Reserve the bump for changes that alter what survives.
  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray arr = doc["features"].to<JsonArray>();
    for (size_t i = 0; i < featureRegistryCount; i++) {
      const FeatureEntry* f = &featureRegistry[i];
      JsonObject o = arr.add<JsonObject>();
      o["id"]         = f->id;
      o["name"]       = f->name;
      o["category"]   = getCategoryName(f->category);
      o["heapKB"]     = f->heapCostKB;
      o["compiled"]   = isFeatureCompiled(f);
      o["enabled"]    = isFeatureEnabled(f);
      o["toggleable"] = (bool)(f->flags & FEATURE_FLAG_RUNTIME_TOGGLE);
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
    PSRAM_STATIC_BUF(jbuf, 4096);
    // serializeJson truncates in silence when it runs out of room, producing a
    // half-sentence that still looks like data — and ArduinoJson's
    // StaticStringWriter does not null-terminate when the output exactly fills
    // the buffer, so an overflow could also hand back an unterminated string.
    // Fail loudly instead; a truncated list reads as "feature present".
    size_t len = serializeJson(doc, jbuf, jbuf_SIZE);
    if (len == 0 || len >= jbuf_SIZE - 1) return "Error: feature list outgrew the response buffer";
    return jbuf;
  }

  CommandArgs a(argsInput);

  // No args - show all features with heap estimates
  if (a.count() == 0) {
    PSRAM_STATIC_BUF(buf, 2048);
    uint32_t freeHeapKB = hw1InternalFreeBytes() / 1024;
    uint32_t enabledCost = getEnabledFeaturesHeapEstimate();
    
    int pos = snprintf(buf, buf_SIZE,
      "[Feature Manager] (heap estimates)\n"
      "═══════════════════════════════════════════\n");
    
    FeatureCategory lastCat = (FeatureCategory)-1;
    
    for (size_t i = 0; i < featureRegistryCount; i++) {
      const FeatureEntry* f = &featureRegistry[i];
      
      // Print category header
      if (f->category != lastCat) {
        lastCat = f->category;
        pos += snprintf(buf + pos, buf_SIZE - pos, "\n[%s]\n", getCategoryName(f->category));
      }
      
      bool compiled = isFeatureCompiled(f);
      bool enabled = isFeatureEnabled(f);
      
      const char* status;
      if (!compiled) {
        status = "[N/C]";
      } else if (enabled) {
        status = "[ON]";
      } else {
        status = "[OFF]";
      }

      pos += snprintf(buf + pos, buf_SIZE - pos,
        " %-12s ~%2dKB  %s\n",
        f->id, f->heapCostKB, status);
    }
    
    pos += snprintf(buf + pos, buf_SIZE - pos,
      "\n[ON] = active | [OFF] = disabled | [N/C] = not compiled\n"
      "═══════════════════════════════════════════\n"
      "Enabled: ~%luKB | Free: %luKB | Max: ~%luKB\n"
      "Usage: features <id> <on|off>",
      (unsigned long)enabledCost, (unsigned long)freeHeapKB,
      (unsigned long)getTotalPossibleHeapCost());

    emitListingTrailer("compiled-in features and their on/off state",
      "sensor/battery DATA: use the read command ('batterystatus', or 'open<sensor>' then '<sensor>read'); to enable/disable a feature: 'features <id> on|off'");
    return buf;
  }

  // Parse args: <id> [on|off]
  if (!a.has(1)) {
    // Single arg - show feature details
    const FeatureEntry* f = getFeatureById(a.arg(0).c_str());
    if (!f) {
      return "Error: Unknown feature. Run 'features' to see list.";
    }
    
    EXT_RAM_BSS_ATTR static char buf[512];
    bool compiled = isFeatureCompiled(f);
    bool enabled = isFeatureEnabled(f);
    
    snprintf(buf, sizeof(buf),
      "[%s] %s\n"
      "Category: %s\n"
      "Heap cost: ~%dKB\n"
      "Compiled: %s\n"
      "Enabled: %s\n"
      "Toggleable: %s\n"
      "%s",
      f->id, f->name,
      getCategoryName(f->category),
      f->heapCostKB,
      compiled ? "yes" : "no",
      enabled ? "yes" : "no",
      canToggleFeature(f) ? "yes" : "no",
      f->description);
    
    return buf;
  }
  
  // Two args - toggle feature
  String featureId = a.arg(0);
  String value = a.arg(1);
  featureId.toLowerCase();
  value.toLowerCase();
  
  const FeatureEntry* f = getFeatureById(featureId.c_str());
  if (!f) {
    return "Unknown feature. Run 'features' to see list.";
  }
  
  if (!canToggleFeature(f)) {
    if (!isFeatureCompiled(f)) {
      return "Error: Feature not compiled in this build.";
    }
    return "Error: Feature cannot be toggled (compile-time only).";
  }
  
  bool enable = (value == "on" || value == "true" || value == "1");
  bool disable = (value == "off" || value == "false" || value == "0");
  
  if (!enable && !disable) {
    return "Error: Value must be on/off, true/false, or 1/0";
  }
  
  bool wasEnabled = *f->enabledSetting;
  *f->enabledSetting = enable;

  writeSettingsJson();

  // Runtime enable/disable — post only on an actual state transition (a no-op
  // re-set to the same value must not post). subject=feature name, detail=on|off.
  if (enable != wasEnabled) {
    systemEventPost(SYSEVT_FEATURE_TOGGLED, f->name, enable ? "on" : "off");
  }

  EXT_RAM_BSS_ATTR static char result[128];
  
  if (enable && !wasEnabled) {
    const char* rebootNote = (f->flags & FEATURE_FLAG_REQUIRES_REBOOT) ? " (reboot required)" : "";
    snprintf(result, sizeof(result), 
      "[Feature] %s enabled (~%dKB)%s",
      f->name, f->heapCostKB, rebootNote);
  } else if (!enable && wasEnabled) {
    const char* rebootNote = (f->flags & FEATURE_FLAG_REQUIRES_REBOOT) ? " (reboot required)" : "";
    snprintf(result, sizeof(result), 
      "[Feature] %s disabled (+%dKB freed)%s",
      f->name, f->heapCostKB, rebootNote);
  } else {
    snprintf(result, sizeof(result), 
      "[Feature] %s already %s",
      f->name, enable ? "enabled" : "disabled");
  }
  
  return result;
}

// ============================================================================
// Feature Setup Wizard Command
// ============================================================================

static const char* cmd_featuresetup(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;

  // Phase 5: the wizard now runs as a CLIMode state machine. Each user
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
  { "features", "Show/toggle system features with heap estimates.", true, cmd_features,
    "features              - List all features\n"
    "features <id>         - Show feature details\n"
    "features <id> <on|off> - Enable/disable feature\n"
    "features json         - JSON capability list" },
  { "featuresetup", "Run the interactive feature configuration wizard.", true, cmd_featuresetup,
    "featuresetup  - Launch the feature config wizard (any CLI transport; navigate with n/b/numbers, 'cancel' to abort)" }
};

extern const size_t featureCommandsCount = sizeof(featureCommands) / sizeof(featureCommands[0]);
