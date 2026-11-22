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
//              ran, plus the list of I2C devices that were running (see the
//              device list below). Applies to ONE boot, then it is gone.
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
// NOT the DS3231 chip, despite the name. 65 bytes, zero flash writes. That makes
// "never trample settings.json" structural rather than a rule to remember: this
// module has no flash writer, so it cannot leak into settings.json even by
// mistake. Never route the overlay through gSettings — it is serialized whole
// -document on the next setSetting() from anywhere (System_Settings.cpp:2498),
// which would persist session state into configured intent hours later.
//
// Power loss clears RTC, so a cold boot returns to pure intent. That is intended,
// not a limitation: an overlay is a statement about this session.
//
// I2C DEVICE LIST (owner decision 2026-11-22). The queue-managed I2C devices
// (System_I2CInstanceCore.h) are not in the bit masks: a capture lists each
// instance that is running (or failed to start while meant to be on) as one
// 2-byte entry (type, instance, bus, 7-bit address;
// System_RamFlushDeviceList.h), up to the device registry's 24. The
// boot after it reopens exactly the listed instances, each only while its bus
// and address settings still match the saved ones (else it stays closed and
// one log line says why). An I2C device that is not listed stays closed for
// that boot even when its start-at-boot is on. Past boot the list holds only
// where it differs from start-at-boot as the boot read it, like the bits: a
// later `<x>autostart`, setup wizard or settings import applies to the
// session-long callers again. A type with several instances
// (the encoder boards; PCA9685 and HT16K33 later) needs no layout change. The
// single-copy features (Wi-Fi, Bluetooth, HTTP, LLM, MQTT, logging, ESP-NOW,
// OLED, automation, camera, SR, microphone, and the GPIO input wheel) keep
// their bits.

// Stable feature ids. APPEND ONLY — never renumber, never reuse a retired slot.
// Deliberately NOT the FeatureRegistry index: that array is build-config
// dependent (the automation entry is #if-gated), so index N means different
// features in different builds of the same firmware. The ids of the I2C queue
// devices (ramFlushIdForDevice) stay for their failed marks and the `features`
// list's run column; their overlay bits are never set (the device list holds
// them).
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
  RF_ENCODER = 22,
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
// diverged, else `intent`. This and ramFlushResolveDevice are the only entry
// points the apply sites call. For the id of an I2C queue device it answers for
// instance 1 from the device list (ramFlushResolveDevice).
//
// Valid for the WHOLE session, not just boot: gSettings.inputAutoStart is re-read
// at runtime on OLED menu entry and login (OLED_Utils.cpp:6004), so a boot-scoped
// overlay would let input silently resurrect minutes after resume.
bool ramFlushResolve(RamFlushFeatureId f, bool intent);

// The same answer for instance n (1-based) of a queue-managed I2C device:
// `intent` (its start-at-boot) when no flush list is in force this boot;
// otherwise the list's answer (listed, with its bus and address settings still
// equal to the saved ones) where it differs from the instance's start-at-boot
// at this boot, and `intent` where it agrees, so a later change to the
// setting applies. A listed instance whose bus or address changed is logged
// once. Session-long, like ramFlushResolve.
enum I2cDeviceType : int;
bool ramFlushResolveDevice(I2cDeviceType device, uint8_t n, bool intent);

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

// The ramflush id of a queue-managed I2C device (System_I2C_Manager.h). Returns
// RF_FEATURE_COUNT for I2C_DEVICE_COUNT, which the two calls above no-op on.
RamFlushFeatureId ramFlushIdForDevice(I2cDeviceType device);

// The failed mark of one instance n (1-based) of a queue device
// (System_I2CInstanceCore.h). Instance 1 is its feature's mark above; an
// instance n >= 2 has its own bit, so a missing second board does not mark
// the first as failed. A capture lists a marked instance whose start-at-boot
// (as this boot resolved it) is on, so the flush tries it again.
void ramFlushMarkAutostartFailed(I2cDeviceType device, uint8_t n);
void ramFlushClearAutostartFailed(I2cDeviceType device, uint8_t n);

// Is the feature running right now (its owner's live flag, not a setting)? false
// for a feature this build does not have. The `features` list reads its "run"
// column from here (System_FeatureRegistry.cpp), so the list and a capture agree.
bool ramFlushReadLive(RamFlushFeatureId f);

// Registered in systemCommands[] next to `reboot`, which it is a sibling of.
const char* cmd_ramflush(const String& argsInput);
