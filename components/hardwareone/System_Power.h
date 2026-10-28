/**
 * System Power Management
 * 
 * Handles CPU frequency scaling and display brightness management
 * for battery optimization.
 */

#ifndef SYSTEM_POWER_H
#define SYSTEM_POWER_H

#include <Arduino.h>

// Power mode constants. A mode sets the clock while the device is in use and
// while idle; every mode runs a busy workload (transcription, camera,
// recording) at the maximum clock — see powerPolicyTick below.
#define POWER_MODE_PERFORMANCE  0
#define POWER_MODE_BALANCED     1
#define POWER_MODE_ADAPTIVE     2   // Floor clock in use and idle; raised only by work. The default.
#define POWER_MODE_ULTRASAVER   3
#define POWER_MODE_LOCKED       4   // Maximum clock always (idle power-save does not downclock)
#define POWER_MODE_COUNT        5
// Settings' constructor repeats this value as a literal (System_Settings.h does
// not include this header); test_power_save_tick.py checks the two agree.
#define POWER_MODE_DEFAULT      POWER_MODE_ADAPTIVE

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

// Change the clock through the HAL after draining existing I2C buses. This does
// not initialize buses, change settings or publish events. Returns false when
// another change is in progress, a bus cannot be acquired, or the HAL/readback
// fails. Lock refusal leaves the clock unchanged; a HAL failure may require the
// caller to inspect the live clock. Optional outClockChanged reports only a
// change made by this call, even on HAL failure; refusal/no-op sets it false.
// The caller chooses active/idle policy.
// Task context only; call outside I2C/SPI transactions and clock callbacks.
bool powerSetCpuFrequencyMhz(uint32_t mhz, bool* outClockChanged = nullptr);

// False if the target clock cannot be applied; no success event is emitted.
// setDisplayBrightness=false keeps the saved oledBrightness (used when the
// saved mode is re-applied at boot or after setup, not chosen by the user).
bool applyPowerMode(uint8_t mode, bool setDisplayBrightness = true);

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
// Activity tracking
//
// Any subsystem representing a real user/peer interaction calls
// powerSaveNoteActivity(local). `local` means someone is at the device — an
// input-device event, a command from the local display, on-device voice — and
// is what wakes the local panel. Remote activity (glasses, phone, web, serial,
// ESP-NOW, MQTT) only keeps the clock at the mode's in-use clock; it never
// lights the panel. Cheap + task-safe: it only stamps timestamps (a 32-bit
// aligned write is atomic on the ESP32); the work runs on the main loop in
// powerPolicyTick() and the local display's own tick.
void powerSaveNoteActivity(bool local);
unsigned long powerSaveLastActivityMs();       // any real interaction
unsigned long powerSaveLastLocalActivityMs();  // someone at the device

// ----------------------------------------------------------------------------
// Clock policy
//
// powerPolicyTick() runs on every main-loop pass in every build, with or
// without a local display. It works out the clock the device should be at and
// applies it through powerSetCpuFrequencyMhz only when that target changes:
//   Locked, or a busy workload (powerWorkloadBoost) → the maximum clock;
//   in use (activity within the idle timeout, the timeout disabled, or the
//   local panel lit) → the mode's active clock;
//   otherwise → the mode's idle clock (UltraSaver's 40 MHz is idle-only, so
//   a lit panel keeps the interactive floor).
// A manual cpufreq / power-mode change therefore lasts until the next change
// of target. A refused change is retried after a backoff — sooner for a raise,
// since a transcription may be waiting on it.
void powerPolicyTick();
// The clock the policy wants right now (`targetMhz` in buildPowerJson) and
// why: "locked", the busy workload's name, "active" (in use) or "idle".
uint32_t powerTargetCpuMhz();
const char* powerTargetReason();
// Name of the busy workload holding the clock at maximum ("stt", "camera",
// "recording"), or nullptr. Read from the workloads' own busy flags on every
// pass, not held as a request: a worker that is cancelled, fails or deletes
// its own task clears its flag and the boost ends with it.
const char* powerWorkloadBoost();
// The local display's tick reports whether the panel is lit. Headless builds
// never call this; the panel then counts as dark.
void powerSetLocalDisplayAwake(bool awake);
bool powerLocalDisplayAwake();

// ----------------------------------------------------------------------------
// Power telemetry JSON
//
// buildPowerJson(JsonDocument&) — defined in System_Power.cpp — is the single
// source of truth for the power snapshot: mode, live/target/active/idle
// clocks, the busy workload holding a boost, idle power-save state,
// sleep-cooldown gating, and the full preset table as a `modes` array.
// `power json` (CLI/BLE) and the web /api/power/status both call it, so every
// interface returns one schema.
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
