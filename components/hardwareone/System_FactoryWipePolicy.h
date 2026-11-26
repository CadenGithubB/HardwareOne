#pragma once
#include <stdint.h>

// ============================================================================
// System_FactoryWipePolicy — the order of the handover wipe (`factoryreset all`)
// ============================================================================
// Pure decision code: no IDF or Arduino types, so a host test can run it on a
// fake (test_factory_wipe_policy.cpp, archived 2026-11-26). System_FactoryWipe.cpp
// supplies the real flash and NVS operations.
//
// The request is one NVS value that survives the restart. The filesystem is
// erased first and NVS, which holds the request, last, so a wipe that loses
// power starts again at the next boot instead of stopping half way. The NVS
// entries are deleted through the NVS API before the partition is erased:
// should the erase stop after the page holding the request is gone, no
// credential is still readable in the pages that are left.

// Boots that may start the wipe before the request is dropped. A wipe that
// crashes the device every time must not hold it in a restart loop.
constexpr uint8_t kFactoryWipeMaxAttempts = 3;

enum class FactoryWipeResult : uint8_t {
  NotArmed,        // no request stored; nothing was touched
  Done,            // filesystem and NVS partition erased
  DoneNvsCleared,  // filesystem erased, NVS entries deleted; the partition erase failed
  Retry,           // not finished; the request stays for the next boot
  GaveUp,          // attempts used up; the request was dropped
};

// Ops:
//   bool armed(uint8_t& attempts)      request stored; attempts = boots that started it
//   bool recordAttempt(uint8_t count)  durable before anything is erased
//   void disarm()                      drop the request
//   bool eraseFilesystem()             the whole filesystem partition
//   bool clearNvsEntries()             every NVS entry except the request
//   bool eraseNvs()                    the NVS partition (takes the request with it)
template <typename Ops>
FactoryWipeResult factoryWipeRun(Ops& ops) {
  uint8_t attempts = 0;
  if (!ops.armed(attempts)) return FactoryWipeResult::NotArmed;
  if (attempts >= kFactoryWipeMaxAttempts) {
    ops.disarm();
    return FactoryWipeResult::GaveUp;
  }
  // An attempt that cannot be counted could repeat without bound.
  if (!ops.recordAttempt(static_cast<uint8_t>(attempts + 1))) return FactoryWipeResult::Retry;
  if (!ops.eraseFilesystem()) return FactoryWipeResult::Retry;
  const bool cleared = ops.clearNvsEntries();
  if (ops.eraseNvs()) return FactoryWipeResult::Done;
  if (!cleared) return FactoryWipeResult::Retry;
  // Nothing readable is left, so the next boot must not erase the filesystem
  // again under whatever first-time setup has written by then.
  ops.disarm();
  return FactoryWipeResult::DoneNvsCleared;
}

inline const char* factoryWipeResultText(FactoryWipeResult result) {
  switch (result) {
    case FactoryWipeResult::NotArmed:       return nullptr;
    case FactoryWipeResult::Done:           return "full wipe completed: filesystem and NVS erased";
    case FactoryWipeResult::DoneNvsCleared: return "full wipe completed: filesystem erased, NVS entries deleted (NVS partition erase failed)";
    case FactoryWipeResult::Retry:          return "full wipe did not finish; it runs again at the next boot";
    case FactoryWipeResult::GaveUp:         return "full wipe abandoned after repeated attempts; stored data may remain";
  }
  return nullptr;
}
