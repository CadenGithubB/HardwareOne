// PROTOTYPE PROBE: generation-fenced console (derived from production by make_proto.py).
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "OLED_ConsoleBuffer.h"   // the PROTOTYPE header next to this file
using SemaphoreHandle_t = void*;
static std::timed_mutex gMutex;
static std::atomic<int> gHeld{0};
static bool gAllowMutex = true;
static SemaphoreHandle_t xSemaphoreCreateMutex() { return gAllowMutex ? (void*)&gMutex : nullptr; }
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE 1
static int xSemaphoreTake(SemaphoreHandle_t h, uint32_t ticks) {
  auto* m = static_cast<std::timed_mutex*>(h);
  bool ok;
  if (ticks == portMAX_DELAY) { m->lock(); ok = true; }
  else ok = m->try_lock_for(std::chrono::milliseconds(ticks));
  if (ok) gHeld.fetch_add(1);
  return ok ? 1 : 0;
}
static void xSemaphoreGive(SemaphoreHandle_t h) { gHeld.fetch_sub(1); static_cast<std::timed_mutex*>(h)->unlock(); }
enum class AllocPref { RequireInternal };
static bool gAllowAlloc = true;
static void* ps_calloc(size_t n, size_t size, AllocPref, const char*) { return gAllowAlloc ? calloc(n, size) : nullptr; }
static void heap_caps_free(void* p) { free(p); }
static uint32_t gNow = 0;
static uint32_t millis() { return gNow; }
static struct { int oledCliHistorySize = 50; } gSettings;
static void noLog(const char*, ...) {}
#define ERROR_SYSTEMF(...) noLog(__VA_ARGS__)
#define DEBUG_SYSTEMF(...) noLog(__VA_ARGS__)

#include "proto_methods.inc"

OLEDConsoleBuffer gOledConsole;
using namespace oled_console;
static const uint32_t DAY = 86400000u;

// One queue envelope as the prototype would carry it.
struct Env { std::string text; uint32_t ts; uint32_t gen; };
// Producer side: what enqueueChunk would do (stamp + capture, no lock).
static Env enqueue(OLEDConsoleBuffer& b, const char* text) { return {text, gNow, b.deliveryGeneration()}; }
// Consumer side: what debugOutputTask's OLED branch would do.
static bool deliver(OLEDConsoleBuffer& b, const Env& e) {
  const uint32_t f0 = b.fencedCount(), g0 = b.generation();
  b.append(e.text.c_str(), e.ts, e.gen);
  const bool fenced = b.fencedCount() == f0 + 1, kept = b.generation() == g0 + 1;
  assert(fenced != kept);
  return kept;
}
static std::string textOf(OLEDConsoleBuffer& b, uint32_t id) { RecordCopy c{}; assert(b.tryCopyRecord(id, c)); return c.text; }

int main() {
  // P7: nothing allocated: captures read 0, appends and clears are silent no-ops.
  {
    gAllowAlloc = false; OLEDConsoleBuffer u; u.init();
    assert(!u.isReady() && u.deliveryGeneration() == 0);
    u.append("x", 1, 0); u.clear();
    assert(u.deliveryGeneration() == 0 && u.fencedCount() == 0 && u.droppedCount() == 0);
    gAllowAlloc = true;
  }
  OLEDConsoleBuffer& b = gOledConsole;
  // Startup: envelopes queued before the ring exists carry generation 0 and
  // are accepted once it exists (no boundary has happened yet).
  Env early = enqueue(b, "queued before init");
  b.append(early.text.c_str(), early.ts, early.gen);  // ring absent: silent
  assert(b.droppedCount() == 0 && b.fencedCount() == 0);
  b.init();
  assert(b.isReady() && b.deliveryGeneration() == 0);
  assert(deliver(b, early));

  // P1: Codex's long-uptime case, and the full day walk, under the prototype.
  gNow = 100; b.clear();
  assert(b.deliveryGeneration() == 1);
  gNow = 100u + 0x80000001u;
  assert(deliver(b, enqueue(b, "new output 24.9 days later")));
  gNow = 1000; b.clear();
  for (int d = 0; d <= 100; ++d) { gNow = 1000u + (uint32_t)d * DAY + 1u; assert(deliver(b, enqueue(b, "daily"))); }
  printf("P1 long uptime: retained on every one of 101 days after a boundary\n");

  // P2: queued before the boundary, drained after it: refused, history stays empty.
  gNow = 4000; Env old1 = enqueue(b, "old 1"), old2 = enqueue(b, "old 2");
  gNow = 5000; b.clear();
  Status st{};
  assert(!deliver(b, old1) && !deliver(b, old2) && b.tryStatus(st) && st.count == 0);
  printf("P2 pre-boundary queued output refused after clear (fenced=%u)\n", b.fencedCount());

  // P3: many envelopes in one ownership period: each append moves the content
  // generation, none of them invalidates the others.
  std::vector<Env> burst;
  for (int i = 0; i < 60; ++i) burst.push_back(enqueue(b, ("burst " + std::to_string(i)).c_str()));
  const uint32_t contentBefore = b.generation();
  for (auto& e : burst) assert(deliver(b, e));
  assert(b.generation() == contentBefore + 60 && b.tryStatus(st) && st.count == 50);
  printf("P3 60 envelopes captured once, all retained while the content generation moved 60 times\n");

  // P4: equal tick is no longer a decision: same-millisecond output after the
  // boundary is retained; the stamp is kept as metadata.
  gNow = 6000; b.clear();
  Env same = enqueue(b, "same tick");
  assert(same.ts == 6000 && deliver(b, same));
  RecordCopy rc{}; assert(b.tryStatus(st) && b.tryCopyRecord(st.newestId, rc) && rc.timestampMs == 6000);
  printf("P4 equal tick retained; record keeps timestamp %u\n", rc.timestampMs);

  // P5: clock rollover is irrelevant to the decision.
  gNow = UINT32_MAX - 10; Env preWrapOld = enqueue(b, "queued before boundary, pre-wrap");
  b.clear();
  gNow = 5; assert(!deliver(b, preWrapOld) && deliver(b, enqueue(b, "after wrap")));
  printf("P5 rollover: pre-boundary refused, post-wrap retained\n");

  // P6: repeated boundaries: anything captured before the last one is refused.
  Env g0 = enqueue(b, "captured now");
  b.clear(); b.clear(); b.clear();
  assert(!deliver(b, g0) && deliver(b, enqueue(b, "fresh")));
  printf("P6 three clears: stale capture refused, fresh capture retained\n");

  // P9: the limit shared with the stamp fence: a command the old identity
  // started that emits AFTER the boundary captures the new generation.
  gNow = 7000; Env queuedOld = enqueue(b, "old: queued before boundary");
  b.clear();
  Env late = enqueue(b, "late: old command after boundary"), fresh = enqueue(b, "new identity");
  assert(!deliver(b, queuedOld) && deliver(b, late) && deliver(b, fresh));
  printf("P9 late output of an old command is NOT distinguished by the generation (needs the command fence)\n");

  // P8: concurrency. Producers capture then append (like enqueue -> drain with
  // the queue in between); a clearer advances the generation. Invariant after
  // the final clear: every retained record was captured at the final
  // generation, and the loss accounting balances.
  b.clear();
  std::atomic<uint32_t> attempts{0};
  auto producer = [&](int id) {
    char text[48];
    for (int i = 0; i < 40000; ++i) {
      const uint32_t gen = b.deliveryGeneration();
      std::this_thread::yield();
      snprintf(text, sizeof(text), "g%u p%d i%d", gen, id, i);
      b.append(text, gNow, gen);
      attempts.fetch_add(1);
    }
  };
  std::thread clearer([&] { for (int i = 0; i < 400; ++i) { std::this_thread::sleep_for(std::chrono::microseconds(50)); b.clear(); } });
  std::thread p1(producer, 1), p2(producer, 2);
  p1.join(); p2.join(); clearer.join();
  const uint32_t finalGen = b.deliveryGeneration();
  assert(b.tryStatus(st) && gHeld.load() == 0);
  for (uint32_t id = st.oldestId; st.count && id <= st.newestId; ++id) {
    const std::string t = textOf(b, id);
    assert(t.rfind("g" + std::to_string(finalGen) + " ", 0) == 0);  // nothing older survives the last clear
  }
  printf("P8 concurrency: %u attempts, %u refused, %u dropped, %u retained at the final generation %u (%u in ring)\n",
         attempts.load(), b.fencedCount(), b.droppedCount(), b.generation(), finalGen, st.count);
  puts("proto_probe: all assertions held (PROTOTYPE, not installed)");
}
