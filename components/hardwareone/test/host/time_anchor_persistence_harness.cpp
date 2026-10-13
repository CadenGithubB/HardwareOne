// Drives the production time-anchor registry (System_TimeAnchors.cpp: load,
// append, prune rewrite, promote sweep and the main-loop tick) against the
// shared fake VFS. The FS lock guard and the checked text writers come from
// System_Mutex.cpp and System_Utils.cpp; the clock, the boot counter and the
// sensor-logging state are doubles. Each case runs in its own process so the
// module's statics start fresh.
#define FAKE_VFS_PROVIDE_VFS_NAMESPACE
#define FAKE_VFS_LOCK_DOUBLE
#include <Arduino.h>
#include "fake_vfs.h"
#include "System_PollPause.h"

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

static unsigned long nowMs = 60000;
static unsigned long millis() { return nowMs; }
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

static void start(const char* name, const std::string& registry) {
  currentCase = name;
  auto& fs = fake_vfs::fs();
  fs.reset();
  fake_vfs::wireLockDouble();
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

int main(int argc, char** argv) {
  const std::string which = argc > 1 ? argv[1] : "";
  struct Case { const char* name; void (*run)(); };
  static const Case cases[] = {
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
