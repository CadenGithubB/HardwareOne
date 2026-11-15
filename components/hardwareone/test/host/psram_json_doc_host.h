#pragma once
// Host stand-in for PSRAM_JSON_DOC (System_MemUtil.h), for harnesses that
// compile production code. The document runs on a counting allocator, so a
// case can tell a PSRAM document from a plain `JsonDocument doc;`, which takes
// ArduinoJson's default malloc allocator and leaves these counters at zero.
#include <ArduinoJson.h>
#include <cstdlib>

struct HostPsramJsonAllocator : ArduinoJson::Allocator {
  unsigned long allocations = 0, frees = 0;
  void* allocate(size_t size) override { ++allocations; return std::malloc(size); }
  void deallocate(void* pointer) override { if (pointer) ++frees; std::free(pointer); }
  void* reallocate(void* pointer, size_t size) override { return std::realloc(pointer, size); }
};
inline HostPsramJsonAllocator gHostPsramJson;
#define PSRAM_JSON_DOC(name) JsonDocument name(&gHostPsramJson)
