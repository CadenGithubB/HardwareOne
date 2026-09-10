#ifndef SYSTEM_FILESYSTEM_INTERNAL_H
#define SYSTEM_FILESYSTEM_INTERNAL_H

#include <Arduino.h>

#include "System_Mutex.h"
#include "System_User.h"

// Internal helpers shared only by the filesystem listing implementations.
// This is deliberately separate from System_Filesystem.h: a resolved role is
// safe to reuse only while the filesystem mutex that protects users.json stays
// owned by the same task, and it must never become a general authorization
// token that callers can pass to VFS operations.
namespace FsInternal {

class LockedListingPermissions final {
 public:
  explicit LockedListingPermissions(const AuthContext& ctx);
  ~LockedListingPermissions() = default;

  LockedListingPermissions(const LockedListingPermissions&) = delete;
  LockedListingPermissions& operator=(const LockedListingPermissions&) = delete;
  LockedListingPermissions(LockedListingPermissions&&) = delete;
  LockedListingPermissions& operator=(LockedListingPermissions&&) = delete;

  // True only on the task that constructed the view and only while that task
  // still owns gFsMutex. Every query repeats this check and fails closed.
  bool ready() const;

  // Silent UI metadata queries. These never authorize I/O and never emit a
  // denial log; VFS::*Guarded remains the sole access-enforcement boundary.
  uint8_t forPath(const String& path) const;
  uint8_t forChildOf(const String& dirPath) const;

 private:
  // First member by design: acquire/reuse gFsMutex before resolving a named
  // account, and release it only after every other captured field is dead.
  FsLockGuard lock_;
  String scope_;
  TaskHandle_t ownerTask_ = nullptr;
  uint8_t role_ = 0;
  bool dynamicBond_ = false;
  bool ready_ = false;
};

}  // namespace FsInternal

#endif  // SYSTEM_FILESYSTEM_INTERNAL_H
