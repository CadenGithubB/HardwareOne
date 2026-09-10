#include "System_MemUtil.h"
#include "System_PsramBuffer.h"

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <limits>
#include <new>
#include <vector>

namespace {

constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

struct RegionEntry {
  void* ptr;
  bool external;
};

struct DebugEvent {
  const char* op = nullptr;
  void* ptr = nullptr;
  size_t size = 0;
  bool requestedPS = false;
  bool usedPS = false;
  bool fellBack = false;
  const char* tag = nullptr;
};

std::vector<RegionEntry> gRegions;
std::vector<uint32_t> gCapsCalls;
std::vector<uint32_t> gPreferCaps;
bool gFailInternal = false;
bool gFailPsram = false;
size_t gPsramTotal = 8u * 1024u * 1024u;
size_t gPsramFree = 8u * 1024u * 1024u;
unsigned gBackendReallocZeroCalls = 0;
unsigned gDebugCalls = 0;
DebugEvent gLastDebug;

bool capsAreExternal(uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) != 0;
}

bool capsFail(uint32_t caps) {
  return capsAreExternal(caps) ? gFailPsram : gFailInternal;
}

void rememberRegion(void* ptr, bool external) {
  if (!ptr) return;
  for (RegionEntry& entry : gRegions) {
    if (entry.ptr == ptr) {
      entry.external = external;
      return;
    }
  }
  gRegions.push_back({ptr, external});
}

void forgetRegion(void* ptr) {
  gRegions.erase(
      std::remove_if(gRegions.begin(), gRegions.end(),
                     [ptr](const RegionEntry& entry) { return entry.ptr == ptr; }),
      gRegions.end());
}

void* allocateForCaps(size_t size, uint32_t caps, bool zeroed) {
  gCapsCalls.push_back(caps);
  if (capsFail(caps)) return nullptr;
  void* ptr = zeroed ? ::calloc(1, size) : ::malloc(size);
  rememberRegion(ptr, capsAreExternal(caps));
  return ptr;
}

void* reallocateForCaps(void* ptr, size_t size, uint32_t caps) {
  gCapsCalls.push_back(caps);
  if (size == 0) {
    ++gBackendReallocZeroCalls;
    forgetRegion(ptr);
    ::free(ptr);
    return nullptr;
  }
  if (capsFail(caps)) return nullptr;
  void* result = ::realloc(ptr, size);
  if (result) {
    forgetRegion(ptr);
    rememberRegion(result, capsAreExternal(caps));
  }
  return result;
}

void resetMocks() {
  gRegions.clear();
  gCapsCalls.clear();
  gPreferCaps.clear();
  gFailInternal = false;
  gFailPsram = false;
  gPsramTotal = 8u * 1024u * 1024u;
  gPsramFree = gPsramTotal;
  gBackendReallocZeroCalls = 0;
  gDebugCalls = 0;
  gLastDebug = {};
  gPsAllocFallbacks.store(0, std::memory_order_relaxed);
  setPsramBypass(false);
}

void assertCaps(const std::vector<uint32_t>& actual,
                std::initializer_list<uint32_t> expected) {
  assert(actual.size() == expected.size());
  assert(std::equal(actual.begin(), actual.end(), expected.begin()));
}

void testPolicyRouting() {
  resetMocks();
  void* defaultPtr = ps_alloc(32, AllocPolicy::DefaultHeap, "test.default");
  assert(defaultPtr != nullptr);
  assert(gCapsCalls.empty());
  assert(gDebugCalls == 1 && !gLastDebug.usedPS && !gLastDebug.fellBack);
  ps_free(defaultPtr);

  resetMocks();
  void* psramPtr = ps_alloc(64, AllocPolicy::PreferPSRAM, "test.prefer_ps");
  assert(psramPtr != nullptr && esp_ptr_external_ram(psramPtr));
  assertCaps(gPreferCaps, {kPsramCaps, kInternalCaps});
  assert(gLastDebug.requestedPS && gLastDebug.usedPS && !gLastDebug.fellBack);
  assert(psAllocFallbackCount() == 0);
  ps_free(psramPtr);

  resetMocks();
  gFailPsram = true;
  void* fallbackPtr = ps_alloc(96, AllocPolicy::PreferPSRAM, "test.fallback");
  assert(fallbackPtr != nullptr && !esp_ptr_external_ram(fallbackPtr));
  assertCaps(gPreferCaps, {kPsramCaps, kInternalCaps});
  assert(gLastDebug.requestedPS && !gLastDebug.usedPS && gLastDebug.fellBack);
  assert(psAllocFallbackCount() == 1);
  ps_free(fallbackPtr);

  resetMocks();
  gFailInternal = true;
  void* preferInternal =
      ps_alloc(48, AllocPolicy::PreferInternal, "test.prefer_internal");
  assert(preferInternal != nullptr && esp_ptr_external_ram(preferInternal));
  assertCaps(gPreferCaps, {kInternalCaps, kPsramCaps});
  assert(!gLastDebug.requestedPS && gLastDebug.usedPS && !gLastDebug.fellBack);
  ps_free(preferInternal);

  resetMocks();
  gFailPsram = true;
  assert(ps_alloc(48, AllocPolicy::RequirePSRAM, "test.require_ps") == nullptr);
  assertCaps(gCapsCalls, {kPsramCaps});
  assert(gDebugCalls == 1 && gLastDebug.ptr == nullptr && !gLastDebug.fellBack);

  resetMocks();
  void* internal =
      ps_alloc(48, AllocPolicy::RequireInternal, "test.require_internal");
  assert(internal != nullptr && !esp_ptr_external_ram(internal));
  assertCaps(gCapsCalls, {kInternalCaps});
  ps_free(internal);
}

void testBypassAndRuntimePresence() {
  resetMocks();
  gPsramFree = 0;
  assert(psramAvailableRuntime());

  setPsramBypass(true);
  void* ptr = ps_alloc(32, AllocPolicy::PreferPSRAM, "test.bypass");
  assert(ptr != nullptr && !esp_ptr_external_ram(ptr));
  assert(gPreferCaps.empty());
  assertCaps(gCapsCalls, {kInternalCaps});
  assert(gLastDebug.requestedPS && !gLastDebug.fellBack);
  assert(psAllocFallbackCount() == 0);
  ps_free(ptr);

  gCapsCalls.clear();
  gDebugCalls = 0;
  assert(ps_alloc(32, AllocPolicy::RequirePSRAM, "test.bypass.strict") == nullptr);
  assert(gCapsCalls.empty());
  assert(gDebugCalls == 1 && gLastDebug.ptr == nullptr);
}

void testNoRegisteredPsram() {
  resetMocks();
  gPsramTotal = 0;
  gPsramFree = 0;
  assert(!psramAvailableRuntime());
  void* ptr = ps_alloc(48, AllocPolicy::PreferPSRAM, "test.no_registered_psram");
  assert(ptr != nullptr && !esp_ptr_external_ram(ptr));
  assert(gPreferCaps.empty());
  assertCaps(gCapsCalls, {kInternalCaps});
  assert(gLastDebug.requestedPS && !gLastDebug.usedPS && !gLastDebug.fellBack);
  assert(psAllocFallbackCount() == 0);
  memset(ptr, 0x6c, 48);
  gCapsCalls.clear();
  void* grown = ps_realloc(ptr, 80, AllocPolicy::PreferPSRAM, "test.no_psram_realloc");
  assert(grown != nullptr && static_cast<unsigned char*>(grown)[0] == 0x6c);
  assertCaps(gCapsCalls, {kInternalCaps});
  ps_free(grown);

  gCapsCalls.clear();
  auto* zeroed = static_cast<unsigned char*>(
      ps_calloc(8, 1, AllocPolicy::PreferPSRAM, "test.no_psram_calloc"));
  assert(zeroed != nullptr);
  for (size_t i = 0; i < 8; ++i) assert(zeroed[i] == 0);
  assertCaps(gCapsCalls, {kInternalCaps});
  ps_free(zeroed);

  gCapsCalls.clear();
  assert(ps_alloc(48, AllocPolicy::RequirePSRAM, "test.no_psram_strict") == nullptr);
  assert(gCapsCalls.empty());
  gFailInternal = true;
  gFailPsram = true;
  assert(ps_alloc(48, AllocPolicy::PreferPSRAM, "test.no_psram_real_oom") == nullptr);
  assertCaps(gCapsCalls, {kInternalCaps});
}

void testNoCompileTimePsram() {
  resetMocks();
  assert(!hasPSRAMAvail());
  // Even an inconsistent mock reporting external capacity must not make a
  // no-PSRAM build request it: the compile-time guard precedes the heap query.
  assert(gPsramTotal > 0 && !psramAvailableRuntime());
  void* ptr = ps_alloc(64, AllocPolicy::PreferPSRAM, "test.no_compile_psram");
  assert(ptr != nullptr && !esp_ptr_external_ram(ptr));
  assert(gPreferCaps.empty());
  assertCaps(gCapsCalls, {kInternalCaps});
  assert(!gLastDebug.fellBack && psAllocFallbackCount() == 0);
  ps_free(ptr);
  gCapsCalls.clear();
  gFailInternal = true;
  gFailPsram = true;
  assert(ps_alloc(64, AllocPolicy::PreferPSRAM, "test.no_compile_psram_oom") == nullptr);
  assertCaps(gCapsCalls, {kInternalCaps});
}

struct ParentLifetimeProbe {
  explicit ParentLifetimeProbe(unsigned* count) : destroyed(count) {}
  ~ParentLifetimeProbe() {
    // ASan catches any attempted free-before-destructor ordering here.
    value = 0;
    ++*destroyed;
  }
  unsigned* destroyed;
  volatile uint32_t value = 0x5a5a5a5a;
};

void testPlacementParentOwnership() {
  for (int mode = 0; mode < 5; ++mode) {
    resetMocks();
    if (mode == 1) gFailPsram = true;
    if (mode == 2) setPsramBypass(true);
    if (mode == 3) { gPsramTotal = 0; gPsramFree = 0; }
    if (mode == 4) { gFailPsram = true; gFailInternal = true; }
    unsigned destroyed = 0;
    void* storage = ps_alloc(sizeof(ParentLifetimeProbe), AllocPolicy::PreferPSRAM,
                            "test.sensor_parent");
    if (mode == 4) {
      assert(storage == nullptr && destroyed == 0);
      ps_delete<ParentLifetimeProbe>(nullptr);
      continue;
    }
    assert(storage != nullptr);
    const bool expectExternal = hasPSRAMAvail() && mode == 0;
    assert(esp_ptr_external_ram(storage) == expectExternal);
    auto* parent = new (storage) ParentLifetimeProbe(&destroyed);
    assert(parent->value == 0x5a5a5a5a && destroyed == 0);
    ps_delete(parent);
    assert(destroyed == 1);
    ps_delete<ParentLifetimeProbe>(nullptr);
    assert(destroyed == 1);
  }
}

void testCallocAndZeroContracts() {
  resetMocks();
  assert(ps_alloc(0, AllocPolicy::PreferPSRAM, "test.zero") == nullptr);
  assert(ps_calloc(0, 4, AllocPolicy::PreferPSRAM, "test.zero") == nullptr);
  assert(ps_calloc(4, 0, AllocPolicy::PreferPSRAM, "test.zero") == nullptr);
  assert(gCapsCalls.empty() && gDebugCalls == 0);

  assert(ps_calloc(std::numeric_limits<size_t>::max(), 2,
                   AllocPolicy::PreferPSRAM, "test.overflow") == nullptr);
  assert(gCapsCalls.empty());

  uint32_t* values = static_cast<uint32_t*>(
      ps_calloc(4, sizeof(uint32_t), AllocPolicy::RequireInternal,
                "test.calloc"));
  assert(values != nullptr);
  for (size_t i = 0; i < 4; ++i) assert(values[i] == 0);
  assert(gLastDebug.size == 4 * sizeof(uint32_t));
  ps_free(values);
}

void testReallocContracts() {
  resetMocks();
  void* ptr = ps_alloc(32, AllocPolicy::RequireInternal, "test.realloc.seed");
  assert(ptr != nullptr);
  memset(ptr, 0x5a, 32);
  gDebugCalls = 0;
  assert(ps_realloc(ptr, 0, AllocPolicy::PreferPSRAM, "test.realloc.zero") ==
         nullptr);
  assert(gBackendReallocZeroCalls == 0);
  assert(gDebugCalls == 0);

  resetMocks();
  ptr = ps_alloc(32, AllocPolicy::RequireInternal, "test.realloc.keep");
  assert(ptr != nullptr);
  memset(ptr, 0x3c, 32);
  gFailInternal = true;
  gDebugCalls = 0;
  assert(ps_realloc(ptr, 64, AllocPolicy::RequireInternal,
                    "test.realloc.keep") == nullptr);
  assert(gDebugCalls == 1 && gLastDebug.ptr == nullptr);
  assert(static_cast<unsigned char*>(ptr)[0] == 0x3c);
  ps_free(ptr);

  resetMocks();
  ptr = ps_alloc(32, AllocPolicy::RequireInternal, "test.realloc.fallback");
  assert(ptr != nullptr);
  gFailPsram = true;
  gDebugCalls = 0;
  void* fallback = ps_realloc(ptr, 64, AllocPolicy::PreferPSRAM,
                              "test.realloc.fallback");
  assert(fallback != nullptr && !esp_ptr_external_ram(fallback));
  assert(gDebugCalls == 1 && gLastDebug.fellBack);
  assert(psAllocFallbackCount() == 1);
  ps_free(fallback);

  resetMocks();
  void* allocated =
      ps_realloc(nullptr, 24, AllocPolicy::RequirePSRAM, "test.realloc.null");
  assert(allocated != nullptr && esp_ptr_external_ram(allocated));
  ps_free(allocated);

  resetMocks();
  void* jsonPtr = PsramJsonAllocator::instance()->allocate(16);
  assert(jsonPtr != nullptr);
  assert(PsramJsonAllocator::instance()->reallocate(jsonPtr, 0) == nullptr);
  assert(gBackendReallocZeroCalls == 0);
}

void testPsramBuffer() {
  for (int mode = 0; mode < 4; ++mode) {
    resetMocks();
    if (mode == 1) setPsramBypass(true);
    if (mode == 2) gFailPsram = true;
    if (mode == 3) gPsramTotal = 0;
    PsramBuffer out(1024, "test.output");
    assert(out.ok() && out.size() == 0 && out.capacity() == 0);
    assert(strcmp(out.c_str(), "") == 0);
    assert(out.reserve(0) && out.capacity() == 0);
    assert(out.append("abc"));
    assert(esp_ptr_external_ram(out.data()) == (hasPSRAMAvail() && mode == 0));
    const std::string prefix(200, 'x');
    assert(out.append(prefix.c_str(), prefix.size()));
    const std::string original = out.c_str();
    assert(out.append(out.data(), out.size()));  // forces growth and may move
    assert(std::string(out.c_str()) == original + original);
    assert(out.data()[out.size()] == '\0');
    assert(out.reserveAdditional(10));
    const size_t before = out.size();
    memcpy(out.data() + before, "tail", 4);
    assert(out.setSize(before + 4));  // commit must preserve direct-write bytes
    assert(std::string(out.c_str()) == original + original + "tail");
    const std::string saved = out.c_str();
    assert(!out.reserveAdditional(SIZE_MAX));
    assert(out.failure() == PsramBuffer::Failure::Limit);
    assert(!out.append("must not appear"));
    assert(std::string(out.c_str()) == saved);
    const size_t retained = out.capacity();
    out.clear();
    assert(out.ok() && out.size() == 0 && out.capacity() == retained);
    assert(out.append("reused"));
  }
  resetMocks();
  {
    PsramBuffer zero(0, "test.zero");
    assert(zero.reserve(0) && zero.append(""));
    assert(!zero.append('x') && zero.failure() == PsramBuffer::Failure::Limit);
    PsramBuffer exact(4, "test.exact");
    assert(exact.append("abc") && exact.size() == 3 && exact.capacity() == 4);
    assert(!exact.append('d') && strcmp(exact.c_str(), "abc") == 0);
    exact.clear();
    assert(!exact.setSize(4) && exact.failure() == PsramBuffer::Failure::Limit);
    exact.clear();
    assert(!exact.append(nullptr, 1));
  }
  resetMocks();
  {
    PsramBuffer out(1024, "test.failure");
    gFailInternal = gFailPsram = true;
    assert(!out.reserve(1));
    assert(out.failure() == PsramBuffer::Failure::Allocation && out.data() == nullptr);
    gFailInternal = gFailPsram = false;
    assert(!out.append("sticky"));
    out.clear();
    assert(out.append("retained"));
    char* original = out.data();
    gFailInternal = gFailPsram = true;
    assert(!out.reserve(1024));
    assert(out.failure() == PsramBuffer::Failure::Allocation);
    assert(out.data() == original && strcmp(out.c_str(), "retained") == 0);
    out.clear();
    assert(out.append("no allocation needed"));
  }
  resetMocks();
}

}  // namespace

extern "C" size_t heap_caps_get_free_size(uint32_t caps) {
  return capsAreExternal(caps) ? gPsramFree : 1024u * 1024u;
}

extern "C" size_t heap_caps_get_minimum_free_size(uint32_t caps) {
  return heap_caps_get_free_size(caps);
}

extern "C" size_t heap_caps_get_largest_free_block(uint32_t caps) {
  return heap_caps_get_free_size(caps);
}

extern "C" size_t heap_caps_get_total_size(uint32_t caps) {
  return capsAreExternal(caps) ? gPsramTotal : 1024u * 1024u;
}

extern "C" void* heap_caps_malloc(size_t size, uint32_t caps) {
  return allocateForCaps(size, caps, false);
}

extern "C" void* heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
  if (n != 0 && size > std::numeric_limits<size_t>::max() / n) return nullptr;
  return allocateForCaps(n * size, caps, true);
}

extern "C" void* heap_caps_realloc(void* ptr, size_t size, uint32_t caps) {
  return reallocateForCaps(ptr, size, caps);
}

extern "C" void* heap_caps_malloc_prefer(size_t size, size_t num, ...) {
  va_list args;
  va_start(args, num);
  void* result = nullptr;
  for (size_t i = 0; i < num; ++i) {
    const uint32_t caps = va_arg(args, uint32_t);
    gPreferCaps.push_back(caps);
    if (!result) result = allocateForCaps(size, caps, false);
  }
  va_end(args);
  return result;
}

extern "C" void* heap_caps_calloc_prefer(size_t n, size_t size, size_t num, ...) {
  va_list args;
  va_start(args, num);
  void* result = nullptr;
  for (size_t i = 0; i < num; ++i) {
    const uint32_t caps = va_arg(args, uint32_t);
    gPreferCaps.push_back(caps);
    if (!result) result = heap_caps_calloc(n, size, caps);
  }
  va_end(args);
  return result;
}

extern "C" void* heap_caps_realloc_prefer(void* ptr, size_t size, size_t num, ...) {
  va_list args;
  va_start(args, num);
  void* result = nullptr;
  for (size_t i = 0; i < num; ++i) {
    const uint32_t caps = va_arg(args, uint32_t);
    gPreferCaps.push_back(caps);
    if (!result) result = reallocateForCaps(ptr, size, caps);
  }
  va_end(args);
  return result;
}

extern "C" bool esp_ptr_external_ram(const void* ptr) {
  for (auto it = gRegions.rbegin(); it != gRegions.rend(); ++it) {
    if (it->ptr == ptr) return it->external;
  }
  return false;
}

extern "C" void memAllocDebug(const char* op, void* ptr, size_t size,
                               bool requestedPS, bool usedPS, bool fellBack,
                               const char* tag) {
  ++gDebugCalls;
  gLastDebug = {op, ptr, size, requestedPS, usedPS, fellBack, tag};
}

int main() {
  if (hasPSRAMAvail()) {
    testPolicyRouting();
    testBypassAndRuntimePresence();
    testReallocContracts();
  } else {
    testNoCompileTimePsram();
  }
  testNoRegisteredPsram();
  testPlacementParentOwnership();
  testCallocAndZeroContracts();
  testPsramBuffer();
  puts("System_MemUtil host tests passed");
  return 0;
}
