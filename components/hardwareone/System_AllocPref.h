#pragma once

#include <cstddef>
#include <cstdint>

// The allocation choice. Numeric values stay fixed: host tests and saved call
// sites depend on them. RequireInternal is internal memory only, never PSRAM.
enum class AllocPref : uint8_t {
  PreferPsram     = 0,  // SPIRAM|8BIT, then INTERNAL|8BIT
  PreferInternal  = 1,  // INTERNAL|8BIT, then SPIRAM|8BIT
  RequirePsram    = 2,  // SPIRAM|8BIT only
  RequireInternal = 3,  // INTERNAL|8BIT only
  DefaultHeap     = 4   // libc allocator, unless the global bypass is enabled
};
using AllocPolicy = AllocPref;

void* ps_alloc(size_t size, AllocPolicy policy, const char* tag);
void* ps_calloc(size_t n, size_t size, AllocPolicy policy, const char* tag);
void* ps_realloc(void* ptr, size_t size, AllocPolicy policy, const char* tag);
void ps_free(void* ptr);
