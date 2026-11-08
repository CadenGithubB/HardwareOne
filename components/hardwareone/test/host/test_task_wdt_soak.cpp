// System_TaskWdt.h against a fake tick clock and watchdog.
//
// Built twice: task_wdt_soak_tests with HW1_WDT_SOAK=1 (what
// tools/build_board.sh --wdt-soak compiles) and task_wdt_default_tests without
// it (every ordinary build). The default build must make no watchdog call and
// keep the single 60 s wait; the soak build must keep the same total timeout
// while feeding a subscribed caller at least once a second.

#include "System_TaskWdt.h"

// The fakes below exist in both variants; the default build must not call them.
#include "esp_task_wdt.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

struct FakeSemaphore {
  bool everGiven = false;
  TickType_t givenAt = 0;
};

namespace {
TickType_t gNow = 0;
bool gSubscribed = false;
int gAdds = 0;
int gStatusCalls = 0;
std::vector<TickType_t> gResets;
std::vector<TickType_t> gWaits;
int gFailures = 0;

void reset(bool subscribed) {
  gNow = 1000;  // a non-zero start so elapsed time is measured, not assumed
  gSubscribed = subscribed;
  gAdds = 0;
  gStatusCalls = 0;
  gResets.clear();
  gWaits.clear();
}

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++gFailures;
  }
}

[[maybe_unused]] TickType_t maxWait() {
  TickType_t m = 0;
  for (TickType_t w : gWaits) m = w > m ? w : m;
  return m;
}

// Longest stretch without a feed, from the start of the wait to its end.
[[maybe_unused]] TickType_t longestUnfed(TickType_t start, TickType_t end) {
  TickType_t last = start, worst = 0;
  for (TickType_t t : gResets) {
    worst = t - last > worst ? t - last : worst;
    last = t;
  }
  return end - last > worst ? end - last : worst;
}
}  // namespace

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
  gWaits.push_back(wait);
  if (s->everGiven && s->givenAt <= gNow + wait) {
    if (s->givenAt > gNow) gNow = s->givenAt;
    return pdTRUE;
  }
  gNow += wait;
  return pdFALSE;
}

TickType_t xTaskGetTickCount(void) { return gNow; }

esp_err_t esp_task_wdt_add(void* task) {
  check(task == nullptr, "only the calling task is subscribed");
  ++gAdds;
  gSubscribed = true;
  return ESP_OK;
}

esp_err_t esp_task_wdt_status(void* task) {
  check(task == nullptr, "status asks about the calling task");
  ++gStatusCalls;
  return gSubscribed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t esp_task_wdt_reset(void) {
  check(gSubscribed, "reset only from a subscribed task");
  gResets.push_back(gNow);
  return ESP_OK;
}

#if HW1_WDT_SOAK

static void soakTests() {
  FakeSemaphore never;
  FakeSemaphore later;
  later.everGiven = true;
  later.givenAt = 1000 + 12345;

  // A subscribed main loop waiting for a command that never finishes: the
  // full 60 s still elapses, and it is never more than a second unfed.
  reset(true);
  check(taskWdtAwareSemaphoreTake(&never, 60000) == pdFALSE, "timeout still reported");
  check(gNow - 1000 == 60000, "total wait unchanged at 60 s");
  check(maxWait() <= kTaskWdtWaitSliceMs, "no slice longer than 1 s");
  check(longestUnfed(1000, gNow) <= kTaskWdtWaitSliceMs, "fed at least once a second");
  check(gResets.size() == 59, "one feed between each pair of slices");

  // A command that finishes after 12.3 s: returns at once, fed meanwhile.
  reset(true);
  check(taskWdtAwareSemaphoreTake(&later, 60000) == pdTRUE, "completion reported");
  check(gNow == later.givenAt, "returns when the command completes");
  check(gResets.size() == 12, "fed once per elapsed second");

  // Shorter than one slice, and zero: one plain wait, no feed.
  reset(true);
  check(taskWdtAwareSemaphoreTake(&never, 500) == pdFALSE, "short timeout reported");
  check(gWaits.size() == 1 && gWaits[0] == 500 && gResets.empty(), "short wait is one take");
  reset(true);
  check(taskWdtAwareSemaphoreTake(&never, 0) == pdFALSE, "zero timeout reported");
  check(gWaits.size() == 1 && gWaits[0] == 0 && gResets.empty(), "zero wait polls once");

  // Not subscribed (httpd, cmd_exec, any other task): unchanged single wait.
  reset(false);
  check(taskWdtAwareSemaphoreTake(&never, 60000) == pdFALSE, "unsubscribed timeout");
  check(gWaits.size() == 1 && gWaits[0] == 60000, "unsubscribed caller keeps one 60 s wait");
  check(gResets.empty(), "unsubscribed caller is never fed");

  // Feed and subscribe.
  reset(false);
  taskWdtFeed();
  check(gResets.empty(), "feed is a no-op for an unsubscribed task");
  check(taskWdtSubscribeCurrent(), "subscribe reports success");
  check(gAdds == 1, "subscribe adds the calling task");
  taskWdtFeed();
  check(gResets.size() == 1, "feed resets a subscribed task");
}

#else

static void defaultTests() {
  FakeSemaphore never;
  reset(true);  // even if something else subscribed the task
  check(!taskWdtSubscribeCurrent(), "default build never subscribes");
  taskWdtFeed();
  check(taskWdtAwareSemaphoreTake(&never, 60000) == pdFALSE, "default timeout reported");
  check(gWaits.size() == 1 && gWaits[0] == 60000, "default build keeps one 60 s wait");
  check(gAdds == 0 && gStatusCalls == 0 && gResets.empty(),
        "default build makes no task-watchdog call");
}

#endif

int main() {
#if HW1_WDT_SOAK
  soakTests();
#else
  defaultTests();
#endif
  if (gFailures) return EXIT_FAILURE;
  std::printf("task watchdog %s helpers: ok\n", HW1_WDT_SOAK ? "soak" : "default");
  return EXIT_SUCCESS;
}
