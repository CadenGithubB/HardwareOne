#include "System_ESPSR.h"
#include "System_Events.h"  // systemEventPost — voice events
#include "System_BuildConfig.h"
#if ENABLE_OLED_DISPLAY
#include "OLED_UI.h"  // oledNotificationBannerShow — voice-failure banner
#endif
#include <esp_attr.h>

#if ENABLE_ESP_SR

#include "System_Notifications.h"
#include "System_Debug.h"
#include "System_TaskUtils.h"
#include "System_VFS.h"
#include "System_BuildConfig.h"
#include "System_MemUtil.h"
#include "System_Microphone.h"
#include "System_Mutex.h"
#include "G2_Glasses.h"  // g2MicSetAfeFeedActive / g2MicReadPcmSamples (Phase 2B)
#include "HAL_Audio.h"   // single PDM/I2S capture owner (audioCaptureStart/audioReadPcm)
#include "System_Command.h"
#include "System_CommandTypes.h"  // Command / ORIGIN_VOICE — voice cmds route through cmd_exec
#include "System_CLI.h"
#include "System_User.h"
#include "System_AuthIdentity.h"  // currentAuthContext (voice arm captures caller identity)
#include <ctype.h>
#include <math.h>
#include "esp_timer.h"
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// ESP-SR includes
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_speech_commands.h"
#include "esp_mn_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "driver/i2s_pdm.h"

// Debug tags - SR output gates on the DEBUG_SR bank, not DEBUG_MICROPHONE.
// DEBUG_SR is the master ("all SR"); each gate ORs it with the one sub-flag
// naming that output's family, mirroring DEBUG_G2_*F.
#define TAG_SR "ESP_SR"
#define DEBUG_SRF(fmt, ...)       DEBUGF_QUEUE_DEBUG(DEBUG_SR | DEBUG_SR_LIFECYCLE, "[%s] " fmt, TAG_SR, ##__VA_ARGS__)
#define DEBUG_SR_AUDIOF(fmt, ...) DEBUGF_QUEUE_DEBUG(DEBUG_SR | DEBUG_SR_AFE,       "[%s] " fmt, TAG_SR, ##__VA_ARGS__)
#define INFO_SRF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_SR, "[INFO][SR] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_SRF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_WARN) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_SR, "[WARN][SR] " fmt, ##__VA_ARGS__); } while (0)
#define ERROR_SRF(fmt, ...) DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_SR, "[ERROR][SR] " fmt, ##__VA_ARGS__)

// I2S PDM Configuration for microphone (XIAO ESP32S3 Sense uses PDM mic)
#define I2S_SR_NUM          I2S_NUM_0
#define I2S_SR_SAMPLE_RATE  16000
#define I2S_SR_BITS         16
#define I2S_SR_CHANNELS     1

// Use PDM microphone pins from BuildConfig
#ifndef MIC_CLK_PIN
  #define MIC_CLK_PIN       42  // Default for XIAO ESP32S3 Sense
#endif
#ifndef MIC_DATA_PIN
  #define MIC_DATA_PIN      41  // Default for XIAO ESP32S3 Sense
#endif

// Task configuration (SR_STACK_WORDS and SR_TASK_PRIORITY_LEVEL defined in System_TaskUtils.h)
#define SR_AUDIO_CHUNK_MS   32
#define SR_AUDIO_CHUNK_SIZE (I2S_SR_SAMPLE_RATE * I2S_SR_CHANNELS * sizeof(int16_t) * SR_AUDIO_CHUNK_MS / 1000)

// State
static bool gESPSRInitialized = false;
static bool gESPSRRunning = false;
static bool gESPSRWakeDetected = false;
static TaskHandle_t gSRTaskHandle = nullptr;
static volatile bool gSRTaskShouldRun = false;

// AFE and model handles
static esp_afe_sr_iface_t* gAFE = nullptr;

// Resolved model names (e.g. "wn9_hilexin", "mn7_en"). Populated when
// initAFE/initMultiNet succeed so the web Models card can show what's
// actually loaded rather than just a green check. Empty string when no
// model is loaded (deinit / startup state).
static char gWnModelName[64] = {0};
static char gMnModelName[64] = {0};

// Mic source is UNIFIED in HAL_Audio — there is no separate SR-local source of
// truth. The device-wide `micsource` command sets the preference (audioSetSource);
// the SR feed loop pulls through audioReadPcm(), which dispatches PDM vs the G2
// LC3→PCM ring internally, and initI2SMicrophone() leases capture via
// audioCaptureStart("sr") which arms whichever source resolves + (for G2) enables
// the glasses stream. Kept only for telemetry on the G2 path:
static uint64_t gSrG2BytesOk = 0;
static uint32_t gSrG2ReadZero = 0;
static esp_afe_sr_data_t* gAFEData = nullptr;
static model_iface_data_t* gMNData = nullptr;
static const esp_mn_iface_t* gMNModel = nullptr;
static SemaphoreHandle_t gMNCommandMutex = nullptr;
static bool gMNCommandsAllocated = false;

static bool gRestoreMicAfterSR = false;

// Statistics
static uint32_t gWakeWordCount = 0;
static uint32_t gCommandCount = 0;

static SemaphoreHandle_t gVoiceArmMutex = nullptr;
static bool gVoiceArmed = false;
static String gVoiceArmedUser = "";
static CommandSource gVoiceArmedByTransport = SOURCE_INTERNAL;
static String gVoiceArmedByIp = "";
static uint32_t gVoiceArmedAtMs = 0;

extern bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);

static void ensureVoiceArmMutex() {
  if (!gVoiceArmMutex) {
    gVoiceArmMutex = xSemaphoreCreateMutex();
  }
}

static const char* transportToStableString(CommandSource t) {
  switch (t) {
    case SOURCE_WEB: return "web";
    case SOURCE_SERIAL: return "serial";
    case SOURCE_LOCAL_DISPLAY: return "display";
    case SOURCE_BLUETOOTH: return "bluetooth";
    case SOURCE_MQTT: return "mqtt";
    case SOURCE_ESPNOW: return "espnow";
    case SOURCE_INTERNAL: return "internal";
    case SOURCE_VOICE: return "voice";
    case SOURCE_G2_GLASSES: return "g2";
    case SOURCE_UART: return "uart";
    default: return "unknown";
  }
}

static void voiceDisarmInternal() {
  bool wasArmed = gVoiceArmed;
  String prevUser = gVoiceArmedUser;
  gVoiceArmed = false;
  gVoiceArmedUser = "";
  gVoiceArmedByTransport = SOURCE_INTERNAL;
  gVoiceArmedByIp = "";
  gVoiceArmedAtMs = 0;
  if (wasArmed) systemEventPost(SYSEVT_VOICE_DISARMED, prevUser.c_str());
}

static bool voiceArmFromContextInternal(const AuthContext& ctx) {
  if (ctx.transport == SOURCE_INTERNAL) return false;
  if (ctx.user.length() == 0) return false;

  gVoiceArmed = true;
  gVoiceArmedUser = ctx.user;
  gVoiceArmedByTransport = ctx.transport;
  gVoiceArmedByIp = ctx.ip;
  gVoiceArmedAtMs = millis();
  systemEventPost(SYSEVT_VOICE_ARMED, gVoiceArmedUser.c_str());
  return true;
}

static bool isVoiceArmed(String& outUser) {
  ensureVoiceArmMutex();
  if (gVoiceArmMutex) {
    if (xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  }
  bool armed = gVoiceArmed;
  outUser = gVoiceArmedUser;
  if (gVoiceArmMutex) xSemaphoreGive(gVoiceArmMutex);
  return armed;
}

static bool executeVoiceCommandAsArmedUser(const char* cliCmd, char* out, size_t outSize) {
  String user;
  if (!isVoiceArmed(user) || user.length() == 0) {
    snprintf(out, outSize, "Voice not armed");
    return false;
  }

  // Route through cmd_exec_task (submitAndExecuteSync) instead of calling
  // executeCommand() directly on the SR task. This serializes voice commands with
  // every other command source (no concurrent-handler races on shared state /
  // static response buffers) and runs them on cmd_exec's 8 KB stack, not ours.
  extern bool submitAndExecuteSync(const Command& cmd, String& out);
  Command uc;
  uc.line = cliCmd;
  uc.ctx.origin = ORIGIN_VOICE;
  uc.ctx.auth.transport = SOURCE_VOICE;
  uc.ctx.auth.user = user;
  uc.ctx.auth.ip = "voice";
  uc.ctx.auth.path = "/voice";
  uc.ctx.auth.sid = "";
  uc.ctx.auth.opaque = nullptr;
  uc.ctx.id = (uint32_t)millis();
  uc.ctx.timestampMs = (uint32_t)millis();
  uc.ctx.outputMask = MSG_ROUTE_FILE;  // audit to file log; response is the return value
  uc.ctx.validateOnly = false;
  uc.ctx.captureOutput = false;
  uc.ctx.replyHandle = nullptr;
  uc.ctx.httpReq = nullptr;

  String result;
  bool ok = submitAndExecuteSync(uc, result);
  strncpy(out, result.c_str(), outSize - 1);
  out[outSize - 1] = '\0';
  return ok;
}
static uint32_t gLastWakeMs = 0;
static String gLastCommand = "";
static float gLastConfidence = 0.0f;  // Last command confidence (0.0-1.0)

// ============================================================================
// Hierarchical Voice Command State Machine (supports 2 or 3 levels)
// Flow: Wake -> Category -> [SubCategory] -> Target
// ============================================================================
enum class VoiceState {
  IDLE,              // Waiting for wake word
  AWAIT_CATEGORY,    // Wake detected, listening for category (e.g., "camera", "sensor")
  AWAIT_SUBCATEGORY, // Category detected, listening for sub-category (e.g., "thermal", "GPS")
  AWAIT_TARGET       // Category/SubCategory detected, listening for target (e.g., "open", "close")
};

static VoiceState gVoiceState = VoiceState::IDLE;
static String gCurrentCategory = "";    // The detected category (e.g., "sensor")
static String gCurrentSubCategory = ""; // The detected sub-category (e.g., "thermal")
static uint32_t gCategoryTimeoutMs = 0; // Timeout for next stage detection

static uint8_t gSrDebugLevel = 0;
static uint32_t gSrTelemetryPeriodMs = 0;
static uint32_t gSrLastTelemetryMs = 0;
static uint64_t gSrI2SBytesOk = 0;
static uint32_t gSrI2SReadOk = 0;
static uint32_t gSrI2SReadErr = 0;
static uint32_t gSrI2SReadZero = 0;
static uint32_t gSrAfeFeedOk = 0;
static uint32_t gSrAfeFetchOk = 0;
static uint32_t gSrMnDetectCalls = 0;
static uint32_t gSrMnDetected = 0;
static float gSrLastVolumeDb = 0.0f;
static int gSrLastVadState = -1;
static int gSrLastWakeWordIndex = 0;
static int gSrLastWakeNetModelIndex = 0;
static int gSrLastAfeRetValue = 0;
static int gSrLastAfeTriggerChannel = -1;

static int16_t gSrLastPcmMin = 0;
static int16_t gSrLastPcmMax = 0;
static float gSrLastPcmAbsAvg = 0.0f;

static int gSrAfeFeedChunk = 0;
static int gSrAfeFetchChunk = 0;
static float gSrEstSampleRateHz = 0.0f;
static uint64_t gSrLastTelemetryBytesOk = 0;

// Minimum confidence threshold for command detection (0.0 - 1.0)
static float gSrMinCategoryConfidence = 0.15f;
static float gSrMinCommandConfidence = 0.12f;

// Reserved ID range for global voice commands (route cat "*")
// These IDs are assigned dynamically starting from this value
static const int GLOBAL_VOICE_CMD_ID_START = 990;
static uint32_t gSrLowConfidenceRejects = 0;

static bool gSrGapAcceptEnabled = true;
static float gSrGapAcceptFloor = 0.12f;
static float gSrGapAcceptGap = 0.08f;
static bool gSrTargetRequireSpeech = false;  // Disabled: VAD often shows 0 even during speech
static uint32_t gSrGapAccepts = 0;

static bool gSrDynGainEnabled = true;
static float gSrDynGainMin = 0.70f;
static float gSrDynGainMax = 2.50f;
static float gSrDynGainTargetPeak = 12000.0f;
static float gSrDynGainAlpha = 0.06f;
static float gSrDynGainCurrent = 1.0f;
static uint32_t gSrDynGainApplied = 0;
static uint32_t gSrDynGainBypassed = 0;

// Software gain - uses shared function from microphone module
// The XIAO ESP32S3 Sense PDM mic outputs very low amplitude, needs ~16-24x boost
// Audio preprocessing (DC offset, high-pass, pre-emphasis, gain) is now in System_Microphone.cpp

// Raw output mode - shows ALL MultiNet hypotheses regardless of confidence
static bool gSrRawOutputEnabled = false;

// Audio filter toggle - when false, only DC offset + gain applied (no high-pass/pre-emphasis)
// This can help if the AFE's internal processing conflicts with our filters
static bool gSrFiltersEnabled = true;  // Default ON - apply high-pass and pre-emphasis

// Auto-tuning state
static bool gSrAutoTuneActive = false;
static uint8_t gSrAutoTuneStep = 0;
static uint32_t gSrAutoTuneStartMs = 0;
static uint32_t gSrAutoTuneStepStartMs = 0;
static const uint32_t kAutoTuneStepDurationMs = 8000;  // 8 seconds per config

// Tiers 1-2 carry session/lifecycle plumbing, tiers 3-4 the audio chain, so a
// tier routes to the gate for its family. gSrDebugLevel picks the verbosity,
// the mask picks the family; both derive from the same six flags.
#define SR_DBG_L(lvl, fmt, ...) \
  do { \
    if (gSrDebugLevel >= (lvl)) { \
      if ((lvl) >= 3) { DEBUG_SR_AUDIOF(fmt, ##__VA_ARGS__); } \
      else            { DEBUG_SRF(fmt, ##__VA_ARGS__); } \
    } \
  } while (0)
#define SR_INFO_L(lvl, fmt, ...) do { if (gSrDebugLevel >= (lvl)) { INFO_SRF(fmt, ##__VA_ARGS__); } } while (0)

// Map the new bool-flag debug system to the legacy integer level so existing
// SR_DBG_L/SR_INFO_L call sites keep working unchanged. The `cmd_sr_debug_level`
// CLI command can still set gSrDebugLevel directly as a manual override.
//   - parent debugSr            -> level 4 (everything)
//   - any AFE / tuning sub      -> level 3 (chunk-level audio chain)
//   - any wake / command sub    -> level 2 (recognizer events)
//   - lifecycle only            -> level 1 (init / start / stop)
//   - none                      -> level 0
void srSyncDebugLevel() {
  uint8_t lvl = 0;
  if (gSettings.debugSr) {
    lvl = 4;
  } else if (gSettings.debugSrAfe || gSettings.debugSrTuning) {
    lvl = 3;
  } else if (gSettings.debugSrWake || gSettings.debugSrCommand) {
    lvl = 2;
  } else if (gSettings.debugSrLifecycle) {
    lvl = 1;
  }
  gSrDebugLevel = lvl;
}

enum class SrSnipDest : uint8_t { Auto = 0, SD = 1, LittleFS = 2 };

static volatile bool gSrSnipEnabled = false;
static volatile bool gSrSnipManualStartRequested = false;
static volatile bool gSrSnipManualStopRequested = false;
static uint32_t gSrSnipPreMs = 800;
static uint32_t gSrSnipMaxMs = 6000;
static SrSnipDest gSrSnipDest = SrSnipDest::Auto;
static const char* kSrSnipFolderSd = "/sd/ESP-SR Models/snips";
static const char* kSrSnipFolderInternal = "/sr_snips";

static int16_t* gSrSnipRing = nullptr;
static size_t gSrSnipRingSamples = 0;
static size_t gSrSnipRingHead = 0;

static bool gSrSnipSessionActive = false;
static uint32_t gSrSnipSessionStartMs = 0;
static uint32_t gSrSnipSessionDeadlineMs = 0;
static int16_t* gSrSnipSessionBuf = nullptr;
static size_t gSrSnipSessionSamplesCap = 0;
static size_t gSrSnipSessionSamplesWritten = 0;
static uint32_t gSrSnipSessionId = 0;
static int gSrSnipSessionCmdId = -1;
static char gSrSnipSessionPhrase[64] = {0};
static char gSrSnipSessionReason[16] = {0};

typedef struct {
  int16_t* pcm;
  uint32_t samples;
  uint32_t sample_rate;
  uint16_t bits;
  uint16_t channels;
  uint32_t created_ms;
  uint32_t session_id;
  int32_t cmd_id;
  SrSnipDest dest;
  char phrase[64];
  char reason[16];
} SrSnipJob;

static QueueHandle_t gSrSnipQueue = nullptr;
static TaskHandle_t gSrSnipWriterTask = nullptr;

// Callback for wake word and command detection
static void (*gWakeWordCallback)(const char* wakeWord) = nullptr;
static void (*gCommandCallback)(int commandId, const char* commandPhrase) = nullptr;

static const char* kESPSRCommandFile = "/sd/ESPSR/commands.txt";

// Voice command to CLI mapping
#define MAX_VOICE_CLI_MAPPINGS 128
struct VoiceCliMapping {
  int commandId;
  const char* cliCommand;  // Points to CommandEntry::name (static storage)
};
static VoiceCliMapping gVoiceCliMappings[MAX_VOICE_CLI_MAPPINGS];
static size_t gVoiceCliMappingCount = 0;

static void clearVoiceCliMappings() {
  gVoiceCliMappingCount = 0;
}

static void addVoiceCliMapping(int cmdId, const char* cliCmd) {
  if (gVoiceCliMappingCount < MAX_VOICE_CLI_MAPPINGS) {
    gVoiceCliMappings[gVoiceCliMappingCount].commandId = cmdId;
    gVoiceCliMappings[gVoiceCliMappingCount].cliCommand = cliCmd;
    gVoiceCliMappingCount++;
  }
}

static const char* findCliCommandForId(int cmdId) {
  for (size_t i = 0; i < gVoiceCliMappingCount; i++) {
    if (gVoiceCliMappings[i].commandId == cmdId) {
      return gVoiceCliMappings[i].cliCommand;
    }
  }
  return nullptr;
}

// Forward declarations for MultiNet helpers (defined later in file)
static bool mnCommandsReady();
static bool lockMN(uint32_t timeoutMs);
static void unlockMN();
static esp_mn_error_t* mnUpdateLocked();
static String normalizePhrase(const char* phrase);

// ============================================================================
// Voice route table — the ONLY place voice phrases live.
// ============================================================================
// Each route joins a spoken phrase hierarchy (cat [-> sub] -> target) to a
// canonical CLI command name. The command registry is the source of truth for
// what exists in THIS build: routeAlive() drops any route whose command never
// registered (its feature is compiled out), so the grammar is correct in every
// flag combination with no #ifs here. cat "*" = global phrase, available at
// every menu stage (see addSpecialPhrases). Two routes may share one cli
// (phrase aliases, e.g. cancel/nevermind).
//
// This table replaced the voiceCategory/voiceSubCategory/voiceTarget columns
// that every CommandEntry used to carry (~12 B x ~940 rows of .rodata in
// SR-less builds). Adding a voice alias = one row here, nothing else.
struct VoiceRoute {
  const char* cli;     // canonical command name (join key into the registry)
  const char* cat;     // level-1 phrase; "*" = global
  const char* sub;     // level-2 phrase (nullptr for 2-level routes)
  const char* target;  // final phrase
};

static const VoiceRoute kVoiceRoutes[] = {
  // Global phrases (available at every stage)
  { "voicecancel",    "*",          nullptr,          "cancel" },
  { "voicecancel",    "*",          nullptr,          "nevermind" },
  { "voicehelp",      "*",          nullptr,          "help" },
  // System / battery
  { "status",         "system",     nullptr,          "status" },
  { "reboot",         "system",     nullptr,          "reboot" },
  { "ramflush",       "system",     nullptr,          "ramflush" },
  { "batterystatus",  "battery",    nullptr,          "status" },
  // WiFi
  { "wifistatus",     "wifi",       nullptr,          "status" },
  { "radiopower",     "wifi",       nullptr,          "radio" },
  { "wifiscan",       "wifi",       nullptr,          "scan" },
  // LED
  { "ledcolor",       "led",        nullptr,          "change color" },
  { "ledclear",       "led",        nullptr,          "turn off" },
  // Voice pipeline itself
  { "closesr",        "voice",      nullptr,          "close" },
  // Connection
  { "openble",        "connection", "bluetooth",      "open" },
  { "closeble",       "connection", "bluetooth",      "close" },
  // Sensors (3-level: "sensor" -> device -> action)
  { "openimu",        "sensor",     "motion sensor",  "open" },
  { "closeimu",       "sensor",     "motion sensor",  "close" },
  { "opentof",        "sensor",     "time of flight", "open" },
  { "closetof",       "sensor",     "time of flight", "close" },
  { "openinput",      "sensor",     "input",          "open" },
  { "closeinput",     "sensor",     "input",          "close" },
  { "openrtc",        "sensor",     "clock",          "open" },
  { "closertc",       "sensor",     "clock",          "close" },
  { "openapds",       "sensor",     "gesture",        "open" },
  { "closeapds",      "sensor",     "gesture",        "close" },
  { "openmic",        "sensor",     "microphone",     "open" },
  { "closemic",       "sensor",     "microphone",     "close" },
  { "opengps",        "sensor",     "GPS",            "open" },
  { "closegps",       "sensor",     "GPS",            "close" },
  { "openfmradio",    "sensor",     "radio",          "open" },
  { "closefmradio",   "sensor",     "radio",          "close" },
  { "openpresence",   "sensor",     "presence",       "open" },
  { "closepresence",  "sensor",     "presence",       "close" },
  { "openthermal",    "sensor",     "thermal camera", "open" },
  { "closethermal",   "sensor",     "thermal camera", "close" },
  { "opencamera",     "sensor",     "camera",         "open" },
  { "closecamera",    "sensor",     "camera",         "close" },
  { "cameracapture",  "sensor",     "camera",         "take picture" },
  { "camerarecord",   "sensor",     "camera",         "record" },
};
static constexpr size_t kVoiceRouteCount = sizeof(kVoiceRoutes) / sizeof(kVoiceRoutes[0]);

// Liveness filter: a route only exists if its command is registered in THIS
// build's registry (registration is already feature-gated at the module level).
static bool routeAlive(const VoiceRoute& r) {
  return findCommand(String(r.cli)) != nullptr;
}

// One-time report of routes whose command didn't resolve. Compiled-out
// features are expected on slim builds; a rename in a command table without a
// matching route edit is a bug this makes loud instead of silent.
static void srReportDeadVoiceRoutes() {
  static bool reported = false;
  if (reported) return;
  reported = true;
  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    if (!routeAlive(kVoiceRoutes[i])) {
      WARN_SYSTEMF("[VOICE] route '%s' (phrase '%s') has no registered command — renamed or compiled out",
                   kVoiceRoutes[i].cli,
                   kVoiceRoutes[i].target ? kVoiceRoutes[i].target : "");
    }
  }
}

// Add global voice routes (cat "*") to current MultiNet command set
// These are available at all stages (category, subcategory, target)
// Call after clearing commands, before adding stage-specific phrases
static void addSpecialPhrases() {
  int globalId = GLOBAL_VOICE_CMD_ID_START;  // Start from reserved ID range

  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (!r.cat || !r.target) continue;

    // Check for global marker
    if (strcmp(r.cat, "*") != 0) continue;
    if (!routeAlive(r)) continue;

    esp_err_t err = esp_mn_commands_add(globalId, r.target);
    if (err == ESP_OK) {
      addVoiceCliMapping(globalId, r.cli);  // Map to CLI command name, not voice phrase
      INFO_SRF("[HIER-DEBUG] Added global phrase: id=%d phrase='%s' -> cli='%s'",
               globalId, r.target, r.cli);
    } else {
      WARN_SYSTEMF("[HIER-DEBUG] Failed to add global phrase '%s': err=0x%x",
                   r.target, err);
    }
    globalId++;
  }
}

// Load targets for a specific category into MultiNet
// Returns true if any targets were loaded, false if category has no targets (single-stage)
static bool loadTargetsForCategory(const char* category) {
  INFO_SRF("[HIER-DEBUG] loadTargetsForCategory('%s') called", category);
  INFO_SRF("[HIER-DEBUG]   Total voice routes: %u", (unsigned)kVoiceRouteCount);
  
  if (!mnCommandsReady()) {
    WARN_SYSTEMF("[HIER-DEBUG] loadTargetsForCategory: MultiNet not ready!");
    return false;
  }
  if (!lockMN(5000)) {
    WARN_SYSTEMF("[HIER-DEBUG] loadTargetsForCategory: Failed to lock MultiNet after 5s!");
    return false;
  }
  
  INFO_SRF("[HIER-DEBUG] Clearing MultiNet commands...");
  esp_mn_commands_clear();
  clearVoiceCliMappings();
  
  // Always add special phrases (cancel, help) for abort/help capability
  addSpecialPhrases();
  
  String normCategory = normalizePhrase(category);
  int nextId = 1;
  int loaded = 0;
  int scanned = 0;
  int categoryMatches = 0;
  
  // Find all routes with this category and load their targets
  INFO_SRF("[HIER-DEBUG] Scanning routes for category '%s' (normalized='%s') targets...", category, normCategory.c_str());
  for (size_t i = 0; i < kVoiceRouteCount && nextId < MAX_VOICE_CLI_MAPPINGS; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    scanned++;

    if (!r.cat) continue;
    if (!routeAlive(r)) continue;

    if (normalizePhrase(r.cat) == normCategory) {
      categoryMatches++;
      INFO_SRF("[HIER-DEBUG]   Found category match: cmd='%s' target='%s'",
               r.cli, r.target ? r.target : "(null)");

      if (r.target && r.target[0] != '\0') {
        esp_err_t err = esp_mn_commands_add(nextId, r.target);
        if (err == ESP_OK) {
          addVoiceCliMapping(nextId, r.cli);  // Map target ID to CLI command
          INFO_SRF("[HIER-DEBUG]   OK Added to MultiNet: id=%d phrase='%s' -> cli='%s'",
                   nextId, r.target, r.cli);
          nextId++;
          loaded++;
        } else {
          WARN_SYSTEMF("[HIER-DEBUG]   FAIL Failed to add '%s': err=0x%x", r.target, err);
        }
      } else {
        INFO_SRF("[HIER-DEBUG]   (no target - single-stage command)");
      }
    }
  }
  
  INFO_SRF("[HIER-DEBUG] Scan complete: scanned=%d, categoryMatches=%d, loaded=%d", 
           scanned, categoryMatches, loaded);
  
  if (loaded > 0) {
    INFO_SRF("[HIER-DEBUG] Updating MultiNet with %d targets...", loaded);
    esp_mn_error_t* errList = mnUpdateLocked();
    if (errList && errList->num > 0) {
      WARN_SYSTEMF("[HIER-DEBUG] MultiNet update had %d errors", errList->num);
    } else {
      INFO_SRF("[HIER-DEBUG] MultiNet update successful");
    }
  }
  unlockMN();
  
  INFO_SRF("[HIER] ===== Loaded %d targets for category '%s' =====", loaded, category);
  return loaded > 0;
}

// Load categories (first-stage commands) into MultiNet
static void loadCategories() {
  INFO_SRF("[HIER-DEBUG] ========== loadCategories() BEGIN ==========");
  INFO_SRF("[HIER-DEBUG] Total voice routes: %u", (unsigned)kVoiceRouteCount);
  srReportDeadVoiceRoutes();

  if (!mnCommandsReady()) {
    WARN_SYSTEMF("[HIER-DEBUG] loadCategories: MultiNet not ready!");
    return;
  }
  if (!lockMN(5000)) {
    WARN_SYSTEMF("[HIER-DEBUG] loadCategories: Failed to lock MultiNet after 5s!");
    return;
  }
  
  INFO_SRF("[HIER-DEBUG] Clearing MultiNet commands...");
  esp_mn_commands_clear();
  clearVoiceCliMappings();
  
  // Always add special phrases (cancel, help) for abort/help capability
  addSpecialPhrases();
  
  int nextId = 1;
  int loaded = 0;
  int scanned = 0;
  int withVoice = 0;
  int duplicates = 0;
  
  // Add unique categories
  INFO_SRF("[HIER-DEBUG] Scanning routes for unique categories...");
  for (size_t i = 0; i < kVoiceRouteCount && nextId < MAX_VOICE_CLI_MAPPINGS; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    scanned++;

    if (!r.cat || r.cat[0] == '\0') continue;
    // Skip global routes (cat "*") - already handled by addSpecialPhrases()
    if (strcmp(r.cat, "*") == 0) continue;
    if (!routeAlive(r)) continue;

    withVoice++;
    INFO_SRF("[HIER-DEBUG]   [%u] cmd='%s' category='%s' target='%s'",
             (unsigned)i, r.cli, r.cat,
             r.target ? r.target : "(null)");

    // Check for duplicates
    bool exists = false;
    for (int j = 0; j < nextId - 1; j++) {
      esp_mn_phrase_t* existing = esp_mn_commands_get_from_index(j);
      if (existing && existing->string && strcmp(existing->string, r.cat) == 0) {
        exists = true;
        break;
      }
    }
    if (exists) {
      INFO_SRF("[HIER-DEBUG]     ^ Category '%s' already added (duplicate)", r.cat);
      duplicates++;
      continue;
    }

    esp_err_t err = esp_mn_commands_add(nextId, r.cat);
    if (err == ESP_OK) {
      addVoiceCliMapping(nextId, r.cat);  // Map to category name
      INFO_SRF("[HIER-DEBUG]     OK Added category to MultiNet: id=%d phrase='%s'", nextId, r.cat);
      nextId++;
      loaded++;
    } else {
      WARN_SYSTEMF("[HIER-DEBUG]     FAIL Failed to add category '%s': err=0x%x", r.cat, err);
    }
  }
  
  INFO_SRF("[HIER-DEBUG] Scan complete: scanned=%d, withVoice=%d, duplicates=%d, unique=%d", 
           scanned, withVoice, duplicates, loaded);
  
  if (loaded > 0) {
    INFO_SRF("[HIER-DEBUG] Updating MultiNet with %d categories...", loaded);
    esp_mn_error_t* errList = mnUpdateLocked();
    if (errList && errList->num > 0) {
      WARN_SYSTEMF("[HIER-DEBUG] MultiNet update had %d errors", errList->num);
    } else {
      INFO_SRF("[HIER-DEBUG] MultiNet update successful");
    }
  }
  unlockMN();
  
  INFO_SRF("[HIER] ===== Loaded %d unique categories =====", loaded);
  INFO_SRF("[HIER-DEBUG] ========== loadCategories() END ==========");
}

// Normalize a phrase: trim whitespace, convert to lowercase
static String normalizePhrase(const char* phrase) {
  if (!phrase) return "";
  String s(phrase);
  s.trim();
  s.toLowerCase();
  return s;
}

// Case-insensitive comparison helper
static bool phraseMatches(const char* registryPhrase, const char* recognizedPhrase) {
  if (!registryPhrase || !recognizedPhrase) return false;
  String normalized = normalizePhrase(recognizedPhrase);
  String registry = normalizePhrase(registryPhrase);
  return normalized == registry;
}

// Find CLI command for a category+target combination
static const char* findCommandForCategoryTarget(const char* category, const char* target) {
  String normCategory = normalizePhrase(category);
  String normTarget = normalizePhrase(target);
  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (r.cat && r.target && routeAlive(r) &&
        normalizePhrase(r.cat) == normCategory &&
        normalizePhrase(r.target) == normTarget) {
      return r.cli;
    }
  }
  return nullptr;
}

// Check if a category has sub-categories (3-level hierarchy)
static bool categoryHasSubCategories(const char* category) {
  String normCategory = normalizePhrase(category);
  INFO_SRF("[HIER-DEBUG] categoryHasSubCategories('%s')", category);
  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (r.cat && r.sub && routeAlive(r) &&
        normalizePhrase(r.cat) == normCategory &&
        r.sub[0] != '\0') {
      INFO_SRF("[HIER-DEBUG]   Found subcategory: '%s' -> cmd='%s'", r.sub, r.cli);
      return true;
    }
  }
  return false;
}

// Check if a category has direct targets (2-level hierarchy, no subcategory)
static bool categoryHasDirectTargets(const char* category) {
  String normCategory = normalizePhrase(category);
  INFO_SRF("[HIER-DEBUG] categoryHasDirectTargets('%s')", category);
  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (r.cat && r.target && routeAlive(r) &&
        normalizePhrase(r.cat) == normCategory &&
        r.target[0] != '\0' &&
        (!r.sub || r.sub[0] == '\0')) {
      INFO_SRF("[HIER-DEBUG]   Found direct target: '%s' -> cmd='%s'", r.target, r.cli);
      return true;
    }
  }
  return false;
}

// Check if a category has any targets (either direct or via subcategory)
static bool categoryHasTargets(const char* category) {
  String normCategory = normalizePhrase(category);
  INFO_SRF("[HIER-DEBUG] categoryHasTargets('%s') -> normalized='%s'", category, normCategory.c_str());
  int targetCount = 0;
  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (r.cat && r.target && routeAlive(r) &&
        normalizePhrase(r.cat) == normCategory && r.target[0] != '\0') {
      targetCount++;
      INFO_SRF("[HIER-DEBUG]   Found target: '%s' -> cmd='%s'", r.target, r.cli);
    }
  }
  INFO_SRF("[HIER-DEBUG] categoryHasTargets('%s') = %s (found %d targets)", 
           normCategory.c_str(), targetCount > 0 ? "true" : "false", targetCount);
  return targetCount > 0;
}

// Load sub-categories for a category into MultiNet (for 3-level hierarchy)
static bool loadSubCategoriesForCategory(const char* category) {
  INFO_SRF("[HIER-DEBUG] loadSubCategoriesForCategory('%s')", category);
  
  if (!mnCommandsReady()) {
    WARN_SYSTEMF("[HIER-DEBUG] loadSubCategoriesForCategory: MultiNet not ready!");
    return false;
  }
  if (!lockMN(5000)) {
    WARN_SYSTEMF("[HIER-DEBUG] loadSubCategoriesForCategory: Failed to lock MultiNet!");
    return false;
  }
  
  esp_mn_commands_clear();
  clearVoiceCliMappings();
  
  // Always add special phrases (cancel, help) for abort/help capability
  addSpecialPhrases();
  
  String normCategory = normalizePhrase(category);
  int nextId = 1;
  int loaded = 0;
  
  // Find all unique sub-categories for this category
  for (size_t i = 0; i < kVoiceRouteCount && nextId < MAX_VOICE_CLI_MAPPINGS; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (!r.cat || !r.sub) continue;
    if (normalizePhrase(r.cat) != normCategory) continue;
    if (r.sub[0] == '\0') continue;
    if (!routeAlive(r)) continue;

    // Check for duplicates
    bool exists = false;
    for (int j = 0; j < nextId - 1; j++) {
      esp_mn_phrase_t* existing = esp_mn_commands_get_from_index(j);
      if (existing && existing->string && strcmp(existing->string, r.sub) == 0) {
        exists = true;
        break;
      }
    }
    if (exists) continue;

    esp_err_t err = esp_mn_commands_add(nextId, r.sub);
    if (err == ESP_OK) {
      addVoiceCliMapping(nextId, r.sub);
      INFO_SRF("[HIER-DEBUG]   Added subcategory: id=%d phrase='%s'", nextId, r.sub);
      nextId++;
      loaded++;
    }
  }
  
  if (loaded > 0) {
    esp_mn_error_t* errList = mnUpdateLocked();
    if (errList && errList->num > 0) {
      WARN_SYSTEMF("[HIER-DEBUG] MultiNet update had %d errors", errList->num);
    }
  }
  unlockMN();
  
  INFO_SRF("[HIER] Loaded %d subcategories for '%s'", loaded, category);
  return loaded > 0;
}

// Load targets for a specific category+subcategory combination
static bool loadTargetsForCategorySubCategory(const char* category, const char* subCategory) {
  INFO_SRF("[HIER-DEBUG] loadTargetsForCategorySubCategory('%s', '%s')", category, subCategory);
  
  if (!mnCommandsReady()) {
    WARN_SYSTEMF("[HIER-DEBUG] loadTargetsForCategorySubCategory: MultiNet not ready!");
    return false;
  }
  if (!lockMN(5000)) {
    WARN_SYSTEMF("[HIER-DEBUG] loadTargetsForCategorySubCategory: Failed to lock MultiNet!");
    return false;
  }
  
  esp_mn_commands_clear();
  clearVoiceCliMappings();
  
  // Always add special phrases (cancel, help) for abort/help capability
  addSpecialPhrases();
  
  String normCategory = normalizePhrase(category);
  String normSubCategory = normalizePhrase(subCategory);
  int nextId = 1;
  int loaded = 0;
  
  for (size_t i = 0; i < kVoiceRouteCount && nextId < MAX_VOICE_CLI_MAPPINGS; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (!r.cat || !r.sub || !r.target) continue;
    if (normalizePhrase(r.cat) != normCategory) continue;
    if (normalizePhrase(r.sub) != normSubCategory) continue;
    if (r.target[0] == '\0') continue;
    if (!routeAlive(r)) continue;

    esp_err_t err = esp_mn_commands_add(nextId, r.target);
    if (err == ESP_OK) {
      addVoiceCliMapping(nextId, r.cli);
      INFO_SRF("[HIER-DEBUG]   Added target: id=%d phrase='%s' -> cli='%s'",
               nextId, r.target, r.cli);
      nextId++;
      loaded++;
    }
  }
  
  if (loaded > 0) {
    esp_mn_error_t* errList = mnUpdateLocked();
    if (errList && errList->num > 0) {
      WARN_SYSTEMF("[HIER-DEBUG] MultiNet update had %d errors", errList->num);
    }
  }
  unlockMN();
  
  INFO_SRF("[HIER] Loaded %d targets for '%s'->'%s'", loaded, category, subCategory);
  return loaded > 0;
}

// Find CLI command for a 3-level category+subcategory+target combination
static const char* findCommandForCategorySubCategoryTarget(const char* category, const char* subCategory, const char* target) {
  String normCategory = normalizePhrase(category);
  String normSubCategory = normalizePhrase(subCategory);
  String normTarget = normalizePhrase(target);
  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (r.cat && r.sub && r.target && routeAlive(r) &&
        normalizePhrase(r.cat) == normCategory &&
        normalizePhrase(r.sub) == normSubCategory &&
        normalizePhrase(r.target) == normTarget) {
      return r.cli;
    }
  }
  return nullptr;
}

// Find the CLI command for a single-stage category (no target required)
static const char* findCommandForSingleStageCategory(const char* category) {
  String normCategory = normalizePhrase(category);
  INFO_SRF("[HIER-DEBUG] findCommandForSingleStageCategory('%s') -> normalized='%s'", category, normCategory.c_str());
  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (r.cat && routeAlive(r) && normalizePhrase(r.cat) == normCategory) {
      // Return the first route with this category (assumes single-stage has one command)
      if (!r.target || r.target[0] == '\0') {
        INFO_SRF("[HIER-DEBUG]   Found single-stage: cmd='%s'", r.cli);
        return r.cli;
      }
    }
  }
  INFO_SRF("[HIER-DEBUG]   No single-stage command found for category '%s'", normCategory.c_str());
  return nullptr;
}

static const char* voiceStateToString(VoiceState state) {
  switch (state) {
    case VoiceState::IDLE: return "IDLE";
    case VoiceState::AWAIT_CATEGORY: return "AWAIT_CATEGORY";
    case VoiceState::AWAIT_SUBCATEGORY: return "AWAIT_SUBCATEGORY";
    case VoiceState::AWAIT_TARGET: return "AWAIT_TARGET";
    default: return "UNKNOWN";
  }
}

static void onVoiceCommandDetected(int commandId, const char* phrase) {
  INFO_SRF("[HIER-DEBUG] ########## onVoiceCommandDetected() ##########");
  INFO_SRF("[HIER-DEBUG] commandId=%d, phrase='%s'", commandId, phrase ? phrase : "(null)");
  INFO_SRF("[HIER-DEBUG] Current state: %s", voiceStateToString(gVoiceState));
  INFO_SRF("[HIER-DEBUG] Current category: '%s', subcategory: '%s'", 
           gCurrentCategory.c_str(), gCurrentSubCategory.c_str());
  
  const char* mappedValue = findCliCommandForId(commandId);
  INFO_SRF("[HIER-DEBUG] Mapped value from ID: '%s'", mappedValue ? mappedValue : "(null)");
  
  // Check for global voice commands by phrase (simpler than ID matching)
  // Note: MultiNet may return phrases like "CAN CANCEL" or "HELP" with extra words
  String normPhrase = phrase ? normalizePhrase(phrase) : "";
  
  // Handle "cancel" or "nevermind" - abort current sequence
  if ((normPhrase.indexOf("cancel") >= 0 || normPhrase.indexOf("nevermind") >= 0) && gVoiceState != VoiceState::IDLE) {
    INFO_SRF("[HIER] CANCEL DETECTED - Aborting from state: %s", voiceStateToString(gVoiceState));
    broadcastOutput("[Voice] Cancelled.");
    gVoiceState = VoiceState::IDLE;
    gCurrentCategory = "";
    gCurrentSubCategory = "";
    loadCategories();
    return;
  }
  
  // Handle "help" - show available options for current state (check if phrase contains "help")
  if (normPhrase.indexOf("help") >= 0) {
    INFO_SRF("[HIER] HELP REQUESTED - State: %s", voiceStateToString(gVoiceState));
    
    if (gVoiceState == VoiceState::AWAIT_CATEGORY) {
      broadcastOutput("[Voice Help] Say a category:");
      for (size_t i = 0; i < kVoiceRouteCount; i++) {
        const VoiceRoute& r = kVoiceRoutes[i];
        if (r.cat && r.cat[0] != '\0' && strcmp(r.cat, "*") != 0 && routeAlive(r)) {
          bool dup = false;
          for (size_t j = 0; j < i && !dup; j++) {
            if (kVoiceRoutes[j].cat &&
                normalizePhrase(kVoiceRoutes[j].cat) == normalizePhrase(r.cat)) dup = true;
          }
          if (!dup) broadcastOutput(String("  - ") + r.cat);
        }
      }
    } else if (gVoiceState == VoiceState::AWAIT_SUBCATEGORY) {
      broadcastOutput(String("[Voice Help] ") + gCurrentCategory + " - which one?");
      String normCat = normalizePhrase(gCurrentCategory.c_str());
      for (size_t i = 0; i < kVoiceRouteCount; i++) {
        const VoiceRoute& r = kVoiceRoutes[i];
        if (r.cat && r.sub && routeAlive(r) &&
            normalizePhrase(r.cat) == normCat && r.sub[0] != '\0') {
          bool dup = false;
          for (size_t j = 0; j < i && !dup; j++) {
            if (kVoiceRoutes[j].sub &&
                normalizePhrase(kVoiceRoutes[j].sub) == normalizePhrase(r.sub)) dup = true;
          }
          if (!dup) broadcastOutput(String("  - ") + r.sub);
        }
      }
    } else if (gVoiceState == VoiceState::AWAIT_TARGET) {
      String normCat = normalizePhrase(gCurrentCategory.c_str());
      if (gCurrentSubCategory.length() > 0) {
        broadcastOutput(String("[Voice Help] ") + gCurrentCategory + " " + gCurrentSubCategory + " - what action?");
        String normSub = normalizePhrase(gCurrentSubCategory.c_str());
        for (size_t i = 0; i < kVoiceRouteCount; i++) {
          const VoiceRoute& r = kVoiceRoutes[i];
          if (r.cat && r.sub && r.target && routeAlive(r) &&
              normalizePhrase(r.cat) == normCat &&
              normalizePhrase(r.sub) == normSub && r.target[0] != '\0') {
            broadcastOutput(String("  - ") + r.target);
          }
        }
      } else {
        broadcastOutput(String("[Voice Help] ") + gCurrentCategory + " - what action?");
        for (size_t i = 0; i < kVoiceRouteCount; i++) {
          const VoiceRoute& r = kVoiceRoutes[i];
          if (r.cat && r.target && routeAlive(r) &&
              normalizePhrase(r.cat) == normCat &&
              (!r.sub || r.sub[0] == '\0') && r.target[0] != '\0') {
            broadcastOutput(String("  - ") + r.target);
          }
        }
      }
    } else {
      broadcastOutput("[Voice Help] Say the wake word first.");
    }
    broadcastOutput("  - cancel, help");
    
    if (gVoiceState != VoiceState::IDLE) {
      gCategoryTimeoutMs = millis() + gSettings.srCommandTimeout;
    }
    return;
  }
  
  if (gVoiceState == VoiceState::AWAIT_CATEGORY) {
    // We detected a category
    const char* category = phrase ? phrase : mappedValue;
    if (!category) {
      WARN_SYSTEMF("[HIER-DEBUG] Category detected but no phrase available!");
      return;
    }
    
    INFO_SRF("[HIER] ============================================");
    INFO_SRF("[HIER] CATEGORY DETECTED: '%s'", category);
    INFO_SRF("[HIER] ============================================");
    
    // Check hierarchy: subcategories first (3-level), then direct targets (2-level), then single-stage
    if (categoryHasSubCategories(category)) {
      // 3-level: category -> subcategory -> target
      INFO_SRF("[HIER-DEBUG] Category has subcategories -> transitioning to AWAIT_SUBCATEGORY");
      gCurrentCategory = category;
      gCurrentSubCategory = "";
      gVoiceState = VoiceState::AWAIT_SUBCATEGORY;
      gCategoryTimeoutMs = millis() + gSettings.srCommandTimeout;
      
      loadSubCategoriesForCategory(category);
      INFO_SRF("[HIER] Now listening for SUBCATEGORY... (timeout in %d ms)", gSettings.srCommandTimeout);
      
      // User-facing feedback
      String normCat = normalizePhrase(category);
      broadcastOutput(String("[Voice] ") + normCat + "... which one?");
      
    } else if (categoryHasDirectTargets(category)) {
      // 2-level: category -> target (no subcategory)
      INFO_SRF("[HIER-DEBUG] Category has direct targets -> transitioning to AWAIT_TARGET");
      gCurrentCategory = category;
      gCurrentSubCategory = "";
      gVoiceState = VoiceState::AWAIT_TARGET;
      gCategoryTimeoutMs = millis() + gSettings.srCommandTimeout;
      
      loadTargetsForCategory(category);
      INFO_SRF("[HIER] Now listening for TARGET... (timeout in %d ms)", gSettings.srCommandTimeout);
      
      // User-facing feedback
      String normCat = normalizePhrase(category);
      broadcastOutput(String("[Voice] ") + normCat + "... what action?");
      
    } else {
      // Single-stage: execute immediately
      INFO_SRF("[HIER-DEBUG] Category has NO targets/subcategories -> single-stage execution");
      const char* cliCmd = findCommandForSingleStageCategory(category);
      if (cliCmd) {
        INFO_SRF("[HIER] Single-stage command -> CLI: %s", cliCmd);
        String normCat = normalizePhrase(category);
        broadcastOutput(String("[Voice] OK, ") + normCat + ".");
        
        static char* cmdOut = nullptr;
        if (!cmdOut) cmdOut = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "sr.cmdOut");
        if (!cmdOut) { WARN_SYSTEMF("Failed to alloc cmdOut"); return; }
        bool ok = executeVoiceCommandAsArmedUser(cliCmd, cmdOut, 2048);
        INFO_SRF("[HIER] Result: %s", ok ? cmdOut : cmdOut);
        if (!ok) {
          broadcastOutput("[VOICE] Command rejected (voice not armed or not authorized)");
        }
        if (ok) systemEventPost(SYSEVT_VOICE_COMMAND, normCat.c_str());
#if ENABLE_OLED_DISPLAY
        else {
          char vmsg[32];
          snprintf(vmsg, sizeof(vmsg), "Voice: %s", normCat.c_str());
          oledNotificationBannerShow(vmsg, PairingRibbonIcon::ERROR_ICON, 2000);
        }
#endif
        gCommandCount++;
        gLastCommand = category;
      } else {
        WARN_SYSTEMF("[HIER] Category '%s' has no associated command!", category);
        broadcastOutput("[Voice] Sorry, I don't know how to do that.");
      }
      // Return to idle
      gVoiceState = VoiceState::IDLE;
      gCurrentCategory = "";
      gCurrentSubCategory = "";
      loadCategories();
    }
    
  } else if (gVoiceState == VoiceState::AWAIT_SUBCATEGORY) {
    // We detected a subcategory within the current category
    const char* subCategory = phrase ? phrase : mappedValue;
    if (!subCategory) {
      WARN_SYSTEMF("[HIER-DEBUG] SubCategory detected but no phrase available!");
      return;
    }
    
    INFO_SRF("[HIER] ============================================");
    INFO_SRF("[HIER] SUBCATEGORY DETECTED: '%s' (category: '%s')", subCategory, gCurrentCategory.c_str());
    INFO_SRF("[HIER] ============================================");
    
    // Now load targets for this category+subcategory combination
    gCurrentSubCategory = subCategory;
    gVoiceState = VoiceState::AWAIT_TARGET;
    gCategoryTimeoutMs = millis() + gSettings.srCommandTimeout;
    
    loadTargetsForCategorySubCategory(gCurrentCategory.c_str(), subCategory);
    INFO_SRF("[HIER] Now listening for TARGET... (timeout in %d ms)", gSettings.srCommandTimeout);
    
    // User-facing feedback
    String normSubCat = normalizePhrase(subCategory);
    broadcastOutput(String("[Voice] ") + normSubCat + "... what action?");
    
  } else if (gVoiceState == VoiceState::AWAIT_TARGET) {
    // We detected a target
    const char* target = phrase ? phrase : "";
    
    INFO_SRF("[HIER] ============================================");
    INFO_SRF("[HIER] TARGET DETECTED: '%s' (category: '%s', subcategory: '%s')", 
             target, gCurrentCategory.c_str(), gCurrentSubCategory.c_str());
    INFO_SRF("[HIER] ============================================");
    
    // Find and execute the command - check if we're in 2-level or 3-level mode
    const char* cliCmd = findCliCommandForId(commandId);
    INFO_SRF("[HIER-DEBUG] CLI command from mapping: '%s'", cliCmd ? cliCmd : "(null)");
    
    if (cliCmd) {
      INFO_SRF("[HIER] EXECUTING: %s", cliCmd);
      
      // User-facing feedback
      String normTarget = normalizePhrase(target);
      if (gCurrentSubCategory.length() > 0) {
        String normSubCat = normalizePhrase(gCurrentSubCategory.c_str());
        broadcastOutput(String("[Voice] OK, ") + normSubCat + " " + normTarget + ".");
        gLastCommand = gCurrentCategory + " " + gCurrentSubCategory + " " + target;
      } else {
        String normCat = normalizePhrase(gCurrentCategory.c_str());
        broadcastOutput(String("[Voice] OK, ") + normCat + " " + normTarget + ".");
        gLastCommand = gCurrentCategory + " " + target;
      }
      
      static char* cmdOut = nullptr;
      if (!cmdOut) cmdOut = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "sr.cmdOut");
      if (!cmdOut) { WARN_SYSTEMF("Failed to alloc cmdOut"); return; }
      bool ok = executeVoiceCommandAsArmedUser(cliCmd, cmdOut, 2048);
      INFO_SRF("[HIER] RESULT: %s", ok ? cmdOut : cmdOut);
      if (!ok) {
        broadcastOutput("[VOICE] Command rejected (voice not armed or not authorized)");
      }
      if (ok) systemEventPost(SYSEVT_VOICE_COMMAND, normTarget.c_str());
#if ENABLE_OLED_DISPLAY
      else {
        char vmsg[32];
        snprintf(vmsg, sizeof(vmsg), "Voice: %s", normTarget.c_str());
        oledNotificationBannerShow(vmsg, PairingRibbonIcon::ERROR_ICON, 2000);
      }
#endif
      gCommandCount++;
    } else {
      WARN_SYSTEMF("[HIER] No CLI command found for '%s'->'%s'->'%s'!", 
                   gCurrentCategory.c_str(), gCurrentSubCategory.c_str(), target);
      broadcastOutput("[Voice] Sorry, I don't understand that.");
    }
    
    // Return to idle, reload categories
    INFO_SRF("[HIER-DEBUG] Returning to IDLE, reloading categories...");
    gVoiceState = VoiceState::IDLE;
    gCurrentCategory = "";
    gCurrentSubCategory = "";
    loadCategories();
    
  } else {
    // Fallback: direct command execution (shouldn't happen in hierarchical mode)
    WARN_SYSTEMF("[HIER-DEBUG] Unexpected state: %s - falling back to direct execution", 
                 voiceStateToString(gVoiceState));
    if (mappedValue) {
      INFO_SRF("Voice command %d ('%s') -> CLI: %s", commandId, phrase ? phrase : "", mappedValue);
      static char* cmdOut = nullptr;
      if (!cmdOut) cmdOut = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "sr.cmdOut");
      if (!cmdOut) { WARN_SYSTEMF("Failed to alloc cmdOut"); return; }
      bool ok = executeVoiceCommandAsArmedUser(mappedValue, cmdOut, 2048);
      INFO_SRF("CLI result: %s", ok ? cmdOut : cmdOut);
      if (!ok) {
        broadcastOutput("[VOICE] Command rejected (voice not armed or not authorized)");
      }
    } else {
      INFO_SRF("Voice command %d ('%s') has no CLI mapping", commandId, phrase ? phrase : "");
    }
  }
  
  INFO_SRF("[HIER-DEBUG] ########## onVoiceCommandDetected() END ##########");
}

// Forward declarations
static bool initI2SMicrophone();
static void deinitI2SMicrophone();
static bool initAFE();
static void deinitAFE();
static bool initMultiNet();
static void deinitMultiNet();
static void srTask(void* param);
static void srSnipWriterTask(void* param);
static void srSnipRingPush(const int16_t* samples, size_t count);
static void srSnipStartSession(const char* reason, int cmdId, const char* phrase);
static void srSnipFeedSession(const int16_t* samples, size_t count);
static void srSnipEndSession(bool save);

static void writeWavHeader(File& f, uint32_t dataSize, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t channels) {
  uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
  uint16_t blockAlign = channels * bitsPerSample / 8;
  uint32_t chunkSize = 36 + dataSize;
  f.write((const uint8_t*)"RIFF", 4);
  f.write((const uint8_t*)&chunkSize, 4);
  f.write((const uint8_t*)"WAVE", 4);
  f.write((const uint8_t*)"fmt ", 4);
  uint32_t subchunk1Size = 16;
  f.write((const uint8_t*)&subchunk1Size, 4);
  uint16_t audioFormat = 1;
  f.write((const uint8_t*)&audioFormat, 2);
  f.write((const uint8_t*)&channels, 2);
  f.write((const uint8_t*)&sampleRate, 4);
  f.write((const uint8_t*)&byteRate, 4);
  f.write((const uint8_t*)&blockAlign, 2);
  f.write((const uint8_t*)&bitsPerSample, 2);
  f.write((const uint8_t*)"data", 4);
  f.write((const uint8_t*)&dataSize, 4);
}

static String srSnipGetFolder() {
  if (gSrSnipDest == SrSnipDest::SD || (gSrSnipDest == SrSnipDest::Auto && VFS::isSDAvailable())) {
    return String(kSrSnipFolderSd);
  }
  return String(kSrSnipFolderInternal);
}

static void srSnipWriterTask(void* param) {
  (void)param;
  INFO_SRF("Snippet writer task started");
  SrSnipJob job;
  while (true) {
    if (xQueueReceive(gSrSnipQueue, &job, portMAX_DELAY) == pdTRUE) {
      if (job.pcm == nullptr || job.samples == 0) {
        SR_DBG_L(2, "SnipWriter: skipping empty job");
        continue;
      }
      String folder = srSnipGetFolder();
      if (!VFS::existsGuarded(folder, VFS::systemAuth("espsr.snip_writer_mkdir"))) {
        VFS::mkdirGuarded(folder, VFS::systemAuth("espsr.snip_writer_mkdir"));
      }
      char fname[128];
      snprintf(fname, sizeof(fname), "%s/%s_%lu_%ld.wav",
               folder.c_str(), job.reason, (unsigned long)job.session_id, (long)job.cmd_id);
      File f = VFS::openGuarded(fname, FILE_WRITE, VFS::systemAuth("espsr.snip_writer"), true);
      if (!f) {
        ERROR_SRF("SnipWriter: failed to open %s", fname);
        free(job.pcm);
        continue;
      }
      uint32_t dataSize = job.samples * sizeof(int16_t);
      size_t written = 0;
      {
        FsLockGuard fsGuard("espsr.snip.write");
        writeWavHeader(f, dataSize, job.sample_rate, job.bits, job.channels);
        written = f.write((const uint8_t*)job.pcm, dataSize);
        f.close();
      }
      free(job.pcm);
      uint32_t durationMs = (job.samples * 1000) / job.sample_rate;
      uint32_t bitrate = (job.sample_rate * job.bits * job.channels) / 1000;
      INFO_SRF("SnipWriter: saved %s (%u samples, %u ms, %u kbps, %u bytes written)",
               fname, (unsigned)job.samples, (unsigned)durationMs, (unsigned)bitrate, (unsigned)written);
    }
  }
}

static bool srSnipInitRingBuffer() {
  if (gSrSnipRing) return true;
  size_t preSamples = (I2S_SR_SAMPLE_RATE * gSrSnipPreMs) / 1000;
  gSrSnipRingSamples = preSamples;
  gSrSnipRing = (int16_t*)heap_caps_malloc(gSrSnipRingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!gSrSnipRing) {
    gSrSnipRing = (int16_t*)malloc(gSrSnipRingSamples * sizeof(int16_t));
  }
  if (!gSrSnipRing) {
    ERROR_SRF("Failed to allocate snippet ring buffer (%u samples)", (unsigned)gSrSnipRingSamples);
    gSrSnipRingSamples = 0;
    return false;
  }
  memset(gSrSnipRing, 0, gSrSnipRingSamples * sizeof(int16_t));
  gSrSnipRingHead = 0;
  SR_DBG_L(1, "Snippet ring buffer allocated: %u samples (%u ms pre-trigger)",
           (unsigned)gSrSnipRingSamples, (unsigned)gSrSnipPreMs);
  return true;
}

static void srSnipFreeRingBuffer() {
  if (gSrSnipRing) {
    free(gSrSnipRing);
    gSrSnipRing = nullptr;
  }
  gSrSnipRingSamples = 0;
  gSrSnipRingHead = 0;
}

static void srSnipRingPush(const int16_t* samples, size_t count) {
  if (!gSrSnipRing || gSrSnipRingSamples == 0 || !samples || count == 0) return;
  for (size_t i = 0; i < count; ++i) {
    gSrSnipRing[gSrSnipRingHead] = samples[i];
    gSrSnipRingHead = (gSrSnipRingHead + 1) % gSrSnipRingSamples;
  }
}

static void srSnipStartSession(const char* reason, int cmdId, const char* phrase) {
  if (gSrSnipSessionActive) {
    SR_DBG_L(1, "SnipSession: already active, ending previous");
    srSnipEndSession(true);
  }
  size_t maxSamples = (I2S_SR_SAMPLE_RATE * gSrSnipMaxMs) / 1000;
  gSrSnipSessionBuf = (int16_t*)heap_caps_malloc(maxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!gSrSnipSessionBuf) {
    gSrSnipSessionBuf = (int16_t*)malloc(maxSamples * sizeof(int16_t));
  }
  if (!gSrSnipSessionBuf) {
    ERROR_SRF("SnipSession: failed to allocate session buffer (%u samples)", (unsigned)maxSamples);
    return;
  }
  gSrSnipSessionSamplesCap = maxSamples;
  gSrSnipSessionSamplesWritten = 0;
  gSrSnipSessionStartMs = millis();
  gSrSnipSessionDeadlineMs = gSrSnipSessionStartMs + gSrSnipMaxMs;
  gSrSnipSessionId++;
  gSrSnipSessionCmdId = cmdId;
  strncpy(gSrSnipSessionPhrase, phrase ? phrase : "", sizeof(gSrSnipSessionPhrase) - 1);
  gSrSnipSessionPhrase[sizeof(gSrSnipSessionPhrase) - 1] = '\0';
  strncpy(gSrSnipSessionReason, reason ? reason : "snip", sizeof(gSrSnipSessionReason) - 1);
  gSrSnipSessionReason[sizeof(gSrSnipSessionReason) - 1] = '\0';
  if (gSrSnipRing && gSrSnipRingSamples > 0) {
    size_t copyCount = (gSrSnipRingSamples < maxSamples) ? gSrSnipRingSamples : maxSamples;
    size_t startIdx = (gSrSnipRingHead + gSrSnipRingSamples - copyCount) % gSrSnipRingSamples;
    for (size_t i = 0; i < copyCount && gSrSnipSessionSamplesWritten < gSrSnipSessionSamplesCap; ++i) {
      gSrSnipSessionBuf[gSrSnipSessionSamplesWritten++] = gSrSnipRing[(startIdx + i) % gSrSnipRingSamples];
    }
    SR_DBG_L(2, "SnipSession: copied %u pre-trigger samples from ring", (unsigned)copyCount);
  }
  gSrSnipSessionActive = true;
  SR_DBG_L(1, "SnipSession: started (reason=%s, id=%u, maxMs=%u)", reason, (unsigned)gSrSnipSessionId, (unsigned)gSrSnipMaxMs);
}

static void srSnipFeedSession(const int16_t* samples, size_t count) {
  if (!gSrSnipSessionActive || !gSrSnipSessionBuf || !samples || count == 0) return;
  if (millis() > gSrSnipSessionDeadlineMs) {
    SR_DBG_L(1, "SnipSession: deadline reached, ending");
    srSnipEndSession(true);
    return;
  }
  size_t spaceLeft = gSrSnipSessionSamplesCap - gSrSnipSessionSamplesWritten;
  size_t toCopy = (count < spaceLeft) ? count : spaceLeft;
  if (toCopy > 0) {
    memcpy(&gSrSnipSessionBuf[gSrSnipSessionSamplesWritten], samples, toCopy * sizeof(int16_t));
    gSrSnipSessionSamplesWritten += toCopy;
  }
  if (gSrSnipSessionSamplesWritten >= gSrSnipSessionSamplesCap) {
    SR_DBG_L(1, "SnipSession: buffer full, ending");
    srSnipEndSession(true);
  }
}

static void srSnipEndSession(bool save) {
  if (!gSrSnipSessionActive) return;
  gSrSnipSessionActive = false;
  if (!save || gSrSnipSessionSamplesWritten == 0 || !gSrSnipSessionBuf) {
    SR_DBG_L(1, "SnipSession: ended without saving (save=%d, samples=%u)", save, (unsigned)gSrSnipSessionSamplesWritten);
    if (gSrSnipSessionBuf) {
      free(gSrSnipSessionBuf);
      gSrSnipSessionBuf = nullptr;
    }
    return;
  }
  if (!gSrSnipQueue) {
    WARN_SRF("SnipSession: no queue, discarding");
    free(gSrSnipSessionBuf);
    gSrSnipSessionBuf = nullptr;
    return;
  }
  SrSnipJob job;
  job.pcm = gSrSnipSessionBuf;
  job.samples = gSrSnipSessionSamplesWritten;
  job.sample_rate = I2S_SR_SAMPLE_RATE;
  job.bits = I2S_SR_BITS;
  job.channels = I2S_SR_CHANNELS;
  job.created_ms = gSrSnipSessionStartMs;
  job.session_id = gSrSnipSessionId;
  job.cmd_id = gSrSnipSessionCmdId;
  job.dest = gSrSnipDest;
  strncpy(job.phrase, gSrSnipSessionPhrase, sizeof(job.phrase) - 1);
  job.phrase[sizeof(job.phrase) - 1] = '\0';
  strncpy(job.reason, gSrSnipSessionReason, sizeof(job.reason) - 1);
  job.reason[sizeof(job.reason) - 1] = '\0';
  gSrSnipSessionBuf = nullptr;
  gSrSnipSessionSamplesWritten = 0;
  if (xQueueSend(gSrSnipQueue, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
    WARN_SRF("SnipSession: queue full, discarding");
    free(job.pcm);
  } else {
    SR_DBG_L(1, "SnipSession: queued %u samples for writing", (unsigned)job.samples);
  }
}

static bool srSnipInit() {
  if (gSrSnipQueue) return true;
  gSrSnipQueue = xQueueCreate(4, sizeof(SrSnipJob));
  if (!gSrSnipQueue) {
    ERROR_SRF("Failed to create snippet queue");
    return false;
  }
  taskStackRecord("sr_snip_wr", SR_SNIP_STACK_WORDS);
  BaseType_t ret = xTaskCreatePinnedToCore(srSnipWriterTask, "sr_snip_wr", SR_SNIP_STACK_WORDS, nullptr, TASK_PRIORITY_NORMAL, &gSrSnipWriterTask, 0);
  if (ret != pdPASS) {
    ERROR_SRF("Failed to create snippet writer task");
    vQueueDelete(gSrSnipQueue);
    gSrSnipQueue = nullptr;
    return false;
  }
  if (!srSnipInitRingBuffer()) {
    WARN_SRF("Snippet ring buffer init failed, capture may be incomplete");
  }
  INFO_SRF("Snippet capture system initialized");
  return true;
}

static void srSnipDeinit() {
  if (gSrSnipSessionActive) {
    srSnipEndSession(false);
  }
  if (gSrSnipWriterTask) {
    vTaskDelete(gSrSnipWriterTask);
    gSrSnipWriterTask = nullptr;
  }
  if (gSrSnipQueue) {
    SrSnipJob job;
    while (xQueueReceive(gSrSnipQueue, &job, 0) == pdTRUE) {
      if (job.pcm) free(job.pcm);
    }
    vQueueDelete(gSrSnipQueue);
    gSrSnipQueue = nullptr;
  }
  srSnipFreeRingBuffer();
  INFO_SRF("Snippet capture system deinitialized");
}

// Auto-tuning configurations to cycle through
struct AutoTuneConfig {
  float afeGain;
  float dynGainMax;
  bool dynGainEnabled;
  const char* description;
};

// Columns: afeGain, dynGainMax, dynGainEnabled, description
static const AutoTuneConfig kAutoTuneConfigs[] = {
  { 1.0f, 2.5f, true,  "Baseline: gain=1.0, dyngain max=2.5" },
  { 2.0f, 2.0f, true,  "Higher input: gain=2.0, dyngain max=2.0" },
  { 3.0f, 1.5f, true,  "High input: gain=3.0, dyngain max=1.5" },
  { 4.0f, 1.2f, true,  "Very high input: gain=4.0, dyngain max=1.2" },
  { 2.0f, 0.0f, false, "No dyngain: gain=2.0, dyngain OFF" },
  { 3.0f, 0.0f, false, "No dyngain high: gain=3.0, dyngain OFF" },
};
static const size_t kAutoTuneConfigCount = sizeof(kAutoTuneConfigs) / sizeof(kAutoTuneConfigs[0]);

// Check and advance auto-tune step
static void srAutoTuneCheck() {
  if (!gSrAutoTuneActive) return;
  
  uint32_t now = millis();
  uint32_t elapsed = now - gSrAutoTuneStepStartMs;
  
  if (elapsed >= kAutoTuneStepDurationMs) {
    // Move to next step
    gSrAutoTuneStep++;
    
    if (gSrAutoTuneStep >= kAutoTuneConfigCount) {
      // All steps complete
      gSrAutoTuneActive = false;
      gSrRawOutputEnabled = false;
      broadcastOutput("");
      broadcastOutput("=== AUTO-TUNE COMPLETE ===");
      broadcastOutput(String("Tested ") + (int)kAutoTuneConfigCount + " configurations. Review logs above to find best settings.");
      broadcastOutput("Apply best config with: sr tuning gain <value> and sr dyngain max <value>");
      return;
    }
    
    // Apply next config
    gSrAutoTuneStepStartMs = now;
    const AutoTuneConfig& cfg = kAutoTuneConfigs[gSrAutoTuneStep];
    gSettings.srAfeGain = cfg.afeGain;
    gSrDynGainMax = cfg.dynGainMax;
    gSrDynGainEnabled = cfg.dynGainEnabled;
    gSrDynGainCurrent = 1.0f;
    
    broadcastOutput("");
    broadcastOutput(String("=== AUTO-TUNE Step ") + (gSrAutoTuneStep + 1) + "/" + (int)kAutoTuneConfigCount + " ===");
    broadcastOutput(String("Config: ") + cfg.description);
    broadcastOutput("Say test phrases now! (NOTE: AFE gain change needs SR restart)");
  }
}

static void srDebugPrintTelemetry() {
  // Check auto-tune advancement
  srAutoTuneCheck();
  
  uint32_t uptimeMs = millis();
  // Use WARN level so telemetry always prints (INFO requires DEBUG_SYSTEM flag)
  WARN_SRF("=== SR Telemetry ===");
  WARN_SRF("Uptime: %u ms, Running: %s", (unsigned)uptimeMs, gESPSRRunning ? "yes" : "no");
  
  // Show raw mode and auto-tune status
  if (gSrRawOutputEnabled || gSrAutoTuneActive) {
    WARN_SRF("Raw=%s AutoTune=%s (step %d/%d)", 
                 gSrRawOutputEnabled ? "ON" : "OFF",
                 gSrAutoTuneActive ? "ACTIVE" : "off",
                 gSrAutoTuneStep + 1, (int)kAutoTuneConfigCount);
  }
  WARN_SRF("I2S: reads_ok=%u, reads_err=%u, reads_zero=%u, bytes_ok=%llu",
           (unsigned)gSrI2SReadOk, (unsigned)gSrI2SReadErr, (unsigned)gSrI2SReadZero, (unsigned long long)gSrI2SBytesOk);
  WARN_SRF("I2S: est_rate=%.1f Hz", gSrEstSampleRateHz);
  WARN_SRF("PCM: min=%d, max=%d, abs_avg=%.1f",
           (int)gSrLastPcmMin, (int)gSrLastPcmMax, gSrLastPcmAbsAvg);
  WARN_SRF("AFE: feed_chunk=%d, fetch_chunk=%d", gSrAfeFeedChunk, gSrAfeFetchChunk);
  WARN_SRF("AFE: feeds=%u, fetches=%u, last_vol=%.1f dB, last_vad=%d, last_ret=%d",
           (unsigned)gSrAfeFeedOk, (unsigned)gSrAfeFetchOk, gSrLastVolumeDb, gSrLastVadState, gSrLastAfeRetValue);
  WARN_SRF("Wake: count=%u, last_ms=%u, last_idx=%d, last_model=%d",
           (unsigned)gWakeWordCount, (unsigned)gLastWakeMs, gSrLastWakeWordIndex, gSrLastWakeNetModelIndex);
  WARN_SRF("MN: detect_calls=%u, detected=%u, accepted=%u, last_cmd='%s'",
           (unsigned)gSrMnDetectCalls, (unsigned)gSrMnDetected, (unsigned)gCommandCount, gLastCommand.c_str());
  WARN_SRF("Accept: gap_enabled=%d floor=%.2f gap=%.2f require_speech=%d gap_accepts=%u rejects=%u",
           gSrGapAcceptEnabled ? 1 : 0, gSrGapAcceptFloor, gSrGapAcceptGap,
           gSrTargetRequireSpeech ? 1 : 0, (unsigned)gSrGapAccepts, (unsigned)gSrLowConfidenceRejects);
  WARN_SRF("DynGain: enabled=%d cur=%.2f min=%.2f max=%.2f target_peak=%.0f alpha=%.2f applied=%u bypassed=%u",
           gSrDynGainEnabled ? 1 : 0, gSrDynGainCurrent, gSrDynGainMin, gSrDynGainMax,
           gSrDynGainTargetPeak, gSrDynGainAlpha, (unsigned)gSrDynGainApplied, (unsigned)gSrDynGainBypassed);
  WARN_SRF("Snip: enabled=%d, session_active=%d, ring_samples=%u",
           gSrSnipEnabled ? 1 : 0, gSrSnipSessionActive ? 1 : 0, (unsigned)gSrSnipRingSamples);
  WARN_SRF("====================");
}

static void srDebugResetCounters() {
  gSrI2SBytesOk = 0;
  gSrI2SReadOk = 0;
  gSrI2SReadErr = 0;
  gSrI2SReadZero = 0;
  gSrAfeFeedOk = 0;
  gSrAfeFetchOk = 0;
  gSrMnDetectCalls = 0;
  gSrMnDetected = 0;
  gSrLowConfidenceRejects = 0;
  gSrGapAccepts = 0;
  gSrDynGainApplied = 0;
  gSrDynGainBypassed = 0;
  gSrDynGainCurrent = 1.0f;
  INFO_SRF("Debug counters reset");
}

static void restoreMicrophoneAfterSRIfNeeded() {
#if ENABLE_MICROPHONE
  if (!gRestoreMicAfterSR) return;
  gRestoreMicAfterSR = false;

  INFO_SRF("Restoring microphone sensor after SR...");
  if (!initMicrophone()) {
    WARN_SRF("Failed to restore microphone sensor after SR");
  }
#else
  gRestoreMicAfterSR = false;
#endif
}

static bool ensureMNCommandMutex() {
  if (gMNCommandMutex) return true;
  gMNCommandMutex = xSemaphoreCreateMutex();
  return (gMNCommandMutex != nullptr);
}

static bool lockMN(uint32_t timeoutMs) {
  if (!gMNCommandMutex) return true;
  return xSemaphoreTake(gMNCommandMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

static void unlockMN() {
  if (!gMNCommandMutex) return;
  xSemaphoreGive(gMNCommandMutex);
}

static bool mnCommandsReady() {
  if (!gMNModel || !gMNData) return false;
  if (!ensureMNCommandMutex()) return false;
  if (gMNCommandsAllocated) return true;
  if (!lockMN(1000)) return false;
  esp_err_t err = esp_mn_commands_alloc(gMNModel, gMNData);
  unlockMN();
  if (err != ESP_OK) return false;
  gMNCommandsAllocated = true;
  return true;
}

static esp_mn_error_t* mnUpdateLocked() {
  return esp_mn_commands_update();
}

static bool isAllDigits(const String& s) {
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    if (!isDigit(s[i])) return false;
  }
  return true;
}

static float clampFloat(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static int16_t clampS16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

static bool loadCommandsFileLocked(size_t& outAdded, size_t& outErrors) {
  outAdded = 0;
  outErrors = 0;

  if (!VFS::isSDAvailable()) {
    return false;
  }

  if (!VFS::existsGuarded(kESPSRCommandFile, VFS::systemAuth("espsr.commands_load"))) {
    return true;
  }

  File f = VFS::openGuarded(kESPSRCommandFile, "r", VFS::systemAuth("espsr.commands_load"));
  if (!f) {
    return false;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int sep = line.indexOf(':');
    if (sep <= 0) continue;
    String idStr = line.substring(0, sep);
    String phrase = line.substring(sep + 1);
    idStr.trim();
    phrase.trim();
    if (idStr.length() == 0 || phrase.length() == 0) continue;
    int id = idStr.toInt();
    if (id <= 0) continue;
    esp_err_t err = esp_mn_commands_add(id, phrase.c_str());
    if (err == ESP_OK) {
      outAdded++;
    } else {
      outErrors++;
    }
  }

  f.close();
  return true;
}

static bool saveCommandsFileLocked(size_t& outSaved) {
  outSaved = 0;

  FsLockGuard fsGuard("espsr.commands.save");

  if (!VFS::isSDAvailable()) {
    return false;
  }

  VFS::mkdirGuarded("/sd/ESPSR", VFS::systemAuth("espsr.commands_save_mkdir"));
  File f = VFS::openGuarded(kESPSRCommandFile, "w", VFS::systemAuth("espsr.commands_save"), true);
  if (!f) {
    return false;
  }

  for (int i = 0; ; ++i) {
    esp_mn_phrase_t* phrase = esp_mn_commands_get_from_index(i);
    if (!phrase) break;
    if (!phrase->string) continue;
    f.print((int)phrase->command_id);
    f.print(':');
    f.println(phrase->string);
    outSaved++;
  }
  f.close();
  return true;
}

// ============================================================================
// I2S Microphone Setup
// ============================================================================

static bool initI2SMicrophone() {
#if ENABLE_MICROPHONE
  // Honor the device-wide source preference (same one the mic sensor uses), then
  // lease capture for SR. audioCaptureStart resolves the source (PDM-first if the
  // preference is unavailable) and, for G2, arms the ring + enables the glasses
  // stream — so SR runs off the glasses mic on a PDM-less board too.
  if (gSettings.micSource == "pdm" && audioSourceAvailable(AUDIO_SRC_LOCAL_PDM)) {
    audioSetSource(AUDIO_SRC_LOCAL_PDM);
  } else if (gSettings.micSource == "g2" && audioSourceAvailable(AUDIO_SRC_G2_LEFT)) {
    audioSetSource(AUDIO_SRC_G2_LEFT);
  }
  return audioCaptureStart("sr", I2S_SR_SAMPLE_RATE);
#else
  return false;   // no mic subsystem compiled in this build
#endif  // ENABLE_MICROPHONE
}

static void deinitI2SMicrophone() {
#if ENABLE_MICROPHONE
  audioCaptureStop("sr");          // release SR's capture lease (PDM or G2)
#endif  // ENABLE_MICROPHONE
}

// ============================================================================
// AFE (Audio Front-End) Setup
// ============================================================================

static bool initAFE() {
  WARN_SYSTEMF("[SR_AFE] ========== initAFE() START ==========");
  WARN_SYSTEMF("[SR_AFE] Heap: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  
  // Get AFE interface
  WARN_SYSTEMF("[SR_AFE] Getting AFE interface from ESP_AFE_SR_HANDLE...");
  gAFE = (esp_afe_sr_iface_t*)&ESP_AFE_SR_HANDLE;
  WARN_SYSTEMF("[SR_AFE] AFE interface pointer: %p", gAFE);
  if (!gAFE) {
    ERROR_SRF("Failed to get AFE interface");
    return false;
  }
  
  // Load models based on srModelSource setting
  // 0 = partition (default), 1 = SD card, 2 = LittleFS
  srmodel_list_t *models = nullptr;
  WARN_SYSTEMF("[SR_AFE] srModelSource setting = %d (0=partition, 1=SD, 2=LittleFS)", gSettings.srModelSource);
  
  if (gSettings.srModelSource == 1) {
    // Try SD card models
    WARN_SYSTEMF("[SR_AFE] Attempting to load models from SD card: /sd/ESP-SR Models");
    models = esp_srmodel_init("/sd/ESP-SR Models");
    WARN_SYSTEMF("[SR_AFE] SD card esp_srmodel_init returned: %p", models);
    if (models) {
      INFO_SRF("SD card models loaded successfully");
    } else {
      INFO_SRF("SD card model loading failed, falling back to partition models");
      logSystemEvent("SR", "model source fallback: SD load failed → using partition models (not the configured source)");
    }
  } else if (gSettings.srModelSource == 2) {
    // Try LittleFS models
    WARN_SYSTEMF("[SR_AFE] Attempting to load models from LittleFS: /ESP-SR Models");
    models = esp_srmodel_init("/ESP-SR Models");
    WARN_SYSTEMF("[SR_AFE] LittleFS esp_srmodel_init returned: %p", models);
    if (models) {
      INFO_SRF("LittleFS models loaded successfully");
    } else {
      INFO_SRF("LittleFS model loading failed, falling back to partition models");
      logSystemEvent("SR", "model source fallback: LittleFS load failed → using partition models (not the configured source)");
    }
  }
  
  // Use partition-based models if not loaded from external source or if external failed
  if (!models) {
    // First check if models were already initialized (from previous call)
    WARN_SYSTEMF("[SR_AFE] Checking get_static_srmodels()...");
    models = get_static_srmodels();
    WARN_SYSTEMF("[SR_AFE] get_static_srmodels returned: %p", models);
    if (!models) {
      // Initialize models from the "model" partition using esp_srmodel_init
      // This function takes the partition LABEL as a string
      WARN_SYSTEMF("[SR_AFE] Calling esp_srmodel_init('model') for partition...");
      models = esp_srmodel_init("model");
      WARN_SYSTEMF("[SR_AFE] Partition esp_srmodel_init returned: %p", models);
      if (models) {
        INFO_SRF("Partition models loaded successfully");
      } else {
        ERROR_SRF("Failed to load models from partition");
        logSystemEvent("SR", "FAILED to load models from partition — speech recognition unavailable this boot");
      }
    } else {
      INFO_SRF("Using previously initialized models");
    }
  }
  
  // Check for wake word model availability
  char* wn_name = nullptr;
  if (models) {
    WARN_SYSTEMF("[SR_AFE] Models pointer valid, calling esp_srmodel_filter with ESP_WN_PREFIX...");
    wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    WARN_SYSTEMF("[SR_AFE] esp_srmodel_filter returned: %s", wn_name ? wn_name : "(NULL)");
  } else {
    ERROR_SRF("Models pointer is NULL, cannot filter");
  }
  
  // If no wake word model, we cannot run ESP-SR meaningfully
  if (!wn_name) {
    ERROR_SRF("No wake word model found!");
    ERROR_SRF("Ensure CONFIG_SR_WN_* is enabled in sdkconfig");
    gAFE = nullptr;
    return false;
  }
  
  INFO_SRF("Found wake word model: %s", wn_name);
  // Capture the resolved name so the web status card can display it.
  // Cleared in stopESPSR() when AFE is torn down.
  strncpy(gWnModelName, wn_name, sizeof(gWnModelName) - 1);
  gWnModelName[sizeof(gWnModelName) - 1] = '\0';

  // Configure AFE
  WARN_SYSTEMF("[SR_AFE] Creating AFE config with AFE_CONFIG_DEFAULT()...");
  afe_config_t afe_config = AFE_CONFIG_DEFAULT();
  afe_config.wakenet_model_name = wn_name;
  afe_config.aec_init = false;  // No echo cancellation (no speaker feedback)
  afe_config.se_init = true;    // Enable noise suppression
  afe_config.vad_init = true;   // Enable voice activity detection
  afe_config.wakenet_init = true;
  afe_config.voice_communication_init = false;
  afe_config.afe_ringbuf_size = 50;
  afe_config.afe_linear_gain = gSettings.srAfeGain;
  afe_config.memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  
  // AGC mode from settings (0=off, 1=-9dB, 2=-6dB, 3=-3dB)
  switch (gSettings.srAgcMode) {
    case 0: afe_config.agc_mode = AFE_MN_PEAK_NO_AGC; break;
    case 1: afe_config.agc_mode = AFE_MN_PEAK_AGC_MODE_1; break;
    case 3: afe_config.agc_mode = AFE_MN_PEAK_AGC_MODE_3; break;
    default: afe_config.agc_mode = AFE_MN_PEAK_AGC_MODE_2; break;
  }
  
  // VAD mode from settings (0-4, higher = more sensitive)
  afe_config.vad_mode = (vad_mode_t)gSettings.srVadMode;
  
  // Configure for single PDM microphone with no reference channel
  // XIAO ESP32S3 Sense has only 1 PDM mic and no speaker for echo cancellation
  // total_ch_num MUST equal mic_num + ref_num
  afe_config.pcm_config.total_ch_num = 1;
  afe_config.pcm_config.mic_num = 1;
  afe_config.pcm_config.ref_num = 0;
  
  WARN_SYSTEMF("[SR_AFE] AFE config: aec=%d, se=%d, vad=%d, wakenet=%d, voice_comm=%d",
               afe_config.aec_init, afe_config.se_init, afe_config.vad_init,
               afe_config.wakenet_init, afe_config.voice_communication_init);
  WARN_SYSTEMF("[SR_AFE] AFE config: ringbuf_size=%d, linear_gain=%.2f, agc_mode=%d",
               afe_config.afe_ringbuf_size, afe_config.afe_linear_gain, afe_config.agc_mode);
  WARN_SYSTEMF("[SR_AFE] AFE pcm_config: total_ch=%d, mic_num=%d, ref_num=%d",
               afe_config.pcm_config.total_ch_num, afe_config.pcm_config.mic_num, afe_config.pcm_config.ref_num);
  
  WARN_SYSTEMF("[SR_AFE] Heap before AFE create: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  WARN_SYSTEMF("[SR_AFE] Calling gAFE->create_from_config()...");
  gAFEData = gAFE->create_from_config(&afe_config);
  WARN_SYSTEMF("[SR_AFE] gAFE->create_from_config returned: %p", gAFEData);
  WARN_SYSTEMF("[SR_AFE] Heap after AFE create: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  if (!gAFEData) {
    ERROR_SRF("Failed to create AFE from config");
    gAFE = nullptr;
    return false;
  }
  
  WARN_SYSTEMF("[SR_AFE] ========== initAFE() SUCCESS ==========");
  INFO_SRF("AFE initialized successfully");
  return true;
}

static void deinitAFE() {
  if (gAFE && gAFEData) {
    gAFE->destroy(gAFEData);
    gAFEData = nullptr;
    gAFE = nullptr;
    DEBUG_SRF("AFE deinitialized");
  }
}

// ============================================================================
// MultiNet (Command Recognition) Setup
// ============================================================================

static bool initMultiNet() {
  WARN_SYSTEMF("[SR_MN] ========== initMultiNet() START ==========");
  WARN_SYSTEMF("[SR_MN] Heap: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  
  // Get available models (should already be initialized by initAFE)
  WARN_SYSTEMF("[SR_MN] Calling get_static_srmodels()...");
  srmodel_list_t *models = get_static_srmodels();
  WARN_SYSTEMF("[SR_MN] get_static_srmodels returned: %p", models);
  if (!models) {
    WARN_SRF("No models available, command recognition disabled");
    return true;  // Not a fatal error
  }
  
  // Get MultiNet model
  WARN_SYSTEMF("[SR_MN] Calling esp_srmodel_filter for MultiNet...");
  char* mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
  WARN_SYSTEMF("[SR_MN] esp_srmodel_filter returned: %s", mn_name ? mn_name : "(NULL)");
  if (!mn_name) {
    WARN_SRF("No MultiNet model found, command recognition disabled");
    return true;
  }
  
  WARN_SYSTEMF("[SR_MN] Calling esp_mn_handle_from_name('%s')...", mn_name);
  gMNModel = esp_mn_handle_from_name(mn_name);
  WARN_SYSTEMF("[SR_MN] esp_mn_handle_from_name returned: %p", gMNModel);
  if (!gMNModel) {
    WARN_SRF("Failed to get MultiNet handle for: %s", mn_name);
    return true;
  }
  
  WARN_SYSTEMF("[SR_MN] Calling gMNModel->create('%s', %d)...", mn_name, gSettings.srCommandTimeout);
  gMNData = gMNModel->create(mn_name, gSettings.srCommandTimeout);
  WARN_SYSTEMF("[SR_MN] gMNModel->create returned: %p", gMNData);
  WARN_SYSTEMF("[SR_MN] Heap after MN create: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  if (!gMNData) {
    WARN_SRF("Failed to create MultiNet data");
    gMNModel = nullptr;
    return true;
  }

  if (mnCommandsReady()) {
    if (lockMN(2000)) {
      size_t added = 0;
      size_t parseErrors = 0;
      bool ok = loadCommandsFileLocked(added, parseErrors);
      esp_mn_error_t* errList = mnUpdateLocked();
      unlockMN();

      if (!ok) {
        WARN_SRF("Failed to read commands file: %s", kESPSRCommandFile);
      } else {
        INFO_SRF("Loaded %u commands from %s", (unsigned)added, kESPSRCommandFile);
      }
      if (parseErrors > 0) {
        WARN_SRF("%u command lines could not be added", (unsigned)parseErrors);
      }
      if (errList && errList->num > 0) {
        WARN_SRF("%d commands rejected by MultiNet", errList->num);
      }
    } else {
      WARN_SRF("Failed to lock MultiNet for command load");
    }
  }
  
  // Capture the resolved name so the web status card can display it.
  // Cleared in stopESPSR() when MultiNet is torn down.
  strncpy(gMnModelName, mn_name, sizeof(gMnModelName) - 1);
  gMnModelName[sizeof(gMnModelName) - 1] = '\0';

  INFO_SRF("MultiNet initialized: %s", mn_name);
  return true;
}

static void deinitMultiNet() {
  if (gMNModel && gMNData) {
    if (lockMN(2000)) {
      esp_mn_commands_free();
      unlockMN();
    }
    gMNCommandsAllocated = false;
    gMNModel->destroy(gMNData);
    gMNData = nullptr;
    gMNModel = nullptr;
    DEBUG_SRF("MultiNet deinitialized");
  }
}

// ============================================================================
// Speech Recognition Task
// ============================================================================

static void srTask(void* param) {
  (void)param;
  
  WARN_SYSTEMF("[SR_TASK] ========== srTask() STARTED ==========");
  WARN_SYSTEMF("[SR_TASK] Heap: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  WARN_SYSTEMF("[SR_TASK] Running on core %d, priority %d", xPortGetCoreID(), uxTaskPriorityGet(NULL));
  
  int afeFeedChunk = 0;
  int afeFetchChunk = 0;
  int afeSampleRate = I2S_SR_SAMPLE_RATE;
  int afeTotalChannels = 1;
  int afeMicChannels = 1;
  
  WARN_SYSTEMF("[SR_TASK] gAFE=%p, gAFEData=%p", gAFE, gAFEData);
  if (gAFE && gAFEData) {
    WARN_SYSTEMF("[SR_TASK] Querying AFE parameters...");
    afeFeedChunk = gAFE->get_feed_chunksize(gAFEData);
    afeFetchChunk = gAFE->get_fetch_chunksize(gAFEData);
    afeSampleRate = gAFE->get_samp_rate(gAFEData);
    afeTotalChannels = gAFE->get_total_channel_num(gAFEData);
    afeMicChannels = gAFE->get_channel_num(gAFEData);
    WARN_SYSTEMF("[SR_TASK] AFE feed_chunk=%d samples (%d bytes)", afeFeedChunk, afeFeedChunk * (int)sizeof(int16_t));
    WARN_SYSTEMF("[SR_TASK] AFE fetch_chunk=%d samples (%d bytes)", afeFetchChunk, afeFetchChunk * (int)sizeof(int16_t));
    WARN_SYSTEMF("[SR_TASK] AFE sample_rate=%d Hz", afeSampleRate);
    WARN_SYSTEMF("[SR_TASK] AFE total_channels=%d, mic_channels=%d", afeTotalChannels, afeMicChannels);
    INFO_SRF("AFE params: feed_chunk=%d, fetch_chunk=%d, rate=%d, total_ch=%d, mic_ch=%d",
             afeFeedChunk, afeFetchChunk, afeSampleRate, afeTotalChannels, afeMicChannels);
  } else {
    WARN_SYSTEMF("[SR_TASK] WARNING: AFE not initialized! gAFE=%p gAFEData=%p", gAFE, gAFEData);
  }

  gSrAfeFeedChunk = afeFeedChunk;
  gSrAfeFetchChunk = afeFetchChunk;

  size_t feedChunkSamples = (afeFeedChunk > 0) ? (size_t)afeFeedChunk : (size_t)160;
  size_t feedChunkBytes = feedChunkSamples * sizeof(int16_t);

  size_t mnBufSamplesCap = (afeFetchChunk > 0) ? (size_t)afeFetchChunk : (size_t)160;
  size_t mnBufBytes = mnBufSamplesCap * sizeof(int16_t);

  size_t i2sReadBytes = (size_t)SR_AUDIO_CHUNK_SIZE;
  if (i2sReadBytes < feedChunkBytes) {
    i2sReadBytes = feedChunkBytes;
  }
  size_t i2sReadSamplesCap = i2sReadBytes / sizeof(int16_t);

  WARN_SYSTEMF("[SR_TASK] Allocating buffers: i2sRead=%u bytes, afeFeed=%u bytes, ring=%u samples, mn=%u bytes",
               (unsigned)i2sReadBytes, (unsigned)feedChunkBytes, (unsigned)(feedChunkSamples * 16), (unsigned)mnBufBytes);
  
  int16_t* i2sReadBuf = (int16_t*)heap_caps_malloc(i2sReadBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  WARN_SYSTEMF("[SR_TASK] i2sReadBuf (PSRAM): %p", i2sReadBuf);
  if (!i2sReadBuf) {
    i2sReadBuf = (int16_t*)malloc(i2sReadBytes);
    WARN_SYSTEMF("[SR_TASK] i2sReadBuf (fallback heap): %p", i2sReadBuf);
  }
  int16_t* afeFeedBuf = (int16_t*)heap_caps_malloc(feedChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  WARN_SYSTEMF("[SR_TASK] afeFeedBuf (PSRAM): %p", afeFeedBuf);
  if (!afeFeedBuf) {
    afeFeedBuf = (int16_t*)malloc(feedChunkBytes);
    WARN_SYSTEMF("[SR_TASK] afeFeedBuf (fallback heap): %p", afeFeedBuf);
  }
  size_t ringSamplesCap = feedChunkSamples * 16;
  int16_t* ringBuf = (int16_t*)heap_caps_malloc(ringSamplesCap * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  WARN_SYSTEMF("[SR_TASK] ringBuf (PSRAM): %p", ringBuf);
  if (!ringBuf) {
    ringBuf = (int16_t*)malloc(ringSamplesCap * sizeof(int16_t));
    WARN_SYSTEMF("[SR_TASK] ringBuf (fallback heap): %p", ringBuf);
  }
  int16_t* mnInputBuf = (int16_t*)heap_caps_malloc(mnBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  WARN_SYSTEMF("[SR_TASK] mnInputBuf (PSRAM): %p", mnInputBuf);
  if (!mnInputBuf) {
    mnInputBuf = (int16_t*)malloc(mnBufBytes);
    WARN_SYSTEMF("[SR_TASK] mnInputBuf (fallback heap): %p", mnInputBuf);
  }
  if (!i2sReadBuf || !afeFeedBuf || !ringBuf || !mnInputBuf) {
    ERROR_SRF("Failed to allocate SR buffers (read=%u, feed=%u, ring=%u samples, mn=%u bytes)",
             (unsigned)i2sReadBytes, (unsigned)feedChunkBytes, (unsigned)ringSamplesCap, (unsigned)mnBufBytes);
    if (i2sReadBuf) free(i2sReadBuf);
    if (afeFeedBuf) free(afeFeedBuf);
    if (ringBuf) free(ringBuf);
    if (mnInputBuf) free(mnInputBuf);
    gSRTaskShouldRun = false;
    vTaskDelete(nullptr);
    return;
  }

  size_t ringHead = 0;
  size_t ringCount = 0;
  size_t fedSinceFetchSamples = 0;

  WARN_SYSTEMF("[SR_TASK] Buffers allocated OK. feed_chunk=%u samples, i2s_read_cap=%u samples, ring_cap=%u samples, mn_cap=%u samples",
               (unsigned)feedChunkSamples, (unsigned)i2sReadSamplesCap, (unsigned)ringSamplesCap, (unsigned)mnBufSamplesCap);
  WARN_SYSTEMF("[SR_TASK] (PDM I2S owned by HAL_Audio)");
  SR_DBG_L(1, "SR buffers: feed_chunk=%u samples, read_cap=%u samples (%u bytes), ring_cap=%u samples",
           (unsigned)feedChunkSamples, (unsigned)i2sReadSamplesCap, (unsigned)i2sReadBytes, (unsigned)ringSamplesCap);
  
  bool listeningForCommand = false;
  uint32_t commandTimeoutMs = 0;
  bool commandSpeechStarted = false;  // Has user started speaking the command?
  uint32_t loopCount = 0;
  uint32_t lastDetailedLogLoop = 0;
  
  WARN_SYSTEMF("[SR_TASK] ========== ENTERING MAIN LOOP ==========");
  
  while (gSRTaskShouldRun) {
    loopCount++;
    bool doDetailedLog = (loopCount <= 5) || (loopCount - lastDetailedLogLoop >= 500);
    
    if (gSrSnipManualStartRequested) {
      gSrSnipManualStartRequested = false;
      srSnipStartSession("manual", -1, nullptr);
    }
    if (gSrSnipManualStopRequested) {
      gSrSnipManualStopRequested = false;
      srSnipEndSession(true);
    }
    
    if (gSrTelemetryPeriodMs > 0) {
      uint32_t now = millis();
      if (now - gSrLastTelemetryMs >= gSrTelemetryPeriodMs) {
        uint32_t dt = now - gSrLastTelemetryMs;
        uint64_t dbytes = gSrI2SBytesOk - gSrLastTelemetryBytesOk;
        if (dt > 0) {
          gSrEstSampleRateHz = (float)((double)dbytes * 1000.0 / (double)dt / (double)(sizeof(int16_t) * I2S_SR_CHANNELS));
        }
        gSrLastTelemetryMs = now;
        gSrLastTelemetryBytesOk = gSrI2SBytesOk;
        srDebugPrintTelemetry();
      }
    }
    
    size_t bytesRead = 0;
    uint32_t readStartMs = millis();
    esp_err_t err = ESP_OK;
    {
      // Unified pull — HAL_Audio dispatches PDM vs the G2 LC3→PCM ring by the
      // active source; short reads are fine (the downstream ring re-chunks into
      // 160-sample AFE frames). For G2, block up to 100 ms for the BLE pipe.
#if ENABLE_MICROPHONE
      size_t got = audioReadPcm((int16_t*)i2sReadBuf, i2sReadBytes / sizeof(int16_t), /*timeoutMs*/ 100);
      bytesRead = got * sizeof(int16_t);
#else
      bytesRead = 0;   // no mic subsystem compiled in this build
#endif
      if (audioGetSource() == AUDIO_SRC_G2_LEFT) {
        if (bytesRead > 0) gSrG2BytesOk += bytesRead;
        else               gSrG2ReadZero++;
      }
    }
    uint32_t readDurationMs = millis() - readStartMs;

    if (doDetailedLog) {
      WARN_SYSTEMF("[SR_LOOP] Loop %u: %s_read took %u ms, err=0x%x (%s), bytesRead=%u",
                   (unsigned)loopCount,
                   audioGetSource() == AUDIO_SRC_G2_LEFT ? "g2_mic" : "i2s",
                   (unsigned)readDurationMs, err, esp_err_to_name(err), (unsigned)bytesRead);
    }

    if (err != ESP_OK) {
      gSrI2SReadErr++;
      if (loopCount <= 10) {
        WARN_SYSTEMF("[SR_LOOP] I2S READ ERROR at loop %u: %s", (unsigned)loopCount, esp_err_to_name(err));
      }
      SR_DBG_L(3, "I2S read error: %s (loop=%u)", esp_err_to_name(err), (unsigned)loopCount);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (bytesRead == 0) {
      gSrI2SReadZero++;
      if (loopCount <= 10) {
        WARN_SYSTEMF("[SR_LOOP] %s READ ZERO BYTES at loop %u",
                     audioGetSource() == AUDIO_SRC_G2_LEFT ? "G2-MIC" : "I2S",
                     (unsigned)loopCount);
      }
      SR_DBG_L(3, "read zero bytes (loop=%u, source=%u)",
               (unsigned)loopCount, (unsigned)audioGetSource());
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    
    gSrI2SReadOk++;
    gSrI2SBytesOk += bytesRead;
    size_t samplesRead = bytesRead / sizeof(int16_t);

    if (samplesRead > 0) {
      int16_t mn = 32767;
      int16_t mx = -32768;
      int64_t sumAbs = 0;
      for (size_t i = 0; i < samplesRead; i++) {
        int16_t v = i2sReadBuf[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sumAbs += (v < 0) ? -(int32_t)v : (int32_t)v;
      }
      gSrLastPcmMin = mn;
      gSrLastPcmMax = mx;
      gSrLastPcmAbsAvg = (float)sumAbs / (float)samplesRead;
      
      if (doDetailedLog) {
        WARN_SYSTEMF("[SR_LOOP] Loop %u: PCM samples=%u, min=%d, max=%d, avg_abs=%.1f",
                     (unsigned)loopCount, (unsigned)samplesRead, mn, mx, gSrLastPcmAbsAvg);
        lastDetailedLogLoop = loopCount;
      }
    }
    
    SR_DBG_L(4, "I2S read: %u bytes, %u samples (loop=%u)", (unsigned)bytesRead, (unsigned)samplesRead, (unsigned)loopCount);

    if (gSrSnipEnabled && gSrSnipRing) {
      srSnipRingPush(i2sReadBuf, samplesRead);
    }
    if (gSrSnipSessionActive) {
      srSnipFeedSession(i2sReadBuf, samplesRead);
    }

    if (ringSamplesCap > 0 && samplesRead > 0) {
      const int16_t* src = i2sReadBuf;
      size_t srcSamples = samplesRead;
      if (srcSamples >= ringSamplesCap) {
        src = &i2sReadBuf[srcSamples - ringSamplesCap];
        srcSamples = ringSamplesCap;
        memcpy(ringBuf, src, srcSamples * sizeof(int16_t));
        ringHead = 0;
        ringCount = srcSamples;
      } else {
        size_t freeSpace = ringSamplesCap - ringCount;
        if (srcSamples > freeSpace) {
          size_t drop = srcSamples - freeSpace;
          ringHead = (ringHead + drop) % ringSamplesCap;
          ringCount -= drop;
        }
        size_t tail = (ringHead + ringCount) % ringSamplesCap;
        size_t first = srcSamples;
        if (tail + first > ringSamplesCap) {
          first = ringSamplesCap - tail;
        }
        memcpy(&ringBuf[tail], src, first * sizeof(int16_t));
        if (srcSamples > first) {
          memcpy(&ringBuf[0], &src[first], (srcSamples - first) * sizeof(int16_t));
        }
        ringCount += srcSamples;
      }
    }

    if (gAFE && gAFEData) {
      while (ringCount >= feedChunkSamples) {
        size_t first = feedChunkSamples;
        if (ringHead + first > ringSamplesCap) {
          first = ringSamplesCap - ringHead;
        }
        memcpy(afeFeedBuf, &ringBuf[ringHead], first * sizeof(int16_t));
        if (feedChunkSamples > first) {
          memcpy(&afeFeedBuf[first], &ringBuf[0], (feedChunkSamples - first) * sizeof(int16_t));
        }
        ringHead = (ringHead + feedChunkSamples) % ringSamplesCap;
        ringCount -= feedChunkSamples;

        // Apply shared audio preprocessing (DC offset, optional filters, gain)
        applyMicAudioProcessing(afeFeedBuf, feedChunkSamples, getMicSoftwareGainMultiplier(), gSrFiltersEnabled);

        gAFE->feed(gAFEData, afeFeedBuf);
        gSrAfeFeedOk++;
        fedSinceFetchSamples += feedChunkSamples;
        
        if (gSrAfeFeedOk <= 5 || (gSrAfeFeedOk % 500 == 0)) {
          int16_t feedMin = 32767, feedMax = -32768;
          for (size_t i = 0; i < feedChunkSamples; i++) {
            if (afeFeedBuf[i] < feedMin) feedMin = afeFeedBuf[i];
            if (afeFeedBuf[i] > feedMax) feedMax = afeFeedBuf[i];
          }
          float swg = getMicSoftwareGainMultiplier();
          WARN_SYSTEMF("[SR_AFE] Feed #%u: min=%d, max=%d, dc=%d, swgain=%.1f, micgain=%d",
                       (unsigned)gSrAfeFeedOk, feedMin, feedMax, (int)getMicDcOffset(), swg, gSettings.microphoneGain);
        }
      }

      while (fedSinceFetchSamples >= (size_t)afeFetchChunk) {
        fedSinceFetchSamples -= (size_t)afeFetchChunk;
        
        afe_fetch_result_t* fetchResult = gAFE->fetch(gAFEData);
        
        if (gSrAfeFetchOk < 10 || (gSrAfeFetchOk % 100 == 0)) {
          WARN_SYSTEMF("[SR_AFE] Fetch #%u: result_ptr=%p", (unsigned)(gSrAfeFetchOk + 1), fetchResult);
        }
        
        if (!fetchResult) {
          if (gSrAfeFetchOk < 5) {
            WARN_SYSTEMF("[SR_AFE] Fetch #%u returned NULL", (unsigned)(gSrAfeFetchOk + 1));
          }
          continue;
        }

        gSrAfeFetchOk++;
        gSrLastAfeRetValue = fetchResult->ret_value;
        
        if (gSrAfeFetchOk <= 10) {
          WARN_SYSTEMF("[SR_AFE] Fetch #%u: ret=%d, vol=%.1f dB, vad=%d, wake=%d, data=%p",
                       (unsigned)gSrAfeFetchOk, fetchResult->ret_value, fetchResult->data_volume,
                       (int)fetchResult->vad_state, (int)fetchResult->wakeup_state, fetchResult->data);
        }
        
        if (fetchResult->ret_value == ESP_FAIL) {
          if (gSrAfeFetchOk <= 10) {
            WARN_SYSTEMF("[SR_AFE] Fetch #%u: ret_value=ESP_FAIL, skipping", (unsigned)gSrAfeFetchOk);
          }
          continue;
        }

        gSrLastVolumeDb = fetchResult->data_volume;
        gSrLastVadState = (int)fetchResult->vad_state;
        gSrLastAfeTriggerChannel = fetchResult->trigger_channel_id;

        SR_DBG_L(4, "AFE fetch: vol=%.1f dB, vad=%d, wake_state=%d, ret=%d",
                 fetchResult->data_volume, (int)fetchResult->vad_state,
                 (int)fetchResult->wakeup_state, fetchResult->ret_value);

        if (fetchResult->wakeup_state == WAKENET_DETECTED) {
          gWakeWordCount++;
          gLastWakeMs = millis();
          gESPSRWakeDetected = true;
          listeningForCommand = true;
          commandSpeechStarted = false;  // Reset - waiting for user to start speaking command
          commandTimeoutMs = millis() + gSettings.srCommandTimeout;
          gSrLastWakeWordIndex = fetchResult->wake_word_index;
          gSrLastWakeNetModelIndex = fetchResult->wakenet_model_index;
          
          // Transition to hierarchical state machine
          INFO_SRF("[HIER-DEBUG] State transition: %s -> AWAIT_CATEGORY", voiceStateToString(gVoiceState));
          gVoiceState = VoiceState::AWAIT_CATEGORY;
          gCurrentCategory = "";
          
          INFO_SRF("[HIER] ============================================");
          INFO_SRF("[HIER] WAKE WORD DETECTED!");
          INFO_SRF("[HIER] ============================================");
          INFO_SRF("[HIER] Listening for CATEGORY... (timeout in %d ms)", gSettings.srCommandTimeout);
          
          // User-facing feedback
          broadcastOutput("");
          broadcastOutput("[Voice] Yes?");
          systemEventPost(SYSEVT_VOICE_WAKE);
          INFO_SRF("[HIER-DEBUG] Voice CLI mappings count: %u", (unsigned)gVoiceCliMappingCount);
          INFO_SRF("Wake stats: count=%u, idx=%d, model=%d, vol=%.1f dB, wake_len=%d",
                   gWakeWordCount, fetchResult->wake_word_index, fetchResult->wakenet_model_index,
                   fetchResult->data_volume, fetchResult->wake_word_length);
          
          if (gSrSnipEnabled && !gSrSnipSessionActive) {
            srSnipStartSession("wake", -1, nullptr);
          }
          
          if (gWakeWordCallback) {
            gWakeWordCallback("hey_device");
          }
        }
        
        if (listeningForCommand && gMNModel && gMNData) {
          // Extend timeout when user starts speaking their command
          if (!commandSpeechStarted && fetchResult->vad_state == AFE_VAD_SPEECH) {
            commandSpeechStarted = true;
            commandTimeoutMs = millis() + gSettings.srCommandTimeout;  // Fresh timeout from speech start
            SR_DBG_L(1, "Speech detected - timeout extended to %d ms from now", gSettings.srCommandTimeout);
          }
          
          if (millis() > commandTimeoutMs) {
            INFO_SRF("[HIER-DEBUG] ===== TIMEOUT TRIGGERED =====");
            INFO_SRF("[HIER-DEBUG] Current state: %s", voiceStateToString(gVoiceState));
            INFO_SRF("[HIER-DEBUG] Current category: '%s'", gCurrentCategory.c_str());
            INFO_SRF("[HIER-DEBUG] Time since wake: %u ms", (unsigned)(millis() - gLastWakeMs));
            
            gMNModel->clean(gMNData);
            
            // Handle timeout based on current state
            if (gVoiceState == VoiceState::AWAIT_CATEGORY) {
              INFO_SRF("[HIER] ============================================");
              INFO_SRF("[HIER] TIMEOUT: No category detected");
              INFO_SRF("[HIER] ============================================");
              INFO_SRF("[HIER-DEBUG] State transition: AWAIT_CATEGORY -> IDLE");
              
              // User-facing feedback
              broadcastOutput("[Voice] Sorry, I didn't catch that.");
              
              gVoiceState = VoiceState::IDLE;
              gCurrentCategory = "";
              gCurrentSubCategory = "";
            } else if (gVoiceState == VoiceState::AWAIT_SUBCATEGORY) {
              INFO_SRF("[HIER] ============================================");
              INFO_SRF("[HIER] TIMEOUT: No subcategory detected for '%s'", gCurrentCategory.c_str());
              INFO_SRF("[HIER] ============================================");
              INFO_SRF("[HIER-DEBUG] State transition: AWAIT_SUBCATEGORY -> IDLE");
              
              // User-facing feedback
              broadcastOutput(String("[Voice] Timed out waiting for ") + gCurrentCategory + " selection.");
              
              gVoiceState = VoiceState::IDLE;
              gCurrentCategory = "";
              gCurrentSubCategory = "";
              // Reload categories for next wake word
              INFO_SRF("[HIER-DEBUG] Reloading categories after subcategory timeout...");
              loadCategories();
            } else if (gVoiceState == VoiceState::AWAIT_TARGET) {
              INFO_SRF("[HIER] ============================================");
              INFO_SRF("[HIER] TIMEOUT: No target detected for '%s'->'%s'", 
                       gCurrentCategory.c_str(), gCurrentSubCategory.c_str());
              INFO_SRF("[HIER] ============================================");
              INFO_SRF("[HIER-DEBUG] State transition: AWAIT_TARGET -> IDLE");
              
              // User-facing feedback
              if (gCurrentSubCategory.length() > 0) {
                broadcastOutput(String("[Voice] Timed out waiting for ") + gCurrentSubCategory + " action.");
              } else {
                broadcastOutput(String("[Voice] Timed out waiting for ") + gCurrentCategory + " action.");
              }
              
              gVoiceState = VoiceState::IDLE;
              gCurrentCategory = "";
              gCurrentSubCategory = "";
              // Reload categories for next wake word
              INFO_SRF("[HIER-DEBUG] Reloading categories after target timeout...");
              loadCategories();
            }
            
            listeningForCommand = false;
            gESPSRWakeDetected = false;
            if (gSrSnipSessionActive) {
              srSnipEndSession(true);
            }
          } else {
            if (!lockMN(50)) {
              continue;
            }
            bool mnLocked = true;
            gSrMnDetectCalls++;

            const bool isCategoryStageNow = (gVoiceState == VoiceState::AWAIT_CATEGORY);
            const bool isSubCategoryStageNow = (gVoiceState == VoiceState::AWAIT_SUBCATEGORY);
            const bool isTargetStageNow = (gVoiceState == VoiceState::AWAIT_TARGET);
            const bool speechOkNow = (!gSrTargetRequireSpeech) || isCategoryStageNow || isSubCategoryStageNow || commandSpeechStarted || (fetchResult->vad_state == AFE_VAD_SPEECH);
            if ((isTargetStageNow || isSubCategoryStageNow) && gSrTargetRequireSpeech && !speechOkNow) {
              if (mnLocked) {
                unlockMN();
              }
              continue;
            }

            const bool dynGainOkNow = (fetchResult->vad_state == AFE_VAD_SPEECH) || commandSpeechStarted;

            int16_t* mnInput = (int16_t*)fetchResult->data;
            size_t mnSamples = 0;
            if (fetchResult->data && fetchResult->data_size > 0) {
              mnSamples = (size_t)fetchResult->data_size / sizeof(int16_t);
            }
            if (gSrDynGainEnabled && dynGainOkNow && mnInputBuf && mnSamples > 0 && mnSamples <= mnBufSamplesCap) {
              int32_t peakAbs = 0;
              for (size_t i = 0; i < mnSamples; ++i) {
                int32_t v = (int32_t)mnInput[i];
                if (v < 0) v = -v;
                if (v > peakAbs) peakAbs = v;
              }
              if (peakAbs > 0) {
                float desired = gSrDynGainTargetPeak / (float)peakAbs;
                desired = clampFloat(desired, gSrDynGainMin, gSrDynGainMax);
                gSrDynGainCurrent = gSrDynGainCurrent + (desired - gSrDynGainCurrent) * gSrDynGainAlpha;
                gSrDynGainCurrent = clampFloat(gSrDynGainCurrent, gSrDynGainMin, gSrDynGainMax);
                for (size_t i = 0; i < mnSamples; ++i) {
                  int32_t s = (int32_t)((float)mnInput[i] * gSrDynGainCurrent);
                  mnInputBuf[i] = clampS16(s);
                }
                mnInput = mnInputBuf;
                gSrDynGainApplied++;
              } else {
                gSrDynGainBypassed++;
              }
            } else {
              gSrDynGainBypassed++;
            }

            esp_mn_state_t mnState = gMNModel->detect(gMNData, mnInput);
            
            SR_DBG_L(4, "MN detect: state=%d (DETECTING=0, DETECTED=1, TIMEOUT=2)", (int)mnState);
            
            // Log periodically during AWAIT_SUBCATEGORY or AWAIT_TARGET to show we're listening
            static uint32_t lastTargetListenLog = 0;
            if ((gVoiceState == VoiceState::AWAIT_SUBCATEGORY || gVoiceState == VoiceState::AWAIT_TARGET) && mnState == ESP_MN_STATE_DETECTING) {
              uint32_t now = millis();
              if (now - lastTargetListenLog > 1500) {
                lastTargetListenLog = now;
                const char* stageStr = (gVoiceState == VoiceState::AWAIT_SUBCATEGORY) ? "SUBCATEGORY" : "TARGET";
                INFO_SRF("[%s] Listening... vad=%d vol=%.1f dB",
                         stageStr, fetchResult->vad_state, 20.0f * log10f(fetchResult->data_volume + 1e-10f));
              }
            }
            
            if (mnState == ESP_MN_STATE_DETECTED) {
              gSrMnDetected++;
              esp_mn_results_t* results = gMNModel->get_results(gMNData);
              if (results && results->num > 0) {
                int cmdId = results->command_id[0];
                const char* cmdPhrase = results->string;
                float cmdProb = results->prob[0];
                char cmdPhraseCopy[128];
                cmdPhraseCopy[0] = '\0';
                if (cmdPhrase) {
                  strncpy(cmdPhraseCopy, cmdPhrase, sizeof(cmdPhraseCopy) - 1);
                  cmdPhraseCopy[sizeof(cmdPhraseCopy) - 1] = '\0';
                }
                
                const bool isCategoryStage = (gVoiceState == VoiceState::AWAIT_CATEGORY);
                const bool isSubCategoryStage = (gVoiceState == VoiceState::AWAIT_SUBCATEGORY);
                // Use category confidence for both category and subcategory stages
                const float requiredConfidence = (isCategoryStage || isSubCategoryStage) ? gSrMinCategoryConfidence : gSrMinCommandConfidence;
                const float cmdProb2 = (results->num > 1) ? results->prob[1] : 0.0f;
                const bool speechOk = (!gSrTargetRequireSpeech) || isCategoryStage || isSubCategoryStage || commandSpeechStarted || (fetchResult->vad_state == AFE_VAD_SPEECH);
                const bool acceptByGap = (!isCategoryStage && !isSubCategoryStage) && gSrGapAcceptEnabled && speechOk && (cmdProb >= gSrGapAcceptFloor) && ((cmdProb - cmdProb2) >= gSrGapAcceptGap);
                const bool accepted = (cmdProb >= requiredConfidence) || acceptByGap;
                INFO_SRF("=== VOICE COMMAND CANDIDATES ===");
                INFO_SRF("  #1: id=%d '%s' prob=%.1f%% %s", 
                         cmdId, cmdPhrase ? cmdPhrase : "?", cmdProb * 100.0f,
                         accepted ? "<-- SELECTED" : "<-- REJECTED");
                
                // Always show all candidates so user can see what was considered
                for (int i = 1; i < results->num && i < ESP_MN_RESULT_MAX_NUM; ++i) {
                  // Look up the phrase for this command ID
                  char* altPhraseStr = esp_mn_commands_get_string(results->command_id[i]);
                  INFO_SRF("  #%d: id=%d '%s' prob=%.1f%%", 
                           i + 1, results->command_id[i], altPhraseStr, results->prob[i] * 100.0f);
                }
                INFO_SRF("================================");
                
                if (!accepted) {
                  gSrLowConfidenceRejects++;
                  WARN_SRF("Rejected command: id=%d prob=%.3f (need>=%.2f or gap floor=%.2f gap=%.2f) (rejects=%lu)",
                               cmdId, cmdProb, requiredConfidence, gSrGapAcceptFloor, gSrGapAcceptGap, (unsigned long)gSrLowConfidenceRejects);
                  if (isCategoryStage) {
                    broadcastOutput(String("[Voice] I heard '") + normalizePhrase(cmdPhraseCopy) + "'... can you say it again?");
                  } else {
                    broadcastOutput("[Voice] Sorry, can you repeat that?");
                  }

                  gMNModel->clean(gMNData);
                  commandTimeoutMs = millis() + gSettings.srCommandTimeout;
                  if (gSrSnipSessionActive) {
                    srSnipEndSession(true);
                  }
                } else {
                  if (acceptByGap && cmdProb < requiredConfidence) {
                    gSrGapAccepts++;
                  }
                  gCommandCount++;
                  gLastCommand = cmdPhrase ? cmdPhrase : String(cmdId);
                  gLastConfidence = cmdProb;
                  
                  if (gSrSnipSessionActive) {
                    gSrSnipSessionCmdId = cmdId;
                    strncpy(gSrSnipSessionPhrase, cmdPhrase ? cmdPhrase : "", sizeof(gSrSnipSessionPhrase) - 1);
                    srSnipEndSession(true);
                  }
                  // Reset MultiNet state to prevent stale detection on next wake
                  gMNModel->clean(gMNData);

                  if (gCommandCallback) {
                    unlockMN();
                    mnLocked = false;
                    gCommandCallback(cmdId, cmdPhraseCopy[0] ? cmdPhraseCopy : nullptr);

                    // Continue listening if we're in a multi-stage state
                    if (gVoiceState == VoiceState::AWAIT_SUBCATEGORY || gVoiceState == VoiceState::AWAIT_TARGET) {
                      listeningForCommand = true;
                      gESPSRWakeDetected = true;
                      commandSpeechStarted = false;
                      commandTimeoutMs = millis() + gSettings.srCommandTimeout;
                    } else {
                      listeningForCommand = false;
                      gESPSRWakeDetected = false;
                    }
                  } else {
                    listeningForCommand = false;
                    gESPSRWakeDetected = false;
                  }
                }
              }
            } else if (mnState == ESP_MN_STATE_TIMEOUT) {
              SR_DBG_L(1, "MN state timeout");
              gMNModel->clean(gMNData);
              
              // Reset hierarchical state on MN timeout
              if (gVoiceState == VoiceState::AWAIT_SUBCATEGORY || gVoiceState == VoiceState::AWAIT_TARGET) {
                INFO_SRF("[HIER] MN timeout in %s stage - returning to idle", 
                         gVoiceState == VoiceState::AWAIT_SUBCATEGORY ? "subcategory" : "target");
                // Release lock before loadCategories to avoid deadlock
                unlockMN();
                mnLocked = false;
                loadCategories();
              }
              gVoiceState = VoiceState::IDLE;
              gCurrentCategory = "";
              gCurrentSubCategory = "";
              
              listeningForCommand = false;
              gESPSRWakeDetected = false;
              if (gSrSnipSessionActive) {
                srSnipEndSession(true);
              }
            }
            if (mnLocked) {
              unlockMN();
            }
          }
        }
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  
  if (gSrSnipSessionActive) {
    srSnipEndSession(false);
  }
  
  free(i2sReadBuf);
  free(afeFeedBuf);
  free(ringBuf);
  free(mnInputBuf);
  INFO_SRF("SR task stopped (loops=%u)", (unsigned)loopCount);
  vTaskDelete(nullptr);
}

// ============================================================================
// Public API
// ============================================================================

void initESPSR() {
  if (gESPSRInitialized) return;
  
  INFO_SRF("Initializing ESP-SR...");
  
  // Create ESP-SR Models folder for custom model storage
  // Try SD card first, fall back to LittleFS if SD not available
  bool folderCreated = false;
  
  if (VFS::isSDAvailable()) {
    if (VFS::mkdirGuarded("/sd/ESPSR", VFS::systemAuth("espsr.init_mkdir"))) {
      INFO_SRF("Created /sd/ESPSR folder on SD card");
      folderCreated = true;
    } else if (VFS::existsGuarded("/sd/ESPSR", VFS::systemAuth("espsr.init_mkdir"))) {
      DEBUG_SRF("/sd/ESPSR already exists");
      folderCreated = true;
    }
  }

  if (!folderCreated) {
    if (VFS::mkdirGuarded("/ESPSR", VFS::systemAuth("espsr.init_mkdir"))) {
      INFO_SRF("Created /ESPSR folder on LittleFS");
    } else if (VFS::existsGuarded("/ESPSR", VFS::systemAuth("espsr.init_mkdir"))) {
      DEBUG_SRF("/ESPSR already exists on LittleFS");
    }
  }
  
  gESPSRInitialized = true;
}

bool startESPSR() {
  WARN_SYSTEMF("[SR_START] ########## startESPSR() BEGIN ##########");
  WARN_SYSTEMF("[SR_START] Heap: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  
  initESPSR();
  if (gESPSRRunning) {
    WARN_SYSTEMF("[SR_START] Already running, returning true");
    return true;
  }
  
  INFO_SRF("Starting ESP-SR pipeline...");

#if ENABLE_MICROPHONE
  WARN_SYSTEMF("[SR_START] Checking microphone sensor: gMicRunning=%d recState=%s",
               gMicRunning ? 1 : 0,
               micRecordingStateName(getMicRecordingState()));
  if (gMicRunning || micRecordingBusy()) {
    // Restore only a source that was actually enabled. A source-loss callback
    // can leave gMicRunning=false while its WAV is still FINALIZING; that case
    // must still be joined before SR claims HAL_Audio, but should not be
    // resurrected after SR exits.
    gRestoreMicAfterSR = gMicRunning;
    INFO_SRF("Microphone sensor/recorder is busy; draining it to start SR");
    if (!stopMicrophone()) {
      ERROR_SRF("Microphone recorder did not reach IDLE; refusing SR start");
      gRestoreMicAfterSR = false;
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
#endif
  
  // Initialize components in order
  WARN_SYSTEMF("[SR_START] Step 1: initI2SMicrophone()");
  if (!initI2SMicrophone()) {
    ERROR_SRF("Failed to init I2S microphone");
    restoreMicrophoneAfterSRIfNeeded();
    return false;
  }
  WARN_SYSTEMF("[SR_START] Step 1 COMPLETE");
  
  WARN_SYSTEMF("[SR_START] Step 2: initAFE()");
  if (!initAFE()) {
    ERROR_SRF("Failed to init AFE");
    deinitI2SMicrophone();
    restoreMicrophoneAfterSRIfNeeded();
    return false;
  }
  WARN_SYSTEMF("[SR_START] Step 2 COMPLETE");
  
  WARN_SYSTEMF("[SR_START] Step 3: initMultiNet()");
  if (!initMultiNet()) {
    WARN_SRF("MultiNet init failed, continuing without command recognition");
  }
  WARN_SYSTEMF("[SR_START] Step 3 COMPLETE");
  
  // Start SR task
  WARN_SYSTEMF("[SR_START] Step 4: Creating srTask (stack=%u, priority=%d, core=1)",
               (unsigned)SR_STACK_WORDS, (int)SR_TASK_PRIORITY_LEVEL);
  gSRTaskShouldRun = true;
  taskStackRecord("sr_task", SR_STACK_WORDS);
  BaseType_t ret = xTaskCreatePinnedToCore(
    srTask,
    "sr_task",
    SR_STACK_WORDS,
    nullptr,
    SR_TASK_PRIORITY_LEVEL,
    &gSRTaskHandle,
    1  // Run on core 1
  );
  WARN_SYSTEMF("[SR_START] xTaskCreatePinnedToCore returned: %d (pdPASS=%d)", (int)ret, (int)pdPASS);
  
  if (ret != pdPASS) {
    ERROR_SRF("Failed to create SR task");
    deinitMultiNet();
    deinitAFE();
    deinitI2SMicrophone();
    restoreMicrophoneAfterSRIfNeeded();
    return false;
  }
  
  gESPSRRunning = true;
  
  // Reset audio preprocessing state (shared with microphone module)
  resetMicAudioProcessingState();
  
  // Auto-sync voice commands from registry (hierarchical categories)
  INFO_SRF("[SR_START] ========================================");
  INFO_SRF("[SR_START] Step 5: Auto-syncing voice commands");
  INFO_SRF("[SR_START] ========================================");
  INFO_SRF("[HIER-DEBUG] Initializing hierarchical voice state machine...");
  INFO_SRF("[HIER-DEBUG] Setting state to IDLE");
  gVoiceState = VoiceState::IDLE;
  gCurrentCategory = "";
  INFO_SRF("[HIER-DEBUG] Calling loadCategories()...");
  loadCategories();
  INFO_SRF("[HIER-DEBUG] Registering onVoiceCommandDetected callback...");
  setESPSRCommandCallback(onVoiceCommandDetected);
  INFO_SRF("[SR_START] Step 5 COMPLETE - Voice commands auto-synced");
  INFO_SRF("[HIER-DEBUG] Initial state: %s, mappings: %u", voiceStateToString(gVoiceState), (unsigned)gVoiceCliMappingCount);
  
  WARN_SYSTEMF("[SR_START] ########## startESPSR() SUCCESS ##########");
  INFO_SRF("ESP-SR pipeline started successfully");
  return true;
}

void stopESPSR() {
  if (!gESPSRRunning) return;
  
  INFO_SRF("Stopping ESP-SR pipeline...");
  
  // Stop task
  gSRTaskShouldRun = false;
  if (gSRTaskHandle) {
    vTaskDelay(pdMS_TO_TICKS(200));  // Wait for task to stop
    gSRTaskHandle = nullptr;
  }
  
  // Deinit components in reverse order
  deinitMultiNet();
  deinitAFE();
  deinitI2SMicrophone();

  // Clear the resolved model names so the web status card stops showing
  // them once SR has been torn down.
  gWnModelName[0] = '\0';
  gMnModelName[0] = '\0';

  restoreMicrophoneAfterSRIfNeeded();

  gESPSRRunning = false;
  gESPSRWakeDetected = false;
  INFO_SRF("ESP-SR pipeline stopped");
}

bool isESPSRRunning() {
  return gESPSRRunning;
}

bool isESPSRWakeActive() {
  return gESPSRWakeDetected;
}

void setESPSRWakeCallback(void (*callback)(const char*)) {
  gWakeWordCallback = callback;
}

void setESPSRCommandCallback(void (*callback)(int, const char*)) {
  gCommandCallback = callback;
}

void buildESPSRStatusJson(String& output) {
  PSRAM_JSON_DOC(doc);
  doc["enabled"] = true;
  doc["initialized"] = gESPSRInitialized;
  doc["running"] = gESPSRRunning;
  doc["wakeActive"] = gESPSRWakeDetected;
  doc["state"] = getESPSRVoiceState();
  doc["category"] = gCurrentCategory;
  doc["subcategory"] = gCurrentSubCategory;
  doc["wakeCount"] = gWakeWordCount;
  doc["commandCount"] = gCommandCount;
  doc["lastWakeMs"] = gLastWakeMs;
  doc["lastCommand"] = gLastCommand;
  doc["lastConfidence"] = gLastConfidence;
  doc["lowConfidenceRejects"] = gSrLowConfidenceRejects;
  doc["hasAFE"] = (gAFE != nullptr);
  doc["hasMultiNet"] = (gMNModel != nullptr);
  // Concrete model names so the web status card can show what's actually
  // loaded (e.g. "wn9_hilexin", "mn7_en") rather than just a green check.
  // Empty strings when not loaded.
  doc["wnModelName"] = gWnModelName;
  doc["mnModelName"] = gMnModelName;
  doc["voiceCliMappings"] = (int)gVoiceCliMappingCount;
  doc["voiceArmed"] = gVoiceArmed;
  doc["voiceArmedUser"] = gVoiceArmedUser;
  doc["voiceArmedBy"] = transportToStableString(gVoiceArmedByTransport);
  doc["rawOutput"] = gSrRawOutputEnabled;
  doc["autotuneActive"] = gSrAutoTuneActive;
  doc["autotuneStep"] = gSrAutoTuneStep;
  doc["volumeDb"] = gSrLastVolumeDb;
  doc["vadState"] = gSrLastVadState;
  doc["micgain"] = gSettings.microphoneGain;
  // Stack: FreeRTOS high-water mark = minimum *unused* stack left (in words).
  // Peak depth estimate ≈ allocWords - hwmWords (upper bound since last reset).
  doc["srTaskStackAllocWords"] = SR_STACK_WORDS;
  doc["srTaskStackAllocBytes"] = (uint32_t)SR_STACK_WORDS * 4u;
  if (gESPSRRunning && gSRTaskHandle) {
    const UBaseType_t hwm = uxTaskGetStackHighWaterMark(gSRTaskHandle);
    doc["srTaskStackHwmWords"] = (uint32_t)hwm;
    doc["srTaskStackHwmBytes"] = (uint32_t)hwm * 4u;
    const uint32_t usedWords =
        (SR_STACK_WORDS > hwm) ? (uint32_t)(SR_STACK_WORDS - hwm) : 0u;
    doc["srTaskStackPeakUsedBytesEst"] = usedWords * 4u;
  } else {
    doc["srTaskStackHwmWords"] = 0;
    doc["srTaskStackHwmBytes"] = 0;
    doc["srTaskStackPeakUsedBytesEst"] = 0;
    doc["srTaskStackHwmNote"] =
        "Run srstart; HWM resets when task is created. Sample after stress.";
  }
  serializeJson(doc, output);
}

static const char* setEnabledFromArgs(const String& argsInput) {
  (void)argsInput;
  return "Error: ENABLE_ESP_SR is a compile-time flag";
}

const char* cmd_sr(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  return "Error: invalid arguments — Usage: sr <enable|start|stop|status|stack|cmds|debug|confidence|timeout|tuning|accept|dyngain|raw|autotune|snip>";
}

const char* cmd_sr_enable(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return setEnabledFromArgs(argsInput);
}

const char* cmd_sr_start(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (!gSettings.srEnabled) {
    return "ERROR: Speech recognition is disabled - run 'srenabled 1' first";
  }
  bool ok = startESPSR();
  if (!ok) return "Error: failed to start";

  ensureVoiceArmMutex();
  bool armed = false;
  if (gVoiceArmMutex && xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    armed = voiceArmFromContextInternal(currentAuthContext());
    xSemaphoreGive(gVoiceArmMutex);
  } else {
    armed = voiceArmFromContextInternal(currentAuthContext());
  }

  if (armed) {
    char msg[192];
    snprintf(msg, sizeof(msg), "[VOICE] Armed as '%s' (by %s)", gVoiceArmedUser.c_str(), transportToStableString(gVoiceArmedByTransport));
    broadcastOutput(msg);
  }

  static String out;
  out = "OK";
  if (armed) {
    out += " (voice armed as '";
    out += gVoiceArmedUser;
    out += "')";
  } else {
    out += " (voice NOT armed)";
    // SR is listening but recognized commands will be rejected until voice
    // is armed to an authenticated user. Arming needs a real transport/user
    // (not the internal console), so name the explicit arm command.
    cliHint("recognized commands are rejected until armed - run 'voicearm'");
  }
  return out.c_str();
}

const char* cmd_sr_stop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  stopESPSR();

  ensureVoiceArmMutex();
  if (gVoiceArmMutex && xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    voiceDisarmInternal();
    xSemaphoreGive(gVoiceArmMutex);
  } else {
    voiceDisarmInternal();
  }
  broadcastOutput("[VOICE] Disarmed (sr stopped)");
  return "OK";
}

static const char* cmd_voice_arm_cli(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  ensureVoiceArmMutex();
  bool armed = false;
  if (gVoiceArmMutex && xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    armed = voiceArmFromContextInternal(currentAuthContext());
    xSemaphoreGive(gVoiceArmMutex);
  } else {
    armed = voiceArmFromContextInternal(currentAuthContext());
  }
  if (!armed) return "Error: cannot arm voice from this transport/user";
  static String out;
  out = "OK: voice armed as '";
  out += gVoiceArmedUser;
  out += "'";
  return out.c_str();
}

static const char* cmd_voice_disarm_cli(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  ensureVoiceArmMutex();
  if (gVoiceArmMutex && xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    voiceDisarmInternal();
    xSemaphoreGive(gVoiceArmMutex);
  } else {
    voiceDisarmInternal();
  }
  broadcastOutput("[VOICE] Disarmed");
  return "OK";
}

static const char* cmd_voice_status_cli(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  ensureVoiceArmMutex();
  static String out;
  out = "";
  if (gVoiceArmMutex && xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    if (!gVoiceArmed) {
      out = "voice: disarmed";
    } else {
      out = "voice: armed user='";
      out += gVoiceArmedUser;
      out += "' by=";
      out += transportToStableString(gVoiceArmedByTransport);
    }
    xSemaphoreGive(gVoiceArmMutex);
  } else {
    if (gVoiceArmed) {
      char buf[96];
      snprintf(buf, sizeof(buf), "voice: armed user='%s' by=%s", gVoiceArmedUser.c_str(), transportToStableString(gVoiceArmedByTransport));
      out = buf;
    } else {
      out = "voice: disarmed";
    }
  }
  return out.c_str();
}

const char* cmd_sr_status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  static String out;
  out = "";
  buildESPSRStatusJson(out);
  return out.c_str();
}

// Stack watermark for sr_task — run after wake/commands/G2 mic stress; see cmd_sr().
static const char* cmd_sr_stack(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  static char buf[224];
  if (!gSRTaskHandle || !gESPSRRunning) {
    return "Error: sr_task: not running — use srstart first";
  }
  const UBaseType_t hwm = uxTaskGetStackHighWaterMark(gSRTaskHandle);
  const uint32_t allocB = (uint32_t)SR_STACK_WORDS * 4u;
  const uint32_t freeB = (uint32_t)hwm * 4u;
  const uint32_t usedEst =
      (SR_STACK_WORDS > hwm) ? (uint32_t)(SR_STACK_WORDS - hwm) * 4u : 0u;
  snprintf(buf, sizeof(buf),
           "sr_task: alloc=%uB est_peak_used=%uB hwm_free=%uB (%u words free). "
           "Shrink SR_STACK_WORDS in System_TaskUtils.h only if hwm_free stays "
           ">= ~25%% of alloc after your worst-case voice test.",
           (unsigned)allocB, (unsigned)usedEst, (unsigned)freeB, (unsigned)hwm);
  return buf;
}

// Voice control commands - these are handled specially in onVoiceCommandDetected
// but registered here for consistency with the command registry system
static const char* cmd_voice_cancel(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  // This is handled in onVoiceCommandDetected, not via CLI
  return "Voice cancel - resets voice state to idle";
}

static const char* cmd_voice_help(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  // This is handled in onVoiceCommandDetected, not via CLI
  return "Voice help - shows available options for current state";
}

static const char* cmd_sr_cmds(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  return "Error: invalid arguments — Usage: sr cmds <list|add|del|clear|save|reload>";
}

static const char* cmd_sr_cmds_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  static String out;
  out = "";

  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: srstart";
  }

  if (!lockMN(2000)) {
    return "Error: busy";
  }

  for (int i = 0; ; ++i) {
    esp_mn_phrase_t* phrase = esp_mn_commands_get_from_index(i);
    if (!phrase) break;
    if (!phrase->string) continue;
    out += String((int)phrase->command_id);
    out += ": ";
    out += phrase->string;
    // Show CLI command mapping if available
    const char* cliCmd = findCliCommandForId(phrase->command_id);
    if (cliCmd) {
      out += " -> ";
      out += cliCmd;
    }
    out += "\n";
  }

  if (out.length() == 0) {
    out = "(no commands)";
  }

  unlockMN();
  return out.c_str();
}

static const char* cmd_sr_cmds_add(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: sr cmds add <id> <phrase>";
  String idStr = a.arg(0);
  String phrase = a.remaining(0);
  if (!isAllDigits(idStr) || phrase.length() == 0) return "Error: invalid arguments — Usage: sr cmds add <id> <phrase>";
  int id = a.argInt(0, 0);
  if (id <= 0) return "Error: id must be > 0";

  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: srstart";
  }
  if (!lockMN(4000)) {
    return "Error: busy";
  }

  esp_err_t err = esp_mn_commands_add(id, phrase.c_str());
  esp_mn_error_t* errList = nullptr;
  if (err == ESP_OK) {
    errList = mnUpdateLocked();
  }
  unlockMN();

  if (err != ESP_OK) {
    return "Error: failed to add command";
  }
  if (errList && errList->num > 0) {
    return "Error: MultiNet rejected one or more commands";
  }
  return "OK";
}

static const char* cmd_sr_cmds_del(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) return "Error: invalid arguments — Usage: sr cmds del <phrase|id>";

  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: srstart";
  }
  if (!lockMN(4000)) {
    return "Error: busy";
  }

  const char* phrase = nullptr;
  String tmp;
  if (isAllDigits(arg)) {
    int id = arg.toInt();
    char* s = esp_mn_commands_get_string(id);
    if (s) {
      tmp = String(s);
      phrase = tmp.c_str();
    }
  } else {
    phrase = arg.c_str();
  }

  esp_err_t err = ESP_ERR_INVALID_STATE;
  esp_mn_error_t* errList = nullptr;
  if (phrase && strlen(phrase) > 0) {
    err = esp_mn_commands_remove(phrase);
    if (err == ESP_OK) {
      errList = mnUpdateLocked();
    }
  }
  unlockMN();

  if (err != ESP_OK) {
    return "Error: command not found";
  }
  if (errList && errList->num > 0) {
    return "Error: MultiNet rejected one or more commands";
  }
  return "OK";
}

static const char* cmd_sr_cmds_clear(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  if (arg != "confirm") return "Error: invalid arguments — Usage: sr cmds clear confirm";

  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: srstart";
  }
  if (!lockMN(4000)) {
    return "Error: busy";
  }

  esp_err_t err = esp_mn_commands_clear();
  esp_mn_error_t* errList = nullptr;
  if (err == ESP_OK) {
    errList = mnUpdateLocked();
  }
  unlockMN();

  if (err != ESP_OK) return "Error: failed";
  if (errList && errList->num > 0) return "Error: MultiNet rejected one or more commands";
  return "OK";
}

static const char* cmd_sr_cmds_reload(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: srstart";
  }
  if (!lockMN(6000)) {
    return "Error: busy";
  }

  esp_mn_commands_clear();
  size_t added = 0;
  size_t parseErrors = 0;
  bool ok = loadCommandsFileLocked(added, parseErrors);
  esp_mn_error_t* errList = mnUpdateLocked();
  unlockMN();

  if (!ok) return "Error: failed to read commands file (is SD mounted?)";
  if ((errList && errList->num > 0) || parseErrors > 0) {
    return "Error: some commands could not be loaded";
  }
  static char buf[96];
  snprintf(buf, sizeof(buf), "OK (loaded %u)", (unsigned)added);
  return buf;
}

static const char* cmd_sr_cmds_save(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: srstart";
  }
  if (!lockMN(6000)) {
    return "Error: busy";
  }
  size_t saved = 0;
  bool ok = saveCommandsFileLocked(saved);
  unlockMN();
  if (!ok) return "Error: failed to write commands file (is SD mounted?)";
  static char buf[96];
  snprintf(buf, sizeof(buf), "OK (saved %u)", (unsigned)saved);
  return buf;
}

static const char* cmd_sr_cmds_sync(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: srstart";
  }
  
  // Reset hierarchical state machine
  gVoiceState = VoiceState::IDLE;
  gCurrentCategory = "";
  
  // Load categories using the hierarchical helper
  loadCategories();
  
  // Register the command callback to execute CLI commands
  setESPSRCommandCallback(onVoiceCommandDetected);
  
  // Count how many categories were loaded
  size_t added = 0;
  if (lockMN(2000)) {
    for (int i = 0; ; i++) {
      esp_mn_phrase_t* phrase = esp_mn_commands_get_from_index(i);
      if (!phrase) break;
      added++;
    }
    unlockMN();
  }
  
  static char buf[128];
  snprintf(buf, sizeof(buf), "OK (synced %u voice categories from registry)", (unsigned)added);
  INFO_SRF("[HIER] Voice command sync complete - %u categories loaded", (unsigned)added);
  return buf;
}

// Phase 2B: switch the SR feed loop's audio source between the local
// PDM mic (default, via I2S) and the G2 left-temple mic (via the
// BLE-driven LC3 ring buffer). Switching to G2 also arms the ring +
// decoder; switching back disarms it. Caller is responsible for
// having a hijack page active and `g2micon` started — without those
// the firmware sends no audio and the SR loop will see "read zero"
// every iteration.
// (cmd_setmicsource removed — the mic source is unified device-wide. Use the
// `micsource [auto|pdm|g2]` command (System_Microphone), which sets the single
// preference that BOTH the mic sensor and this SR feed loop honor. The G2 ring
// depth / overrun telemetry that lived here is available via `srstats`.)

static const char* cmd_sr_debug(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  return "Error: invalid arguments — Usage: sr debug <level|telem|stats|reset>";
}

static const char* cmd_sr_debug_level(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "Current debug level: %u (0=off, 1-4=verbose)", gSrDebugLevel);
    return buf;
  }
  int lvl = arg.toInt();
  if (lvl < 0) lvl = 0;
  if (lvl > 4) lvl = 4;
  gSrDebugLevel = (uint8_t)lvl;
  static char buf[64];
  snprintf(buf, sizeof(buf), "Debug level set to %u", gSrDebugLevel);
  return buf;
}

static const char* cmd_sr_debug_telem(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    static char buf[96];
    snprintf(buf, sizeof(buf), "Telemetry period: %u ms (0=off)", (unsigned)gSrTelemetryPeriodMs);
    return buf;
  }
  int ms = arg.toInt();
  if (ms < 0) ms = 0;
  gSrTelemetryPeriodMs = (uint32_t)ms;
  gSrLastTelemetryMs = millis();
  static char buf[96];
  snprintf(buf, sizeof(buf), "Telemetry period set to %u ms", (unsigned)gSrTelemetryPeriodMs);
  return buf;
}

static const char* cmd_sr_debug_stats(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  srDebugPrintTelemetry();
  return "OK (stats printed to log)";
}

static const char* cmd_sr_debug_reset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  srDebugResetCounters();
  return "OK";
}

static const char* cmd_sr_confidence(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    static char buf[96];
    snprintf(buf, sizeof(buf),
             "Category confidence threshold: %.2f\nTarget confidence threshold: %.2f (rejects: %lu)\nUsage: sr confidence [<0.0-1.0> | category <0.0-1.0> | target <0.0-1.0>]",
             gSrMinCategoryConfidence, gSrMinCommandConfidence, (unsigned long)gSrLowConfidenceRejects);
    return buf;
  }

  String first = a.arg(0);
  bool setCategoryOnly = (first == "category");
  bool setTargetOnly = (first == "target");

  float val = 0.0f;
  if (setCategoryOnly || setTargetOnly) {
    if (!a.has(1)) return "Error: missing value";
    val = a.argFloat(1, 0.0f);
  } else {
    val = a.argFloat(0, 0.0f);
  }

  if (val < 0.0f || val > 1.0f) {
    return "Error: threshold must be 0.0-1.0";
  }

  if (setCategoryOnly) {
    gSrMinCategoryConfidence = val;
  } else if (setTargetOnly) {
    gSrMinCommandConfidence = val;
  } else {
    gSrMinCategoryConfidence = val;
    gSrMinCommandConfidence = val;
  }

  static char buf[96];
  snprintf(buf, sizeof(buf), "Confidence thresholds: category=%.2f target=%.2f", gSrMinCategoryConfidence, gSrMinCommandConfidence);
  return buf;
}

static const char* cmd_sr_accept(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    static String out;
    out = "Target acceptance:\n";
    out += "  gap_enabled=";
    out += gSrGapAcceptEnabled ? "1" : "0";
    out += "\n  floor=";
    out += String(gSrGapAcceptFloor, 2);
    out += "\n  gap=";
    out += String(gSrGapAcceptGap, 2);
    out += "\n  require_speech=";
    out += gSrTargetRequireSpeech ? "1" : "0";
    out += "\n  gap_accepts=";
    out += String((unsigned)gSrGapAccepts);
    out += "\nUsage: sr accept [on|off|floor <0.0-1.0>|gap <0.0-1.0>|speech <0|1>]";
    return out.c_str();
  }

  String key = a.arg(0);
  key.toLowerCase();
  String val = a.has(1) ? a.arg(1) : String();

  if (key == "on") {
    gSrGapAcceptEnabled = true;
    return "OK (gap accept enabled)";
  }
  if (key == "off") {
    gSrGapAcceptEnabled = false;
    return "OK (gap accept disabled)";
  }
  if (key == "floor") {
    if (val.length() == 0) return "Error: missing floor value";
    float f = val.toFloat();
    if (f < 0.0f || f > 1.0f) return "Error: floor must be 0.0-1.0";
    gSrGapAcceptFloor = f;
    static char buf[64];
    snprintf(buf, sizeof(buf), "OK (floor=%.2f)", gSrGapAcceptFloor);
    return buf;
  }
  if (key == "gap") {
    if (val.length() == 0) return "Error: missing gap value";
    float g = val.toFloat();
    if (g < 0.0f || g > 1.0f) return "Error: gap must be 0.0-1.0";
    gSrGapAcceptGap = g;
    static char buf[64];
    snprintf(buf, sizeof(buf), "OK (gap=%.2f)", gSrGapAcceptGap);
    return buf;
  }
  if (key == "speech" || key == "require_speech") {
    if (val.length() == 0) return "Error: missing speech value (0/1)";
    int v = val.toInt();
    gSrTargetRequireSpeech = (v != 0);
    return gSrTargetRequireSpeech ? "OK (require_speech=1)" : "OK (require_speech=0)";
  }

  return "Error: invalid arguments — Usage: sr accept [on|off|floor <0.0-1.0>|gap <0.0-1.0>|speech <0|1>]";
}

static const char* cmd_sr_dyngain(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    static String out;
    out = "Dynamic gain (MultiNet input only):\n";
    out += "  enabled=";
    out += gSrDynGainEnabled ? "1" : "0";
    out += "\n  current=";
    out += String(gSrDynGainCurrent, 2);
    out += "\n  min=";
    out += String(gSrDynGainMin, 2);
    out += "\n  max=";
    out += String(gSrDynGainMax, 2);
    out += "\n  target_peak=";
    out += String(gSrDynGainTargetPeak, 0);
    out += "\n  alpha=";
    out += String(gSrDynGainAlpha, 2);
    out += "\n  applied=";
    out += String((unsigned)gSrDynGainApplied);
    out += "\n  bypassed=";
    out += String((unsigned)gSrDynGainBypassed);
    out += "\nUsage: sr dyngain [on|off|min <0.1-10>|max <0.1-10>|target <1000-30000>|alpha <0.0-1.0>|reset]";
    return out.c_str();
  }

  String key = a.arg(0);
  key.toLowerCase();
  String val = a.has(1) ? a.arg(1) : String();

  if (key == "on") {
    gSrDynGainEnabled = true;
    return "OK (dyngain enabled)";
  }
  if (key == "off") {
    gSrDynGainEnabled = false;
    return "OK (dyngain disabled)";
  }
  if (key == "reset") {
    gSrDynGainCurrent = 1.0f;
    gSrDynGainApplied = 0;
    gSrDynGainBypassed = 0;
    return "OK";
  }
  if (key == "min") {
    if (val.length() == 0) return "Error: missing min value";
    float v = val.toFloat();
    if (v < 0.1f || v > 10.0f) return "Error: min must be 0.1-10";
    gSrDynGainMin = v;
    if (gSrDynGainMax < gSrDynGainMin) gSrDynGainMax = gSrDynGainMin;
    gSrDynGainCurrent = clampFloat(gSrDynGainCurrent, gSrDynGainMin, gSrDynGainMax);
    static char buf[64];
    snprintf(buf, sizeof(buf), "OK (min=%.2f)", gSrDynGainMin);
    return buf;
  }
  if (key == "max") {
    if (val.length() == 0) return "Error: missing max value";
    float v = val.toFloat();
    if (v < 0.1f || v > 10.0f) return "Error: max must be 0.1-10";
    gSrDynGainMax = v;
    if (gSrDynGainMin > gSrDynGainMax) gSrDynGainMin = gSrDynGainMax;
    gSrDynGainCurrent = clampFloat(gSrDynGainCurrent, gSrDynGainMin, gSrDynGainMax);
    static char buf[64];
    snprintf(buf, sizeof(buf), "OK (max=%.2f)", gSrDynGainMax);
    return buf;
  }
  if (key == "target") {
    if (val.length() == 0) return "Error: missing target value";
    float v = val.toFloat();
    if (v < 1000.0f || v > 30000.0f) return "Error: target must be 1000-30000";
    gSrDynGainTargetPeak = v;
    static char buf[72];
    snprintf(buf, sizeof(buf), "OK (target_peak=%.0f)", gSrDynGainTargetPeak);
    return buf;
  }
  if (key == "alpha") {
    if (val.length() == 0) return "Error: missing alpha value";
    float v = val.toFloat();
    if (v < 0.0f || v > 1.0f) return "Error: alpha must be 0.0-1.0";
    gSrDynGainAlpha = v;
    static char buf[64];
    snprintf(buf, sizeof(buf), "OK (alpha=%.2f)", gSrDynGainAlpha);
    return buf;
  }

  return "Error: invalid arguments — Usage: sr dyngain [on|off|min <0.1-10>|max <0.1-10>|target <1000-30000>|alpha <0.0-1.0>|reset]";
}

static const char* cmd_sr_raw(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();
  
  if (args.length() == 0) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "Raw output mode: %s\nShows ALL MultiNet detections regardless of confidence.\nUsage: sr raw [on|off]",
             gSrRawOutputEnabled ? "ON" : "OFF");
    return buf;
  }
  
  if (args == "on" || args == "1") {
    gSrRawOutputEnabled = true;
    return "OK (raw output enabled - all detections will be shown)";
  }
  if (args == "off" || args == "0") {
    gSrRawOutputEnabled = false;
    return "OK (raw output disabled)";
  }
  
  return "Error: invalid arguments — Usage: sr raw [on|off]";
}

static const char* cmd_sr_autotune(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();
  
  if (args.length() == 0 || args == "status") {
    if (gSrAutoTuneActive) {
      uint32_t elapsed = millis() - gSrAutoTuneStepStartMs;
      uint32_t remaining = (elapsed < kAutoTuneStepDurationMs) ? (kAutoTuneStepDurationMs - elapsed) / 1000 : 0;
      EXT_RAM_BSS_ATTR static char buf[256];
      snprintf(buf, sizeof(buf), 
               "Auto-tune ACTIVE: step %d/%d\n  Config: %s\n  %lu sec remaining\n  Say test phrases now!\nUsage: sr autotune [start|stop]",
               gSrAutoTuneStep + 1, (int)kAutoTuneConfigCount,
               kAutoTuneConfigs[gSrAutoTuneStep].description,
               (unsigned long)remaining);
      return buf;
    } else {
      return "Auto-tune: INACTIVE\nCycles through gain configurations to find best settings.\nUsage: sr autotune [start|stop]";
    }
  }
  
  if (args == "start") {
    if (gSrAutoTuneActive) {
      return "Auto-tune already running. Use 'srautotune stop' to cancel.";
    }
    gSrAutoTuneActive = true;
    gSrAutoTuneStep = 0;
    gSrAutoTuneStartMs = millis();
    gSrAutoTuneStepStartMs = millis();
    gSrRawOutputEnabled = true;  // Enable raw output during tuning
    
    // Apply first config
    gSettings.srAfeGain = kAutoTuneConfigs[0].afeGain;
    gSrDynGainMax = kAutoTuneConfigs[0].dynGainMax;
    gSrDynGainEnabled = kAutoTuneConfigs[0].dynGainEnabled;
    gSrDynGainCurrent = 1.0f;
    
    broadcastOutput("");
    broadcastOutput("=== AUTO-TUNE STARTED ===");
    broadcastOutput(String("Will cycle through ") + (int)kAutoTuneConfigCount + " configurations, " +
                      (int)(kAutoTuneStepDurationMs / 1000) + " sec each.");
    broadcastOutput("Say test phrases (system, battery, cancel, help) during each step.");
    broadcastOutput(String("Step 1/") + (int)kAutoTuneConfigCount + ": " + kAutoTuneConfigs[0].description);
    broadcastOutput("NOTE: AFE gain change requires SR restart. Run 'srstop' then 'srstart'.");
    
    return "Auto-tune started. Restart SR to apply AFE gain change.";
  }
  
  if (args == "stop") {
    if (!gSrAutoTuneActive) {
      return "Error: Auto-tune not running.";
    }
    gSrAutoTuneActive = false;
    gSrRawOutputEnabled = false;
    broadcastOutput("");
    broadcastOutput("=== AUTO-TUNE STOPPED ===");
    return "Auto-tune stopped. Review the results above to pick best config.";
  }
  
  return "Error: invalid arguments — Usage: sr autotune [start|stop|status]";
}

static const char* cmd_sr_timeout(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();
  
  if (args.length() == 0) {
    static char buf[96];
    snprintf(buf, sizeof(buf), "Command timeout: %d ms (%.1f sec)\nUsage: sr timeout <1000-30000>",
             gSettings.srCommandTimeout, gSettings.srCommandTimeout / 1000.0f);
    return buf;
  }
  
  int val = args.toInt();
  if (val < 1000 || val > 30000) {
    return "Error: timeout must be 1000-30000 ms";
  }
  
  setSetting(gSettings.srCommandTimeout, val);
  static char buf[80];
  snprintf(buf, sizeof(buf), "Command timeout set to %d ms (%.1f sec). Saved.", val, val / 1000.0f);
  return buf;
}

// Forward declarations so cmd_sr_tuning can delegate "srtuning <sub> <value>"
// to the dedicated single-param setters defined below.
static const char* cmd_sr_tuning_swgain(const String& argsInput);
static const char* cmd_sr_tuning_gain(const String& argsInput);
static const char* cmd_sr_tuning_agc(const String& argsInput);
static const char* cmd_sr_tuning_vad(const String& argsInput);
static const char* cmd_sr_tuning_filters(const String& argsInput);

static const char* cmd_sr_tuning(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // "srtuning <param> <value>" delegates to the matching setter; bare prints status.
  String args = argsInput;
  args.trim();
  if (args.length() > 0) {
    int sp = args.indexOf(' ');
    String sub = (sp < 0) ? args : args.substring(0, sp);
    String rest = (sp < 0) ? String("") : args.substring(sp + 1);
    sub.toLowerCase();
    rest.trim();
    if (sub == "gain")    return cmd_sr_tuning_gain(rest);
    if (sub == "agc")     return cmd_sr_tuning_agc(rest);
    if (sub == "vad")     return cmd_sr_tuning_vad(rest);
    if (sub == "swgain")  return cmd_sr_tuning_swgain(rest);
    if (sub == "filters") return cmd_sr_tuning_filters(rest);
    return "Error: unknown tuning param. Use: srtuning [gain|agc|vad|swgain|filters] <value>";
  }

  EXT_RAM_BSS_ATTR static char buf[520];
  int mg = gSettings.microphoneGain;
  float swgain = 24.0f * ((float)mg / 50.0f);
  snprintf(buf, sizeof(buf),
    "=== SR Audio Tuning ===\n"
    "micgain: %d%% (shared with microphone, 0-100) [LIVE]\n"
    "swgain: %.1f (derived from micgain) [LIVE]\n"
    "dcoffset: %d (current DC offset estimate)\n"
    "filters: %s (high-pass + pre-emphasis) [LIVE]\n"
    "gain: %.1f (AFE linear gain, 0.1-10.0)\n"
    "agc: %d (0=off, 1=-9dB, 2=-6dB, 3=-3dB)\n"
    "vad: %d (sensitivity 0-4, higher=more sensitive)\n"
    "confidence: %.2f (command threshold)\n"
    "timeout: %d ms\n"
    "\nUsage: micgain <0-100>\n"
    "Usage: srtuning <gain|agc|vad|filters> <value>\n"
    "Usage: srtuning swgain <1.0-50.0> (sets micgain)",
    mg, swgain, (int)getMicDcOffset(), gSrFiltersEnabled ? "ON" : "OFF",
    gSettings.srAfeGain, gSettings.srAgcMode, gSettings.srVadMode,
    gSrMinCommandConfidence, gSettings.srCommandTimeout);
  return buf;
}

static const char* cmd_sr_tuning_swgain(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();
  
  if (args.length() == 0) {
    static char buf[100];
    int mg = gSettings.microphoneGain;
    float swgain = 24.0f * ((float)mg / 50.0f);
    snprintf(buf, sizeof(buf), "micgain: %d%% (swgain: %.1f, DC offset: %d)\nUsage: sr tuning swgain <1.0-50.0>",
             mg, swgain, (int)getMicDcOffset());
    return buf;
  }
  
  float val = args.toFloat();
  if (val < 1.0f || val > 50.0f) {
    return "Error: swgain must be 1.0-50.0";
  }

  int mg = (int)lroundf((val / 24.0f) * 50.0f);
  if (mg < 0) mg = 0;
  if (mg > 100) mg = 100;
  setSetting(gSettings.microphoneGain, mg);
#if ENABLE_MICROPHONE
  micGain = mg;
#endif
  float actualSwgain = 24.0f * ((float)mg / 50.0f);
  static char buf[120];
  snprintf(buf, sizeof(buf), "OK (micgain=%d%%, swgain=%.1f)", mg, actualSwgain);
  return buf;
}

static const char* cmd_sr_tuning_gain(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();
  
  if (args.length() == 0) {
    static char buf[80];
    snprintf(buf, sizeof(buf), "AFE linear gain: %.1f\nUsage: sr tuning gain <0.1-10.0>", gSettings.srAfeGain);
    return buf;
  }
  
  float val = args.toFloat();
  if (val < 0.1f || val > 10.0f) {
    return "Error: gain must be 0.1-10.0";
  }
  
  setSetting(gSettings.srAfeGain, val);
  static char buf[80];
  snprintf(buf, sizeof(buf), "AFE gain set to %.1f. Restart SR to apply.", val);
  return buf;
}

static const char* cmd_sr_tuning_agc(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();
  
  if (args.length() == 0) {
    static char buf[100];
    snprintf(buf, sizeof(buf), "AGC mode: %d (0=off, 1=-9dB, 2=-6dB, 3=-3dB)\nUsage: sr tuning agc <0-3>", gSettings.srAgcMode);
    return buf;
  }
  
  int val = args.toInt();
  if (val < 0 || val > 3) {
    return "Error: agc must be 0-3";
  }
  
  setSetting(gSettings.srAgcMode, val);
  static char buf[80];
  const char* modeStr[] = {"off", "-9dB", "-6dB", "-3dB"};
  snprintf(buf, sizeof(buf), "AGC mode set to %d (%s). Restart SR to apply.", val, modeStr[val]);
  return buf;
}

static const char* cmd_sr_tuning_vad(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();
  
  if (args.length() == 0) {
    static char buf[100];
    snprintf(buf, sizeof(buf), "VAD mode: %d (0-4, higher=more sensitive)\nUsage: sr tuning vad <0-4>", gSettings.srVadMode);
    return buf;
  }
  
  int val = args.toInt();
  if (val < 0 || val > 4) {
    return "Error: vad must be 0-4";
  }
  
  setSetting(gSettings.srVadMode, val);
  static char buf[80];
  snprintf(buf, sizeof(buf), "VAD sensitivity set to %d. Restart SR to apply.", val);
  return buf;
}

static const char* cmd_sr_tuning_filters(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();
  
  if (args.length() == 0) {
    static char buf[150];
    snprintf(buf, sizeof(buf), 
      "Audio filters: %s (high-pass + pre-emphasis)\n"
      "When OFF: only DC offset removal + gain applied\n"
      "Usage: sr tuning filters <on|off>",
      gSrFiltersEnabled ? "ON" : "OFF");
    return buf;
  }
  
  if (args.equalsIgnoreCase("on") || args == "1") {
    gSrFiltersEnabled = true;
    return "Audio filters ENABLED (high-pass + pre-emphasis)";
  } else if (args.equalsIgnoreCase("off") || args == "0") {
    gSrFiltersEnabled = false;
    return "Audio filters DISABLED (DC offset + gain only)";
  }
  return "Error: invalid arguments — Usage: sr tuning filters <on|off>";
}

static const char* cmd_sr_snip(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  return "Error: invalid arguments — Usage: sr snip <on|off|start|stop|status|config>";
}

static const char* cmd_sr_snip_on(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (!gESPSRRunning) {
    return "Error: SR not running. Run: srstart";
  }
  if (!srSnipInit()) {
    return "Error: failed to initialize snippet capture";
  }
  gSrSnipEnabled = true;
  return "Snippet capture enabled (will trigger on wake word)";
}

static const char* cmd_sr_snip_off(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  gSrSnipEnabled = false;
  return "Snippet capture disabled";
}

static const char* cmd_sr_snip_start(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (!gESPSRRunning) {
    return "Error: SR not running. Run: srstart";
  }
  if (!srSnipInit()) {
    return "Error: failed to initialize snippet capture";
  }
  gSrSnipManualStartRequested = true;
  return "Manual snippet capture started";
}

static const char* cmd_sr_snip_stop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (!gSrSnipSessionActive) {
    return "Error: No active snippet session";
  }
  gSrSnipManualStopRequested = true;
  return "Manual snippet capture stopped";
}

static const char* cmd_sr_snip_status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  static String out;
  out = "";
  out += "Snippet capture: ";
  out += gSrSnipEnabled ? "enabled" : "disabled";
  out += "\nSession active: ";
  out += gSrSnipSessionActive ? "yes" : "no";
  out += "\nRing buffer: ";
  out += String((unsigned)gSrSnipRingSamples);
  out += " samples (";
  out += String((unsigned)gSrSnipPreMs);
  out += " ms pre-trigger)\nMax duration: ";
  out += String((unsigned)gSrSnipMaxMs);
  out += " ms\nDestination: ";
  out += (gSrSnipDest == SrSnipDest::Auto) ? "auto" : ((gSrSnipDest == SrSnipDest::SD) ? "sd" : "internal");
  out += "\nFolder: ";
  out += srSnipGetFolder();
  out += "\nQueue initialized: ";
  out += gSrSnipQueue ? "yes" : "no";
  out += "\nSession ID: ";
  out += String((unsigned)gSrSnipSessionId);
  if (gSrSnipSessionActive) {
    out += "\nSession samples: ";
    out += String((unsigned)gSrSnipSessionSamplesWritten);
    out += "/";
    out += String((unsigned)gSrSnipSessionSamplesCap);
  }
  return out.c_str();
}

static const char* cmd_sr_snip_config(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (a.count() == 0) {
    static String out;
    out = "Snippet config:\n";
    out += "  pre_ms=";
    out += String((unsigned)gSrSnipPreMs);
    out += " (pre-trigger buffer)\n  max_ms=";
    out += String((unsigned)gSrSnipMaxMs);
    out += " (max duration)\n  dest=";
    out += (gSrSnipDest == SrSnipDest::Auto) ? "auto" : ((gSrSnipDest == SrSnipDest::SD) ? "sd" : "internal");
    out += "\nUsage: sr snip config <pre_ms|max_ms|dest> <value>";
    return out.c_str();
  }
  if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: sr snip config <pre_ms|max_ms|dest> <value>";
  String key = a.arg(0);
  key.toLowerCase();
  String val = a.arg(1);
  if (key == "pre_ms") {
    int v = val.toInt();
    if (v < 100) v = 100;
    if (v > 5000) v = 5000;
    gSrSnipPreMs = (uint32_t)v;
    srSnipFreeRingBuffer();
    if (gSrSnipEnabled) srSnipInitRingBuffer();
    static char buf[64];
    snprintf(buf, sizeof(buf), "pre_ms set to %u", (unsigned)gSrSnipPreMs);
    return buf;
  } else if (key == "max_ms") {
    int v = val.toInt();
    if (v < 1000) v = 1000;
    if (v > 30000) v = 30000;
    gSrSnipMaxMs = (uint32_t)v;
    static char buf[64];
    snprintf(buf, sizeof(buf), "max_ms set to %u", (unsigned)gSrSnipMaxMs);
    return buf;
  } else if (key == "dest") {
    val.toLowerCase();
    if (val == "auto") {
      gSrSnipDest = SrSnipDest::Auto;
    } else if (val == "sd") {
      gSrSnipDest = SrSnipDest::SD;
    } else if (val == "internal" || val == "littlefs") {
      gSrSnipDest = SrSnipDest::LittleFS;
    } else {
      return "Error: dest must be auto, sd, or internal";
    }
    return "Destination updated";
  }
  return "Error: Unknown config key. Use: pre_ms, max_ms, dest";
}

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry espsrCommands[] = {
  { "sr", "ESP-SR speech recognition commands.", false, cmd_sr, "Usage: sr <enable|start|stop|status|stack|cmds|debug|confidence|timeout|tuning|accept|dyngain|raw|autotune|snip>" },
  { "srenable", "ESP-SR enable is a compile-time flag (cannot be toggled at runtime).", true, cmd_sr_enable, "Usage: srenable   (informational; ESP-SR is set at compile time, any 0|1 argument is ignored)" },
  { "opensr", "Start ESP-SR pipeline and arm voice as the current user.", false, cmd_sr_start, "Usage: opensr" },
  { "closesr", "Stop ESP-SR pipeline.", false, cmd_sr_stop, "Usage: closesr" },
  { "srstatus", "Show ESP-SR status.", false, cmd_sr_status, "Usage: srstatus" },
  { "srstack", "Show sr_task stack high-water mark (run after voice stress test).", false, cmd_sr_stack, "Usage: srstack" },
  { "srstart", "Start ESP-SR pipeline and arm voice as the current user.", false, cmd_sr_start, "Usage: srstart" },
  { "srstop", "Stop ESP-SR pipeline.", false, cmd_sr_stop, "Usage: srstop" },
  { "voicearm", "Arm voice command execution as the current authenticated user.", false, cmd_voice_arm_cli, "Usage: voicearm" },
  { "voicedisarm", "Disarm voice command execution.", false, cmd_voice_disarm_cli, "Usage: voicedisarm" },
  { "voicestatus", "Show voice arming status.", false, cmd_voice_status_cli, "Usage: voicestatus" },
  { "srcmds", "Manage MultiNet command phrases.", true, cmd_sr_cmds, "Usage: srcmds <list|add|del|clear|save|reload|sync>" },
  { "srcmdslist", "List current MultiNet commands.", true, cmd_sr_cmds_list, "Usage: srcmdslist" },
  { "srcmdsadd", "Add or update a MultiNet command.", true, cmd_sr_cmds_add, "Usage: srcmdsadd <id> <phrase>" },
  { "srcmdsdel", "Delete a MultiNet command (by phrase or id).", true, cmd_sr_cmds_del, "Usage: srcmdsdel <phrase|id>" },
  { "srcmdsclear", "Clear all MultiNet commands.", true, cmd_sr_cmds_clear, "Usage: srcmdsclear confirm" },
  { "srcmdsreload", "Reload commands from SD file.", true, cmd_sr_cmds_reload, "Usage: srcmdsreload" },
  { "srcmdssave", "Save current commands to SD file.", true, cmd_sr_cmds_save, "Usage: srcmdssave" },
  { "srcmdssync", "Sync voice commands from CLI registry.", true, cmd_sr_cmds_sync, "Usage: srcmdssync" },
  { "srdebug", "SR debug/telemetry commands.", false, cmd_sr_debug, "Usage: srdebug <level|telem|stats|reset>" },
  { "srdebuglevel", "Set debug verbosity (0-4).", false, cmd_sr_debug_level, "Usage: srdebuglevel [0-4]" },
  { "srdebugtelem", "Set periodic telemetry interval (ms, 0=off).", false, cmd_sr_debug_telem, "Usage: srdebugtelem [ms]" },
  { "srdebugstats", "Print current SR statistics.", false, cmd_sr_debug_stats, "Usage: srdebugstats" },
  { "srdebugreset", "Reset SR debug counters.", false, cmd_sr_debug_reset, "Usage: srdebugreset" },
  { "srconfidence", "Get/set command confidence threshold.", false, cmd_sr_confidence, "Usage: srconfidence [<0.0-1.0> | category <0.0-1.0> | target <0.0-1.0>]" },
  { "sraccept", "Configure target acceptance policy (gap acceptance).", false, cmd_sr_accept, "Usage: sraccept [on|off|floor <0.0-1.0>|gap <0.0-1.0>|speech <0|1>]" },
  { "srdyngain", "Configure dynamic gain normalization (MultiNet input only).", false, cmd_sr_dyngain, "Usage: srdyngain [on|off|min <0.1-10>|max <0.1-10>|target <1000-30000>|alpha <0.0-1.0>|reset]" },
  { "srraw", "Toggle raw output mode (shows all MultiNet hypotheses).", false, cmd_sr_raw, "Usage: srraw [on|off]" },
  { "srautotune", "Auto-cycle through gain configurations to find best settings.", false, cmd_sr_autotune, "Usage: srautotune [start|stop|status]" },
  { "srtimeout", "Get/set command listening timeout.", false, cmd_sr_timeout, "Usage: srtimeout [1000-30000]" },
  { "srtuning", "Show/set audio tuning parameters.", false, cmd_sr_tuning, "Usage: srtuning [<gain|agc|vad|swgain|filters> <value>]  (bare = show status)" },
  { "srtuningswgain", "Set software gain (1.0-50.0) by updating shared micgain.", false, cmd_sr_tuning_swgain, "Usage: srtuningswgain <1.0-50.0>" },
  { "srtuninggain", "Set AFE linear gain (0.1-10.0).", false, cmd_sr_tuning_gain, "Usage: srtuninggain <0.1-10.0>" },
  { "srtuningagc", "Set AGC mode (0=off, 1-3=levels).", false, cmd_sr_tuning_agc, "Usage: srtuningagc <0-3>" },
  { "srtuningvad", "Set VAD sensitivity (0-4).", false, cmd_sr_tuning_vad, "Usage: srtuningvad <0-4>" },
  { "srtuningfilters", "Toggle audio filters (high-pass + pre-emphasis).", false, cmd_sr_tuning_filters, "Usage: srtuningfilters <on|off>" },
  { "srsnip", "Voice snippet capture commands.", false, cmd_sr_snip, "Usage: srsnip <on|off|start|stop|status|config>" },
  { "srsnipon", "Enable auto-capture on wake word.", false, cmd_sr_snip_on, "Usage: srsnipon" },
  { "srsnipoff", "Disable auto-capture.", false, cmd_sr_snip_off, "Usage: srsnipoff" },
  { "srsnipstart", "Start manual snippet capture now.", false, cmd_sr_snip_start, "Usage: srsnipstart" },
  { "srsnipstop", "Stop manual snippet capture and save.", false, cmd_sr_snip_stop, "Usage: srsnipstop" },
  { "srsnipstatus", "Show snippet capture status.", false, cmd_sr_snip_status, "Usage: srsnipstatus" },
  { "srsnipconfig", "Configure snippet capture params.", false, cmd_sr_snip_config, "Usage: srsnipconfig [pre_ms|max_ms|dest] [value]" },
  // Voice-only helper commands; their "*" (all-stages) phrases live in kVoiceRoutes
  { "voicecancel", "Cancel current voice command sequence.", false, cmd_voice_cancel },
  { "voicehelp", "Show available voice options for current state.", false, cmd_voice_help },
};

const size_t espsrCommandsCount = sizeof(espsrCommands) / sizeof(espsrCommands[0]);
// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// ESP-SR Settings Module
// ============================================================================

static bool isESPSRConnected() {
  return gESPSRInitialized;
}

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry espsrSettingsEntries[] = {
  { "srEnabled", SETTING_BOOL, &gSettings.srEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "srenabled" },
  { "srAutoStart", SETTING_BOOL, &gSettings.srAutoStart, 0, 0, nullptr, 0, 1, "Auto-start at boot", nullptr, false, nullptr, "srautostart" },
  { "srModelSource", SETTING_INT, &gSettings.srModelSource, 0, 0, nullptr, 0, 2, "Model source (0=partition, 1=SD, 2=LittleFS)", "0|Partition,1|SD,2|LittleFS", false, nullptr, "srmodelsource" },
  { "srCommandTimeout", SETTING_INT, &gSettings.srCommandTimeout, 6000, 0, nullptr, 1000, 30000, "Command timeout (ms)", nullptr, false, nullptr, "srtimeout" },
};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule espsrSettingsModule = {
  "espsr",
  "apps.espsr",
  espsrSettingsEntries,
  sizeof(espsrSettingsEntries) / sizeof(espsrSettingsEntries[0]),
  isESPSRConnected,
  "ESP-SR on-device speech recognition"
};

void registerESPSRHandlers(httpd_handle_t server) {
  (void)server;
}

// ============================================================================
// Voice State Getters (for OLED/Web display)
// ============================================================================

const char* getESPSRVoiceState() {
  switch (gVoiceState) {
    case VoiceState::IDLE: return "idle";
    case VoiceState::AWAIT_CATEGORY: return "category";
    case VoiceState::AWAIT_SUBCATEGORY: return "subcategory";
    case VoiceState::AWAIT_TARGET: return "target";
    default: return "unknown";
  }
}

const char* getESPSRCurrentCategory() {
  static String s;
  s = gCurrentCategory;
  return s.c_str();
}

const char* getESPSRCurrentSubCategory() {
  static String s;
  s = gCurrentSubCategory;
  return s.c_str();
}

const char* getESPSRLastCommand() {
  static String s;
  s = gLastCommand;
  return s.c_str();
}

float getESPSRLastConfidence() {
  return gLastConfidence;
}

uint32_t getESPSRWakeCount() {
  return gWakeWordCount;
}

uint32_t getESPSRCommandCount() {
  return gCommandCount;
}

#endif // ENABLE_ESP_SR
