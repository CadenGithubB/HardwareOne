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

// Seconds since boot. Wraps millis()/1000 with a single rollover behavior
// (millis() wraps at ~49 days). Use for uptime display.
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

// Friendly device name from settings, with sensible fallbacks. Resolution
// order: gSettings.espnowDeviceName → macString(). Never returns an empty
// string — callers can use it directly in printf-style formatting.
String deviceName();

// ----- Firmware ----------------------------------------------------------

// Running firmware version from esp_app_get_description()->version. Returns
// "unknown" if the app description is missing (defensive — should never fire
// on a properly-built image). Always non-empty.
const char* firmwareVersion();

} // namespace SelfDevice

#endif // SYSTEM_SELF_DEVICE_H
