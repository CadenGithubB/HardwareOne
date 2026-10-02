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
#define POWER_MODE_LOCKED       4   // Maximum clock always (idle power-save does not downclock)
#define POWER_MODE_COUNT        5

// PLL frequencies accepted by the selected IDF target/revision. P4 revisions
// below 3.0 use the 360 MHz PLL; revision 3.x builds use the 400 MHz PLL.
// Keep target details here so commands and every UI share the same policy.
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  #if defined(CONFIG_ESP32P4_SELECTS_REV_LESS_V3) && CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    #define POWER_CPU_PERFORMANCE_MHZ 360
    #define POWER_CPU_BALANCED_MHZ    180
    #define POWER_INTERACTIVE_FLOOR_MHZ 90
  #else
    #define POWER_CPU_PERFORMANCE_MHZ 400
    #define POWER_CPU_BALANCED_MHZ    200
    #define POWER_INTERACTIVE_FLOOR_MHZ 100
  #endif
#else
  // Existing ESP32 / ESP32-S3 policy.
  #define POWER_CPU_PERFORMANCE_MHZ 240
  #define POWER_CPU_BALANCED_MHZ    160
  #define POWER_INTERACTIVE_FLOOR_MHZ 80
#endif

// UltraSaver's XTAL clock is idle-only; interactive modes use the PLL floor.
#define POWER_CPU_ULTRA_IDLE_MHZ 40

// ============================================================================
// Power Mode Management Functions
// ============================================================================

const char* getPowerModeName(uint8_t mode);
// Nominal (table) clock for the mode — UltraSaver's is 40. This is the DEEP
// value; use the active/idle accessors below for what actually gets applied.
uint32_t getPowerModeCpuFreq(uint8_t mode);
// Clock applied while the device is actively used: max(nominal, target floor).
uint32_t getPowerModeActiveCpuFreq(uint8_t mode);
// Clock the idle power-save path may drop to (OLED blanked, radio still up):
//   Locked → keep active clock
//   Performance / Balanced / PowerSaver → target PLL floor
//   UltraSaver → idle XTAL clock
uint32_t getPowerModeIdleCpuFreq(uint8_t mode);
uint8_t getPowerModeDisplayBrightness(uint8_t mode);

// Direct interactive overrides, ascending. Index outside count returns zero.
size_t getPowerCpuFrequencyCount();
uint32_t getPowerCpuFrequencyMhz(size_t index);
bool isPowerCpuFrequencySupported(uint32_t mhz);

// False if the target clock cannot be applied; no success event is emitted.
bool applyPowerMode(uint8_t mode);
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

// ----------------------------------------------------------------------------
// Power telemetry JSON
//
// buildPowerJson(JsonDocument&) — defined in System_Power.cpp — is the single
// source of truth for the power snapshot: mode, live/active/idle clocks, auto
// mode, idle power-save state, sleep-cooldown gating, and the full preset
// table as a `modes` array. `power json` (CLI/BLE) and the web
// /api/power/status both call it, so every interface returns one schema.
//
// Declared at the call sites with a local `extern` (same pattern as
// buildBatteryJson) so this header does not pull in ArduinoJson.

// ============================================================================
// Command Registry
// ============================================================================

struct CommandEntry;
extern const CommandEntry powerCommands[];
extern const size_t powerCommandsCount;

#endif
