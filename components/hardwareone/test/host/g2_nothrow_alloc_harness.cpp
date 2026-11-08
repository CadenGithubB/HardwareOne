// Host harness: G2 page launchers refuse, not abort, when the heap is out.
// The firmware builds without C++ exceptions, so a failing plain `new` ends in
// std::terminate -> abort -> panic reboot. This harness models that exactly:
// the throwing operator new aborts the process when the heap is "exhausted",
// while the nothrow form returns nullptr. The runner inserts the production
// argument structs, drop functions and launchers from G2_Glasses.cpp; the
// session queue, workers and heap probes are boundary doubles.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); \
} } while (false)

static bool gHeapExhausted = false;
static unsigned gLiveObjects = 0;

void* operator new(std::size_t size) {
  if (gHeapExhausted) {
    std::fprintf(stderr, "plain operator new under exhaustion: firmware would abort\n");
    std::abort();
  }
  void* p = std::malloc(size ? size : 1);
  if (!p) std::abort();
  ++gLiveObjects;
  return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if (gHeapExhausted) return nullptr;
  void* p = std::malloc(size ? size : 1);
  if (p) ++gLiveObjects;
  return p;
}
void operator delete(void* p) noexcept { if (p) { --gLiveObjects; std::free(p); } }
void operator delete(void* p, std::size_t) noexcept { if (p) { --gLiveObjects; std::free(p); } }
void operator delete(void* p, const std::nothrow_t&) noexcept { if (p) { --gLiveObjects; std::free(p); } }

#define DEBUG_G2F(...) do {} while (false)
static constexpr unsigned MALLOC_CAP_INTERNAL = 1u << 11;
static constexpr unsigned MALLOC_CAP_8BIT = 1u << 2;
static size_t heap_caps_get_largest_free_block(unsigned) { return 64 * 1024; }
struct { uint32_t getFreeHeap() const { return 64 * 1024; } } ESP;

static volatile uint32_t gWidgetLifecycleEpoch = 1;
static volatile uint32_t gHealthPageGen = 0;

static unsigned gSubmits = 0;
static void* gQueued = nullptr;
static void (*gQueuedDrop)(void*) = nullptr;
static bool g2SessionSubmit(void (*run)(void*), void* payload, void (*drop)(void*),
                            const char*, uint32_t) {
  CHECK(run && payload && drop);
  ++gSubmits;
  gQueued = payload;
  gQueuedDrop = drop;
  return true;
}
static void g2HealthPageWorker(void*) {}
static void g2MapPageWorker(void*) {}
static void g2BmpViewerWorker(void*) {}

// INSERT_G2_SITES

static void drainQueued() {
  if (gQueued) gQueuedDrop(gQueued);
  gQueued = nullptr;
  gQueuedDrop = nullptr;
}

int main() {
  // With memory, each launcher allocates its arguments and hands them to the
  // session queue (the drop releases them).
  CHECK(g2ShowHealthPage(nullptr));
  drainQueued();
  CHECK(g2ShowMapPage(nullptr));
  drainQueued();
  CHECK(g2ShowBmpFile("/sd/a.bmp", nullptr));
  drainQueued();
  CHECK(gSubmits == 3 && gLiveObjects == 0);

  // Out of memory: every launcher refuses (returns false) and submits nothing.
  gHeapExhausted = true;
  CHECK(!g2ShowHealthPage(nullptr));
  CHECK(!g2ShowMapPage(nullptr));
  CHECK(!g2ShowBmpFile("/sd/a.bmp", nullptr));
  gHeapExhausted = false;
  CHECK(gSubmits == 3 && gQueued == nullptr && gLiveObjects == 0);

  std::printf("g2 nothrow launchers: %u checks passed\n", checks);
  return 0;
}
