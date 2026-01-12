/**
 * System Feature Registry
 * 
 * Centralized registry of all system features with heap cost estimates
 * and enable/disable capabilities for boot-time configuration.
 */

#ifndef SYSTEM_FEATURE_REGISTRY_H
#define SYSTEM_FEATURE_REGISTRY_H

#include <Arduino.h>
#include "System_BuildConfig.h"

// Feature categories for organization
enum FeatureCategory {
  FEATURE_CAT_CORE,      // Essential system features
  FEATURE_CAT_NETWORK,   // WiFi, ESP-NOW, HTTP, Bluetooth
  FEATURE_CAT_DISPLAY,   // OLED, LED
  FEATURE_CAT_SENSOR,    // I2C sensors
  FEATURE_CAT_SYSTEM     // Automations, logging, etc.
};

// Feature flags for capabilities
enum FeatureFlags {
  FEATURE_FLAG_NONE           = 0,
  FEATURE_FLAG_RUNTIME_TOGGLE = (1 << 0),  // Can be enabled/disabled at runtime
  FEATURE_FLAG_REQUIRES_REBOOT = (1 << 1), // Needs reboot to take effect
  FEATURE_FLAG_COMPILE_TIME   = (1 << 2),  // Controlled by compile flag only
  FEATURE_FLAG_ESSENTIAL      = (1 << 3),  // Should not be disabled (e.g., gamepad for OLED nav)
};

// Feature entry structure
struct FeatureEntry {
  const char* id;              // Short identifier (e.g., "wifi", "thermal")
  const char* name;            // Human-readable name
  FeatureCategory category;    // Category for grouping
  uint16_t heapCostKB;         // Estimated heap usage in KB
  uint8_t flags;               // FeatureFlags
  bool* enabledSetting;        // Pointer to gSettings.xxxEnabled/AutoStart (nullptr if compile-time only)
  bool (*isCompileEnabled)();  // Function to check if compiled in (nullptr = always compiled)
  const char* description;     // Brief description
};

// Feature registry functions
void initFeatureRegistry();
size_t getFeatureCount();
const FeatureEntry* getFeatureByIndex(size_t index);
const FeatureEntry* getFeatureById(const char* id);
const FeatureEntry* getFeaturesByCategory(FeatureCategory cat, size_t* outCount);

// Heap estimation
uint32_t getEnabledFeaturesHeapEstimate();
uint32_t getTotalPossibleHeapCost();
uint32_t getCategoryHeapEstimate(FeatureCategory cat);

// Feature status helpers
bool isFeatureEnabled(const FeatureEntry* feature);
bool isFeatureCompiled(const FeatureEntry* feature);
bool canToggleFeature(const FeatureEntry* feature);

// CLI command
const char* cmd_features(const String& cmd);

#endif // SYSTEM_FEATURE_REGISTRY_H
