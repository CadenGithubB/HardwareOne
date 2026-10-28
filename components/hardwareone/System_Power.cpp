/**
 * System Power Management
 * 
 * Handles CPU frequency scaling and display brightness management
 * for battery optimization.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include "System_Power.h"
#include "System_BuildConfig.h"
#include "System_Settings.h"
#include "System_Debug.h"
#include "System_Command.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_MemUtil.h"    // ps_alloc / PSRAM_JSON_DOC — `power json` blob
#include "System_OTASafety.h"  // otaSafetyIsPendingVerification — sleep gate
#include "System_Utils.h"      // argWantsJson() — opt-in JSON output
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#if ENABLE_I2C_SYSTEM
#include "System_I2C_Manager.h"
#include "System_PollPause.h"
#endif
#include "System_UartLink.h"   // uartLinkIsRunning / uartLinkEffectiveBaud — idle-clock clamp
#include "soc/soc_caps.h"      // SOC_UART_SUPPORT_REF_TICK

// Serialize the complete HAL transition, including its before/after callbacks.
// A concurrent or recursive request is refused without waiting on its caller.
static StaticSemaphore_t sCpuFrequencyMutexStorage;
static SemaphoreHandle_t sCpuFrequencyMutex =
    xSemaphoreCreateMutexStatic(&sCpuFrequencyMutexStorage);

bool powerSetCpuFrequencyMhz(uint32_t mhz, bool* outClockChanged) {
  if (outClockChanged) *outClockChanged = false;
  if (!sCpuFrequencyMutex || xSemaphoreTake(sCpuFrequencyMutex, 0) != pdTRUE) {
    return false;
  }

  bool applied = true;
  if (getCpuFrequencyMhz() != mhz) {
#if ENABLE_I2C_SYSTEM
    PollPauseGuard pause;
    // Settings may apply before I2C startup. Changing the clock must not create
    // the manager or initialize buses as a side effect.
    I2CDeviceManager* mgr = I2CDeviceManager::existingInstance();
    SemaphoreHandle_t held[I2CDeviceManager::NUM_BUSES] = {};
    if (mgr) {
      // Bus locks are non-recursive. Reject a transaction's own request before
      // taking another bus, rather than waiting with the buses in reverse order.
      for (uint8_t b = 0; b < I2CDeviceManager::NUM_BUSES; ++b) {
        SemaphoreHandle_t mutex = mgr->getBusMutex(b);
        if (mutex && xSemaphoreGetMutexHolder(mutex) == xTaskGetCurrentTaskHandle()) {
          applied = false;
          break;
        }
      }
      if (applied) {
        // Pausing stops new polls; the locks also drain in-flight transfers and
        // exclude command-driven I2C work until the clock transition completes.
        for (uint8_t b = 0; b < I2CDeviceManager::NUM_BUSES; ++b) {
          SemaphoreHandle_t mutex = mgr->getBusMutex(b);
          if (!mutex) continue;
          if (xSemaphoreTake(mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            applied = false;
            break;
          }
          held[b] = mutex;
        }
      }
    }
#endif
    if (applied) {
      const uint32_t before = getCpuFrequencyMhz();
      const bool accepted = setCpuFrequencyMhz(mhz);
      const uint32_t actual = getCpuFrequencyMhz();
      if (outClockChanged) *outClockChanged = actual != before;
      applied = accepted && actual == mhz;
    }
#if ENABLE_I2C_SYSTEM
    for (uint8_t b = I2CDeviceManager::NUM_BUSES; b > 0; --b) {
      if (held[b - 1]) xSemaphoreGive(held[b - 1]);
    }
#endif
  }
  xSemaphoreGive(sCpuFrequencyMutex);
  return applied;
}

// Power mode configuration
struct PowerModeConfig {
  const char* name;
  uint32_t cpuFreqMhz;
  uint8_t displayBrightnessPercent;
};

// The clock while in use (idle derives from it: getPowerModeIdleCpuFreq) and
// the brightness a user's choice of the mode applies. Busy workloads raise any
// mode to the maximum clock (powerPolicyTick), so Adaptive — floor in use and
// idle — is the mode that leaves the clock to the work.
static const PowerModeConfig gPowerModes[] = {
  {"Performance", POWER_CPU_PERFORMANCE_MHZ, 100},
  {"Balanced",    POWER_CPU_BALANCED_MHZ,     80},
  {"Adaptive",    POWER_INTERACTIVE_FLOOR_MHZ, 80},
  {"UltraSaver",  POWER_CPU_ULTRA_IDLE_MHZ,   30},
  {"Locked",      POWER_CPU_PERFORMANCE_MHZ, 100}
};

static const uint32_t gPowerCpuFrequencies[] = {
  POWER_INTERACTIVE_FLOOR_MHZ, POWER_CPU_BALANCED_MHZ, POWER_CPU_PERFORMANCE_MHZ
};

size_t getPowerCpuFrequencyCount() {
  return sizeof(gPowerCpuFrequencies) / sizeof(gPowerCpuFrequencies[0]);
}

uint32_t getPowerCpuFrequencyMhz(size_t index) {
  return index < getPowerCpuFrequencyCount() ? gPowerCpuFrequencies[index] : 0;
}

bool isPowerCpuFrequencySupported(uint32_t mhz) {
  for (size_t i = 0; i < getPowerCpuFrequencyCount(); ++i) {
    if (gPowerCpuFrequencies[i] == mhz) return true;
  }
  return false;
}

static_assert(sizeof(gPowerModes) / sizeof(gPowerModes[0]) == POWER_MODE_COUNT,
              "gPowerModes must match POWER_MODE_COUNT");

// ============================================================================
// Power Mode Management
// ============================================================================

const char* getPowerModeName(uint8_t mode) {
  if (mode >= POWER_MODE_COUNT) {
    return "Unknown";
  }
  return gPowerModes[mode].name;
}

uint32_t getPowerModeCpuFreq(uint8_t mode) {
  if (mode >= POWER_MODE_COUNT) {
    return POWER_CPU_PERFORMANCE_MHZ;  // Default to target performance
  }
  return gPowerModes[mode].cpuFreqMhz;
}

// Interactive clock = nominal clamped UP to the target PLL floor;
// all faster modes are returned unchanged.
uint32_t getPowerModeActiveCpuFreq(uint8_t mode) {
  uint32_t f = getPowerModeCpuFreq(mode);
  return f < POWER_INTERACTIVE_FLOOR_MHZ ? POWER_INTERACTIVE_FLOOR_MHZ : f;
}

// Idle clock while OLED power-save has blanked the panel (radio stays up).
// Locked holds its interactive clock. Other modes use the target PLL floor,
// except UltraSaver, which uses its nominal idle-only XTAL clock.
uint32_t getPowerModeIdleCpuFreq(uint8_t mode) {
  if (mode == POWER_MODE_LOCKED) {
    return getPowerModeActiveCpuFreq(mode);
  }
  uint32_t f = getPowerModeCpuFreq(mode);
  return f < POWER_INTERACTIVE_FLOOR_MHZ ? f : POWER_INTERACTIVE_FLOOR_MHZ;
}

uint8_t getPowerModeDisplayBrightness(uint8_t mode) {
  if (mode >= POWER_MODE_COUNT) {
    return 100;  // Default to full brightness
  }
  return gPowerModes[mode].displayBrightnessPercent;
}

bool applyPowerMode(uint8_t mode, bool setDisplayBrightness) {
  DEBUG_SYSTEMF("[POWER] applyPowerMode called with mode=%d", mode);
  
  if (mode >= POWER_MODE_COUNT) {
    ERROR_SYSTEMF("Invalid power mode: %d", mode);
    return false;
  }
  
  const PowerModeConfig& config = gPowerModes[mode];
  // UltraSaver's lower XTAL clock is reserved for the drain-guarded idle path.
  // Active mode changes always use the target's interactive PLL frequencies.
  const uint32_t targetFreq = getPowerModeActiveCpuFreq(mode);
  DEBUG_SYSTEMF("[POWER] Config: name=%s nominal=%lu active=%lu displayBright=%d",
                config.name, (unsigned long)config.cpuFreqMhz,
                (unsigned long)targetFreq, config.displayBrightnessPercent);

  // Apply CPU frequency
  uint32_t currentFreq = getCpuFrequencyMhz();
  DEBUG_SYSTEMF("[POWER] Current CPU freq: %lu MHz, target: %lu MHz",
                (unsigned long)currentFreq, (unsigned long)targetFreq);
  if (currentFreq != targetFreq) {
    INFO_SYSTEMF("Changing CPU frequency: %lu MHz -> %lu MHz",
                 (unsigned long)currentFreq, (unsigned long)targetFreq);
  }
  if (!powerSetCpuFrequencyMhz(targetFreq)) {
    ERROR_SYSTEMF("CPU frequency change failed: requested %lu MHz, actual %lu MHz",
                  (unsigned long)targetFreq, (unsigned long)getCpuFrequencyMhz());
    return false;
  }
  
  // Apply display brightness. A user mode change sets the mode's brightness;
  // re-applying the saved mode (boot, setup) keeps the saved oledBrightness.
  uint8_t targetBrightness = (config.displayBrightnessPercent * 255) / 100;
  
  // Only apply if different from current setting
  if (setDisplayBrightness && gSettings.oledBrightness != targetBrightness) {
    INFO_SYSTEMF("Adjusting display brightness: %d -> %d (mode: %s)", 
                 gSettings.oledBrightness, targetBrightness, config.name);
    setSetting(gSettings.oledBrightness, (int)targetBrightness);
    
    // Apply to hardware if OLED is enabled
    #if ENABLE_OLED_DISPLAY
      extern void applyOLEDBrightness();
      applyOLEDBrightness();
    #endif
  }
  
  INFO_SYSTEMF("Power mode applied: %s (CPU: %lu MHz, Display: %d%%)",
               config.name, (unsigned long)targetFreq, config.displayBrightnessPercent);

  systemEventPost(SYSEVT_POWER_MODE_CHANGED, config.name);

  // Annotate the battery log with the power-mode change (carries the freq it set).
  {
    extern void batteryLogEvent(const char* event);
    char ev[40];
    snprintf(ev, sizeof(ev), "powermode:%s:%luMHz", config.name, (unsigned long)targetFreq);
    batteryLogEvent(ev);
  }
  return true;
}

// ============================================================================
// Power telemetry JSON — single source of truth
// ============================================================================
// Used by BOTH `power json` (CLI/BLE) and the web /api/power/status, so every
// interface returns the SAME schema. Read-only: it samples live state and
// settings, never mutates and never stamps power-save activity (a polling web
// page must not hold the device out of idle power-save — see the
// SOURCE_INTERNAL/SOURCE_UART carve-out in executeCommand).
//
// The `modes` array is what lets a UI render the preset picker without
// re-encoding the gPowerModes table: name, the clock actually applied while
// the device is in use, the clock idle power-save may sink to, and the mode's
// display brightness. Any duplicated target frequency table in
// JavaScript is a duplicate of this table waiting to drift.
//
// No secrets; safe on any transport.
void buildPowerJson(JsonDocument& doc) {
  const uint8_t mode = gSettings.powerMode;

  doc["schema"]            = 2;
  doc["mode"]              = mode;
  doc["modeName"]          = getPowerModeName(mode);
  // Live HAL read — shows the policy's downclock or boost, not just the mode's
  // table value. cpuMhz differing from targetMhz means a change is pending.
  doc["cpuMhz"]            = (unsigned long)getCpuFrequencyMhz();
  doc["targetMhz"]         = (unsigned long)powerTargetCpuMhz();
  const char* boost = powerWorkloadBoost();
  doc["boost"]             = boost ? boost : "none";
  doc["xtalMhz"]           = (unsigned long)getXtalFrequencyMhz();
  doc["apbMhz"]            = (unsigned long)(getApbFrequency() / 1000000UL);
  doc["activeMhz"]         = (unsigned long)getPowerModeActiveCpuFreq(mode);
  doc["idleMhz"]           = (unsigned long)getPowerModeIdleCpuFreq(mode);
  doc["interactiveFloorMhz"] = (unsigned long)POWER_INTERACTIVE_FLOOR_MHZ;
  doc["maxMhz"]            = (unsigned long)POWER_CPU_PERFORMANCE_MHZ;
  JsonArray frequencies = doc["supportedCpuMhz"].to<JsonArray>();
  for (size_t i = 0; i < getPowerCpuFrequencyCount(); ++i) {
    frequencies.add(getPowerCpuFrequencyMhz(i));
  }

  // No battery-driven mode switch exists in this firmware; say so rather than
  // reporting a stored flag that nothing acts on.
  doc["autoMode"]          = false;
  doc["autoModeSupported"] = false;
  doc["displayBrightness"] = gSettings.oledBrightness;          // 0..255
  doc["displayDimLevel"]   = gSettings.powerDisplayDimLevel;    // percent

  // Idle timeout. The clock drops to the mode's idle clock after it on every
  // build; the local panel also blanks where there is one (`displayPowerSave`
  // tells the UI whether to describe that part).
  doc["powerSaveMinutes"]  = (unsigned long)gSettings.powerSaveTimeoutMinutes;
#if ENABLE_OLED_DISPLAY
  doc["displayPowerSave"]  = true;
#else
  doc["displayPowerSave"]  = false;
#endif
  doc["localDisplayAwake"] = powerLocalDisplayAwake();
  doc["idleMsAgo"]         = (unsigned long)(millis() - powerSaveLastActivityMs());

  // Sleep gating: why a lightsleep/deepsleep request would be refused right now.
  unsigned long cooldownRemain = 0;
  const bool sleepAllowed = powerSleepTransitionAllowed(&cooldownRemain);
  doc["cooldownMs"]          = (unsigned long)gSettings.powerTransitionCooldownMs;
  doc["cooldownRemainingMs"] = cooldownRemain;
  doc["otaProbation"]        = otaSafetyIsPendingVerification();
  doc["sleepAllowed"]        = sleepAllowed && !otaSafetyIsPendingVerification();

  JsonArray modes = doc["modes"].to<JsonArray>();
  for (uint8_t i = 0; i < POWER_MODE_COUNT; i++) {
    JsonObject m = modes.add<JsonObject>();
    m["index"]      = i;
    m["name"]       = gPowerModes[i].name;
    m["activeMhz"]  = (unsigned long)getPowerModeActiveCpuFreq(i);
    m["idleMhz"]    = (unsigned long)getPowerModeIdleCpuFreq(i);
    m["brightness"] = gPowerModes[i].displayBrightnessPercent;
  }
}

// ============================================================================
// CLI Commands
// ============================================================================

const char* cmd_power(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Structured path: the whole power snapshot as one verbatim JSON blob, no
  // broadcastOutput (interleaving the stream and the blob makes it
  // unparseable). Same builder the web /api/power/status uses.
  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    buildPowerJson(doc);
    // Worst case (widest numbers, longest mode names, "recording" boost)
    // serializes to about 910 B and the `modes` table is fixed-size, so it
    // cannot grow at runtime. 1 KB leaves over 100 B of headroom; serializeJson
    // silently TRUNCATES to fit, which would emit invalid JSON rather than
    // failing loudly. test_power_save_tick.py measures the worst case and
    // fails below 64 B of headroom.
    static char* jbuf = nullptr;
    if (!jbuf) jbuf = (char*)ps_alloc(1024, AllocPref::PreferPSRAM, "power.json");
    if (!jbuf) return "{\"error\":\"oom\"}";
    serializeJson(doc, jbuf, 1024);
    return jbuf;
  }
  
  DEBUG_SYSTEMF("[POWER_CMD] cmd_power called with: '%s'", argsInput.c_str());
  
  
  // Parse command args: "" or "mode [name]" or "auto [on|off]"
  CommandArgs a(argsInput);
  DEBUG_SYSTEMF("[POWER_CMD] args after trim: '%s'", a.raw().c_str());

  if (a.count() == 0) {
    // Show current power status
    broadcastOutput("Power Management Status:");
    BROADCAST_PRINTF("  Mode: %s (CPU: %lu MHz)",
                     getPowerModeName(gSettings.powerMode),
                     (unsigned long)getCpuFrequencyMhz());
    // Why the policy wants that clock: locked, a busy workload (stt / camera /
    // recording), active (in use) or idle.
    BROADCAST_PRINTF("  Target: %lu MHz (%s)", (unsigned long)powerTargetCpuMhz(),
                     powerTargetReason());
    BROADCAST_PRINTF("  Display Brightness: %d/255 (%d%%)",
                     gSettings.oledBrightness,
                     (gSettings.oledBrightness * 100) / 255);
    broadcastOutput("  Auto Mode: not available in this firmware");
    broadcastOutput("\nAvailable modes:");
    for (uint8_t i = 0; i < POWER_MODE_COUNT; i++) {
      const uint32_t act  = getPowerModeActiveCpuFreq(i);
      const uint32_t idle = getPowerModeIdleCpuFreq(i);
      if (idle < act) {
        // UltraSaver: active clock plus its idle-only deep floor.
        BROADCAST_PRINTF("  %d: %s (CPU: %lu MHz, %lu MHz idle, Display: %d%%)",
                         i, gPowerModes[i].name,
                         (unsigned long)act, (unsigned long)idle,
                         gPowerModes[i].displayBrightnessPercent);
      } else {
        BROADCAST_PRINTF("  %d: %s (CPU: %lu MHz, Display: %d%%)",
                         i, gPowerModes[i].name,
                         (unsigned long)act,
                         gPowerModes[i].displayBrightnessPercent);
      }
    }
    BROADCAST_PRINTF("  (Every mode runs transcription, camera and recording at %lu MHz. Locked holds %lu MHz through idle; UltraSaver uses %lu MHz only after the idle timeout, once the screen has blanked where there is one; see 'powersave')",
                     (unsigned long)POWER_CPU_PERFORMANCE_MHZ,
                     (unsigned long)getPowerModeActiveCpuFreq(POWER_MODE_LOCKED),
                     (unsigned long)getPowerModeIdleCpuFreq(POWER_MODE_ULTRASAVER));
    return "[Power] Status displayed";
  }

  // Parse subcommand
  String subCmd = a.arg(0);
  String subArgs = a.remaining(0);

  if (subCmd.equalsIgnoreCase("mode")) {
    if (subArgs.length() == 0) {
      return "Error: invalid arguments — Usage: power mode [perf|balanced|adaptive|ultra|locked|0-4]";
    }

    uint8_t newMode = 255;

    // Parse mode name or number
    if (subArgs.equalsIgnoreCase("perf") || subArgs.equalsIgnoreCase("performance")) {
      newMode = POWER_MODE_PERFORMANCE;
    } else if (subArgs.equalsIgnoreCase("balanced") || subArgs.equalsIgnoreCase("bal")) {
      newMode = POWER_MODE_BALANCED;
    } else if (subArgs.equalsIgnoreCase("adaptive")) {
      newMode = POWER_MODE_ADAPTIVE;
    } else if (subArgs.equalsIgnoreCase("ultra") || subArgs.equalsIgnoreCase("ultrasaver")) {
      newMode = POWER_MODE_ULTRASAVER;
    } else if (subArgs.equalsIgnoreCase("locked") || subArgs.equalsIgnoreCase("lock") ||
               subArgs.equalsIgnoreCase("max")) {
      newMode = POWER_MODE_LOCKED;
    } else {
      // Try parsing as number
      int modeNum = subArgs.toInt();
      if (modeNum >= 0 && modeNum < POWER_MODE_COUNT) {
        newMode = modeNum;
      }
    }
    
    if (newMode >= POWER_MODE_COUNT) {
      return "Error: Invalid mode. Use: perf, balanced, adaptive, ultra, locked, or 0-4";
    }
    
    const uint8_t previousMode = gSettings.powerMode;
    setSetting(gSettings.powerMode, (uint8_t)newMode);
    if (!applyPowerMode(newMode)) {
      setSetting(gSettings.powerMode, previousMode);
      return "Error: CPU frequency change failed; previous power mode restored";
    }
    
    BROADCAST_PRINTF("Power mode set to: %s", getPowerModeName(newMode));
    return "[Power] Mode updated";
    
  } else if (subCmd.equalsIgnoreCase("auto") || subCmd.equalsIgnoreCase("threshold")) {
    // Nothing switches the mode on battery level in this firmware, so neither
    // auto mode nor its threshold is stored or reported as active.
    if (subCmd.equalsIgnoreCase("auto") &&
        (subArgs.equalsIgnoreCase("off") || subArgs.equalsIgnoreCase("false") || subArgs == "0")) {
      return "[Power] Auto mode is off (not available in this firmware)";
    }
    return "Error: auto power mode is not available in this firmware; choose a mode with 'power mode'";

  } else {
    return "Error: Unknown subcommand. Use: power [mode]";
  }
}

// ============================================================================
// Sleep transition cooldown ("anti-flap")
// ============================================================================
// Static state — one timestamp shared by light-sleep and deep-sleep entry
// callers. millis() rolls over after ~49 days; the comparison uses unsigned
// subtraction which handles the wrap correctly so long as the cooldown is
// less than half the range (we cap the setting at 60s, far below 24 days).
static unsigned long sLastSleepTransitionMs = 0;

bool powerSleepTransitionAllowed(unsigned long* outRemainingMs) {
  const uint32_t cooldown = gSettings.powerTransitionCooldownMs;
  if (cooldown == 0) {
    if (outRemainingMs) *outRemainingMs = 0;
    return true;  // explicitly disabled
  }
  if (sLastSleepTransitionMs == 0) {
    if (outRemainingMs) *outRemainingMs = 0;
    return true;  // no previous transition recorded — first call always permitted
  }
  const unsigned long now = millis();
  const unsigned long elapsed = now - sLastSleepTransitionMs;  // unsigned: handles rollover
  if (elapsed >= cooldown) {
    if (outRemainingMs) *outRemainingMs = 0;
    return true;
  }
  if (outRemainingMs) *outRemainingMs = cooldown - elapsed;
  return false;
}

void powerSleepTransitionMark() {
  sLastSleepTransitionMs = millis();
  // Avoid the sentinel value: if millis() happens to return 0 on the first
  // call after boot (extremely unlikely but harmless to guard), bump by 1.
  if (sLastSleepTransitionMs == 0) sLastSleepTransitionMs = 1;
}

// ----------------------------------------------------------------------------
// Activity timestamps. Stamped by powerSaveNoteActivity() from any task (the
// input poll, the command executor, ...). A 32-bit aligned read/write is
// atomic on the ESP32, so no lock is needed for cross-task use. The local
// stamp is a subset of the general one: someone at the device.
static volatile unsigned long sPowerSaveLastActivityMs = 0;
static volatile unsigned long sPowerSaveLastLocalActivityMs = 0;
void powerSaveNoteActivity(bool local) {
  const unsigned long now = millis();
  sPowerSaveLastActivityMs = now;
  if (local) sPowerSaveLastLocalActivityMs = now;
}
unsigned long powerSaveLastActivityMs()      { return sPowerSaveLastActivityMs; }
unsigned long powerSaveLastLocalActivityMs() { return sPowerSaveLastLocalActivityMs; }

// ============================================================================
// Clock policy
// ============================================================================
// Demand is read from the workloads' own busy flags on every pass instead of
// being held as a request. Each flag stays set until its work has fully
// finished — the STT workers clear theirs as their last step before
// vTaskDelete, after the transcript is saved; recording's covers the file's
// finalization — and a worker that is cancelled, fails to start or deletes
// its own task (vTaskDelete runs no destructors) clears it too. So there is
// nothing to release and nothing that can leak.
const char* powerWorkloadBoost() {
#if ENABLE_LOCAL_STT
  // Real-time inference on this CPU: at the floor clock (100 MHz on the P4)
  // it runs ~3x slower than speech and falls behind (measured 2026-10-01).
  extern bool sttAnySessionActive();
  if (sttAnySessionActive()) return "stt";
#endif
#if ENABLE_CAMERA_SENSOR
  extern bool cameraCaptureBusy();   // open stream or recording session
  if (cameraCaptureBusy()) return "camera";
#endif
#if ENABLE_MICROPHONE
  extern bool micRecordingBusy();    // STARTING through FINALIZING
  if (micRecordingBusy()) return "recording";
#endif
  return nullptr;
}

static volatile bool sLocalDisplayAwake = false;
void powerSetLocalDisplayAwake(bool awake) { sLocalDisplayAwake = awake; }
bool powerLocalDisplayAwake()              { return sLocalDisplayAwake; }

// Why the policy wants the clock it wants; the battery log and `power` show it.
struct PowerDecision {
  uint32_t mhz;
  const char* reason;   // "locked", a boost name, "active" or "idle"
  bool mandatory;       // re-asserted every lap (Locked, a boost), not applied once on change
};

static PowerDecision powerDecide() {
  const uint8_t mode = gSettings.powerMode;
  if (mode == POWER_MODE_LOCKED) return {POWER_CPU_PERFORMANCE_MHZ, "locked", true};
  if (const char* boost = powerWorkloadBoost()) return {POWER_CPU_PERFORMANCE_MHZ, boost, true};

  // In use: any real interaction within the idle timeout, the timeout
  // disabled, or the local panel still lit (its blanking can lag the countdown
  // behind the shared sleep cooldown, and UltraSaver's 40 MHz is idle-only).
  // The elapsed time is a 32-bit difference so it stays right across the
  // millis() wrap (and the host tests see the target's arithmetic).
  const uint32_t timeoutMs = gSettings.powerSaveTimeoutMinutes * 60000UL;
  const uint32_t sinceActivityMs = (uint32_t)(millis() - powerSaveLastActivityMs());
  const bool inUse = timeoutMs == 0 || sLocalDisplayAwake || sinceActivityMs < timeoutMs;
  if (inUse) return {getPowerModeActiveCpuFreq(mode), "active", false};

  uint32_t idleMhz = getPowerModeIdleCpuFreq(mode);
  // A UART host link running above REF_TICK is clocked from APB, and on chips
  // whose HAL has that fallback (ESP32 / ESP32-S2) APB follows the CPU clock
  // once it sinks below 80 MHz. UltraSaver's 40 MHz idle floor would therefore
  // halve the link's effective baud and corrupt every byte — including the
  // CM5's heartbeat, so freshness would lapse with no way to recover: UART
  // traffic deliberately does not count as activity (see the SOURCE_UART
  // exclusion in executeCommand), and a garbled command cannot be parsed into
  // a wake in the first place. Clamp to the interactive floor instead; 240/160/
  // 80 are all PLL-derived and pin APB at 80 MHz, so 80 is enough — the other
  // modes already idle there. Costs UltraSaver some deep-idle saving only
  // while a fast link is actually up.
#if SOC_UART_SUPPORT_REF_TICK
  const bool clamp = idleMhz < POWER_INTERACTIVE_FLOOR_MHZ && uartLinkIsRunning() &&
                     uartLinkEffectiveBaud() > UART_LINK_REF_TICK_BAUD_LIMIT;
  static bool sClampSaid = false;   // decided every lap: say it once per fast link
  if (clamp && !sClampSaid) {
    DEBUG_SYSTEMF("[POWER] idle floor %lu -> %lu MHz: UART link at %d baud is "
                  "APB-clocked and cannot survive a sub-80 MHz CPU",
                  (unsigned long)idleMhz,
                  (unsigned long)POWER_INTERACTIVE_FLOOR_MHZ,
                  uartLinkEffectiveBaud());
  }
  sClampSaid = clamp;
  if (clamp) idleMhz = POWER_INTERACTIVE_FLOOR_MHZ;
#endif
  return {idleMhz, "idle", false};
}

uint32_t powerTargetCpuMhz()    { return powerDecide().mhz; }
const char* powerTargetReason() { return powerDecide().reason; }

void powerPolicyTick() {
  static uint32_t appliedMhz = 0;      // last target this tick confirmed; 0 = none yet
  static bool     retryPending = false;
  static uint32_t refusedMhz = 0;      // the target the owner refused ...
  static uint32_t nextAttemptMs = 0;   // ... and when it may be tried again
  static bool     inited = false;
  // A refused change means the shared owner could not take its gate or drain
  // an I2C bus within its wait. Retrying on the very next lap would pin the
  // whole loop to that wait; space the attempts instead. A raise retries
  // sooner: a transcription may already be running on too slow a clock.
  constexpr uint32_t kRaiseRetryMs = 500;
  constexpr uint32_t kLowerRetryMs = 5000;

  // 32-bit on purpose: millis() wraps at 2^32 ms, and the backoff below is
  // only ever compared while retryPending, so the difference never spans more
  // than one backoff. (A gate compared against a stamp from before the wrap
  // would otherwise freeze this tick for 24.85 days.)
  const uint32_t now = (uint32_t)millis();
  if (!inited) {
    // Boot counts as use: the idle countdown starts at the first tick, not at
    // millis() == 0, so a slow boot cannot land straight in idle.
    inited = true;
    powerSaveNoteActivity(false);
  }

  const PowerDecision decision = powerDecide();
  const uint32_t before = getCpuFrequencyMhz();
  // Locked and a boost are re-asserted: a `power mode` or `cpufreq` issued
  // while a workload runs must not leave it at the lower clock. "active" and
  // "idle" act only when the target changes, so a manual clock holds there
  // until the policy itself wants something new.
  const bool due = decision.mandatory ? before != decision.mhz : decision.mhz != appliedMhz;
  if (!due) {
    appliedMhz = decision.mhz;
    return;
  }
  if (retryPending) {
    // Only the refused target waits out its backoff; a new target is a new attempt.
    if (decision.mhz == refusedMhz && (int32_t)(now - nextAttemptMs) < 0) return;
    retryPending = false;
  }
  if (before != decision.mhz && !powerSetCpuFrequencyMhz(decision.mhz)) {
    retryPending = true;
    refusedMhz = decision.mhz;
    nextAttemptMs = now + (decision.mhz > before ? kRaiseRetryMs : kLowerRetryMs);
    // The HAL can move the clock and still report failure. Remember what is
    // really there, so the next lap judges against reality, not the old target.
    appliedMhz = getCpuFrequencyMhz();
    return;
  }
  appliedMhz = decision.mhz;
  if (before == decision.mhz) return;   // already there (applyPowerMode / cpufreq set it)
  DEBUG_SYSTEMF("[POWER] clock %lu -> %lu MHz (%s)", (unsigned long)before,
                (unsigned long)decision.mhz, decision.reason);
  {
    extern void batteryLogEvent(const char* event);
    char ev[32];
    snprintf(ev, sizeof(ev), "cpu:%s:%luMHz", decision.reason, (unsigned long)decision.mhz);
    batteryLogEvent(ev);
  }
}

// CLI: powercooldown [ms]
//   no arg → print current value
//   ms     → set new cooldown (0..60000); persisted via setSetting
static const char* cmd_powercooldown(const String& argsInput) {
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    snprintf(getDebugBuffer(), 1024,
             "Sleep transition cooldown: %lu ms (%s)",
             (unsigned long)gSettings.powerTransitionCooldownMs,
             gSettings.powerTransitionCooldownMs == 0 ? "disabled" : "active");
    return getDebugBuffer();
  }
  long v = arg.toInt();
  if (v < 0 || v > 60000) return "Error: invalid arguments — Usage: powercooldown <0..60000>  (ms; 0 disables)";
  setSetting(gSettings.powerTransitionCooldownMs, (uint32_t)v);
  snprintf(getDebugBuffer(), 1024,
           "Sleep transition cooldown set to %ld ms%s",
           v, v == 0 ? " (disabled)" : "");
  return getDebugBuffer();
}

// CLI: powersave [minutes]
//   no arg  → print current value
//   minutes → idle timeout before the OLED blanks + the CPU downclocks
//             (0..1440); 0 disables. Persisted via setSetting. The radio stays
//             up (CPU drops to its mode-dependent idle floor) so the device stays
//             reachable while the screen is dark.
static const char* cmd_powersave(const String& argsInput) {
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    snprintf(getDebugBuffer(), 1024,
             "Power saving: %lu min (%s)",
             (unsigned long)gSettings.powerSaveTimeoutMinutes,
             gSettings.powerSaveTimeoutMinutes == 0 ? "disabled"
                                                    : "blanks OLED; CPU downclock is mode-dependent");
    return getDebugBuffer();
  }
  long v = arg.toInt();
  if (v < 0 || v > 1440) return "Error: invalid arguments — Usage: powersave <0..1440>  (minutes; 0 disables)";
  setSetting(gSettings.powerSaveTimeoutMinutes, (uint32_t)v);
  snprintf(getDebugBuffer(), 1024,
           "Power saving set to %ld min%s",
           v, v == 0 ? " (disabled)" : "");
  return getDebugBuffer();
}

// Command table
// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry powerCommands[] = {
  {"power",         "Power management [mode]", true, cmd_power,         "Usage:\n  power - show current power status\n  power json - status as one JSON blob (same schema as /api/power/status)\n  power mode <perf|balanced|adaptive|ultra|locked|0-4>"},
  {"powercooldown", "Sleep transition cooldown (ms; 0 disables)", true, cmd_powercooldown, "Usage: powercooldown <0..60000>"},
  {"powersave",     "Idle power-save: OLED off + optional downclock (0 disables)", true, cmd_powersave, "Usage: powersave <0..1440>"}
};

const size_t powerCommandsCount = sizeof(powerCommands) / sizeof(powerCommands[0]);

// ============================================================================
// Modular Settings Registration
// ============================================================================

// Power module is always available (CPU frequency control is always present)
static bool isPowerModuleConnected() {
  return true;  // Power management always available
}

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry powerSettingEntries[] = {
  // Fields are uint8_t — must use SETTING_U8 to avoid 4-byte-write overflow
  // into adjacent struct members. See System_Settings.h SettingType enum.
  { "mode", SETTING_U8, &gSettings.powerMode, POWER_MODE_DEFAULT, 0, nullptr, 0, POWER_MODE_COUNT - 1, "Power Mode", "Performance,Balanced,Adaptive,UltraSaver,Locked", false, nullptr, "power mode" },
  { "displayDimLevel", SETTING_U8, &gSettings.powerDisplayDimLevel, 30, 0, nullptr, 0, 100, "Display Dim Level (%)", nullptr, false, nullptr, "powerdim" },
  { "transitionCooldownMs", SETTING_INT, &gSettings.powerTransitionCooldownMs, 5000, 0, nullptr, 0, 60000, "Sleep cooldown (ms, 0=disabled)", nullptr, false, nullptr, "powercooldown" },
  { "powerSaveMinutes", SETTING_INT, &gSettings.powerSaveTimeoutMinutes, 10, 0, nullptr, 0, 1440, "Power saving (min, 0=disabled)", nullptr, false, nullptr, "powersave" }
};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule powerSettingsModule = {
  "power",
  "system.power",
  powerSettingEntries,
  sizeof(powerSettingEntries) / sizeof(powerSettingEntries[0]),
  isPowerModuleConnected,
  "CPU frequency scaling and battery optimization"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp
