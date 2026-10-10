// test_g2_power_completion.py compiles the complete production Power page,
// redraw helper, message types and worker dispatch beside these boundaries.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <new>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::abort(); } } while (false)

// Fault injection and ownership accounting cover production's two nothrow
// allocations. The harness uses fixed arrays, avoiding extra heap traffic.
static void* owned[32]{};
static unsigned allocationAttempts = 0, failAllocationAt = 0, liveAllocations = 0;
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  ++allocationAttempts;
  if (allocationAttempts == failAllocationAt) return nullptr;
  void* pointer = std::malloc(size);
  CHECK(pointer != nullptr);
  for (auto& slot : owned) if (!slot) {
    slot = pointer; ++liveAllocations; return pointer;
  }
  std::abort();
}
void operator delete(void* pointer) noexcept {
  if (!pointer) return;
  for (auto& slot : owned) if (slot == pointer) {
    slot = nullptr; --liveAllocations; break;
  }
  std::free(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
#endif

#define ENABLE_BLUETOOTH 1
#define ENABLE_G2_GLASSES 1
#define EXT_RAM_BSS_ATTR
template <typename... Args> void quietLog(const char*, Args...) {}
#define DEBUG_G2F(...) quietLog(__VA_ARGS__)
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
static unsigned criticalDepth = 0;
#define portENTER_CRITICAL(m) ((m)->lock(), ++criticalDepth)
#define portEXIT_CRITICAL(m) (--criticalDepth, (m)->unlock())

struct PageSwapArgs;
struct ToastSpec;
struct NotifySpec;
struct CustomSpec;
struct NativeNotifSpec;
// INSERT_TYPES

static uint32_t menuGen = 1;
static uint64_t commandSeq = 0;
static G2HijackPage page = G2_HIJACK_PAGE_MAIN;
static unsigned listsShown = 0, textsShown = 0, delays = 0, delayedMs = 0;
static bool insideCallback = false, commandAccepts = true, queueAccepts = true;
static char shownRows[8][80]{};
static size_t shownCount = 0;
static char shownText[80]{};

uint32_t g2CurrentMenuGen() { return menuGen; }
uint64_t g2CurrentCmdSeq() { return commandSeq; }
void g2BumpMenuGen() { ++menuGen; }
G2HijackPage g2GetHijackPage() { return page; }
void g2SetHijackPage(G2HijackPage value) {
  CHECK(!insideCallback);
  if (value != page) g2BumpMenuGen();
  page = value;
}
bool g2ShowListPage(const char* const* rows, size_t count) {
  CHECK(!insideCallback);
  CHECK(criticalDepth == 0);
  CHECK(count <= 8);
  shownCount = count;
  for (size_t i = 0; i < count; ++i)
    std::snprintf(shownRows[i], sizeof(shownRows[i]), "%s", rows[i]);
  ++listsShown;
  return true;
}
void g2ShowText(const char* text) {
  CHECK(!insideCallback);
  CHECK(criticalDepth == 0);
  std::snprintf(shownText, sizeof(shownText), "%s", text);
  ++textsShown;
}
void g2RedrawHijackMainMenu() { ++listsShown; shownCount = 0; }
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned ms) {
  CHECK(!insideCallback);
  CHECK(criticalDepth == 0);
  ++delays; delayedMs += ms;
}

enum { POWER_MODE_PERFORMANCE, POWER_MODE_BALANCED, POWER_MODE_POWERSAVER,
       POWER_MODE_ULTRASAVER, POWER_MODE_LOCKED, POWER_MODE_COUNT };
struct { uint8_t powerMode = POWER_MODE_BALANCED; } gSettings;
const char* getPowerModeName(uint8_t mode) {
  static const char* names[] = {"Performance", "Balanced", "PowerSaver", "UltraSaver", "Locked"};
  CHECK(mode < POWER_MODE_COUNT);
  return names[mode];
}
uint32_t getCpuFrequencyMhz() { return 240; }
uint32_t getPowerModeActiveCpuFreq(uint8_t) { return 240; }
uint32_t getPowerModeIdleCpuFreq(uint8_t mode) { return mode == POWER_MODE_ULTRASAVER ? 40 : 240; }

using Callback = void (*)(bool, const char*, const G2CmdCookie&, void*);
struct Command {
  char line[80];
  G2CmdCookie cookie;
  Callback callback;
  void* userData;
};
static Command commands[16]{};
static size_t commandCount = 0;
static unsigned submitCalls = 0;
bool g2SubmitHijackCommand(const char* line, const G2CmdCookie& cookie,
                           Callback callback, void* userData) {
  CHECK(!insideCallback);
  CHECK(criticalDepth == 0);
  ++submitCalls;
  if (!commandAccepts) return false;
  CHECK(commandCount < 16);
  Command& command = commands[commandCount++];
  std::snprintf(command.line, sizeof(command.line), "%s", line);
  command.cookie = cookie;
  command.cookie.menuGen = menuGen;
  command.cookie.seq = ++commandSeq;
  command.callback = callback;
  command.userData = userData;
  return true;
}

static LensUiJob* jobs[16]{};
static size_t jobCount = 0;
static unsigned queueCalls = 0;
bool g2EnqueueLensJob(LensUiJob* job, G2LensEnqueueWait wait) {
  CHECK(criticalDepth == 0);
  CHECK(wait == G2LensEnqueueWait::Bounded50Ms);
  CHECK(job != nullptr && job->kind == LensJobKind::Redraw);
  CHECK(job->payload.redraw && job->payload.redraw->render);
  ++queueCalls;
  if (!queueAccepts) return false;
  CHECK(jobCount < 16);
  jobs[jobCount++] = job;
  return true;
}

// INSERT_REDRAW_HELPER
// INSERT_PRODUCTION

static void drainUi() {
  while (jobCount) {
    LensUiJob* job = jobs[0];
    for (size_t i = 1; i < jobCount; ++i) jobs[i - 1] = jobs[i];
    --jobCount;
    const uint32_t liveGen = g2CurrentMenuGen();
    switch (job->kind) {
      // INSERT_REDRAW_DISPATCH
      default: std::abort();
    }
    CHECK(job->payload.redraw == nullptr);
    delete job;
  }
  CHECK(liveAllocations == 0);
}

static Command takeCommand() {
  CHECK(commandCount != 0);
  Command command = commands[0];
  for (size_t i = 1; i < commandCount; ++i) commands[i - 1] = commands[i];
  --commandCount;
  return command;
}

static void complete(const Command& command, bool okay, const char* result) {
  const unsigned oldLists = listsShown, oldTexts = textsShown, oldDelays = delays;
  insideCallback = true;
  command.callback(okay, result, command.cookie, command.userData);
  insideCallback = false;
  CHECK(listsShown == oldLists && textsShown == oldTexts && delays == oldDelays);
}

static bool rowHas(size_t row, const char* text) {
  return row < shownCount && std::strstr(shownRows[row], text) != nullptr;
}

static void home() {
  drainUi();
  CHECK(commandCount == 0);
  commandAccepts = queueAccepts = true;
  failAllocationAt = allocationAttempts = queueCalls = 0;
  page = G2_HIJACK_PAGE_MAIN;
  ++menuGen;
  gSettings.powerMode = POWER_MODE_BALANCED;
  g2ShowPowerMenu();
  CHECK(page == G2_HIJACK_PAGE_POWER);
  CHECK(shownCount == 5 && rowHas(0, "Main Menu"));
}

static void cpu() {
  g2PowerHandleTap(1);
  CHECK(shownCount == 1 + POWER_MODE_COUNT && rowHas(0, "Back"));
}

static Command submitPreset(unsigned mode = POWER_MODE_PERFORMANCE) {
  g2PowerHandleTap(1 + mode);
  Command command = takeCommand();
  CHECK(command.cookie.targetPage == G2_HIJACK_PAGE_POWER);
  CHECK(command.cookie.menuGen == menuGen);
  CHECK(command.cookie.seq == commandSeq);
  return command;
}

static void checkCookie(const Command& command) {
  CHECK(jobCount == 1);
  CHECK(jobs[0]->submitMenuGen == command.cookie.menuGen);
  CHECK(jobs[0]->cmdSeq == command.cookie.seq);
  CHECK(jobs[0]->targetPage == command.cookie.targetPage);
  CHECK(jobs[0]->targetNetSub == command.cookie.targetNetSub);
}

static void testPresetCompletion() {
  home(); cpu();
  Command command = submitPreset();
  CHECK(std::strcmp(command.line, "power mode perf") == 0);
  const unsigned before = listsShown;
  // The service applied its setting before invoking the completion.
  gSettings.powerMode = POWER_MODE_PERFORMANCE;
  complete(command, true, "OK: Performance mode");
  checkCookie(command);
  CHECK(listsShown == before);
  drainUi();
  CHECK(listsShown == before + 1);
  CHECK(shownCount == 1 + POWER_MODE_COUNT && rowHas(1, "[X]"));
  CHECK(rowHas(0, "Back"));

  // The executor's result buffer is borrowed only for the callback duration.
  command = submitPreset(POWER_MODE_ULTRASAVER);
  CHECK(std::strcmp(command.line, "power mode ultra") == 0);
  char reply[] = "Error: Permission denied";
  complete(command, false, reply);
  checkCookie(command);
  std::memset(reply, 'X', sizeof(reply) - 1);
  drainUi();
  CHECK(rowHas(0, "Permission denied"));
  CHECK(!rowHas(0, "Error:") && !rowHas(0, "XXXX"));
  CHECK(shownCount == 1 + POWER_MODE_COUNT && rowHas(1, "[X]"));

  command = submitPreset(POWER_MODE_BALANCED);
  gSettings.powerMode = POWER_MODE_BALANCED;
  complete(command, true, "OK: Balanced");
  drainUi();
  CHECK(rowHas(0, "Back") && rowHas(2, "[X]"));

  // Errors in the textual result stay errors even if command lookup returned
  // true. Empty false replies still provide a useful refusal row.
  for (const char* result : {"Error: denied", "ERROR: denied", "operation refused", ""}) {
    command = submitPreset();
    complete(command, result[0] != '\0', result);
    drainUi();
    CHECK(!rowHas(0, "Back"));
    CHECK(shownRows[0][3] != '\0');
  }
  command = submitPreset();
  complete(command, false, nullptr);
  drainUi();
  CHECK(!rowHas(0, "Back") && shownRows[0][3] != '\0');
}

static void testDestructiveCompletions() {
  static const char* lines[] = {"reboot", "ramflush", "deepsleep"};
  for (unsigned action = 0; action < 3; ++action) {
    home();
    g2PowerHandleTap(2 + action);
    CHECK(shownCount == 2 && rowHas(0, "Cancel") && rowHas(1, "Confirm"));
    const unsigned oldDelays = delays, oldMs = delayedMs;
    g2PowerHandleTap(1);
    Command command = takeCommand();
    CHECK(std::strcmp(command.line, lines[action]) == 0);
    // Preserve the brief pending-notification pause before reboot/sleep.
    CHECK(delays == oldDelays + 1 && delayedMs == oldMs + 150);
    const unsigned oldSubmits = submitCalls;
    g2PowerHandleTap(1);  // A duplicate confirm must never default to reboot.
    CHECK(submitCalls == oldSubmits && commandCount == 0);
    CHECK(delays == oldDelays + 1 && delayedMs == oldMs + 150);
    complete(command, false, "Error: Not permitted");
    checkCookie(command);
    drainUi();
    CHECK(shownCount == 5 && rowHas(0, "Not permitted"));
    CHECK(rowHas(2, "Restart") && rowHas(4, "Power Off"));
  }

  home();
  g2PowerHandleTap(4); g2PowerHandleTap(1);
  Command command = takeCommand();
  // Deep-sleep cooldown historically returns non-Error text.
  complete(command, true, "Sleep refused");
  drainUi();
  CHECK(shownCount == 5 && rowHas(0, "refused"));

  home();
  g2PowerHandleTap(2); g2PowerHandleTap(1);
  command = takeCommand();
  const unsigned before = listsShown;
  complete(command, true, "OK: Restarting");
  CHECK(jobCount == 0 && listsShown == before);
}

static void leaveAndReopenCpu() {
  g2PowerHandleTap(0);  // CPU -> Actions.
  g2PowerHandleTap(0);  // Actions -> Main.
  CHECK(page == G2_HIJACK_PAGE_MAIN);
  g2ShowPowerMenu(); cpu();
}

static void testNavigationStaleness() {
  home(); cpu();
  Command command = submitPreset();
  leaveAndReopenCpu();
  CHECK(menuGen != command.cookie.menuGen);
  unsigned before = listsShown;
  complete(command, false, "Error: old view");
  drainUi();
  CHECK(listsShown == before && rowHas(0, "Back"));

  // A queued redraw becomes stale if navigation occurs before dispatch.
  command = submitPreset();
  complete(command, false, "Error: queued old view");
  CHECK(jobCount == 1);
  leaveAndReopenCpu();
  before = listsShown;
  drainUi();
  CHECK(listsShown == before && rowHas(0, "Back"));

  // These subviews all have the same G2 page ID. Their navigation still
  // invalidates an old CPU result, including when the user returns to CPU.
  command = submitPreset();
  const G2HijackPage unchangedPage = page;
  g2PowerHandleTap(0); cpu();
  CHECK(page == unchangedPage && menuGen != command.cookie.menuGen);
  before = listsShown;
  complete(command, false, "Error: old CPU picker");
  drainUi();
  CHECK(listsShown == before && rowHas(0, "Back"));

  // A failed destructive operation also cannot reopen Power after leaving.
  home();
  g2PowerHandleTap(4); g2PowerHandleTap(1);
  command = takeCommand();
  g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
  before = listsShown;
  complete(command, false, "Error: old shutdown");
  drainUi();
  CHECK(page == G2_HIJACK_PAGE_MAIN && listsShown == before);

  // Navigating between confirmation and actions invalidates the command
  // without requiring any change to the outer page.
  home();
  g2PowerHandleTap(2); g2PowerHandleTap(1);
  command = takeCommand();
  g2PowerHandleTap(0);
  CHECK(shownCount == 5);
  before = listsShown;
  complete(command, false, "Error: cancelled view");
  drainUi();
  CHECK(shownCount == 5 && listsShown == before && rowHas(0, "Main Menu"));
}

static void testNewerRequestsWin() {
  home(); cpu();
  Command older = submitPreset(POWER_MODE_PERFORMANCE);
  Command newer = submitPreset(POWER_MODE_BALANCED);
  CHECK(older.cookie.menuGen != newer.cookie.menuGen);
  unsigned before = listsShown;
  complete(older, false, "Error: old request");
  drainUi();
  CHECK(listsShown == before);
  complete(newer, false, "Error: New refusal");
  drainUi();
  CHECK(rowHas(0, "New refusal"));

  older = submitPreset(POWER_MODE_PERFORMANCE);
  complete(older, false, "Error: queued old");
  CHECK(jobCount == 1);
  newer = submitPreset(POWER_MODE_LOCKED);
  complete(newer, false, "Error: Newest refusal");
  before = listsShown;
  drainUi();
  CHECK(listsShown == before + 1 && rowHas(0, "Newest refusal"));

  // A delayed old callback cannot overwrite a newer result already waiting
  // for the display worker.
  older = submitPreset(); newer = submitPreset(POWER_MODE_BALANCED);
  complete(newer, false, "Error: Current result");
  complete(older, false, "Error: old delayed");
  drainUi();
  CHECK(rowHas(0, "Current result"));
}

static void testRedrawAdmissionFailures() {
  for (unsigned failure = 1; failure <= 3; ++failure) {
    home(); cpu();
    Command command = submitPreset();
    if (failure <= 2) failAllocationAt = allocationAttempts + failure;
    else queueAccepts = false;
    const unsigned before = listsShown;
    complete(command, false, "Error: Dropped result");
    CHECK(jobCount == 0 && liveAllocations == 0);
    CHECK(listsShown == before && rowHas(0, "Back"));

    // A later successful submission remains usable after any failure.
    failAllocationAt = 0; queueAccepts = true;
    command = submitPreset(POWER_MODE_LOCKED);
    gSettings.powerMode = POWER_MODE_LOCKED;
    complete(command, true, "OK: Locked");
    checkCookie(command);
    drainUi();
    CHECK(rowHas(0, "Back") && rowHas(5, "[X]"));
  }
}

static void testSubmissionRefusal() {
  home(); cpu();
  commandAccepts = false;
  const unsigned oldDelays = delays, oldSubmits = submitCalls;
  g2PowerHandleTap(1);
  CHECK(submitCalls == oldSubmits + 1 && commandCount == 0);
  CHECK(jobCount == 0 && delays == oldDelays);
  CHECK(shownCount == 1 + POWER_MODE_COUNT && rowHas(0, "Busy"));
  commandAccepts = true;
  Command command = submitPreset();
  complete(command, true, "OK: Performance");
  drainUi();
  CHECK(rowHas(0, "Back"));

  home();
  g2PowerHandleTap(2);
  commandAccepts = false;
  const unsigned before = delayedMs;
  g2PowerHandleTap(1);
  CHECK(commandCount == 0 && jobCount == 0);
  CHECK(delayedMs == before + 150);  // No timed error-banner delay.
  CHECK(shownCount == 5 && rowHas(0, "Busy"));
}

int main() {
  testPresetCompletion();
  testDestructiveCompletions();
  testNavigationStaleness();
  testNewerRequestsWin();
  testRedrawAdmissionFailures();
  testSubmissionRefusal();
  drainUi();
  CHECK(commandCount == 0 && liveAllocations == 0 && criticalDepth == 0);
  std::printf("G2 production Power completion/dispatch: %u checks passed\n", checks);
}
