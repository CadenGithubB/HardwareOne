#include "System_ESPSR.h"

#if ENABLE_ESP_SR

#include "System_Debug.h"
#include "System_VFS.h"
#include "System_BuildConfig.h"
#include "System_Microphone.h"
#include "System_Mutex.h"
#include "System_Command.h"
#include "System_CLI.h"
#include "System_Auth.h"
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

// Debug tags - use DEBUG_MICROPHONE flag and system logging macros
#define TAG_SR "ESP_SR"
#define DEBUG_SRF(fmt, ...) DEBUG_MICF("[%s] " fmt, TAG_SR, ##__VA_ARGS__)
#define INFO_SRF(fmt, ...)  do { if (getLogLevel() >= LOG_LEVEL_INFO) DEBUGF_QUEUE(0xFFFFFFFF, "[INFO][SYS] [SR] " fmt, ##__VA_ARGS__); } while (0)
#define WARN_SRF(fmt, ...)  WARN_SYSTEMF("[SR] " fmt, ##__VA_ARGS__)
#define ERROR_SRF(fmt, ...) ERROR_SYSTEMF("[SR] " fmt, ##__VA_ARGS__)

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

// Task configuration
#define SR_TASK_STACK_SIZE  (8 * 1024)
#define SR_TASK_PRIORITY    5
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
static esp_afe_sr_data_t* gAFEData = nullptr;
static model_iface_data_t* gMNData = nullptr;
static const esp_mn_iface_t* gMNModel = nullptr;
static SemaphoreHandle_t gMNCommandMutex = nullptr;
static bool gMNCommandsAllocated = false;

// I2S handle
static i2s_chan_handle_t gI2SRxHandle = nullptr;
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

extern AuthContext gExecAuthContext;
extern void broadcastOutput(const char* msg);
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
    default: return "unknown";
  }
}

static void voiceDisarmInternal() {
  gVoiceArmed = false;
  gVoiceArmedUser = "";
  gVoiceArmedByTransport = SOURCE_INTERNAL;
  gVoiceArmedByIp = "";
  gVoiceArmedAtMs = 0;
}

static bool voiceArmFromContextInternal(const AuthContext& ctx) {
  if (ctx.transport == SOURCE_INTERNAL) return false;
  if (ctx.user.length() == 0) return false;

  gVoiceArmed = true;
  gVoiceArmedUser = ctx.user;
  gVoiceArmedByTransport = ctx.transport;
  gVoiceArmedByIp = ctx.ip;
  gVoiceArmedAtMs = millis();
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

  AuthContext vctx;
  vctx.transport = SOURCE_VOICE;
  vctx.user = user;
  vctx.ip = "voice";
  vctx.path = "/voice";
  vctx.sid = "";
  vctx.opaque = nullptr;

  return executeCommand(vctx, cliCmd, out, outSize);
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

// Reserved ID range for global voice commands (voiceCategory="*")
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

#define SR_DBG_L(lvl, fmt, ...) do { if (gSrDebugLevel >= (lvl)) { DEBUG_SRF(fmt, ##__VA_ARGS__); } } while (0)
#define SR_INFO_L(lvl, fmt, ...) do { if (gSrDebugLevel >= (lvl)) { INFO_SRF(fmt, ##__VA_ARGS__); } } while (0)

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

// Add global voice commands (voiceCategory="*") to current MultiNet command set
// These are available at all stages (category, subcategory, target)
// Call after clearing commands, before adding stage-specific phrases
static void addSpecialPhrases() {
  int globalId = GLOBAL_VOICE_CMD_ID_START;  // Start from reserved ID range
  
  for (size_t i = 0; i < gCommandsCount; i++) {
    const CommandEntry* entry = gCommands[i];
    if (!entry || !entry->voiceCategory || !entry->voiceTarget) continue;
    
    // Check for global marker
    if (strcmp(entry->voiceCategory, "*") != 0) continue;
    
    esp_err_t err = esp_mn_commands_add(globalId, entry->voiceTarget);
    if (err == ESP_OK) {
      addVoiceCliMapping(globalId, entry->name);  // Map to CLI command name, not voice phrase
      INFO_SRF("[HIER-DEBUG] Added global phrase: id=%d phrase='%s' -> cli='%s'", 
               globalId, entry->voiceTarget, entry->name);
    } else {
      WARN_SYSTEMF("[HIER-DEBUG] Failed to add global phrase '%s': err=0x%x", 
                   entry->voiceTarget, err);
    }
    globalId++;
  }
}

// Load targets for a specific category into MultiNet
// Returns true if any targets were loaded, false if category has no targets (single-stage)
static bool loadTargetsForCategory(const char* category) {
  INFO_SRF("[HIER-DEBUG] loadTargetsForCategory('%s') called", category);
  INFO_SRF("[HIER-DEBUG]   Total commands in registry: %u", (unsigned)gCommandsCount);
  
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
  
  // Find all commands with this category and load their targets
  INFO_SRF("[HIER-DEBUG] Scanning registry for category '%s' (normalized='%s') targets...", category, normCategory.c_str());
  for (size_t i = 0; i < gCommandsCount && nextId < MAX_VOICE_CLI_MAPPINGS; i++) {
    const CommandEntry* entry = gCommands[i];
    scanned++;
    
    if (!entry) continue;
    if (!entry->voiceCategory) continue;
    
    if (normalizePhrase(entry->voiceCategory) == normCategory) {
      categoryMatches++;
      INFO_SRF("[HIER-DEBUG]   Found category match: cmd='%s' target='%s'", 
               entry->name, entry->voiceTarget ? entry->voiceTarget : "(null)");
      
      if (entry->voiceTarget && entry->voiceTarget[0] != '\0') {
        esp_err_t err = esp_mn_commands_add(nextId, entry->voiceTarget);
        if (err == ESP_OK) {
          addVoiceCliMapping(nextId, entry->name);  // Map target ID to CLI command
          INFO_SRF("[HIER-DEBUG]   ✓ Added to MultiNet: id=%d phrase='%s' -> cli='%s'", 
                   nextId, entry->voiceTarget, entry->name);
          nextId++;
          loaded++;
        } else {
          WARN_SYSTEMF("[HIER-DEBUG]   ✗ Failed to add '%s': err=0x%x", entry->voiceTarget, err);
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
  INFO_SRF("[HIER-DEBUG] Total commands in registry: %u", (unsigned)gCommandsCount);
  
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
  INFO_SRF("[HIER-DEBUG] Scanning registry for unique categories...");
  for (size_t i = 0; i < gCommandsCount && nextId < MAX_VOICE_CLI_MAPPINGS; i++) {
    const CommandEntry* entry = gCommands[i];
    scanned++;
    
    if (!entry) continue;
    if (!entry->voiceCategory || entry->voiceCategory[0] == '\0') continue;
    // Skip global commands (voiceCategory="*") - already handled by addSpecialPhrases()
    if (strcmp(entry->voiceCategory, "*") == 0) continue;
    
    withVoice++;
    INFO_SRF("[HIER-DEBUG]   [%u] cmd='%s' category='%s' target='%s'", 
             (unsigned)i, entry->name, entry->voiceCategory, 
             entry->voiceTarget ? entry->voiceTarget : "(null)");
    
    // Check for duplicates
    bool exists = false;
    for (int j = 0; j < nextId - 1; j++) {
      esp_mn_phrase_t* existing = esp_mn_commands_get_from_index(j);
      if (existing && existing->string && strcmp(existing->string, entry->voiceCategory) == 0) {
        exists = true;
        break;
      }
    }
    if (exists) {
      INFO_SRF("[HIER-DEBUG]     ^ Category '%s' already added (duplicate)", entry->voiceCategory);
      duplicates++;
      continue;
    }
    
    esp_err_t err = esp_mn_commands_add(nextId, entry->voiceCategory);
    if (err == ESP_OK) {
      addVoiceCliMapping(nextId, entry->voiceCategory);  // Map to category name
      INFO_SRF("[HIER-DEBUG]     ✓ Added category to MultiNet: id=%d phrase='%s'", nextId, entry->voiceCategory);
      nextId++;
      loaded++;
    } else {
      WARN_SYSTEMF("[HIER-DEBUG]     ✗ Failed to add category '%s': err=0x%x", entry->voiceCategory, err);
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
  for (size_t i = 0; i < gCommandsCount; i++) {
    const CommandEntry* entry = gCommands[i];
    if (entry && entry->voiceCategory && entry->voiceTarget &&
        normalizePhrase(entry->voiceCategory) == normCategory &&
        normalizePhrase(entry->voiceTarget) == normTarget) {
      return entry->name;
    }
  }
  return nullptr;
}

// Check if a category has sub-categories (3-level hierarchy)
static bool categoryHasSubCategories(const char* category) {
  String normCategory = normalizePhrase(category);
  INFO_SRF("[HIER-DEBUG] categoryHasSubCategories('%s')", category);
  for (size_t i = 0; i < gCommandsCount; i++) {
    const CommandEntry* entry = gCommands[i];
    if (entry && entry->voiceCategory && entry->voiceSubCategory &&
        normalizePhrase(entry->voiceCategory) == normCategory &&
        entry->voiceSubCategory[0] != '\0') {
      INFO_SRF("[HIER-DEBUG]   Found subcategory: '%s' -> cmd='%s'", entry->voiceSubCategory, entry->name);
      return true;
    }
  }
  return false;
}

// Check if a category has direct targets (2-level hierarchy, no subcategory)
static bool categoryHasDirectTargets(const char* category) {
  String normCategory = normalizePhrase(category);
  INFO_SRF("[HIER-DEBUG] categoryHasDirectTargets('%s')", category);
  for (size_t i = 0; i < gCommandsCount; i++) {
    const CommandEntry* entry = gCommands[i];
    if (entry && entry->voiceCategory && entry->voiceTarget &&
        normalizePhrase(entry->voiceCategory) == normCategory &&
        entry->voiceTarget[0] != '\0' &&
        (!entry->voiceSubCategory || entry->voiceSubCategory[0] == '\0')) {
      INFO_SRF("[HIER-DEBUG]   Found direct target: '%s' -> cmd='%s'", entry->voiceTarget, entry->name);
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
  for (size_t i = 0; i < gCommandsCount; i++) {
    const CommandEntry* entry = gCommands[i];
    if (entry && entry->voiceCategory && entry->voiceTarget &&
        normalizePhrase(entry->voiceCategory) == normCategory && entry->voiceTarget[0] != '\0') {
      targetCount++;
      INFO_SRF("[HIER-DEBUG]   Found target: '%s' -> cmd='%s'", entry->voiceTarget, entry->name);
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
  for (size_t i = 0; i < gCommandsCount && nextId < MAX_VOICE_CLI_MAPPINGS; i++) {
    const CommandEntry* entry = gCommands[i];
    if (!entry || !entry->voiceCategory || !entry->voiceSubCategory) continue;
    if (normalizePhrase(entry->voiceCategory) != normCategory) continue;
    if (entry->voiceSubCategory[0] == '\0') continue;
    
    // Check for duplicates
    bool exists = false;
    for (int j = 0; j < nextId - 1; j++) {
      esp_mn_phrase_t* existing = esp_mn_commands_get_from_index(j);
      if (existing && existing->string && strcmp(existing->string, entry->voiceSubCategory) == 0) {
        exists = true;
        break;
      }
    }
    if (exists) continue;
    
    esp_err_t err = esp_mn_commands_add(nextId, entry->voiceSubCategory);
    if (err == ESP_OK) {
      addVoiceCliMapping(nextId, entry->voiceSubCategory);
      INFO_SRF("[HIER-DEBUG]   Added subcategory: id=%d phrase='%s'", nextId, entry->voiceSubCategory);
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
  
  for (size_t i = 0; i < gCommandsCount && nextId < MAX_VOICE_CLI_MAPPINGS; i++) {
    const CommandEntry* entry = gCommands[i];
    if (!entry || !entry->voiceCategory || !entry->voiceSubCategory || !entry->voiceTarget) continue;
    if (normalizePhrase(entry->voiceCategory) != normCategory) continue;
    if (normalizePhrase(entry->voiceSubCategory) != normSubCategory) continue;
    if (entry->voiceTarget[0] == '\0') continue;
    
    esp_err_t err = esp_mn_commands_add(nextId, entry->voiceTarget);
    if (err == ESP_OK) {
      addVoiceCliMapping(nextId, entry->name);
      INFO_SRF("[HIER-DEBUG]   Added target: id=%d phrase='%s' -> cli='%s'", 
               nextId, entry->voiceTarget, entry->name);
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
  for (size_t i = 0; i < gCommandsCount; i++) {
    const CommandEntry* entry = gCommands[i];
    if (entry && entry->voiceCategory && entry->voiceSubCategory && entry->voiceTarget &&
        normalizePhrase(entry->voiceCategory) == normCategory &&
        normalizePhrase(entry->voiceSubCategory) == normSubCategory &&
        normalizePhrase(entry->voiceTarget) == normTarget) {
      return entry->name;
    }
  }
  return nullptr;
}

// Find the CLI command for a single-stage category (no target required)
static const char* findCommandForSingleStageCategory(const char* category) {
  String normCategory = normalizePhrase(category);
  INFO_SRF("[HIER-DEBUG] findCommandForSingleStageCategory('%s') -> normalized='%s'", category, normCategory.c_str());
  for (size_t i = 0; i < gCommandsCount; i++) {
    const CommandEntry* entry = gCommands[i];
    if (entry && entry->voiceCategory && normalizePhrase(entry->voiceCategory) == normCategory) {
      // Return the first command with this category (assumes single-stage has one command)
      if (!entry->voiceTarget || entry->voiceTarget[0] == '\0') {
        INFO_SRF("[HIER-DEBUG]   Found single-stage: cmd='%s'", entry->name);
        return entry->name;
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
    Serial.printf("\033[1;33m[Voice] Cancelled.\033[0m\n");
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
      Serial.printf("\033[1;35m[Voice Help] Say a category:\033[0m\n");
      for (size_t i = 0; i < gCommandsCount; i++) {
        const CommandEntry* e = gCommands[i];
        if (e && e->voiceCategory && e->voiceCategory[0] != '\0' && strcmp(e->voiceCategory, "*") != 0) {
          bool dup = false;
          for (size_t j = 0; j < i && !dup; j++) {
            if (gCommands[j] && gCommands[j]->voiceCategory &&
                normalizePhrase(gCommands[j]->voiceCategory) == normalizePhrase(e->voiceCategory)) dup = true;
          }
          if (!dup) Serial.printf("  - %s\n", e->voiceCategory);
        }
      }
    } else if (gVoiceState == VoiceState::AWAIT_SUBCATEGORY) {
      Serial.printf("\033[1;35m[Voice Help] %s - which one?\033[0m\n", gCurrentCategory.c_str());
      String normCat = normalizePhrase(gCurrentCategory.c_str());
      for (size_t i = 0; i < gCommandsCount; i++) {
        const CommandEntry* e = gCommands[i];
        if (e && e->voiceCategory && e->voiceSubCategory &&
            normalizePhrase(e->voiceCategory) == normCat && e->voiceSubCategory[0] != '\0') {
          bool dup = false;
          for (size_t j = 0; j < i && !dup; j++) {
            if (gCommands[j] && gCommands[j]->voiceSubCategory &&
                normalizePhrase(gCommands[j]->voiceSubCategory) == normalizePhrase(e->voiceSubCategory)) dup = true;
          }
          if (!dup) Serial.printf("  - %s\n", e->voiceSubCategory);
        }
      }
    } else if (gVoiceState == VoiceState::AWAIT_TARGET) {
      String normCat = normalizePhrase(gCurrentCategory.c_str());
      if (gCurrentSubCategory.length() > 0) {
        Serial.printf("\033[1;35m[Voice Help] %s %s - what action?\033[0m\n", 
                      gCurrentCategory.c_str(), gCurrentSubCategory.c_str());
        String normSub = normalizePhrase(gCurrentSubCategory.c_str());
        for (size_t i = 0; i < gCommandsCount; i++) {
          const CommandEntry* e = gCommands[i];
          if (e && e->voiceCategory && e->voiceSubCategory && e->voiceTarget &&
              normalizePhrase(e->voiceCategory) == normCat &&
              normalizePhrase(e->voiceSubCategory) == normSub && e->voiceTarget[0] != '\0') {
            Serial.printf("  - %s\n", e->voiceTarget);
          }
        }
      } else {
        Serial.printf("\033[1;35m[Voice Help] %s - what action?\033[0m\n", gCurrentCategory.c_str());
        for (size_t i = 0; i < gCommandsCount; i++) {
          const CommandEntry* e = gCommands[i];
          if (e && e->voiceCategory && e->voiceTarget &&
              normalizePhrase(e->voiceCategory) == normCat &&
              (!e->voiceSubCategory || e->voiceSubCategory[0] == '\0') && e->voiceTarget[0] != '\0') {
            Serial.printf("  - %s\n", e->voiceTarget);
          }
        }
      }
    } else {
      Serial.printf("\033[1;35m[Voice Help] Say the wake word first.\033[0m\n");
    }
    Serial.printf("  - cancel, help\n");
    
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
      Serial.printf("\033[1;36m[Voice] %s... which one?\033[0m\n", normCat.c_str());
      
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
      Serial.printf("\033[1;36m[Voice] %s... what action?\033[0m\n", normCat.c_str());
      
    } else {
      // Single-stage: execute immediately
      INFO_SRF("[HIER-DEBUG] Category has NO targets/subcategories -> single-stage execution");
      const char* cliCmd = findCommandForSingleStageCategory(category);
      if (cliCmd) {
        INFO_SRF("[HIER] Single-stage command -> CLI: %s", cliCmd);
        String normCat = normalizePhrase(category);
        Serial.printf("\033[1;32m[Voice] OK, %s.\033[0m\n", normCat.c_str());
        
        static char cmdOut[2048];
        bool ok = executeVoiceCommandAsArmedUser(cliCmd, cmdOut, sizeof(cmdOut));
        INFO_SRF("[HIER] Result: %s", ok ? cmdOut : cmdOut);
        if (!ok) {
          broadcastOutput("[VOICE] Command rejected (voice not armed or not authorized)");
        }
        gCommandCount++;
        gLastCommand = category;
      } else {
        WARN_SYSTEMF("[HIER] Category '%s' has no associated command!", category);
        Serial.printf("\033[1;31m[Voice] Sorry, I don't know how to do that.\033[0m\n");
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
    Serial.printf("\033[1;36m[Voice] %s... what action?\033[0m\n", normSubCat.c_str());
    
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
        Serial.printf("\033[1;32m[Voice] OK, %s %s.\033[0m\n", normSubCat.c_str(), normTarget.c_str());
        gLastCommand = gCurrentCategory + " " + gCurrentSubCategory + " " + target;
      } else {
        String normCat = normalizePhrase(gCurrentCategory.c_str());
        Serial.printf("\033[1;32m[Voice] OK, %s %s.\033[0m\n", normCat.c_str(), normTarget.c_str());
        gLastCommand = gCurrentCategory + " " + target;
      }
      
      static char cmdOut[2048];
      bool ok = executeVoiceCommandAsArmedUser(cliCmd, cmdOut, sizeof(cmdOut));
      INFO_SRF("[HIER] RESULT: %s", ok ? cmdOut : cmdOut);
      if (!ok) {
        broadcastOutput("[VOICE] Command rejected (voice not armed or not authorized)");
      }
      gCommandCount++;
    } else {
      WARN_SYSTEMF("[HIER] No CLI command found for '%s'->'%s'->'%s'!", 
                   gCurrentCategory.c_str(), gCurrentSubCategory.c_str(), target);
      Serial.printf("\033[1;31m[Voice] Sorry, I don't understand that.\033[0m\n");
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
      static char cmdOut[2048];
      bool ok = executeVoiceCommandAsArmedUser(mappedValue, cmdOut, sizeof(cmdOut));
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
      if (!VFS::exists(folder)) {
        VFS::mkdir(folder);
      }
      char fname[128];
      snprintf(fname, sizeof(fname), "%s/%s_%lu_%ld.wav",
               folder.c_str(), job.reason, (unsigned long)job.session_id, (long)job.cmd_id);
      File f = VFS::open(fname, FILE_WRITE, true);
      if (!f) {
        ERROR_SRF("SnipWriter: failed to open %s", fname);
        free(job.pcm);
        continue;
      }
      uint32_t dataSize = job.samples * sizeof(int16_t);
      writeWavHeader(f, dataSize, job.sample_rate, job.bits, job.channels);
      size_t written = f.write((const uint8_t*)job.pcm, dataSize);
      f.close();
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
  BaseType_t ret = xTaskCreatePinnedToCore(srSnipWriterTask, "sr_snip_wr", 4096, nullptr, 3, &gSrSnipWriterTask, 0);
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
      Serial.printf("\n\033[1;32m=== AUTO-TUNE COMPLETE ===\033[0m\n");
      Serial.printf("Tested %d configurations. Review logs above to find best settings.\n", (int)kAutoTuneConfigCount);
      Serial.printf("Apply best config with: sr tuning gain <value> and sr dyngain max <value>\n\n");
      return;
    }
    
    // Apply next config
    gSrAutoTuneStepStartMs = now;
    const AutoTuneConfig& cfg = kAutoTuneConfigs[gSrAutoTuneStep];
    gSettings.srAfeGain = cfg.afeGain;
    gSrDynGainMax = cfg.dynGainMax;
    gSrDynGainEnabled = cfg.dynGainEnabled;
    gSrDynGainCurrent = 1.0f;
    
    Serial.printf("\n\033[1;33m=== AUTO-TUNE Step %d/%d ===\033[0m\n", 
                  gSrAutoTuneStep + 1, (int)kAutoTuneConfigCount);
    Serial.printf("Config: %s\n", cfg.description);
    Serial.printf("Say test phrases now! (NOTE: AFE gain change needs SR restart)\n\n");
  }
}

static void srDebugPrintTelemetry() {
  // Check auto-tune advancement
  srAutoTuneCheck();
  
  uint32_t uptimeMs = millis();
  // Use WARN level so telemetry always prints (INFO requires DEBUG_SYSTEM flag)
  WARN_SYSTEMF("[SR] === SR Telemetry ===");
  WARN_SYSTEMF("[SR] Uptime: %u ms, Running: %s", (unsigned)uptimeMs, gESPSRRunning ? "yes" : "no");
  
  // Show raw mode and auto-tune status
  if (gSrRawOutputEnabled || gSrAutoTuneActive) {
    WARN_SYSTEMF("[SR] Raw=%s AutoTune=%s (step %d/%d)", 
                 gSrRawOutputEnabled ? "ON" : "OFF",
                 gSrAutoTuneActive ? "ACTIVE" : "off",
                 gSrAutoTuneStep + 1, (int)kAutoTuneConfigCount);
  }
  WARN_SYSTEMF("[SR] I2S: reads_ok=%u, reads_err=%u, reads_zero=%u, bytes_ok=%llu",
           (unsigned)gSrI2SReadOk, (unsigned)gSrI2SReadErr, (unsigned)gSrI2SReadZero, (unsigned long long)gSrI2SBytesOk);
  WARN_SYSTEMF("[SR] I2S: est_rate=%.1f Hz", gSrEstSampleRateHz);
  WARN_SYSTEMF("[SR] PCM: min=%d, max=%d, abs_avg=%.1f",
           (int)gSrLastPcmMin, (int)gSrLastPcmMax, gSrLastPcmAbsAvg);
  WARN_SYSTEMF("[SR] AFE: feed_chunk=%d, fetch_chunk=%d", gSrAfeFeedChunk, gSrAfeFetchChunk);
  WARN_SYSTEMF("[SR] AFE: feeds=%u, fetches=%u, last_vol=%.1f dB, last_vad=%d, last_ret=%d",
           (unsigned)gSrAfeFeedOk, (unsigned)gSrAfeFetchOk, gSrLastVolumeDb, gSrLastVadState, gSrLastAfeRetValue);
  WARN_SYSTEMF("[SR] Wake: count=%u, last_ms=%u, last_idx=%d, last_model=%d",
           (unsigned)gWakeWordCount, (unsigned)gLastWakeMs, gSrLastWakeWordIndex, gSrLastWakeNetModelIndex);
  WARN_SYSTEMF("[SR] MN: detect_calls=%u, detected=%u, accepted=%u, last_cmd='%s'",
           (unsigned)gSrMnDetectCalls, (unsigned)gSrMnDetected, (unsigned)gCommandCount, gLastCommand.c_str());
  WARN_SYSTEMF("[SR] Accept: gap_enabled=%d floor=%.2f gap=%.2f require_speech=%d gap_accepts=%u rejects=%u",
           gSrGapAcceptEnabled ? 1 : 0, gSrGapAcceptFloor, gSrGapAcceptGap,
           gSrTargetRequireSpeech ? 1 : 0, (unsigned)gSrGapAccepts, (unsigned)gSrLowConfidenceRejects);
  WARN_SYSTEMF("[SR] DynGain: enabled=%d cur=%.2f min=%.2f max=%.2f target_peak=%.0f alpha=%.2f applied=%u bypassed=%u",
           gSrDynGainEnabled ? 1 : 0, gSrDynGainCurrent, gSrDynGainMin, gSrDynGainMax,
           gSrDynGainTargetPeak, gSrDynGainAlpha, (unsigned)gSrDynGainApplied, (unsigned)gSrDynGainBypassed);
  WARN_SYSTEMF("[SR] Snip: enabled=%d, session_active=%d, ring_samples=%u",
           gSrSnipEnabled ? 1 : 0, gSrSnipSessionActive ? 1 : 0, (unsigned)gSrSnipRingSamples);
  WARN_SYSTEMF("[SR] ====================");
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
#if ENABLE_MICROPHONE_SENSOR
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

  if (!VFS::exists(kESPSRCommandFile)) {
    return true;
  }

  File f = VFS::open(kESPSRCommandFile, "r");
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

  if (!VFS::isSDAvailable()) {
    return false;
  }

  VFS::mkdir("/sd/ESPSR");
  File f = VFS::open(kESPSRCommandFile, "w");
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
  WARN_SYSTEMF("[SR_I2S] ========== initI2SMicrophone() START ==========");
  WARN_SYSTEMF("[SR_I2S] Heap: free=%u, PSRAM_free=%u", 
               (unsigned)esp_get_free_heap_size(), 
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  I2sMicLockGuard i2sGuard("sr.i2s.init");
  
  // Create I2S channel for PDM RX
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_SR_NUM, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = 1024;
  
  WARN_SYSTEMF("[SR_I2S] Channel config: i2s_num=%d, dma_desc_num=%d, dma_frame_num=%d",
               (int)I2S_SR_NUM, (int)chan_cfg.dma_desc_num, (int)chan_cfg.dma_frame_num);
  
  WARN_SYSTEMF("[SR_I2S] Calling i2s_new_channel()...");
  esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &gI2SRxHandle);
  WARN_SYSTEMF("[SR_I2S] i2s_new_channel returned: 0x%x (%s), handle=%p", 
               err, esp_err_to_name(err), gI2SRxHandle);
  if (err != ESP_OK) {
    ERROR_SRF("Failed to create I2S channel: %s", esp_err_to_name(err));
    return false;
  }
  
  // Configure PDM RX mode for the onboard PDM microphone
  // XIAO ESP32S3 Sense MSM261S4030H0R outputs on LEFT channel
  i2s_pdm_rx_slot_config_t slot_cfg =
#ifdef I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG
    I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#else
    I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#endif

#if !defined(I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG) && defined(I2S_PDM_DATA_FMT_PCM)
  slot_cfg.data_fmt = I2S_PDM_DATA_FMT_PCM;
#endif

  i2s_pdm_rx_config_t pdm_cfg = {
    .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(I2S_SR_SAMPLE_RATE),
    .slot_cfg = slot_cfg,
    .gpio_cfg = {
      .clk = (gpio_num_t)MIC_CLK_PIN,
      .din = (gpio_num_t)MIC_DATA_PIN,
      .invert_flags = {
        .clk_inv = false,
      },
    },
  };

  WARN_SYSTEMF("[SR_I2S] PDM clk_cfg: sample_rate_hz=%u, clk_src=%d, mclk_mult=%d, bclk_div=%u",
            (unsigned)pdm_cfg.clk_cfg.sample_rate_hz,
            (int)pdm_cfg.clk_cfg.clk_src,
            (int)pdm_cfg.clk_cfg.mclk_multiple,
            (unsigned)pdm_cfg.clk_cfg.bclk_div);
  WARN_SYSTEMF("[SR_I2S] PDM gpio_cfg: clk=%d, din=%d, clk_inv=%d",
            (int)pdm_cfg.gpio_cfg.clk, (int)pdm_cfg.gpio_cfg.din,
            (int)pdm_cfg.gpio_cfg.invert_flags.clk_inv);

#ifdef I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG
  WARN_SYSTEMF("[SR_I2S] PDM slot cfg: I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG");
#else
  WARN_SYSTEMF("[SR_I2S] PDM slot cfg: I2S_PDM_RX_SLOT_DEFAULT_CONFIG%s", 
#ifdef I2S_PDM_DATA_FMT_PCM
            " + data_fmt=PCM"
#else
            ""
#endif
  );
#endif
  
  WARN_SYSTEMF("[SR_I2S] Calling i2s_channel_init_pdm_rx_mode()...");
  err = i2s_channel_init_pdm_rx_mode(gI2SRxHandle, &pdm_cfg);
  WARN_SYSTEMF("[SR_I2S] i2s_channel_init_pdm_rx_mode returned: 0x%x (%s)", err, esp_err_to_name(err));
  if (err != ESP_OK) {
    ERROR_SRF("Failed to init I2S PDM RX mode: %s", esp_err_to_name(err));
    i2s_del_channel(gI2SRxHandle);
    gI2SRxHandle = nullptr;
    return false;
  }
  
  WARN_SYSTEMF("[SR_I2S] Calling i2s_channel_enable()...");
  err = i2s_channel_enable(gI2SRxHandle);
  WARN_SYSTEMF("[SR_I2S] i2s_channel_enable returned: 0x%x (%s)", err, esp_err_to_name(err));
  if (err != ESP_OK) {
    ERROR_SRF("Failed to enable I2S channel: %s", esp_err_to_name(err));
    i2s_del_channel(gI2SRxHandle);
    gI2SRxHandle = nullptr;
    return false;
  }

  WARN_SYSTEMF("[SR_I2S] Starting PDM warm-up flush (10 reads of 512 bytes)...");
  {
    int16_t flushBuf[256];
    size_t flushBytesRead = 0;
    int flushOkCount = 0;
    for (int i = 0; i < 10; i++) {
      esp_err_t flushErr = i2s_channel_read(gI2SRxHandle, flushBuf, sizeof(flushBuf), &flushBytesRead, pdMS_TO_TICKS(100));
      if (flushErr == ESP_OK && flushBytesRead > 0) {
        flushOkCount++;
        if (i == 9) {
          int16_t mn = 32767, mx = -32768;
          for (size_t j = 0; j < flushBytesRead / 2; j++) {
            if (flushBuf[j] < mn) mn = flushBuf[j];
            if (flushBuf[j] > mx) mx = flushBuf[j];
          }
          WARN_SYSTEMF("[SR_I2S] Flush[%d]: %u bytes, min=%d, max=%d", i, (unsigned)flushBytesRead, mn, mx);
        }
      } else {
        WARN_SYSTEMF("[SR_I2S] Flush[%d]: err=0x%x, bytes=%u", i, flushErr, (unsigned)flushBytesRead);
      }
    }
    WARN_SYSTEMF("[SR_I2S] Warm-up flush complete: %d/10 reads OK", flushOkCount);
  }
  
  WARN_SYSTEMF("[SR_I2S] ========== initI2SMicrophone() SUCCESS ==========");
  INFO_SRF("PDM microphone initialized (CLK=%d, DATA=%d)", MIC_CLK_PIN, MIC_DATA_PIN);
  return true;
}

static void deinitI2SMicrophone() {
  if (gI2SRxHandle) {
    I2sMicLockGuard i2sGuard("sr.i2s.deinit");
    i2s_channel_disable(gI2SRxHandle);
    i2s_del_channel(gI2SRxHandle);
    gI2SRxHandle = nullptr;
    DEBUG_SRF("I2S microphone deinitialized");
  }
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
  WARN_SYSTEMF("[SR_TASK] gI2SRxHandle=%p", gI2SRxHandle);
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
    esp_err_t err;
    {
      I2sMicLockGuard i2sGuard("sr.i2s.read");
      err = i2s_channel_read(gI2SRxHandle, i2sReadBuf, i2sReadBytes, &bytesRead, pdMS_TO_TICKS(100));
    }
    uint32_t readDurationMs = millis() - readStartMs;
    
    if (doDetailedLog) {
      WARN_SYSTEMF("[SR_LOOP] Loop %u: i2s_read took %u ms, err=0x%x (%s), bytesRead=%u",
                   (unsigned)loopCount, (unsigned)readDurationMs, err, esp_err_to_name(err), (unsigned)bytesRead);
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
        WARN_SYSTEMF("[SR_LOOP] I2S READ ZERO BYTES at loop %u", (unsigned)loopCount);
      }
      SR_DBG_L(3, "I2S read zero bytes (loop=%u)", (unsigned)loopCount);
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
          Serial.println();
          Serial.println("\033[1;36m[Voice] Yes?\033[0m");
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
              Serial.println("\033[1;33m[Voice] Sorry, I didn't catch that.\033[0m");
              
              gVoiceState = VoiceState::IDLE;
              gCurrentCategory = "";
              gCurrentSubCategory = "";
            } else if (gVoiceState == VoiceState::AWAIT_SUBCATEGORY) {
              INFO_SRF("[HIER] ============================================");
              INFO_SRF("[HIER] TIMEOUT: No subcategory detected for '%s'", gCurrentCategory.c_str());
              INFO_SRF("[HIER] ============================================");
              INFO_SRF("[HIER-DEBUG] State transition: AWAIT_SUBCATEGORY -> IDLE");
              
              // User-facing feedback
              Serial.printf("\033[1;33m[Voice] Timed out waiting for %s selection.\033[0m\n", gCurrentCategory.c_str());
              
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
                Serial.printf("\033[1;33m[Voice] Timed out waiting for %s action.\033[0m\n", gCurrentSubCategory.c_str());
              } else {
                Serial.printf("\033[1;33m[Voice] Timed out waiting for %s action.\033[0m\n", gCurrentCategory.c_str());
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
                  WARN_SYSTEMF("[SR] Rejected command: id=%d prob=%.3f (need>=%.2f or gap floor=%.2f gap=%.2f) (rejects=%lu)",
                               cmdId, cmdProb, requiredConfidence, gSrGapAcceptFloor, gSrGapAcceptGap, (unsigned long)gSrLowConfidenceRejects);
                  if (isCategoryStage) {
                    Serial.printf("\033[1;33m[Voice] I heard '%s'... can you say it again?\033[0m\n", normalizePhrase(cmdPhraseCopy).c_str());
                  } else {
                    Serial.println("\033[1;33m[Voice] Sorry, can you repeat that?\033[0m");
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
    if (VFS::mkdir("/sd/ESPSR")) {
      INFO_SRF("Created /sd/ESPSR folder on SD card");
      folderCreated = true;
    } else if (VFS::exists("/sd/ESPSR")) {
      DEBUG_SRF("/sd/ESPSR already exists");
      folderCreated = true;
    }
  }
  
  if (!folderCreated) {
    if (VFS::mkdir("/ESPSR")) {
      INFO_SRF("Created /ESPSR folder on LittleFS");
    } else if (VFS::exists("/ESPSR")) {
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

#if ENABLE_MICROPHONE_SENSOR
  WARN_SYSTEMF("[SR_START] Checking microphone sensor: micEnabled=%d", micEnabled ? 1 : 0);
  if (micEnabled) {
    gRestoreMicAfterSR = true;
    INFO_SRF("Microphone sensor is running; stopping it to start SR");
    if (micRecording) {
      stopRecording();
    }
    stopMicrophone();
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
               (unsigned)SR_TASK_STACK_SIZE, SR_TASK_PRIORITY);
  gSRTaskShouldRun = true;
  BaseType_t ret = xTaskCreatePinnedToCore(
    srTask,
    "sr_task",
    SR_TASK_STACK_SIZE,
    nullptr,
    SR_TASK_PRIORITY,
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
  JsonDocument doc;
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
  serializeJson(doc, output);
}

static const char* setEnabledFromArgs(const String& cmd) {
  (void)cmd;
  return "Error: ENABLE_ESP_SR is a compile-time flag";
}

const char* cmd_sr(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  return "Usage: sr <enable|start|stop|status|cmds|debug|confidence|timeout|tuning|accept|dyngain|raw|autotune|snip>";
}

const char* cmd_sr_enable(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return setEnabledFromArgs(cmd);
}

const char* cmd_sr_start(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  bool ok = startESPSR();
  if (!ok) return "Error: failed to start";

  ensureVoiceArmMutex();
  bool armed = false;
  if (gVoiceArmMutex && xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    armed = voiceArmFromContextInternal(gExecAuthContext);
    xSemaphoreGive(gVoiceArmMutex);
  } else {
    armed = voiceArmFromContextInternal(gExecAuthContext);
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
  }
  return out.c_str();
}

const char* cmd_sr_stop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
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

static const char* cmd_voice_arm_cli(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  ensureVoiceArmMutex();
  bool armed = false;
  if (gVoiceArmMutex && xSemaphoreTake(gVoiceArmMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    armed = voiceArmFromContextInternal(gExecAuthContext);
    xSemaphoreGive(gVoiceArmMutex);
  } else {
    armed = voiceArmFromContextInternal(gExecAuthContext);
  }
  if (!armed) return "Error: cannot arm voice from this transport/user";
  static String out;
  out = "OK: voice armed as '";
  out += gVoiceArmedUser;
  out += "'";
  return out.c_str();
}

static const char* cmd_voice_disarm_cli(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
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

static const char* cmd_voice_status_cli(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
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
    out = gVoiceArmed ? (String("voice: armed user='") + gVoiceArmedUser + "' by=" + transportToStableString(gVoiceArmedByTransport)) : "voice: disarmed";
  }
  return out.c_str();
}

const char* cmd_sr_status(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  static String out;
  out = "";
  buildESPSRStatusJson(out);
  return out.c_str();
}

// Voice control commands - these are handled specially in onVoiceCommandDetected
// but registered here for consistency with the command registry system
static const char* cmd_voice_cancel(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  // This is handled in onVoiceCommandDetected, not via CLI
  return "Voice cancel - resets voice state to idle";
}

static const char* cmd_voice_help(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  // This is handled in onVoiceCommandDetected, not via CLI
  return "Voice help - shows available options for current state";
}

static const char* cmd_sr_cmds(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  return "Usage: sr cmds <list|add|del|clear|save|reload>";
}

static const char* cmd_sr_cmds_list(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  static String out;
  out = "";

  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: sr start";
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

static const char* cmd_sr_cmds_add(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
  args.trim();
  if (args.length() == 0) return "Usage: sr cmds add <id> <phrase>";
  int sp = args.indexOf(' ');
  if (sp <= 0) return "Usage: sr cmds add <id> <phrase>";
  String idStr = args.substring(0, sp);
  String phrase = args.substring(sp + 1);
  idStr.trim();
  phrase.trim();
  if (!isAllDigits(idStr) || phrase.length() == 0) return "Usage: sr cmds add <id> <phrase>";
  int id = idStr.toInt();
  if (id <= 0) return "Error: id must be > 0";

  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: sr start";
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

static const char* cmd_sr_cmds_del(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = cmd;
  arg.trim();
  if (arg.length() == 0) return "Usage: sr cmds del <phrase|id>";

  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: sr start";
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

static const char* cmd_sr_cmds_clear(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = cmd;
  arg.trim();
  if (arg != "confirm") return "Usage: sr cmds clear confirm";

  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: sr start";
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

static const char* cmd_sr_cmds_reload(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: sr start";
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

static const char* cmd_sr_cmds_save(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: sr start";
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

static const char* cmd_sr_cmds_sync(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  if (!mnCommandsReady()) {
    return "Error: MultiNet not initialized. Run: sr start";
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

static const char* cmd_sr_debug(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  return "Usage: sr debug <level|telem|stats|reset>";
}

static const char* cmd_sr_debug_level(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = cmd;
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

static const char* cmd_sr_debug_telem(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = cmd;
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

static const char* cmd_sr_debug_stats(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  srDebugPrintTelemetry();
  return "OK (stats printed to log)";
}

static const char* cmd_sr_debug_reset(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  srDebugResetCounters();
  return "OK";
}

static const char* cmd_sr_confidence(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
  args.trim();
  
  if (args.length() == 0) {
    static char buf[96];
    snprintf(buf, sizeof(buf),
             "Category confidence threshold: %.2f\nTarget confidence threshold: %.2f (rejects: %lu)\nUsage: sr confidence [<0.0-1.0> | category <0.0-1.0> | target <0.0-1.0>]",
             gSrMinCategoryConfidence, gSrMinCommandConfidence, (unsigned long)gSrLowConfidenceRejects);
    return buf;
  }

  int sp = args.indexOf(' ');
  String first = args;
  String rest = "";
  if (sp >= 0) {
    first = args.substring(0, sp);
    rest = args.substring(sp + 1);
    first.trim();
    rest.trim();
  }

  bool setCategoryOnly = (first == "category");
  bool setTargetOnly = (first == "target");

  float val = 0.0f;
  if (setCategoryOnly || setTargetOnly) {
    if (rest.length() == 0) return "Error: missing value";
    val = rest.toFloat();
  } else {
    val = args.toFloat();
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

static const char* cmd_sr_accept(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
  args.trim();
  args.toLowerCase();

  if (args.length() == 0) {
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

  int sp = args.indexOf(' ');
  String key = args;
  String val = "";
  if (sp >= 0) {
    key = args.substring(0, sp);
    val = args.substring(sp + 1);
    key.trim();
    val.trim();
  }

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

  return "Usage: sr accept [on|off|floor <0.0-1.0>|gap <0.0-1.0>|speech <0|1>]";
}

static const char* cmd_sr_dyngain(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
  args.trim();
  args.toLowerCase();

  if (args.length() == 0) {
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

  int sp = args.indexOf(' ');
  String key = args;
  String val = "";
  if (sp >= 0) {
    key = args.substring(0, sp);
    val = args.substring(sp + 1);
    key.trim();
    val.trim();
  }

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

  return "Usage: sr dyngain [on|off|min <0.1-10>|max <0.1-10>|target <1000-30000>|alpha <0.0-1.0>|reset]";
}

static const char* cmd_sr_raw(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
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
  
  return "Usage: sr raw [on|off]";
}

static const char* cmd_sr_autotune(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
  args.trim();
  
  if (args.length() == 0 || args == "status") {
    if (gSrAutoTuneActive) {
      uint32_t elapsed = millis() - gSrAutoTuneStepStartMs;
      uint32_t remaining = (elapsed < kAutoTuneStepDurationMs) ? (kAutoTuneStepDurationMs - elapsed) / 1000 : 0;
      static char buf[256];
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
      return "Auto-tune already running. Use 'sr autotune stop' to cancel.";
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
    
    Serial.printf("\n\033[1;36m=== AUTO-TUNE STARTED ===\033[0m\n");
    Serial.printf("Will cycle through %d configurations, %d sec each.\n", (int)kAutoTuneConfigCount, (int)(kAutoTuneStepDurationMs / 1000));
    Serial.printf("Say test phrases (system, battery, cancel, help) during each step.\n");
    Serial.printf("\033[1;33mStep 1/%d: %s\033[0m\n", (int)kAutoTuneConfigCount, kAutoTuneConfigs[0].description);
    Serial.printf("NOTE: AFE gain change requires SR restart. Run 'sr stop' then 'sr start'.\n\n");
    
    return "Auto-tune started. Restart SR to apply AFE gain change.";
  }
  
  if (args == "stop") {
    if (!gSrAutoTuneActive) {
      return "Auto-tune not running.";
    }
    gSrAutoTuneActive = false;
    gSrRawOutputEnabled = false;
    Serial.printf("\n\033[1;36m=== AUTO-TUNE STOPPED ===\033[0m\n");
    return "Auto-tune stopped. Review the results above to pick best config.";
  }
  
  return "Usage: sr autotune [start|stop|status]";
}

static const char* cmd_sr_timeout(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
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
  
  gSettings.srCommandTimeout = val;
  writeSettingsJson();
  static char buf[80];
  snprintf(buf, sizeof(buf), "Command timeout set to %d ms (%.1f sec). Saved.", val, val / 1000.0f);
  return buf;
}

static const char* cmd_sr_tuning(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  static char buf[520];
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
    "Usage: sr tuning <gain|agc|vad|filters> <value>\n"
    "Usage: sr tuning swgain <1.0-50.0> (sets micgain)",
    mg, swgain, (int)getMicDcOffset(), gSrFiltersEnabled ? "ON" : "OFF",
    gSettings.srAfeGain, gSettings.srAgcMode, gSettings.srVadMode,
    gSrMinCommandConfidence, gSettings.srCommandTimeout);
  return buf;
}

static const char* cmd_sr_tuning_swgain(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
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
  gSettings.microphoneGain = mg;
#if ENABLE_MICROPHONE_SENSOR
  micGain = mg;
#endif
  writeSettingsJson();
  float actualSwgain = 24.0f * ((float)mg / 50.0f);
  static char buf[120];
  snprintf(buf, sizeof(buf), "OK (micgain=%d%%, swgain=%.1f)", mg, actualSwgain);
  return buf;
}

static const char* cmd_sr_tuning_gain(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
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
  
  gSettings.srAfeGain = val;
  writeSettingsJson();
  static char buf[80];
  snprintf(buf, sizeof(buf), "AFE gain set to %.1f. Restart SR to apply.", val);
  return buf;
}

static const char* cmd_sr_tuning_agc(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
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
  
  gSettings.srAgcMode = val;
  writeSettingsJson();
  static char buf[80];
  const char* modeStr[] = {"off", "-9dB", "-6dB", "-3dB"};
  snprintf(buf, sizeof(buf), "AGC mode set to %d (%s). Restart SR to apply.", val, modeStr[val]);
  return buf;
}

static const char* cmd_sr_tuning_vad(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
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
  
  gSettings.srVadMode = val;
  writeSettingsJson();
  static char buf[80];
  snprintf(buf, sizeof(buf), "VAD sensitivity set to %d. Restart SR to apply.", val);
  return buf;
}

static const char* cmd_sr_tuning_filters(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
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
  return "Usage: sr tuning filters <on|off>";
}

static const char* cmd_sr_snip(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  return "Usage: sr snip <on|off|start|stop|status|config>";
}

static const char* cmd_sr_snip_on(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  if (!gESPSRRunning) {
    return "Error: SR not running. Run: sr start";
  }
  if (!srSnipInit()) {
    return "Error: failed to initialize snippet capture";
  }
  gSrSnipEnabled = true;
  return "Snippet capture enabled (will trigger on wake word)";
}

static const char* cmd_sr_snip_off(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  gSrSnipEnabled = false;
  return "Snippet capture disabled";
}

static const char* cmd_sr_snip_start(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  if (!gESPSRRunning) {
    return "Error: SR not running. Run: sr start";
  }
  if (!srSnipInit()) {
    return "Error: failed to initialize snippet capture";
  }
  gSrSnipManualStartRequested = true;
  return "Manual snippet capture started";
}

static const char* cmd_sr_snip_stop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
  if (!gSrSnipSessionActive) {
    return "No active snippet session";
  }
  gSrSnipManualStopRequested = true;
  return "Manual snippet capture stopped";
}

static const char* cmd_sr_snip_status(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)cmd;
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

static const char* cmd_sr_snip_config(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = cmd;
  args.trim();
  if (args.length() == 0) {
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
  int sp = args.indexOf(' ');
  if (sp <= 0) return "Usage: sr snip config <pre_ms|max_ms|dest> <value>";
  String key = args.substring(0, sp);
  String val = args.substring(sp + 1);
  key.trim();
  val.trim();
  key.toLowerCase();
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
  return "Unknown config key. Use: pre_ms, max_ms, dest";
}

const CommandEntry espsrCommands[] = {
  { "sr", "ESP-SR speech recognition commands.", false, cmd_sr, "Usage: sr <enable|start|stop|status|cmds|debug|confidence|timeout|tuning|accept|dyngain|raw|autotune|snip>" },
  { "sr enable", "Enable/disable ESP-SR (compile-time flag).", true, cmd_sr_enable, "Usage: sr enable <0|1>" },
  { "sr start", "Start ESP-SR pipeline.", false, cmd_sr_start, "Usage: sr start" },
  { "sr stop", "Stop ESP-SR pipeline.", false, cmd_sr_stop, "Usage: sr stop", "voice", "close" },
  { "sr status", "Show ESP-SR status.", false, cmd_sr_status, "Usage: sr status" },
  { "voice arm", "Arm voice command execution as the current authenticated user.", false, cmd_voice_arm_cli, "Usage: voice arm" },
  { "voice disarm", "Disarm voice command execution.", false, cmd_voice_disarm_cli, "Usage: voice disarm" },
  { "voice status", "Show voice arming status.", false, cmd_voice_status_cli, "Usage: voice status" },
  { "sr cmds", "Manage MultiNet command phrases.", true, cmd_sr_cmds, "Usage: sr cmds <list|add|del|clear|save|reload|sync>" },
  { "sr cmds list", "List current MultiNet commands.", true, cmd_sr_cmds_list, "Usage: sr cmds list" },
  { "sr cmds add", "Add or update a MultiNet command.", true, cmd_sr_cmds_add, "Usage: sr cmds add <id> <phrase>" },
  { "sr cmds del", "Delete a MultiNet command (by phrase or id).", true, cmd_sr_cmds_del, "Usage: sr cmds del <phrase|id>" },
  { "sr cmds clear", "Clear all MultiNet commands.", true, cmd_sr_cmds_clear, "Usage: sr cmds clear confirm" },
  { "sr cmds reload", "Reload commands from SD file.", true, cmd_sr_cmds_reload, "Usage: sr cmds reload" },
  { "sr cmds save", "Save current commands to SD file.", true, cmd_sr_cmds_save, "Usage: sr cmds save" },
  { "sr cmds sync", "Sync voice commands from CLI registry.", true, cmd_sr_cmds_sync, "Usage: sr cmds sync" },
  { "sr debug", "SR debug/telemetry commands.", false, cmd_sr_debug, "Usage: sr debug <level|telem|stats|reset>" },
  { "sr debug level", "Set debug verbosity (0-4).", false, cmd_sr_debug_level, "Usage: sr debug level [0-4]" },
  { "sr debug telem", "Set periodic telemetry interval (ms, 0=off).", false, cmd_sr_debug_telem, "Usage: sr debug telem [ms]" },
  { "sr debug stats", "Print current SR statistics.", false, cmd_sr_debug_stats, "Usage: sr debug stats" },
  { "sr debug reset", "Reset SR debug counters.", false, cmd_sr_debug_reset, "Usage: sr debug reset" },
  { "sr confidence", "Get/set command confidence threshold.", false, cmd_sr_confidence, "Usage: sr confidence [0.0-1.0]" },
  { "sr accept", "Configure target acceptance policy (gap acceptance).", false, cmd_sr_accept, "Usage: sr accept [on|off|floor <0.0-1.0>|gap <0.0-1.0>|speech <0|1>]" },
  { "sr dyngain", "Configure dynamic gain normalization (MultiNet input only).", false, cmd_sr_dyngain, "Usage: sr dyngain [on|off|min <0.1-10>|max <0.1-10>|target <1000-30000>|alpha <0.0-1.0>|reset]" },
  { "sr raw", "Toggle raw output mode (shows all MultiNet hypotheses).", false, cmd_sr_raw, "Usage: sr raw [on|off]" },
  { "sr autotune", "Auto-cycle through gain configurations to find best settings.", false, cmd_sr_autotune, "Usage: sr autotune [start|stop|status]" },
  { "sr timeout", "Get/set command listening timeout.", false, cmd_sr_timeout, "Usage: sr timeout [1000-30000]" },
  { "sr tuning", "Show/set audio tuning parameters.", false, cmd_sr_tuning, "Usage: sr tuning [gain|agc|vad]" },
  { "sr tuning swgain", "Set software gain (1.0-50.0) by updating shared micgain.", false, cmd_sr_tuning_swgain, "Usage: sr tuning swgain <1.0-50.0>" },
  { "sr tuning gain", "Set AFE linear gain (0.1-10.0).", false, cmd_sr_tuning_gain, "Usage: sr tuning gain <0.1-10.0>" },
  { "sr tuning agc", "Set AGC mode (0=off, 1-3=levels).", false, cmd_sr_tuning_agc, "Usage: sr tuning agc <0-3>" },
  { "sr tuning vad", "Set VAD sensitivity (0-4).", false, cmd_sr_tuning_vad, "Usage: sr tuning vad <0-4>" },
  { "sr tuning filters", "Toggle audio filters (high-pass + pre-emphasis).", false, cmd_sr_tuning_filters, "Usage: sr tuning filters <on|off>" },
  { "sr snip", "Voice snippet capture commands.", false, cmd_sr_snip, "Usage: sr snip <on|off|start|stop|status|config>" },
  { "sr snip on", "Enable auto-capture on wake word.", false, cmd_sr_snip_on, "Usage: sr snip on" },
  { "sr snip off", "Disable auto-capture.", false, cmd_sr_snip_off, "Usage: sr snip off" },
  { "sr snip start", "Start manual snippet capture now.", false, cmd_sr_snip_start, "Usage: sr snip start" },
  { "sr snip stop", "Stop manual snippet capture and save.", false, cmd_sr_snip_stop, "Usage: sr snip stop" },
  { "sr snip status", "Show snippet capture status.", false, cmd_sr_snip_status, "Usage: sr snip status" },
  { "sr snip config", "Configure snippet capture params.", false, cmd_sr_snip_config, "Usage: sr snip config [pre_ms|max_ms|dest] [value]" },
  // Global voice commands - voiceCategory="*" means available at all stages
  { "voice cancel", "Cancel current voice command sequence.", false, cmd_voice_cancel, nullptr, "*", "cancel" },
  { "voice cancel", "Cancel current voice command sequence.", false, cmd_voice_cancel, nullptr, "*", "nevermind" },
  { "voice help", "Show available voice options for current state.", false, cmd_voice_help, nullptr, "*", "help" },
};

const size_t espsrCommandsCount = sizeof(espsrCommands) / sizeof(espsrCommands[0]);
REGISTER_COMMAND_MODULE(espsrCommands, espsrCommandsCount, "ESPSR");

// ============================================================================
// ESP-SR Settings Module
// ============================================================================

static bool isESPSRConnected() {
  return gESPSRInitialized;
}

static const SettingEntry espsrSettingsEntries[] = {
  { "srAutoStart",      SETTING_BOOL, &gSettings.srAutoStart,      0, 0, nullptr, 0, 1, "Auto-start at boot", nullptr },
  { "srModelSource",    SETTING_INT,  &gSettings.srModelSource,    0, 0, nullptr, 0, 2, "Model source (0=partition, 1=SD, 2=LittleFS)", nullptr },
  { "srCommandTimeout", SETTING_INT,  &gSettings.srCommandTimeout, 6000, 0, nullptr, 1000, 30000, "Command timeout (ms)", nullptr }
};

extern const SettingsModule espsrSettingsModule = {
  "espsr",
  "espsr",
  espsrSettingsEntries,
  sizeof(espsrSettingsEntries) / sizeof(espsrSettingsEntries[0]),
  isESPSRConnected,
  "ESP-SR speech recognition settings"
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
