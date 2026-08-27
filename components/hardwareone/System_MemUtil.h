#pragma once

#include <Arduino.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" {
  #include "esp_heap_caps.h"
  #include "esp_memory_utils.h"
}

// ── Correct internal-heap queries ──────────────────────────────────────
// Do NOT use ESP.getFreeHeap()/getHeapSize()/getMinFreeHeap()/getMaxAllocHeap()
// for internal-RAM decisions or reporting. Arduino defines them as
// heap_caps_*(MALLOC_CAP_INTERNAL) with NO MALLOC_CAP_8BIT
// (components/arduino/cores/esp32/Esp.cpp:159-176).
//
// On ESP32 classic that includes an IRAM-only heap which malloc(), new, String,
// and FreeRTOS task stacks cannot use. The helpers below deliberately describe
// byte-addressable internal RAM only.

inline size_t hw1InternalFreeBytes() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

inline size_t hw1InternalMinFreeBytes() {
  return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

inline size_t hw1InternalLargestBlock() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Physical 8-bit internal RAM, with the reserve pool's double-count removed.
// Exact to within one byte per reserve chunk: the pool is registered as
// [start, start + size - 1], so each chunk's span is size-1 (esp_psram.c).
inline size_t hw1InternalTotalBytes() {
  size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if defined(CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL) && (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL > 0)
  const size_t reserved = static_cast<size_t>(CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL);
  if (total > reserved) total -= reserved;
#endif
  return total;
}

// ── Allocation policy and malloc-family API ────────────────────────────

// Keep the historical AllocPref type and the first two numeric values intact.
// AllocPolicy is the clearer spelling for new code.
enum class AllocPref : uint8_t {
  PreferPSRAM     = 0,  // SPIRAM|8BIT, then INTERNAL|8BIT
  PreferInternal  = 1,  // INTERNAL|8BIT, then SPIRAM|8BIT
  RequirePSRAM    = 2,  // SPIRAM|8BIT only
  RequireInternal = 3,  // INTERNAL|8BIT only
  DefaultHeap     = 4   // libc allocator, unless the global bypass is enabled
};
using AllocPolicy = AllocPref;

inline bool hasPSRAMAvail() {
#if defined(BOARD_HAS_PSRAM) || (defined(CONFIG_SPIRAM) && CONFIG_SPIRAM)
  return true;
#else
  return false;
#endif
}

// True when a byte-addressable external heap is registered. This deliberately
// tests total size, not current free size: an exhausted pool still exists and a
// preferred allocation should be observable as a fallback.
bool psramAvailableRuntime();

// When enabled, no allocation in this API may enter or migrate into PSRAM.
// Common legacy syntax (`psramBypassGlobal() = true`) remains valid.
std::atomic_bool& psramBypassGlobal();
inline bool psramBypassEnabled() {
  return psramBypassGlobal().load(std::memory_order_relaxed);
}
inline void setPsramBypass(bool enabled) {
  psramBypassGlobal().store(enabled, std::memory_order_relaxed);
}

// Counts successful PreferPSRAM allocations that had to use internal DRAM.
// Updates are atomic; logging is rate-limited in System_MemUtil.cpp.
extern std::atomic<uint32_t> gPsAllocFallbacks;
uint32_t psAllocFallbackCount();

// Optional allocation debug hook, defined weakly in HardwareOne.cpp. The
// requestedPS records caller policy, usedPS comes from the returned pointer's
// actual memory region, and fellBack is true only when an attempted
// PreferPSRAM allocation succeeds from internal DRAM.
extern "C" void memAllocDebug(const char* op, void* ptr, size_t size,
                              bool requestedPS, bool usedPS, bool fellBack,
                              const char* tag);

// Canonical forms. New call sites should always supply a stable descriptive
// tag such as "video.http.chunk".
void* ps_alloc(size_t size, AllocPolicy policy, const char* tag);
void* ps_calloc(size_t n, size_t size, AllocPolicy policy, const char* tag);
void* ps_realloc(void* ptr, size_t size, AllocPolicy policy, const char* tag);
void ps_free(void* ptr);

// Compatibility forms used by the current tree. calloc/realloc intentionally
// have no two-argument default: Arduino declares C functions with those exact
// signatures, so a C++ overload with a default policy is ambiguous.
inline void* ps_alloc(size_t size, AllocPref pref) {
  return ps_alloc(size, static_cast<AllocPolicy>(pref), nullptr);
}
inline void* ps_alloc(size_t size) {
  return ps_alloc(size, AllocPolicy::PreferPSRAM, nullptr);
}
inline void* ps_calloc(size_t n, size_t size, AllocPref pref) {
  return ps_calloc(n, size, static_cast<AllocPolicy>(pref), nullptr);
}
inline void* ps_realloc(void* ptr, size_t size, AllocPref pref) {
  return ps_realloc(ptr, size, static_cast<AllocPolicy>(pref), nullptr);
}

// Destroy a placement-new object allocated by ps_alloc, then release storage.
template <typename T>
inline void ps_delete(T* obj) {
  if (!obj) return;
  obj->~T();
  ps_free(static_cast<void*>(obj));
}

// ============================================================================
// ArduinoJson PSRAM allocator
// ============================================================================

#include <ArduinoJson.h>

class PsramJsonAllocator : public ArduinoJson::Allocator {
public:
  void* allocate(size_t size) override;
  void deallocate(void* ptr) override;
  void* reallocate(void* ptr, size_t newSize) override;

  static PsramJsonAllocator* instance();

private:
  PsramJsonAllocator() = default;
};

inline ArduinoJson::Allocator* psramJsonAllocator() {
  return PsramJsonAllocator::instance();
}

#define PSRAM_JSON_DOC(name) JsonDocument name(psramJsonAllocator())

// ============================================================================
// PSRAM-backed static command output buffers
// ============================================================================
// Each expansion lazily creates one persistent buffer for its call site.

#define PSRAM_STATIC_BUF(name, size) \
  static char* name = nullptr; \
  static constexpr size_t name##_SIZE = size; \
  if (!name) { \
    name = (char*)ps_alloc(size, AllocPolicy::PreferPSRAM, #name); \
    if (name) name[0] = '\0'; \
  } \
  if (!name) return "Error: Failed to allocate buffer"
