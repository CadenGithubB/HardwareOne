#pragma once
#include <Arduino.h>
extern "C" {
  #include "esp_heap_caps.h"
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
