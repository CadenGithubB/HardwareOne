// Template: production creation functions and exit macro are inserted by the
// Python runner. Only FreeRTOS/task logging boundaries are mocked here.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using BaseType_t = int;
using UBaseType_t = unsigned;
using TaskFunction_t = void (*)(void*);
struct MockTCB { TaskFunction_t entry; };
using TaskHandle_t = MockTCB*;
constexpr BaseType_t pdPASS = 1;
constexpr UBaseType_t TASK_PRIORITY_LOW = 1;
constexpr BaseType_t I2C_SENSOR_CORE = 1;
constexpr unsigned I2C_DEVICE_INPUT = 0;
constexpr const char* INPUT_TASK_NAME = "input_task";
constexpr const char* INPUT_TASK_TAG = "input";
// INSERT_PRODUCTION_CONSTANTS_HERE

TaskHandle_t gInputTaskHandle = nullptr;
TaskHandle_t gThermalTaskHandle = nullptr;
TaskHandle_t gImuTaskHandle = nullptr;
TaskHandle_t gTofTaskHandle = nullptr;
TaskHandle_t gFmRadioTaskHandle = nullptr;
TaskHandle_t gApdsTaskHandle = nullptr;
TaskHandle_t gPresenceTaskHandle = nullptr;
TaskHandle_t gGpsTaskHandle = nullptr;
TaskHandle_t gRtcTaskHandle = nullptr;

static TaskHandle_t* expectedSlot = nullptr;
static TaskFunction_t expectedEntry = nullptr;
static TaskHandle_t currentTask = nullptr;
static bool failCreation = false;
static unsigned createAttempts = 0;
static unsigned inputStops = 0;
static unsigned exitLogs = 0;
static bool (*startDuringDelete)() = nullptr;
static std::vector<TaskHandle_t> awaitingIdle;
struct TaskDeleted {};

template <typename... Args>
static void debugLog(const char*, Args...) {}
#define DEBUG_CLIF(...) debugLog(__VA_ARGS__)

static void lifecycleLog() {
  // Exit logging can yield: it must run before the slot permits another start.
  assert(expectedSlot && *expectedSlot == currentTask);
  ++exitLogs;
}
#define INFO_INPUT_LIFECYCLEF(...) lifecycleLog()
#define INFO_THERMAL_LIFECYCLEF(...) lifecycleLog()
#define INFO_IMU_LIFECYCLEF(...) lifecycleLog()
#define INFO_TOF_LIFECYCLEF(...) lifecycleLog()
#define INFO_FMRADIO_LIFECYCLEF(...) lifecycleLog()
#define INFO_APDS_LIFECYCLEF(...) lifecycleLog()
#define INFO_PRESENCE_LIFECYCLEF(...) lifecycleLog()
#define INFO_GPS_LIFECYCLEF(...) lifecycleLog()

static BaseType_t xTaskCreateLogged(TaskFunction_t entry, const char*, uint32_t stack,
                                   void* argument, UBaseType_t, TaskHandle_t* slot,
                                   const char*, BaseType_t) {
  assert(slot == expectedSlot && *slot == nullptr);
  assert(entry == expectedEntry && argument == nullptr && stack > 0);
  ++createAttempts;
  if (failCreation) return 0;
  *slot = new MockTCB{entry};
  return pdPASS;
}

[[maybe_unused]] static void requestSensorStop(unsigned device) {
  assert(device == I2C_DEVICE_INPUT);
  ++inputStops;
}

[[noreturn]] static void vTaskDelete(TaskHandle_t handle) {
  assert(handle == nullptr);  // This is self-deletion, never deleting a successor.
  assert(currentTask && expectedSlot && *expectedSlot == nullptr);
  awaitingIdle.push_back(currentTask);
  if (startDuringDelete) {
    assert(startDuringDelete());
    assert(*expectedSlot && *expectedSlot != currentTask);
  }
  throw TaskDeleted{};  // FreeRTOS self-deletion cannot return to sensor cleanup.
}

// INSERT_PRODUCTION_EXIT_HERE

static void inputTask(void*) { SENSOR_TASK_EXIT(INPUT, gInputTaskHandle); }
static void thermalTask(void*) { SENSOR_TASK_EXIT(THERMAL, gThermalTaskHandle); }
static void imuTask(void*) { SENSOR_TASK_EXIT(IMU, gImuTaskHandle); }
static void tofTask(void*) { SENSOR_TASK_EXIT(TOF, gTofTaskHandle); }
static void fmRadioTask(void*) { SENSOR_TASK_EXIT(FMRADIO, gFmRadioTaskHandle); }
static void apdsTask(void*) { SENSOR_TASK_EXIT(APDS, gApdsTaskHandle); }
static void presenceTask(void*) { SENSOR_TASK_EXIT(PRESENCE, gPresenceTaskHandle); }
static void gpsTask(void*) { SENSOR_TASK_EXIT(GPS, gGpsTaskHandle); }
static void rtcTask(void*) {
  // RTC has its own bounded invalidation loop before this production exit tail.
  // INSERT_PRODUCTION_RTC_EXIT_HERE
}

// INSERT_PRODUCTION_CREATE_HERE

static void idleRetiresDeletedTasks() {
  for (TaskHandle_t task : awaitingIdle) delete task;
  awaitingIdle.clear();
}

static void selfExit() {
  currentTask = *expectedSlot;
  assert(currentTask);
  try {
    currentTask->entry(nullptr);
    assert(false && "sensor self-deletion returned");
  } catch (const TaskDeleted&) {
    currentTask = nullptr;
  }
}

int main() {
  struct Case {
    const char* name;
    bool (*start)();
    TaskFunction_t entry;
    TaskHandle_t* slot;
  };
  const Case cases[] = {
    {"input", createInputTask, inputTask, &gInputTaskHandle},
    {"thermal", createThermalTask, thermalTask, &gThermalTaskHandle},
    {"imu", createIMUTask, imuTask, &gImuTaskHandle},
    {"tof", createToFTask, tofTask, &gTofTaskHandle},
    {"fmradio", createFMRadioTask, fmRadioTask, &gFmRadioTaskHandle},
    {"apds", createAPDSTask, apdsTask, &gApdsTaskHandle},
    {"presence", createPresenceTask, presenceTask, &gPresenceTaskHandle},
    {"gps", createGPSTask, gpsTask, &gGpsTaskHandle},
    {"rtc", createRTCTask, rtcTask, &gRtcTaskHandle},
  };
  for (const auto& test : cases) {
    expectedSlot = test.slot;
    expectedEntry = test.entry;
    assert(*test.slot == nullptr);
    const unsigned attemptsBefore = createAttempts;
    const unsigned stopsBefore = inputStops;
    failCreation = true;
    assert(!test.start());
    assert(*test.slot == nullptr && createAttempts == attemptsBefore + 1);
    assert(inputStops == stopsBefore);  // The owner, not task allocation, rolls back lifecycle state.
    failCreation = false;

    for (unsigned cycle = 0; cycle < 3; ++cycle) {
      assert(test.start());
      TaskHandle_t live = *test.slot;
      assert(live);
      const unsigned attempts = createAttempts;
      assert(test.start() && test.start());
      assert(*test.slot == live && createAttempts == attempts);
      const unsigned logs = exitLogs;
      selfExit();
      assert(*test.slot == nullptr);
      assert(exitLogs == logs + (test.slot == &gRtcTaskHandle ? 0u : 1u));
      assert(awaitingIdle.size() == 1);
      idleRetiresDeletedTasks();
      // The next iteration starts after the old TCB was actually freed. A
      // retained handle must never be passed to a task-state query here.
    }

    assert(test.start());
    startDuringDelete = test.start;
    selfExit();
    startDuringDelete = nullptr;
    // A new task is allowed as soon as shutdown publishes null, even before
    // idle reclaims the old TCB. Reclaiming it must leave its replacement live.
    TaskHandle_t replacement = *test.slot;
    assert(replacement && awaitingIdle.size() == 1);
    idleRetiresDeletedTasks();
    const unsigned attempts = createAttempts;
    assert(test.start() && *test.slot == replacement && createAttempts == attempts);
    selfExit();
    idleRetiresDeletedTasks();
    for (const auto& other : cases) assert(*other.slot == nullptr);
    std::printf("%s: failure/retry, duplicate start, and repeated restart passed\n", test.name);
  }
}
