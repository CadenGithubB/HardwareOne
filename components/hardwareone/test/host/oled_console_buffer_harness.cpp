// Runs the OLED console history owner as it ships: OLED_ConsoleCore.h compiled
// directly, OLEDConsoleBuffer's methods spliced out of OLED_ConsoleBuffer.cpp
// over doubles for the mutex, allocator, clock and settings. Assembled by
// test_oled_console_buffer.py (ASan/UBSan).
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

// ---- platform doubles -------------------------------------------------------
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
static void xSemaphoreGive(SemaphoreHandle_t h) {
  gHeld.fetch_sub(1);
  static_cast<std::timed_mutex*>(h)->unlock();
}
enum class AllocPref { RequireInternal };
static bool gAllowAlloc = true;
static std::vector<void*> gAllocs;
static void* ps_calloc(size_t n, size_t size, AllocPref, const char*) {
  if (!gAllowAlloc) return nullptr;
  void* p = calloc(n, size);
  gAllocs.push_back(p);
  return p;
}
static void heap_caps_free(void* p) { free(p); }
static uint32_t gNow = 0;
static uint32_t millis() { return gNow; }
static struct { int oledCliHistorySize = 50; } gSettings;
static void noLog(const char*, ...) {}
#define ERROR_SYSTEMF(...) noLog(__VA_ARGS__)
#define DEBUG_SYSTEMF(...) noLog(__VA_ARGS__)

// INSERT_PRODUCTION_METHODS

OLEDConsoleBuffer gOledConsole;

using namespace oled_console;

static std::string textOf(OLEDConsoleBuffer& b, uint32_t id) {
  RecordCopy c{};
  bool busy = false;
  assert(b.tryCopyRecord(id, c, &busy) && !busy);
  return c.text;
}

static void checkSanitizer() {
  char out[64];
  bool t = false, m = false;
  assert(sanitize("\x1b[31mred\x1b[0m first\r\nsecond\t!", out, sizeof(out), &t, &m) == strlen(out));
  assert(std::string(out) == "red first\nsecond !" && !t && m);
  sanitize("caf\xc3\xa9 \xe2\x80\xa6 \xe2\x00", out, sizeof(out), &t, &m);
  assert(std::string(out) == "cafe ... " && !t && !m);   // the lead byte before NUL drops
  sanitize("abc\xe2\x82", out, sizeof(out), &t, &m);
  assert(std::string(out) == "abc" && !t);                 // truncated sequence drops
  sanitize("\x80\x80ok\n\n\n", out, sizeof(out), &t, &m);
  assert(std::string(out) == "ok" && !m);                  // stray continuations, trailing breaks
  sanitize("\x1b]0;title\x07x\x1b" "7y", out, sizeof(out), &t, &m);
  assert(std::string(out) == "xy");                        // OSC and two-byte escapes
  sanitize("a\x01\x7f" "b\x1b[", out, sizeof(out), &t, &m);
  assert(std::string(out) == "ab" && !t);                  // controls, DEL, unterminated CSI
  char small[5];
  sanitize("abcdefgh", small, sizeof(small), &t, &m);
  assert(std::string(small) == "abcd" && t);
  sanitize("abcd\x1b[0m", small, sizeof(small), &t, &m);
  assert(!t);                                              // a control-only tail is not lost text
  sanitize("abcd\xe2\x82\xac", small, sizeof(small), &t, &m);
  assert(t && std::string(small) == "abcd");               // a folded glyph that cannot fit is
  std::string ellipsis = "\xe2\x80\xa6";
  sanitize(("ab" + ellipsis).c_str(), small, sizeof(small), &t, &m);
  assert(std::string(small) == "ab" && t);                 // "..." needs three columns
  assert(sanitize("x", nullptr, 0, &t, &m) == 0 && t);
  assert(sanitize(nullptr, out, sizeof(out), &t, &m) == 0 && out[0] == '\0' && !t);
}

static void checkRingModel() {
  Record recs[10];
  char arena[640];
  memset(recs, 0, sizeof(recs));
  memset(arena, 0, sizeof(arena));
  Ring r;
  assert(r.attach(recs, 10, arena, sizeof(arena)));
  assert(r.status().count == 0 && !r.contains(0) && !r.contains(1));
  std::deque<std::pair<uint32_t, std::string>> model;
  unsigned seed = 7;
  auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return seed >> 8; };
  size_t used = 0, longest = 0;
  for (int i = 0; i < 6000; ++i) {
    const size_t len = rnd() % 3 == 0 ? rnd() % 256 : rnd() % 40;
    std::string s;
    for (size_t k = 0; k < len; ++k) s += char('a' + rnd() % 26);
    if (len && rnd() % 5 == 0) s[len / 2] = '\n';
    const uint32_t id = r.append(s.c_str(), (uint32_t)i);
    assert(id != kNoRecord);
    std::string stored = s;
    while (!stored.empty() && stored.back() == '\n') stored.pop_back();
    while (model.size() >= 10 || used + stored.size() > 640) {
      used -= model.front().second.size();
      model.pop_front();
    }
    model.emplace_back(id, stored);
    used += stored.size();
    if (stored.size() > longest) longest = stored.size();
    const Status st = r.status();
    assert(st.count == model.size() && st.capacity == 10);
    assert(st.oldestId == model.front().first && st.newestId == model.back().first);
    for (auto& e : model) {
      RecordCopy c{};
      assert(r.copyRecord(e.first, c) && c.id == e.first);
      assert(std::string(c.text) == e.second && c.length == e.second.size());
      assert(((c.flags & kFlagMultiline) != 0) == (e.second.find('\n') != std::string::npos));
      assert(c.timestampMs <= (uint32_t)i);
    }
    Preview pv[12];
    assert(r.copyWindow(st.oldestId, 12, 19, pv) == model.size());
    for (size_t k = 0; k < model.size(); ++k) {
      const std::string& text = model[k].second;
      const std::string first = text.substr(0, text.find('\n'));
      assert(pv[k].id == model[k].first);
      assert(std::string(pv[k].text) == first.substr(0, 19));
      assert(pv[k].cut == (first.size() > 19 || first.size() < text.size()));
    }
    assert(r.copyWindow(st.newestId, 3, 19, pv) == 1 && pv[0].id == st.newestId);
    assert(!r.contains(st.oldestId - 1) && !r.contains(st.newestId + 1));
    assert(r.copyWindow(st.newestId + 1, 3, 19, pv) == 0);
  }
  assert(longest == 255 && r.status().evicted > 5000);
  // Long records use the byte budget: 640 bytes hold two 255-byte records.
  r.clear();
  const std::string big(255, 'z');
  for (int i = 0; i < 5; ++i) r.append(big.c_str(), 0);
  assert(r.status().count == 2);
  // A record longer than an envelope is cut and flagged; the count is kept.
  const std::string huge(400, 'q');
  const uint32_t id = r.append(huge.c_str(), 9);
  RecordCopy c{};
  assert(r.copyRecord(id, c) && c.length == 255 && (c.flags & kFlagTruncated));
  assert(r.status().truncated == 1);
  // Clear keeps counting IDs so an old selection can never alias a new record.
  const uint32_t before = r.status().newestId;
  r.clear();
  assert(r.status().count == 0 && r.append("after", 1) == before + 1);
  // The ID counter never issues 0: at the wrap the history restarts at 1.
  Ring w;
  Record wr[4];
  char wa[256];
  memset(wr, 0, sizeof(wr));
  memset(wa, 0, sizeof(wa));
  assert(w.attach(wr, 4, wa, sizeof(wa), UINT32_MAX - 1));
  assert(w.append("a", 0) == UINT32_MAX - 1 && w.append("b", 0) == UINT32_MAX);
  assert(w.append("c", 0) == 1 && w.status().count == 1 && w.status().oldestId == 1);
  assert(!w.attach(nullptr, 4, wa, sizeof(wa)) && !w.attach(wr, 0, wa, sizeof(wa)));
  // Two records in one millisecond and timestamp zero are ordinary data.
  assert(w.append("d", 0) == 2 && w.append("e", 0) == 3 && w.append("f", UINT32_MAX) == 4);
  assert(w.copyRecord(3, c) && std::string(c.text) == "e" && c.timestampMs == 0);
  assert(w.copyRecord(4, c) && c.timestampMs == UINT32_MAX);
  assert(w.status().newestId == 4 && w.status().oldestId == 1);
  Ring unbound;
  assert(unbound.append("x", 0) == kNoRecord && unbound.status().count == 0 && !unbound.copyRecord(1, c));
}

static void checkOwner() {
  // Allocation failure leaves an owner that refuses everything quietly.
  {
    gAllowAlloc = false;
    OLEDConsoleBuffer b;
    b.init();
    assert(!b.isReady() && b.deliveryGeneration() == 0);
    b.append("x", 1, 0);
    b.clear();  // nothing to erase: the generation does not move either
    Status st{};
    assert(!b.tryStatus(st) && b.generation() == 0 && b.deliveryGeneration() == 0);
    assert(b.fencedCount() == 0 && b.droppedCount() == 0);
    gAllowAlloc = true;
  }
  {
    gAllowMutex = false;
    OLEDConsoleBuffer b;
    b.init();
    assert(!b.isReady());
    gAllowMutex = true;
  }
  // The depth is latched at init from the setting, clamped to 10..100.
  {
    gSettings.oledCliHistorySize = 3;
    OLEDConsoleBuffer b;
    b.init();
    Status st{};
    assert(b.isReady() && b.tryStatus(st) && st.capacity == 10);
    gSettings.oledCliHistorySize = 500;
    OLEDConsoleBuffer c;
    c.init();
    assert(c.tryStatus(st) && st.capacity == 100);
    gSettings.oledCliHistorySize = 50;
  }
  OLEDConsoleBuffer& b = gOledConsole;
  const uint32_t early = b.deliveryGeneration();  // what the queue captures before the ring exists
  b.append("before init", 1, early);  // no ring yet: dropped silently, not counted as a lock loss
  assert(early == 0 && b.droppedCount() == 0);
  b.init();
  Status st{};
  assert(b.isReady() && b.tryStatus(st) && st.capacity == 50 && st.count == 0);
  const uint32_t g0 = b.generation();
  b.append("alpha", 1000, early);  // queued before init, delivered after: retained
  b.append("beta", 1000, b.deliveryGeneration());   // same millisecond: distinct records
  b.append("zero", 0, b.deliveryGeneration());      // timestamp zero is data
  b.init();                 // a second init leaves the live ring alone
  assert(b.tryStatus(st) && st.count == 3 && st.oldestId == 1 && st.newestId == 3);
  assert(b.generation() == g0 + 3 && b.generation() == st.generation);
  assert(textOf(b, 1) == "alpha" && textOf(b, 2) == "beta" && textOf(b, 3) == "zero");
  Preview pv[4];
  bool busy = true;
  assert(b.tryCopyWindow(1, 4, 19, pv, &busy) == 3 && !busy && pv[2].id == 3);
  assert(b.tryCopyWindow(9, 4, 19, pv, &busy) == 0 && !busy);   // gone, not busy
  RecordCopy c{};
  assert(!b.tryCopyRecord(9, c, &busy) && !busy);

  // A held ring drops producer output (counted) and reports busy to screens.
  assert(xSemaphoreTake(&gMutex, 0) == pdTRUE);
  b.append("lost", 2000, b.deliveryGeneration());
  assert(b.droppedCount() == 1);
  assert(!b.tryStatus(st));
  assert(b.tryCopyWindow(1, 4, 19, pv, &busy) == 0 && busy);
  assert(!b.tryCopyRecord(1, c, &busy) && busy);
  xSemaphoreGive(&gMutex);
  assert(gHeld.load() == 0);
  assert(b.tryStatus(st) && st.count == 3 && b.generation() == g0 + 3);

  // The identity boundary erases history and advances the delivery
  // generation. An envelope carries the generation it was queued under: one
  // queued by the old identity is refused after the boundary however it is
  // stamped, while one queued after the boundary is retained in the boundary
  // tick, 24.9 days later and across the millisecond rollover alike.
  const uint32_t d0 = b.deliveryGeneration();
  gNow = 5000;
  const uint32_t g1 = b.generation();
  b.clear();
  assert(b.deliveryGeneration() == d0 + 1);
  assert(b.generation() == g1 + 1 && b.tryStatus(st) && st.count == 0);
  b.append("queued earlier", 4000, d0);
  b.append("queued at the boundary tick", 5000, d0);
  assert(b.fencedCount() == 2 && b.tryStatus(st) && st.count == 0);
  b.append("same tick, new identity", 5000, b.deliveryGeneration());
  assert(b.tryStatus(st) && st.count == 1 && st.oldestId == 4 && textOf(b, 4) == "same tick, new identity");
  RecordCopy meta{};
  assert(b.tryCopyRecord(4, meta) && meta.timestampMs == 5000);  // the stamp stays as metadata
  b.append("24.9 days later", 5000u + 0x80000001u, b.deliveryGeneration());
  b.append("after rollover", 5, b.deliveryGeneration());
  assert(b.tryStatus(st) && st.count == 3 && b.fencedCount() == 2);
  // Envelopes queued under one generation survive each other's content
  // changes; after further boundaries every older capture is refused.
  const uint32_t d1 = b.deliveryGeneration();
  for (int i = 0; i < 60; ++i) b.append("burst", 6000, d1);
  assert(b.fencedCount() == 2 && b.tryStatus(st) && st.count == 50);
  b.clear();
  b.clear();
  b.clear();
  assert(b.deliveryGeneration() == d1 + 3);
  b.append("stale capture", 7000, d1);
  b.append("stale capture", 7000, d1 + 2);
  assert(b.fencedCount() == 4 && b.tryStatus(st) && st.count == 0);
  b.append("fresh capture", 7000, b.deliveryGeneration());
  assert(b.tryStatus(st) && st.count == 1);

  // Producer and screen on real threads: every snapshot is coherent and the
  // lock is never observed held after a call returns.
  gNow = 100000;
  b.clear();  // "fresh capture" would not match the pattern below
  const uint32_t liveGen = b.deliveryGeneration();
  b.append("rec sentinel", gNow, liveGen);
  Status base{};
  assert(b.tryStatus(base) && base.count == 1);
  const uint32_t droppedBefore = b.droppedCount(), fencedBefore = b.fencedCount();
  std::atomic<bool> done{false};
  std::atomic<uint32_t> appended{0};
  std::thread producer([&] {
    char text[32];
    for (uint32_t i = 0; i < 20000; ++i) {
      snprintf(text, sizeof(text), "rec %lu", (unsigned long)i);
      b.append(text, gNow + 1 + i, liveGen);  // captured under the live generation
      appended.fetch_add(1);
    }
    done.store(true);
  });
  unsigned snapshots = 0;
  while (!done.load() || snapshots < 50) {
    Status s{};
    if (!b.tryStatus(s)) continue;
    assert(gHeld.load() >= 0);
    if (s.count == 0) continue;
    Preview win[8];
    bool wbusy = false;
    const size_t n = b.tryCopyWindow(s.oldestId, 8, 19, win, &wbusy);
    for (size_t k = 0; k < n; ++k) {
      assert(win[k].id == s.oldestId + k);
      assert(strncmp(win[k].text, "rec ", 4) == 0 && !win[k].cut);
    }
    RecordCopy rc{};
    bool rbusy = false;
    if (b.tryCopyRecord(s.newestId, rc, &rbusy)) {
      assert(rc.id == s.newestId && strncmp(rc.text, "rec ", 4) == 0 && rc.length == strlen(rc.text));
    }
    ++snapshots;
  }
  producer.join();
  assert(b.tryStatus(st) && st.count == 50 && gHeld.load() == 0);
  const uint32_t lost = (b.droppedCount() - droppedBefore) + (b.fencedCount() - fencedBefore);
  assert(b.fencedCount() == fencedBefore);
  assert(st.newestId - base.newestId == appended.load() - lost);  // one ID per stored envelope after the sentinel
  printf("console owner: %u concurrent snapshots, %lu drops under load\n", snapshots,
         (unsigned long)lost);

  // Producers capturing then appending (the queue sits between the two)
  // against boundaries landing at any moment: after the last boundary every
  // retained record was captured under the final generation, and every
  // attempt is accounted for as retained, refused or dropped.
  std::atomic<uint32_t> attempts{0};
  const uint32_t storedBefore = b.generation(), droppedBefore2 = b.droppedCount(), fencedBefore2 = b.fencedCount();
  auto captureThenAppend = [&](int who) {
    char text[48];
    for (uint32_t i = 0; i < 20000; ++i) {
      const uint32_t gen = b.deliveryGeneration();
      std::this_thread::yield();
      snprintf(text, sizeof(text), "g%lu p%d i%lu", (unsigned long)gen, who, (unsigned long)i);
      b.append(text, gNow, gen);
      attempts.fetch_add(1);
    }
  };
  std::thread clearer([&] {
    for (int i = 0; i < 200; ++i) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      b.clear();
    }
  });
  std::thread p1(captureThenAppend, 1), p2(captureThenAppend, 2);
  p1.join();
  p2.join();
  clearer.join();
  const uint32_t finalGen = b.deliveryGeneration();
  assert(b.tryStatus(st) && gHeld.load() == 0);
  const std::string tag = "g" + std::to_string(finalGen) + " ";
  for (uint32_t id = st.oldestId; st.count && id <= st.newestId; ++id) {
    assert(textOf(b, id).compare(0, tag.size(), tag) == 0);  // nothing older survives the last boundary
  }
  const uint32_t stored = b.generation() - storedBefore - 200;  // the content generation also moved once per clear
  assert(stored + (b.fencedCount() - fencedBefore2) + (b.droppedCount() - droppedBefore2) == attempts.load());
  printf("console owner: %lu attempts across 200 boundaries, %lu refused, %lu dropped, %u retained at generation %lu\n",
         (unsigned long)attempts.load(), (unsigned long)(b.fencedCount() - fencedBefore2),
         (unsigned long)(b.droppedCount() - droppedBefore2), (unsigned)st.count, (unsigned long)finalGen);
}

int main() {
  checkSanitizer();
  checkRingModel();
  checkOwner();
  for (void* p : gAllocs) free(p);
  puts("OLED console buffer: sanitizer bounds, contiguous IDs, byte budget, loss accounting, delivery generation and concurrency passed");
}
