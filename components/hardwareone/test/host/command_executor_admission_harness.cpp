// Host harness for test_command_executor_admission.py. The driver splices in
// System_Utils.cpp's executeCommandCallerAdmitted() and the opening of
// executeCommand() up to and including its admission refusal, closed with a
// "reached the handlers" return. The executor queue, the executor task handle,
// the current task and the error log are doubles.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
static int checks = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s (%s, line %d)\n", msg, #cond, __LINE__);   \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

// ---- FreeRTOS doubles ---------------------------------------------------------
using TaskHandle_t = void*;
using QueueHandle_t = void*;
static int gQueueObject, gExecTaskObject, gHttpdTaskObject;
QueueHandle_t gCmdExecQ = nullptr;
TaskHandle_t gCmdExecTaskHandle = nullptr;
static TaskHandle_t gCurrentTask = nullptr;
static const char* gCurrentTaskName = "httpd";
static TaskHandle_t xTaskGetCurrentTaskHandle() { return gCurrentTask; }
static const char* pcTaskGetName(TaskHandle_t) { return gCurrentTaskName; }

static std::string gErrorLog;
static int gErrorLines = 0;
#define ERROR_SYSTEMF(fmt, ...)                                                \
  do {                                                                         \
    char line_[256];                                                           \
    std::snprintf(line_, sizeof(line_), fmt, ##__VA_ARGS__);                   \
    gErrorLog += line_;                                                        \
    gErrorLog += '\n';                                                         \
    ++gErrorLines;                                                             \
  } while (0)

// Only the opening of executeCommand runs here; ctx is not read before the
// admission check.
struct AuthContext { int transport = 0; };

// ---- production -----------------------------------------------------------------
// INSERT_PRODUCTION

static void world(bool queue, bool execTask, TaskHandle_t current, const char* name) {
  gCmdExecQ = queue ? &gQueueObject : nullptr;
  gCmdExecTaskHandle = execTask ? &gExecTaskObject : nullptr;
  gCurrentTask = current;
  gCurrentTaskName = name;
  gErrorLog.clear();
  gErrorLines = 0;
}

static bool run(const char* line, char* out, size_t outSize) {
  AuthContext ctx;
  std::memset(out, 'x', outSize);
  return executeCommandPrefix(ctx, line, out, outSize);
}

int main() {
  char out[96];
  const char* secret = "login bob hunter2";

  // Early boot: no executor queue yet, so the calling task is the executor
  // (submitAndExecuteSync's direct path). Any task is admitted.
  world(false, false, &gHttpdTaskObject, "main");
  CHECK(executeCommandCallerAdmitted(), "early boot (no queue) is admitted");
  CHECK(run(secret, out, sizeof(out)), "early boot reaches the handlers");
  CHECK(out[0] == '\0', "early boot starts from an empty result");
  CHECK(gErrorLines == 0, "early boot logs no refusal");

  // The executor task itself.
  world(true, true, &gExecTaskObject, "cmd_exec_task");
  CHECK(executeCommandCallerAdmitted(), "cmd_exec is admitted");
  CHECK(run(secret, out, sizeof(out)), "cmd_exec reaches the handlers");
  CHECK(gErrorLines == 0, "cmd_exec logs no refusal");

  // Any other task once the queue exists: httpd (the old ESP-NOW status
  // endpoint), an automation, a BLE callback.
  world(true, true, &gHttpdTaskObject, "httpd");
  CHECK(!executeCommandCallerAdmitted(), "httpd is refused once the queue exists");
  CHECK(!run(secret, out, sizeof(out)), "a refused caller gets false");
  CHECK(std::strncmp(out, "Error", 5) == 0, "a refused caller gets an Error result");
  CHECK(gErrorLines == 1, "the refusal is logged once");
  CHECK(gErrorLog.find("httpd") != std::string::npos, "the log names the calling task");
  CHECK(gErrorLog.find("hunter2") == std::string::npos && gErrorLog.find("login") == std::string::npos,
        "the log never carries the command text");

  // The window between creating the queue and the executor task: the handle is
  // still null, and a null current-task handle must not compare equal to it.
  world(true, false, &gHttpdTaskObject, "httpd");
  CHECK(!executeCommandCallerAdmitted(), "queue without an executor task refuses other tasks");
  world(true, false, nullptr, "?");
  CHECK(!executeCommandCallerAdmitted(), "a null task handle never matches a null executor handle");

  // A tiny buffer still gets a terminated result.
  world(true, true, &gHttpdTaskObject, "httpd");
  char small[6];
  CHECK(!run(secret, small, sizeof(small)), "a refused caller with a small buffer gets false");
  CHECK(std::memchr(small, '\0', sizeof(small)) != nullptr, "the small result is terminated");

  std::fprintf(stderr, "checks=%d\n", checks);
  std::printf("command executor admission: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
