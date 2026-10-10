#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using String = std::string;

// INSERT_TYPES

struct CommandContext {
  AuthContext auth{};
  TransportSessionEpoch transportSessionEpoch = 0;
  uint32_t behaviorFlags = COMMAND_CONTEXT_DEFAULT;
  bool validateOnly = false;
};
static CommandContext invocation;
static CommandContext* currentContext;
static void* currentCommandContext() { return currentContext; }

using TaskHandle_t = void*;
static char executorTask, mainTask;
TaskHandle_t gCmdExecTaskHandle = &executorTask;
static TaskHandle_t currentTask = &executorTask;
static TaskHandle_t xTaskGetCurrentTaskHandle() { return currentTask; }
static int64_t nowUs;
static int64_t esp_timer_get_time() { return nowUs; }

using TickType_t = uint32_t;
using SemaphoreHandle_t = std::recursive_mutex*;
using StaticSemaphore_t = std::recursive_mutex;
static constexpr TickType_t portMAX_DELAY = UINT32_MAX;
static constexpr int pdTRUE = 1;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* storage) { return storage; }
static int xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t wait) {
  if (wait == 0) return mutex->try_lock() ? pdTRUE : 0;
  mutex->lock(); return pdTRUE;
}
static void xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) { mutex->unlock(); }
#define DEBUGF(...) ((void)0)

static std::vector<std::pair<CommandSource, TransportSessionEpoch>> liveSessions;
static bool transportSessionEpochIsLive(CommandSource source, TransportSessionEpoch epoch) {
  return std::find(liveSessions.begin(), liveSessions.end(), std::make_pair(source, epoch)) != liveSessions.end();
}

// INSERT_PRODUCTION

static int inputCalls, tickCalls, exitCalls, acceptedCompletions;
static bool loseOwnerInInput, reloginInInput, completeOnTick;
static bool exitEligibility, otherTaskEligibility, missingExecutorEligibility;
static CommandSource ownerSource;
static TransportSessionEpoch ownerEpoch;
static int completedScenarios;

static void closeOwner() {
  const auto owner = std::make_pair(ownerSource, ownerEpoch);
  liveSessions.erase(std::remove(liveSessions.begin(), liveSessions.end(), owner), liveSessions.end());
}
static void onEnter(void*) { assert(!cliModeExitCanComplete()); }
static CLIModeInputResult onInput(const String&, void*, char*, size_t) {
  ++inputCalls;
  assert(!cliModeExitCanComplete()); // HandlingInput is not the commit boundary.
  if (loseOwnerInInput) closeOwner();
  if (reloginInInput) liveSessions.emplace_back(ownerSource, ownerEpoch + 1);
  return CLI_MODE_HANDLED_AND_EXIT;
}
static void onExit(void*) {
  ++exitCalls;
  exitEligibility = cliModeExitCanComplete();
  if (exitEligibility) ++acceptedCompletions;
  const TaskHandle_t originalTask = currentTask;
  currentTask = &mainTask;
  otherTaskEligibility = cliModeExitCanComplete();
  currentTask = originalTask;
  const TaskHandle_t executor = gCmdExecTaskHandle;
  gCmdExecTaskHandle = nullptr;
  missingExecutorEligibility = cliModeExitCanComplete();
  gCmdExecTaskHandle = executor;
}
static void onTick(void*) {
  ++tickCalls;
  assert(!cliModeExitCanComplete());
  if (completeOnTick) assert(cliExitMode());
}
static const CLIMode mode{"completion-test", onEnter, onInput, onExit, onTick, nullptr, 25};

static void reset() {
  assert(!cliInModeActive());
  liveSessions.clear(); invocation = {}; currentContext = &invocation;
  currentTask = &executorTask; gCmdExecTaskHandle = &executorTask; nowUs = 0;
  inputCalls = tickCalls = exitCalls = acceptedCompletions = 0;
  loseOwnerInInput = reloginInInput = completeOnTick = false;
  exitEligibility = otherTaskEligibility = missingExecutorEligibility = false;
  assert(!cliModeExitCanComplete());
}
static void enter(CommandSource source = SOURCE_SERIAL, TransportSessionEpoch epoch = 11) {
  ownerSource = source; ownerEpoch = epoch;
  invocation.auth.transport = source;
  invocation.auth.user = "test-user";
  invocation.transportSessionEpoch = epoch;
  liveSessions.emplace_back(source, epoch);
  assert(cliEnterMode(&mode));
  assert(!cliModeExitCanComplete());
}
static void assertExited(bool accepted) {
  assert(exitCalls == 1 && exitEligibility == accepted);
  assert(acceptedCompletions == (accepted ? 1 : 0));
  assert(!otherTaskEligibility && !missingExecutorEligibility);
  assert(!cliInModeActive() && !cliModeExitCanComplete());
  ++completedScenarios;
}
static void finishSerial() {
  char out[80]{};
  assert(cliModeDispatchInput("next", out, sizeof(out)));
}
static void queueDisplayExit() {
  currentContext = nullptr; currentTask = &mainTask; completeOnTick = true;
  cliModeTick();
  assert(tickCalls == 1 && exitCalls == 0 && cliInModeActive());
  assert(!cliModeExitCanComplete());
  assert(!cliModeExecutorDrainPending()); // Main task cannot execute finalization.
}
static void drainExit() {
  currentTask = &executorTask;
  assert(!cliModeExitCanComplete()); // ExitPending still does not authorize Apply.
  assert(cliModeExecutorDrainPending());
}

int main() {
  // A live exact owner can complete; eligibility is false before/after onExit.
  reset(); enter(); finishSerial(); assert(inputCalls == 1); assertExited(true);

  // Loss while the final input runs must not be mistaken for successful Finish.
  reset(); enter(); loseOwnerInInput = true; finishSerial(); assertExited(false);
  reset(); enter(); loseOwnerInInput = reloginInInput = true;
  finishSerial(); assertExited(false); // A replacement login cannot inherit ownership.

  // Local display Finish is deferred to the executor, preserving the owner's identity.
  reset(); enter(SOURCE_LOCAL_DISPLAY); queueDisplayExit(); drainExit(); assertExited(true);
  reset(); enter(SOURCE_LOCAL_DISPLAY); queueDisplayExit(); closeOwner();
  drainExit(); assertExited(false);
  reset(); enter(SOURCE_LOCAL_DISPLAY); queueDisplayExit(); closeOwner();
  liveSessions.emplace_back(SOURCE_WEB, ownerEpoch); // Same number, different transport.
  drainExit(); assertExited(false);

  // The deadline is rechecked both for ordinary timeout and queued completion.
  reset(); enter(); nowUs = 25000;
  assert(cliModeExecutorDrainPending()); assertExited(false);
  reset(); enter(SOURCE_LOCAL_DISPLAY); queueDisplayExit(); nowUs = 25000;
  drainExit(); assertExited(false);

  // Unrelated commands cannot become the owning wizard's final answer.
  reset(); enter(); invocation.auth.transport = SOURCE_WEB;
  liveSessions.emplace_back(SOURCE_WEB, ownerEpoch);
  char out[80]{};
  assert(!cliModeDispatchInput("next", out, sizeof(out)) && inputCalls == 0);
  invocation.auth.transport = ownerSource; finishSerial(); assertExited(true);

  assert(completedScenarios == 9);
  std::printf("CLI mode completion owner contracts passed: %d lifecycle scenarios\n", completedScenarios);
}
