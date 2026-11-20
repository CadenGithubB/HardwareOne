// Drives the production time-anchor registry (System_TimeAnchors.cpp: load,
// append, prune rewrite, promote sweep and the main-loop tick) against the
// shared fake VFS. The FS lock guard and the checked text writers come from
// System_Mutex.cpp and System_Utils.cpp; the clock, the boot counter and the
// sensor-logging state are doubles. Each case runs in its own process so the
// module's statics start fresh.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#include <Arduino.h>
#include "fake_vfs.h"
#include "System_PollPause.h"
#include "System_LogRetentionCore.h"

// The filesystem mutex double for the real FsLockGuard. Unlike fake_vfs.h's
// own (FAKE_VFS_LOCK_DOUBLE) it can be held by another task: a try-lock then
// misses, and a wait without limit, which would hang the main loop, is counted.
using TickType_t = uint32_t;
inline constexpr TickType_t portMAX_DELAY = UINT32_MAX;
inline constexpr int pdTRUE = 1;
struct HarnessFsSemaphore {
  bool heldByMe = false, heldElsewhere = false;
  unsigned takes = 0, tryMisses = 0, blockingWaitsWhileBusy = 0;
};
static HarnessFsSemaphore gHarnessFsSemaphore;
using SemaphoreHandle_t = HarnessFsSemaphore*;
static SemaphoreHandle_t gFsMutex = &gHarnessFsSemaphore;
static bool isHeldByCurrentTask(SemaphoreHandle_t mutex) { return mutex && mutex->heldByMe; }
static bool isFsLockedByCurrentTask() { return isHeldByCurrentTask(gFsMutex); }
static int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) {
  if (mutex->heldElsewhere) {
    if (timeout != portMAX_DELAY) { ++mutex->tryMisses; return 0; }
    ++mutex->blockingWaitsWhileBusy;  // the other task "finishes" and the wait ends
    mutex->heldElsewhere = false;
  }
  mutex->heldByMe = true; ++mutex->takes; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->heldByMe = false; }

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}
volatile bool gSensorPollingPaused = false;
static unsigned pauseDepth = 0;
void pollPause(uint8_t) { ++pauseDepth; }
void pollResume(uint8_t) { if (pauseDepth) --pauseDepth; }
bool pollPaused(uint8_t) { return pauseDepth != 0; }

static unsigned checks = 0;
static const char* currentCase = "";
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "time anchor check failed in %s at line %d: %s\n", currentCase, line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// ---------------------------------------------------------------------------
// Doubles for what System_TimeAnchors.cpp reaches outside the registry.
// ---------------------------------------------------------------------------
#define CAPTURE_DIR_SENSORS "/logging_captures/sensors"
#define EXT_RAM_BSS_ATTR
#define DEBUG_SYSTEMF(...) ((void)0)
static std::vector<std::string> warnings;
static void warnSystem(const char* format, ...) {
  char line[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  warnings.emplace_back(line);
}
#define WARN_SYSTEMF(...) warnSystem(__VA_ARGS__)
static std::vector<std::string> eventLines;
static void logSystemEvent(const char* category, const char* format, ...) {
  char line[320];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  eventLines.emplace_back(std::string(category) + ": " + line);
}

// The capacity figures the retention pass reads. The internal flash: only
// what is already cached (peekStatsSnapshot), never a fresh sample; the cached
// figure is the fake volume's own, unless a case says there is none yet. The
// card: the fake VFS's own getStats (a fresh reading, recorded as "stats"),
// whatever the cache holds.
namespace VFS {
struct CapacitySnapshot { uint64_t totalBytes, usedBytes, freeBytes; uint32_t sampledAtMs; };
static bool snapshotCached[2] = {true, true};
static unsigned snapshotInvalidations[2] = {0, 0};
static uint32_t snapshotAgeMs = 0;  // how long ago the cached figure was taken
static uint32_t harnessMillis();
static bool peekStatsSnapshot(StorageType type, CapacitySnapshot& out) {
  if (!snapshotCached[type]) return false;
  uint64_t total = 0, used = 0, free = 0;
  if (!fake_vfs::fs().getStats(type == SDCARD ? fake_vfs::Backend::SD : fake_vfs::Backend::LittleFS, total, used, free)) {
    return false;
  }
  out = {total, used, free, harnessMillis() - snapshotAgeMs};
  return true;
}
static void invalidateStatsSnapshot(StorageType type) { ++snapshotInvalidations[type]; }
static size_t logReserveBytes() { return 100 * 1024; }
}  // namespace VFS

static unsigned long nowMs = 60000;
static unsigned long millis() { return nowMs; }
static uint32_t VFS::harnessMillis() { return static_cast<uint32_t>(nowMs); }
static time_t wallTime = 1760000000;  // 2025-10-09
static time_t testWallTime(time_t* out) {
  if (out) *out = wallTime;
  return wallTime;
}
#define time(out) testWallTime(out)

bool filesystemReady = true;
uint32_t gBootCounter = 5;
bool gSensorLoggingRunning = false;
String gSensorLogPath;
namespace Clock {
static bool synced = true;
static bool isSynced() { return synced; }
static bool isValidEpoch(time_t epoch) { return epoch >= 1577836800; }  // 2020-01-01
}  // namespace Clock

// INSERT_EVERY_MS
// INSERT_FS_GUARD
// INSERT_TEXT_IO
// INSERT_TIME_ANCHORS

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static const char* const kRegistry = CAPTURE_DIR_SENSORS "/.anchors.csv";

struct Row { unsigned long boot, ms; long long epoch; };

// Every line must be a complete row; returns false on a line that is not.
static bool parseRows(const std::string& text, std::vector<Row>& rows) {
  rows.clear();
  if (!text.empty() && text.back() != '\n') return false;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    Row row{};
    int used = 0;
    if (std::sscanf(line.c_str(), "%lu,%lu,%lld%n", &row.boot, &row.ms, &row.epoch, &used) != 3 ||
        used != static_cast<int>(line.size())) {
      return false;
    }
    rows.push_back(row);
  }
  return true;
}

static std::string row(unsigned long boot, unsigned long ms, long long epoch) {
  return std::to_string(boot) + "," + std::to_string(ms) + "," + std::to_string(epoch) + "\n";
}

static void tick() {
  nowMs += 5000;  // past the tick's own 5 s throttle
  timeAnchorsTick();
}
static unsigned namesRead() { return fake_vfs::fs().count("nextName"); }
static unsigned opensSoFar() { return fake_vfs::fs().count("open"); }

static void start(const char* name, const std::string& registry) {
  currentCase = name;
  auto& fs = fake_vfs::fs();
  fs.reset();
  fs.lockHeld = [] { return gHarnessFsSemaphore.heldByMe; };
  fs.mkdirAll(CAPTURE_DIR_SENSORS);
  if (!registry.empty()) fs.put(kRegistry, registry);
  setenv("TZ", "UTC", 1);
  tzset();
}

static bool hasTemporary() {
  for (const std::string& name : fake_vfs::fs().list(CAPTURE_DIR_SENSORS)) {
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) return true;
  }
  return false;
}

static void tempFault(const char* kind) {
  auto& faults = fake_vfs::fs().faults;
  // The checked writer stages in "<registry>.tmp"; the older writer used ".anchors.tmp".
  for (const char* temporary : {CAPTURE_DIR_SENSORS "/.anchors.csv.tmp", CAPTURE_DIR_SENSORS "/.anchors.tmp"}) {
    if (!std::strcmp(kind, "short")) faults.shortWriteAfter[temporary] = 4;
    else if (!std::strcmp(kind, "open")) faults.open.add(temporary);
    else if (!std::strcmp(kind, "write_error")) faults.writeError.add(temporary);
    else if (!std::strcmp(kind, "rename")) faults.rename.add(temporary);
    else check(!"known fault kind");
  }
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

// The append cannot open the registry: the boot stays unanchored and a later
// tick records it once the filesystem recovers.
static void appendRetriedAfterOpenFailure() {
  const std::string before = row(3, 1000, 1700000000);
  start("append_retried_after_open_failure", before);
  auto& fs = fake_vfs::fs();
  fs.faults.open.add(kRegistry);
  tick();
  check(fs.text(kRegistry) == before);
  tick();
  fs.faults.clear();
  tick();
  std::vector<Row> rows;
  check(parseRows(fs.text(kRegistry), rows));
  check(rows.size() == 2 && rows[0].boot == 3 && rows[1].boot == 5);
  check(rows[1].ms == nowMs && rows[1].epoch == wallTime);
  check(warnings.size() == 1);  // reported once, not on every retry
  const std::string anchored = fs.text(kRegistry);
  tick();
  check(fs.text(kRegistry) == anchored);  // recorded once
}

// A short append leaves a fragment. It is not counted as anchored, and the
// retry replaces the fragment instead of appending onto it.
static void shortAppendRepairedOnRetry() {
  const std::string before = row(3, 1000, 1700000000);
  start("short_append_repaired_on_retry", before);
  auto& fs = fake_vfs::fs();
  fs.faults.shortWriteAfter[kRegistry] = before.size() + 6;
  tick();
  check(fs.text(kRegistry).size() == before.size() + 6);  // the fragment the fault left
  fs.faults.clear();
  tick();
  std::vector<Row> rows;
  check(parseRows(fs.text(kRegistry), rows));
  check(rows.size() == 2 && rows[0].boot == 3 && rows[0].ms == 1000 && rows[0].epoch == 1700000000);
  check(rows[1].boot == 5 && rows[1].ms == nowMs && rows[1].epoch == wallTime);
  check(!hasTemporary());
}

// A write error on the append also leaves the boot unanchored for a retry.
static void appendWriteErrorRetried() {
  start("append_write_error_retried", row(3, 1000, 1700000000));
  auto& fs = fake_vfs::fs();
  fs.faults.writeError.add(kRegistry);
  fs.faults.shortWriteAfter[kRegistry] = row(3, 1000, 1700000000).size() + 3;
  tick();
  fs.faults.clear();
  tick();
  std::vector<Row> rows;
  check(parseRows(fs.text(kRegistry), rows));
  check(rows.size() == 2 && rows[1].boot == 5);
}

// An earlier boot lost power mid-append. This boot's row must not merge into
// that fragment.
static void earlierFragmentNotMerged() {
  const std::string good = row(3, 1000, 1700000000);
  start("earlier_fragment_not_merged", good + "4,20");
  tick();
  const std::string text = fake_vfs::fs().text(kRegistry);
  check(text == good + row(5, nowMs, wallTime));
}

// With unparseable rows the registry is never rewritten; the fragment is closed
// off as its own line so the new row still parses.
static void fragmentClosedOffWhenPartial() {
  const std::string before = "garbage\n" + row(3, 1000, 1700000000) + "4,20";
  start("fragment_closed_off_when_partial", before);
  tick();
  check(fake_vfs::fs().text(kRegistry) == before + "\n" + row(5, nowMs, wallTime));
}

// A sweep that ran before the anchor landed is re-armed once it does, so this
// boot's folder still promotes this boot.
static void anchorRetryRearmsSweep() {
  start("anchor_retry_rearms_sweep", row(3, 1000, 1700000000));
  auto& fs = fake_vfs::fs();
  fs.put(CAPTURE_DIR_SENSORS "/boot-5/health-boot5-1000.csv", "t,v\n");
  fs.faults.open.add(kRegistry);
  tick();
  tick();
  check(fs.exists(CAPTURE_DIR_SENSORS "/boot-5/health-boot5-1000.csv"));
  fs.faults.clear();
  for (int i = 0; i < 4; ++i) tick();
  check(!fs.exists(CAPTURE_DIR_SENSORS "/boot-5"));
}

// A prune rewrite that cannot complete keeps the previous registry, and the
// pruned row stays in RAM so a later rewrite does not drop it either.
static void failedPruneKeepsRegistry(const char* kind) {
  std::string name = std::string("failed_prune_keeps_registry_") + kind;
  const std::string before = row(2, 1000, 1700000000) + row(3, 1000, 1700100000) + row(5, 50000, wallTime - 10);
  start(name.c_str(), before);
  auto& fs = fake_vfs::fs();
  fs.put(CAPTURE_DIR_SENSORS "/boot-2/health-boot2-2000.csv", "t,v\n");
  tempFault(kind);
  for (int i = 0; i < 4; ++i) tick();
  check(!fs.exists(CAPTURE_DIR_SENSORS "/boot-2"));  // promoted and removed
  check(fs.exists(CAPTURE_DIR_SENSORS "/2023-11-14"));
  check(fs.text(kRegistry) == before);
  check(!hasTemporary());
  check(!warnings.empty());

  // Recovered filesystem: boot 3's folder promotes and its row is pruned; the
  // row kept for boot 2 is still written out.
  fs.faults.clear();
  fs.put(CAPTURE_DIR_SENSORS "/boot-3/health-boot3-2000.csv", "t,v\n");
  gSensorLoggingRunning = true;  // a logging stop re-arms the sweep
  tick();
  gSensorLoggingRunning = false;
  for (int i = 0; i < 4; ++i) tick();
  check(!fs.exists(CAPTURE_DIR_SENSORS "/boot-3"));
  check(fs.text(kRegistry) == row(2, 1000, 1700000000) + row(5, 50000, wallTime - 10));
  check(!hasTemporary());
}

// Regression: a working filesystem prunes the promoted boot's row.
static void successfulPrune() {
  start("successful_prune", row(2, 1000, 1700000000) + row(5, 50000, wallTime - 10));
  auto& fs = fake_vfs::fs();
  fs.put(CAPTURE_DIR_SENSORS "/boot-2/health-boot2-2000.csv", "t,v\n");
  for (int i = 0; i < 4; ++i) tick();
  check(!fs.exists(CAPTURE_DIR_SENSORS "/boot-2"));
  check(fs.text(kRegistry) == row(5, 50000, wallTime - 10));
  check(!hasTemporary());
  check(warnings.empty());
}

// S-5: the tick never waits for the filesystem lock. With the lock held by
// another task it does no file work, records no failure and is due again on
// the next lap; the anchor is written once after the lock is free.
static void tickDoesNotWaitForABusyLock() {
  const std::string before = row(3, 1000, 1700000000);
  start("tick_does_not_wait_for_a_busy_lock", before);
  auto& fs = fake_vfs::fs();
  fs.put(CAPTURE_DIR_SENSORS "/boot-3/sensors-boot3-2000.csv", "t,v\n");
  gHarnessFsSemaphore.heldElsewhere = true;
  const size_t callsBefore = fs.log.size();
  tick();
  check(fs.log.size() == callsBefore);  // no filesystem call at all
  check(gHarnessFsSemaphore.tryMisses == 1 && gHarnessFsSemaphore.blockingWaitsWhileBusy == 0);
  check(fs.text(kRegistry) == before && warnings.empty());
  // Due again on the very next laps, not five seconds later.
  for (int lap = 0; lap < 3; ++lap) { nowMs += 10; timeAnchorsTick(); }
  check(gHarnessFsSemaphore.tryMisses == 4 && fs.log.size() == callsBefore && warnings.empty());
  // Free again: the next lap anchors this boot, and the sweep it had not
  // started is still armed.
  gHarnessFsSemaphore.heldElsewhere = false;
  nowMs += 10;
  timeAnchorsTick();
  std::vector<Row> rows;
  check(parseRows(fs.text(kRegistry), rows) && !rows.empty());
  check(rows.back().boot == 5 && rows.back().ms == nowMs && rows.back().epoch == wallTime);
  check(!gHarnessFsSemaphore.heldByMe);
  for (int i = 0; i < 4; ++i) tick();
  check(!fs.exists(CAPTURE_DIR_SENSORS "/boot-3"));  // promoted by its anchor
  check(parseRows(fs.text(kRegistry), rows) && rows.size() == 1 && rows[0].boot == 5);  // recorded once
  check(gHarnessFsSemaphore.blockingWaitsWhileBusy == 0 && warnings.empty());
}

// The task's own hold and a device without the mutex are not "busy".
static void tickRunsUnderItsOwnLock() {
  start("tick_runs_under_its_own_lock", "");
  auto& fs = fake_vfs::fs();
  gHarnessFsSemaphore.heldByMe = true;  // an outer guard of this task
  tick();
  std::vector<Row> rows;
  check(parseRows(fs.text(kRegistry), rows) && rows.size() == 1 && rows[0].boot == 5);
  check(gHarnessFsSemaphore.heldByMe && gHarnessFsSemaphore.tryMisses == 0);  // the outer hold is kept
  gHarnessFsSemaphore.heldByMe = false;
}

static void tickRunsBeforeTheMutexExists() {
  start("tick_runs_before_the_mutex_exists", row(3, 1000, 1700000000));
  auto& fs = fake_vfs::fs();
  fs.put(CAPTURE_DIR_SENSORS "/boot-3/sensors-boot3-2000.csv", "t,v\n");
  gFsMutex = nullptr;
  for (int i = 0; i < 5; ++i) tick();
  gFsMutex = &gHarnessFsSemaphore;
  std::vector<Row> rows;
  check(parseRows(fs.text(kRegistry), rows) && rows.size() == 1 && rows[0].boot == 5);
  check(!fs.exists(CAPTURE_DIR_SENSORS "/boot-3") && gHarnessFsSemaphore.tryMisses == 0);
}

// ---------------------------------------------------------------------------
// Log retention through the tick (the rule itself: test_log_retention.py)
// ---------------------------------------------------------------------------
static const uint64_t kVolume = 1024 * 1024;
static std::string dayName(int day) {
  char name[16];
  std::snprintf(name, sizeof(name), "2025-09-%02d", day);
  return name;
}
static std::string sensorFile(const std::string& root, int day) {
  return root + CAPTURE_DIR_SENSORS "/" + dayName(day) + "/sensors-" + dayName(day) + ".csv";
}
static std::string healthFile(const std::string& root, int day) {
  return root + CAPTURE_DIR_SENSORS "/" + dayName(day) + "/health-" + dayName(day) + ".csv";
}
// Twenty stored days of 40 KB sensor data and 2 KB health data each: 840 KB.
static void fillTwentyDays(const std::string& root) {
  auto& fs = fake_vfs::fs();
  for (int day = 1; day <= 20; ++day) {
    fs.put(sensorFile(root, day), std::string(40 * 1024, 's'));
    fs.put(healthFile(root, day), std::string(2 * 1024, 'h'));
  }
}
static unsigned removalsSoFar() { return fake_vfs::fs().count("remove"); }
// Ticks until `minutes` of uptime have passed; no tick removes more than one
// file or makes more than a bounded number of filesystem calls.
static void runUntilMinute(unsigned minutes) {
  auto& fs = fake_vfs::fs();
  while (nowMs < minutes * 60000ul) {
    const unsigned removed = removalsSoFar();
    const size_t calls = fs.log.size();
    const unsigned names = namesRead();
    tick();
    check(removalsSoFar() - removed <= LogRetention::kRemovalsPerStep);
    // Directory names read without opening anything: at most the walk limit
    // and one step's entries (and the one-entry look at an emptied folder).
    check(namesRead() - names <= LogRetention::kWalkMaxEntries + LogRetention::kEntriesPerStep + 1);
    check(fs.log.size() - calls - (namesRead() - names) <= 120);
    check(!gHarnessFsSemaphore.heldByMe && fs.liveHandles() == 0);
  }
}
static unsigned retentionLines() {
  unsigned lines = 0;
  for (const std::string& line : eventLines) lines += line.rfind("STORAGE: retention:", 0) == 0;
  return lines;
}

// No card: below the low-water mark the oldest day's secondary files go, a day
// a pass, until the high-water mark; health files and the registry stay.
static void retentionPrunesOldestSecondaryDays() {
  start("retention_prunes_oldest_secondary_days", row(5, 50000, wallTime - 10));
  auto& fs = fake_vfs::fs();
  fs.capacity[0].total = kVolume;
  fs.sdAvailable() = false;
  fillTwentyDays("");
  // 840 KB of 1024 KB used: free is below the 204.8 KB low-water mark... not yet:
  // the anchor registry and rounding leave it just above, so add a little.
  fs.put(CAPTURE_DIR_SENSORS "/2025-09-20/extra.csv", std::string(8 * 1024, 'x'));
  uint64_t total = 0, used = 0, free = 0;
  check(fs.getStats(fake_vfs::Backend::LittleFS, total, used, free));
  check(free < LogRetention::lowWaterBytes(kVolume, 100 * 1024));
  check(LogRetention::highWaterBytes(kVolume, 100 * 1024) - free > 80 * 1024);  // more than two days' worth

  // Nothing in the first ten minutes.
  runUntilMinute(9);
  check(removalsSoFar() == 0 && retentionLines() == 0);
  // The first pass: the oldest day's sensor file, and only that.
  runUntilMinute(15);
  check(!fs.exists(sensorFile("", 1)) && fs.exists(healthFile("", 1)));
  check(fs.exists(sensorFile("", 2)) && removalsSoFar() == 1 && retentionLines() == 1);
  check(eventLines.back().find("removed 1 capture file(s) of 2025-09-01, 40 KB, on internal flash") != std::string::npos);
  check(VFS::snapshotInvalidations[VFS::INTERNAL] == 1);
  // No second pass before an hour has gone by.
  runUntilMinute(60);
  check(removalsSoFar() == 1);
  // One day an hour until free space is back above the high-water mark: the
  // 132 KB that were missing are four days of 40 KB.
  const unsigned kDays = 4;
  for (unsigned pass = 2; pass <= kDays; ++pass) {
    runUntilMinute(60 * (pass - 1) + 30);
    check(removalsSoFar() == pass && retentionLines() == pass);
  }
  check(fs.getStats(fake_vfs::Backend::LittleFS, total, used, free));
  check(free >= LogRetention::highWaterBytes(kVolume, 100 * 1024));
  check(free - 40 * 1024 < LogRetention::highWaterBytes(kVolume, 100 * 1024));  // and not a day more
  const unsigned gone = removalsSoFar();
  for (int day = 1; day <= 20; ++day) {
    check(fs.exists(healthFile("", day)));                               // every health file stays
    check(fs.exists(sensorFile("", day)) == (day > static_cast<int>(kDays)));  // oldest first, and no further
    check(fs.isDirectory(CAPTURE_DIR_SENSORS "/" + dayName(day)));       // a folder with a health file stays
  }
  check(fs.exists(kRegistry) && fs.exists(CAPTURE_DIR_SENSORS "/2025-09-20/extra.csv"));
  // Above the mark nothing more goes, however long it runs.
  runUntilMinute(60 * 12);
  check(removalsSoFar() == gone && retentionLines() == kDays);
}

// The open session's file is never removed, and a lock another task holds
// only delays a step.
static void retentionKeepsTheOpenSessionAndWaitsForTheLock() {
  start("retention_keeps_the_open_session_and_waits_for_the_lock", row(5, 50000, wallTime - 10));
  auto& fs = fake_vfs::fs();
  fs.capacity[0].total = kVolume;
  fs.sdAvailable() = false;
  fillTwentyDays("");
  fs.put(CAPTURE_DIR_SENSORS "/2025-09-20/extra.csv", std::string(8 * 1024, 'x'));
  gSensorLoggingRunning = true;
  gSensorLogPath = sensorFile("", 1).c_str();
  fs.put(sensorFile("", 1) + ".1", std::string(1024, 'r'));  // its rotation
  runUntilMinute(9);
  // The lock is busy when the pass would start: nothing happens, nothing waits.
  gHarnessFsSemaphore.heldElsewhere = true;
  runUntilMinute(12);
  check(removalsSoFar() == 0 && gHarnessFsSemaphore.blockingWaitsWhileBusy == 0 && gHarnessFsSemaphore.tryMisses > 0);
  gHarnessFsSemaphore.heldElsewhere = false;
  runUntilMinute(20);
  check(fs.exists(sensorFile("", 1)) && fs.exists(sensorFile("", 1) + ".1"));  // the open session and its rotation
  check(!fs.exists(sensorFile("", 2)) && removalsSoFar() == 1);                // the next day went instead
  check(eventLines.back().find("2025-09-02") != std::string::npos);
}

// Free space above the mark, a writable card, no cached figure, no clock: the
// internal flash is left alone.
static void retentionLeavesTheFlashAlone() {
  start("retention_leaves_the_flash_alone", row(5, 50000, wallTime - 10));
  auto& fs = fake_vfs::fs();
  fs.sdAvailable() = false;
  fillTwentyDays("");
  // Above the low-water mark (the default 4 MB volume).
  runUntilMinute(60 * 3);
  check(removalsSoFar() == 0 && retentionLines() == 0);

  // Below the mark, but a writable card takes the overflow: the days on flash
  // stay, and the card itself has room.
  fs.capacity[0].total = kVolume;
  fs.put(CAPTURE_DIR_SENSORS "/2025-09-20/extra.csv", std::string(8 * 1024, 'x'));
  fs.sdAvailable() = true;
  fs.capacity[1].total = 64ull * 1024 * 1024 * 1024;
  runUntilMinute(60 * 6);
  check(removalsSoFar() == 0 && retentionLines() == 0);

  // No card, but no cached free-space figure either: the pass does not sample one.
  fs.sdAvailable() = false;
  VFS::snapshotCached[VFS::INTERNAL] = false;
  const unsigned statsBefore = fs.count("stats");
  runUntilMinute(60 * 9);
  check(removalsSoFar() == 0 && fs.count("stats") == statsBefore);
  // A figure that is there but old is not acted on either.
  VFS::snapshotCached[VFS::INTERNAL] = true;
  VFS::snapshotAgeMs = LogRetention::kFreeFigureMaxAgeMs + 1000;
  runUntilMinute(60 * 12);
  check(removalsSoFar() == 0 && retentionLines() == 0);
  // And no clock, no pass.
  VFS::snapshotAgeMs = 0;
  Clock::synced = false;
  runUntilMinute(60 * 15);
  check(removalsSoFar() == 0 && retentionLines() == 0);
  // With all of that in order the same volume is pruned (the checks above
  // did not pass for want of anything to remove).
  Clock::synced = true;
  runUntilMinute(60 * 17);
  check(removalsSoFar() >= 1 && retentionLines() >= 1 && !fs.exists(sensorFile("", 1)));
}

// A card that runs low is pruned by the same rule, on the card's own tree.
// Nothing keeps a cached figure of the card fresh (the log writer that fills
// it never samples it), so the decision reads the card itself: once a
// decision, not once a tick.
static void retentionPrunesTheCard() {
  start("retention_prunes_the_card", row(5, 50000, wallTime - 10));
  auto& fs = fake_vfs::fs();
  fs.sdAvailable() = true;
  fs.capacity[1].total = kVolume;
  fs.mkdirAll("/sd" CAPTURE_DIR_SENSORS);
  fillTwentyDays("/sd");
  fs.put("/sd" CAPTURE_DIR_SENSORS "/2025-09-20/extra.csv", std::string(8 * 1024, 'x'));
  fs.put(sensorFile("", 1), "kept on flash");
  VFS::snapshotCached[VFS::SDCARD] = false;  // no status page open, no transcript saved: no cached figure
  const unsigned statsBefore = fs.count("stats");
  runUntilMinute(20);
  check(!fs.exists(sensorFile("/sd", 1)) && fs.exists(healthFile("/sd", 1)) && fs.exists(sensorFile("/sd", 2)));
  check(fs.exists(sensorFile("", 1)));  // the flash copy of the tree is not the card's
  check(retentionLines() == 1 && eventLines.back().find("on the SD card") != std::string::npos);
  check(VFS::snapshotInvalidations[VFS::SDCARD] == 1 && VFS::snapshotInvalidations[VFS::INTERNAL] == 0);
  check(fs.count("stats") - statsBefore == 1);  // one reading for the one decision
  // The next decision, an hour later, reads the card again and takes the next day.
  runUntilMinute(80);
  check(!fs.exists(sensorFile("/sd", 2)) && retentionLines() == 2 && fs.count("stats") - statsBefore == 2);
  // A card that cannot be read (pulled between the mount check and the
  // reading) removes nothing, and the decision is taken again a minute later.
  fs.capacity[1].statsFail = true;
  const unsigned removedBefore = removalsSoFar();
  runUntilMinute(135);
  check(removalsSoFar() == removedBefore);
  fs.capacity[1].statsFail = false;
  runUntilMinute(140);
  check(removalsSoFar() > removedBefore && !fs.exists(sensorFile("/sd", 3)));
}

// The internal flash without a cached figure is asked again a minute later,
// not an hour later: the first figure that turns up is acted on.
static void retentionRetriesWithoutAFigure() {
  start("retention_retries_without_a_figure", row(5, 50000, wallTime - 10));
  auto& fs = fake_vfs::fs();
  fs.capacity[0].total = kVolume;
  fs.sdAvailable() = false;
  fillTwentyDays("");
  fs.put(CAPTURE_DIR_SENSORS "/2025-09-20/extra.csv", std::string(8 * 1024, 'x'));
  VFS::snapshotCached[VFS::INTERNAL] = false;
  runUntilMinute(30);
  check(removalsSoFar() == 0 && retentionLines() == 0);
  VFS::snapshotCached[VFS::INTERNAL] = true;  // a log writer has sampled the volume
  runUntilMinute(33);
  check(removalsSoFar() == 1 && !fs.exists(sensorFile("", 1)));
  // And from that decision on it is once an hour again.
  runUntilMinute(90);
  check(removalsSoFar() == 1);
  runUntilMinute(96);
  check(removalsSoFar() == 2);
}

// A sensors folder with more day folders than the walk limit: no step opens
// the entries it passes (they are named from the directory), the pass still
// removes the oldest listed day's sensor log, and the days beyond the limit
// are not looked at.
static void retentionWalksALargeFolderByName() {
  start("retention_walks_a_large_folder_by_name", row(5, 50000, wallTime - 10));
  auto& fs = fake_vfs::fs();
  fs.sdAvailable() = false;
  std::vector<std::string> days;
  for (int year = 2023; year <= 2024 && days.size() < 600; ++year) {
    for (int month = 1; month <= 12 && days.size() < 600; ++month) {
      for (int day = 1; day <= 28 && days.size() < 600; ++day) {
        char name[16];
        std::snprintf(name, sizeof(name), "%04d-%02d-%02d", year, month, day);
        days.emplace_back(name);
      }
    }
  }
  check(days.size() == 600 && days.size() > LogRetention::kWalkMaxEntries);
  for (const std::string& day : days) {
    fs.put(std::string(CAPTURE_DIR_SENSORS "/") + day + "/sensors-" + day + ".csv", std::string(1024, 's'));
  }
  uint64_t total = 0, used = 0, free = 0;
  check(fs.getStats(fake_vfs::Backend::LittleFS, total, used, free));
  fs.capacity[0].total = used + 150 * 1024;  // below the low-water mark of two reserves
  while (nowMs < 9 * 60000ul) tick();  // the anchor and the promote sweep are done by now
  check(removalsSoFar() == 0);
  const unsigned opensBefore = opensSoFar(), nextBefore = fs.count("openNext");
  unsigned ticks = 0, mostNames = 0;
  while (removalsSoFar() == 0) {
    const unsigned names = namesRead(), opens = opensSoFar();
    tick();
    check(opensSoFar() - opens <= 3);  // the folder itself, and the one file that goes
    mostNames = std::max(mostNames, namesRead() - names);
    check(++ticks < 400);
  }
  check(fs.count("openNext") == nextBefore);        // no entry is opened to be listed
  check(opensSoFar() - opensBefore <= ticks + 4);   // about one directory open a step
  check(mostNames > LogRetention::kEntriesPerStep && mostNames <= LogRetention::kWalkMaxEntries + LogRetention::kEntriesPerStep);
  // The oldest day went, with its folder (nothing else was in it): the pass
  // ends on one of the next steps.
  for (int i = 0; i < 6 && retentionLines() == 0; ++i) tick();
  check(retentionLines() == 1 && removalsSoFar() == 1);
  check(!fs.exists(std::string(CAPTURE_DIR_SENSORS "/") + days[0]));
  check(fs.exists(std::string(CAPTURE_DIR_SENSORS "/") + days[1] + "/sensors-" + days[1] + ".csv"));
  check(eventLines.back().find("removed 1 capture file(s) of 2023-01-01, 1 KB, on internal flash") != std::string::npos);
}

int main(int argc, char** argv) {
  const std::string which = argc > 1 ? argv[1] : "";
  struct Case { const char* name; void (*run)(); };
  static const Case cases[] = {
    {"tick_does_not_wait_for_a_busy_lock", tickDoesNotWaitForABusyLock},
    {"tick_runs_under_its_own_lock", tickRunsUnderItsOwnLock},
    {"tick_runs_before_the_mutex_exists", tickRunsBeforeTheMutexExists},
    {"retention_prunes_oldest_secondary_days", retentionPrunesOldestSecondaryDays},
    {"retention_keeps_the_open_session_and_waits_for_the_lock", retentionKeepsTheOpenSessionAndWaitsForTheLock},
    {"retention_leaves_the_flash_alone", retentionLeavesTheFlashAlone},
    {"retention_prunes_the_card", retentionPrunesTheCard},
    {"retention_retries_without_a_figure", retentionRetriesWithoutAFigure},
    {"retention_walks_a_large_folder_by_name", retentionWalksALargeFolderByName},
    {"append_retried_after_open_failure", appendRetriedAfterOpenFailure},
    {"short_append_repaired_on_retry", shortAppendRepairedOnRetry},
    {"append_write_error_retried", appendWriteErrorRetried},
    {"earlier_fragment_not_merged", earlierFragmentNotMerged},
    {"fragment_closed_off_when_partial", fragmentClosedOffWhenPartial},
    {"anchor_retry_rearms_sweep", anchorRetryRearmsSweep},
    {"failed_prune_keeps_registry_short", [] { failedPruneKeepsRegistry("short"); }},
    {"failed_prune_keeps_registry_open", [] { failedPruneKeepsRegistry("open"); }},
    {"failed_prune_keeps_registry_write_error", [] { failedPruneKeepsRegistry("write_error"); }},
    {"failed_prune_keeps_registry_rename", [] { failedPruneKeepsRegistry("rename"); }},
    {"successful_prune", successfulPrune},
  };
  if (which == "--list") {
    for (const Case& c : cases) std::printf("%s\n", c.name);
    return 0;
  }
  for (const Case& c : cases) {
    if (which != c.name) continue;
    c.run();
    std::printf("%s: %u checks passed\n", c.name, checks);
    return 0;
  }
  std::fprintf(stderr, "unknown case: %s\n", which.c_str());
  return 2;
}
