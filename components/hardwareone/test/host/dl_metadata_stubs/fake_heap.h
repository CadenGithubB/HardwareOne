#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace fake {
enum class Origin { PsAlloc, HookAligned, Prebuilt, Data };
struct Budget {
  size_t psram = 64u << 20;
  size_t internal = 8u << 20;
};
struct Counts {
  size_t psAllocBlocks = 0, alignedBlocks = 0;  // blocks handed out (PSRAM or internal)
  size_t psFrees = 0, otherFrees = 0;           // blocks returned through ps_free / other paths
  size_t foreignThroughPsFree = 0;              // prebuilt-style blocks returned through ps_free
};
extern std::atomic<size_t> gMallocAlignedCalls;  // upstream path (no hook installed)
void reset(Budget budget = {});
void setBudget(Budget budget);
size_t live(Origin origin);
size_t liveAll();
size_t usedPsram();
size_t usedInternal();
Counts counts();
bool owns(const void* p);
Origin origin(const void* p);
size_t alignment(const void* p);
void* prebuiltAlloc(size_t size);  // what prebuilt libfbs_model does for TensorBase objects
void release(void* p, Origin path);
using DebugHook = void (*)(const char*, void*, size_t, bool, bool, bool, const char*);
}  // namespace fake
void fakeSetBypass(bool on);
void fakeSetDebugHook(fake::DebugHook hook);
bool fake_heap_is_psram(const void* p);
