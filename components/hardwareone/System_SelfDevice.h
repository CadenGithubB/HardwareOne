#ifndef SYSTEM_SELF_DEVICE_H
#define SYSTEM_SELF_DEVICE_H

#include <Arduino.h>
#include <stdint.h>

// =============================================================================
// SelfDevice — accessors for this device's own state
// =============================================================================
//
// Every UI (web, CLI, MQTT, OLED, G2, future) needs the same handful of
// "what's my..." answers: uptime, free heap, MAC, firmware version, name.
// Pre-consolidation these were each open-coded at every callsite with
// inconsistent units (seconds vs ms, bytes vs KB), inconsistent formatting
// ("AA:BB:CC..." vs lowercase variants), and inconsistent fallbacks
// (deviceName missing → "" in some places, MAC in others).
//
// This namespace is the single source of truth.
//
// Scope discipline (mirrors BondedPeer / MeshPeers):
//   - Pure reads. No mutation here.
//   - Implementation-layer callers (System_ESPNow.cpp internals, allocator
//     guards, deep debug-log context) keep their direct calls — they're the
//     plumbing, not consumers of the facade.
//   - Facade consumers (web handlers, CLI commands, MQTT publishers,
//     settings-JSON builders) should use these.
// =============================================================================

namespace SelfDevice {

// ----- Time / lifecycle ---------------------------------------------------

// Seconds since boot. Counts on past the 49.7 days at which millis() starts
// again at 0 (it reads the 64-bit boot timer). Every place that shows, sends
// or compares uptime asks here; `millis() / 1000` is not an uptime.
uint32_t uptimeSeconds();

// Milliseconds since boot. Just millis(), but routed through here so callers
// that want "time-of-this-device" don't reach for the raw Arduino API and
// can be swapped to a monotonic clock later if needed.
inline uint32_t uptimeMs() { return (uint32_t)millis(); }

// ----- Memory -------------------------------------------------------------

// Total free heap (DRAM + PSRAM combined as ESP.getFreeHeap reports it).
// Units: bytes. For UI display, divide by 1024 yourself — keep helpers honest
// about units rather than offering KB/MB variants that proliferate.
uint32_t freeHeapBytes();

// Free internal DRAM (caller-controlled allocations from MALLOC_CAP_INTERNAL).
// Tighter constraint than freeHeapBytes — DRAM is the small, contested pool.
uint32_t dramFreeBytes();

// Free PSRAM (caller-controlled allocations from MALLOC_CAP_SPIRAM). Returns
// 0 on boards without PSRAM.
uint32_t psramFreeBytes();

// Lowest heap ever seen since boot. Useful for memory-pressure diagnostics —
// "we got down to 12 KB at some point" tells you a transient burst happened
// even if the current snapshot looks healthy.
uint32_t minFreeHeapBytes();

// ----- Identity -----------------------------------------------------------

// Copy this device's STA-interface MAC into out[6]. Returns false on failure
// (e.g., WiFi not yet initialized — rare but possible during early boot).
bool macBytes(uint8_t out[6]);

// Formatted STA MAC as "AA:BB:CC:DD:EE:FF" (uppercase, colon-separated).
// Returns "00:00:00:00:00:00" on failure rather than empty so callers can
// safely print without NULL-checking.
String macString();

// This device's name as Bluetooth and Wi-Fi use it (System_DeviceName.h has
// the rule): the stored name (gSettings.espnowDeviceName, set with
// 'devicename') when it passes the rule, else "HardwareOne-XXXXXX" from the
// last three bytes of the chip's base MAC (eFuse; available from boot, before
// any radio). On S3 and classic ESP32 that is the station MAC macBytes()
// reports; on a P4 it is the P4's own address, not the C6 radio's address
// that macBytes() and ESP-NOW show. A stored name that breaks the rule (settings.json is
// loaded without the setter's check) gives the fallback and one WARN. Never
// empty, at most 31 bytes.
String deviceName();

// Hand the name to the Wi-Fi stack as the station hostname: the stack applies
// it when the station interface starts (and, when it is already up, to the
// running interface, for its next DHCP exchange). Called after the settings
// load at boot and by the setter. No-op in builds without Wi-Fi.
void applyHostname();

// ----- Firmware ----------------------------------------------------------

// Running firmware version from esp_app_get_description()->version. Returns
// "unknown" if the app description is missing (defensive — should never fire
// on a properly-built image). Always non-empty.
const char* firmwareVersion();

} // namespace SelfDevice

// devicename [<name>|clear] — the one setter of the device name (System_DeviceName.cpp).
const char* cmd_devicename(const String& argsInput);

// After gSettings.espnowDeviceName changed (cleared when `name` is empty): the
// one place that hands it on — the settings event, ESP-NOW's own entry and the
// metadata push, and the Wi-Fi hostname. Used by 'devicename' and by the setup
// wizards' save. `warn` (may be null) gets " Warning: <who> has the same name.
// ..." when another device this one knows has the name, else "".
void deviceNameApply(const String& name, char* warn, size_t warnCap);

#endif // SYSTEM_SELF_DEVICE_H
