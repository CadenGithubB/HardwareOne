// Host fake of the ESP-IDF heap and of System_MemUtil's ps_alloc family, for
// stt_dl_metadata_harness.cpp. Every block is registered with where it lives (PSRAM or
// internal RAM) and which allocator made it, so the test can tell which free path
// returned it. PSRAM and internal RAM each have a byte budget the test can shrink.
#include "fake_heap.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "System_MemUtil.h"
#include "esp_heap_caps.h"
#include "dl_define.hpp"

namespace fake {
namespace {
struct Block {
  size_t size;
  Origin origin;
  bool psram;
  size_t align;
};
std::mutex gMutex;
std::map<const void*, Block> gBlocks;
Budget gBudget;
size_t gPsramUsed = 0, gInternalUsed = 0;
Counts gCounts;
}  // namespace
std::atomic<size_t> gMallocAlignedCalls{0};
namespace {

void* take(size_t align, size_t size, bool psram, Origin origin) {
  std::lock_guard<std::mutex> lock(gMutex);
  size_t& used = psram ? gPsramUsed : gInternalUsed;
  const size_t limit = psram ? gBudget.psram : gBudget.internal;
  if (used + size > limit) return nullptr;
  void* p = nullptr;
  if (align < sizeof(void*)) align = sizeof(void*);
  if (posix_memalign(&p, align, size ? size : 1)) return nullptr;
  std::memset(p, 0xa5, size);
  used += size;
  gBlocks.emplace(p, Block{size, origin, psram, align});
  if (origin == Origin::PsAlloc) gCounts.psAllocBlocks++;
  if (origin == Origin::HookAligned) gCounts.alignedBlocks++;
  return p;
}
}  // namespace

void reset(Budget budget) {
  std::lock_guard<std::mutex> lock(gMutex);
  assert(gBlocks.empty());
  gBudget = budget;
  gPsramUsed = gInternalUsed = 0;
  gCounts = {};
}
void setBudget(Budget budget) {
  std::lock_guard<std::mutex> lock(gMutex);
  gBudget = budget;
}
size_t live(Origin origin) {
  std::lock_guard<std::mutex> lock(gMutex);
  size_t n = 0;
  for (const auto& b : gBlocks) n += b.second.origin == origin;
  return n;
}
size_t liveAll() {
  std::lock_guard<std::mutex> lock(gMutex);
  return gBlocks.size();
}
size_t usedPsram() {
  std::lock_guard<std::mutex> lock(gMutex);
  return gPsramUsed;
}
size_t usedInternal() {
  std::lock_guard<std::mutex> lock(gMutex);
  return gInternalUsed;
}
Counts counts() {
  std::lock_guard<std::mutex> lock(gMutex);
  return gCounts;
}
bool owns(const void* p) {
  std::lock_guard<std::mutex> lock(gMutex);
  return gBlocks.count(p) != 0;
}
Origin origin(const void* p) {
  std::lock_guard<std::mutex> lock(gMutex);
  auto it = gBlocks.find(p);
  assert(it != gBlocks.end());
  return it->second.origin;
}
size_t alignment(const void* p) {
  std::lock_guard<std::mutex> lock(gMutex);
  auto it = gBlocks.find(p);
  assert(it != gBlocks.end());
  return it->second.align;
}
void* prebuiltAlloc(size_t size) { return take(16, size, true, Origin::Prebuilt); }
// Release through one free path; `allowed` lists the origins that path may see.
void release(void* p, Origin path) {
  if (!p) return;
  std::lock_guard<std::mutex> lock(gMutex);
  auto it = gBlocks.find(p);
  assert(it != gBlocks.end() && "free of a block the fake heap never handed out (or a double free)");
  const Origin o = it->second.origin;
  // The metadata hook's blocks come back only through the hook's free (ps_free).
  if (o == Origin::PsAlloc || o == Origin::HookAligned) assert(path == Origin::PsAlloc);
  (path == Origin::PsAlloc ? gCounts.psFrees : gCounts.otherFrees)++;
  if (o == Origin::Prebuilt && path == Origin::PsAlloc) gCounts.foreignThroughPsFree++;
  (it->second.psram ? gPsramUsed : gInternalUsed) -= it->second.size;
  gBlocks.erase(it);
  std::free(p);
}
}  // namespace fake

bool fake_heap_is_psram(const void* p) {
  std::lock_guard<std::mutex> lock(fake::gMutex);
  auto it = fake::gBlocks.find(p);
  return it != fake::gBlocks.end() && it->second.psram;
}

// ---- esp_heap_caps.h ----
void* heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
  assert(alignment && !(alignment & (alignment - 1)));
  assert(!(caps & MALLOC_CAP_SIMD) && "metadata never asks for SIMD (cache-line padding)");
  const bool psram = caps & MALLOC_CAP_SPIRAM;
  assert(psram || (caps & MALLOC_CAP_INTERNAL));
  return fake::take(alignment, size, psram, fake::Origin::HookAligned);
}
void* heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps) {
  void* p = fake::take(alignment, n * size, caps & MALLOC_CAP_SPIRAM, fake::Origin::Data);
  if (p) std::memset(p, 0, n * size);
  return p;
}
void heap_caps_free(void* p) { fake::release(p, fake::Origin::Data); }
size_t heap_caps_get_free_size(uint32_t caps) {
  std::lock_guard<std::mutex> lock(fake::gMutex);
  return caps & MALLOC_CAP_SPIRAM ? fake::gBudget.psram - fake::gPsramUsed : fake::gBudget.internal - fake::gInternalUsed;
}
size_t heap_caps_get_largest_free_block(uint32_t caps) { return heap_caps_get_free_size(caps); }

// ---- System_MemUtil.h ----
std::atomic<uint32_t> gPsAllocFallbacks{0};
static std::atomic<bool> gBypass{false};
bool psramBypassEnabled() { return gBypass.load(); }
void fakeSetBypass(bool on) { gBypass.store(on); }
bool psramAvailableRuntime() { return true; }
static fake::DebugHook gDebugHook = nullptr;
void fakeSetDebugHook(fake::DebugHook hook) { gDebugHook = hook; }
extern "C" void memAllocDebug(const char* op, void* ptr, size_t size, bool requestedPS, bool usedPS, bool fellBack,
                              const char* tag) {
  if (gDebugHook) gDebugHook(op, ptr, size, requestedPS, usedPS, fellBack, tag);
}
// As System_MemUtil.cpp: PreferPSRAM = PSRAM, then internal; a fallback is counted.
void* ps_alloc(size_t size, AllocPolicy policy, const char* tag) {
  assert(policy == AllocPolicy::PreferPSRAM && tag && !std::strncmp(tag, "stt.", 4));
  if (size == 0) return nullptr;
  const bool bypass = psramBypassEnabled();
  void* p = bypass ? nullptr : fake::take(4, size, true, fake::Origin::PsAlloc);
  if (!p) p = fake::take(4, size, false, fake::Origin::PsAlloc);
  const bool used = p && fake_heap_is_psram(p);
  const bool fellBack = !bypass && p && !used;
  if (fellBack) gPsAllocFallbacks.fetch_add(1);
  memAllocDebug("malloc", p, size, true, used, fellBack, tag);
  return p;
}
void ps_free(void* p) { fake::release(p, fake::Origin::PsAlloc); }

// ---- the upstream allocator ESP-DL keeps for unhooked use (dl_tool.cpp) ----
namespace dl::tool {
void* malloc_aligned(size_t size, uint32_t caps) {
  assert(caps == MALLOC_CAP_DEFAULT);
  fake::gMallocAlignedCalls.fetch_add(1);
  // Not registered: with no hook installed metadata_free uses std::free.
  void* p = nullptr;
  return posix_memalign(&p, 16, size ? size : 1) ? nullptr : p;
}
void* calloc_aligned(size_t n, size_t size, uint32_t caps) { return heap_caps_aligned_calloc(16, n, size, caps); }
void copy_memory(void* dst, void* src, const size_t n) { std::memcpy(dst, src, n); }
memory_addr_type_t memory_addr_type(void* p) { return fake_heap_is_psram(p) ? MEMORY_ADDR_PSRAM : MEMORY_ADDR_INTERNAL; }
}  // namespace dl::tool
