#include "System_AuthIdentity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  // vTaskSetThreadLocalStoragePointerAndDelCallback

// FreeRTOS gives us configNUM_THREAD_LOCAL_STORAGE_POINTERS slots per task
// (raised to 4 in sdkconfig to leave headroom for future TLS-backed state).
//
// Slot 0 is OWNED BY ESP-IDF's pthread library (PTHREAD_TLS_INDEX in
// components/pthread/pthread_local_storage.c). Anything that calls
// pthread_getspecific (including some libc paths, ArduinoJson serialization,
// etc.) reads slot 0 as a values_list_t* and dereferences it. We MUST NOT
// claim slot 0 — we use slot 1.
//
// If another subsystem wants its own TLS slot, allocate the next unused
// index (2 or 3) and update this coordinator comment.
static constexpr BaseType_t kAuthTlsSlot = 1;

namespace {

struct TaskIdentity {
  AuthContext ctx;        // default-constructed: user="", transport=0 → ANON
  String      user;
  bool        isAdmin = false;
};

const TaskIdentity& anonSentinel() {
  static const TaskIdentity kAnon{};
  return kAnon;
}

void deleteIdentity(int /*index*/, void* p) {
  delete static_cast<TaskIdentity*>(p);
}

TaskIdentity* getOrCreateSlot() {
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (!self) return nullptr;  // pre-scheduler context
  TaskIdentity* slot = static_cast<TaskIdentity*>(
      pvTaskGetThreadLocalStoragePointer(self, kAuthTlsSlot));
  if (!slot) {
    slot = new TaskIdentity{};
    vTaskSetThreadLocalStoragePointerAndDelCallback(
        self, kAuthTlsSlot, slot, deleteIdentity);
  }
  return slot;
}

const TaskIdentity* getSlotReadOnly() {
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (!self) return &anonSentinel();
  TaskIdentity* slot = static_cast<TaskIdentity*>(
      pvTaskGetThreadLocalStoragePointer(self, kAuthTlsSlot));
  return slot ? slot : &anonSentinel();
}

}  // namespace

const AuthContext& currentAuthContext() { return getSlotReadOnly()->ctx; }
const String&      currentExecUser()    { return getSlotReadOnly()->user; }
bool               currentExecIsAdmin() { return getSlotReadOnly()->isAdmin; }

AuthContext systemIdentity(const char* purpose) {
  AuthContext ctx;
  ctx.transport = SOURCE_INTERNAL;
  ctx.user      = "system";
  ctx.path      = String("/system/") + (purpose ? purpose : "?");
  ctx.ip        = "internal";
  ctx.sid       = "";
  ctx.opaque    = nullptr;
  return ctx;
}

ExecIdentityGuard::ExecIdentityGuard(const AuthContext& install)
    : savedCtx_(),
      savedUser_(),
      savedIsAdmin_(false) {
  TaskIdentity* slot = getOrCreateSlot();
  if (!slot) return;  // pre-scheduler — nothing to install yet
  savedCtx_     = slot->ctx;
  savedUser_    = slot->user;
  savedIsAdmin_ = slot->isAdmin;
  slot->ctx     = install;
  slot->user    = install.user;
  slot->isAdmin = isAdminUser(install.user);
}

ExecIdentityGuard::~ExecIdentityGuard() {
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (!self) return;
  TaskIdentity* slot = static_cast<TaskIdentity*>(
      pvTaskGetThreadLocalStoragePointer(self, kAuthTlsSlot));
  if (!slot) return;
  slot->ctx     = savedCtx_;
  slot->user    = savedUser_;
  slot->isAdmin = savedIsAdmin_;
}

void initAuthIdentityForCurrentTask() { (void)getOrCreateSlot(); }
