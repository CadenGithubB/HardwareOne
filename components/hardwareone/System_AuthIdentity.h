// Per-task identity for guarded operations. Each FreeRTOS task gets its own
// copy of (AuthContext, user, isAdmin) stored in its TCB's thread-local
// storage slot. Default is ANON (zero-initialized); tasks that need SYSTEM
// access must install it explicitly via ExecIdentityGuard.
//
// Replaces the old globals gExecAuthContext, gExecUser, gExecIsAdmin. During
// migration the legacy globals still exist and ExecIdentityGuard keeps them
// in sync; readers can switch to the accessor functions below file-by-file.
//
// Thread safety: each task sees only its own slot. No locks needed.
// Concurrent tasks can have completely different identities simultaneously
// with zero interference.

#ifndef SYSTEM_AUTH_IDENTITY_H
#define SYSTEM_AUTH_IDENTITY_H

#include <Arduino.h>
#include "System_User.h"  // AuthContext, isAdminUser, SOURCE_INTERNAL

// Read accessors — return the calling task's current identity.
const AuthContext& currentAuthContext();
const String&      currentExecUser();
bool               currentExecIsAdmin();

// Build a SYSTEM identity AuthContext (transport=SOURCE_INTERNAL, user="system").
// Use for firmware-internal work that needs full FS access. Always pair with
// ExecIdentityGuard — never assign directly.
AuthContext systemIdentity(const char* purpose);

// Scoped identity install. Constructor saves the prior identity and installs
// the new one in the calling task's TLS slot. Destructor restores. Construct
// on the stack to bracket the region where you want a specific identity
// active.
class ExecIdentityGuard {
 public:
  explicit ExecIdentityGuard(const AuthContext& install);
  ~ExecIdentityGuard();
  ExecIdentityGuard(const ExecIdentityGuard&)            = delete;
  ExecIdentityGuard& operator=(const ExecIdentityGuard&) = delete;
  ExecIdentityGuard(ExecIdentityGuard&&)                 = delete;
  ExecIdentityGuard& operator=(ExecIdentityGuard&&)      = delete;

 private:
  AuthContext savedCtx_;
  String      savedUser_;
  bool        savedIsAdmin_;
};

// Convenience: install SYSTEM for the rest of the current scope.
// Equivalent to `ExecIdentityGuard guard(systemIdentity(purpose));`.
#define SYSTEM_IDENTITY_SCOPE_CONCAT_INNER(a, b) a##b
#define SYSTEM_IDENTITY_SCOPE_CONCAT(a, b) SYSTEM_IDENTITY_SCOPE_CONCAT_INNER(a, b)
#define SYSTEM_IDENTITY_SCOPE(purpose)                                        \
  ExecIdentityGuard SYSTEM_IDENTITY_SCOPE_CONCAT(_sysIdentityGuard_, __LINE__)( \
      systemIdentity(purpose))

// Initialize the TLS slot for the calling task. Idempotent. Worker tasks
// don't need to call this — their first ExecIdentityGuard construction
// allocates the slot lazily. Calling explicitly from app_main is good
// documentation: it makes the main task's slot allocation explicit.
void initAuthIdentityForCurrentTask();

#endif  // SYSTEM_AUTH_IDENTITY_H
