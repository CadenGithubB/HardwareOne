// The ring owner task's lifetime (G2_Ring.cpp): ringWakeOwner, the real
// ringOwnerTask on a thread of its own, ringOwnerEnsure and g2RingOwnerRelease,
// with the FreeRTOS surface faked: a binary semaphore, a self-deleting task and
// a real clock. The wake never holds a task handle; a release with no owner is
// done at once; a release while the link is up times out and keeps the owner
// (stop request withdrawn); a release while the link is down ends the owner,
// which nulls its own handle before deleting itself; ringOwnerEnsure brings a
// new owner back with the owner-only state reset; the owner cannot release
// itself. test_ring_phone_yield.py inserts the production code.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
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

// ---- FreeRTOS surface --------------------------------------------------------
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;
constexpr BaseType_t pdTRUE = 1, pdPASS = 1, pdFAIL = 0;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
constexpr BaseType_t PRO_CORE = 0;

// A binary semaphore: gives while already given collapse into one pending wake.
struct StaticSemaphore_t {
  std::mutex mutex;
  std::condition_variable cv;
  bool given = false;
};
using SemaphoreHandle_t = StaticSemaphore_t*;
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* storage) { return storage; }
static std::atomic<unsigned> gGives{0};
static void xSemaphoreGive(SemaphoreHandle_t s) {
  CHECK(s != nullptr);
  {
    std::lock_guard<std::mutex> lock(s->mutex);
    s->given = true;
  }
  ++gGives;
  s->cv.notify_one();
}
static BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
  CHECK(s != nullptr);
  std::unique_lock<std::mutex> lock(s->mutex);
  if (!s->cv.wait_for(lock, std::chrono::milliseconds(ticks), [s] { return s->given; })) return 0;
  s->given = false;
  return pdTRUE;
}

// Tasks: one std::thread each; the FakeTask object is the handle. The handle is
// written before the thread can run, as xTaskCreate does, and the owner's
// vTaskDelete(nullptr) unwinds its thread.
struct FakeTask { std::thread thread; };
using TaskHandle_t = void*;
using TaskFunction_t = void (*)(void*);
static thread_local TaskHandle_t tCurrentTask = nullptr;
static int gMainTask;
static TaskHandle_t xTaskGetCurrentTaskHandle() { return tCurrentTask ? tCurrentTask : &gMainTask; }
struct TaskDeleted {};
static std::vector<FakeTask*> gTasks;
static std::atomic<unsigned> gTaskCreates{0}, gTaskDeletes{0};
static bool gCreateFails = false;
static std::function<void()> gOnDelete;  // runs on the owner's thread at vTaskDelete
[[noreturn]] static void vTaskDelete(TaskHandle_t handle) {
  CHECK(handle == nullptr);  // the owner deletes itself
  if (gOnDelete) gOnDelete();
  ++gTaskDeletes;
  throw TaskDeleted{};
}
static void vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
static std::atomic<uint32_t> gStackFree{5000};
static std::atomic<unsigned> gLaps{0};
static std::function<void()> gOnLap;  // runs on the owner's thread once per lap
static uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t handle) {
  CHECK(handle == nullptr);  // the owner samples itself, never a kept handle
  ++gLaps;
  if (gOnLap) gOnLap();
  return gStackFree.load();
}
static BaseType_t xTaskCreateLogged(TaskFunction_t fn, const char* name, uint32_t stackBytes, void* arg,
                                    UBaseType_t priority, TaskHandle_t* handleOut, const char* tag,
                                    BaseType_t core);
static uint32_t millis() {
  static const auto t0 = std::chrono::steady_clock::now();
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
}
static std::mutex gLogMutex;
static std::vector<std::string> gLogs;
static void hostLog(const char* level, const char* format, ...) {
  char text[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  std::lock_guard<std::mutex> lock(gLogMutex);
  gLogs.push_back(std::string(level) + " " + text);
}
static bool logged(const char* needle) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  for (const auto& l : gLogs)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}
#define DEBUG_RING_LIFECYCLEF(...) hostLog("D", __VA_ARGS__)
#define WARN_RINGF(...) hostLog("W", __VA_ARGS__)

// ---- Ring module boundary: the rest of the owner's lap, idle ------------------
static portMUX_TYPE sTransportMux;
static std::atomic<bool> gOnline{false};
static std::atomic<uint32_t> gGeneration{0};
static bool ringSnapshotLink(uint32_t& generation) {
  generation = gGeneration.load();
  return gOnline.load();
}
static std::atomic<unsigned> gDrops{0}, gBegins{0};
static void ringDropOwnedGeneration(uint32_t) { ++gDrops; }
static void ringBeginOwnedGeneration(uint32_t) { ++gBegins; }
static volatile bool sSetupRequested = false;
static struct { bool valid = false; } sActivePacketAck;
static uint8_t sPacketAckCount = 0;
static bool ringServicePacketAck() { return false; }
constexpr uint8_t RING_RX_QUEUE_DEPTH = 8;
using QueueHandle_t = void*;
static QueueHandle_t sRxQueue = nullptr;  // an empty ingress: the lap's RX loop never claims a slab
static BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t) { return 0; }
static bool ringClaimQueuedRxSlab(uint8_t) { return false; }
static volatile uint32_t sRxQueueDropped = 0;
constexpr size_t R1_MAX_FRAME = 256;
struct RingRxFrame {
  uint32_t generation;
  uint16_t length;
  uint8_t bytes[R1_MAX_FRAME];
};
static RingRxFrame sRxSlabs[RING_RX_QUEUE_DEPTH + 1];
static void ringProcessRxFrame(const uint8_t*, size_t, uint32_t) { CHECK(false); }
static void ringReleaseRxSlab(uint8_t) { CHECK(false); }
static struct { bool active = false; } sSetupOwner;
static void ringServiceSetup() { CHECK(false); }
enum G2RingSetupState { G2_RING_SETUP_IDLE = 0, G2_RING_SETUP_READY };
static struct { G2RingSetupState setupState = G2_RING_SETUP_IDLE; } sControlStatus;
static std::atomic<unsigned> gServiceLaps{0};
static void ringServiceControlReconciliation() { ++gServiceLaps; }
static void ringServiceHistoryCoordinator() {}
static void ringServiceHealthPageRefreshCoordinator() {}
static void ringServiceNormalTransaction() {}
struct RingActiveTransaction { uint32_t marker = 7; };  // the NSDMI a re-creation must restore
static RingActiveTransaction sActiveTransaction;
static std::atomic<unsigned> gIngressResets{0};
static void ringResetRxIngressForInit() { ++gIngressResets; }

// INSERT_PRODUCTION

static BaseType_t xTaskCreateLogged(TaskFunction_t fn, const char* name, uint32_t stackBytes, void* arg,
                                    UBaseType_t priority, TaskHandle_t* handleOut, const char* tag,
                                    BaseType_t core) {
  CHECK(!std::strcmp(name, "r1_owner") && stackBytes == RING_OWNER_STACK_BYTES && !arg && priority == 4);
  CHECK(!std::strcmp(tag, "r1.owner") && core == PRO_CORE && handleOut == &sRingOwnerTask);
  ++gTaskCreates;
  if (gCreateFails) return pdFAIL;
  FakeTask* task = new FakeTask;
  gTasks.push_back(task);
  *handleOut = task;
  task->thread = std::thread([fn, task] {
    tCurrentTask = task;
    try {
      fn(nullptr);
    } catch (TaskDeleted&) {
    }
  });
  return pdPASS;
}

static TaskHandle_t ownerHandle() {
  std::lock_guard<std::mutex> lock(sRingOwnerMux);
  return sRingOwnerTask;
}
static bool stopRequested() {
  std::lock_guard<std::mutex> lock(sRingOwnerMux);
  return sRingOwnerStop;
}
// Waits (bounded) for the owner to run `laps` more laps.
static bool awaitLaps(unsigned laps, uint32_t limitMs = 2000) {
  const unsigned target = gLaps.load() + laps;
  const uint32_t started = millis();
  while (gLaps.load() < target) {
    if (millis() - started > limitMs) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

int main() {
  // 1. The wake holds no task handle: before the semaphore exists it is a
  //    no-op, and with no owner at all it is still just a give.
  CHECK(sRingOwnerTask == nullptr && sRingWake == nullptr);
  ringWakeOwner();
  CHECK(gGives == 0);
  sRingWake = xSemaphoreCreateBinaryStatic(&sRingWakeStorage);  // as g2RingInit does, once
  ringWakeOwner();
  ringWakeOwner();
  CHECK(gGives == 2 && xSemaphoreTake(sRingWake, 0) == pdTRUE && xSemaphoreTake(sRingWake, 0) == 0);

  // 2. A release with no owner is done at once and asks nothing.
  {
    const uint32_t started = millis();
    CHECK(g2RingOwnerRelease(1000));
    CHECK(millis() - started < 100 && !stopRequested() && gTaskCreates == 0 && gTaskDeletes == 0);
  }

  // 3. ringOwnerEnsure creates the owner once, resetting the owner-only state
  //    first; the owner then publishes its own high-water mark every lap.
  sActiveTransaction.marker = 99;
  CHECK(sRingOwnerStackFreeMin.load() == RING_OWNER_STACK_BYTES);
  CHECK(ringOwnerEnsure());
  CHECK(gTaskCreates == 1 && ownerHandle() != nullptr && !stopRequested());
  CHECK(sActiveTransaction.marker == 7 && gIngressResets == 1);
  CHECK(awaitLaps(1));
  CHECK(sRingOwnerStackFreeMin.load() == 5000);
  gStackFree = 4000;
  ringWakeOwner();
  CHECK(awaitLaps(2) && sRingOwnerStackFreeMin.load() == 4000);
  const TaskHandle_t first = ownerHandle();
  CHECK(ringOwnerEnsure() && gTaskCreates == 1 && ownerHandle() == first);  // present: nothing new
  CHECK(gDrops == 0 && gBegins == 0 && gServiceLaps == 0);

  // 4. While the link is up the owner never exits: the release times out,
  //    withdraws its request and keeps the owner, which keeps lapping.
  gGeneration = 1;
  gOnline = true;
  {
    const uint32_t started = millis();
    CHECK(!g2RingOwnerRelease(60));
    const uint32_t elapsed = millis() - started;
    CHECK(elapsed >= 60 && elapsed < 1000);
    CHECK(ownerHandle() == first && !stopRequested() && gTaskDeletes == 0);
    CHECK(logged("owner release timed out after 60 ms; owner kept"));
    CHECK(awaitLaps(2) && ownerHandle() == first);
    CHECK(!g2RingOwnerRelease(0) && ownerHandle() == first && !stopRequested());
  }
  // The lap's own work still runs beside the exit check: a requested setup
  // takes the generation and services it while online.
  sSetupRequested = true;
  sControlStatus.setupState = G2_RING_SETUP_READY;
  ringWakeOwner();
  CHECK(awaitLaps(3) && gBegins == 1 && gServiceLaps >= 1 && gDrops == 0);

  // 5. The owner cannot release itself: refused before any request is made
  //    (no stop flag, not even a wake), and it keeps lapping.
  {
    std::atomic<bool> tried{false}, result{true}, gaveWake{true};
    gOnLap = [&] {
      if (!tried.exchange(true)) {
        const unsigned gives = gGives.load();
        result = g2RingOwnerRelease(10);
        gaveWake = gGives.load() != gives;
      }
    };
    CHECK(awaitLaps(2) && tried.load() && !result.load() && !gaveWake.load());
    gOnLap = nullptr;
    CHECK(ownerHandle() == first && !stopRequested() && gTaskDeletes == 0);
    CHECK(logged("owner release refused from the owner task"));
  }

  // 6. Link down: the release wakes the owner (one give, so the exit does
  //    not wait for the lap's 20 ms timeout) and ends it. At the moment it
  //    deletes itself the handle is already null, the request cleared and the
  //    stack figure back to the whole budget.
  gOnline = false;
  {
    std::atomic<bool> handleNullAtDelete{false}, stopClearAtDelete{false}, stackResetAtDelete{false};
    gOnDelete = [&] {
      handleNullAtDelete = ownerHandle() == nullptr;
      stopClearAtDelete = !stopRequested();
      stackResetAtDelete = sRingOwnerStackFreeMin.load() == RING_OWNER_STACK_BYTES;
    };
    const unsigned gives = gGives.load();
    const uint32_t started = millis();
    CHECK(g2RingOwnerRelease(2000));
    CHECK(millis() - started < 1000 && gGives.load() == gives + 1);
    CHECK(gTaskDeletes == 1 && ownerHandle() == nullptr && !stopRequested());
    CHECK(handleNullAtDelete && stopClearAtDelete && stackResetAtDelete);
    CHECK(gDrops == 1);  // the owned generation was dropped before the exit
    CHECK(sRingOwnerStackFreeMin.load() == RING_OWNER_STACK_BYTES);
    gOnDelete = nullptr;
    gTasks.front()->thread.join();
  }
  // A producer waking a gone owner is harmless, and a second release is done.
  ringWakeOwner();
  CHECK(g2RingOwnerRelease(10) && gTaskDeletes == 1);

  // 7. Re-creation after a release: a new owner, owner-only state reset again,
  //    and a pending stale wake costs it nothing but a lap.
  sSetupRequested = false;
  sActiveTransaction.marker = 99;
  {
    const unsigned lapsBefore = gLaps.load();
    CHECK(ringOwnerEnsure());
    CHECK(gTaskCreates == 2 && ownerHandle() != nullptr && ownerHandle() != first && !stopRequested());
    CHECK(sActiveTransaction.marker == 7 && gIngressResets == 2);
    CHECK(awaitLaps(2) && gLaps.load() >= lapsBefore + 2);
    CHECK(sRingOwnerStackFreeMin.load() == 4000);
    const unsigned gives = gGives.load();
    CHECK(g2RingOwnerRelease(2000) && gTaskDeletes == 2 && ownerHandle() == nullptr);
    CHECK(gGives.load() == gives + 1);
    CHECK(sRingOwnerStackFreeMin.load() == RING_OWNER_STACK_BYTES);
    gTasks.back()->thread.join();
  }

  // 8. A failed create leaves no owner (and nothing to release).
  gCreateFails = true;
  CHECK(!ringOwnerEnsure() && gTaskCreates == 3 && ownerHandle() == nullptr);
  CHECK(logged("owner task creation failed"));
  CHECK(g2RingOwnerRelease(10));
  gCreateFails = false;

  for (FakeTask* task : gTasks) delete task;
  std::printf("Ring owner lifetime: %d checks; the owner exits only when asked and offline, "
              "nulls its own handle, and comes back on ringOwnerEnsure\n", gChecks);
  return 0;
}
