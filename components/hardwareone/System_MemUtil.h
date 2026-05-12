#pragma once
#include <Arduino.h>
#include <esp_log.h>
extern "C" {
  #include "esp_heap_caps.h"
  #include "esp_memory_utils.h"
}

// ── PSRAM fallback diagnostics ────────────────────────────────────────────
// Incremented every time a ps_alloc/ps_calloc/ps_realloc request that asked
// for PSRAM had to fall back to the internal DRAM heap. This makes an
// otherwise silent failure mode observable. Previously a depleted PSRAM pool
// would silently redirect large buffers into the ~300 KB internal heap and
// starve other subsystems with no log trail; surfacing the count and logging
// the specific caller turns that into a grep-able event.
//
// Header-inline variable (C++17) avoids the need for a companion .cpp.
inline volatile uint32_t gPsAllocFallbacks = 0;

inline void __psAllocReportFallback(size_t size, const char* tag) {
  gPsAllocFallbacks = gPsAllocFallbacks + 1;
  ESP_LOGW("mem", "ps_alloc fallback to internal heap: %u bytes%s%s",
           (unsigned)size,
           tag ? " tag=" : "",
           tag ? tag : "");
}

// Pre-allocation snapshots (defined in main sketch)
extern size_t gAllocHeapBefore;
extern size_t gAllocPsBefore;

inline void __capture_mem_before() {
  gAllocHeapBefore = ESP.getFreeHeap();
  size_t psTot = ESP.getPsramSize();
  gAllocPsBefore = (psTot > 0) ? ESP.getFreePsram() : 0;
}

// Optional allocation debug hook (defined weakly elsewhere).
// Do not implement here to allow an override in the main sketch.
// Signature: op ("malloc"/"calloc"/"realloc"), returned ptr, size (or new size),
// requestedPS indicates if the call preferred PSRAM, usedPS is derived from ptr.
extern "C" void memAllocDebug(const char* op, void* ptr, size_t size,
                              bool requestedPS, bool usedPS, const char* tag);

// Compute usedPS from the returned pointer and dispatch to memAllocDebug.
// usedPS is decided by the actual region of the pointer (esp_ptr_external_ram),
// not by which branch produced it — under CONFIG_SPIRAM_USE_MALLOC, a plain
// malloc() can still land in PSRAM for sizes above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL.
inline void __memAllocReport(const char* op, void* ptr, size_t size,
                             bool requestedPS, const char* tag) {
  const bool usedPS = (ptr != nullptr) && esp_ptr_external_ram(ptr);
  memAllocDebug(op, ptr, size, requestedPS, usedPS, tag);
}

inline bool hasPSRAMAvail() {
#if defined(BOARD_HAS_PSRAM) || defined(CONFIG_SPIRAM)
  return true;
#else
  return false;
#endif
}

inline void* ps_try_malloc(size_t size) {
  if (hasPSRAMAvail()) {
    __capture_mem_before();
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (p) {
      // Best-effort logging; function may not be defined (weak)
      return p;
    }
  }
  __capture_mem_before();
  void* p2 = malloc(size);
  return p2;
}

inline void* ps_try_calloc(size_t n, size_t size) {
  if (hasPSRAMAvail()) {
    __capture_mem_before();
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
    if (p) {
      return p;
    }
  }
  __capture_mem_before();
  void* p2 = calloc(n, size);
  return p2;
}

inline void* ps_try_realloc(void* ptr, size_t size) {
  if (hasPSRAMAvail()) {
    __capture_mem_before();
    void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (p) {
      return p;
    }
  }
  __capture_mem_before();
  void* p2 = realloc(ptr, size);
  return p2;
}

// ----------------------------------------------------------------------------
// New allocation API (scaffolding only) — prefer PSRAM with per-call control
// ----------------------------------------------------------------------------

// Global bypass switch: when true, force allocations to internal heap
// (helpful for performance testing or when PSRAM proves problematic).
inline bool& psramBypassGlobal() {
  static bool gBypass = false;
  return gBypass;
}

enum class AllocPref : uint8_t {
  PreferPSRAM,
  PreferInternal
};

// Runtime availability check (compile-time + runtime free check)
inline bool psramAvailableRuntime() {
  if (!hasPSRAMAvail()) return false;
#if defined(ESP_ARDUINO_VERSION) || defined(ESP_PLATFORM)
  // Guard against platforms without these APIs; if not present, fall back to compile-time check
  size_t freePs = 0;
  // heap_caps_get_free_size is available via esp_heap_caps.h
  freePs = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  return freePs > 0;
#else
  return true;
#endif
}

inline void* ps_alloc(size_t size, AllocPref pref = AllocPref::PreferPSRAM) {
  const bool wantPS = (pref == AllocPref::PreferPSRAM) && !psramBypassGlobal() && psramAvailableRuntime();
  if (wantPS) {
    __capture_mem_before();
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (p) {
      __memAllocReport("malloc", p, size, wantPS, nullptr);
      return p;
    }
  }
  __capture_mem_before();
  void* p2 = malloc(size);
  if (p2 && wantPS) __psAllocReportFallback(size, nullptr);
  __memAllocReport("malloc", p2, size, wantPS, nullptr);
  return p2;
}

// Tagged overload: record a human-readable name for this allocation
inline void* ps_alloc(size_t size, AllocPref pref, const char* tag) {
  const bool wantPS = (pref == AllocPref::PreferPSRAM) && !psramBypassGlobal() && psramAvailableRuntime();
  if (wantPS) {
    __capture_mem_before();
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (p) {
      __memAllocReport("malloc", p, size, wantPS, tag);
      return p;
    }
  }
  __capture_mem_before();
  void* p2 = malloc(size);
  if (p2 && wantPS) __psAllocReportFallback(size, tag);
  __memAllocReport("malloc", p2, size, wantPS, tag);
  return p2;
}

inline void* ps_calloc(size_t n, size_t size, AllocPref pref = AllocPref::PreferPSRAM) {
  const bool wantPS = (pref == AllocPref::PreferPSRAM) && !psramBypassGlobal() && psramAvailableRuntime();
  if (wantPS) {
    __capture_mem_before();
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
    if (p) {
      __memAllocReport("calloc", p, n * size, wantPS, nullptr);
      return p;
    }
  }
  __capture_mem_before();
  void* p2 = calloc(n, size);
  if (p2 && wantPS) __psAllocReportFallback(n * size, nullptr);
  __memAllocReport("calloc", p2, n * size, wantPS, nullptr);
  return p2;
}

// Tagged overload
inline void* ps_calloc(size_t n, size_t size, AllocPref pref, const char* tag) {
  const bool wantPS = (pref == AllocPref::PreferPSRAM) && !psramBypassGlobal() && psramAvailableRuntime();
  if (wantPS) {
    __capture_mem_before();
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
    if (p) {
      __memAllocReport("calloc", p, n * size, wantPS, tag);
      return p;
    }
  }
  __capture_mem_before();
  void* p2 = calloc(n, size);
  if (p2 && wantPS) __psAllocReportFallback(n * size, tag);
  __memAllocReport("calloc", p2, n * size, wantPS, tag);
  return p2;
}

inline void* ps_realloc(void* ptr, size_t size, AllocPref pref = AllocPref::PreferPSRAM) {
  const bool wantPS = (pref == AllocPref::PreferPSRAM) && !psramBypassGlobal() && psramAvailableRuntime();
  if (wantPS) {
    __capture_mem_before();
    void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (p) {
      __memAllocReport("realloc", p, size, wantPS, nullptr);
      return p;
    }
    // Fall through to internal realloc if PSRAM attempt failed
  }
  __capture_mem_before();
  void* p2 = realloc(ptr, size);
  if (p2 && wantPS) __psAllocReportFallback(size, nullptr);
  __memAllocReport("realloc", p2, size, wantPS, nullptr);
  return p2;
}

// Tagged overload
inline void* ps_realloc(void* ptr, size_t size, AllocPref pref, const char* tag) {
  const bool wantPS = (pref == AllocPref::PreferPSRAM) && !psramBypassGlobal() && psramAvailableRuntime();
  if (wantPS) {
    __capture_mem_before();
    void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (p) {
      __memAllocReport("realloc", p, size, wantPS, tag);
      return p;
    }
  }
  __capture_mem_before();
  void* p2 = realloc(ptr, size);
  __memAllocReport("realloc", p2, size, wantPS, tag);
  return p2;
}

// C++ helpers: placement-new style wrappers for objects
template <typename T, typename... Args>
inline T* ps_new(AllocPref pref, Args&&... args) {
  void* mem = ps_alloc(sizeof(T), pref);
  if (!mem) return nullptr;
  return new (mem) T(std::forward<Args>(args)...);
}

template <typename T>
inline void ps_delete(T* obj) {
  if (!obj) return;
  obj->~T();
  free((void*)obj);
}

// ============================================================================
// Allocation Tracker Entry
// ============================================================================
// Shared struct for tracking per-tag allocation statistics.
// Defined once here; used by HardwareOne.cpp, System_MemoryMonitor.cpp, System_Utils.cpp

struct AllocEntry {
  char tag[24];
  size_t totalBytes;
  size_t psramBytes;
  size_t dramBytes;
  uint16_t count;
  bool isActive;
};

extern const int MAX_ALLOC_ENTRIES;
extern AllocEntry gAllocTracker[];

// ============================================================================
// ArduinoJson PSRAM Allocator
// ============================================================================
// Custom allocator for ArduinoJson v7 that uses PSRAM instead of internal heap.
// This moves all JSON parsing/building memory to PSRAM, freeing internal RAM.
//
// Usage:
//   JsonDocument doc(psramJsonAllocator());  // Uses PSRAM
//   JsonDocument doc;                         // Uses internal heap (default)
//
// Or use the convenience macro:
//   PSRAM_JSON_DOC(doc);                      // Equivalent to above
// ============================================================================

#include <ArduinoJson.h>

class PsramJsonAllocator : public ArduinoJson::Allocator {
public:
  void* allocate(size_t size) override {
    // Try PSRAM first, fall back to internal heap
    if (psramAvailableRuntime() && !psramBypassGlobal()) {
      void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
      if (p) return p;
    }
    return malloc(size);
  }

  void deallocate(void* ptr) override {
    free(ptr);  // Works for both PSRAM and internal heap
  }

  void* reallocate(void* ptr, size_t new_size) override {
    // Try PSRAM first, fall back to internal heap
    if (!ptr) {
      return allocate(new_size);
    }
    if (psramAvailableRuntime() && !psramBypassGlobal() && esp_ptr_external_ram(ptr)) {
      void* p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
      if (p) return p;
    }
    return realloc(ptr, new_size);
  }

  static PsramJsonAllocator* instance() {
    static PsramJsonAllocator allocator;
    return &allocator;
  }

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
// These replace large static char[] buffers that would otherwise live in .bss
// (internal RAM). The buffer is lazily allocated on first use and persists.

// Generic PSRAM buffer helper - returns a persistent PSRAM-backed buffer
// Usage: char* buf = getPsramBuffer<4096>("sddiag");
template<size_t SIZE>
inline char* getPsramBuffer(const char* tag = nullptr) {
  static char* buf = nullptr;
  if (!buf) {
    buf = (char*)ps_alloc(SIZE, AllocPref::PreferPSRAM, tag);
    if (buf) {
      buf[0] = '\0';
    }
  }
  return buf;
}

// Pre-defined buffer sizes for common use cases
// 1KB buffer for small command outputs
inline char* getPsramBuffer1K(const char* tag = nullptr) {
  return getPsramBuffer<1024>(tag);
}

// 2KB buffer for medium command outputs  
inline char* getPsramBuffer2K(const char* tag = nullptr) {
  return getPsramBuffer<2048>(tag);
}

// 4KB buffer for large command outputs
inline char* getPsramBuffer4K(const char* tag = nullptr) {
  return getPsramBuffer<4096>(tag);
}

// Macro for easy static buffer replacement
// Usage: PSRAM_STATIC_BUF(buf, 2048) replaces: static char buf[2048]
// Note: Also defines buf_SIZE constant for use instead of sizeof(buf)
#define PSRAM_STATIC_BUF(name, size) \
  static char* name = nullptr; \
  static constexpr size_t name##_SIZE = size; \
  if (!name) { \
    name = (char*)ps_alloc(size, AllocPref::PreferPSRAM, #name); \
    if (name) name[0] = '\0'; \
  } \
  if (!name) return "Error: Failed to allocate buffer"

