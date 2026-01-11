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
extern Settings gSettings;

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
  Serial.printf("[POWER] applyPowerMode called with mode=%d\n", mode);
  
  if (mode >= POWER_MODE_COUNT) {
    ERROR_SYSTEMF("Invalid power mode: %d", mode);
    return;
  }
  
  const PowerModeConfig& config = gPowerModes[mode];
  Serial.printf("[POWER] Config: name=%s cpuFreq=%lu displayBright=%d\n", 
                config.name, (unsigned long)config.cpuFreqMhz, config.displayBrightnessPercent);
  
  // Apply CPU frequency
  uint32_t currentFreq = getCpuFrequencyMhz();
  Serial.printf("[POWER] Current CPU freq: %lu MHz, target: %lu MHz\n", 
                (unsigned long)currentFreq, (unsigned long)config.cpuFreqMhz);
  if (currentFreq != config.cpuFreqMhz) {
    INFO_SYSTEMF("Changing CPU frequency: %lu MHz -> %lu MHz", 
                 (unsigned long)currentFreq, (unsigned long)config.cpuFreqMhz);
    setCpuFrequencyMhz(config.cpuFreqMhz);
    Serial.printf("[POWER] After setCpuFrequencyMhz, actual freq: %lu MHz\n", 
                  (unsigned long)getCpuFrequencyMhz());
  } else {
    Serial.println("[POWER] CPU frequency already at target, skipping");
  }
  
  // Apply display brightness
  extern Settings gSettings;
  uint8_t targetBrightness = (config.displayBrightnessPercent * 255) / 100;
  
  // Only apply if different from current setting
  if (gSettings.oledBrightness != targetBrightness) {
    INFO_SYSTEMF("Adjusting display brightness: %d -> %d (mode: %s)", 
                 gSettings.oledBrightness, targetBrightness, config.name);
    gSettings.oledBrightness = targetBrightness;
    
    // Apply to hardware if OLED is enabled
    #if ENABLE_OLED_DISPLAY
      extern void applyOLEDBrightness();
      applyOLEDBrightness();
    #endif
  }
  
  INFO_SYSTEMF("Power mode applied: %s (CPU: %lu MHz, Display: %d%%)",
               config.name, (unsigned long)config.cpuFreqMhz, config.displayBrightnessPercent);
}

void checkAutoPowerMode() {
  extern Settings gSettings;
  
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

const char* cmd_power(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  Serial.printf("[POWER_CMD] cmd_power called with: '%s'\n", originalCmd.c_str());
  
  extern Settings gSettings;
  
  // Parse command: "power" or "power mode [name]" or "power auto [on|off]"
  String args = originalCmd.substring(5);  // Skip "power"
  args.trim();
  Serial.printf("[POWER_CMD] args after trim: '%s'\n", args.c_str());
  
  if (args.length() == 0) {
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
  int spaceIdx = args.indexOf(' ');
  String subCmd = spaceIdx > 0 ? args.substring(0, spaceIdx) : args;
  String subArgs = spaceIdx > 0 ? args.substring(spaceIdx + 1) : "";
  subCmd.trim();
  subArgs.trim();
  
  if (subCmd.equalsIgnoreCase("mode")) {
    if (subArgs.length() == 0) {
      return "Error: Usage: power mode [perf|balanced|saver|ultra|0-3]";
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
    
    Serial.printf("[POWER_CMD] Setting gSettings.powerMode to %d\n", newMode);
    gSettings.powerMode = newMode;
    Serial.println("[POWER_CMD] Calling applyPowerMode...");
    applyPowerMode(newMode);
    Serial.println("[POWER_CMD] Calling writeSettingsJson...");
    writeSettingsJson();
    Serial.println("[POWER_CMD] writeSettingsJson completed");
    
    BROADCAST_PRINTF("Power mode set to: %s", getPowerModeName(newMode));
    return "[Power] Mode updated";
    
  } else if (subCmd.equalsIgnoreCase("auto")) {
    if (subArgs.length() == 0) {
      return "Error: Usage: power auto [on|off]";
    }
    
    bool enable = subArgs.equalsIgnoreCase("on") || subArgs.equalsIgnoreCase("true") || subArgs == "1";
    gSettings.powerAutoMode = enable;
    writeSettingsJson();
    
    BROADCAST_PRINTF("Auto power mode: %s", enable ? "ON" : "OFF");
    if (enable) {
      BROADCAST_PRINTF("Will switch to PowerSaver when battery < %d%%", gSettings.powerBatteryThreshold);
    }
    return "[Power] Auto mode updated";
    
  } else if (subCmd.equalsIgnoreCase("threshold")) {
    if (subArgs.length() == 0) {
      return "Error: Usage: power threshold [0-100]";
    }
    
    int threshold = subArgs.toInt();
    if (threshold < 0 || threshold > 100) {
      return "Error: Threshold must be 0-100";
    }
    
    gSettings.powerBatteryThreshold = threshold;
    writeSettingsJson();
    BROADCAST_PRINTF("Battery threshold set to: %d%%", threshold);
    return "[Power] Threshold updated";
    
  } else {
    return "Error: Unknown subcommand. Use: power [mode|auto|threshold]";
  }
}

// Command table
const CommandEntry powerCommands[] = {
  {"power", "Power management (mode, auto, threshold)", false, cmd_power, "Usage: power [mode <0-3>] [auto <on|off>] [threshold <percent>]"}
};

const size_t powerCommandsCount = sizeof(powerCommands) / sizeof(powerCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _power_cmd_registrar(powerCommands, powerCommandsCount, "power");

// ============================================================================
// Modular Settings Registration
// ============================================================================

// Power module is always available (CPU frequency control is always present)
static bool isPowerModuleConnected() {
  return true;  // Power management always available
}

static const SettingEntry powerSettingEntries[] = {
  { "mode", SETTING_INT, &gSettings.powerMode, 0, 0, nullptr, 0, 3, "Power Mode", "Performance,Balanced,PowerSaver,UltraSaver" },
  { "autoMode", SETTING_BOOL, &gSettings.powerAutoMode, 0, 0, nullptr, 0, 1, "Auto Mode", nullptr },
  { "batteryThreshold", SETTING_INT, &gSettings.powerBatteryThreshold, 20, 0, nullptr, 0, 100, "Battery Threshold (%)", nullptr },
  { "displayDimLevel", SETTING_INT, &gSettings.powerDisplayDimLevel, 30, 0, nullptr, 0, 100, "Display Dim Level (%)", nullptr }
};

static const SettingsModule powerSettingsModule = {
  "power",
  "power",
  powerSettingEntries,
  sizeof(powerSettingEntries) / sizeof(powerSettingEntries[0]),
  isPowerModuleConnected,
  "CPU frequency scaling and battery optimization"
};

// Auto-register with settings system
static struct PowerSettingsRegistrar {
  PowerSettingsRegistrar() { registerSettingsModule(&powerSettingsModule); }
} _powerSettingsRegistrar;

void registerPowerSettingsModule() {
  registerSettingsModule(&powerSettingsModule);
}
