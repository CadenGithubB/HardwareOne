// Host harness for flags and counters that more than one task changes.
//
// test_shared_flag_atomics.py inserts production text at the INSERT markers:
// the definition of each variable and the statements that change it, taken from
// the firmware sources as they are. Threads then run those statements against
// each other the way the tasks do on the device. Every check is an invariant
// that holds whenever each change is one atomic operation, however the threads
// are scheduled; a load-modify-store breaks it within a few thousand rounds.
// The type checks at the end do not depend on timing.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <type_traits>
#include <vector>

static int failures = 0;
#define CHECK(condition, what) do { \
  if (!(condition)) { std::fprintf(stderr, "FAIL (line %d): %s\n", __LINE__, what); ++failures; } \
} while (0)

static std::atomic<bool> gStart{false};
static void waitForStart() { while (!gStart.load()) std::this_thread::yield(); }
static void runTogether(std::vector<std::thread>& threads) {
  gStart.store(true);
  for (std::thread& thread : threads) thread.join();
  threads.clear();
  gStart.store(false);
}

// ---- Output lanes: gOutputFlags ---------------------------------------------
// INSERT_ROUTE_BITS
struct { bool outSerial = true; } gSettings;
// INSERT_OUTPUT_FLAGS_DEFINITION
static void applySettingsOutputLane() {
// INSERT_APPLY_SETTINGS_LANE
}
// INSERT_LANE_STEPS

constexpr int kLaneRounds = 150000;
constexpr int kRereads = 4;

// One task owns one lane bit here, so after its own change the bit must read as
// it left it until its next change, whatever the other tasks do to other bits.
static void laneOwner(void (*open)(), void (*close)(), uint32_t bit, std::atomic<uint32_t>* lost) {
  waitForStart();
  for (int round = 0; round < kLaneRounds; ++round) {
    open();
    for (int i = 0; i < kRereads; ++i) if (!(gOutputFlags & bit)) lost->fetch_add(1);
    close();
    for (int i = 0; i < kRereads; ++i) if (gOutputFlags & bit) lost->fetch_add(1);
  }
}

static void serialLaneOwner(std::atomic<uint32_t>* lost) {
  waitForStart();
  for (int round = 0; round < kLaneRounds; ++round) {
    gSettings.outSerial = (round & 1) != 0;
    applySettingsOutputLane();
    const bool open = (gOutputFlags & MSG_ROUTE_SERIAL) != 0;
    if (open != gSettings.outSerial) lost->fetch_add(1);
  }
}

static void testOutputLanes() {
  gOutputFlags = MSG_ROUTE_SERIAL;
  std::atomic<uint32_t> lostFile{0}, lostBle{0}, lostG2{0}, lostWeb{0}, lostSerial{0};
  std::vector<std::thread> threads;
  threads.emplace_back(laneOwner, laneOpen_FILE, laneClose_FILE, uint32_t(MSG_ROUTE_FILE), &lostFile);
  threads.emplace_back(laneOwner, laneOpen_BLE, laneClose_BLE, uint32_t(MSG_ROUTE_BLE), &lostBle);
  threads.emplace_back(laneOwner, laneOpen_G2, laneClose_G2, uint32_t(MSG_ROUTE_G2), &lostG2);
  threads.emplace_back(laneOwner, laneOpen_WEB, laneClose_WEB, uint32_t(MSG_ROUTE_WEB), &lostWeb);
  threads.emplace_back(serialLaneOwner, &lostSerial);
  runTogether(threads);
  CHECK(lostFile.load() == 0, "`log start`/`log stop` (FILE lane) kept against the other lanes' changes");
  CHECK(lostBle.load() == 0, "outble / BLE session end (BLE lane) kept against the other lanes' changes");
  CHECK(lostG2.load() == 0, "outg2 (G2 lane) kept against the other lanes' changes");
  CHECK(lostWeb.load() == 0, "HTTP server start/stop (WEB lane) kept against the other lanes' changes");
  CHECK(lostSerial.load() == 0, "applySettings sets the SERIAL lane and nothing else");
  // Every owner ended on "closed"; the last applySettings round had outSerial on.
  CHECK(gOutputFlags == uint32_t(MSG_ROUTE_SERIAL), "after the run only the SERIAL lane is open");

  // applySettings keeps the four runtime lanes and drops any other bit.
  gOutputFlags = MSG_ROUTE_WEB | MSG_ROUTE_FILE | MSG_ROUTE_BLE | MSG_ROUTE_G2 | MSG_ROUTE_OLED;
  gSettings.outSerial = false;
  applySettingsOutputLane();
  CHECK(gOutputFlags == uint32_t(MSG_ROUTE_WEB | MSG_ROUTE_FILE | MSG_ROUTE_BLE | MSG_ROUTE_G2),
        "applySettings with outSerial off keeps WEB, FILE, BLE and G2");
  gSettings.outSerial = true;
  applySettingsOutputLane();
  CHECK(gOutputFlags == uint32_t(MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE | MSG_ROUTE_BLE | MSG_ROUTE_G2),
        "applySettings with outSerial on adds SERIAL");
  laneClose_SERIAL();
  CHECK(!(gOutputFlags & MSG_ROUTE_SERIAL), "`outserial 0` closes the SERIAL lane");
  laneOpen_SERIAL();
  CHECK((gOutputFlags & MSG_ROUTE_SERIAL) != 0, "`outserial 1` opens the SERIAL lane");

  CHECK((std::is_same<decltype(gOutputFlags), std::atomic<uint32_t>>::value),
        "gOutputFlags is a std::atomic<uint32_t>");
}

// ---- Glasses tap masks: gCamStreamPendingTap, gHealthPagePendingTap ---------
// INSERT_TAP_MASK_DEFINITIONS
static void camTapRecord(uint32_t bit) {
// INSERT_CAM_TAP_RECORD
}
static uint32_t camTapsDrain() {
// INSERT_CAM_TAPS_DRAIN
  return taps;
}
static void healthTapRecord(uint32_t idx) {
// INSERT_HEALTH_TAP_RECORD
}
static uint32_t healthTapsDrain() {
// INSERT_HEALTH_TAPS_DRAIN
  return taps;
}

constexpr uint32_t kTapsPerRow = 60000;

// The event dispatch records one tap of a row and waits until the worker has
// taken it, as a user's next tap comes after the page reacted. Every recorded
// tap must come out of exactly one drain.
template <typename Mask>
static void tapProducer(Mask* pending, void (*record)(uint32_t), uint32_t argument, uint32_t bit) {
  waitForStart();
  for (uint32_t tap = 0; tap < kTapsPerRow; ++tap) {
    record(argument);
    for (unsigned spins = 0; (*pending & bit) != 0; ++spins) if (spins > 200) std::this_thread::yield();
  }
}

static void tapWorker(uint32_t (*drain)(), const std::atomic<bool>* stop, uint32_t* handled) {
  waitForStart();
  for (;;) {
    const bool last = stop->load();
    for (uint32_t taps = drain(); taps != 0; taps &= taps - 1) ++*handled;
    if (last) return;
  }
}

template <typename Mask>
static uint32_t runTapMask(Mask* pending, void (*record)(uint32_t), uint32_t (*drain)(),
                           const uint32_t* arguments, const uint32_t* bits, unsigned rows) {
  *pending = 0;
  std::atomic<bool> stop{false};
  uint32_t handled = 0;
  std::thread worker(tapWorker, drain, &stop, &handled);
  std::vector<std::thread> producers;
  for (unsigned row = 0; row < rows; ++row)
    producers.emplace_back(tapProducer<Mask>, pending, record, arguments[row], bits[row]);
  runTogether(producers);
  stop.store(true);
  worker.join();
  return handled;
}

static void testTapMasks() {
  const uint32_t camBits[] = {0x1u, 0x2u, 0x4u, 0x8u};  // Back, Snapshot, Rec, Settings
  const uint32_t camHandled = runTapMask(&gCamStreamPendingTap, camTapRecord, camTapsDrain, camBits, camBits, 4);
  CHECK(camHandled == 4 * kTapsPerRow, "every camera-stream row tap reaches the stream worker once");
  const uint32_t healthRows[] = {0, 1, 5, 31};
  const uint32_t healthBits[] = {1u << 0, 1u << 1, 1u << 5, 1u << 31};
  const uint32_t healthHandled = runTapMask(&gHealthPagePendingTap, healthTapRecord, healthTapsDrain,
                                            healthRows, healthBits, 4);
  CHECK(healthHandled == 4 * kTapsPerRow, "every Health page row tap reaches the page worker once");

  camTapRecord(0x2u);
  camTapRecord(0x8u);
  CHECK(camTapsDrain() == 0xAu, "two taps recorded before a drain are both in it");
  CHECK(camTapsDrain() == 0u, "a drain leaves nothing pending");

  CHECK((std::is_same<decltype(gCamStreamPendingTap), std::atomic<uint32_t>>::value),
        "gCamStreamPendingTap is a std::atomic<uint32_t>");
  CHECK((std::is_same<decltype(gHealthPagePendingTap), std::atomic<uint32_t>>::value),
        "gHealthPagePendingTap is a std::atomic<uint32_t>");
}

int main() {
  testOutputLanes();
  testTapMasks();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("shared flags and counters: every change is one atomic operation\n");
  return 0;
}
