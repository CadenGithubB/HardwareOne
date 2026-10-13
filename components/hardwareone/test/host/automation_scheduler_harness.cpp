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
#define DEBUGF(...) do {} while (false)
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

enum class AllocPref { RequireInternal, PreferPSRAM };
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
struct { bool automationEnabled = true; } gSettings;
static constexpr int SYSEVT_COUNT = 8;
static int systemEventKindFromName(const char* name) {
  return std::strcmp(name, "edge") == 0 ? 1 : std::strcmp(name, "other") == 0 ? 2 : 0;
}
static const char* systemEventKindName(uint8_t kind) {
  return kind == 1 ? "edge" : kind == 2 ? "other" : "?";
}
static constexpr size_t SYSEVT_DETAIL_LEN = 80;
static const char* AUTOMATIONS_JSON_FILE = "/automations.json";
static std::mutex storeMutex;
static String storeJson;
static bool storeExists = false, readSucceeds = true, writeSucceeds = true;
static std::atomic<unsigned> reads{0}, writes{0}, ticks{0};
static std::function<void()> afterRead, beforeTick;
static void io() { CHECK(stateLockDepth == 0); }
namespace VFS {
static int systemAuth(const char*) { return 0; }
static bool existsGuarded(const char*, int) {
  io(); std::lock_guard<std::mutex> lock(storeMutex); return storeExists;
}
}
static bool readText(const char*, String& out) {
  io(); ++reads;
  {
    std::lock_guard<std::mutex> lock(storeMutex);
    if (!storeExists || !readSucceeds) return false;
    out = storeJson;
  }
  if (afterRead) afterRead();
  return true;
}
static bool writeTextAtomic(const char*, const String& json) {
  io(); ++writes; std::lock_guard<std::mutex> lock(storeMutex);
  if (!writeSucceeds) return false;
  storeJson = json; storeExists = true; return true;
}
static void replaceStore(const String& json) {
  std::lock_guard<std::mutex> lock(storeMutex); storeJson = json; storeExists = true;
}
void notifyAutomationScheduler();
bool startAutomationScheduler();
void stopAutomationScheduler();
bool automationSchedulerRunning();
void automationTick();
static void schedulerTickMinute();

// INSERT_CACHE_OWNER

// Only full trigger evaluation/command dispatch is substituted. The production
// poll chooses when to call it; every call performs the real JSON cache rebuild.
static void schedulerTickMinute() {
  CHECK(stateLockDepth == 0);
  ++ticks;
  // The real full tick's pending-event drain statement is inserted here.
  // INSERT_PENDING_EVENT_DRAIN
  if (beforeTick) beforeTick();
  rebuildAutomationsCache();
}

// INSERT_SCHEDULER_API

// INSERT_TRIGGER_SUMMARY

static const char* emptyRules = R"({"version":2,"automations":[]})";
static const char* futureRules = R"({"version":2,"automations":[{"id":1,"enabled":true,"triggers":[{"type":"interval","nextAt":101}]}]})";
static const char* eventRules = R"({"version":2,"automations":[{"id":1,"enabled":true,"triggers":[{"type":"event","on":"edge","nextAt":1}]}]})";

static void reset(const char* json = emptyRules) {
  afterRead = beforeTick = {};
  stopAutomationScheduler();
  CHECK(allocations == releases);
  clockMs += 100000;
  clockSeconds = 100;
  gSettings.automationEnabled = true;
  storeExists = json != nullptr;
  storeJson = json ? json : "";
  readSucceeds = writeSucceeds = true;
  failAllocation = false;
  reads = writes = ticks = 0;
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

// Permission revoked by a writer other than the owner (settings page, import)
// stops the scheduler on its next tick; a later grant stays idle until the
// owner starts it again (`automation system enable`).
static void permissionRevoked() {
  reset(futureRules);
  CHECK(startAutomationScheduler() && automationSchedulerRunning());
  automationTick();
  CHECK(ticks == 1);
  gSettings.automationEnabled = false;
  automationTick();
  CHECK(!automationSchedulerRunning() && ticks == 1 && allocations == releases);
  gSettings.automationEnabled = true;
  automationTick();
  CHECK(!automationSchedulerRunning() && ticks == 1);
  CHECK(startAutomationScheduler() && automationSchedulerRunning());
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

int main() {
  summaries();
  lifecycle();
  permissionRevoked();
  timing();
  invalidationAndOverlap();
  concurrentRequests();
  reset();
  std::printf("automation scheduler: %u summary, ownership, timing and overlap checks passed\n", checks.load());
}
