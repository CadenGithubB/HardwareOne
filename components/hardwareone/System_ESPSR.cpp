#include "System_ESPSR.h"
#include "System_Events.h"  // systemEventPost — voice events
#include "System_BuildConfig.h"
#if ENABLE_OLED_DISPLAY
#include "OLED_UI.h"  // oledNotificationBannerShow — voice-failure banner
#endif
#include <esp_attr.h>
#include <esp_heap_caps.h>

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
#include "System_ESPSRModelPack.h"
#include <atomic>
#include <new>
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

// Every microphone enters speech recognition through the shared mono PCM HAL.
#define I2S_SR_SAMPLE_RATE  16000
#define I2S_SR_BITS         16
#define I2S_SR_CHANNELS     1

// Task configuration (SR_STACK_WORDS and SR_TASK_PRIORITY_LEVEL defined in System_TaskUtils.h)
#define SR_AUDIO_CHUNK_MS   32
#define SR_AUDIO_CHUNK_SIZE (I2S_SR_SAMPLE_RATE * I2S_SR_CHANNELS * sizeof(int16_t) * SR_AUDIO_CHUNK_MS / 1000)

// State
static bool gESPSRInitialized = false;
static std::atomic<bool> gESPSRRunning{false};
static bool gESPSRWakeDetected = false;
static TaskHandle_t gSRTaskHandle = nullptr;
static std::atomic<bool> gSRTaskShouldRun{false};
static std::atomic<bool> gSRFeedShouldRun{false};
static std::atomic<bool> gSRTaskExited{true};
static std::atomic<bool> gSRFeedExited{true};
static std::atomic<UBaseType_t> gSRStackHwm{0};
static TaskHandle_t gSRFeedTaskHandle = nullptr;
static SemaphoreHandle_t gSRLifecycleMutex = nullptr;
static SemaphoreHandle_t gSRSnipMutex = nullptr;
static int16_t* gSRFeedBuffer = nullptr;
static int16_t* gSRMNBuffer = nullptr;
static size_t gSRFeedSamples = 0;
static size_t gSRFetchSamples = 0;
static std::atomic<const char*> gSRError{nullptr};
static srmodel_list_t* gSRModels = nullptr;
static void* gSRModelBlob = nullptr;
static size_t gSRModelBytes = 0;
static const char* gSRModelSource = "none";

// Task creation and teardown are serialized even when initiated from the OLED.
// A timed-out stop retains all handles and buffers for a subsequent join.
class SRLifecycleGuard {
 public:
  SRLifecycleGuard() : locked_(gSRLifecycleMutex &&
      xSemaphoreTake(gSRLifecycleMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {}
  ~SRLifecycleGuard() { if (locked_) xSemaphoreGive(gSRLifecycleMutex); }
  explicit operator bool() const { return locked_; }
 private:
  bool locked_;
};

// Capture and detection now run independently. Preserve raw snippet capture
// while serializing its session buffer against detection and CLI operations.
class SRSnipGuard {
 public:
  SRSnipGuard() : locked_(gSRSnipMutex &&
      xSemaphoreTakeRecursive(gSRSnipMutex, portMAX_DELAY) == pdTRUE) {}
  ~SRSnipGuard() { if (locked_) xSemaphoreGiveRecursive(gSRSnipMutex); }
  explicit operator bool() const { return locked_; }
 private:
  bool locked_;
};

static void srSetError(const char* message) {
  gSRError.store(message, std::memory_order_release);
  ERROR_SRF("%s", message);
}


// AFE and model handles
static const esp_afe_sr_iface_t* gAFE = nullptr;

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
// FST lookup arrays scale with the highest ID, not phrase count. Bound custom
// IDs before the vendor's unsafe allocation path; reserved globals use990-992.
static constexpr int kSRMaxCommandId = 1023;
static bool gMNCommandsAllocated = false;
static bool gMNGrammarPublished = false;
// A failed/partial command-file reload or publication must never be saved over
// the user's commands file. A successful file reload reestablishes trust.
static bool gMnTableTrusted = true;

static bool gRestoreMicAfterSR = false;

// Statistics
static uint32_t gWakeWordCount = 0;
static std::atomic<uint32_t> gCommandCount{0};

static SemaphoreHandle_t gVoiceArmMutex = nullptr;
static bool gVoiceArmed = false;
static String gVoiceArmedUser = "";
static CommandSource gVoiceArmedByTransport = SOURCE_INTERNAL;
static String gVoiceArmedByIp = "";
static uint32_t gVoiceArmedAtMs = 0;
static uint32_t gVoiceArmGeneration = 0;
static TransportSessionEpoch gVoiceArmEpoch = 0;
static bool gVoiceArmRequiresEpoch = false;

extern bool executeCommand(AuthContext& ctx, const char* cmd, char* out, size_t outSize);

static portMUX_TYPE gSRMutexInitMux = portMUX_INITIALIZER_UNLOCKED;

static bool ensureSRMutex(SemaphoreHandle_t& slot, bool recursive = false) {
  portENTER_CRITICAL(&gSRMutexInitMux);
  bool ready = slot != nullptr;
  portEXIT_CRITICAL(&gSRMutexInitMux);
  if (ready) return true;
  SemaphoreHandle_t candidate = recursive ? xSemaphoreCreateRecursiveMutex() : xSemaphoreCreateMutex();
  if (!candidate) return false;
  portENTER_CRITICAL(&gSRMutexInitMux);
  if (!slot) { slot = candidate; candidate = nullptr; }
  portEXIT_CRITICAL(&gSRMutexInitMux);
  if (candidate) vSemaphoreDelete(candidate);
  return true;
}

static void ensureVoiceArmMutex() {
  ensureSRMutex(gVoiceArmMutex);
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
  ++gVoiceArmGeneration;
  gVoiceArmEpoch = 0;
  gVoiceArmRequiresEpoch = false;
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

  const bool requiresEpoch = ctx.transport == SOURCE_SERIAL ||
      ctx.transport == SOURCE_LOCAL_DISPLAY || ctx.transport == SOURCE_BLUETOOTH ||
      ctx.transport == SOURCE_UART || ctx.transport == SOURCE_G2_GLASSES ||
      (ctx.transport == SOURCE_WEB && ctx.sid.length() != 0);
  const TransportSessionEpoch epoch = captureTransportSessionEpoch(ctx);
  if (requiresEpoch && epoch == 0) return false;
  ++gVoiceArmGeneration;
  gVoiceArmEpoch = epoch;
  gVoiceArmRequiresEpoch = requiresEpoch;
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
  if (!gVoiceArmMutex || xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  bool armed = gVoiceArmed;
  outUser = gVoiceArmedUser;
  if (gVoiceArmMutex) xSemaphoreGive(gVoiceArmMutex);
  return armed;
}

struct VoiceCommandJob {
  Command command;
  uint32_t armGeneration = 0;
  CommandSource armTransport = SOURCE_INTERNAL;
  TransportSessionEpoch armEpoch = 0;
  bool requiresEpoch = false;
};

// Runs on cmd_exec, so checking the captured authority and executing the normal
// authenticated pipeline cannot deadlock an SR stop queued ahead of this job.
static void runVoiceCommandJob(void* opaque) {
  VoiceCommandJob* job = static_cast<VoiceCommandJob*>(opaque);
  bool authorized = false;
  if (gVoiceArmMutex && xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    authorized = gVoiceArmed && gVoiceArmGeneration == job->armGeneration &&
        gVoiceArmedUser == job->command.ctx.auth.user &&
        (!job->requiresEpoch || (job->armEpoch != 0 &&
         transportSessionEpochIsLive(job->armTransport, job->armEpoch)));
    // A bypass login must not survive enabling authentication on its source.
    if (authorized && job->command.ctx.auth.user == "AuthBypass") {
      authorized = !((job->armTransport == SOURCE_SERIAL && gSettings.serialRequireAuth) ||
          (job->armTransport == SOURCE_UART && gSettings.uartRequireAuth) ||
          (job->armTransport == SOURCE_LOCAL_DISPLAY && gSettings.localDisplayRequireAuth) ||
          (job->armTransport == SOURCE_BLUETOOTH && gSettings.bleRequireAuth));
    }
    xSemaphoreGive(gVoiceArmMutex);
  }
  char* result = static_cast<char*>(ps_alloc(CMD_RESULT_MAX, AllocPref::PreferPSRAM, "sr.command.result"));
  bool ok = false;
  if (result) {
    if (authorized) {
      void* previous = currentCommandContext();
      setCurrentCommandContext(&job->command.ctx);
      ok = executeCommand(job->command.ctx.auth, job->command.line.c_str(), result, CMD_RESULT_MAX);
      setCurrentCommandContext(previous);
    } else {
      snprintf(result, CMD_RESULT_MAX, "Voice authority changed before command execution");
    }
    broadcastOutput(String(ok ? "[Voice] Completed: " : "[Voice] Failed: ") + result);
    free(result);
  } else {
    broadcastOutput("[Voice] Failed: insufficient memory for command execution");
  }
  if (ok) {
    ++gCommandCount;
    systemEventPost(SYSEVT_VOICE_COMMAND, job->command.line.c_str());
  }
  job->~VoiceCommandJob();
  free(job);
}

static bool executeVoiceCommandAsArmedUser(const char* cliCmd, char* out, size_t outSize) {
  ensureVoiceArmMutex();
  if (!gVoiceArmMutex || xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    snprintf(out, outSize, "Voice authority busy");
    return false;
  }
  VoiceCommandJob* job = nullptr;
  if (gVoiceArmed && gVoiceArmedUser.length() != 0) {
    void* memory = ps_alloc(sizeof(VoiceCommandJob), AllocPref::PreferPSRAM, "sr.command.job");
    if (memory) {
      job = new (memory) VoiceCommandJob();
      job->armGeneration = gVoiceArmGeneration;
      job->armTransport = gVoiceArmedByTransport;
      job->armEpoch = gVoiceArmEpoch;
      job->requiresEpoch = gVoiceArmRequiresEpoch;
      job->command.ctx.auth.user = gVoiceArmedUser;
    }
  }
  xSemaphoreGive(gVoiceArmMutex);
  if (!job) {
    snprintf(out, outSize, "Voice not armed or insufficient memory");
    return false;
  }
  job->command.line = cliCmd;
  job->command.ctx.origin = ORIGIN_VOICE;
  job->command.ctx.auth.transport = SOURCE_VOICE;
  job->command.ctx.auth.ip = "voice";
  job->command.ctx.auth.path = "/voice";
  job->command.ctx.id = millis();
  job->command.ctx.timestampMs = millis();
  job->command.ctx.outputMask = MSG_ROUTE_FILE;
  job->command.ctx.behaviorFlags = COMMAND_CONTEXT_MODE_INDEPENDENT;
  extern bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg);
  if (!submitDeferredToCmdExec(runVoiceCommandJob, job)) {
    job->~VoiceCommandJob();
    free(job);
    snprintf(out, outSize, "Command queue unavailable");
    return false;
  }
  snprintf(out, outSize, "Queued for authenticated execution");
  return true;
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
// AFE supplies the recognizer's front end. Extra high-pass/pre-emphasis reduced
// wake recognition in the P4 acoustic comparison; retain it only as an opt-in.
static bool gSrFiltersEnabled = false;

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
static bool mnUpdateLocked();
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

static esp_mn_phrase_t* findLoadedVoicePhrase(const char* phrase) {
  for (int index = 0; ; ++index) {
    esp_mn_phrase_t* existing = esp_mn_commands_get_from_index(index);
    if (!existing) return nullptr;
    if (existing->string && normalizePhrase(existing->string) == normalizePhrase(phrase)) return existing;
  }
}

// MN7 results.string is a phonetic sequence. Dispatch and UI need the original
// registered phrase, copied while the command table lock is still held.
static bool copyRecognizedVoicePhrase(int commandId, char* out, size_t capacity) {
  const char* phrase = esp_mn_commands_get_string(commandId);
  if (!phrase || !phrase[0] || strlen(phrase) >= capacity) return false;
  memcpy(out, phrase, strlen(phrase) + 1);
  return true;
}

// Duplicate targets may be aliases of the same command. A phrase bound to two
// different commands is ambiguous and must not silently overwrite its old ID.
static bool loadedTargetMatches(esp_mn_phrase_t* existing, const char* cli) {
  const char* mapped = existing ? findCliCommandForId(existing->command_id) : nullptr;
  return mapped && strcmp(mapped, cli) == 0;
}

// Add global voice routes (cat "*") to current MultiNet command set
// These are available at all stages (category, subcategory, target)
// Call after clearing commands, before adding stage-specific phrases
static bool addSpecialPhrases() {
  bool valid = true;
  int globalId = GLOBAL_VOICE_CMD_ID_START;  // Start from reserved ID range

  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (!r.cat || !r.target) continue;

    // Check for global marker
    if (strcmp(r.cat, "*") != 0) continue;
    if (!routeAlive(r)) continue;

    if (esp_mn_phrase_t* existing = findLoadedVoicePhrase(r.target)) {
      valid = loadedTargetMatches(existing, r.cli) && valid;
      continue;
    }
    esp_err_t err = esp_mn_commands_add(globalId, r.target);
    if (err == ESP_OK) {
      addVoiceCliMapping(globalId, r.cli);  // Map to CLI command name, not voice phrase
      DEBUG_SRF("[HIER-DEBUG] Added global phrase: id=%d phrase='%s' -> cli='%s'",
               globalId, r.target, r.cli);
    } else {
      valid = false;
      WARN_SYSTEMF("[HIER-DEBUG] Failed to add global phrase '%s': err=0x%x",
                   r.target, err);
    }
    globalId++;
  }
  return valid;
}

// Load targets for a specific category into MultiNet
// Returns true if any targets were loaded, false if category has no targets (single-stage)
static bool loadTargetsForCategory(const char* category) {
  DEBUG_SRF("[HIER-DEBUG] loadTargetsForCategory('%s') called", category);
  DEBUG_SRF("[HIER-DEBUG]   Total voice routes: %u", (unsigned)kVoiceRouteCount);
  
  if (!mnCommandsReady()) {
    WARN_SYSTEMF("[HIER-DEBUG] loadTargetsForCategory: MultiNet not ready!");
    return false;
  }
  if (!lockMN(5000)) {
    WARN_SYSTEMF("[HIER-DEBUG] loadTargetsForCategory: Failed to lock MultiNet after 5s!");
    return false;
  }
  
  DEBUG_SRF("[HIER-DEBUG] Clearing MultiNet commands...");
  if (esp_mn_commands_clear() != ESP_OK) { unlockMN(); return false; }
  clearVoiceCliMappings();
  
  // Always add special phrases (cancel, help) for abort/help capability
  bool valid = addSpecialPhrases();
  
  String normCategory = normalizePhrase(category);
  int nextId = 1;
  int loaded = 0;
  int scanned = 0;
  int categoryMatches = 0;
  
  // Find all routes with this category and load their targets
  DEBUG_SRF("[HIER-DEBUG] Scanning routes for category '%s' (normalized='%s') targets...", category, normCategory.c_str());
  for (size_t i = 0; i < kVoiceRouteCount && nextId < MAX_VOICE_CLI_MAPPINGS; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    scanned++;

    if (!r.cat || (r.sub && r.sub[0])) continue;
    if (!routeAlive(r)) continue;

    if (normalizePhrase(r.cat) == normCategory) {
      categoryMatches++;
      DEBUG_SRF("[HIER-DEBUG]   Found category match: cmd='%s' target='%s'",
               r.cli, r.target ? r.target : "(null)");

      if (r.target && r.target[0] != '\0') {
        if (esp_mn_phrase_t* existing = findLoadedVoicePhrase(r.target)) {
          valid = loadedTargetMatches(existing, r.cli) && valid;
          continue;
        }
        esp_err_t err = esp_mn_commands_add(nextId, r.target);
        if (err == ESP_OK) {
          addVoiceCliMapping(nextId, r.cli);  // Map target ID to CLI command
          DEBUG_SRF("[HIER-DEBUG]   OK Added to MultiNet: id=%d phrase='%s' -> cli='%s'",
                   nextId, r.target, r.cli);
          nextId++;
          loaded++;
        } else {
          valid = false;
          WARN_SYSTEMF("[HIER-DEBUG]   FAIL Failed to add '%s': err=0x%x", r.target, err);
        }
      } else {
        DEBUG_SRF("[HIER-DEBUG]   (no target - single-stage command)");
      }
    }
  }
  
  DEBUG_SRF("[HIER-DEBUG] Scan complete: scanned=%d, categoryMatches=%d, loaded=%d",
           scanned, categoryMatches, loaded);
  
  if (loaded > 0) {
    DEBUG_SRF("[HIER-DEBUG] Updating MultiNet with %d targets...", loaded);
    if (!mnUpdateLocked()) {
      valid = false;
      WARN_SYSTEMF("[HIER-DEBUG] MultiNet update failed");
    } else {
      DEBUG_SRF("[HIER-DEBUG] MultiNet update successful");
    }
  }
  unlockMN();
  
  INFO_SRF("[HIER] ===== Loaded %d targets for category '%s' =====", loaded, category);
  return valid && loaded > 0;
}

// Load categories (first-stage commands) into MultiNet
static bool loadCategories() {
  DEBUG_SRF("[HIER-DEBUG] ========== loadCategories() BEGIN ==========");
  DEBUG_SRF("[HIER-DEBUG] Total voice routes: %u", (unsigned)kVoiceRouteCount);
  srReportDeadVoiceRoutes();

  if (!mnCommandsReady()) {
    WARN_SYSTEMF("[HIER-DEBUG] loadCategories: MultiNet not ready!");
    return false;
  }
  if (!lockMN(5000)) {
    WARN_SYSTEMF("[HIER-DEBUG] loadCategories: Failed to lock MultiNet after 5s!");
    return false;
  }
  
  DEBUG_SRF("[HIER-DEBUG] Clearing MultiNet commands...");
  if (esp_mn_commands_clear() != ESP_OK) { unlockMN(); return false; }
  clearVoiceCliMappings();
  
  // Always add special phrases (cancel, help) for abort/help capability
  bool valid = addSpecialPhrases();
  
  int nextId = 1;
  int loaded = 0;
  int scanned = 0;
  int withVoice = 0;
  int duplicates = 0;
  
  // Add unique categories
  DEBUG_SRF("[HIER-DEBUG] Scanning routes for unique categories...");
  for (size_t i = 0; i < kVoiceRouteCount && nextId < MAX_VOICE_CLI_MAPPINGS; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    scanned++;

    if (!r.cat || r.cat[0] == '\0') continue;
    // Skip global routes (cat "*") - already handled by addSpecialPhrases()
    if (strcmp(r.cat, "*") == 0) continue;
    if (!routeAlive(r)) continue;

    withVoice++;
    DEBUG_SRF("[HIER-DEBUG]   [%u] cmd='%s' category='%s' target='%s'",
             (unsigned)i, r.cli, r.cat,
             r.target ? r.target : "(null)");

    // Globals and stage phrases share one list, independent of command IDs.
    if (findLoadedVoicePhrase(r.cat)) {
      DEBUG_SRF("[HIER-DEBUG]     ^ Category '%s' already added (duplicate)", r.cat);
      duplicates++;
      continue;
    }

    esp_err_t err = esp_mn_commands_add(nextId, r.cat);
    if (err == ESP_OK) {
      addVoiceCliMapping(nextId, r.cat);  // Map to category name
      DEBUG_SRF("[HIER-DEBUG]     OK Added category to MultiNet: id=%d phrase='%s'", nextId, r.cat);
      nextId++;
      loaded++;
    } else {
      valid = false;
      WARN_SYSTEMF("[HIER-DEBUG]     FAIL Failed to add category '%s': err=0x%x", r.cat, err);
    }
  }
  
  DEBUG_SRF("[HIER-DEBUG] Scan complete: scanned=%d, withVoice=%d, duplicates=%d, unique=%d",
           scanned, withVoice, duplicates, loaded);
  
  valid = valid && loaded > 0;
  if (loaded > 0) {
    DEBUG_SRF("[HIER-DEBUG] Updating MultiNet with %d categories...", loaded);
    if (!mnUpdateLocked()) {
      valid = false;
      WARN_SYSTEMF("[HIER-DEBUG] MultiNet update failed");
    } else {
      DEBUG_SRF("[HIER-DEBUG] MultiNet update successful");
    }
  }
  unlockMN();
  
  INFO_SRF("[HIER] ===== Loaded %d unique categories =====", loaded);
  DEBUG_SRF("[HIER-DEBUG] ========== loadCategories() END ========== ");
  return valid;
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
  DEBUG_SRF("[HIER-DEBUG] categoryHasSubCategories('%s')", category);
  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (r.cat && r.sub && routeAlive(r) &&
        normalizePhrase(r.cat) == normCategory &&
        r.sub[0] != '\0') {
      DEBUG_SRF("[HIER-DEBUG]   Found subcategory: '%s' -> cmd='%s'", r.sub, r.cli);
      return true;
    }
  }
  return false;
}

// Check if a category has direct targets (2-level hierarchy, no subcategory)
static bool categoryHasDirectTargets(const char* category) {
  String normCategory = normalizePhrase(category);
  DEBUG_SRF("[HIER-DEBUG] categoryHasDirectTargets('%s')", category);
  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (r.cat && r.target && routeAlive(r) &&
        normalizePhrase(r.cat) == normCategory &&
        r.target[0] != '\0' &&
        (!r.sub || r.sub[0] == '\0')) {
      DEBUG_SRF("[HIER-DEBUG]   Found direct target: '%s' -> cmd='%s'", r.target, r.cli);
      return true;
    }
  }
  return false;
}

// Check if a category has any targets (either direct or via subcategory)
static bool categoryHasTargets(const char* category) {
  String normCategory = normalizePhrase(category);
  DEBUG_SRF("[HIER-DEBUG] categoryHasTargets('%s') -> normalized='%s'", category, normCategory.c_str());
  int targetCount = 0;
  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (r.cat && r.target && routeAlive(r) &&
        normalizePhrase(r.cat) == normCategory && r.target[0] != '\0') {
      targetCount++;
      DEBUG_SRF("[HIER-DEBUG]   Found target: '%s' -> cmd='%s'", r.target, r.cli);
    }
  }
  DEBUG_SRF("[HIER-DEBUG] categoryHasTargets('%s') = %s (found %d targets)",
           normCategory.c_str(), targetCount > 0 ? "true" : "false", targetCount);
  return targetCount > 0;
}

// Load sub-categories for a category into MultiNet (for 3-level hierarchy)
static bool loadSubCategoriesForCategory(const char* category) {
  DEBUG_SRF("[HIER-DEBUG] loadSubCategoriesForCategory('%s')", category);
  
  if (!mnCommandsReady()) {
    WARN_SYSTEMF("[HIER-DEBUG] loadSubCategoriesForCategory: MultiNet not ready!");
    return false;
  }
  if (!lockMN(5000)) {
    WARN_SYSTEMF("[HIER-DEBUG] loadSubCategoriesForCategory: Failed to lock MultiNet!");
    return false;
  }
  
  if (esp_mn_commands_clear() != ESP_OK) { unlockMN(); return false; }
  clearVoiceCliMappings();
  
  // Always add special phrases (cancel, help) for abort/help capability
  bool valid = addSpecialPhrases();
  
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

    // Globals and stage phrases share one list, independent of command IDs.
    if (findLoadedVoicePhrase(r.sub)) continue;

    esp_err_t err = esp_mn_commands_add(nextId, r.sub);
    if (err == ESP_OK) {
      addVoiceCliMapping(nextId, r.sub);
      DEBUG_SRF("[HIER-DEBUG]   Added subcategory: id=%d phrase='%s'", nextId, r.sub);
      nextId++;
      loaded++;
    } else {
      valid = false;
    }
  }
  
  if (loaded > 0) {
    if (!mnUpdateLocked()) {
      valid = false;
      WARN_SYSTEMF("[HIER-DEBUG] MultiNet update failed");
    }
  }
  unlockMN();
  
  INFO_SRF("[HIER] Loaded %d subcategories for '%s'", loaded, category);
  return valid && loaded > 0;
}

// Load targets for a specific category+subcategory combination
static bool loadTargetsForCategorySubCategory(const char* category, const char* subCategory) {
  DEBUG_SRF("[HIER-DEBUG] loadTargetsForCategorySubCategory('%s', '%s')", category, subCategory);
  
  if (!mnCommandsReady()) {
    WARN_SYSTEMF("[HIER-DEBUG] loadTargetsForCategorySubCategory: MultiNet not ready!");
    return false;
  }
  if (!lockMN(5000)) {
    WARN_SYSTEMF("[HIER-DEBUG] loadTargetsForCategorySubCategory: Failed to lock MultiNet!");
    return false;
  }
  
  if (esp_mn_commands_clear() != ESP_OK) { unlockMN(); return false; }
  clearVoiceCliMappings();
  
  // Always add special phrases (cancel, help) for abort/help capability
  bool valid = addSpecialPhrases();
  
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

    if (esp_mn_phrase_t* existing = findLoadedVoicePhrase(r.target)) {
      valid = loadedTargetMatches(existing, r.cli) && valid;
      continue;
    }
    esp_err_t err = esp_mn_commands_add(nextId, r.target);
    if (err == ESP_OK) {
      addVoiceCliMapping(nextId, r.cli);
      DEBUG_SRF("[HIER-DEBUG]   Added target: id=%d phrase='%s' -> cli='%s'",
               nextId, r.target, r.cli);
      nextId++;
      loaded++;
    } else {
      valid = false;
    }
  }
  
  if (loaded > 0) {
    if (!mnUpdateLocked()) {
      valid = false;
      WARN_SYSTEMF("[HIER-DEBUG] MultiNet update failed");
    }
  }
  unlockMN();
  
  INFO_SRF("[HIER] Loaded %d targets for '%s'->'%s'", loaded, category, subCategory);
  return valid && loaded > 0;
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
  DEBUG_SRF("[HIER-DEBUG] findCommandForSingleStageCategory('%s') -> normalized='%s'", category, normCategory.c_str());
  for (size_t i = 0; i < kVoiceRouteCount; i++) {
    const VoiceRoute& r = kVoiceRoutes[i];
    if (r.cat && routeAlive(r) && normalizePhrase(r.cat) == normCategory) {
      // Return the first route with this category (assumes single-stage has one command)
      if (!r.target || r.target[0] == '\0') {
        DEBUG_SRF("[HIER-DEBUG]   Found single-stage: cmd='%s'", r.cli);
        return r.cli;
      }
    }
  }
  DEBUG_SRF("[HIER-DEBUG]   No single-stage command found for category '%s'", normCategory.c_str());
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

static void voiceTableLoadFailed() {
  srSetError("Speech command table failed; stop and restart speech recognition");
  gVoiceState = VoiceState::IDLE;
  gESPSRWakeDetected = false;
  gSRFeedShouldRun.store(false, std::memory_order_release);
  gESPSRRunning.store(false, std::memory_order_release);
  broadcastOutput("[Voice] Command table unavailable; stop and restart speech recognition.");
}

static void onVoiceCommandDetected(int commandId, const char* phrase) {
  DEBUG_SRF("[HIER-DEBUG] ########## onVoiceCommandDetected() ##########");
  DEBUG_SRF("[HIER-DEBUG] commandId=%d, phrase='%s'", commandId, phrase ? phrase : "(null)");
  DEBUG_SRF("[HIER-DEBUG] Current state: %s", voiceStateToString(gVoiceState));
  DEBUG_SRF("[HIER-DEBUG] Current category: '%s', subcategory: '%s'",
           gCurrentCategory.c_str(), gCurrentSubCategory.c_str());
  
  const char* mappedValue = findCliCommandForId(commandId);
  DEBUG_SRF("[HIER-DEBUG] Mapped value from ID: '%s'", mappedValue ? mappedValue : "(null)");
  
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
    if (!loadCategories()) voiceTableLoadFailed();
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
      DEBUG_SRF("[HIER-DEBUG] Category has subcategories -> transitioning to AWAIT_SUBCATEGORY");
      gCurrentCategory = category;
      gCurrentSubCategory = "";
      gVoiceState = VoiceState::AWAIT_SUBCATEGORY;
      gCategoryTimeoutMs = millis() + gSettings.srCommandTimeout;
      
      if (!loadSubCategoriesForCategory(category)) { voiceTableLoadFailed(); return; }
      INFO_SRF("[HIER] Now listening for SUBCATEGORY... (timeout in %d ms)", gSettings.srCommandTimeout);
      
      // User-facing feedback
      String normCat = normalizePhrase(category);
      broadcastOutput(String("[Voice] ") + normCat + "... which one?");
      
    } else if (categoryHasDirectTargets(category)) {
      // 2-level: category -> target (no subcategory)
      DEBUG_SRF("[HIER-DEBUG] Category has direct targets -> transitioning to AWAIT_TARGET");
      gCurrentCategory = category;
      gCurrentSubCategory = "";
      gVoiceState = VoiceState::AWAIT_TARGET;
      gCategoryTimeoutMs = millis() + gSettings.srCommandTimeout;
      
      if (!loadTargetsForCategory(category)) { voiceTableLoadFailed(); return; }
      INFO_SRF("[HIER] Now listening for TARGET... (timeout in %d ms)", gSettings.srCommandTimeout);
      
      // User-facing feedback
      String normCat = normalizePhrase(category);
      broadcastOutput(String("[Voice] ") + normCat + "... what action?");
      
    } else {
      // Single-stage: execute immediately
      DEBUG_SRF("[HIER-DEBUG] Category has NO targets/subcategories -> single-stage execution");
      const char* cliCmd = findCommandForSingleStageCategory(category);
      if (cliCmd) {
        INFO_SRF("[HIER] Single-stage command -> CLI: %s", cliCmd);
        String normCat = normalizePhrase(category);
        broadcastOutput(String("[Voice] Queuing ") + normCat + ".");
        
        char cmdOut[128];
        bool ok = executeVoiceCommandAsArmedUser(cliCmd, cmdOut, sizeof(cmdOut));
        INFO_SRF("[HIER] Result: %s", ok ? cmdOut : cmdOut);
        if (!ok) {
          broadcastOutput(String("[Voice] Not queued: ") + cmdOut);
        }
#if ENABLE_OLED_DISPLAY
        if (!ok) {
          char vmsg[32];
          snprintf(vmsg, sizeof(vmsg), "Voice: %s", normCat.c_str());
          oledNotificationBannerShow(vmsg, PairingRibbonIcon::ERROR_ICON, 2000);
        }
#endif
        gLastCommand = category;
      } else {
        WARN_SYSTEMF("[HIER] Category '%s' has no associated command!", category);
        broadcastOutput("[Voice] Sorry, I don't know how to do that.");
      }
      // Return to idle
      gVoiceState = VoiceState::IDLE;
      gCurrentCategory = "";
      gCurrentSubCategory = "";
      if (!loadCategories()) voiceTableLoadFailed();
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
    
    if (!loadTargetsForCategorySubCategory(gCurrentCategory.c_str(), subCategory)) { voiceTableLoadFailed(); return; }
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
    DEBUG_SRF("[HIER-DEBUG] CLI command from mapping: '%s'", cliCmd ? cliCmd : "(null)");
    
    if (cliCmd) {
      INFO_SRF("[HIER] EXECUTING: %s", cliCmd);
      
      // User-facing feedback
      String normTarget = normalizePhrase(target);
      if (gCurrentSubCategory.length() > 0) {
        String normSubCat = normalizePhrase(gCurrentSubCategory.c_str());
        broadcastOutput(String("[Voice] Queuing ") + normSubCat + " " + normTarget + ".");
        gLastCommand = gCurrentCategory + " " + gCurrentSubCategory + " " + target;
      } else {
        String normCat = normalizePhrase(gCurrentCategory.c_str());
        broadcastOutput(String("[Voice] Queuing ") + normCat + " " + normTarget + ".");
        gLastCommand = gCurrentCategory + " " + target;
      }
      
      char cmdOut[128];
      bool ok = executeVoiceCommandAsArmedUser(cliCmd, cmdOut, sizeof(cmdOut));
      INFO_SRF("[HIER] RESULT: %s", ok ? cmdOut : cmdOut);
      if (!ok) {
        broadcastOutput(String("[Voice] Not queued: ") + cmdOut);
      }
#if ENABLE_OLED_DISPLAY
      if (!ok) {
        char vmsg[32];
        snprintf(vmsg, sizeof(vmsg), "Voice: %s", normTarget.c_str());
        oledNotificationBannerShow(vmsg, PairingRibbonIcon::ERROR_ICON, 2000);
      }
#endif
    } else {
      WARN_SYSTEMF("[HIER] No CLI command found for '%s'->'%s'->'%s'!", 
                   gCurrentCategory.c_str(), gCurrentSubCategory.c_str(), target);
      broadcastOutput("[Voice] Sorry, I don't understand that.");
    }
    
    // Return to idle, reload categories
    DEBUG_SRF("[HIER-DEBUG] Returning to IDLE, reloading categories...");
    gVoiceState = VoiceState::IDLE;
    gCurrentCategory = "";
    gCurrentSubCategory = "";
    if (!loadCategories()) voiceTableLoadFailed();
    
  } else {
    // Fallback: direct command execution (shouldn't happen in hierarchical mode)
    WARN_SYSTEMF("[HIER-DEBUG] Unexpected state: %s - falling back to direct execution", 
                 voiceStateToString(gVoiceState));
    if (mappedValue) {
      INFO_SRF("Voice command %d ('%s') -> CLI: %s", commandId, phrase ? phrase : "", mappedValue);
      char cmdOut[128];
      bool ok = executeVoiceCommandAsArmedUser(mappedValue, cmdOut, sizeof(cmdOut));
      INFO_SRF("CLI result: %s", ok ? cmdOut : cmdOut);
      if (!ok) {
        broadcastOutput(String("[Voice] Not queued: ") + cmdOut);
      }
    } else {
      INFO_SRF("Voice command %d ('%s') has no CLI mapping", commandId, phrase ? phrase : "");
    }
  }
  
  DEBUG_SRF("[HIER-DEBUG] ########## onVoiceCommandDetected() END ##########");
}

// Forward declarations
static bool initI2SMicrophone();
static void deinitI2SMicrophone();
static bool initAFE();
static void deinitAFE();
static bool initMultiNet();
static bool deinitMultiNet();
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
        ps_free(job.pcm);
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
      ps_free(job.pcm);
      uint32_t durationMs = (job.samples * 1000) / job.sample_rate;
      uint32_t bitrate = (job.sample_rate * job.bits * job.channels) / 1000;
      INFO_SRF("SnipWriter: saved %s (%u samples, %u ms, %u kbps, %u bytes written)",
               fname, (unsigned)job.samples, (unsigned)durationMs, (unsigned)bitrate, (unsigned)written);
    }
  }
}

static bool srSnipInitRingBuffer() {
  SRSnipGuard guard;
  if (!guard) return false;
  if (gSrSnipRing) return true;
  size_t preSamples = (I2S_SR_SAMPLE_RATE * gSrSnipPreMs) / 1000;
  gSrSnipRingSamples = preSamples;
  gSrSnipRing = (int16_t*)ps_alloc(gSrSnipRingSamples * sizeof(int16_t),
                                   AllocPref::RequirePSRAM, "sr.snip.ring");
  if (!gSrSnipRing) {
    ERROR_SRF("Failed to allocate PSRAM snippet ring buffer (%u samples); snippet capture disabled",
              (unsigned)gSrSnipRingSamples);
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
  SRSnipGuard guard;
  if (!guard) return;
  if (gSrSnipRing) {
    ps_free(gSrSnipRing);
    gSrSnipRing = nullptr;
  }
  gSrSnipRingSamples = 0;
  gSrSnipRingHead = 0;
}

static void srSnipRingPush(const int16_t* samples, size_t count) {
  SRSnipGuard guard;
  if (!guard) return;
  if (!gSrSnipRing || gSrSnipRingSamples == 0 || !samples || count == 0) return;
  for (size_t i = 0; i < count; ++i) {
    gSrSnipRing[gSrSnipRingHead] = samples[i];
    gSrSnipRingHead = (gSrSnipRingHead + 1) % gSrSnipRingSamples;
  }
}

static void srSnipStartSession(const char* reason, int cmdId, const char* phrase) {
  SRSnipGuard guard;
  if (!guard) return;
  if (gSrSnipSessionActive) {
    SR_DBG_L(1, "SnipSession: already active, ending previous");
    srSnipEndSession(true);
  }
  size_t maxSamples = (I2S_SR_SAMPLE_RATE * gSrSnipMaxMs) / 1000;
  gSrSnipSessionBuf = (int16_t*)ps_alloc(maxSamples * sizeof(int16_t),
                                         AllocPref::RequirePSRAM,
                                         "sr.snip.session");
  if (!gSrSnipSessionBuf) {
    ERROR_SRF("SnipSession: failed to allocate PSRAM session buffer (%u samples); speech remains active",
              (unsigned)maxSamples);
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
  SRSnipGuard guard;
  if (!guard) return;
  if (!gSrSnipSessionActive || !gSrSnipSessionBuf || !samples || count == 0) return;
  if (static_cast<int32_t>(millis() - gSrSnipSessionDeadlineMs) >= 0) {
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
  SRSnipGuard guard;
  if (!guard) return;
  if (!gSrSnipSessionActive) return;
  gSrSnipSessionActive = false;
  if (!save || gSrSnipSessionSamplesWritten == 0 || !gSrSnipSessionBuf) {
    SR_DBG_L(1, "SnipSession: ended without saving (save=%d, samples=%u)", save, (unsigned)gSrSnipSessionSamplesWritten);
    if (gSrSnipSessionBuf) {
      ps_free(gSrSnipSessionBuf);
      gSrSnipSessionBuf = nullptr;
    }
    return;
  }
  if (!gSrSnipQueue) {
    WARN_SRF("SnipSession: no queue, discarding");
    ps_free(gSrSnipSessionBuf);
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
    ps_free(job.pcm);
  } else {
    SR_DBG_L(1, "SnipSession: queued %u samples for writing", (unsigned)job.samples);
  }
}

static bool srSnipInit() {
  SRSnipGuard guard;
  if (!guard) return false;
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
  SRSnipGuard guard;
  if (!guard) return;
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
      if (job.pcm) ps_free(job.pcm);
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
           (unsigned)gSrMnDetectCalls, (unsigned)gSrMnDetected, (unsigned)gCommandCount.load(), gLastCommand.c_str());
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
  return ensureSRMutex(gMNCommandMutex);
}

static bool lockMN(uint32_t timeoutMs) {
  if (!ensureMNCommandMutex() ||
      xSemaphoreTake(gMNCommandMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
  // OLED stop can race a CLI command that tested readiness before taking this
  // lock. Recheck the handles while teardown is excluded.
  if (!gMNModel || !gMNData) {
    xSemaphoreGive(gMNCommandMutex);
    return false;
  }
  return true;
}

static void unlockMN() {
  if (!gMNCommandMutex) return;
  xSemaphoreGive(gMNCommandMutex);
}

static bool mnCommandsReady() {
  if (!gMNModel || !gMNData) return false;
  if (!ensureMNCommandMutex()) return false;
  if (!lockMN(1000)) return false;
  if (!gMNCommandsAllocated) {
    gMNCommandsAllocated = esp_mn_commands_alloc(gMNModel, gMNData) == ESP_OK;
  }
  const bool ready = gMNCommandsAllocated;
  unlockMN();
  return ready;
}

static bool mnTableUpdateFailed(const char* reason) {
  gMnTableTrusted = false;
  srSetError(reason);
  gSRFeedShouldRun.store(false, std::memory_order_release);
  gESPSRRunning.store(false, std::memory_order_release);
  return false;
}

struct MNCommandSnapshot {
  int commandId;
  float threshold;
  const char* text;
  const char* phonemes;
};

static bool mnUsesCommandFileGrammar() {
  return strncmp(gMnModelName, "mn6", 3) == 0 || strncmp(gMnModelName, "mn7", 3) == 0;
}

static srmodel_data_t* findMNCommandFile(int& fileIndex) {
  fileIndex = -1;
  if (!gSRModels || !gSRModels->model_name || !gSRModels->model_data) return nullptr;
  for (int model = 0; model < gSRModels->num; ++model) {
    if (!gSRModels->model_name[model] || strcmp(gSRModels->model_name[model], "fst") != 0) continue;
    srmodel_data_t* files = gSRModels->model_data[model];
    if (!files || !files->files || !files->data || !files->sizes) return nullptr;
    for (int file = 0; file < files->num; ++file) {
      if (files->files[file] && strcmp(files->files[file], "commands_en.txt") == 0) {
        fileIndex = file;
        return files;
      }
    }
  }
  return nullptr;
}

// Caller holds the MN mutex, or the startup lifecycle before any worker exists.
// Only the public auxiliary-file descriptor changes. AFE's weights and the
// original packed bytes stay intact. ESP-SR copies this CSV and its phrases
// during create(), so the caller can free it after the descriptor is restored.
static model_iface_data_t* createMNWithCommandFile(const char* csv, size_t bytes) {
  int file = -1;
  srmodel_data_t* files = findMNCommandFile(file);
  if (!files || !csv || !bytes || !csv[0] || bytes > 131072) {
    srSetError("Speech bundle needs fst/commands_en.txt and a nonempty command table");
    return nullptr;
  }
  char* originalData = files->data[file];
  const int originalSize = files->sizes[file];
  files->data[file] = const_cast<char*>(csv);
  files->sizes[file] = static_cast<int>(bytes);
  model_iface_data_t* model = gMNModel->create(gMnModelName, gSettings.srCommandTimeout);
  files->data[file] = originalData;
  files->sizes[file] = originalSize;
  return model;
}

static bool mnUpdateLocked() {
  if (!mnUsesCommandFileGrammar()) {
    const esp_mn_error_t* errors = esp_mn_commands_update();
    if (errors && errors->num > 0) return mnTableUpdateFailed("MultiNet rejected speech commands; restart SR");
    gMNGrammarPublished = esp_mn_commands_get_from_index(0) != nullptr;
    return true;
  }
  // ESP-SR 2.5.5 MN6/7 leak ID-sized buffers when updating an existing graph.
  // They also publish a bundled demonstration graph during create(), and an
  // empty demonstration makes the vendor create wrapper dereference NULL.
  // Supply the desired grammar to create itself, exactly once per instance.
  int file = -1;
  if (!findMNCommandFile(file)) {
    return mnTableUpdateFailed("Speech bundle is missing fst/commands_en.txt; rebuild the bundle");
  }
  size_t count = 0;
  size_t stringBytes = 0;
  size_t csvBytes = 1;
  for (int i = 0; ; ++i) {
    const esp_mn_phrase_t* phrase = esp_mn_commands_get_from_index(i);
    if (!phrase) break;
    if (count >= ESP_MN_MAX_PHRASE_NUM || !phrase->string || !phrase->string[0] ||
        phrase->command_id <= 0 || phrase->command_id > kSRMaxCommandId || phrase->wave) {
      return mnTableUpdateFailed("Speech command table cannot be copied; restart SR");
    }
    const size_t textBytes = strlen(phrase->string) + 1;
    const size_t phonemeBytes = phrase->phonemes ? strlen(phrase->phonemes) + 1 : 0;
    // The vendor parses unquoted CSV with a 128-byte line buffer. Validate
    // before destroying the current model; do not silently truncate a phrase.
    if (strpbrk(phrase->string, ",\r\n") ||
        (phrase->phonemes && (!phrase->phonemes[0] || strpbrk(phrase->phonemes, ",\r\n")))) {
      return mnTableUpdateFailed("Speech phrase contains unsupported CSV characters; restart SR");
    }
    const size_t lineBytes = static_cast<size_t>(snprintf(nullptr, 0, "%d", phrase->command_id)) +
        textBytes + phonemeBytes + 1; // separators and newline, excluding final NUL
    if (lineBytes >= 128) {
      return mnTableUpdateFailed("Speech phrase exceeds the MultiNet command-file line limit; restart SR");
    }
    stringBytes += textBytes + phonemeBytes;
    csvBytes += lineBytes;
    ++count;
  }
  if (count == 0) {
    // Keep the current model solely for validating later CLI additions. Its
    // old graph must never run, and no unsafe empty create/update is attempted.
    gMNGrammarPublished = false;
    return true;
  }

  const size_t bytes = count * sizeof(MNCommandSnapshot) + stringBytes + csvBytes;
  void* storage = ps_alloc(bytes, AllocPref::PreferPSRAM, "sr.mn.commands");
  if (!storage) return mnTableUpdateFailed("Not enough memory to replace speech commands; restart SR");
  auto* commands = static_cast<MNCommandSnapshot*>(storage);
  char* cursor = reinterpret_cast<char*>(commands + count);
  char* csv = cursor + stringBytes;
  size_t csvUsed = 0;
  for (size_t i = 0; i < count; ++i) {
    const esp_mn_phrase_t* phrase = esp_mn_commands_get_from_index(static_cast<int>(i));
    commands[i].commandId = phrase->command_id;
    commands[i].threshold = phrase->threshold;
    const size_t textBytes = strlen(phrase->string) + 1;
    memcpy(cursor, phrase->string, textBytes);
    commands[i].text = cursor;
    cursor += textBytes;
    commands[i].phonemes = nullptr;
    if (phrase->phonemes) {
      const size_t phonemeBytes = strlen(phrase->phonemes) + 1;
      memcpy(cursor, phrase->phonemes, phonemeBytes);
      commands[i].phonemes = cursor;
      cursor += phonemeBytes;
    }
    const int written = snprintf(csv + csvUsed, csvBytes - csvUsed, "%d,%s%s%s\n",
        commands[i].commandId, commands[i].text,
        commands[i].phonemes ? "," : "", commands[i].phonemes ? commands[i].phonemes : "");
    if (written <= 0 || static_cast<size_t>(written) >= csvBytes - csvUsed) {
      free(storage);
      return mnTableUpdateFailed("Failed to serialize speech commands; restart SR");
    }
    csvUsed += static_cast<size_t>(written);
  }

  gMNModel->destroy(gMNData);
  gMNData = nullptr;
  gMNCommandsAllocated = false;
  gMNGrammarPublished = false;
  gMNData = createMNWithCommandFile(csv, csvUsed);
  if (!gMNData) {
    free(storage);
    return mnTableUpdateFailed("Failed to recreate speech recognizer; stop and restart SR");
  }
  // create owns/populates the public list. Reallocating it here would discard
  // the canonical phrases, and calling commands_update would leak again.
  gMNCommandsAllocated = true;
  if (gMNModel->get_samp_rate(gMNData) != I2S_SR_SAMPLE_RATE ||
      (gSRFetchSamples && gMNModel->get_samp_chunksize(gMNData) != static_cast<int>(gSRFetchSamples))) {
    free(storage);
    return mnTableUpdateFailed("Recreated speech recognizer has invalid configuration; restart SR");
  }
  for (size_t i = 0; i < count; ++i) {
    const auto& command = commands[i];
    esp_mn_phrase_t* restored = esp_mn_commands_get_from_index(static_cast<int>(i));
    if (!restored || restored->command_id != command.commandId || !restored->string ||
        strcmp(restored->string, command.text) != 0 ||
        (command.phonemes && (!restored->phonemes || strcmp(restored->phonemes, command.phonemes) != 0))) {
      free(storage);
      return mnTableUpdateFailed("Recreated speech command table does not match; restart SR");
    }
    restored->threshold = command.threshold;
  }
  if (esp_mn_commands_get_from_index(static_cast<int>(count))) {
    free(storage);
    return mnTableUpdateFailed("Recreated speech command table contains unexpected phrases; restart SR");
  }
  free(storage);
  gMNGrammarPublished = true;
  return true;
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
  } else {
    audioSetSource(AUDIO_SRC_NONE);  // auto/unavailable preference: resolve afresh
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

static void deinitSRModels() {
  if (gSRModels) {
    if (gSRModelBlob) srmodel_host_deinit(gSRModels);
    else esp_srmodel_deinit(gSRModels);
    gSRModels = nullptr;
  }
  // srmodel_load retains pointers into this buffer. Free only after MN, AFE,
  // and the vendor model list have all been destroyed.
  free(gSRModelBlob);
  gSRModelBlob = nullptr;
  gSRModelBytes = 0;
  gSRModelSource = "none";
}

static bool loadSRModelFile(const char* path) {
  File file = VFS::openGuarded(path, "r", VFS::systemAuth("espsr.models_load"));
  if (!file) {
    srSetError("Speech model bundle is missing or unreadable");
    return false;
  }
  size_t bytes = 0;
  {
    FsLockGuard guard("espsr.models.size");
    bytes = file.size();
  }
  if (bytes == 0 || bytes > ESPSRModelPack::kMaxFileBytes) {
    file.close();
    srSetError("Speech model bundle has an invalid size");
    return false;
  }
  void* blob = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!blob) {
    file.close();
    srSetError("Not enough PSRAM to load speech model bundle");
    return false;
  }
  size_t read = 0;
  while (read < bytes) {
    const size_t chunk = (bytes - read > 16384) ? 16384 : bytes - read;
    size_t got;
    {
      FsLockGuard guard("espsr.models.read");
      got = file.read(static_cast<uint8_t*>(blob) + read, chunk);
    }
    if (got == 0 || got > chunk) break;
    read += got;
    taskYIELD();
  }
  bool unchanged;
  {
    FsLockGuard guard("espsr.models.close");
    unchanged = file.size() == bytes;
    file.close();
  }
  const char* error = nullptr;
  if (read != bytes || !unchanged || !ESPSRModelPack::validate(blob, bytes, &error)) {
    free(blob);
    srSetError("Speech model bundle is truncated or malformed");
    ERROR_SRF("Bundle %s: %s", path, error ? error : "incomplete read");
    return false;
  }
  gSRModels = srmodel_load(blob);
  if (!gSRModels) {
    free(blob);
    srSetError("Speech model loader failed");
    return false;
  }
  gSRModelBlob = blob;
  gSRModelBytes = bytes;
  gSRModelSource = path;
  INFO_SRF("Loaded model bundle %s (%u bytes)", path, (unsigned)bytes);
  return true;
}

static bool initSRModels() {
  if (get_static_srmodels()) {
    srSetError("Speech model list is already owned by another subsystem");
    return false;
  }
  if (gSettings.srModelSource == 1 || gSettings.srModelSource == 2) {
    const char* path = gSettings.srModelSource == 1 ?
        "/sd/ESP-SR Models/srmodels.bin" : "/ESP-SR Models/srmodels.bin";
    if (loadSRModelFile(path)) return true;
    WARN_SRF("Configured model source %s failed; trying model partition", path);
    logSystemEvent("SR", "configured model file unavailable; trying partition fallback");
  }
  // The vendor initializer asserts when the partition is absent. Check first.
  if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model")) {
    srSetError("No speech model partition; install srmodels.bin and select SD or LittleFS");
    return false;
  }
  gSRModels = esp_srmodel_init("model");
  if (!gSRModels || gSRModels->num <= 0) {
    srSetError("No speech models available in model partition");
    return false;
  }
  gSRModelSource = "partition:model";
  gSRError.store(nullptr, std::memory_order_release); // fallback succeeded
  INFO_SRF("Using speech models from %s", gSRModelSource);
  return true;
}

static bool initAFE() {
  if (!initSRModels()) return false;
  char* wnName = esp_srmodel_filter(gSRModels, ESP_WN_PREFIX, nullptr);
  if (!wnName) {
    srSetError("Speech bundle has no WakeNet model");
    return false;
  }
  afe_config_t* config = afe_config_init("M", gSRModels, AFE_TYPE_SR, AFE_MODE_LOW_COST);
  if (!config) {
    srSetError("Failed to allocate speech front-end configuration");
    return false;
  }
  config->wakenet_model_name = wnName;
  config->aec_init = false; // no playback reference channel
  config->se_init = false;  // microphone-array enhancement needs multiple mics
  config->ns_init = false;  // ESP-SR recommends disabling NS for ASR accuracy
  config->vad_init = true;
  config->wakenet_init = true;
  config->afe_ringbuf_size = 50;
  config->afe_linear_gain = clampFloat(gSettings.srAfeGain, 0.1f, 10.0f);
  config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  // Preserve the existing setting's target levels, using the new AGC fields.
  config->agc_init = gSettings.srAgcMode != 0;
  config->agc_mode = AFE_AGC_MODE_WAKENET;
  config->agc_target_level_dbfs = gSettings.srAgcMode == 1 ? 9 :
                                gSettings.srAgcMode == 3 ? 3 : 6;
  config->vad_mode = static_cast<vad_mode_t>(gSettings.srVadMode);
  gAFE = esp_afe_handle_from_config(config);
  if (gAFE) gAFEData = gAFE->create_from_config(config);
  afe_config_free(config);
  if (!gAFEData) {
    srSetError("Failed to create speech front end");
    return false;
  }
  strlcpy(gWnModelName, wnName, sizeof(gWnModelName));
  INFO_SRF("Speech front end ready: %s, mono 16 kHz", gWnModelName);
  return true;
}

static void deinitAFE() {
  if (gAFE && gAFEData) gAFE->destroy(gAFEData);
  gAFEData = nullptr;
  gAFE = nullptr;
}

static bool initMultiNet() {
  char* mnName = esp_srmodel_filter(gSRModels, ESP_MN_PREFIX, ESP_MN_ENGLISH);
  if (!mnName || !(gMNModel = esp_mn_handle_from_name(mnName))) {
    srSetError("Speech bundle has no supported English MultiNet model");
    return false;
  }
  strlcpy(gMnModelName, mnName, sizeof(gMnModelName));
  // A valid nonempty bootstrap avoids ESP-SR's unsafe empty-create path. This
  // is never recognized: loadCategories replaces it before workers start.
  static const char bootstrap[] = "1,TELL ME A JOKE,TfL Mm c qbK\n";
  gMNData = mnUsesCommandFileGrammar() ? createMNWithCommandFile(bootstrap, sizeof(bootstrap) - 1) :
                                       gMNModel->create(mnName, gSettings.srCommandTimeout);
  if (!gMNData || !mnCommandsReady()) {
    srSetError("Failed to initialize speech command recognizer");
    return false;
  }
  strlcpy(gMnModelName, mnName, sizeof(gMnModelName));
  INFO_SRF("MultiNet initialized: %s", gMnModelName);
  return true;
}

static bool deinitMultiNet() {
  if (gMNModel && gMNData) {
    if (!lockMN(2000)) {
      srSetError("Speech command recognizer is still busy; retry stop");
      return false;
    }
    // ESP-SR 2.5.5 MultiNet destroy owns the command list too. Freeing it
    // separately would make destroy clear an already-freed list.
    gMNModel->destroy(gMNData);
    gMNCommandsAllocated = false;
    gMNGrammarPublished = false;
    gMNData = nullptr;
    gMNModel = nullptr;
    unlockMN();
  }
  gMNModel = nullptr;
  gMNGrammarPublished = false;
  return true;
}

// ============================================================================
// Speech Recognition Task
// ============================================================================

static void srFeedTask(void*) {
  size_t filled = 0;
  uint32_t lastAudioMs = millis();
  while (gSRFeedShouldRun.load(std::memory_order_acquire)) {
    const size_t got = audioReadPcm(gSRFeedBuffer + filled, gSRFeedSamples - filled, 100);
    if (!gSRFeedShouldRun.load(std::memory_order_acquire)) break;
    if (got == 0) {
      ++gSrI2SReadZero;
      if (audioGetSource() == AUDIO_SRC_G2_LEFT) ++gSrG2ReadZero;
      if (strcmp(audioCaptureOwner(), "sr") != 0 || millis() - lastAudioMs >= 5000) {
        srSetError("Microphone stream stopped; stop and restart speech recognition");
        gSRFeedShouldRun.store(false, std::memory_order_release);
        gESPSRRunning.store(false, std::memory_order_release);
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    if (got > gSRFeedSamples - filled) {
      srSetError("Microphone returned more samples than requested");
      gESPSRRunning.store(false, std::memory_order_release);
      break;
    }
    lastAudioMs = millis();
    ++gSrI2SReadOk;
    gSrI2SBytesOk += got * sizeof(int16_t);
    if (audioGetSource() == AUDIO_SRC_G2_LEFT) gSrG2BytesOk += got * sizeof(int16_t);
    {
      SRSnipGuard guard;
      if (guard) {
        if (gSrSnipEnabled) srSnipRingPush(gSRFeedBuffer + filled, got);
        srSnipFeedSession(gSRFeedBuffer + filled, got);
      }
    }
    filled += got;
    if (filled < gSRFeedSamples) continue; // retain short HAL reads, never pad/drop
    int16_t minimum = 32767, maximum = -32768;
    int64_t sumAbs = 0;
    for (size_t i = 0; i < filled; ++i) {
      const int32_t value = gSRFeedBuffer[i];
      if (value < minimum) minimum = value;
      if (value > maximum) maximum = value;
      sumAbs += value < 0 ? -value : value;
    }
    gSrLastPcmMin = minimum;
    gSrLastPcmMax = maximum;
    gSrLastPcmAbsAvg = static_cast<float>(sumAbs) / filled;
    // Glasses already supply processed PCM; board-mic gain/filter settings
    // must not amplify or re-filter their stream.
    if (audioGetSource() == AUDIO_SRC_LOCAL_PDM) {
      applyMicAudioProcessing(gSRFeedBuffer, filled, getMicSoftwareGainMultiplier(), gSrFiltersEnabled);
    }
    if (gAFE->feed(gAFEData, gSRFeedBuffer) <= 0) {
      srSetError("Speech front end rejected microphone audio");
      gESPSRRunning.store(false, std::memory_order_release);
      break;
    }
    ++gSrAfeFeedOk;
    filled = 0;
  }
  gSRFeedShouldRun.store(false, std::memory_order_release);
  // After this acknowledgement the worker must never touch pipeline resources.
  gSRFeedExited.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

static void srTask(void*) {
  int16_t* mnInputBuf = gSRMNBuffer;
  const size_t mnBufSamplesCap = gSRFetchSamples;
  bool listeningForCommand = false;
  uint32_t commandTimeoutMs = 0;
  bool commandSpeechStarted = false;
  while (gSRTaskShouldRun.load(std::memory_order_acquire)) {
    gSRStackHwm.store(uxTaskGetStackHighWaterMark(nullptr), std::memory_order_relaxed);
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


    // Fetch independently of microphone feed: ESP-SR 2.x buffers internally,
    // and waiting for output must never prevent new input from reaching AFE.
    afe_fetch_result_t* fetchResult = gAFE->fetch_with_delay(gAFEData, pdMS_TO_TICKS(100));
    if (!gSRTaskShouldRun.load(std::memory_order_acquire)) break;
    if (gSRFeedExited.load(std::memory_order_acquire) && gSRError.load()) break;
    if (!fetchResult) continue;
    gSrLastAfeRetValue = fetchResult->ret_value;
    if (fetchResult->ret_value != ESP_OK) continue;
    if (!fetchResult->data || fetchResult->data_size != static_cast<int>(gSRFetchSamples * sizeof(int16_t))) {
      srSetError("Speech front end returned an invalid PCM frame");
      gESPSRRunning.store(false, std::memory_order_release);
      gSRFeedShouldRun.store(false, std::memory_order_release);
      // Keep draining until the feeder acknowledges stop, then exit safely.
      if (gSRFeedExited.load(std::memory_order_acquire)) break;
      continue;
    }
    ++gSrAfeFetchOk;
    gSrLastVolumeDb = fetchResult->data_volume;
    gSrLastVadState = (int)fetchResult->vad_state;
    gSrLastAfeTriggerChannel = fetchResult->trigger_channel_id;

    SR_DBG_L(4, "AFE fetch: vol=%.1f dB, vad=%d, wake_state=%d, ret=%d",
             fetchResult->data_volume, (int)fetchResult->vad_state,
             (int)fetchResult->wakeup_state, fetchResult->ret_value);

    if (fetchResult->wakeup_state == WAKENET_DETECTED) {
      // A repeated wake aborts the old hierarchy. Reload categories before
      // accepting more PCM, otherwise a target table would be read as categories.
      if (gVoiceState != VoiceState::IDLE && !loadCategories()) {
        srSetError("Could not reset speech command table after wake word");
        gESPSRRunning.store(false, std::memory_order_release);
        gSRFeedShouldRun.store(false, std::memory_order_release);
        continue;
      }
      if (lockMN(50)) {
        gMNModel->clean(gMNData);
        unlockMN();
      }
      gWakeWordCount++;
      gLastWakeMs = millis();
      gESPSRWakeDetected = true;
      listeningForCommand = true;
      commandSpeechStarted = false;  // Reset - waiting for user to start speaking command
      commandTimeoutMs = millis() + gSettings.srCommandTimeout;
      gSrLastWakeWordIndex = fetchResult->wake_word_index;
      gSrLastWakeNetModelIndex = fetchResult->wakenet_model_index;

      // Transition to hierarchical state machine
      DEBUG_SRF("[HIER-DEBUG] State transition: %s -> AWAIT_CATEGORY", voiceStateToString(gVoiceState));
      gVoiceState = VoiceState::AWAIT_CATEGORY;
      gCurrentCategory = "";
      gCurrentSubCategory = "";

      INFO_SRF("[HIER] ============================================");
      INFO_SRF("[HIER] WAKE WORD DETECTED!");
      INFO_SRF("[HIER] ============================================");
      INFO_SRF("[HIER] Listening for CATEGORY... (timeout in %d ms)", gSettings.srCommandTimeout);

      // User-facing feedback
      broadcastOutput("");
      broadcastOutput("[Voice] Yes?");
      systemEventPost(SYSEVT_VOICE_WAKE);
      DEBUG_SRF("[HIER-DEBUG] Voice CLI mappings count: %u", (unsigned)gVoiceCliMappingCount);
      INFO_SRF("Wake stats: count=%u, idx=%d, model=%d, vol=%.1f dB, wake_len=%d",
               gWakeWordCount, fetchResult->wake_word_index, fetchResult->wakenet_model_index,
               fetchResult->data_volume, fetchResult->wake_word_length);

      if (gSrSnipEnabled && !gSrSnipSessionActive) {
        srSnipStartSession("wake", -1, nullptr);
      }

      if (gWakeWordCallback) {
        char* words = esp_srmodel_get_wake_words(gSRModels, gWnModelName);
        gWakeWordCallback(words ? words : gWnModelName);
        free(words); // ESP-SR allocates the returned wake-word string.
      }
    }

    if (listeningForCommand && gMNModel && gMNData) {
      // Extend timeout when user starts speaking their command
      if (!commandSpeechStarted && fetchResult->vad_state == VAD_SPEECH) {
        commandSpeechStarted = true;
        commandTimeoutMs = millis() + gSettings.srCommandTimeout;  // Fresh timeout from speech start
        SR_DBG_L(1, "Speech detected - timeout extended to %d ms from now", gSettings.srCommandTimeout);
      }

      if (static_cast<int32_t>(millis() - commandTimeoutMs) >= 0) {
        DEBUG_SRF("[HIER-DEBUG] ===== TIMEOUT TRIGGERED =====");
        DEBUG_SRF("[HIER-DEBUG] Current state: %s", voiceStateToString(gVoiceState));
        DEBUG_SRF("[HIER-DEBUG] Current category: '%s'", gCurrentCategory.c_str());
        DEBUG_SRF("[HIER-DEBUG] Time since wake: %u ms", (unsigned)(millis() - gLastWakeMs));

        if (lockMN(50)) {
          gMNModel->clean(gMNData);
          unlockMN();
        }

        // Handle timeout based on current state
        if (gVoiceState == VoiceState::AWAIT_CATEGORY) {
          INFO_SRF("[HIER] ============================================");
          INFO_SRF("[HIER] TIMEOUT: No category detected");
          INFO_SRF("[HIER] ============================================");
          DEBUG_SRF("[HIER-DEBUG] State transition: AWAIT_CATEGORY -> IDLE");

          // User-facing feedback
          broadcastOutput("[Voice] Sorry, I didn't catch that.");

          gVoiceState = VoiceState::IDLE;
          gCurrentCategory = "";
          gCurrentSubCategory = "";
        } else if (gVoiceState == VoiceState::AWAIT_SUBCATEGORY) {
          INFO_SRF("[HIER] ============================================");
          INFO_SRF("[HIER] TIMEOUT: No subcategory detected for '%s'", gCurrentCategory.c_str());
          INFO_SRF("[HIER] ============================================");
          DEBUG_SRF("[HIER-DEBUG] State transition: AWAIT_SUBCATEGORY -> IDLE");

          // User-facing feedback
          broadcastOutput(String("[Voice] Timed out waiting for ") + gCurrentCategory + " selection.");

          gVoiceState = VoiceState::IDLE;
          gCurrentCategory = "";
          gCurrentSubCategory = "";
          // Reload categories for next wake word
          DEBUG_SRF("[HIER-DEBUG] Reloading categories after subcategory timeout...");
          if (!loadCategories()) voiceTableLoadFailed();
        } else if (gVoiceState == VoiceState::AWAIT_TARGET) {
          INFO_SRF("[HIER] ============================================");
          INFO_SRF("[HIER] TIMEOUT: No target detected for '%s'->'%s'",
                   gCurrentCategory.c_str(), gCurrentSubCategory.c_str());
          INFO_SRF("[HIER] ============================================");
          DEBUG_SRF("[HIER-DEBUG] State transition: AWAIT_TARGET -> IDLE");

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
          DEBUG_SRF("[HIER-DEBUG] Reloading categories after target timeout...");
          if (!loadCategories()) voiceTableLoadFailed();
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
        if (!gMNGrammarPublished || gSRError.load(std::memory_order_acquire)) {
          unlockMN();
          continue;
        }
        bool mnLocked = true;
        gSrMnDetectCalls++;

        const bool isCategoryStageNow = (gVoiceState == VoiceState::AWAIT_CATEGORY);
        const bool isSubCategoryStageNow = (gVoiceState == VoiceState::AWAIT_SUBCATEGORY);
        const bool isTargetStageNow = (gVoiceState == VoiceState::AWAIT_TARGET);
        const bool speechOkNow = (!gSrTargetRequireSpeech) || isCategoryStageNow || isSubCategoryStageNow || commandSpeechStarted || (fetchResult->vad_state == VAD_SPEECH);
        if ((isTargetStageNow || isSubCategoryStageNow) && gSrTargetRequireSpeech && !speechOkNow) {
          if (mnLocked) {
            unlockMN();
          }
          continue;
        }

        const bool dynGainOkNow = (fetchResult->vad_state == VAD_SPEECH) || commandSpeechStarted;

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
                     stageStr, fetchResult->vad_state, fetchResult->data_volume);
          }
        }

        if (mnState == ESP_MN_STATE_DETECTED) {
          gSrMnDetected++;
          esp_mn_results_t* results = gMNModel->get_results(gMNData);
          if (results && results->num > 0) {
            int cmdId = results->command_id[0];
            char cmdPhraseCopy[128];
            if (!copyRecognizedVoicePhrase(cmdId, cmdPhraseCopy, sizeof(cmdPhraseCopy))) {
              WARN_SRF("Ignoring recognition with unknown or oversized phrase id=%d", cmdId);
              gMNModel->clean(gMNData);
              unlockMN();
              continue;
            }
            const char* cmdPhrase = cmdPhraseCopy;
            float cmdProb = results->prob[0];

            const bool isCategoryStage = (gVoiceState == VoiceState::AWAIT_CATEGORY);
            const bool isSubCategoryStage = (gVoiceState == VoiceState::AWAIT_SUBCATEGORY);
            // Use category confidence for both category and subcategory stages
            const float requiredConfidence = (isCategoryStage || isSubCategoryStage) ? gSrMinCategoryConfidence : gSrMinCommandConfidence;
            const float cmdProb2 = (results->num > 1) ? results->prob[1] : 0.0f;
            const bool speechOk = (!gSrTargetRequireSpeech) || isCategoryStage || isSubCategoryStage || commandSpeechStarted || (fetchResult->vad_state == VAD_SPEECH);
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
              gLastCommand = cmdPhrase ? cmdPhrase : String(cmdId);
              gLastConfidence = cmdProb;

              {
                SRSnipGuard guard;
                if (guard && gSrSnipSessionActive) {
                  gSrSnipSessionCmdId = cmdId;
                  strlcpy(gSrSnipSessionPhrase, cmdPhrase ? cmdPhrase : "", sizeof(gSrSnipSessionPhrase));
                  srSnipEndSession(true);
                }
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
            if (!loadCategories()) voiceTableLoadFailed();
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
  srSnipEndSession(false);
  gSRStackHwm.store(uxTaskGetStackHighWaterMark(nullptr), std::memory_order_relaxed);
  gSRTaskExited.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

// ============================================================================
// Public API
// ============================================================================

void initESPSR() {
  ensureSRMutex(gSRLifecycleMutex);
  ensureSRMutex(gSRSnipMutex, true);
  ensureVoiceArmMutex();
  if (!gSRLifecycleMutex || !gSRSnipMutex || !gVoiceArmMutex) {
    srSetError("Failed to allocate speech synchronization primitives");
    return;
  }
  SRLifecycleGuard guard;
  if (!guard || gESPSRInitialized) return;
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

static bool srWaitForWorker(const std::atomic<bool>& exited, uint32_t timeoutMs) {
  const uint32_t started = millis();
  while (!exited.load(std::memory_order_acquire)) {
    if (millis() - started >= timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}

// Caller holds lifecycle mutex. AFE fetch stays alive until feed has stopped,
// so a feeder blocked on AFE backpressure can finish during shutdown.
static bool stopESPSRLocked() {
  gESPSRRunning.store(false, std::memory_order_release);
  gSRFeedShouldRun.store(false, std::memory_order_release);
  if (!srWaitForWorker(gSRFeedExited, 5000)) {
    srSetError("Speech feeder is still stopping; resources retained, retry stop");
    return false;
  }
  gSRTaskShouldRun.store(false, std::memory_order_release);
  if (!srWaitForWorker(gSRTaskExited, 5000)) {
    srSetError("Speech detector is still stopping; resources retained, retry stop");
    return false;
  }
  gSRFeedTaskHandle = nullptr;
  gSRTaskHandle = nullptr;
  if (!deinitMultiNet()) return false;
  deinitAFE();
  deinitSRModels();
  free(gSRFeedBuffer);
  free(gSRMNBuffer);
  gSRFeedBuffer = gSRMNBuffer = nullptr;
  gSRFeedSamples = gSRFetchSamples = 0;
  deinitI2SMicrophone();
  gWnModelName[0] = gMnModelName[0] = '\0';
  gESPSRWakeDetected = false;
  gVoiceState = VoiceState::IDLE;
  gCurrentCategory = "";
  gCurrentSubCategory = "";
  restoreMicrophoneAfterSRIfNeeded();
  return true;
}

static void prepareSRAudioProcessing() {
  // SR claims the HAL directly, so it must restore the saved gain that the
  // microphone sensor would normally apply in initMicrophone().
  if (gSettings.microphoneGain >= 0 && gSettings.microphoneGain <= 100) {
    micGain = gSettings.microphoneGain;
  }
  resetMicAudioProcessingState();
}

bool startESPSR() {
  initESPSR();
  SRLifecycleGuard guard;
  if (!guard) return false;
  if (gESPSRRunning.load(std::memory_order_acquire)) return true;
  if (gSRTaskHandle || gSRFeedTaskHandle || gAFEData || gSRModels) {
    srSetError("Previous speech pipeline needs a completed stop before restart");
    return false;
  }
  gSRError.store(nullptr, std::memory_order_release);
#if ENABLE_MICROPHONE
  if (gMicRunning || micRecordingBusy()) {
    gRestoreMicAfterSR = gMicRunning;
    if (!stopMicrophone()) {
      gRestoreMicAfterSR = false;
      srSetError("Microphone recorder did not reach idle");
      return false;
    }
  }
#endif
  if (!initI2SMicrophone() || !initAFE() || !initMultiNet()) {
    if (!gSRError.load()) srSetError("Failed to claim microphone for speech");
    stopESPSRLocked();
    return false;
  }
  const int feed = gAFE->get_feed_chunksize(gAFEData);
  const int fetch = gAFE->get_fetch_chunksize(gAFEData);
  const int mn = gMNModel->get_samp_chunksize(gMNData);
  if (feed <= 0 || feed > 4096 || fetch <= 0 || fetch > 4096 || fetch != mn ||
      gAFE->get_samp_rate(gAFEData) != I2S_SR_SAMPLE_RATE ||
      gAFE->get_feed_channel_num(gAFEData) != 1 ||
      gAFE->get_fetch_channel_num(gAFEData) != 1 ||
      gMNModel->get_samp_rate(gMNData) != I2S_SR_SAMPLE_RATE || !gAFE->fetch_with_delay) {
    srSetError("Speech models require incompatible PCM rate, channels, or frame sizes");
    stopESPSRLocked();
    return false;
  }
  gSRFeedSamples = feed;
  gSRFetchSamples = fetch;
  gSrAfeFeedChunk = feed;
  gSrAfeFetchChunk = fetch;
  gSRFeedBuffer = static_cast<int16_t*>(ps_alloc(feed * sizeof(int16_t), AllocPref::PreferPSRAM, "sr.afe.feed"));
  gSRMNBuffer = static_cast<int16_t*>(ps_alloc(fetch * sizeof(int16_t), AllocPref::PreferPSRAM, "sr.mn.input"));
  if (!gSRFeedBuffer || !gSRMNBuffer) {
    srSetError("Failed to allocate speech PCM buffers");
    stopESPSRLocked();
    return false;
  }
  // Configure command tables, callbacks and DSP before either worker can run.
  prepareSRAudioProcessing();
  gSrDynGainCurrent = 1.0f;
  gVoiceState = VoiceState::IDLE;
  gCurrentCategory = "";
  gCurrentSubCategory = "";
  if (!loadCategories()) {
    srSetError("Speech command table could not be loaded");
    stopESPSRLocked();
    return false;
  }
  setESPSRCommandCallback(onVoiceCommandDetected);
  gSRTaskShouldRun.store(true, std::memory_order_release);
  gSRTaskExited.store(false, std::memory_order_release);
  taskStackRecord("sr_task", SR_STACK_WORDS);
  if (xTaskCreatePinnedToCore(srTask, "sr_task", SR_STACK_WORDS, nullptr,
      SR_TASK_PRIORITY_LEVEL, &gSRTaskHandle, 1) != pdPASS) {
    gSRTaskExited.store(true, std::memory_order_release);
    srSetError("Failed to create speech detection task");
    stopESPSRLocked();
    return false;
  }
  gSRFeedShouldRun.store(true, std::memory_order_release);
  gSRFeedExited.store(false, std::memory_order_release);
  taskStackRecord("sr_feed", SR_STACK_WORDS);
  if (xTaskCreatePinnedToCore(srFeedTask, "sr_feed", SR_STACK_WORDS, nullptr,
      SR_TASK_PRIORITY_LEVEL, &gSRFeedTaskHandle, 1) != pdPASS) {
    gSRFeedExited.store(true, std::memory_order_release);
    srSetError("Failed to create speech capture task");
    stopESPSRLocked();
    return false;
  }
  gESPSRRunning.store(true, std::memory_order_release);
  if (gSRError.load(std::memory_order_acquire)) {
    stopESPSRLocked();
    return false;
  }
  INFO_SRF("Speech pipeline started: %s / %s (%s)", gWnModelName, gMnModelName, gSRModelSource);
  return true;
}

bool stopESPSR() {
  initESPSR();
  SRLifecycleGuard guard;
  if (!guard) return false;
  // Invalidate queued recognition before waiting for either worker.
  if (!gVoiceArmMutex || xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  voiceDisarmInternal();
  xSemaphoreGive(gVoiceArmMutex);
  return stopESPSRLocked();
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
  doc["running"] = gESPSRRunning.load();
  doc["stopping"] = !gESPSRRunning.load() && (gSRTaskHandle || gSRFeedTaskHandle || gAFEData);
  doc["lastError"] = gSRError.load() ? gSRError.load() : "";
  doc["modelSource"] = gSRModelSource;
  doc["modelBytes"] = gSRModelBytes;
  doc["wakeActive"] = gESPSRWakeDetected;
  doc["state"] = getESPSRVoiceState();
  doc["category"] = gCurrentCategory;
  doc["subcategory"] = gCurrentSubCategory;
  doc["wakeCount"] = gWakeWordCount;
  doc["commandCount"] = gCommandCount.load();
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
  // ESP-IDF task stack depths and high-water marks are bytes, unlike vanilla
  // FreeRTOS. Preserve the legacy Words fields as derived 32-bit word counts.
  doc["srTaskStackAllocWords"] = (uint32_t)SR_STACK_WORDS / sizeof(uint32_t);
  doc["srTaskStackAllocBytes"] = (uint32_t)SR_STACK_WORDS;
  if (gESPSRRunning && gSRTaskHandle) {
    const uint32_t hwmBytes = gSRStackHwm.load(std::memory_order_relaxed);
    doc["srTaskStackHwmWords"] = hwmBytes / sizeof(uint32_t);
    doc["srTaskStackHwmBytes"] = hwmBytes;
    doc["srTaskStackPeakUsedBytesEst"] =
        (SR_STACK_WORDS > hwmBytes) ? (uint32_t)(SR_STACK_WORDS - hwmBytes) : 0u;
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
  if (!ok) {
    static String error;
    error = String("Error: ") + (gSRError.load() ? gSRError.load() : "speech lifecycle is busy");
    return error.c_str();
  }

  ensureVoiceArmMutex();
  bool armed = false;
  if (gVoiceArmMutex && xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    armed = voiceArmFromContextInternal(currentAuthContext());
    xSemaphoreGive(gVoiceArmMutex);
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
  if (!stopESPSR()) return "Error: speech stop incomplete; resources retained, retry srstop";

  ensureVoiceArmMutex();
  if (gVoiceArmMutex && xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    voiceDisarmInternal();
    xSemaphoreGive(gVoiceArmMutex);
  } else {
    return "Error: voice authority busy; retry";
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
    return "Error: voice authority busy; retry";
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
  const UBaseType_t hwm = gSRStackHwm.load(std::memory_order_relaxed);
  const uint32_t allocB = (uint32_t)SR_STACK_WORDS;
  const uint32_t freeB = (uint32_t)hwm;
  const uint32_t usedEst =
      (SR_STACK_WORDS > hwm) ? (uint32_t)(SR_STACK_WORDS - hwm) : 0u;
  snprintf(buf, sizeof(buf),
           "sr_task: alloc=%uB est_peak_used=%uB hwm_free=%uB. "
           "Shrink SR_STACK_WORDS in System_TaskUtils.h only if hwm_free stays "
           ">= ~25%% of alloc after your worst-case voice test.",
           (unsigned)allocB, (unsigned)usedEst, (unsigned)freeB);
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
  if (id <= 0 || id > kSRMaxCommandId) return "Error: id must be 1-1023";

  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: srstart";
  }
  if (!lockMN(4000)) {
    return "Error: busy";
  }

  esp_err_t err = esp_mn_commands_add(id, phrase.c_str());
  bool updated = true;
  if (err == ESP_OK) {
    updated = mnUpdateLocked();
  }
  unlockMN();

  if (err != ESP_OK) {
    return "Error: failed to add command";
  }
  if (!updated) {
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
  bool updated = true;
  if (phrase && strlen(phrase) > 0) {
    err = esp_mn_commands_remove(phrase);
    if (err == ESP_OK) {
      updated = mnUpdateLocked();
    }
  }
  unlockMN();

  if (err != ESP_OK) {
    return "Error: command not found";
  }
  if (!updated) {
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
  bool updated = true;
  if (err == ESP_OK) {
    updated = mnUpdateLocked();
  }
  unlockMN();

  if (err != ESP_OK) return "Error: failed";
  if (!updated) return "Error: MultiNet rejected one or more commands";
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

  // The clear must precede the load (loadCommandsFileLocked appends), so a failed
  // load leaves the LIVE table empty. Mark it untrusted for the duration so
  // `sr cmds save` cannot serialise that emptiness over the commands file.
  gMnTableTrusted = false;
  esp_mn_commands_clear();
  size_t added = 0;
  size_t parseErrors = 0;
  bool ok = loadCommandsFileLocked(added, parseErrors);
  if (ok && parseErrors == 0) gMnTableTrusted = true;
  const bool updated = mnUpdateLocked();
  unlockMN();

  if (!ok) return "Error: failed to read commands file (is SD mounted?)";
  if (!updated || parseErrors > 0) {
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
  if (!gMnTableTrusted) {
    unlockMN();
    return "Error: the live command table did not load cleanly - refusing to overwrite the "
           "commands file. Reload with `sr cmds reload`, or fix the file first.";
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
  if (!loadCategories()) {
    voiceTableLoadFailed();
    return "Error: failed to sync voice command table";
  }
  
  // Register the command callback to execute CLI commands
  setESPSRCommandCallback(onVoiceCommandDetected);
  
  // Count how many categories were loaded
  size_t added = 0;
  if (lockMN(2000)) {
    for (int i = 0; ; i++) {
      esp_mn_phrase_t* phrase = esp_mn_commands_get_from_index(i);
      if (!phrase) break;
      if (phrase->command_id < GLOBAL_VOICE_CMD_ID_START) added++;
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
    static char buf[256];
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
    snprintf(buf, sizeof(buf), "VAD mode: %d (0-4, higher=more restrictive)\nUsage: sr tuning vad <0-4>", gSettings.srVadMode);
    return buf;
  }
  
  int val = args.toInt();
  if (val < 0 || val > 4) {
    return "Error: vad must be 0-4";
  }
  
  setSetting(gSettings.srVadMode, val);
  static char buf[80];
  snprintf(buf, sizeof(buf), "VAD mode set to %d (higher rejects more noise). Restart SR to apply.", val);
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
