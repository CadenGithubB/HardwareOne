/**
 * System Power Management
 * 
 * Handles CPU frequency scaling and display brightness management
 * for battery optimization.
 */

#ifndef SYSTEM_POWER_H
#define SYSTEM_POWER_H

#include <Arduino.h>

// Power mode constants
#define POWER_MODE_PERFORMANCE  0
#define POWER_MODE_BALANCED     1
#define POWER_MODE_POWERSAVER   2
#define POWER_MODE_ULTRASAVER   3

// ============================================================================
// Power Mode Management Functions
// ============================================================================

const char* getPowerModeName(uint8_t mode);
uint32_t getPowerModeCpuFreq(uint8_t mode);
uint8_t getPowerModeDisplayBrightness(uint8_t mode);

void applyPowerMode(uint8_t mode);
void checkAutoPowerMode();

// ----------------------------------------------------------------------------
// Sleep transition cooldown ("anti-flap")
//
// Sleep entry is expensive and not idempotent — half-initialised peripherals,
// WiFi/BLE reconnect cycles, and a bouncing trigger can chew battery in
// seconds. powerSleepTransitionAllowed returns true if the cooldown
// (gSettings.powerTransitionCooldownMs) has elapsed since the last
// successful entry call, OR if the cooldown is disabled (0). Pass a
// non-null outRemainingMs to learn how long the caller must still wait.
//
// powerSleepTransitionMark stamps "now" as the last entry time. The contract
// is: callers check Allowed() first, return early if false, otherwise call
// Mark() right before invoking esp_light_sleep_start / esp_deep_sleep_start.
// Light-sleep wake doesn't re-stamp — the cooldown clock keeps running so a
// rapid wake → sleep → wake cycle is suppressed.
bool powerSleepTransitionAllowed(unsigned long* outRemainingMs = nullptr);
void powerSleepTransitionMark();

// ----------------------------------------------------------------------------
// Adaptive power-save activity tracking
//
// Any subsystem representing a real user/peer interaction (an input-device
// event, or a CLI/web/ESP-NOW command) calls powerSaveNoteActivity() to reset
// the idle timer and wake the device if it's in power-save. Cheap + task-safe:
// it only stamps a timestamp (a 32-bit aligned write is atomic on the ESP32);
// the heavy wake work runs on the main loop in powerSaveTick(). This decouples
// power-save from any specific input device, so a headless box benefits too.
void powerSaveNoteActivity();
unsigned long powerSaveLastActivityMs();

// ============================================================================
// Command Registry
// ============================================================================

struct CommandEntry;
extern const CommandEntry powerCommands[];
extern const size_t powerCommandsCount;

#endif
