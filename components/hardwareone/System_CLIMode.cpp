// System_CLIMode.cpp — session-owned interactive CLI slot.

#include "System_CLIMode.h"

#include <atomic>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "System_AuthIdentity.h"
#include "System_CommandTypes.h"
#include "System_Debug.h"
#include "System_User.h"

namespace {
struct CliModeOwner {
  CommandSource source = SOURCE_INTERNAL;
  TransportSessionEpoch epoch = 0;
};

enum class CliModePhase : uint8_t {
  Active,
  HandlingInput,
  ExitPending,
  Exiting,
};

struct CliModeState {
  const CliMode* mode = nullptr;
  CliModeOwner owner;
  uint32_t instanceId = 0;
  int64_t enteredUs = 0;
  int64_t lastInputUs = 0;
  CliModePhase phase = CliModePhase::Active;
  const char* exitReason = nullptr;
};

CliModeState sMode;
uint32_t sNextInstanceId = 0;
std::atomic<bool> sSuppressAmbientSerial{false};
StaticSemaphore_t sModeMutexStorage;
SemaphoreHandle_t sModeMutex = nullptr;
portMUX_TYPE sModeInitMux = portMUX_INITIALIZER_UNLOCKED;
constexpr uint32_t kDefaultIdleTimeoutMs = 5u * 60u * 1000u;

SemaphoreHandle_t modeMutex() {
  if (sModeMutex) return sModeMutex;
  portENTER_CRITICAL(&sModeInitMux);
  if (!sModeMutex) {
    sModeMutex = xSemaphoreCreateRecursiveMutexStatic(&sModeMutexStorage);
  }
  portEXIT_CRITICAL(&sModeInitMux);
  return sModeMutex;
}

class ModeLock {
 public:
  explicit ModeLock(TickType_t wait = portMAX_DELAY) {
    SemaphoreHandle_t mutex = modeMutex();
    locked_ = mutex && xSemaphoreTakeRecursive(mutex, wait) == pdTRUE;
  }
  ~ModeLock() {
    if (locked_) xSemaphoreGiveRecursive(sModeMutex);
  }
  explicit operator bool() const { return locked_; }
 private:
  bool locked_ = false;
};

bool currentOwner(CliModeOwner& out, bool requireInteractive = true) {
  const CommandContext* ctx =
      static_cast<const CommandContext*>(currentCommandContext());
  if (!ctx || ctx->transportSessionEpoch == 0) return false;
  if (requireInteractive &&
      ((ctx->behaviorFlags & COMMAND_CONTEXT_MODE_INDEPENDENT) ||
       ctx->validateOnly)) {
    return false;
  }
  if (!transportSessionEpochIsLive(ctx->auth.transport,
                                   ctx->transportSessionEpoch)) {
    return false;
  }
  out.source = ctx->auth.transport;
  out.epoch = ctx->transportSessionEpoch;
  return true;
}

bool sameOwner(const CliModeOwner& a, const CliModeOwner& b) {
  return a.epoch != 0 && a.source == b.source && a.epoch == b.epoch;
}

bool ownerStillLive(const CliModeOwner& owner) {
  return owner.epoch != 0 &&
         transportSessionEpochIsLive(owner.source, owner.epoch);
}

bool modeTimedOutLocked(int64_t nowUs) {
  if (!sMode.mode) return false;
  const uint32_t timeoutMs = sMode.mode->idleTimeoutMs
                                 ? sMode.mode->idleTimeoutMs
                                 : kDefaultIdleTimeoutMs;
  return timeoutMs != 0 && nowUs >= sMode.lastInputUs &&
         static_cast<uint64_t>(nowUs - sMode.lastInputUs) >=
             static_cast<uint64_t>(timeoutMs) * 1000ULL;
}

bool modeLogicallyActiveLocked() {
  return sMode.mode && sMode.phase == CliModePhase::Active &&
         ownerStillLive(sMode.owner) &&
         !modeTimedOutLocked(esp_timer_get_time());
}

void requestExitLocked(const char* reason) {
  if (!sMode.mode || sMode.phase != CliModePhase::Active) return;
  sMode.phase = CliModePhase::ExitPending;
  sMode.exitReason = reason ? reason : "requested";
}

void requestInvalidExitLocked() {
  if (!sMode.mode || sMode.phase != CliModePhase::Active) return;
  if (!ownerStillLive(sMode.owner)) {
    requestExitLocked("owner_lost");
  } else if (modeTimedOutLocked(esp_timer_get_time())) {
    requestExitLocked("idle_timeout");
  }
}

struct ExitClaim {
  const CliMode* mode = nullptr;
  uint32_t instanceId = 0;
  const char* reason = nullptr;
};

bool claimExitLocked(ExitClaim& claim) {
  requestInvalidExitLocked();
  if (!sMode.mode || sMode.phase != CliModePhase::ExitPending) return false;
  claim.mode = sMode.mode;
  claim.instanceId = sMode.instanceId;
  claim.reason = sMode.exitReason;
  sMode.phase = CliModePhase::Exiting;
  return true;
}

bool callerMayExitLocked() {
  CliModeOwner caller;
  if (currentOwner(caller, /*requireInteractive=*/true)) {
    return sameOwner(caller, sMode.owner);
  }
  // The only callback with physical input outside a queued CommandContext is
  // the OLED wizard tick.
  return !currentCommandContext() &&
         sMode.owner.source == SOURCE_LOCAL_DISPLAY;
}
}  // namespace

bool cliEnterMode(const CliMode* mode) {
  return cliEnterModePrepared(mode, nullptr, nullptr);
}

bool cliEnterModePrepared(const CliMode* mode,
                          CliModeEntryCommit commit,
                          void* commitData) {
  if (!mode) return false;
  CliModeOwner owner;
  if (!currentOwner(owner, /*requireInteractive=*/true)) {
    DEBUGF(DEBUG_CLI,
           "[climode] enter '%s' rejected: invocation has no live interactive session",
           mode->name ? mode->name : "(unnamed)");
    return false;
  }

  bool entered = false;
  {
    ModeLock lock;
    if (!lock) return false;
    requestInvalidExitLocked();
    if (sMode.mode) {
      DEBUGF(DEBUG_CLI, "[climode] enter '%s' rejected: '%s' already active",
             mode->name ? mode->name : "(unnamed)",
             sMode.mode->name ? sMode.mode->name : "(unnamed)");
      return false;
    }
    // Revalidate after acquiring the slot lock. A logout/revoke that wins
    // before this point must prevent publication of a dead owner.
    if (!ownerStillLive(owner)) return false;

    if (commit) commit(commitData);
    do {
      ++sNextInstanceId;
    } while (sNextInstanceId == 0);
    const int64_t nowUs = esp_timer_get_time();
    sMode.mode = mode;
    sMode.owner = owner;
    sMode.instanceId = sNextInstanceId;
    sMode.enteredUs = nowUs;
    sMode.lastInputUs = nowUs;
    sMode.phase = CliModePhase::Active;
    sMode.exitReason = nullptr;
    sSuppressAmbientSerial.store(owner.source == SOURCE_SERIAL,
                                 std::memory_order_release);
    DEBUGF(DEBUG_CLI, "[climode] enter '%s' instance=%lu transport=%d epoch=%lu",
           mode->name ? mode->name : "(unnamed)",
           static_cast<unsigned long>(sMode.instanceId),
           static_cast<int>(owner.source),
           static_cast<unsigned long>(owner.epoch));
    if (mode->onEnter) mode->onEnter(mode->userData);
    requestInvalidExitLocked();
    entered = sMode.phase == CliModePhase::Active;
  }
  if (!entered) (void)cliModeExecutorDrainPending();
  return entered;
}

bool cliExitMode() {
  ModeLock lock;
  if (!lock || !sMode.mode || sMode.phase != CliModePhase::Active) return false;
  if (!callerMayExitLocked()) return false;
  requestExitLocked("owner_exit");
  return true;
}

bool cliInModeActive() {
  ModeLock lock;
  return lock && sMode.mode;
}

const CliMode* cliCurrentMode() {
  ModeLock lock;
  return lock ? sMode.mode : nullptr;
}

bool cliModeCurrentCommandOwns(const CliMode* expected) {
  CliModeOwner caller;
  if (!currentOwner(caller, /*requireInteractive=*/true)) return false;
  ModeLock lock;
  if (!lock || !modeLogicallyActiveLocked()) return false;
  return (!expected || expected == sMode.mode) && sameOwner(caller, sMode.owner);
}

bool cliModeOwnedBySession(CommandSource source,
                           TransportSessionEpoch epoch,
                           const CliMode* expected) {
  if (epoch == 0) return false;
  ModeLock lock;
  if (!lock || !modeLogicallyActiveLocked()) return false;
  return (!expected || expected == sMode.mode) &&
         sMode.owner.source == source && sMode.owner.epoch == epoch;
}

bool cliModeCurrentInvocationCanInteract() {
  CliModeOwner owner;
  return currentOwner(owner, /*requireInteractive=*/true);
}

bool cliModeNoteActivity() {
  ModeLock lock;
  if (!lock || !sMode.mode || sMode.phase != CliModePhase::Active) return false;
  if (!callerMayExitLocked() || !ownerStillLive(sMode.owner)) return false;
  sMode.lastInputUs = esp_timer_get_time();
  return true;
}

bool cliModeSuppressesAmbientSerial() {
  // Output consumption must never wait on a mode callback, but it also must
  // not fail open while that callback owns the mutex/slot. This publication
  // spans Active, HandlingInput, ExitPending and Exiting and is cleared only
  // after onExit has finished and the reserved slot is actually reusable.
  return sSuppressAmbientSerial.load(std::memory_order_acquire);
}

uint32_t cliModeInstanceId() {
  ModeLock lock;
  return lock && sMode.mode ? sMode.instanceId : 0;
}

void cliModeTick() {
  ModeLock lock(0);
  if (!lock || !sMode.mode) return;
  requestInvalidExitLocked();
  if (sMode.phase != CliModePhase::Active) return;
  if (!sMode.mode->onTick) return;
  // OLED polling is physical input. It must never mutate a wizard opened by
  // web, BLE or serial, even if those sessions have the same username.
  if (sMode.owner.source != SOURCE_LOCAL_DISPLAY) return;
  sMode.mode->onTick(sMode.mode->userData);
}

bool cliModeDispatchInput(const String& line, char* out, size_t outSize) {
  // Completion requested by a previous main-loop tick is drained only on the
  // executor, before any new mode input can run.
  (void)cliModeExecutorDrainPending();

  const CommandContext* ctx =
      static_cast<const CommandContext*>(currentCommandContext());
  if (ctx && ((ctx->behaviorFlags & COMMAND_CONTEXT_MODE_INDEPENDENT) ||
              ctx->validateOnly)) {
    return false;
  }

  CliModeOwner caller;
  if (!currentOwner(caller, /*requireInteractive=*/true)) return false;

  bool consumed = false;
  const CliMode* mode = nullptr;
  uint32_t instance = 0;
  {
    ModeLock lock;
    if (!lock || !sMode.mode) return false;
    requestInvalidExitLocked();
    if (sMode.phase != CliModePhase::Active) return false;
    if (!sameOwner(caller, sMode.owner)) {
      // Foreign commands proceed normally. In particular, a foreign "yes" is
      // just an unknown registry command and cannot consume a confirmation.
      return false;
    }
    if (!sMode.mode->onInput) return false;
    if (out && outSize) out[0] = '\0';

    sMode.lastInputUs = esp_timer_get_time();
    mode = sMode.mode;
    instance = sMode.instanceId;
    // Reserve the slot, but release the global mode mutex before invoking the
    // callback. Wizard transitions may scan Wi-Fi and confirmation resolution
    // can perform filesystem/user work; neither may impose its lock ordering on
    // mode queries or transport lifecycle hooks.
    sMode.phase = CliModePhase::HandlingInput;
  }

  const CliModeInputResult result =
      mode->onInput(line, mode->userData, out, outSize);

  {
    ModeLock lock;
    if (!lock || !sMode.mode || sMode.instanceId != instance ||
        sMode.mode != mode || sMode.phase != CliModePhase::HandlingInput) {
      consumed = result == CLI_MODE_HANDLED ||
                 result == CLI_MODE_HANDLED_AND_EXIT;
    } else {
      sMode.phase = CliModePhase::Active;
      requestInvalidExitLocked();
      if (sMode.phase != CliModePhase::Active &&
          (result == CLI_MODE_PASSTHROUGH ||
           result == CLI_MODE_PASSTHROUGH_AND_EXIT)) {
        if (out && outSize) {
          snprintf(out, outSize,
                   "Error: interactive session changed while handling input.");
        }
        consumed = true;
      } else {
        switch (result) {
          case CLI_MODE_HANDLED:
            consumed = true;
            break;
          case CLI_MODE_HANDLED_AND_EXIT:
            requestExitLocked("completed");
            consumed = true;
            break;
          case CLI_MODE_PASSTHROUGH:
            consumed = false;
            break;
          case CLI_MODE_PASSTHROUGH_AND_EXIT:
            requestExitLocked("passthrough");
            consumed = false;
            break;
          default:
            DEBUGF(DEBUG_CLI,
                   "[climode] '%s' returned invalid input result=%d; consuming",
                   mode->name ? mode->name : "(unnamed)",
                   static_cast<int>(result));
            consumed = true;
            break;
        }
      }
    }
  }
  // onExit runs outside the mutex, but before passthrough registry execution.
  (void)cliModeExecutorDrainPending();
  return consumed;
}

bool cliModeExitCanComplete() {
  if (!gCmdExecTaskHandle ||
      xTaskGetCurrentTaskHandle() != gCmdExecTaskHandle) {
    return false;
  }
  ModeLock lock;
  return lock && sMode.mode && sMode.phase == CliModePhase::Exiting &&
         ownerStillLive(sMode.owner) &&
         !modeTimedOutLocked(esp_timer_get_time());
}

bool cliModeExecutorDrainPending() {
  if (!gCmdExecTaskHandle ||
      xTaskGetCurrentTaskHandle() != gCmdExecTaskHandle) {
    return false;
  }

  ExitClaim claim;
  {
    ModeLock lock;
    if (!lock || !claimExitLocked(claim)) return false;
  }

  DEBUGF(DEBUG_CLI, "[climode] exit '%s' instance=%lu reason=%s",
         claim.mode && claim.mode->name ? claim.mode->name : "(unnamed)",
         static_cast<unsigned long>(claim.instanceId),
         claim.reason ? claim.reason : "unknown");
  // No mode mutex is held here. The slot remains reserved in Exiting phase,
  // preventing callback overlap or replacement until cleanup finishes.
  if (claim.mode && claim.mode->onExit) {
    claim.mode->onExit(claim.mode->userData);
  }

  {
    ModeLock lock;
    if (lock && sMode.mode == claim.mode &&
        sMode.instanceId == claim.instanceId &&
        sMode.phase == CliModePhase::Exiting) {
      sMode = CliModeState{};
      sSuppressAmbientSerial.store(false, std::memory_order_release);
    }
  }
  return true;
}
