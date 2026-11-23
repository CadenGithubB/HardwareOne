// The production cache and scheduler API, and the owner's trigger parser with
// the automation summaries built from it, are inserted by the runner.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

static std::atomic<unsigned> checks{0};
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); \
} } while (false)
void webBatchTestStringAppend() {}
#define ERROR_SYSTEMF(...) do {} while (false)
#define PSRAM_JSON_DOC(name) JsonDocument name

using TickType_t = uint32_t;
static constexpr TickType_t portMAX_DELAY = UINT32_MAX;
static constexpr int pdTRUE = 1;
static thread_local unsigned stateLockDepth = 0;
struct StaticSemaphore_t { std::mutex mutex; };
using SemaphoreHandle_t = StaticSemaphore_t*;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
static int xSemaphoreTake(SemaphoreHandle_t sem, TickType_t) {
  CHECK(sem && stateLockDepth == 0);
  sem->mutex.lock(); ++stateLockDepth; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t sem) {
  CHECK(sem && stateLockDepth == 1);
  --stateLockDepth; sem->mutex.unlock();
}
#define configASSERT(condition) CHECK(condition)

enum class AllocPref { RequireInternal, PreferPsram };
static thread_local bool failAllocation = false;
static std::atomic<unsigned> allocations{0}, releases{0};
static void* ps_calloc(size_t count, size_t size, AllocPref, const char*) {
  if (failAllocation) { failAllocation = false; return nullptr; }
  void* result = std::calloc(count, size);
  if (result) ++allocations;
  return result;
}
static void ps_free(void* memory) { if (memory) ++releases; std::free(memory); }
static std::atomic<uint32_t> clockMs{0};
static std::atomic<time_t> clockSeconds{100};
static uint32_t millis() { return clockMs.load(); }
static time_t testTime(time_t*) { return clockSeconds.load(); }
#define time testTime
static bool everyMs(uint32_t* previous, uint32_t interval) {
  const uint32_t now = millis();
  if (now - *previous < interval) return false;
  *previous = now; return true;
}
static constexpr int SYSEVT_COUNT = 8;
static int systemEventKindFromName(const char* name) {
  return std::strcmp(name, "edge") == 0 ? 1 : std::strcmp(name, "other") == 0 ? 2 : 0;
}
static const char* systemEventKindName(uint8_t kind) {
  return kind == 1 ? "edge" : kind == 2 ? "other" : "?";
}
static constexpr size_t SYSEVT_DETAIL_LEN = 80;
static const char* AUTOMATIONS_JSON_FILE = "/automations.json";
#define DEBUG_AUTOMATIONSF(...) do {} while (false)

// The event ring boundary: the record, ring and lock are stubs; the real
// systemEventLatestSeq() and systemEventFetchSince() are inserted below.
#define SYSEVT_RING_SIZE 48
struct SystemEvent { uint32_t seq; uint32_t tsMs; uint8_t kind; };
static SystemEvent gEventRing[SYSEVT_RING_SIZE];
static uint32_t gEventNextSeq = 1;
static std::mutex gEventMux;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define taskENTER_CRITICAL(mux) (mux)->lock()
#define taskEXIT_CRITICAL(mux) (mux)->unlock()
static uint32_t gEventRingSkipped = 0;
// INSERT_EVENT_RING
// Mirrors systemEventPost()'s ring write; the subscription nudge stays the
// test's explicit automationOnSystemEvent() call.
static uint32_t postEvent(uint8_t kind) {
  std::lock_guard<std::mutex> lock(gEventMux);
  SystemEvent ev{gEventNextSeq++, millis(), kind};
  gEventRing[ev.seq % SYSEVT_RING_SIZE] = ev;
  return ev.seq;
}
// Preallocated so the drain's lazy ps_calloc does not perturb the cache
// allocation ledger or consume the tests' injected allocation failure.
static SystemEvent* sEventBuf =
    static_cast<SystemEvent*>(std::calloc(SYSEVT_RING_SIZE, sizeof(SystemEvent)));
static std::vector<uint32_t> drainedSeqs;  // seqs the last drain matched against
static std::mutex storeMutex;
static String storeJson;
static bool storeExists = false, readSucceeds = true, writeSucceeds = true;
static std::atomic<unsigned> reads{0}, writes{0}, ticks{0};
static std::function<void()> afterRead, beforeTick;
static void io() { CHECK(stateLockDepth == 0); }
namespace VFS {
static int systemAuth(const char*) { return 0; }
static int systemAuth(const char*, const char*) { return 0; }
namespace Scopes { constexpr const char* SYSTEM_STATE = "/system"; }
static bool existsGuarded(const char*, int) {
  io(); std::lock_guard<std::mutex> lock(storeMutex); return storeExists;
}
}
static bool readText(const char*, String& out, int) {
  io(); ++reads;
  {
    std::lock_guard<std::mutex> lock(storeMutex);
    if (!storeExists || !readSucceeds) return false;
    out = storeJson;
  }
  if (afterRead) afterRead();
  return true;
}
static bool writeTextAtomic(const char*, const String& json, int) {
  io(); ++writes; std::lock_guard<std::mutex> lock(storeMutex);
  if (!writeSucceeds) return false;
  storeJson = json; storeExists = true; return true;
}
static void replaceStore(const String& json) {
  std::lock_guard<std::mutex> lock(storeMutex); storeJson = json; storeExists = true;
}
void notifyAutomationScheduler();
bool writeAutomationsJsonAtomic(const String& json);  // System_Automation.h
bool startAutomationScheduler();
void stopAutomationScheduler();
bool automationSchedulerRunning();
void automationTick();
static void schedulerTickMinute();

// Clock::isValidEpoch from System_Clock.cpp, the owner of "is the clock set",
// goes in as ClockOwner. The cases written before the uptime schedule count
// small numbers as a set clock (clockForcedSet, the default after reset()); the
// clock and uptime cases clear it and ask the owner itself.
// INSERT_CLOCK_OWNER
static std::atomic<bool> clockForcedSet{true};
namespace Clock {
static bool isValidEpoch(time_t t) { return clockForcedSet.load() || ClockOwner::isValidEpoch(t); }
}  // namespace Clock
// SelfDevice::uptimeSeconds, settable; it does not follow millis(), which wraps.
static std::atomic<uint32_t> uptimeSec{0};
namespace SelfDevice {
static uint32_t uptimeSeconds() { return uptimeSec.load(); }
}  // namespace SelfDevice
#define EXT_RAM_BSS_ATTR

// INSERT_CACHE_OWNER

// Only full trigger evaluation/command dispatch is substituted. The production
// poll chooses when to call it; every call performs the real JSON cache rebuild.
static void schedulerTickMinute() {
  CHECK(stateLockDepth == 0);
  ++ticks;
  // The real full tick's event drain (pending flag, cursor, fetch, 60 s
  // discard) is inserted here.
  // INSERT_PENDING_EVENT_DRAIN
  drainedSeqs.clear();
  for (int i = 0; i < evCount; ++i) drainedSeqs.push_back(sEventBuf[i].seq);
  if (beforeTick) beforeTick();
  rebuildAutomationsCache();
}

// INSERT_SCHEDULER_API

// INSERT_TRIGGER_SUMMARY

// INSERT_RESCHEDULE

static const char* emptyRules = R"({"version":2,"automations":[]})";
static const char* futureRules = R"({"version":2,"automations":[{"id":1,"enabled":true,"triggers":[{"type":"interval","nextAt":101}]}]})";
static const char* eventRules = R"({"version":2,"automations":[{"id":1,"enabled":true,"triggers":[{"type":"event","on":"edge","nextAt":1}]}]})";

static void reset(const char* json = emptyRules) {
  afterRead = beforeTick = {};
  stopAutomationScheduler();
  CHECK(allocations == releases);
  clockMs += 100000;
  clockSeconds = 100;
  clockForcedSet = true;
#ifdef HAVE_UPTIME_SCHEDULE
  sUptimeSchedule.clear();
#endif
  storeExists = json != nullptr;
  storeJson = json ? json : "";
  readSucceeds = writeSucceeds = true;
  failAllocation = false;
  reads = writes = ticks = 0;
}

// G-73: setup starts the scheduler, then posts more events (battery_low from
// the boot battery sample) before loop() runs the first drain. The first drain
// sees those, but not events from before the start. Runs first: the drain's
// cursor is a function static and only its first drain starts from the start.
static void eventDrainFromStart() {
  reset(eventRules);
  postEvent(1);  // before the scheduler starts: not matched
  CHECK(startAutomationScheduler());
  clockMs += 5000;
  const uint32_t bootLow = postEvent(1);
  automationOnSystemEvent(1);
  clockMs += 5000;
  automationTick();
  CHECK(ticks == 1 && drainedSeqs.size() == 1 && drainedSeqs[0] == bootLow);
  clockMs += 250;
  const uint32_t later = postEvent(1);
  automationOnSystemEvent(1);
  automationTick();
  CHECK(ticks == 2 && drainedSeqs.size() == 1 && drainedSeqs[0] == later);
  // A restart after the first drain keeps the cursor: nothing is replayed.
  stopAutomationScheduler();
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(ticks == 3 && drainedSeqs.empty());
}

static void lifecycle() {
  reset(nullptr);
  automationTick();
  CHECK(!automationSchedulerRunning() && ticks == 0 && reads == 0 && writes == 0);
  writeSucceeds = false;
  CHECK(!startAutomationScheduler());
  CHECK(!automationSchedulerRunning() && writes == 1);
  writeSucceeds = true;
  failAllocation = true;
  CHECK(!startAutomationScheduler());
  CHECK(!automationSchedulerRunning() && storeExists);
  JsonDocument doc;
  CHECK(!deserializeJson(doc, storeJson));
  CHECK(doc["version"] == 2 && doc["automations"].as<JsonArrayConst>().size() == 0);
  CHECK(startAutomationScheduler());
  CHECK(automationSchedulerRunning());
  const unsigned before = writes;
  CHECK(startAutomationScheduler());
  CHECK(writes == before);
  automationTick();
  CHECK(ticks == 1);
  for (int i = 0; i < 20; ++i) automationTick();
  CHECK(ticks == 1 && reads == 1);
  stopAutomationScheduler();
  CHECK(!automationSchedulerRunning() && allocations == releases);
  notifyAutomationScheduler();
  automationOnSystemEvent(1);
  automationTick();
  CHECK(ticks == 1);

  for (bool malformed : {false, true}) {
    reset(malformed ? "{broken" : emptyRules);
    readSucceeds = malformed;
    CHECK(startAutomationScheduler());
    automationTick();
    CHECK(ticks == 1 && writes == 0);
    for (int i = 0; i < 20; ++i) automationTick();
    CHECK(ticks == 1 && reads == 1);
  }
}

// No saved setting stops a running scheduler (the automations "allowed"
// switch was removed 2026-11-21; automationautostart only decides boot). Only
// the owner's stop (`automation system disable`) does, and a start resumes it.
static void runsUntilStopped() {
  reset(futureRules);
  CHECK(startAutomationScheduler() && automationSchedulerRunning());
  automationTick();
  CHECK(ticks == 1);
  CHECK(automationSetSystemEnabled(false) == nullptr);
  automationTick();
  CHECK(!automationSchedulerRunning() && ticks == 1 && allocations == releases);
  automationTick();
  CHECK(!automationSchedulerRunning() && ticks == 1);
  CHECK(automationSetSystemEnabled(true) == nullptr && automationSchedulerRunning());
  automationTick();
  CHECK(ticks == 2);
}

static void timing() {
  reset(R"({"automations":[
    {"id":1,"enabled":false,"triggers":[{"type":"interval","nextAt":1},{"type":"event","on":"edge"}]},
    {"id":2,"enabled":true,"triggers":[{"type":"interval","nextAt":200},{"type":"interval","nextAt":101},{"type":"event","on":"other","nextAt":1}]}
  ]})");
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(ticks == 1);
  clockMs += 250;
  automationOnSystemEvent(1); // Disabled entries do not subscribe.
  automationTick();
  CHECK(ticks == 1); // Disabled due times and event nextAt must not clock-fire.
  clockSeconds = 101;
  automationTick();
  CHECK(ticks == 2); // Earliest scheduled trigger wins, independent of order.

  reset(futureRules);
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(ticks == 1);
  automationTick();
  CHECK(ticks == 1);
  clockSeconds = 101;
  automationTick();
  CHECK(ticks == 2);
  automationNoteCondBlocked(1);
  automationTick();
  CHECK(ticks == 2);
  clockMs += 4999;
  automationTick();
  CHECK(ticks == 2);
  ++clockMs;
  automationTick();
  CHECK(ticks == 3);
  automationNoteCondBlocked(1);
  notifyAutomationScheduler();
  automationTick();
  CHECK(ticks == 4);

  reset(eventRules);
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(ticks == 1);
  clockMs += 249;
  automationOnSystemEvent(2); // Valid, unsubscribed kind must not wake.
  automationOnSystemEvent(SYSEVT_COUNT);
  automationTick();
  CHECK(ticks == 1);
  automationOnSystemEvent(1);
  automationTick();
  CHECK(ticks == 1);
  ++clockMs;
  automationTick();
  CHECK(ticks == 2);
  clockMs += 59999;
  automationTick();
  CHECK(ticks == 2);
  ++clockMs;
  automationTick();
  CHECK(ticks == 3);

  reset(eventRules);
  clockMs = UINT32_MAX - 100;
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(ticks == 1);
  automationOnSystemEvent(1);
  clockMs = 148; // 249 ms elapsed, across wrap.
  automationTick();
  CHECK(ticks == 1);
  clockMs = 149;
  automationTick();
  CHECK(ticks == 2);
  clockMs += 60000;
  automationTick();
  CHECK(ticks == 3);
}

static void invalidationAndOverlap() {
  reset();
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(ticks == 1);
  writeSucceeds = false;
  CHECK(!writeAutomationsJsonAtomic(futureRules));
  automationTick();
  CHECK(ticks == 1);
  writeSucceeds = true;
  CHECK(writeAutomationsJsonAtomic(futureRules));
  automationTick();
  CHECK(ticks == 2);
  replaceStore(emptyRules); // Successful external store save uses explicit notice.
  notifyAutomationScheduler();
  automationTick();
  CHECK(ticks == 3);

  bool notified = false;
  afterRead = [&] {
    if (!notified) {
      notified = true;
      replaceStore(futureRules);
      notifyAutomationScheduler();
    }
  };
  notifyAutomationScheduler();
  automationTick();
  CHECK(ticks == 4 && notified);
  afterRead = {};
  automationTick();
  CHECK(ticks == 5); // Notification during rebuild survives this older snapshot.
  automationTick();
  CHECK(ticks == 5);

  replaceStore(eventRules);
  afterRead = [] { automationOnSystemEvent(1); };
  notifyAutomationScheduler();
  automationTick();
  CHECK(ticks == 6);
  afterRead = {};
  automationTick();
  CHECK(ticks == 6);
  clockMs += 250;
  automationTick();
  CHECK(ticks == 7); // An event posted after the drain survives publication.
  automationTick();
  CHECK(ticks == 7);

  std::promise<void> captured, resume;
  auto resumeFuture = resume.get_future();
  afterRead = [&] { captured.set_value(); resumeFuture.wait(); };
  notifyAutomationScheduler();
  auto reader = std::async(std::launch::async, [] { automationTick(); });
  CHECK(captured.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  stopAutomationScheduler();
  CHECK(!automationSchedulerRunning() && allocations == releases);
  resume.set_value();
  reader.get();
  afterRead = {};
  CHECK(!automationSchedulerRunning() && allocations == releases);
  const unsigned prior = ticks;
  automationTick();
  CHECK(ticks == prior);
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(ticks == prior + 1);

  // A pass claimed before stop may finish after a restart. Its older snapshot
  // must not consume the restart's refresh request or hide the current file.
  reset();
  CHECK(startAutomationScheduler());
  std::promise<void> oldCaptured, finishOld;
  auto finishOldFuture = finishOld.get_future();
  afterRead = [&] { oldCaptured.set_value(); finishOldFuture.wait(); };
  auto oldReader = std::async(std::launch::async, [] { automationTick(); });
  CHECK(oldCaptured.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  stopAutomationScheduler();
  replaceStore(futureRules);
  CHECK(startAutomationScheduler());
  finishOld.set_value();
  oldReader.get();
  afterRead = {};
  CHECK(automationSchedulerRunning() && ticks == 1);
  automationTick();
  CHECK(ticks == 2);
  automationTick();
  CHECK(ticks == 2);
  clockSeconds = 101;
  automationTick();
  CHECK(ticks == 3);
}

static void concurrentRequests() {
  reset(eventRules);
  CHECK(startAutomationScheduler());
  automationTick();
  std::atomic<bool> begin{false};
  std::vector<std::thread> workers;
  workers.emplace_back([&] {
    while (!begin.load()) std::this_thread::yield();
    for (unsigned i = 0; i < 2000; ++i) {
      clockMs += 251;
      automationTick();
    }
  });
  workers.emplace_back([&] {
    while (!begin.load()) std::this_thread::yield();
    for (unsigned i = 0; i < 2000; ++i) { automationOnSystemEvent(1); notifyAutomationScheduler(); }
  });
  for (int writer = 0; writer < 2; ++writer) workers.emplace_back([&] {
    while (!begin.load()) std::this_thread::yield();
    for (unsigned i = 0; i < 300; ++i) {
      stopAutomationScheduler();
      CHECK(startAutomationScheduler());
      automationSchedulerRunning();
    }
  });
  begin = true;
  for (auto& worker : workers) worker.join();
  stopAutomationScheduler();
  CHECK(allocations == releases);
}

// The OLED and G2 pages show these copies; the text comes from the parser the
// scheduler fires from, one case per trigger type.
static void summaries() {
  reset(R"({"automations":[
    {"id":1,"name":"Daily","enabled":true,"commands":["a","b"],"triggers":[{"type":"time","time":"07:30"}]},
    {"id":2,"name":"All days","triggers":[{"type":"time","time":"07:30","days":"sun,mon,tue,wed,thu,fri,sat"}]},
    {"id":3,"name":"Mon Wed","triggers":[{"type":"time","time":"08:00","days":"mon, WED"}]},
    {"id":4,"name":"Weekend","triggers":[{"type":"time","time":"09:05","days":"sat,sun"}]},
    {"id":5,"name":"Biweekly","triggers":[{"type":"time","time":"07:30","days":"fri","weekInterval":2}]},
    {"id":6,"name":"Monthly","triggers":[{"type":"time","time":"07:30","recurrence":"monthly","dayOfMonth":15}]},
    {"id":7,"name":"Yearly","triggers":[{"type":"time","time":"06:05","recurrence":"Yearly","dayOfMonth":9,"month":3}]},
    {"id":8,"name":"No day","triggers":[{"type":"time","time":"07:30","recurrence":"monthly"}]},
    {"id":9,"triggers":[{"type":"interval","intervalMs":7200000}]},
    {"id":10,"triggers":[{"type":"interval","intervalMs":5400000}]},
    {"id":11,"triggers":[{"type":"interval","intervalMs":45000}]},
    {"id":12,"triggers":[{"type":"manual","delayMs":120000}]},
    {"id":13,"triggers":[{"type":"manual","delayMs":10000}]},
    {"id":14,"triggers":[{"type":"boot"}]},
    {"id":15,"triggers":[{"type":"boot","bootDelayMs":5000}]},
    {"id":16,"triggers":[{"type":"event","on":"edge"}]},
    {"id":17,"triggers":[{"type":"event","on":"bogus"}]},
    {"id":18,"triggers":[{"type":"atTime","time":"07:30"}]},
    {"id":19,"triggers":[]},
    {"id":20,"name":"A name longer than thirty-one characters","enabled":true,
     "triggers":[{"type":"atTime","time":"07:30"},{"type":"interval","intervalMs":60000},{"type":"event","on":"edge"}]}
  ]})");
  AutomationSummary list[24];
  std::memset(list, 0, sizeof(list));
  size_t count = 99;
  CHECK(automationCopySummaries(list, 24, &count));
  CHECK(count == 20 && reads == 1);
  struct Expect { long id; AutomationTriggerKind kind; const char* text; };
  using K = AutomationTriggerKind;
  static const Expect expected[] = {
    {1, K::Time, "daily 07:30"}, {2, K::Time, "daily 07:30"},
    {3, K::Time, "weekly 08:00 MoWe"}, {4, K::Time, "weekly 09:05 SaSu"},
    {5, K::Time, "every 2w 07:30 Fr"}, {6, K::Monthly, "monthly 15 07:30"},
    {7, K::Yearly, "yearly Mar 9 06:05"}, {8, K::Invalid, "invalid trigger"},
    {9, K::Interval, "every 2h"}, {10, K::Interval, "every 90m"},
    {11, K::Interval, "every 45s"}, {12, K::Manual, "delay 2m"},
    {13, K::Manual, "delay 10s"}, {14, K::Boot, "boot"}, {15, K::Boot, "boot +5s"},
    {16, K::Event, "on edge"}, {17, K::Event, "on unknown event"},
    {18, K::Invalid, "invalid trigger"}, {19, K::None, ""},
    {20, K::Interval, "every 1m +2"},
  };
  unsigned summaryMismatches = 0;
  for (size_t i = 0; i < 20; ++i) {
    const AutomationSummary& s = list[i];
    if (s.id != expected[i].id || s.kind != expected[i].kind ||
        std::strcmp(s.trigger, expected[i].text) != 0) {
      ++summaryMismatches;
      std::fprintf(stderr, "summary %zu: id=%ld kind=%d text=\"%s\" (want id=%ld kind=%d \"%s\")\n",
                   i, s.id, (int)s.kind, s.trigger, expected[i].id, (int)expected[i].kind,
                   expected[i].text);
    }
  }
  CHECK(summaryMismatches == 0);
  CHECK(std::strcmp(list[0].name, "Daily") == 0 && list[0].enabled);
  CHECK(list[0].commandCount == 2 && list[0].triggerCount == 1);
  CHECK(!list[1].enabled && list[1].commandCount == 0);
  CHECK(std::strcmp(list[8].name, "Auto #9") == 0);
  CHECK(list[18].triggerCount == 0 && list[19].triggerCount == 3 && list[19].enabled);
  CHECK(std::strcmp(list[19].name, "A name longer than thirty-one c") == 0);

  // The cap bounds the copy; entries past it are not written.
  std::memset(list, 0, sizeof(list));
  list[3].id = -5;
  CHECK(automationCopySummaries(list, 3, &count) && count == 3);
  CHECK(list[2].id == 3 && list[3].id == -5);

  AutomationSummary one{};
  CHECK(automationFindSummary(6, &one) && one.kind == K::Monthly);
  CHECK(std::strcmp(one.name, "Monthly") == 0 && std::strcmp(one.trigger, "monthly 15 07:30") == 0);
  CHECK(!automationFindSummary(99, &one));
  CHECK(!automationFindSummary(6, nullptr));

  readSucceeds = false;
  count = 7;
  CHECK(!automationCopySummaries(list, 24, &count) && count == 0);
  CHECK(!automationFindSummary(6, &one));
  readSucceeds = true;
  replaceStore("{broken");
  CHECK(!automationCopySummaries(list, 24, &count) && count == 0);
  replaceStore(R"({"version":2})");
  CHECK(automationCopySummaries(list, 24, &count) && count == 0);
  CHECK(!automationFindSummary(1, &one));
}

// automations.json format version. A newer file is refused: nothing in it is
// cached or listed, the store is never written (not by the empty-store seed,
// not by a writer handed a newer document) and its text stays as it was. No
// "version" (older files), version 1 and a non-numeric version load as today.
static void fileFormatVersion() {
  static const char* newerRules =
      R"({"version":3,"automations":[{"id":1,"enabled":true,"triggers":[{"type":"interval","nextAt":50}]}]})";
  reset(newerRules);
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(ticks == 1 && gAutomationsCacheValid && gAutomationsCacheCount == 0);
  CHECK(sAutomationsRefusedVersion.load() == 3);
  for (int i = 0; i < 20; ++i) automationTick();  // a loaded due rule would tick every pass
  CHECK(ticks == 1);
  String json = "untouched";
  CHECK(!readAutomationsJson(json) && json.length() == 0);
  AutomationSummary list[4]{};
  size_t count = 7;
  CHECK(!automationCopySummaries(list, 4, &count) && count == 0);
  CHECK(!writeAutomationsJsonAtomic(R"({"version":3,"automations":[]})"));
  CHECK(!writeAutomationsJsonAtomic(R"({"automations":[],"version":4.5})"));
  CHECK(ensureAutomationsFile());
  // The add command's load: a refused file is never replaced by the seed.
  json = "untouched";
  CHECK(!loadAutomationsStoreForEdit(json, true) && json.length() == 0);
  CHECK(!loadAutomationsStoreForEdit(json, false) && json.length() == 0);
  uint32_t newer = 0;
  CHECK(!readAutomationsJson(json, &newer) && newer == 3);
  CHECK(writes == 0 && storeJson == newerRules);
  for (const char* future : {R"({"version":2.5,"automations":[]})",
                             R"({"version":1e12,"automations":[]})"}) {
    replaceStore(future);
    CHECK(!readAutomationsJson(json) && sAutomationsRefusedVersion.load() > kAutomationsFileVersion);
  }

  static const char* const loads[] = {
      R"({"automations":[{"id":1,"enabled":true,"triggers":[{"type":"interval","nextAt":101}]}]})",
      R"({"version":1,"automations":[{"id":1,"enabled":true,"triggers":[{"type":"interval","nextAt":101}]}]})",
      R"({"version":"3","automations":[{"id":1,"enabled":true,"triggers":[{"type":"interval","nextAt":101}]}]})",
      futureRules,
  };
  for (const char* rules : loads) {
    replaceStore(rules);
    notifyAutomationScheduler();
    const unsigned before = ticks;
    automationTick();
    CHECK(ticks == before + 1 && gAutomationsCacheCount == 1 && gAutomationsCache[0].nextAt == 101);
    CHECK(sAutomationsRefusedVersion.load() == 0);
    CHECK(readAutomationsJson(json) && json == rules);
    CHECK(automationCopySummaries(list, 4, &count) && count == 1);
  }
  CHECK(writeAutomationsJsonAtomic(emptyRules) && storeJson == emptyRules);
  uint32_t newer2 = 7;
  CHECK(readAutomationsJson(json, &newer2) && newer2 == 0 && json == emptyRules);

  // No file: the add command's load seeds the empty store, and writes it
  // only when asked (validate-only adds write nothing). A current file is
  // loaded as it is.
  reset(nullptr);
  CHECK(loadAutomationsStoreForEdit(json, false) && json == automationsEmptyStoreJson());
  CHECK(writes == 0 && !storeExists);
  CHECK(!readAutomationsJson(json, &newer2) && newer2 == 0);
  CHECK(loadAutomationsStoreForEdit(json, true) && json == automationsEmptyStoreJson());
  CHECK(writes == 1 && storeJson == automationsEmptyStoreJson());
  reset(futureRules);
  CHECK(loadAutomationsStoreForEdit(json, true) && json == futureRules && writes == 0);
  // A file that is there but does not read is not an absent one: the load
  // stops, and the empty store is neither returned nor written over it.
  readSucceeds = false;
  json = "untouched";
  CHECK(!loadAutomationsStoreForEdit(json, true) && json.length() == 0);
  CHECK(!loadAutomationsStoreForEdit(json, false) && json.length() == 0);
  CHECK(writes == 0 && storeJson == futureRules);
  readSucceeds = true;
  CHECK(loadAutomationsStoreForEdit(json, true) && json == futureRules && writes == 0);
  CHECK(automationsEmptyStoreJson() == "{\n  \"version\": 2,\n  \"automations\": []\n}\n");
  stopAutomationScheduler();
}

// G-70: a short interval keeps its next run in RAM, so a fire writes nothing.
static JsonDocument storeDoc() {
  JsonDocument doc;
  std::lock_guard<std::mutex> lock(storeMutex);
  CHECK(!deserializeJson(doc, storeJson));
  return doc;
}

static void shortIntervals() {
  using Table = AutomationVolatileSchedule;
  {
    Table t;
    CHECK(Table::covers(1) && Table::covers(5000) && Table::covers(899999));
    CHECK(!Table::covers(0) && !Table::covers(900000));
    CHECK(!t.set(7, 0, 900000, 500) && !t.set(0, 0, 5000, 500) && !t.set(7, 0, 5000, 0));
    // No entry: a future or slightly late file value is used as it is.
    CHECK(t.effective(7, 0, 5000, 1005, 1000, true) == 1005);
    CHECK(t.effective(7, 0, 5000, 940, 1000, true) == 940);
    CHECK(t.effective(7, 0, 5000, 0, 1000, true) == 0);
    CHECK(t.effective(7, 0, 5000, 3, 1000, false) == 3);  // no clock: no restart
    CHECK(t.effective(7, 0, 900000, 3, 5000, true) == 3);  // long interval: the file rules
    CHECK(t.find(7, 0) == nullptr);
    // A stale file value restarts the interval from now, once.
    CHECK(t.effective(7, 0, 5000, 939, 1000, true) == 1005);
    CHECK(t.effective(7, 0, 5000, 939, 1200, true) == 1005);  // a deferred run stays due
    // Ten-minute interval: stale only after two intervals.
    CHECK(t.effective(8, 0, 600000, 1000, 2200, true) == 1000);
    CHECK(t.effective(8, 0, 600000, 1000, 2201, true) == 2801);
    // After a fire the entry advances; the file value no longer matters.
    CHECK(t.set(7, 0, 5000, 1210) && t.effective(7, 0, 5000, 939, 1206, true) == 1210);
    CHECK(t.peek(7, 0, 5000, 939) == 1210 && t.peek(7, 1, 5000, 939) == 939);
    // Positions in one automation are separate entries.
    CHECK(t.set(7, 1, 5000, 1300) && t.effective(7, 0, 5000, 0, 1206, true) == 1210 &&
          t.effective(7, 1, 5000, 0, 1206, true) == 1300);
    // A later file value was written on purpose and wins.
    CHECK(t.effective(7, 0, 5000, 1400, 1206, true) == 1400 && t.peek(7, 0, 5000, 0) == 1400);
    // An edited interval drops the entry.
    CHECK(t.peek(7, 0, 10000, 55) == 55);
    CHECK(t.effective(7, 0, 10000, 1400, 1401, true) == 1400 && t.find(7, 0) == nullptr);
    // Sweep keeps what was seen and what was set meanwhile.
    t.beginSweep();
    CHECK(t.effective(7, 1, 5000, 0, 1206, true) == 1300);
    CHECK(t.set(9, 0, 5000, 1500));
    t.endSweep();
    CHECK(t.find(7, 1) && t.find(9, 0) && !t.find(8, 0));
  }
  {
    Table t;
    for (int i = 0; i < Table::kEntries; i++) CHECK(t.set(100 + i, 2, 5000, 2000));
    CHECK(!t.set(999, 0, 5000, 2000));                         // full: the caller saves
    CHECK(t.effective(999, 0, 5000, 100, 5000, true) == 100);  // and the file value stands
    CHECK(t.set(100, 2, 5000, 2005) && t.find(100, 2)->nextAt == 2005);
  }

  // Through the scheduler's own code.
  reset();
  clockSeconds = 5000;
  const char* quick = R"({"id":41,"enabled":true,"triggers":[{"type":"interval","intervalMs":5000,"nextAt":4999}]})";
  const char* slow = R"({"id":42,"enabled":true,"triggers":[{"type":"interval","intervalMs":900000,"nextAt":4999}]})";
  const char* mixed = R"({"id":43,"enabled":true,"triggers":[{"type":"event","on":"edge"},{"type":"interval","intervalMs":5000,"nextAt":4990},{"type":"interval","intervalMs":900000,"nextAt":9000}]})";
  const char* off = R"({"id":44,"enabled":false,"triggers":[{"type":"interval","intervalMs":5000,"nextAt":100}]})";
  replaceStore(String(R"({"version":2,"automations":[)") + quick + "," + slow + "," + mixed + "," + off + "]}");
  Trigger parsed[MAX_TRIGGERS];
  CHECK(triggersFromJson(quick, parsed, MAX_TRIGGERS) == 1 && parsed[0].nextAt == 4999 && parsed[0].slot == 0);

  // A fire of the short interval writes nothing and still moves the schedule.
  unsigned before = writes;
  rescheduleAfterFire(41, quick, 5000, false);
  CHECK(writes == before);
  CHECK(triggersFromJson(quick, parsed, MAX_TRIGGERS) == 1 && parsed[0].nextAt == 5005);
  CHECK(computeNextRunTime(quick, 5000) == 5005);
  rescheduleAfterFire(41, quick, 5005, true);
  CHECK(writes == before && computeNextRunTime(quick, 5005) == 5010);

  // A 15-minute interval is saved as before.
  rescheduleAfterFire(42, slow, 5000, false);
  CHECK(writes == before + 1);
  {
    JsonDocument doc = storeDoc();
    CHECK(doc["automations"][0]["triggers"][0]["nextAt"].as<long>() == 4999);  // file untouched
    CHECK(doc["automations"][1]["triggers"][0]["nextAt"].as<long>() == 5900);
  }

  // Short and long together: only the long one causes a write, and the write
  // carries the short one's RAM value.
  rescheduleAfterFire(43, mixed, 5000, false);
  CHECK(writes == before + 1);
  CHECK(triggersFromJson(mixed, parsed, MAX_TRIGGERS) == 3 && parsed[1].slot == 1 &&
        parsed[1].nextAt == 5005 && parsed[2].nextAt == 9000);
  clockSeconds = 9000;
  rescheduleAfterFire(43, mixed, 9000, false);
  CHECK(writes == before + 2);
  {
    JsonDocument doc = storeDoc();
    JsonArray triggers = doc["automations"][2]["triggers"];
    CHECK(triggers[1]["nextAt"].as<long>() == 9005 && triggers[2]["nextAt"].as<long>() == 9900);
  }

  // The due cache and the client view use the RAM value. A disabled
  // automation holds no entry and keeps its file value.
  clockSeconds = 5006;
  CHECK(startAutomationScheduler());
  rebuildAutomationsCache();
  CHECK(gAutomationsCacheCount == 4 && gAutomationsCache[0].id == 41 && gAutomationsCache[0].nextAt == 5010);
  CHECK(gAutomationsCache[1].nextAt == 5900 && gAutomationsCache[2].nextAt == 9005);
  CHECK(gAutomationsCache[3].nextAt == 100 && sVolatileSchedule.find(44, 0) == nullptr);
  {
    JsonDocument doc = storeDoc();
    for (JsonObject automation : doc["automations"].as<JsonArray>()) {
      automationApplyRuntimeSchedule(automation);
    }
    CHECK(doc["automations"][0]["triggers"][0]["nextAt"].as<long>() == 5010);
    CHECK(doc["automations"][1]["triggers"][0]["nextAt"].as<long>() == 5900);
    CHECK(doc["automations"][3]["triggers"][0]["nextAt"].as<long>() == 100);
  }

  // After a restart the table is empty: the stale file value restarts the
  // interval instead of counting as a missed run.
  sVolatileSchedule = Table{};
  clockSeconds = 50000;
  rebuildAutomationsCache();
  CHECK(gAutomationsCache[0].nextAt == 50005 && computeNextRunTime(quick, 50000) == 50005);

  // A removed automation gives its entry back.
  CHECK(sVolatileSchedule.find(41, 0) != nullptr);
  replaceStore(String(R"({"version":2,"automations":[)") + slow + "]}");
  rebuildAutomationsCache();
  CHECK(sVolatileSchedule.find(41, 0) == nullptr && sVolatileSchedule.find(43, 1) == nullptr);
  stopAutomationScheduler();
}

// The clock question (census-duplicates-08). The scheduler took `now > 0` for
// a set clock, which is true one second after boot. It asks System_Clock now:
// a trigger that names a time of day waits for a set clock, in the pass's
// next-run computation and in the due cache alike; an interval trigger counts
// uptime meanwhile (uptimeSchedule below has the rest of that).
static void clockNotSet() {
  reset();
  clockForcedSet = false;
  // 55 hours after a boot without any time source time() reads 200000: past
  // every hand threshold the firmware had (`> 0`, `>= 100000`).
  constexpr time_t kUnset = 200000;
  constexpr time_t kSet = 1763000000;
  CHECK(!Clock::isValidEpoch(kUnset) && !Clock::isValidEpoch(1) && Clock::isValidEpoch(kSet));
  const char* daily = R"({"id":51,"enabled":true,"triggers":[{"type":"time","time":"07:30","nextAt":150000}]})";
  const char* monthly = R"({"id":55,"enabled":true,"triggers":[{"type":"time","time":"07:30","recurrence":"monthly","dayOfMonth":15,"nextAt":150000}]})";
  const char* yearly = R"({"id":56,"enabled":true,"triggers":[{"type":"time","time":"06:05","recurrence":"yearly","dayOfMonth":9,"month":3,"nextAt":150000}]})";
  const char* fresh = R"({"id":54,"enabled":true,"triggers":[{"type":"time","time":"07:30"}]})";
  const char* every = R"({"id":52,"enabled":true,"triggers":[{"type":"interval","intervalMs":900000,"nextAt":150000}]})";
  const char* both = R"({"id":53,"enabled":true,"triggers":[{"type":"time","time":"07:30","nextAt":150000},{"type":"interval","intervalMs":900000,"nextAt":190000}]})";
  clockSeconds = kUnset;
  uptimeSec = kUnset;  // the pass's now without a clock is the uptime
  const time_t up = kUnset;

  // The pass: with the clock not set, a time of day is not part of the next run.
  CHECK(automationNextRunTime(daily, up, false) == 0);
  CHECK(automationNextRunTime(monthly, up, false) == 0);
  CHECK(automationNextRunTime(yearly, up, false) == 0);
  CHECK(automationNextRunTime(fresh, up, false) == 0);  // and none is made up from 1970
  // An interval counts uptime from when the scheduler first sees it; the
  // file's value is not used.
  CHECK(automationNextRunTime(every, up, false) == up + 900);
  CHECK(automationNextRunTime(both, up, false) == up + 900);  // the interval alone decides
  // With the clock set every trigger counts; the public function is that case.
  CHECK(automationNextRunTime(daily, kSet, true) == 150000 && computeNextRunTime(daily, kSet) == 150000);
  CHECK(automationNextRunTime(both, kSet, true) == 150000 && computeNextRunTime(both, kSet) == 150000);
  CHECK(automationNextRunTime(fresh, kSet, true) > kSet && computeNextRunTime(fresh, kSet) > kSet);

  // The due cache agrees with the pass: a waiting trigger is not "due".
  replaceStore(String(R"({"version":2,"automations":[)") + daily + "," + every + "," + both + "," + monthly + "]}");
  CHECK(startAutomationScheduler());
  rebuildAutomationsCache();
  CHECK(gAutomationsCacheCount == 4 && gAutomationsCache[0].id == 51 && gAutomationsCache[0].nextAt == 0);
  CHECK(gAutomationsCache[1].nextAt == up + 900 && gAutomationsCache[2].nextAt == up + 900);
  CHECK(gAutomationsCache[3].nextAt == 0);
  CHECK(!automationsAnyDue(kUnset));
  uptimeSec = kUnset + 900;
  CHECK(automationsAnyDue(kUnset));  // the interval of 52 is due on uptime
  replaceStore(String(R"({"version":2,"automations":[)") + daily + "," + monthly + "]}");
  rebuildAutomationsCache();
  // Stored next runs of 150000 with the clock at 200000: `now > 0` called
  // both due and ran them against 1970.
  CHECK(gAutomationsCacheCount == 2 && !automationsAnyDue(kUnset));

  // The clock is set: the stored next runs count again (the pass then
  // advances a next run from before the clock was set without firing it).
  clockSeconds = kSet;
  CHECK(automationsAnyDue(kSet));  // the cache holds uptime seconds: one pass
  rebuildAutomationsCache();
  CHECK(gAutomationsCache[0].nextAt == 150000 && gAutomationsCache[1].nextAt == 150000 && automationsAnyDue(kSet));
  stopAutomationScheduler();
}

static JsonObject automationNamed(JsonDocument& doc, long id) {
  for (JsonObject automation : doc["automations"].as<JsonArray>()) {
    if (automation["id"].as<long>() == id) return automation;
  }
  CHECK(false);
  return JsonObject();
}

// Interval and armed-delay triggers without a set clock count uptime. An
// interval saved while the clock was set held a 2026 next run in the file and
// never fired on a later boot without a clock; a long one rewrote the file on
// every fire. Now: the first fire one interval after automations start, then
// every interval, no file write, and when the clock is set the schedule is
// carried over with the time each trigger had left.
static void uptimeSchedule() {
  reset();
  clockForcedSet = false;
  constexpr time_t kUnset = 200000;      // time() without a time source
  constexpr time_t kSaved = 1763000000;  // a next run saved while the clock was set
  const char* minute = R"({"id":61,"enabled":true,"commands":["a"],"triggers":[{"type":"interval","intervalMs":60000,"nextAt":1763000000}]})";
  const char* hourly = R"({"id":63,"enabled":true,"commands":["a"],"triggers":[{"type":"interval","intervalMs":3600000,"nextAt":1763000000}]})";
  const char* daily = R"({"id":65,"enabled":true,"commands":["a"],"triggers":[{"type":"time","time":"07:30","nextAt":1763000000}]})";
  replaceStore(String(R"({"version":2,"automations":[)") + minute + "," + hourly + "," + daily + "]}");
  clockSeconds = kUnset;
  uptimeSec = 1000;
  CHECK(startAutomationScheduler());
  automationTick();  // the first pass: automations start at uptime 1000
  CHECK(ticks == 1 && gAutomationsCacheCount == 3);
  // First fire one interval after the start, not at the stored 2026 run.
  CHECK(gAutomationsCache[0].nextAt == 1060 && gAutomationsCache[1].nextAt == 4600);
  CHECK(gAutomationsCache[2].nextAt == 0);  // a time of day waits for the clock
  CHECK(automationNextRunTime(minute, 1000, false) == 1060);
  uptimeSec = 1059;
  automationTick();
  CHECK(ticks == 1);  // nothing is due: no pass, no file read
  uptimeSec = 1060;
  automationTick();
  CHECK(ticks == 2);  // due on uptime
  // The pass fires it and reschedules on uptime: nothing is written, the next
  // run is one interval on, and the cache stops asking for passes.
  rescheduleAfterFire(61, minute, 1060, false);
  CHECK(writes == 0 && automationNextRunTime(minute, 1060, false) == 1120);
  notifyAutomationScheduler();
  automationTick();
  CHECK(ticks == 3 && gAutomationsCache[0].nextAt == 1120);
  for (int i = 0; i < 20; ++i) automationTick();
  CHECK(ticks == 3);
  // The hour long interval is not saved per fire either.
  uptimeSec = 4600;
  rescheduleAfterFire(63, hourly, 4600, false);
  CHECK(writes == 0 && automationNextRunTime(hourly, 4600, false) == 8200);
  // millis() wraps after 49.7 days; the schedule does not use it.
  clockMs = UINT32_MAX - 5;
  clockMs += 10;
  CHECK(automationNextRunTime(minute, 4600, false) == 1120 && automationNextRunTime(hourly, 4600, false) == 8200);
  {
    JsonDocument doc = storeDoc();
    CHECK(automationNamed(doc, 61)["triggers"][0]["nextAt"].as<long>() == kSaved);  // file untouched
  }
  stopAutomationScheduler();

#ifdef HAVE_UPTIME_SCHEDULE
  using Table = AutomationUptimeSchedule;
  {
    Table t{};
    CHECK(t.empty());
    CHECK(t.interval(0, 0, 5000, 10) == 0 && t.interval(7, 0, 0, 10) == 0 && t.empty());
    CHECK(t.interval(7, 0, 5000, 10) == 15 && t.interval(7, 0, 5000, 99) == 15);  // first sight only
    CHECK(t.interval(7, 0, 500, 10) == 11);    // a sub-second interval steps one second
    CHECK(t.interval(7, 0, 10000, 20) == 30);  // an edited interval starts again
    CHECK(t.armed(7, 0) == 0);                  // an interval is not an armed delay
    CHECK(t.arm(7, 1, 40) && t.armed(7, 1) == 40 && t.interval(7, 1, 5000, 50) == 55);  // retyped
    CHECK(t.arm(8, 0, 0) && t.armed(8, 0) == 1);  // a zero time still counts as armed
    t.advance(8, 0, 0);
    CHECK(t.armed(8, 0) == 0 && t.find(8, 0) == nullptr);  // a delay that fired is gone
    t.advance(7, 0, 77);
    CHECK(t.interval(7, 0, 10000, 70) == 77);
    t.beginSweep();
    CHECK(t.interval(7, 0, 10000, 70) == 77);
    t.endSweep();
    CHECK(t.find(7, 0) && !t.find(7, 1));
    t.clear();
    CHECK(t.empty());
    // Full: the next interval has no next run, and a delay cannot be armed.
    for (int i = 0; i < Table::kEntries; i++) CHECK(t.interval(100 + i, 0, 5000, 10) == 15);
    CHECK(t.interval(999, 0, 5000, 10) == 0 && !t.arm(999, 0, 10));
    CHECK(t.arm(100, 0, 30) && t.armed(100, 0) == 30);  // an existing entry is reused
  }
#ifdef HAVE_UPTIME_CARRY_RULE
  {
    // The clock-set carry keeps an armed delay and an interval that ran on
    // uptime; an interval in its first countdown is not carried.
    Table t{};
    CHECK(!t.anyCarries());
    CHECK(t.interval(7, 0, 5000, 10) == 15 && !Table::carries(*t.find(7, 0)) && !t.anyCarries());
    t.advance(7, 0, 20);
    CHECK(Table::carries(*t.find(7, 0)) && t.anyCarries());
    CHECK(t.interval(7, 0, 10000, 20) == 30 && !Table::carries(*t.find(7, 0)));  // edited: a new countdown
    CHECK(t.arm(8, 1, 40) && Table::carries(*t.find(8, 1)));
    CHECK(t.interval(9, 0, 5000, 10) == 15);
    // forget drops every entry of one automation and nothing else.
    t.forget(8);
    t.forget(0);
    CHECK(!t.find(8, 1) && t.find(7, 0) && t.find(9, 0));
    t.forget(7);
    CHECK(!t.find(7, 0) && t.find(9, 0));
    CHECK(t.interval(7, 0, 10000, 50) == 60);  // seen again: one interval from then
  }
#endif

  // Armed delays: in RAM, the file's value is not used, a fire disarms it.
  reset();
  clockForcedSet = false;
  clockSeconds = kUnset;
  uptimeSec = 1000;
  const char* delay = R"({"id":64,"enabled":true,"commands":["a"],"triggers":[{"type":"boot"},{"type":"manual","delayMs":30000,"nextAt":1763000500}]})";
  const char* off = R"({"id":66,"enabled":false,"commands":["a"],"triggers":[{"type":"interval","intervalMs":5000},{"type":"manual","delayMs":5000}]})";
  replaceStore(String(R"({"version":2,"automations":[)") + minute + "," + hourly + "," + delay + "," + off + "]}");
  Trigger parsed[MAX_TRIGGERS];
  CHECK(triggersFromJson(delay, parsed, MAX_TRIGGERS) == 2 && parsed[0].nextAt == 0 && parsed[1].nextAt == 0);
  CHECK(automationUptimeArm(64, 1, 1030) && automationUptimeArm(66, 1, 1005));
  CHECK(triggersFromJson(delay, parsed, MAX_TRIGGERS) == 2 && parsed[1].nextAt == 1030);
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(gAutomationsCacheCount == 4 && gAutomationsCache[2].nextAt == 1030);
  // A disabled automation's interval holds no entry; its armed delay stays.
  CHECK(sUptimeSchedule.find(66, 0) == nullptr && sUptimeSchedule.armed(66, 1) == 1005);
  rescheduleAfterFire(64, delay, 1030, false);
  CHECK(writes == 0 && automationUptimeArmedAt(64, 1) == 0);
  // The client view: no next run for what counts uptime; a time of day keeps
  // the file's value.
  {
    JsonDocument doc;
    CHECK(!deserializeJson(doc, String(R"({"automations":[)") + delay + "," + daily + "]}"));
    for (JsonObject automation : doc["automations"].as<JsonArray>()) automationApplyRuntimeSchedule(automation);
    CHECK(doc["automations"][0]["triggers"][1]["nextAt"].isNull());
    CHECK(doc["automations"][1]["triggers"][0]["nextAt"].as<long>() == kSaved);
  }
  // More intervals than the table holds: the ones beyond it wait for a set
  // clock (no next run, so the cache never calls them due).
  {
    String many = R"({"version":2,"automations":[)";
    for (int i = 0; i < AutomationUptimeSchedule::kEntries + 3; i++) {
      if (i) many += ",";
      many += String(R"({"id":)") + String(std::to_string(200 + i)) +
              R"(,"enabled":true,"triggers":[{"type":"interval","intervalMs":5000}]})";
    }
    replaceStore(many + "]}");
    notifyAutomationScheduler();
    const unsigned before = ticks;
    automationTick();
    CHECK(ticks == before + 1 && gAutomationsCacheCount == AutomationUptimeSchedule::kEntries + 3);
    // The removed automations' entries are given back at the end of that
    // rebuild; the next one fills them.
    notifyAutomationScheduler();
    automationTick();
    int scheduled = 0;
    for (int i = 0; i < gAutomationsCacheCount; i++) scheduled += gAutomationsCache[i].nextAt == 1005;
    CHECK(scheduled == AutomationUptimeSchedule::kEntries);
    CHECK(gAutomationsCache[AutomationUptimeSchedule::kEntries + 2].nextAt == 0);
    for (int i = 0; i < 20; ++i) automationTick();
    CHECK(ticks == before + 2);
  }
  stopAutomationScheduler();

  // The clock is set while the uptime schedule runs. An armed delay and an
  // interval that already ran on uptime keep the time they had left: a short
  // interval goes to the RAM schedule, a long one and an armed delay are
  // written in one save; nothing is late, nothing runs twice, nothing is
  // reported missed.
  reset();
  clockForcedSet = false;
  clockSeconds = kUnset;
  uptimeSec = 1000;
  replaceStore(String(R"({"version":2,"automations":[)") + minute + "," + hourly + "," + delay + "]}");
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(automationUptimeArm(64, 1, 1150));
  uptimeSec = 1060;
  rescheduleAfterFire(61, minute, 1060, false);  // the minute fired at 1060: next 1120
  uptimeSec = 4600;
  rescheduleAfterFire(61, minute, 4600, false);  // and on: next 4660
  rescheduleAfterFire(63, hourly, 4600, false);  // the hour fired at 4600: next 8200
  CHECK(automationUptimeArm(64, 1, 4650));
  uptimeSec = 4640;
  constexpr time_t kSet = 1763100000;
  clockSeconds = kSet;
  CHECK(automationsAnyDue(kSet));  // a pass carries the schedule over
  automationCarryUptimeSchedule(kSet);
  CHECK(writes == 1 && sUptimeSchedule.empty());
  {
    JsonDocument doc = storeDoc();
    CHECK(automationNamed(doc, 61)["triggers"][0]["nextAt"].as<long>() == kSaved);  // in RAM instead
    CHECK(automationNamed(doc, 63)["triggers"][0]["nextAt"].as<long>() == kSet + 3560);
    CHECK(automationNamed(doc, 64)["triggers"][1]["nextAt"].as<long>() == kSet + 10);
  }
  CHECK(sVolatileSchedule.find(61, 0) && sVolatileSchedule.find(61, 0)->nextAt == kSet + 20);
  rebuildAutomationsCache();
  CHECK(gAutomationsCache[0].nextAt == kSet + 20 && gAutomationsCache[1].nextAt == kSet + 3560 &&
        gAutomationsCache[2].nextAt == kSet + 10);
  CHECK(!automationsAnyDue(kSet) && !automationsAnyDue(kSet + 9) && automationsAnyDue(kSet + 10));
  CHECK(automationNextRunTime(minute, kSet, true) == kSet + 20);
  automationCarryUptimeSchedule(kSet);  // nothing left to carry
  CHECK(writes == 1);
  stopAutomationScheduler();
  sVolatileSchedule = AutomationVolatileSchedule{};

  // A trigger that ran on uptime and was due again when the clock was set (a
  // condition held it, say) runs now, not as missed.
  reset();
  clockForcedSet = false;
  clockSeconds = kUnset;
  uptimeSec = 1000;
  replaceStore(String(R"({"version":2,"automations":[)") + hourly + "]}");
  CHECK(startAutomationScheduler());
  automationTick();
  uptimeSec = 4600;
  rescheduleAfterFire(63, hourly, 4600, false);  // next 8200
  uptimeSec = 8600;  // 400 s past it
  clockSeconds = kSet;
  automationCarryUptimeSchedule(kSet);
  {
    JsonDocument doc = storeDoc();
    CHECK(automationNamed(doc, 63)["triggers"][0]["nextAt"].as<long>() == kSet);
  }
  rebuildAutomationsCache();
  CHECK(gAutomationsCache[0].nextAt == kSet && automationsAnyDue(kSet));
  stopAutomationScheduler();
  sVolatileSchedule = AutomationVolatileSchedule{};

  // Most boots have no clock for the first seconds (NTP, the phone or the
  // ring sets it). An interval still in its first countdown then changes
  // nothing: the file's next run stays, as without the uptime schedule.
  // A carry used to move it to the boot time plus one interval, so a daily
  // automation on a device switched off each night never fired.
  {
    // Clock 5 s after the start; the file's next runs lie ahead.
    reset();
    clockForcedSet = false;
    clockSeconds = kUnset;
    uptimeSec = 1000;
    const time_t kNow = kSet;
    const String day = String(R"({"id":71,"enabled":true,"commands":["a"],"triggers":[{"type":"interval","intervalMs":86400000,"nextAt":)") +
                       String(std::to_string(kNow + 40000)) + "}]}";
    const String quick = String(R"({"id":72,"enabled":true,"commands":["a"],"triggers":[{"type":"interval","intervalMs":60000,"nextAt":)") +
                         String(std::to_string(kNow + 30)) + "}]}";
    replaceStore(String(R"({"version":2,"automations":[)") + day + "," + quick + "]}");
    CHECK(startAutomationScheduler());
    automationTick();
    CHECK(gAutomationsCache[0].nextAt == 1000 + 86400 && gAutomationsCache[1].nextAt == 1060);
    uptimeSec = 1005;
    clockSeconds = kNow;
    CHECK(automationsAnyDue(kNow));  // the base changed: one pass
    const unsigned readsBefore = reads;
    automationCarryUptimeSchedule(kNow);
    CHECK(writes == 0 && sUptimeSchedule.empty());
#ifdef HAVE_UPTIME_CARRY_RULE
    CHECK(reads == readsBefore);  // nothing to carry: not even a file read
#endif
    {
      JsonDocument doc = storeDoc();
      CHECK(automationNamed(doc, 71)["triggers"][0]["nextAt"].as<long>() == kNow + 40000);
      CHECK(automationNamed(doc, 72)["triggers"][0]["nextAt"].as<long>() == kNow + 30);
    }
    rebuildAutomationsCache();
    CHECK(gAutomationsCache[0].nextAt == kNow + 40000 && gAutomationsCache[1].nextAt == kNow + 30);
    CHECK(automationNextRunTime(day.c_str(), kNow, true) == kNow + 40000);
    CHECK(!automationsAnyDue(kNow) && !automationsAnyDue(kNow + 29) && automationsAnyDue(kNow + 30));
    stopAutomationScheduler();
    sVolatileSchedule = AutomationVolatileSchedule{};
  }
  {
    // A past file next run: the pass's missed-run rule as before (due now;
    // beyond the grace the pass advances it without firing, which is
    // rescheduleAfterFire from the clock's now), within the grace it runs.
    reset();
    clockForcedSet = false;
    clockSeconds = kUnset;
    uptimeSec = 1000;
    const time_t kNow = kSet;
    const String late = String(R"({"id":73,"enabled":true,"commands":["a"],"triggers":[{"type":"interval","intervalMs":3600000,"nextAt":)") +
                        String(std::to_string(kNow - 3 * 3600)) + "}]}";
    const String recent = String(R"({"id":74,"enabled":true,"commands":["a"],"triggers":[{"type":"interval","intervalMs":3600000,"nextAt":)") +
                          String(std::to_string(kNow - 60)) + "}]}";
    replaceStore(String(R"({"version":2,"automations":[)") + late + "," + recent + "]}");
    CHECK(startAutomationScheduler());
    automationTick();
    uptimeSec = 1005;
    clockSeconds = kNow;
    automationCarryUptimeSchedule(kNow);
    CHECK(writes == 0 && sUptimeSchedule.empty());
    rebuildAutomationsCache();
    CHECK(gAutomationsCache[0].nextAt == kNow - 3 * 3600 && gAutomationsCache[1].nextAt == kNow - 60);
    CHECK(automationsAnyDue(kNow));
    CHECK(automationNextRunTime(late.c_str(), kNow, true) == kNow - 3 * 3600);
    CHECK(kNow - (kNow - 3 * 3600) > kMissedFireGraceSec && kNow - (kNow - 60) <= kMissedFireGraceSec);
    rescheduleAfterFire(73, late.c_str(), kNow, false);  // what the missed-run rule does
    {
      JsonDocument doc = storeDoc();
      CHECK(automationNamed(doc, 73)["triggers"][0]["nextAt"].as<long>() == kNow + 3600);
    }
    stopAutomationScheduler();
    sVolatileSchedule = AutomationVolatileSchedule{};
  }

#ifdef HAVE_UPTIME_CARRY_RULE
  // Adding, editing or deleting an automation drops its uptime entries
  // (automationUptimeForget, called by `automation add` and `automation
  // delete`): an armed delay does not survive an edit (with a clock the
  // rebuilt trigger is written unarmed), and an interval starts again.
  reset();
  clockForcedSet = false;
  clockSeconds = kUnset;
  uptimeSec = 1000;
  replaceStore(String(R"({"version":2,"automations":[)") + minute + "," + delay + "]}");
  CHECK(startAutomationScheduler());
  automationTick();
  CHECK(automationUptimeArm(64, 1, 1030));
  uptimeSec = 1060;
  rescheduleAfterFire(61, minute, 1060, false);  // next 1120
  uptimeSec = 1100;
  automationUptimeForget(64);
  automationUptimeForget(61);
  CHECK(sUptimeSchedule.empty());
  notifyAutomationScheduler();
  automationTick();
  CHECK(gAutomationsCache[0].nextAt == 1160 && gAutomationsCache[1].nextAt == 0);  // restarted, unarmed
  CHECK(automationUptimeArmedAt(64, 1) == 0);
  CHECK(sUptimeSchedule.find(61, 0) && !AutomationUptimeSchedule::carries(*sUptimeSchedule.find(61, 0)));
#endif
  stopAutomationScheduler();
  sVolatileSchedule = AutomationVolatileSchedule{};
#endif
}

// A clock trigger that has no next run gets its first one once the clock is
// set (automationSeedFirstRuns, called by the pass for every enabled
// automation while the clock is set). `automation add` stores no next run
// while the clock is not set (a time counted from 1970 is not one), and
// without this such an automation never ran: the pass works a missing next
// run out afresh from now on every lap, the due cache reads the stored 0 as
// "nothing scheduled", and a fire only advances a next run that exists.
#ifdef HAVE_SEED_FIRST_RUNS
static void firstRunsAfterClockSet() {
  constexpr time_t kSet = 1763000000;
  reset();
  clockForcedSet = false;
  clockSeconds = kSet;
  uptimeSec = 500;
  const char* daily = R"({"id":81,"enabled":true,"commands":["a"],"triggers":[{"type":"time","time":"07:30","nextAt":null}]})";
  const char* slow = R"({"id":82,"enabled":true,"commands":["a"],"triggers":[{"type":"interval","intervalMs":900000,"nextAt":null}]})";
  const char* quick = R"({"id":83,"enabled":true,"commands":["a"],"triggers":[{"type":"interval","intervalMs":5000,"nextAt":null}]})";
  const char* manual = R"({"id":84,"enabled":true,"commands":["a"],"triggers":[{"type":"manual","delayMs":3000,"nextAt":null},{"type":"event","on":"edge"},{"type":"boot","bootDelayMs":0,"nextAt":null}]})";
  const String kept = String(R"({"id":85,"enabled":true,"commands":["a"],"triggers":[{"type":"time","time":"07:30","nextAt":)") +
                      String(std::to_string(kSet + 500)) + R"(},{"type":"interval","intervalMs":900000,"nextAt":null}]})";
  replaceStore(String(R"({"version":2,"automations":[)") + daily + "," + slow + "," + quick + "," + manual + "," + kept + "]}");
  CHECK(startAutomationScheduler());

  // The dead end without the seed: never due, at no later time.
  rebuildAutomationsCache();
  CHECK(gAutomationsCacheCount == 5 && gAutomationsCache[0].nextAt == 0 && gAutomationsCache[1].nextAt == 0 &&
        gAutomationsCache[2].nextAt == 0 && gAutomationsCache[3].nextAt == 0);
  for (time_t later : {kSet, kSet + 3600, kSet + 86400 * 30}) {
    CHECK(automationNextRunTime(daily, later, true) > later);
    CHECK(automationNextRunTime(slow, later, true) > later);
  }
  unsigned before = writes;
  rescheduleAfterFire(81, daily, kSet + 86400, false);  // nothing to advance
  CHECK(writes == before);

  // The time of day: its first run is written, nothing fires.
  CHECK(automationSeedFirstRuns(81, daily, kSet));
  CHECK(writes == before + 1);
  time_t firstDaily = 0;
  String storedDaily;
  {
    JsonDocument doc = storeDoc();
    firstDaily = (time_t)automationNamed(doc, 81)["triggers"][0]["nextAt"].as<long long>();
    serializeJson(automationNamed(doc, 81), storedDaily);
  }
  CHECK(firstDaily > kSet && firstDaily <= kSet + 86400 && Clock::isValidEpoch(firstDaily));
  // Seeded once: the stored automation has its next run and is left alone.
  CHECK(!automationSeedFirstRuns(81, storedDaily.c_str(), kSet + 10) && writes == before + 1);
  // It comes due at that time, and the fire advances it by a day.
  CHECK(automationNextRunTime(storedDaily.c_str(), kSet + 10, true) == firstDaily);
  CHECK(automationNextRunTime(storedDaily.c_str(), firstDaily, true) == firstDaily);
  rescheduleAfterFire(81, storedDaily.c_str(), firstDaily, false);
  {
    JsonDocument doc = storeDoc();
    CHECK(automationNamed(doc, 81)["triggers"][0]["nextAt"].as<long long>() == (long long)firstDaily + 86400);
  }
  before = writes;

  // A long interval is written; a short one goes to the RAM schedule.
  CHECK(automationSeedFirstRuns(82, slow, kSet) && writes == before + 1);
  CHECK(automationSeedFirstRuns(83, quick, kSet) && writes == before + 1);
  {
    JsonDocument doc = storeDoc();
    CHECK(automationNamed(doc, 82)["triggers"][0]["nextAt"].as<long long>() == (long long)kSet + 900);
    CHECK(automationNamed(doc, 83)["triggers"][0]["nextAt"].isNull());
  }
  CHECK(sVolatileSchedule.find(83, 0) && (time_t)sVolatileSchedule.find(83, 0)->nextAt == kSet + 5);
  CHECK(automationNextRunTime(quick, kSet + 5, true) == kSet + 5);
  CHECK(!automationSeedFirstRuns(83, quick, kSet + 2) && writes == before + 1);  // the RAM entry counts

  // Manual, boot and event triggers have no clock run: left alone.
  CHECK(!automationSeedFirstRuns(84, manual, kSet) && writes == before + 1);
  // A trigger that has its next run keeps it; its neighbour without one is seeded.
  CHECK(automationSeedFirstRuns(85, kept.c_str(), kSet) && writes == before + 2);
  {
    JsonDocument doc = storeDoc();
    CHECK(automationNamed(doc, 85)["triggers"][0]["nextAt"].as<long long>() == (long long)kSet + 500);
    CHECK(automationNamed(doc, 85)["triggers"][1]["nextAt"].as<long long>() == (long long)kSet + 900);
    CHECK(automationNamed(doc, 84)["triggers"][0]["nextAt"].isNull());
  }

  // The due cache follows: each is due at its first run and not before.
  rebuildAutomationsCache();
  CHECK(gAutomationsCache[1].nextAt == kSet + 900 && gAutomationsCache[2].nextAt == kSet + 5 &&
        gAutomationsCache[3].nextAt == 0 && gAutomationsCache[4].nextAt == kSet + 500);
  CHECK(!automationsAnyDue(kSet + 4) && automationsAnyDue(kSet + 5));
  stopAutomationScheduler();
  sVolatileSchedule = AutomationVolatileSchedule{};
}
#endif

int main() {
  eventDrainFromStart();
  summaries();
  lifecycle();
  runsUntilStopped();
  fileFormatVersion();
  timing();
  invalidationAndOverlap();
  concurrentRequests();
  shortIntervals();
  uptimeSchedule();
  clockNotSet();
#ifdef HAVE_SEED_FIRST_RUNS
  firstRunsAfterClockSet();
#endif
  reset();
  std::printf("automation scheduler: %u summary, ownership, timing and overlap checks passed\n", checks.load());
}
