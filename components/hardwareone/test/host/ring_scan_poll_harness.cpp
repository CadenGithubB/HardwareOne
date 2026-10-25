// The ring's polled scan (G2_Ring.cpp: the scan callback guard, RingScanCallbacks,
// g2RingScanStop, g2RingScan) with a fake Arduino BLEScan on a virtual clock.
// Each 100 ms poll advances the clock and fires what the case scheduled: the
// scan's own end, a ring advert, another client's queued job, a stop request,
// a cancel or the yield. test_ring_phone_yield.py inserts the production code.
#include "BLE_CoexRecords.h"
#include "BLE_SetupWorkerCore.h"
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <strings.h>
#include <vector>

static int gChecks = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    ++gChecks;                                                                \
    if (!(x)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x);       \
      std::exit(1);                                                           \
    }                                                                         \
  } while (0)

using BleSetupClient = hw1setup::Client;
using TickType_t = uint32_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()

class String {
 public:
  String(const char* text = "") : text_(text ? text : "") {}
  unsigned length() const { return (unsigned)text_.size(); }
  const char* c_str() const { return text_.c_str(); }
 private:
  std::string text_;
};

static std::vector<std::string> gLogs;
static void hostLog(const char* level, const char* format, ...) {
  char text[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  gLogs.push_back(std::string(level) + " " + text);
}
#define DEBUG_RING_LIFECYCLEF(...) hostLog("D", __VA_ARGS__)
#define INFO_RINGF(...) hostLog("I", __VA_ARGS__)
static bool logged(const char* needle) {
  for (const auto& line : gLogs)
    if (line.find(needle) != std::string::npos) return true;
  return false;
}

// ---- Virtual clock and the case's schedule --------------------------------------
static uint32_t gNow = 5000, gScanFrom = 0;
static uint32_t millis() { return gNow; }
struct Event { uint32_t at; std::function<void()> fire; bool done; };
static std::vector<Event> gSchedule;
static unsigned gPolls = 0;

// ---- Fake Arduino BLE -------------------------------------------------------------
struct BLEScanResults {};
struct BLEAddress {
  std::string text;
  std::string toString() const { return text; }
};
class BLEAdvertisedDevice {
 public:
  BLEAdvertisedDevice(std::string name, std::string address, int rssi)
      : name_(std::move(name)), address_{std::move(address)}, rssi_(rssi) {}
  bool haveName() const { return !name_.empty(); }
  std::string getName() const { return name_; }
  int getRSSI() const { return rssi_; }
  BLEAddress getAddress() const { return address_; }
 private:
  std::string name_;
  BLEAddress address_;
  int rssi_;
};
class BLEAdvertisedDeviceCallbacks {
 public:
  virtual ~BLEAdvertisedDeviceCallbacks() = default;
  virtual void onResult(BLEAdvertisedDevice advertisedDevice) = 0;
};
class BLEScan {
 public:
  BLEAdvertisedDeviceCallbacks* callbacks = nullptr;
  bool activeScan = false, startOk = true, running = false, endsOnItsOwn = true;
  uint16_t interval = 0, window = 0;
  unsigned starts = 0, stops = 0, clears = 0;
  uint32_t seconds = 0, startedAt = 0;
  void (*complete)(BLEScanResults) = nullptr;
  // The ring scan asks for duplicates (memory audit BLE-11): the library then
  // frees each report after the callback instead of keeping one per advertiser.
  bool wantDuplicates = false;
  void setAdvertisedDeviceCallbacks(BLEAdvertisedDeviceCallbacks* c, bool duplicates = false) {
    callbacks = c;
    wantDuplicates = duplicates;
  }
  void setActiveScan(bool active) { activeScan = active; }
  void setInterval(uint16_t i) { interval = i; }
  void setWindow(uint16_t w) { window = w; }
  bool start(uint32_t duration, void (*onComplete)(BLEScanResults), bool isContinue) {
    CHECK(!isContinue && onComplete && !running);
    ++starts;
    seconds = duration; complete = onComplete; startedAt = gNow;
    running = startOk;
    return startOk;
  }
  void stop() { ++stops; running = false; }
  void clearResults() { ++clears; }
};
static BLEScan gScan;
struct BLEDevice { static BLEScan* getScan() { return &gScan; } };

static void vTaskDelay(TickType_t ms) {
  ++gPolls;
  gNow += ms;
  for (auto& e : gSchedule)
    if (!e.done && int32_t(gNow - (gScanFrom + e.at)) >= 0) { e.done = true; e.fire(); }
  // The scanner's own end (its completion callback), unless the event is lost.
  if (gScan.running && gScan.endsOnItsOwn && gNow - gScan.startedAt >= gScan.seconds * 1000u) {
    gScan.running = false;
    gScan.complete(BLEScanResults{});
  }
}

// ---- Ring module boundary ------------------------------------------------------------
static bool gYield = false;
static bool bleCentralRingShouldYield() { return gYield; }
static uint32_t gGeneration = 9;
static uint32_t ringConnectCancelGenerationSnapshot() { return gGeneration; }
static bool ringConnectGenerationCurrent(uint32_t expected) { return expected != 0 && expected == gGeneration; }
static bool gFaulted = false;
static bool bleStackLifecycleFaulted() { return gFaulted; }
static void bleStackSetLifecycleFault(bool faulted) { gFaulted = faulted; }
static bool g2RingInit() { return true; }
static BLEAdvertisedDevice* gRingAdvertisedDevice = nullptr;
static String gRingDeviceName, gRingDeviceAddress;
static bool gRingScanFound = false;
static void g2RingSetIdentity(const char*) {}
static bool gOtherQueued = false;
static unsigned gOtherQueuedPolls = 0;
static bool bleSetupOtherQueued(BleSetupClient self) {
  CHECK(self == BleSetupClient::Ring);
  ++gOtherQueuedPolls;
  return gOtherQueued;
}

// The scan's measurement record: every Begin has exactly one End.
static struct {
  bool open = false;
  unsigned begins = 0, ends = 0;
  hw1coex::ScanOutcome outcome = hw1coex::ScanOutcome::Found;
  uint32_t endedAt = 0, matches = 0;
} gCoex;
void bleCoexScanBegin(hw1coex::Peer peer, hw1coex::Purpose purpose, uint16_t interval, uint16_t window,
                      bool active, bool duplicateFilter, uint8_t seconds) {
  CHECK(peer == hw1coex::Peer::Ring && purpose == hw1coex::Purpose::Discovery && !gCoex.open);
  CHECK(interval == 160 && window == 158 && active && !duplicateFilter && seconds > 0);
  gCoex.open = true;
  ++gCoex.begins;
}
void bleCoexScanEnd(hw1coex::ScanOutcome outcome) {
  CHECK(gCoex.open);
  gCoex.open = false;
  ++gCoex.ends;
  gCoex.outcome = outcome;
  gCoex.endedAt = gNow;
}
void bleCoexOnScanMatch(int) { ++gCoex.matches; }
uint32_t bleCoexScanMatches() { return gCoex.matches; }

// INSERT_PRODUCTION

struct Result {
  bool found;
  unsigned stops, polls;
  hw1coex::ScanOutcome outcome;
  uint32_t elapsed;  // from start to the record's end
};
static Result runScan(uint32_t seconds, bool endWhenOtherQueued, std::vector<Event> schedule,
                      bool startOk = true, bool endsOnItsOwn = true) {
  gLogs.clear();
  gScan.startOk = startOk;
  gScan.endsOnItsOwn = endsOnItsOwn;
  gScan.stops = gScan.starts = gScan.clears = 0;
  gScan.running = false;
  gCoex.begins = gCoex.ends = gCoex.matches = 0;
  gSchedule = std::move(schedule);
  gScanFrom = gNow;
  gPolls = 0;
  gOtherQueued = false;
  gYield = false;
  const bool found = g2RingScan(seconds, gGeneration, endWhenOtherQueued);
  CHECK(!gCoex.open && gCoex.begins == gCoex.ends);  // no return between Begin and End
  Result r{found, gScan.stops, gPolls, gCoex.outcome, gCoex.endedAt - gScanFrom};
  delete gRingAdvertisedDevice;
  gRingAdvertisedDevice = nullptr;
  gGeneration = 9;
  g2RingScanStopReset();  // as the next ring job does before it scans (ringSetupRun)
  return r;
}
static void ringAdvert(const char* name) {
  CHECK(gScan.callbacks);
  gScan.callbacks->onResult(BLEAdvertisedDevice(name, "aa:bb:cc:dd:ee:01", -60));
}

int main() {
  using hw1coex::ScanOutcome;
  // The scan runs to its own end: nothing found is Missed; matches seen are
  // Found. Nothing stops a scan that ended by itself.
  Result r = runScan(2, true, {});
  CHECK(!r.found && r.stops == 0 && r.outcome == ScanOutcome::Missed && r.elapsed == 2000);
  CHECK(gCoex.begins == 1 && gScan.starts == 1 && gScan.clears == 1 && gScan.activeScan);
  CHECK(gScan.interval == 100 && gScan.window == 99);
  r = runScan(2, true, {{500, [] { ++gCoex.matches; }, false}});
  CHECK(!r.found && r.stops == 0 && r.outcome == ScanOutcome::Found);
  // An advert that is not the ring changes nothing.
  r = runScan(1, true, {{300, [] { ringAdvert("EVEN G2_A1B2C3_L"); }, false}});
  CHECK(!r.found && r.stops == 0 && r.outcome == ScanOutcome::Missed && r.elapsed == 1000);
  // The ring's advert: the result callback stops the scan; the poll ends at the
  // next tick without a second stop.
  r = runScan(10, true, {{700, [] { ringAdvert("EVEN R1_A1B2C3"); }, false}});
  CHECK(r.found && r.stops == 1 && r.outcome == ScanOutcome::Found && r.elapsed == 700);
  CHECK(gRingScanFound && logged("ringscan: found"));
  // A manual scan gives way to another client's queued job within one poll,
  // and stops the scanner itself.
  r = runScan(10, true, {{300, [] { gOtherQueued = true; }, false}});
  CHECK(!r.found && r.stops == 1 && r.outcome == ScanOutcome::Aborted && r.elapsed == 300);
  CHECK(logged("ringscan ended early") && gOtherQueuedPolls > 0);
  // Without the flag (a connect's own scan) it keeps scanning.
  gOtherQueuedPolls = 0;
  r = runScan(2, false, {{300, [] { gOtherQueued = true; }, false}});
  CHECK(!r.found && r.stops == 0 && r.outcome == ScanOutcome::Missed && r.elapsed == 2000);
  CHECK(!logged("ringscan ended early") && gOtherQueuedPolls == 0);
  // g2RingScanStop (preemption, cancel) ends it within one poll; the stop it
  // made is not repeated.
  r = runScan(10, true, {{300, [] { g2RingScanStop(); }, false}});
  CHECK(!r.found && r.stops == 1 && r.outcome == ScanOutcome::Aborted && r.elapsed == 300);
  // A stale generation or the yield cuts it, and the poll stops the scanner.
  r = runScan(10, true, {{300, [] { ++gGeneration; }, false}});
  CHECK(!r.found && r.stops == 1 && r.outcome == ScanOutcome::Aborted && r.elapsed == 300);
  r = runScan(10, true, {{400, [] { gYield = true; }, false}});
  CHECK(!r.found && r.stops == 1 && r.outcome == ScanOutcome::Aborted && r.elapsed == 400);
  // The scan's own end event is lost: the deadline ends the poll, and the stop
  // frees the scanner for the next start.
  r = runScan(1, true, {}, true, /*endsOnItsOwn=*/false);
  CHECK(!r.found && r.stops == 1 && r.outcome == ScanOutcome::Missed && r.elapsed == 2000);
  // The scanner refuses to start: no poll, no stop, StartFailed.
  r = runScan(5, true, {}, /*startOk=*/false);
  CHECK(!r.found && r.stops == 0 && r.polls == 0 && r.outcome == ScanOutcome::StartFailed && r.elapsed == 0);
  // A stop request only sets the flag: only the scanning job stops the scanner.
  const unsigned stopsBefore = gScan.stops;
  g2RingScanStop();
  CHECK(gScan.stops == stopsBefore);
  // A request made after the job began but before start() still ends the scan,
  // with the job's own stop (so the scanner is never left running).
  r = runScan(1, true, {});
  CHECK(r.stops == 1 && r.outcome == ScanOutcome::Aborted && r.elapsed <= 100);
  // A stale request is cleared when the next ring job starts (ringSetupRun).
  g2RingScanStop();
  g2RingScanStopReset();
  r = runScan(1, true, {});
  CHECK(r.stops == 0 && r.outcome == ScanOutcome::Missed && r.elapsed == 1000);
  // Refused before any scan: yielding, stale, or an advert already stashed.
  gYield = true;
  CHECK(!g2RingScan(5, gGeneration, true) && gCoex.begins == 1);
  gYield = false;
  CHECK(!g2RingScan(5, gGeneration + 1, true));
  gRingAdvertisedDevice = new BLEAdvertisedDevice("EVEN R1_A1B2C3", "aa:bb:cc:dd:ee:01", -50);
  CHECK(g2RingScan(5, 0, true) && gScan.starts == 1);
  delete gRingAdvertisedDevice;
  gRingAdvertisedDevice = nullptr;
  std::printf("Ring scan poll: %d checks; own end, find, other-queued cut, stop request, stale and yield cuts,"
              " lost end event, start failure, outcomes and the post-loop stop\n", gChecks);
  return 0;
}
