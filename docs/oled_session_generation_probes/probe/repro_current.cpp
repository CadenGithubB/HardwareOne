// PROBE (current production code, unmodified): OledConsoleBuffer's methods are
// spliced from OLED_ConsoleBuffer.cpp exactly as test_oled_console_buffer.py does.
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
#include "OLED_ConsoleBuffer.h"
using SemaphoreHandle_t = void*;
static std::timed_mutex gMutex;
static SemaphoreHandle_t xSemaphoreCreateMutex() { return (void*)&gMutex; }
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE 1
static int xSemaphoreTake(SemaphoreHandle_t h, uint32_t ticks) {
  auto* m = static_cast<std::timed_mutex*>(h);
  if (ticks == portMAX_DELAY) { m->lock(); return 1; }
  return m->try_lock_for(std::chrono::milliseconds(ticks)) ? 1 : 0;
}
static void xSemaphoreGive(SemaphoreHandle_t h) { static_cast<std::timed_mutex*>(h)->unlock(); }
enum class AllocPref { RequireInternal };
static void* ps_calloc(size_t n, size_t size, AllocPref, const char*) { return calloc(n, size); }
static void heap_caps_free(void* p) { free(p); }
static uint32_t gNow = 0;
static uint32_t millis() { return gNow; }
static struct { int oledCliHistorySize = 50; } gSettings;
static void noLog(const char*, ...) {}
#define ERROR_SYSTEMF(...) noLog(__VA_ARGS__)
#define DEBUG_SYSTEMF(...) noLog(__VA_ARGS__)

// INSERT_PRODUCTION_METHODS

OledConsoleBuffer gOledConsole;
using namespace oled_console;

// Returns true when the append was retained, false when the fence refused it.
static bool tryAppend(OledConsoleBuffer& b, const char* text, uint32_t ts) {
  // The content generation moves only when a record is stored (a full ring
  // evicts, so the count alone cannot tell retention from refusal).
  const uint32_t f0 = b.fencedCount(); const uint32_t g0 = b.generation();
  b.append(text, ts);
  const bool fenced = b.fencedCount() == f0 + 1;
  const bool kept = b.generation() == g0 + 1;
  assert(fenced != kept);  // exactly one outcome
  return kept;
}
static const uint32_t DAY = 86400000u;

int main() {
  OledConsoleBuffer& b = gOledConsole;
  b.init();
  assert(b.isReady());

  // R6: before any boundary the fence is disarmed: every stamp is accepted.
  assert(tryAppend(b, "unfenced small", 1));
  assert(tryAppend(b, "unfenced huge", 0xFFFFFFFFu));
  assert(tryAppend(b, "unfenced zero", 0));

  // R1: Codex's reproduction, verbatim.
  gNow = 100; b.clear();
  const uint32_t fencedBefore = b.fencedCount();
  const bool r1 = tryAppend(b, "new output", 100u + 0x80000001u);
  printf("R1 clear@100, append@100+0x80000001: %s (fenced %u -> %u)\n",
         r1 ? "RETAINED" : "REFUSED", fencedBefore, b.fencedCount());
  assert(!r1);

  // R2: exact edges of the half range.
  assert( tryAppend(b, "edge accept", 100u + 0x7FFFFFFFu));  // +2^31-1 ms: accepted
  assert(!tryAppend(b, "edge refuse", 100u + 0x80000000u));  // +2^31 ms: refused
  assert(!tryAppend(b, "edge refuse2", 100u + 0xFFFFFFFFu)); // == fence-1 mod 2^32: refused
  assert( tryAppend(b, "wrap accept", 100u + 0u + 1u));      // fence+1: accepted (any time)
  printf("R2 edges: +2^31-1 retained, +2^31 refused, +2^32-1 refused\n");

  // R5: a quiet device, walked day by day for 100 days after one boundary.
  gNow = 1000; b.clear();
  std::string pattern;
  int firstRefusedDay = -1, firstReacceptedDay = -1;
  for (int d = 0; d <= 100; ++d) {
    const bool kept = tryAppend(b, "daily", 1000u + (uint32_t)d * DAY + 1u);
    pattern += kept ? '.' : 'X';
    if (!kept && firstRefusedDay < 0) firstRefusedDay = d;
    if (kept && firstRefusedDay >= 0 && firstReacceptedDay < 0) firstReacceptedDay = d;
  }
  printf("R5 day-by-day after one boundary (.=retained X=refused):\n   %s\n", pattern.c_str());
  printf("   refused from day %d, retained again from day %d (period 2^32 ms = %.2f days, half = %.2f days)\n",
         firstRefusedDay, firstReacceptedDay, 4294967296.0 / DAY, 2147483648.0 / DAY);
  assert(firstRefusedDay == 25 && firstReacceptedDay == 50);

  // R3: equal tick is refused; the next tick is retained.
  gNow = 5000; b.clear();
  assert(!tryAppend(b, "same tick", 5000));
  assert( tryAppend(b, "next tick", 5001));
  printf("R3 equal tick refused, next tick retained\n");

  // R4: ordinary millis() rollover within the half range is handled.
  gNow = UINT32_MAX - 10; b.clear();
  assert(!tryAppend(b, "before wrap, older", UINT32_MAX - 20));
  assert( tryAppend(b, "after wrap", 5));
  printf("R4 rollover: older-before-wrap refused, after-wrap retained\n");

  // R7: a FIFO model of the output queue. "old" was queued by the departing
  // identity before the boundary; "late" is emitted after the boundary by a
  // command that identity started. The stamp fence refuses the first and
  // cannot tell the second from the new identity's own output.
  struct Env { const char* text; uint32_t ts; };
  std::deque<Env> fifo;
  gNow = 4000; fifo.push_back({"old: queued before boundary", gNow});
  gNow = 5000; b.clear();
  gNow = 5001; fifo.push_back({"late: old command, after boundary", gNow});
  fifo.push_back({"new: next identity", gNow});
  std::vector<std::string> kept;
  while (!fifo.empty()) { Env e = fifo.front(); fifo.pop_front(); if (tryAppend(b, e.text, e.ts)) kept.push_back(e.text); }
  assert(kept.size() == 2 && kept[0].rfind("late", 0) == 0 && kept[1].rfind("new", 0) == 0);
  printf("R7 FIFO drain across a boundary: old refused; late(old command) and new both retained\n");

  puts("repro_current: all assertions held");
}
