/**
 * System Power Management
 * 
 * Handles CPU frequency scaling and display brightness management
 * for battery optimization.
 */

#include <Arduino.h>
#include "System_Power.h"
#include "System_Settings.h"
#include "System_Debug.h"
#include "System_Command.h"

// Forward declarations

// Power mode configuration
struct PowerModeConfig {
  const char* name;
  uint32_t cpuFreqMhz;
  uint8_t displayBrightnessPercent;
};

static const PowerModeConfig gPowerModes[] = {
  {"Performance", 240, 100},  // 0: Full speed
  {"Balanced",    160, 80},   // 1: Good balance
  {"PowerSaver",  80,  50},   // 2: Battery focused
  {"UltraSaver",  40,  30}    // 3: Maximum savings
};

static const uint8_t POWER_MODE_COUNT = 4;

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
    return 240;  // Default to performance
  }
  return gPowerModes[mode].cpuFreqMhz;
}

uint8_t getPowerModeDisplayBrightness(uint8_t mode) {
  if (mode >= POWER_MODE_COUNT) {
    return 100;  // Default to full brightness
  }
  return gPowerModes[mode].displayBrightnessPercent;
}

void applyPowerMode(uint8_t mode) {
  DEBUG_SYSTEMF("[POWER] applyPowerMode called with mode=%d", mode);
  
  if (mode >= POWER_MODE_COUNT) {
    ERROR_SYSTEMF("Invalid power mode: %d", mode);
    return;
  }
  
  const PowerModeConfig& config = gPowerModes[mode];
  DEBUG_SYSTEMF("[POWER] Config: name=%s cpuFreq=%lu displayBright=%d", 
                config.name, (unsigned long)config.cpuFreqMhz, config.displayBrightnessPercent);
  
  // Apply CPU frequency
  uint32_t currentFreq = getCpuFrequencyMhz();
  DEBUG_SYSTEMF("[POWER] Current CPU freq: %lu MHz, target: %lu MHz", 
                (unsigned long)currentFreq, (unsigned long)config.cpuFreqMhz);
  if (currentFreq != config.cpuFreqMhz) {
    INFO_SYSTEMF("Changing CPU frequency: %lu MHz -> %lu MHz", 
                 (unsigned long)currentFreq, (unsigned long)config.cpuFreqMhz);
    setCpuFrequencyMhz(config.cpuFreqMhz);
    DEBUG_SYSTEMF("[POWER] After setCpuFrequencyMhz, actual freq: %lu MHz", 
                  (unsigned long)getCpuFrequencyMhz());
  } else {
    DEBUG_SYSTEMF("[POWER] CPU frequency already at target, skipping");
  }
  
  // Apply display brightness
  uint8_t targetBrightness = (config.displayBrightnessPercent * 255) / 100;
  
  // Only apply if different from current setting
  if (gSettings.oledBrightness != targetBrightness) {
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
               config.name, (unsigned long)config.cpuFreqMhz, config.displayBrightnessPercent);

  // Annotate the battery log with the power-mode change (carries the freq it set).
  {
    extern void batteryLogEvent(const char* event);
    char ev[40];
    snprintf(ev, sizeof(ev), "powermode:%s:%luMHz", config.name, (unsigned long)config.cpuFreqMhz);
    batteryLogEvent(ev);
  }
}

void checkAutoPowerMode() {
  
  if (!gSettings.powerAutoMode) {
    return;  // Auto mode disabled
  }
  
  // TODO: Get battery level from hardware
  // For now, this is a placeholder for future battery monitoring
  // When battery monitoring is implemented, check battery % and switch modes:
  //
  // uint8_t batteryPercent = getBatteryLevel();
  // if (batteryPercent < gSettings.powerBatteryThreshold) {
  //   if (gSettings.powerMode < 2) {  // Not already in power saver
  //     gSettings.powerMode = 2;  // Switch to PowerSaver
  //     applyPowerMode(gSettings.powerMode);
  //     WARN_SYSTEMF("Low battery (%d%%) - switching to PowerSaver mode", batteryPercent);
  //   }
  // }
}

// ============================================================================
// CLI Commands
// ============================================================================

const char* cmd_power(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
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
    BROADCAST_PRINTF("  Display Brightness: %d/255 (%d%%)",
                     gSettings.oledBrightness,
                     (gSettings.oledBrightness * 100) / 255);
    BROADCAST_PRINTF("  Auto Mode: %s", gSettings.powerAutoMode ? "ON" : "OFF");
    if (gSettings.powerAutoMode) {
      BROADCAST_PRINTF("  Battery Threshold: %d%%", gSettings.powerBatteryThreshold);
    }
    broadcastOutput("\nAvailable modes:");
    for (uint8_t i = 0; i < POWER_MODE_COUNT; i++) {
      BROADCAST_PRINTF("  %d: %s (CPU: %lu MHz, Display: %d%%)",
                       i, gPowerModes[i].name, 
                       (unsigned long)gPowerModes[i].cpuFreqMhz,
                       gPowerModes[i].displayBrightnessPercent);
    }
    return "[Power] Status displayed";
  }
  
  // Parse subcommand
  String subCmd = a.arg(0);
  String subArgs = a.remaining(0);
  
  if (subCmd.equalsIgnoreCase("mode")) {
    if (subArgs.length() == 0) {
      return "Error: invalid arguments — Usage: power mode [perf|balanced|saver|ultra|0-3]";
    }
    
    uint8_t newMode = 255;
    
    // Parse mode name or number
    if (subArgs.equalsIgnoreCase("perf") || subArgs.equalsIgnoreCase("performance")) {
      newMode = 0;
    } else if (subArgs.equalsIgnoreCase("balanced") || subArgs.equalsIgnoreCase("bal")) {
      newMode = 1;
    } else if (subArgs.equalsIgnoreCase("saver") || subArgs.equalsIgnoreCase("powersaver")) {
      newMode = 2;
    } else if (subArgs.equalsIgnoreCase("ultra") || subArgs.equalsIgnoreCase("ultrasaver")) {
      newMode = 3;
    } else {
      // Try parsing as number
      int modeNum = subArgs.toInt();
      if (modeNum >= 0 && modeNum < POWER_MODE_COUNT) {
        newMode = modeNum;
      }
    }
    
    if (newMode >= POWER_MODE_COUNT) {
      return "Error: Invalid mode. Use: perf, balanced, saver, ultra, or 0-3";
    }
    
    DEBUG_SYSTEMF("[POWER_CMD] Setting gSettings.powerMode to %d", newMode);
    setSetting(gSettings.powerMode, (uint8_t)newMode);
    DEBUG_SYSTEMF("[POWER_CMD] Calling applyPowerMode...");
    applyPowerMode(newMode);
    
    BROADCAST_PRINTF("Power mode set to: %s", getPowerModeName(newMode));
    return "[Power] Mode updated";
    
  } else if (subCmd.equalsIgnoreCase("auto")) {
    if (subArgs.length() == 0) {
      return "Error: invalid arguments — Usage: power auto [on|off]";
    }
    
    bool enable = subArgs.equalsIgnoreCase("on") || subArgs.equalsIgnoreCase("true") || subArgs == "1";
    setSetting(gSettings.powerAutoMode, enable);
    
    BROADCAST_PRINTF("Auto power mode: %s", enable ? "ON" : "OFF");
    if (enable) {
      BROADCAST_PRINTF("Will switch to PowerSaver when battery < %d%%", gSettings.powerBatteryThreshold);
    }
    return "[Power] Auto mode updated";
    
  } else if (subCmd.equalsIgnoreCase("threshold")) {
    if (subArgs.length() == 0) {
      return "Error: invalid arguments — Usage: power threshold [0-100]";
    }
    
    int threshold = subArgs.toInt();
    if (threshold < 0 || threshold > 100) {
      return "Error: Threshold must be 0-100";
    }
    
    setSetting(gSettings.powerBatteryThreshold, (uint8_t)threshold);
    BROADCAST_PRINTF("Battery threshold set to: %d%%", threshold);
    return "[Power] Threshold updated";
    
  } else {
    return "Error: Unknown subcommand. Use: power [mode|auto|threshold]";
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
// Adaptive power-save activity timestamp. Stamped by powerSaveNoteActivity()
// from any task (the input poll, the command executor, ...). A 32-bit aligned
// read/write is atomic on the ESP32, so no lock is needed for cross-task use.
static volatile unsigned long sPowerSaveLastActivityMs = 0;
void powerSaveNoteActivity()            { sPowerSaveLastActivityMs = millis(); }
unsigned long powerSaveLastActivityMs() { return sPowerSaveLastActivityMs; }

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
//             up (CPU only drops to the 80 MHz WiFi floor) so the device stays
//             reachable while the screen is dark.
static const char* cmd_powersave(const String& argsInput) {
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    snprintf(getDebugBuffer(), 1024,
             "Power saving: %lu min (%s)",
             (unsigned long)gSettings.powerSaveTimeoutMinutes,
             gSettings.powerSaveTimeoutMinutes == 0 ? "disabled"
                                                    : "blanks OLED + downclocks when idle");
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
// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry powerCommands[] = {
  {"power",         "Power management [mode] [auto] [threshold]", false, cmd_power,         "Usage:\n  power - show current power status\n  power mode <perf|balanced|saver|ultra|0-3>\n  power auto <on|off>\n  power threshold <0-100>"},
  {"powercooldown", "Sleep transition cooldown (ms; 0 disables)", false, cmd_powercooldown, "Usage: powercooldown <0..60000>"},
  {"powersave",     "Idle power-save: OLED off + downclock (0 disables)", false, cmd_powersave, "Usage: powersave <0..1440>"}
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
  { "mode", SETTING_U8, &gSettings.powerMode, 0, 0, nullptr, 0, 3, "Power Mode", "Performance,Balanced,PowerSaver,UltraSaver", false, nullptr, "power mode" },
  { "autoMode", SETTING_BOOL, &gSettings.powerAutoMode, false, 0, nullptr, 0, 1, "Auto Mode", nullptr, false, nullptr, "power auto" },
  { "batteryThreshold", SETTING_U8, &gSettings.powerBatteryThreshold, 20, 0, nullptr, 0, 100, "Battery Threshold (%)", nullptr, false, nullptr, "power threshold" },
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
