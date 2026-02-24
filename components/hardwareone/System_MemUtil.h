#pragma once
#include <Arduino.h>
extern "C" {
  #include "esp_heap_caps.h"
  #include "esp_memory_utils.h"
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
      return p;
    }
  }
  __capture_mem_before();
  void* p2 = malloc(size);
  return p2;
}

// Tagged overload: record a human-readable name for this allocation
inline void* ps_alloc(size_t size, AllocPref pref, const char* tag) {
  const bool wantPS = (pref == AllocPref::PreferPSRAM) && !psramBypassGlobal() && psramAvailableRuntime();
  if (wantPS) {
    __capture_mem_before();
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (p) {
      return p;
    }
  }
  __capture_mem_before();
  void* p2 = malloc(size);
  return p2;
}

inline void* ps_calloc(size_t n, size_t size, AllocPref pref = AllocPref::PreferPSRAM) {
  const bool wantPS = (pref == AllocPref::PreferPSRAM) && !psramBypassGlobal() && psramAvailableRuntime();
  if (wantPS) {
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

// Tagged overload
inline void* ps_calloc(size_t n, size_t size, AllocPref pref, const char* tag) {
  const bool wantPS = (pref == AllocPref::PreferPSRAM) && !psramBypassGlobal() && psramAvailableRuntime();
  if (wantPS) {
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

inline void* ps_realloc(void* ptr, size_t size, AllocPref pref = AllocPref::PreferPSRAM) {
  const bool wantPS = (pref == AllocPref::PreferPSRAM) && !psramBypassGlobal() && psramAvailableRuntime();
  if (wantPS) {
    __capture_mem_before();
    void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (p) {
      return p;
    }
    // Fall through to internal realloc if PSRAM attempt failed
  }
  __capture_mem_before();
  void* p2 = realloc(ptr, size);
  return p2;
}

// Tagged overload
inline void* ps_realloc(void* ptr, size_t size, AllocPref pref, const char* tag) {
  const bool wantPS = (pref == AllocPref::PreferPSRAM) && !psramBypassGlobal() && psramAvailableRuntime();
  if (wantPS) {
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

