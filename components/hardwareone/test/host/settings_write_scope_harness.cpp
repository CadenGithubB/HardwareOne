// Real scope + batch adapter are inserted by test_settings_write_scope.py.
#include <Arduino.h>
#include "System_SettingsWriteBatchCore.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <type_traits>

void webBatchTestStringAppend() {}
struct AuthContext { uint8_t transport = 0; String user, sid, ip; };
struct CommandContext {
  uint32_t settingsBatchId = 0, transportSessionEpoch = 0;
  uint8_t origin = 0;
  AuthContext auth;
  const char* automationName = nullptr;
};
struct Task { void* slots[4]{}; CommandContext* command = nullptr; };
using TaskHandle_t = Task*;
using BaseType_t = int;
using TickType_t = uint32_t;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
static constexpr uint32_t portMAX_DELAY = UINT32_MAX;
static constexpr int pdTRUE = 1;
static thread_local Task* currentTask;
static thread_local unsigned criticalDepth;
static TaskHandle_t xTaskGetCurrentTaskHandle() { return currentTask; }
static void* pvTaskGetThreadLocalStoragePointer(Task* task, int slot) {
  if (!task || slot >= configNUM_THREAD_LOCAL_STORAGE_POINTERS) std::abort();
  return task->slots[slot];
}
static void vTaskSetThreadLocalStoragePointer(Task* task, int slot, void* value) {
  if (!task || slot >= configNUM_THREAD_LOCAL_STORAGE_POINTERS) std::abort();
  task->slots[slot] = value;
}
static void* currentCommandContext() { return currentTask ? currentTask->command : nullptr; }
static std::atomic<uint32_t> clockMs{1000};
static uint32_t millis() { return clockMs; }
static void portENTER_CRITICAL(std::mutex* mutex) { mutex->lock(); ++criticalDepth; }
static void portEXIT_CRITICAL(std::mutex* mutex) { --criticalDepth; mutex->unlock(); }
struct StaticSemaphore_t { std::recursive_mutex mutex; };
using SemaphoreHandle_t = StaticSemaphore_t*;
static bool writerLockAvailable = true;
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* storage) { return storage; }
static int xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t wait) {
  if (!writerLockAvailable) return 0;
  if (wait == 0) return semaphore->mutex.try_lock() ? 1 : 0;
  semaphore->mutex.lock(); return 1;
}
static void xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore) { semaphore->mutex.unlock(); }
static unsigned scopeErrors;
#define ERROR_STORAGEF(...) ((void)++scopeErrors)
#define WARN_STORAGEF(...) ((void)0)
static std::atomic<unsigned> mainWrites{0}, debugWrites{0};
static bool mainSucceeds = true, debugSucceeds = true;
static bool writeSettingsJson();
static bool writeDebugJson();
uint32_t allocateSettingsWriteBatchId();
// INSERT_SCOPE_CLASS
// INSERT_ADAPTER
static bool writeSettingsJson() {
  if (criticalDepth) std::abort();
  ++mainWrites;
  if (mainSucceeds) clearSettingsWriteRetry(hw1_settings_batch::MAIN);
  return mainSucceeds;
}
static bool writeDebugJson() {
  if (criticalDepth) std::abort();
  ++debugWrites;
  if (debugSucceeds) clearSettingsWriteRetry(hw1_settings_batch::DEBUG);
  return debugSucceeds;
}
static unsigned checks;
static void check(bool good, const char* message) {
  ++checks;
  if (!good) { std::fprintf(stderr, "scope: %s\n", message); std::abort(); }
}
static void reset(Task& task) {
  task = {}; currentTask = &task;
  sSettingsWriteBatches = {};
  sSettingsWritePendingRetryMask = 0;
  sSettingsWriteRetryNotBeforeMs = 0;
  mainWrites = debugWrites = 0;
  mainSucceeds = debugSucceeds = writerLockAvailable = true;
  clockMs += 200000;
}
static SettingsBatchOwner scopeOwner() {
  SettingsBatchOwner owner{};
  check(currentSettingsBatchOwner(owner), "active scope missing persistence owner");
  return owner;
}
static void coalescingAndIsolation(Task& task) {
  reset(task);
  check(requestSettingsPersist() && mainWrites == 1, "ordinary background write suppressed");
  mainWrites = 0;
  {
    SettingsWriteScope scope;
    check(scope.active(), "scope refused available task/batch slot");
    for (unsigned i = 0; i < 100; ++i) check(requestSettingsPersist(), "deferred main refused");
    for (unsigned i = 0; i < 8; ++i) check(requestDebugSettingsPersist(), "deferred debug refused");
    check(mainWrites == 0 && debugWrites == 0, "scope wrote before finish");
    Task other;
    currentTask = &other;
    check(!scope.finish() && scope.active(), "foreign task finished another task's scope");
    check(requestSettingsPersist() && mainWrites == 1, "foreign background write suppressed");
    CommandContext foreignCommand; foreignCommand.transportSessionEpoch = 99;
    other.command = &foreignCommand;
    check(requestDebugSettingsPersist() && debugWrites == 1, "foreign command write suppressed");
    currentTask = &task;
    check(scope.finish(), "complete scope refused finish");
    check(mainWrites == 2 && debugWrites == 2, "changed files did not flush once");
    check(scope.finish() && mainWrites == 2 && debugWrites == 2, "repeat finish rewrote files");
    check(!scope.active() && task.slots[3] == nullptr, "finish retained task override");
  }
  reset(task);
  { SettingsWriteScope unchanged; check(unchanged.active(), "empty scope inactive"); }
  check(mainWrites == 0 && debugWrites == 0, "empty scope wrote files");
  { SettingsWriteScope earlyReturn; check(requestSettingsPersist(), "destructor fixture refused write"); }
  check(mainWrites == 1 && task.slots[3] == nullptr, "destructor did not flush/reset scope");
}
static void nestingAndCommandOwner(Task& task) {
  reset(task);
  SettingsWriteScope outer;
  const auto owner = scopeOwner();
  void* outerTls = task.slots[3];
  check(requestSettingsPersist(), "outer note refused");
  {
    SettingsWriteScope inner;
    check(inner.active() && scopeOwner() == owner, "nested scope changed owner");
    check(!outer.finish() && outer.active(), "out-of-order finish consumed outer scope");
    check(requestDebugSettingsPersist(), "inner note refused");
    check(inner.finish() && mainWrites == 0 && debugWrites == 0, "nested finish wrote early");
    check(task.slots[3] == outerTls, "nested finish did not restore outer scope");
  }
  check(outer.finish() && mainWrites == 1 && debugWrites == 1, "outer finish did not write both once");
  reset(task);
  CommandContext command; command.settingsBatchId = allocateSettingsWriteBatchId(); task.command = &command;
  const auto commandOwner = scopeOwner();
  check(beginSettingsWriteBatch(commandOwner), "command batch begin failed");
  check(requestSettingsPersist() && mainWrites == 0, "command batching changed");
  {
    SettingsWriteScope wizard;
    check(wizard.active() && scopeOwner() != commandOwner, "wizard inherited pre-existing command batch");
    check(requestDebugSettingsPersist(), "wizard debug note refused");
    check(wizard.finish() && mainWrites == 0 && debugWrites == 1, "wizard did not finish its own files");
  }
  check(scopeOwner() == commandOwner, "wizard did not restore command owner");
  const auto saved = finishSettingsWriteBatch(commandOwner);
  check(saved.found && saved.final && saved.dirtyMask == hw1_settings_batch::MAIN, "wizard consumed caller's batch");
  check(flushSettingsDirtyMask(commandOwner, saved.dirtyMask) && mainWrites == 1, "caller batch no longer finishes");
}
static void refusalsAndRetry(Task& task) {
  reset(task);
  currentTask = nullptr;
  { SettingsWriteScope scope; check(!scope.active() && !scope.finish(), "pre-scheduler scope accepted"); }
  currentTask = &task;
  for (uint64_t i = 0; i < 16; ++i) check(beginSettingsWriteBatch({11, i}), "slot fixture failed");
  { SettingsWriteScope scope; check(!scope.active() && !scope.finish(), "full table accepted scope"); }
  check(task.slots[3] == nullptr && mainWrites == 0 && debugWrites == 0, "refusal changed TLS or wrote");
  reset(task);
  for (uint8_t failed : {hw1_settings_batch::MAIN, hw1_settings_batch::DEBUG, hw1_settings_batch::ALL}) {
    mainWrites = debugWrites = 0;
    SettingsWriteScope scope;
    check(scope.active(), "retry scope begin failed");
    check(requestSettingsPersist() && requestDebugSettingsPersist(), "retry note failed");
    mainSucceeds = !(failed & hw1_settings_batch::MAIN);
    debugSucceeds = !(failed & hw1_settings_batch::DEBUG);
    check(!scope.finish() && !scope.active(), "partial flush claimed success");
    check(task.slots[3] == nullptr, "failed flush left task override installed");
    check(!scope.finish() && mainWrites == 1 && debugWrites == 1, "repeat failed finish duplicated writes");
    mainSucceeds = debugSucceeds = true;
    clockMs += 130000;
    settingsWriteBatchTick();
    check(mainWrites == (failed & hw1_settings_batch::MAIN ? 2u : 1u), "main retry missing or successful file repeated");
    check(debugWrites == (failed & hw1_settings_batch::DEBUG ? 2u : 1u), "debug retry missing or successful file repeated");
  }
  reset(task);
  {
    SettingsWriteScope scope;
    check(requestSettingsPersist(), "lock-failure note refused");
    writerLockAvailable = false;
    check(!scope.finish() && mainWrites == 0, "writer lock failure not reported");
  }
  writerLockAvailable = true; clockMs += 130000; settingsWriteBatchTick();
  check(mainWrites == 1, "lock failure lost retry");
  reset(task);
  {
    SettingsWriteScope scope;
    check(requestSettingsPersist(), "expiry note refused");
    clockMs += 130000;
    settingsWriteBatchTick();
    check(mainWrites == 1 && debugWrites == 0, "expired scope did not flush its dirty file");
    check(scope.finish() && mainWrites == 2 && debugWrites == 1,
          "expired scope finish did not confirm durability conservatively");
    check(task.slots[3] == nullptr, "expired scope retained task override");
  }
}
static void concurrentTasks(Task& task) {
  reset(task);
  auto worker = []() {
    Task self; currentTask = &self;
    SettingsWriteScope scope;
    if (!scope.active()) std::abort();
    for (int i = 0; i < 200; ++i) {
      if (!requestSettingsPersist() || !requestDebugSettingsPersist()) std::abort();
    }
    if (!scope.finish() || self.slots[3]) std::abort();
  };
  std::thread one(worker), two(worker);
  one.join(); two.join();
  check(mainWrites == 2 && debugWrites == 2, "concurrent task scopes mixed owners or lost files");
}
// A scope destroyed while its finish is refused (outer destroyed before inner,
// or on a foreign task) is diagnosed; its batch closes with the pending files
// flushed and the task's scope pointer is cleared instead of left dangling.
static void outOfOrderDestruction(Task& task) {
  reset(task); scopeErrors = 0;
  auto* outer = new SettingsWriteScope;
  check(outer->active() && requestSettingsPersist(), "heap outer scope refused");
  {
    SettingsWriteScope inner;
    check(inner.active() && !outer->finish() && outer->active(), "out-of-order finish was not refused");
    delete outer;  // refused finish inside the destructor
    check(scopeErrors == 1 && task.slots[3] == nullptr, "out-of-order destruction not diagnosed or left the scope pointer dangling");
    check(mainWrites == 0, "closing the outer level of a nested batch wrote early");
    check(!inner.finish() && inner.active(), "orphaned inner scope claimed a finish it could not perform");
  }
  check(scopeErrors == 2 && task.slots[3] == nullptr, "orphaned inner destruction not diagnosed or left the scope pointer dangling");
  check(mainWrites == 1 && debugWrites == 0, "orphaned batch did not flush its pending file once");
  check(requestSettingsPersist() && mainWrites == 2, "task no longer accepts ordinary writes after the diagnosis");
  reset(task); scopeErrors = 0;
  auto* foreign = new SettingsWriteScope;
  check(foreign->active() && requestSettingsPersist(), "foreign-task fixture refused");
  Task other; currentTask = &other;
  delete foreign;  // destroyed on another task: cannot touch that task's TLS
  currentTask = &task;
  check(scopeErrors == 1 && task.slots[3] != nullptr && mainWrites == 0, "foreign-task destruction touched the owning task or wrote");
  clockMs += 130000; settingsWriteBatchTick();
  check(mainWrites == 1, "expired orphaned batch did not flush");
  task.slots[3] = nullptr;
}
int main() {
  static_assert(!std::is_copy_constructible<SettingsWriteScope>::value, "scope must not copy TLS ownership");
  static_assert(!std::is_move_constructible<SettingsWriteScope>::value, "scope must not move TLS ownership");
  Task task;
#if configNUM_THREAD_LOCAL_STORAGE_POINTERS > 3
  coalescingAndIsolation(task);
  nestingAndCommandOwner(task);
  outOfOrderDestruction(task);
  refusalsAndRetry(task);
  concurrentTasks(task);
#else
  reset(task);
  SettingsWriteScope scope;
  check(!scope.active() && !scope.finish(), "insufficient TLS capacity accepted scope");
  check(requestSettingsPersist() && mainWrites == 1, "unavailable scope suppressed background write");
#endif
  std::printf("settings write scope: %u checks passed (TLS slots=%d)\n", checks, configNUM_THREAD_LOCAL_STORAGE_POINTERS);
}
