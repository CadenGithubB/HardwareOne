// Host harness for the one table of known tasks.
//
// test_known_task_table.py inserts KnownTaskRow, kKnownTaskMax and the stack
// size constants from System_TaskUtils.h and the production
// collectKnownTaskRows() from System_TaskUtils.cpp, and compiles this file
// twice: with and without ESP-NOW. Host boundaries: the FreeRTOS handle types,
// the task handles and running flags of the owners, and the two ESP-NOW stack
// snapshot functions.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using TaskHandle_t = void*;
using UBaseType_t = unsigned int;
#define INPUT_TASK_NAME "input_gamepad"

static int gTokens[16];
static TaskHandle_t token(int i) { return &gTokens[i]; }

TaskHandle_t gCmdExecTaskHandle = nullptr;
TaskHandle_t queueProcessorTask = nullptr;
TaskHandle_t gInputTaskHandle = nullptr;
TaskHandle_t gThermalTaskHandle = nullptr;
TaskHandle_t gImuTaskHandle = nullptr;
TaskHandle_t gTofTaskHandle = nullptr;
TaskHandle_t gFmRadioTaskHandle = nullptr;
TaskHandle_t gGpsTaskHandle = nullptr;
TaskHandle_t gApdsTaskHandle = nullptr;
TaskHandle_t gPresenceTaskHandle = nullptr;
TaskHandle_t gRtcTaskHandle = nullptr;
bool gInputRunning = false;
bool gThermalRunning = false;
bool gImuRunning = false;
bool gTofRunning = false;
bool gFmRadioRunning = false;
bool gGpsRunning = false;
bool gApdsColorRunning = false;
bool gApdsProximityRunning = false;
bool gApdsGestureRunning = false;
bool gPresenceRunning = false;
bool gRtcRunning = false;

#if ENABLE_ESPNOW
// The owners' snapshots: a handle and a watermark sampled under the owner's
// lifecycle lock, or false while the task is not running.
static bool gHbUp = false;
static bool gTxUp = false;
static int gHbSnapshots = 0;
static int gTxSnapshots = 0;
bool getEspNowTaskStackSnapshot(TaskHandle_t* outHandle, UBaseType_t* outWatermark) {
  ++gHbSnapshots;
  if (!gHbUp) return false;
  *outHandle = token(0);
  *outWatermark = 1111;
  return true;
}
namespace espnowtx {
bool getTaskStackSnapshot(TaskHandle_t* outHandle, UBaseType_t* outWatermark) {
  ++gTxSnapshots;
  if (!gTxUp) return false;
  *outHandle = token(1);
  *outWatermark = 2222;
  return true;
}
}  // namespace espnowtx
#endif

// INSERT_PRODUCTION

static int failures = 0;
static int checks = 0;
static void check(bool ok, const std::string& what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

static const KnownTaskRow* find(const KnownTaskRow* rows, size_t count, const char* name) {
  for (size_t i = 0; i < count; i++) {
    if (std::strcmp(rows[i].name, name) == 0) return &rows[i];
  }
  return nullptr;
}

static size_t aliveCount(const KnownTaskRow* rows, size_t count) {
  size_t alive = 0;
  for (size_t i = 0; i < count; i++) alive += (rows[i].handle && rows[i].alive) ? 1 : 0;
  return alive;
}

// One owner's task comes up: exactly its row is readable, with its handle and
// its stack size.
static void expectOnly(const char* name, TaskHandle_t handle, uint32_t stackBytes) {
  KnownTaskRow rows[kKnownTaskMax];
  const size_t count = collectKnownTaskRows(rows, kKnownTaskMax);
  const KnownTaskRow* row = find(rows, count, name);
  check(row != nullptr, std::string("row exists: ") + name);
  if (!row) return;
  check(row->handle == handle && row->alive, std::string("row is alive with its handle: ") + name);
  check(row->stackBytes == stackBytes, std::string("row carries its stack size: ") + name);
  check(!row->ownerSampled, std::string("row leaves the watermark to the caller: ") + name);
  check(aliveCount(rows, count) == 1, std::string("no other row is alive beside ") + name);
}

int main() {
  KnownTaskRow rows[kKnownTaskMax];
  size_t count = collectKnownTaskRows(rows, kKnownTaskMax);

  // Nothing runs: every row is listed, none is readable.
  check(count == HW1_KNOWN_TASK_ROWS, "every row of the table is returned");
  check(count <= kKnownTaskMax, "the table fits kKnownTaskMax");
  check(aliveCount(rows, count) == 0, "no row is alive while nothing runs");
  std::set<std::string> names;
  for (size_t i = 0; i < count; i++) names.insert(rows[i].name);
  check(names.size() == count, "row names are unique");
  // The row the memory monitor's own copy had lost.
  check(find(rows, count, "espnow_tx") != nullptr, "the table has the espnow_tx row");
  check(find(rows, count, "espnow_task") != nullptr, "the table has the espnow_task row");

  // A smaller buffer is filled, never overrun.
  KnownTaskRow few[3];
  check(collectKnownTaskRows(few, 3) == 3, "a short buffer gets as many rows as it holds");
  check(collectKnownTaskRows(nullptr, 3) == 0, "no buffer, no rows");

  struct Flagged { const char* name; TaskHandle_t* handle; bool* running; uint32_t stackBytes; };
  const Flagged flagged[] = {
      {INPUT_TASK_NAME, &gInputTaskHandle, &gInputRunning, INPUT_STACK_WORDS},
      {"sensor_thermal", &gThermalTaskHandle, &gThermalRunning, THERMAL_STACK_WORDS},
      {"sensor_imu", &gImuTaskHandle, &gImuRunning, IMU_STACK_WORDS},
      {"sensor_tof", &gTofTaskHandle, &gTofRunning, TOF_STACK_WORDS},
      {"sensor_fmradio", &gFmRadioTaskHandle, &gFmRadioRunning, FMRADIO_STACK_WORDS},
      {"sensor_gps", &gGpsTaskHandle, &gGpsRunning, GPS_STACK_WORDS},
      {"sensor_apds", &gApdsTaskHandle, &gApdsColorRunning, APDS_STACK_WORDS},
      {"sensor_apds", &gApdsTaskHandle, &gApdsProximityRunning, APDS_STACK_WORDS},
      {"sensor_apds", &gApdsTaskHandle, &gApdsGestureRunning, APDS_STACK_WORDS},
      {"sensor_presence", &gPresenceTaskHandle, &gPresenceRunning, PRESENCE_STACK_WORDS},
      {"sensor_rtc", &gRtcTaskHandle, &gRtcRunning, RTC_STACK_WORDS},
  };
  int next = 2;
  for (const Flagged& f : flagged) {
    *f.handle = token(next++ % 16);
    // A handle whose task is not running (stop requested) is not readable.
    count = collectKnownTaskRows(rows, kKnownTaskMax);
    check(aliveCount(rows, count) == 0, std::string("a stale handle is not alive: ") + f.name);
    *f.running = true;
    expectOnly(f.name, *f.handle, f.stackBytes);
    *f.running = false;
    *f.handle = nullptr;
  }

  // The two tasks without a running flag are alive while their handle exists.
  gCmdExecTaskHandle = token(14);
  expectOnly("cmd_exec", gCmdExecTaskHandle, CMD_EXEC_STACK_WORDS);
  gCmdExecTaskHandle = nullptr;
  queueProcessorTask = token(15);
  expectOnly("sensor_queue", queueProcessorTask, SENSOR_QUEUE_STACK_WORDS);
  queueProcessorTask = nullptr;

#if ENABLE_ESPNOW
  // The ESP-NOW rows carry the watermark their owners sampled.
  const int hbBefore = gHbSnapshots, txBefore = gTxSnapshots;
  gHbUp = true;
  gTxUp = true;
  count = collectKnownTaskRows(rows, kKnownTaskMax);
  check(gHbSnapshots == hbBefore + 1 && gTxSnapshots == txBefore + 1,
        "each ESP-NOW owner is asked once per collection");
  const KnownTaskRow* hb = find(rows, count, "espnow_task");
  const KnownTaskRow* tx = find(rows, count, "espnow_tx");
  check(hb && hb->alive && hb->handle == token(0) && hb->ownerSampled && hb->highWaterMark == 1111 &&
            hb->stackBytes == ESPNOW_HB_STACK_WORDS,
        "espnow_task: owner's handle and watermark");
  check(tx && tx->alive && tx->handle == token(1) && tx->ownerSampled && tx->highWaterMark == 2222 &&
            tx->stackBytes == ESPNOW_TX_STACK_WORDS,
        "espnow_tx: owner's handle and watermark");
  check(aliveCount(rows, count) == 2, "only the two ESP-NOW rows are alive");
  gTxUp = false;
  count = collectKnownTaskRows(rows, kKnownTaskMax);
  tx = find(rows, count, "espnow_tx");
  check(tx && !tx->alive && tx->handle == nullptr, "espnow_tx stopped: the row is not alive");
  gHbUp = false;
#else
  // Without ESP-NOW the two rows stay listed and are never alive; the
  // snapshot functions do not exist in such a build.
  const KnownTaskRow* hb = find(rows, count, "espnow_task");
  const KnownTaskRow* tx = find(rows, count, "espnow_tx");
  check(hb && !hb->alive && !hb->handle, "espnow_task is not alive without ESP-NOW");
  check(tx && !tx->alive && !tx->handle, "espnow_tx is not alive without ESP-NOW");
#endif

  if (failures != 0) {
    std::fprintf(stderr, "%d of %d known task table checks failed\n", failures, checks);
    return 1;
  }
  std::printf("known task table (ESP-NOW %d): %d checks passed\n", ENABLE_ESPNOW, checks);
  return 0;
}
