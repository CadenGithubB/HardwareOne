#include "System_AuthIdentity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  // vTaskSetThreadLocalStoragePointerAndDelCallback
#include "System_Debug.h"            // WARN_USERF for bump audit log

// Identity generation counter. See the header comment block above the
// declaration for the full protocol; this is just storage + the bump
// helper.
std::atomic<uint32_t> gIdentityGeneration{1};

void bumpIdentityGeneration(const char* reason) {
  // memory_order_release pairs with the acquire load on the consumer side
  // (FileManager::refresh()) so that any state mutations that motivated
  // the bump (e.g. setSetting writes to users.json or pairedByUser) are
  // visible before a consumer sees the new generation and re-fills.
  const uint32_t newGen =
      gIdentityGeneration.fetch_add(1, std::memory_order_release) + 1;
  // Always emit at WARN level — these events are rare (a handful per
  // device lifetime) and useful for audit. If this ever shows up in
  // hot logs, something is bumping when it shouldn't be.
  WARN_USERF("[IDENTITY-GEN] bump → gen=%u (reason: %s)",
             (unsigned)newGen, reason ? reason : "?");
}

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

  // Stage 3: per-task command-execution context. Default nullptr / inactive.
  void*       currentCmdCtx = nullptr;
  CaptureBufState capture   = {nullptr, 0, 0};
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

// ============================================================================
// CommandIdentityScope — transport→notification mapping + composed install
// ============================================================================
//
// Single source of truth for "what NotificationSource matches this transport."
// Previously this switch lived inline in executeCommand (System_Utils.cpp); it
// was duplicated, by hand, anywhere else that needed the same mapping. Now
// every site reaches it via CommandIdentityScope, which closes the
// "transport-X with notif-source-Y" mismatch class entirely.
//
// 1:1 mapping by design: each transport has exactly one notification source.
// SOURCE_ESPNOW / SOURCE_BLUETOOTH / SOURCE_MQTT all collapse to
// NOTIF_SOURCE_REMOTE because the notification UI doesn't distinguish among
// "remote wire formats" — the audit log already does. SOURCE_INTERNAL maps
// to NOTIF_SOURCE_SYSTEM (system-generated events: boot, scheduler, sensor
// lifecycle).
static uint8_t transportToNotifSource(CommandSource t) {
  switch (t) {
    case SOURCE_WEB:            return NOTIF_SOURCE_WEB;
    case SOURCE_SERIAL:         return NOTIF_SOURCE_CLI;
    case SOURCE_LOCAL_DISPLAY:  return NOTIF_SOURCE_OLED;
    case SOURCE_G2_GLASSES:     return NOTIF_SOURCE_G2;
    case SOURCE_VOICE:          return NOTIF_SOURCE_VOICE;
    case SOURCE_ESPNOW:
    case SOURCE_BLUETOOTH:
    case SOURCE_MQTT:           return NOTIF_SOURCE_REMOTE;
    case SOURCE_INTERNAL:       return NOTIF_SOURCE_SYSTEM;
    default:                    return NOTIF_SOURCE_UNKNOWN;
  }
}

CommandIdentityScope::CommandIdentityScope(const AuthContext& ctx)
    : identityGuard_(ctx),
      notifGuard_(transportToNotifSource(ctx.transport),
                  ctx.user.length() ? ctx.user.c_str() : nullptr) {}

// ============================================================================
// Stage 3 — per-task command-execution context accessors
// ============================================================================

void* currentCommandContext() {
  // Reads can run from any task. Tasks with no allocated slot see nullptr
  // via the anon sentinel — broadcastOutput then falls back to MSG_ROUTE_ALL.
  return getSlotReadOnly()->currentCmdCtx;
}

CaptureBufState* currentCaptureState() {
  // Returns a writable pointer to the slot's capture state so broadcastOutput
  // can update len in place. Returns nullptr when no slot exists yet (pre-
  // scheduler, or task that never touched any TLS-backed API); broadcastOutput
  // skips capture entirely in that case.
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (!self) return nullptr;
  TaskIdentity* slot = static_cast<TaskIdentity*>(
      pvTaskGetThreadLocalStoragePointer(self, kAuthTlsSlot));
  return slot ? &slot->capture : nullptr;
}

void setCurrentCommandContext(void* ctx) {
  TaskIdentity* slot = getOrCreateSlot();
  if (!slot) return;
  slot->currentCmdCtx = ctx;
}

void clearCurrentCommandContext() {
  // Don't allocate just to clear — the read accessor returns nullptr for
  // unallocated slots anyway.
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (!self) return;
  TaskIdentity* slot = static_cast<TaskIdentity*>(
      pvTaskGetThreadLocalStoragePointer(self, kAuthTlsSlot));
  if (slot) slot->currentCmdCtx = nullptr;
}

void setCaptureBuffer(char* buf, size_t cap) {
  TaskIdentity* slot = getOrCreateSlot();
  if (!slot) return;
  slot->capture.buf = buf;
  slot->capture.len = 0;
  slot->capture.cap = cap;
}

void clearCaptureBuffer() {
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (!self) return;
  TaskIdentity* slot = static_cast<TaskIdentity*>(
      pvTaskGetThreadLocalStoragePointer(self, kAuthTlsSlot));
  if (slot) {
    slot->capture.buf = nullptr;
    slot->capture.len = 0;
    slot->capture.cap = 0;
  }
}
