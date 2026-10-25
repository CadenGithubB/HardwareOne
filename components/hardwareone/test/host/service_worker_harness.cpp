// Host unit for System_TaskUtils.h's ServiceWorker, the lifecycle the idle
// service workers share (qnote, dictation_svc, cm5_presence). The struct is
// spliced from the header by test_service_worker.py; the kernel is faked below
// and every fake checks it was called outside the critical section.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++failures;                                                               \
    }                                                                           \
  } while (0)

struct portMUX_TYPE { int unused; };
#define portMUX_INITIALIZER_UNLOCKED {0}
static int gDepth = 0;
static void portENTER_CRITICAL(portMUX_TYPE*) { CHECK(gDepth == 0); ++gDepth; }
static void portEXIT_CRITICAL(portMUX_TYPE*) { CHECK(gDepth == 1); --gDepth; }
using TaskHandle_t = void*;
using BaseType_t = int;
using UBaseType_t = unsigned;
using TaskFunction_t = void (*)(void*);
using SemaphoreHandle_t = void*;
struct StaticSemaphore_t { int unused; };
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
static constexpr UBaseType_t TASK_PRIORITY_LOW = 1;
static constexpr BaseType_t PRO_CORE = 0;
static constexpr BaseType_t APP_CORE = 1;

static int gTask[4];
static unsigned tasksCreated = 0, semaphoresCreated = 0, gives = 0, takes = 0;
static uint32_t lastTakeTicks = 0;
static BaseType_t takeResult = pdFALSE;
static BaseType_t createResult = pdPASS;
static std::string createdName, createdTag;
static uint32_t createdStack = 0;
static UBaseType_t createdPriority = 0;
static BaseType_t createdCore = -1;
static void (*onCreate)() = nullptr;  // runs inside the create: a second creator
static bool wakeReadyAtCreate = false;

static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* storage) {
  CHECK(gDepth == 0);
  CHECK(storage != nullptr);
  ++semaphoresCreated;
  return storage;
}
static void xSemaphoreGive(SemaphoreHandle_t) { CHECK(gDepth == 0); ++gives; }
static BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks) {
  CHECK(gDepth == 0);
  CHECK(sem != nullptr);
  ++takes;
  lastTakeTicks = ticks;
  return takeResult;
}

// The helper calls the logged creator; the fake below reads the worker it builds.
static BaseType_t xTaskCreateLogged(TaskFunction_t body, const char* name, uint32_t stack, void* param,
                                    UBaseType_t priority, TaskHandle_t* out, const char* tag,
                                    BaseType_t core);

// INSERT_SERVICE_WORKER

static portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
static ServiceWorker worker{gMux};

static BaseType_t xTaskCreateLogged(TaskFunction_t body, const char* name, uint32_t stack, void* param,
                                    UBaseType_t priority, TaskHandle_t* out, const char* tag,
                                    BaseType_t core) {
  CHECK(gDepth == 0);
  CHECK(body != nullptr && param == nullptr);
  wakeReadyAtCreate = worker.wake != nullptr;  // published before the task exists
  if (onCreate) onCreate();
  if (createResult != pdPASS) return createResult;
  createdName = name;
  createdTag = tag;
  createdStack = stack;
  createdPriority = priority;
  createdCore = core;
  *out = &gTask[tasksCreated % 4];
  ++tasksCreated;
  return pdPASS;
}

static void body(void*) {}
static bool secondCreatorRefused = false;
static void secondCreator() {
  // Another task asking while the claim is held: refused, nothing made.
  const unsigned before = tasksCreated;
  secondCreatorRefused = !worker.ensure(body, "x", 1, "x") && tasksCreated == before;
}

int main() {
  // Before the first ensure: no kernel object, so a give is a no-op.
  worker.give();
  CHECK(gives == 0 && semaphoresCreated == 0 && !worker.present());

  // The first ensure: the wake once, published before the task; the task with
  // the module's name, stack, tag and the service defaults.
  onCreate = secondCreator;
  CHECK(worker.ensure(body, "svc", 4096, "svc.tag"));
  onCreate = nullptr;
  CHECK(semaphoresCreated == 1 && wakeReadyAtCreate && tasksCreated == 1);
  CHECK(createdName == "svc" && createdTag == "svc.tag" && createdStack == 4096 &&
        createdPriority == TASK_PRIORITY_LOW && createdCore == PRO_CORE);
  CHECK(worker.present() && worker.handle == &gTask[0] && !worker.starting);
  CHECK(secondCreatorRefused);

  // Ensured again: nothing new, still true.
  CHECK(worker.ensure(body, "svc", 4096, "svc.tag") && tasksCreated == 1 && semaphoresCreated == 1);

  // A give reaches the one semaphore; a wait reports the kernel's answer and
  // passes the time through.
  worker.give();
  CHECK(gives == 1);
  takeResult = pdTRUE;
  CHECK(worker.wait(250) && lastTakeTicks == 250);
  takeResult = pdFALSE;
  CHECK(!worker.wait(60000) && lastTakeTicks == 60000 && takes == 2);

  // The exit: not idle keeps the handle; idle nulls it, and only the handle
  // (the wake stays for the successor).
  CHECK(!worker.retireIf([] { return false; }) && worker.present());
  CHECK(worker.retireIf([] { return true; }) && !worker.present() && worker.handle == nullptr);
  CHECK(worker.wake != nullptr && semaphoresCreated == 1);
  // A give between the exit and the next ensure waits in the semaphore.
  worker.give();
  CHECK(gives == 2);

  // The next ensure makes a new task on the same wake.
  CHECK(worker.ensure(body, "svc", 4096, "svc.tag"));
  CHECK(tasksCreated == 2 && semaphoresCreated == 1 && worker.handle == &gTask[1] && !worker.starting);

  // A create the kernel refuses: false, nothing published, the claim given
  // back; the next ensure tries again.
  CHECK(worker.retireIf([] { return true; }));
  createResult = pdFAIL;
  CHECK(!worker.ensure(body, "svc", 4096, "svc.tag") && !worker.present() && !worker.starting);
  createResult = pdPASS;
  CHECK(worker.ensure(body, "svc", 4096, "svc.tag") && tasksCreated == 3 && worker.present());

  // Priority and core are the caller's to override.
  CHECK(worker.retireIf([] { return true; }));
  CHECK(worker.ensure(body, "svc", 2048, "svc.tag", 3, APP_CORE));
  CHECK(createdPriority == 3 && createdCore == APP_CORE && createdStack == 2048);

  CHECK(gDepth == 0);
  if (failures) {
    std::fprintf(stderr, "%d ServiceWorker check(s) failed\n", failures);
    return 1;
  }
  std::printf("ServiceWorker: one wake made once and published before the task, one creator at a time, "
              "give before and between tasks kept, wait passes the time through, the idle exit nulls only "
              "the handle, re-creation on the same wake, a refused create retried, overrides passed\n");
  return 0;
}
