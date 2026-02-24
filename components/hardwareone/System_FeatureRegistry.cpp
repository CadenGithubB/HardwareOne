/**
 * System Feature Registry Implementation
 * 
 * Centralized registry of all system features with heap cost estimates
 */

#include "System_FeatureRegistry.h"
#include "System_Settings.h"
#include "System_Command.h"
#include "System_MemUtil.h"
#include <esp_heap_caps.h>

// External settings
extern Settings gSettings;

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
  // Only show NeoPixel if hardware is configured (pin and count set)
#if defined(NEOPIXEL_PIN_DEFAULT) && NEOPIXEL_PIN_DEFAULT >= 0 && NEOPIXEL_COUNT_DEFAULT > 0
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
#if ENABLE_MICROPHONE_SENSOR
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

static bool isGamepadCompiled() {
#if ENABLE_GAMEPAD_SENSOR
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

// ============================================================================
// Feature Registry - All System Features
// ============================================================================
// Heap estimates are approximate and include:
// - Task stack (typically 4-8KB per task)
// - Driver/library buffers
// - Runtime data structures

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
    &gSettings.bluetoothAutoStart, isBluetoothCompiled,
    "BLE server for remote control" },
    
  { "espnow", "ESP-NOW", FEATURE_CAT_NETWORK, 8,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.espnowenabled, isEspNowCompiled,
    "Device-to-device mesh communication" },
    
  { "mqtt", "MQTT", FEATURE_CAT_NETWORK, 6,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.mqttAutoStart, isMqttCompiled,
    "Home Assistant integration via MQTT broker" },

  // === DISPLAY FEATURES ===
  { "oled", "OLED Display", FEATURE_CAT_DISPLAY, 4,
    FEATURE_FLAG_REQUIRES_REBOOT | FEATURE_FLAG_ESSENTIAL,
    &gSettings.oledEnabled, isOledCompiled,
    "128x64 OLED display interface" },
    
  { "led", "NeoPixel LED", FEATURE_CAT_DISPLAY, 2,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.ledStartupEnabled, isNeoPixelCompiled,
    "RGB LED strip/ring effects" },

  // === SENSOR FEATURES ===
  { "gamepad", "Gamepad", FEATURE_CAT_SENSOR, 2,
    FEATURE_FLAG_RUNTIME_TOGGLE | FEATURE_FLAG_ESSENTIAL,
    &gSettings.gamepadAutoStart, isGamepadCompiled,
    "Seesaw gamepad for navigation (required for OLED)" },
    
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

  { "camera", "Camera", FEATURE_CAT_SENSOR, 18,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.cameraAutoStart, isCameraCompiled,
    "ESP32-S3 camera sensor (XIAO ESP32S3 Sense)" },

  { "microphone", "Microphone", FEATURE_CAT_SENSOR, 4,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.microphoneAutoStart, isMicrophoneCompiled,
    "ESP32-S3 PDM microphone (XIAO ESP32S3 Sense)" },

  { "espsr", "Speech Recognition", FEATURE_CAT_SENSOR, 48,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.srAutoStart, isESPSRCompiled,
    "ESP-SR voice commands (requires microphone)" },

  { "edgeimpulse", "Edge Impulse ML", FEATURE_CAT_SENSOR, 32,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.edgeImpulseEnabled, isEdgeImpulseCompiled,
    "ML inference for object detection (requires camera)" },

  // === HARDWARE FEATURES (shown on first page) ===
  { "i2c", "I2C Bus", FEATURE_CAT_NETWORK, 4,
    FEATURE_FLAG_REQUIRES_REBOOT,
    &gSettings.i2cBusEnabled, isI2CCompiled,
    "I2C hardware bus (required for OLED and sensors)" },
    
#if ENABLE_AUTOMATION
  { "automation", "Automations", FEATURE_CAT_SYSTEM, 8,
    FEATURE_FLAG_RUNTIME_TOGGLE,
    &gSettings.automationsEnabled, isAutomationCompiled,
    "Scheduled tasks and conditional logic" },
#endif
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

const char* cmd_features(const String& argsIn) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String args = argsIn;
  args.trim();
  
  // No args - show all features with heap estimates
  if (args.length() == 0) {
    PSRAM_STATIC_BUF(buf, 2048);
    uint32_t freeHeapKB = ESP.getFreeHeap() / 1024;
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
        status = "N/A";
      } else if (enabled) {
        status = "[ON]";
      } else {
        status = "off";
      }
      
      const char* essential = (f->flags & FEATURE_FLAG_ESSENTIAL) ? "*" : " ";
      
      pos += snprintf(buf + pos, buf_SIZE - pos,
        " %s%-12s ~%2dKB  %s\n",
        essential, f->id, f->heapCostKB, status);
    }
    
    pos += snprintf(buf + pos, buf_SIZE - pos,
      "\n═══════════════════════════════════════════\n"
      "Enabled: ~%luKB | Free: %luKB | Max: ~%luKB\n"
      "* = essential (should stay enabled)\n"
      "Usage: features <id> <on|off>",
      (unsigned long)enabledCost, (unsigned long)freeHeapKB,
      (unsigned long)getTotalPossibleHeapCost());
    
    return buf;
  }
  
  // Parse args: <id> [on|off]
  int secondSpace = args.indexOf(' ');
  if (secondSpace < 0) {
    // Single arg - show feature details
    const FeatureEntry* f = getFeatureById(args.c_str());
    if (!f) {
      return "Unknown feature. Run 'features' to see list.";
    }
    
    static char buf[512];
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
      canToggleFeature(f) ? "yes" : "no (compile-time or essential)",
      f->description);
    
    return buf;
  }
  
  // Two args - toggle feature
  String featureId = args.substring(0, secondSpace);
  String value = args.substring(secondSpace + 1);
  featureId.toLowerCase();
  value.toLowerCase();
  
  const FeatureEntry* f = getFeatureById(featureId.c_str());
  if (!f) {
    return "Unknown feature. Run 'features' to see list.";
  }
  
  if (!canToggleFeature(f)) {
    if (!isFeatureCompiled(f)) {
      return "Feature not compiled in this build.";
    }
    if (f->flags & FEATURE_FLAG_ESSENTIAL) {
      return "Essential feature - should not be disabled.";
    }
    return "Feature cannot be toggled (compile-time only).";
  }
  
  bool enable = (value == "on" || value == "true" || value == "1");
  bool disable = (value == "off" || value == "false" || value == "0");
  
  if (!enable && !disable) {
    return "Value must be on/off, true/false, or 1/0";
  }
  
  bool wasEnabled = *f->enabledSetting;
  *f->enabledSetting = enable;
  
  writeSettingsJson();
  
  static char result[128];
  uint32_t freeHeapKB = ESP.getFreeHeap() / 1024;
  
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
// Command Registry
// ============================================================================

static const CommandEntry featureCommands[] = {
  { "features", "Show/toggle system features with heap estimates.", false, cmd_features,
    "features              - List all features\n"
    "features <id>         - Show feature details\n"
    "features <id> <on|off> - Enable/disable feature" }
};

static const size_t featureCommandsCount = sizeof(featureCommands) / sizeof(featureCommands[0]);

// Auto-register commands
static CommandModuleRegistrar _feature_cmd_registrar(featureCommands, featureCommandsCount, "features");
