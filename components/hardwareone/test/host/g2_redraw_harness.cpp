#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); } } while (false)

// Only production's nothrow allocations are fault-injected. Fixed bookkeeping
// avoids allocating while testing the allocations themselves.
static void* owned[2]{};
static unsigned allocationAttempts = 0;
static unsigned failAllocationAt = 0;
static unsigned liveAllocations = 0;

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  ++allocationAttempts;
  if (allocationAttempts == failAllocationAt) return nullptr;
  void* pointer = std::malloc(size);
  CHECK(pointer != nullptr);
  for (auto& slot : owned) {
    if (!slot) {
      slot = pointer;
      ++liveAllocations;
      return pointer;
    }
  }
  std::abort();
}

void operator delete(void* pointer) noexcept {
  if (!pointer) return;
  for (auto& slot : owned) {
    if (slot == pointer) {
      slot = nullptr;
      --liveAllocations;
      break;
    }
  }
  std::free(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  ::operator delete(pointer);
}
#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept {
  ::operator delete(pointer);
}
#endif

struct PageSwapArgs;
struct ToastSpec;
struct NotifySpec;
struct CustomSpec;
struct NativeNotifSpec;
// INSERT_TYPES

static unsigned renderCalls = 0;
static unsigned queueCalls = 0;
static bool queueAccepts = true;
static LensUiJob* queued = nullptr;
static uint32_t currentGeneration = 100;

template <typename... Args> void quietLog(const char*, Args...) {}
#define ERROR_MEMORYF(...) quietLog(__VA_ARGS__)
#define WARN_G2F(...) quietLog(__VA_ARGS__)
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)

uint32_t g2CurrentMenuGen() { return currentGeneration; }
G2HijackPage g2GetHijackPage() { return G2_HIJACK_PAGE_MAIN; }

bool g2EnqueueLensJob(LensUiJob* job, G2LensEnqueueWait wait) {
  ++queueCalls;
  CHECK(wait == G2LensEnqueueWait::Bounded50Ms);
  CHECK(job != nullptr);
  CHECK(job->kind == LensJobKind::Redraw);
  CHECK(job->payload.redraw != nullptr);
  CHECK(job->payload.redraw->render != nullptr);
  CHECK(liveAllocations == 2);
  CHECK(queued == nullptr);
  if (!queueAccepts) return false;
  queued = job;
  return true;
}

// INSERT_PRODUCTION

static void render() { ++renderCalls; }

static void dispatch() {
  CHECK(queued != nullptr);
  LensUiJob* job = queued;
  queued = nullptr;
  const uint32_t liveGen = g2CurrentMenuGen();
  switch (job->kind) {
    // INSERT_REDRAW_DISPATCH
    default: std::abort();
  }
  CHECK(job->payload.redraw == nullptr);
  CHECK(liveAllocations == 1);
  delete job;
  CHECK(liveAllocations == 0);
}

static void reset() {
  CHECK(liveAllocations == 0);
  CHECK(queued == nullptr);
  CHECK(owned[0] == nullptr && owned[1] == nullptr);
  allocationAttempts = failAllocationAt = renderCalls = queueCalls = 0;
  queueAccepts = true;
}

int main() {
  // Cookie metadata belongs to the submitted command. Advancing navigation or
  // the latest command sequence before its completion must not restamp it.
  const G2CmdCookie cookie{UINT64_C(0xfedcba9876543210), 37,
                           G2_HIJACK_PAGE_SETTINGS, 219};
  reset();
  currentGeneration = cookie.menuGen + 1;
  CHECK(g2EnqueueRedraw(cookie, render, "settings"));
  CHECK(queueCalls == 1 && renderCalls == 0);
  CHECK(queued->submitMenuGen == cookie.menuGen);
  CHECK(queued->cmdSeq == cookie.seq);
  CHECK(queued->targetPage == cookie.targetPage);
  CHECK(queued->targetNetSub == cookie.targetNetSub);
  CHECK(queued->payload.redraw->render == render);
  CHECK(cookie.menuGen == 37 && cookie.seq == UINT64_C(0xfedcba9876543210));
  CHECK(allocationAttempts == 2 && liveAllocations == 2);
  dispatch();
  CHECK(renderCalls == 0);  // Production worker rejects the stale completion.

  reset();
  currentGeneration = cookie.menuGen;
  CHECK(g2EnqueueRedraw(cookie, render, nullptr));
  CHECK(renderCalls == 0);
  dispatch();
  CHECK(renderCalls == 1);  // Current results render only on the worker.

  reset();
  CHECK(g2EnqueueRedraw(cookie, render, "network"));
  ++currentGeneration;  // Navigation after queue admission also invalidates it.
  dispatch();
  CHECK(renderCalls == 0);

  for (unsigned failure = 1; failure <= 2; ++failure) {
    reset();
    failAllocationAt = failure;
    CHECK(!g2EnqueueRedraw(cookie, render, nullptr));
    CHECK(allocationAttempts == failure);
    CHECK(queueCalls == 0 && queued == nullptr);
    CHECK(renderCalls == 0 && liveAllocations == 0);
  }

  reset();
  queueAccepts = false;  // Queue full or worker unavailable: no inline fallback.
  CHECK(!g2EnqueueRedraw(cookie, render, nullptr));
  CHECK(queueCalls == 1 && queued == nullptr);
  CHECK(renderCalls == 0 && liveAllocations == 0);

  reset();
  CHECK(!g2EnqueueRedraw(cookie, nullptr, nullptr));
  CHECK(allocationAttempts == 0 && queueCalls == 0);
  CHECK(renderCalls == 0 && liveAllocations == 0);

  reset();
  std::printf("G2 production redraw submission/dispatch: %u checks passed\n", checks);
}
