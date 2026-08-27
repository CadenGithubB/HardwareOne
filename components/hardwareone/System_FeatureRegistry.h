/**
 * System Feature Registry
 * 
 * Centralized registry of all system features with heap cost estimates
 * and enable/disable capabilities for boot-time configuration.
 */

#ifndef SYSTEM_FEATUREREGISTRY_H
#define SYSTEM_FEATUREREGISTRY_H

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

// Feature flags. TWO ORTHOGONAL AXES packed into one byte — do not read this
// as a 3-way enum.
//
//   Axis 1 (mutability): exactly one of RUNTIME_TOGGLE or COMPILE_TIME.
//     Redundant with enabledSetting (COMPILE_TIME <=> enabledSetting == nullptr);
//     a static_assert-equivalent check in the .cpp keeps them in step. The bit
//     exists so a row DECLARES its intent rather than the reader inferring it
//     from a pointer being null.
//   Axis 2 (timing): REQUIRES_REBOOT is OPTIONAL and only meaningful alongside
//     RUNTIME_TOGGLE — wifi, oled and i2c are both toggleable AND reboot-gated.
//     Before 2026-08-22 those three carried REQUIRES_REBOOT *instead of*
//     RUNTIME_TOGGLE, which made the JSON `toggleable` field disagree with
//     canToggleFeature() on every one of them.
enum FeatureFlags {
  FEATURE_FLAG_NONE           = 0,         // sentinel; no row uses it
  FEATURE_FLAG_RUNTIME_TOGGLE = (1 << 0),  // `features <id> on|off` is accepted (read by canToggleFeature)
  FEATURE_FLAG_REQUIRES_REBOOT = (1 << 1), // takes effect next boot (read by the toggle reply + System_I2C.cpp)
  FEATURE_FLAG_COMPILE_TIME   = (1 << 2),  // no enable setting exists; == !RUNTIME_TOGGLE
  FEATURE_FLAG_ESSENTIAL      = (1 << 3),  // read by the setup wizard; set by no row today
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
// NOTE: there is deliberately no getFeaturesByCategory(). It was declared here
// for years and never defined — a latent link error for the first caller — and
// it CANNOT be implemented as declared: returning a pointer + count describes a
// contiguous slice, but featureRegistry[] is ordered for display, not grouped by
// category (NETWORK, DISPLAY, SENSOR and SYSTEM each appear in three separate
// runs). Iterate with getFeatureCount() + getFeatureByIndex() and filter on
// ->category, which is what getCategoryHeapEstimate() already does.

// Heap estimation
uint32_t getEnabledFeaturesHeapEstimate();
uint32_t getTotalPossibleHeapCost();
uint32_t getCategoryHeapEstimate(FeatureCategory cat);

// Feature status helpers
bool isFeatureEnabled(const FeatureEntry* feature);
bool isFeatureCompiled(const FeatureEntry* feature);
bool canToggleFeature(const FeatureEntry* feature);

// CLI command
const char* cmd_features(const String& argsInput);

#endif // SYSTEM_FEATUREREGISTRY_H
