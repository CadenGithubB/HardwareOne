#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// System_RamFlush — resume the running feature set across a RAM-flush reboot
// ============================================================================
// `ramflush` reboots the device to reclaim fragmented internal DRAM, then brings
// back the features that were actually running when you ran it. A plain `reboot`
// is untouched and still comes up on configured intent alone.
//
// TWO LAYERS, and the separation is the whole point:
//
//   intent   = gSettings.*AutoStart — what the user configured to start at boot.
//              This module NEVER writes it, in any store. Runtime start/stop
//              deliberately does not touch it either (System_I2C.cpp:1708-1712),
//              which is exactly what makes layer 2 derivable.
//   overlay  = the features whose live state diverged from intent when `ramflush`
//              ran. Applies to ONE boot, then it is gone.
//
// Divergence IS touch. Live state can only differ from intent if someone toggled
// it at runtime, precisely because the two are decoupled — so no per-toggle-site
// instrumentation is needed, and a session where you changed nothing produces an
// empty overlay and a bit-for-bit ordinary boot.
//
// The diff is captured against INTENT, never against state-achieved-at-boot. Diff
// against achieved state and the overlay reads empty right after being applied,
// erases itself, and resume works exactly once before silently dying.
//
// Storage is RTC_NOINIT: on-die SRAM in the ESP32-S3's always-on power domain —
// NOT the DS3231 chip, despite the name. 20 bytes, zero flash writes. That makes
// "never trample settings.json" structural rather than a rule to remember: this
// module has no flash writer, so it cannot leak into settings.json even by
// mistake. Never route the overlay through gSettings — it is serialized whole
// -document on the next setSetting() from anywhere (System_Settings.cpp:2498),
// which would persist session state into configured intent hours later.
//
// Power loss clears RTC, so a cold boot returns to pure intent. That is intended,
// not a limitation: an overlay is a statement about this session.

// Stable feature ids. APPEND ONLY — never renumber, never reuse a retired slot.
// Deliberately NOT the FeatureRegistry index: that array is build-config
// dependent (the automation entry is #if-gated), so index N means different
// features in different builds of the same firmware.
typedef enum : uint8_t {
  RF_THERMAL    = 0,
  RF_TOF        = 1,
  RF_IMU        = 2,
  RF_GPS        = 3,
  RF_FMRADIO    = 4,
  RF_APDS       = 5,
  RF_INPUT      = 6,
  RF_RTC        = 7,
  RF_PRESENCE   = 8,
  RF_CAMERA     = 9,
  RF_SR         = 10,
  RF_MICROPHONE = 11,
  RF_HTTP       = 12,
  RF_LLM        = 13,
  RF_MQTT       = 14,
  RF_BLUETOOTH  = 15,
  RF_SENSORLOG  = 16,
  RF_SYSTEMLOG  = 17,
  RF_ESPNOW     = 18,
  RF_WIFI       = 19,
  RF_OLED       = 20,
  RF_AUTOMATION = 21,
  RF_FEATURE_COUNT
} RamFlushFeatureId;

// Capture live-vs-intent divergence into RTC, then let the caller reboot.
// Called ONLY from cmd_ramflush — never from rebootDevice()/recordRebootIntent(),
// or a plain reboot (and factoryreset, which reboots via its own deferred timer)
// would silently acquire resume semantics.
void ramFlushCaptureOverlay(void);

// Consume the overlay: validate, copy to plain RAM, and invalidate the RTC magic
// IMMEDIATELY — before anything is applied. If applying the overlay panics, the
// next boot has no overlay and comes up on pure intent, so a feature that wedged
// the device cannot be restored into a boot loop. Call once, early in setup(),
// before every apply site. Safe to call unconditionally on any boot.
void ramFlushConsumeOverlay(void);

// The resolved answer to "should this feature be on?" — the overlay's value if it
// diverged, else `intent`. This is the ONLY entry point the apply sites call.
//
// Valid for the WHOLE session, not just boot: gSettings.inputAutoStart is re-read
// at runtime on OLED menu entry and login (OLED_Utils.cpp:6004), so a boot-scoped
// overlay would let input silently resurrect minutes after resume.
bool ramFlushResolve(RamFlushFeatureId f, bool intent);

// True if a valid overlay was consumed this boot (for `ramflush status`).
bool ramFlushOverlayActive(void);

// Drop any stored overlay. For factoryreset — RTC survives a config wipe, so
// without this a factory-reset device resumes the feature set it just erased.
void ramFlushClearOverlay(void);

// Mark a feature as having FAILED to autostart rather than having been turned off
// by the user. Without this, an unplugged sensor reads live=false vs intent=true
// and the next capture records "the user turned it off" — suppressing a
// configured autostart on every later boot, with replugging doing nothing to fix
// it. Sensor availability is best-effort (System_I2C.cpp:2950-2964), so this is
// mandatory, not defensive.
void ramFlushMarkAutostartFailed(RamFlushFeatureId f);
void ramFlushClearAutostartFailed(RamFlushFeatureId f);

// Map an I2C autostart module name ("thermal", "gamepad", …) to its id. Returns
// RF_FEATURE_COUNT for anything unrecognised, which the two calls above no-op on.
RamFlushFeatureId ramFlushIdForModule(const char* moduleName);

// Registered in systemCommands[] next to `reboot`, which it is a sibling of.
const char* cmd_ramflush(const String& argsInput);
