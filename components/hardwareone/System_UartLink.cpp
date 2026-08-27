// System_UartLink.cpp — UART host link drain. See System_UartLink.h for the
// design charter and docs/UART_HOST_LINK_PLAN.md for the verified plan.
#include "System_UartLink.h"
#include "System_Cm5HostControl.h"
#include "System_Cm5Presence.h"
#include "System_Dictation.h"  // direct keyboard-dictation control plane
#include "System_LLMCm5.h"   // cm5 llm push/end intrinsic (no-op header when off)
#include "System_Debug.h"
#include "System_User.h"  // sessionStampNow + shared auth/session helpers
#include "System_CommandLimits.h"

#include <atomic>
#include <freertos/semphr.h>

// Session gate lives here.
// Defined unconditionally so the auth plumbing (revocation sweeps, session
// list, tgRequireAuth) links on every board, including ones without link pins.
// The active epoch is the single atomic authority token: zero means logged
// out; every login publishes a new nonzero value. The username is fixed
// storage under sUartSessionMux and is never exposed as a mutable String.
static constexpr size_t kUartSessionUserMax = 64;
static char sUartSessionUser[kUartSessionUserMax + 1] = {};
static bool sUartSessionCm5PresenceAllowed = false;
static portMUX_TYPE sUartSessionMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE sUartLifecycleInitMux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t sUartLifecycleMutexStorage;
static SemaphoreHandle_t sUartLifecycleMutex = nullptr;
static std::atomic<uint32_t> sUartSessionGeneration{0};
static std::atomic<uint32_t> sUartActiveSessionEpoch{0};
static std::atomic<uint32_t> sUartTransportGeneration{0};
static std::atomic<uint32_t> sUartActiveTransportEpoch{0};
static std::atomic<unsigned long> sLastInteractionMs{0}; // idle clock
enum class UartLinkSessionEvent : uint8_t {
  Never,
  Authenticated,
  Cleared,
  LinkStopped,
  IdleTimeout,
  ExplicitLogout,
  TransportLogout,
  AccountRevoked,
};
static std::atomic<uint8_t> sUartLastSessionEvent{
    static_cast<uint8_t>(UartLinkSessionEvent::Never)};

static SemaphoreHandle_t uartLifecycleMutex() {
  if (sUartLifecycleMutex) return sUartLifecycleMutex;
  portENTER_CRITICAL(&sUartLifecycleInitMux);
  if (!sUartLifecycleMutex) {
    sUartLifecycleMutex =
        xSemaphoreCreateRecursiveMutexStatic(&sUartLifecycleMutexStorage);
  }
  portEXIT_CRITICAL(&sUartLifecycleInitMux);
  return sUartLifecycleMutex;
}

class UartLifecycleGuard {
 public:
  explicit UartLifecycleGuard(TickType_t wait = portMAX_DELAY) {
    SemaphoreHandle_t mutex = uartLifecycleMutex();
    locked_ = mutex && xSemaphoreTakeRecursive(mutex, wait) == pdTRUE;
  }
  ~UartLifecycleGuard() {
    if (locked_) xSemaphoreGiveRecursive(sUartLifecycleMutex);
  }
  explicit operator bool() const { return locked_; }
 private:
  bool locked_ = false;
};

uint32_t uartLinkSessionEpoch() {
  return sUartActiveSessionEpoch.load(std::memory_order_acquire);
}

uint32_t uartLinkTransportSessionEpoch() {
  return sUartActiveTransportEpoch.load(std::memory_order_acquire);
}

// The caller holds sUartSessionMux. A command-transport incarnation exists
// whenever the physical link is open, including auth-disabled/AuthBypass use.
// It is deliberately separate from the named login epoch used by CM5 leases.
static void uartLinkRotateTransportLocked(bool open) {
  sUartActiveTransportEpoch.store(0, std::memory_order_release);
  if (!open) return;
  uint32_t current = sUartTransportGeneration.load(std::memory_order_relaxed);
  uint32_t next = 0;
  do {
    next = current + 1u;
    if (next == 0) next = 1;
  } while (!sUartTransportGeneration.compare_exchange_weak(
      current, next, std::memory_order_release, std::memory_order_relaxed));
  sUartActiveTransportEpoch.store(next, std::memory_order_release);
}

void uartLinkAuthPolicyChanged() {
  UartLifecycleGuard lifecycle;
  if (!lifecycle) return;
  bool rotated = false;
  uint32_t transportEpoch = 0;
  portENTER_CRITICAL(&sUartSessionMux);
  // A named login already rotated away every older bypass request and remains
  // valid across a policy toggle. Only the unauthenticated link incarnation
  // needs replacement.
  if (sUartActiveSessionEpoch.load(std::memory_order_relaxed) == 0 &&
      sUartActiveTransportEpoch.load(std::memory_order_relaxed) != 0) {
    uartLinkRotateTransportLocked(true);
    rotated = true;
  }
  transportEpoch =
      sUartActiveTransportEpoch.load(std::memory_order_relaxed);
  portEXIT_CRITICAL(&sUartSessionMux);
  DEBUG_UART_LIFECYCLEF(
      "[uartlink] auth policy changed transport_epoch=%lu rotated=%d",
      static_cast<unsigned long>(transportEpoch), rotated ? 1 : 0);
}

uint32_t uartLinkSessionGeneration() {
  return sUartSessionGeneration.load(std::memory_order_acquire);
}

static const char* uartLinkSessionEventName(UartLinkSessionEvent event) {
  switch (event) {
    case UartLinkSessionEvent::Never: return "never";
    case UartLinkSessionEvent::Authenticated: return "authenticated";
    case UartLinkSessionEvent::Cleared: return "cleared";
    case UartLinkSessionEvent::LinkStopped: return "link_stop";
    case UartLinkSessionEvent::IdleTimeout: return "idle_timeout";
    case UartLinkSessionEvent::ExplicitLogout: return "explicit_logout";
    case UartLinkSessionEvent::TransportLogout: return "transport_logout";
    case UartLinkSessionEvent::AccountRevoked: return "account_revoked";
  }
  return "unknown";
}

const char* uartLinkSessionLastEventName() {
  return uartLinkSessionEventName(static_cast<UartLinkSessionEvent>(
      sUartLastSessionEvent.load(std::memory_order_acquire)));
}

UartLinkSessionDiagnostics uartLinkSessionDiagnostics() {
  UartLinkSessionDiagnostics out{};
  UartLinkSessionEvent event = UartLinkSessionEvent::Never;
  portENTER_CRITICAL(&sUartSessionMux);
  out.activeEpoch = sUartActiveSessionEpoch.load(std::memory_order_relaxed);
  out.lastEpoch = sUartSessionGeneration.load(std::memory_order_relaxed);
  event = static_cast<UartLinkSessionEvent>(
      sUartLastSessionEvent.load(std::memory_order_relaxed));
  portEXIT_CRITICAL(&sUartSessionMux);
  out.lastEvent = uartLinkSessionEventName(event);
  return out;
}

uint32_t uartLinkSessionAuthenticated(const String& user,
                                      bool allowCm5Presence,
                                      uint32_t* transportEpochOut) {
  if (transportEpochOut) *transportEpochOut = 0;
  UartLifecycleGuard lifecycle;
  if (!lifecycle) return 0;
  const unsigned long interactionStamp = sessionStampNow();
  // Publish the new identity and generation before opening the auth gate. A
  // producer from the previous login may have one physical frame already
  // admitted, but every later session-fenced admission observes the new epoch.
  char nextUser[kUartSessionUserMax + 1] = {};
  strlcpy(nextUser, user.length() ? user.c_str() : "uart",
          sizeof(nextUser));
  portENTER_CRITICAL(&sUartSessionMux);
  sUartActiveSessionEpoch.store(0, std::memory_order_release);
  memcpy(sUartSessionUser, nextUser, sizeof(sUartSessionUser));
  sUartSessionCm5PresenceAllowed = allowCm5Presence;
  uint32_t current = sUartSessionGeneration.load(std::memory_order_relaxed);
  uint32_t next = 0;
  do {
    next = current + 1u;
    if (next == 0) next = 1;  // zero is permanently reserved for no session
  } while (!sUartSessionGeneration.compare_exchange_weak(
      current, next, std::memory_order_release, std::memory_order_relaxed));
  sUartActiveSessionEpoch.store(next, std::memory_order_release);
  uartLinkRotateTransportLocked(true);
  const uint32_t transportEpoch =
      sUartActiveTransportEpoch.load(std::memory_order_relaxed);
  sUartLastSessionEvent.store(
      static_cast<uint8_t>(UartLinkSessionEvent::Authenticated),
      std::memory_order_release);
  sLastInteractionMs.store(interactionStamp, std::memory_order_release);
  portEXIT_CRITICAL(&sUartSessionMux);
  if (transportEpochOut) *transportEpochOut = transportEpoch;
  cm5PresenceNotifySessionChanged();
  DEBUG_UART_LIFECYCLEF(
      "[uartlink] session authenticated named_epoch=%lu transport_epoch=%lu control=%d",
      static_cast<unsigned long>(next),
      static_cast<unsigned long>(transportEpoch),
      allowCm5Presence ? 1 : 0);
  return next;
}

uint32_t uartLinkSessionCleared(UartLinkSessionClearReason reason,
                                uint32_t expectedTransportEpoch) {
  UartLifecycleGuard lifecycle;
  if (!lifecycle) return 0;
  // Close the fast auth gate first. The epoch advances on the next successful
  // login; keeping the last value while logged out makes status diagnostics
  // useful without granting any authority.
  portENTER_CRITICAL(&sUartSessionMux);
  if (expectedTransportEpoch != 0 &&
      sUartActiveTransportEpoch.load(std::memory_order_relaxed) !=
          expectedTransportEpoch) {
    portEXIT_CRITICAL(&sUartSessionMux);
    return 0;
  }
  sUartActiveSessionEpoch.store(0, std::memory_order_release);
  sUartSessionUser[0] = '\0';
  sUartSessionCm5PresenceAllowed = false;
  UartLinkSessionEvent event = UartLinkSessionEvent::Cleared;
  switch (reason) {
    case UartLinkSessionClearReason::LinkStop:
      event = UartLinkSessionEvent::LinkStopped;
      break;
    case UartLinkSessionClearReason::IdleTimeout:
      event = UartLinkSessionEvent::IdleTimeout;
      break;
    case UartLinkSessionClearReason::ExplicitLogout:
      event = UartLinkSessionEvent::ExplicitLogout;
      break;
    case UartLinkSessionClearReason::TransportLogout:
      event = UartLinkSessionEvent::TransportLogout;
      break;
    case UartLinkSessionClearReason::Unspecified:
      break;
  }
  const bool linkRemainsOpen =
      reason != UartLinkSessionClearReason::LinkStop &&
      sUartActiveTransportEpoch.load(std::memory_order_relaxed) != 0;
  uartLinkRotateTransportLocked(linkRemainsOpen);
  const uint32_t transportEpoch =
      sUartActiveTransportEpoch.load(std::memory_order_relaxed);
  sUartLastSessionEvent.store(static_cast<uint8_t>(event),
                              std::memory_order_release);
  sLastInteractionMs.store(0, std::memory_order_release);
  portEXIT_CRITICAL(&sUartSessionMux);
  cm5PresenceNotifySessionChanged();
  DEBUG_UART_LIFECYCLEF(
      "[uartlink] session %s transport_epoch=%lu",
      uartLinkSessionEventName(event),
      static_cast<unsigned long>(transportEpoch));
  return transportEpoch;
}

bool uartLinkSessionSnapshot(char* userOut, size_t userOutSize,
                             uint32_t* epochOut,
                             uint32_t* transportEpochOut,
                             bool* cm5PresenceAllowedOut) {
  if (userOut && userOutSize) userOut[0] = '\0';
  portENTER_CRITICAL(&sUartSessionMux);
  const uint32_t activeEpoch =
      sUartActiveSessionEpoch.load(std::memory_order_relaxed);
  const bool authed = activeEpoch != 0;
  if (userOut && userOutSize)
    strlcpy(userOut, sUartSessionUser, userOutSize);
  if (epochOut)
    *epochOut = activeEpoch;
  if (transportEpochOut)
    *transportEpochOut =
        sUartActiveTransportEpoch.load(std::memory_order_relaxed);
  if (cm5PresenceAllowedOut)
    *cm5PresenceAllowedOut = authed && sUartSessionCm5PresenceAllowed;
  portEXIT_CRITICAL(&sUartSessionMux);
  return authed;
}

bool uartLinkNamedSessionBeginUse(uint32_t transportEpoch,
                                  const String& expectedUser) {
  if (transportEpoch == 0 || expectedUser.length() == 0) return false;
  SemaphoreHandle_t mutex = uartLifecycleMutex();
  if (!mutex || xSemaphoreTakeRecursive(mutex, portMAX_DELAY) != pdTRUE)
    return false;

  char user[kUartSessionUserMax + 1] = {};
  uint32_t namedEpoch = 0;
  uint32_t liveTransportEpoch = 0;
  const bool authed = uartLinkSessionSnapshot(
      user, sizeof(user), &namedEpoch, &liveTransportEpoch);
  if (!authed || namedEpoch == 0 || liveTransportEpoch != transportEpoch ||
      expectedUser != user) {
    xSemaphoreGiveRecursive(mutex);
    return false;
  }
  return true;
}

void uartLinkNamedSessionEndUse() {
  if (sUartLifecycleMutex) xSemaphoreGiveRecursive(sUartLifecycleMutex);
}

bool uartLinkTransportSessionBeginUse(uint32_t transportEpoch) {
  if (transportEpoch == 0) return false;
  SemaphoreHandle_t mutex = uartLifecycleMutex();
  if (!mutex || xSemaphoreTakeRecursive(mutex, portMAX_DELAY) != pdTRUE)
    return false;
  if (uartLinkTransportSessionEpoch() != transportEpoch) {
    xSemaphoreGiveRecursive(mutex);
    return false;
  }
  return true;
}

void uartLinkTransportSessionEndUse() {
  if (sUartLifecycleMutex) xSemaphoreGiveRecursive(sUartLifecycleMutex);
}

static bool uartSessionUserEqualsAsciiNoCase(const char* a, const char* b) {
  while (*a && *b) {
    char ca = *a++;
    char cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
    if (ca != cb) return false;
  }
  return *a == '\0' && *b == '\0';
}

bool uartLinkSessionClearIfUser(const String& user) {
  if (!user.length()) return false;
  UartLifecycleGuard lifecycle;
  if (!lifecycle) return false;
  char target[kUartSessionUserMax + 1] = {};
  strlcpy(target, user.c_str(), sizeof(target));
  bool cleared = false;
  portENTER_CRITICAL(&sUartSessionMux);
  if (sUartActiveSessionEpoch.load(std::memory_order_relaxed) != 0 &&
      uartSessionUserEqualsAsciiNoCase(sUartSessionUser, target)) {
    sUartActiveSessionEpoch.store(0, std::memory_order_release);
    sUartSessionUser[0] = '\0';
    sUartSessionCm5PresenceAllowed = false;
    sUartLastSessionEvent.store(
        static_cast<uint8_t>(UartLinkSessionEvent::AccountRevoked),
        std::memory_order_release);
    if (sUartActiveTransportEpoch.load(std::memory_order_relaxed) != 0)
      uartLinkRotateTransportLocked(true);
    cleared = true;
  }
  portEXIT_CRITICAL(&sUartSessionMux);
  if (cleared) {
    cm5PresenceNotifySessionChanged();
    DEBUG_UART_LIFECYCLEF("[uartlink] session account_revoked");
  }
  return cleared;
}

bool uartLinkSessionRestrictCm5PresenceIfUser(
    const String& user, uint32_t expectedSessionEpoch) {
  if (!user.length() || expectedSessionEpoch == 0) return false;
  UartLifecycleGuard lifecycle;
  if (!lifecycle) return false;
  char target[kUartSessionUserMax + 1] = {};
  strlcpy(target, user.c_str(), sizeof(target));
  bool restricted = false;
  portENTER_CRITICAL(&sUartSessionMux);
  if (sUartActiveSessionEpoch.load(std::memory_order_relaxed) ==
          expectedSessionEpoch &&
      uartSessionUserEqualsAsciiNoCase(sUartSessionUser, target)) {
    sUartSessionCm5PresenceAllowed = false;
    restricted = true;
  }
  portEXIT_CRITICAL(&sUartSessionMux);
  if (restricted) {
    DEBUG_UART_LIFECYCLEF(
        "[uartlink] session control restricted named_epoch=%lu",
        static_cast<unsigned long>(expectedSessionEpoch));
  }
  return restricted;
}

#ifdef UART_LINK_PORT

#include "System_BuildConfig.h"
#include "System_CommandTypes.h"
#include "System_Settings.h"
#include "System_Events.h"        // systemEventPost — SYSEVT_LOGIN_OK/FAIL for the host link
#include "System_SetupWizard.h"   // gWizardOwnsSerial — park while wizard owns the CLI
#include "System_Command.h"       // CommandArgs — cmd_voicefetch argument parsing
#include "System_Filesystem.h"    // requireQuotedPath
#include "System_VFS.h"           // VFS::openGuarded — permission-guarded read
#include "System_Mutex.h"         // FsLockGuard
#include "System_MemUtil.h"       // ps_alloc / AllocPref
#include "System_AuthIdentity.h"  // currentAuthContext — transport/AuthBypass gate
#include "System_Utils.h"         // RETURN_VALID_IF_VALIDATE_CSTR
#include "System_LiveAudio.h"     // liveAudioStreamActive — no bulk fetch overlap
#if ENABLE_MICROPHONE
#include "System_Microphone.h"    // micRecordingBusy — never fetch an open WAV
#endif
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// The link must never bind the UART instance that carries the IDF console —
// two unsynchronized writers byte-interleave, and on classic-ESP32 boards
// Serial IS Serial0, so binding UART0 there would tear down the live console.
// On S3 this fails the build until the console has moved to USB-Serial-JTAG
// (CONFIG_ESP_CONSOLE_UART_NUM resolves to -1); on classic boards the link
// binds UART1/2 and the console keeps UART0.
#if defined(CONFIG_ESP_CONSOLE_UART_NUM) && (CONFIG_ESP_CONSOLE_UART_NUM == UART_LINK_UART_NUM)
#error "UART link is configured on the IDF console UART. Move the console (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) or change UART_LINK_UART_NUM for this board."
#endif

#if ENABLE_HTTP_SERVER
#include "WebServer_Server.h"     // isLoginLocked/recordFailedLogin/clearLoginAttempts/authSuccessUnified/recordLoginAttempt
#endif

// Defined in HardwareOne.cpp (web-mirror command feed). Declared there with
// defaulted trailing args; this TU passes all four explicitly.
extern void appendCommandToFeed(const char* source, const String& cmd, const String& user, const String& ip);
extern bool submitAndExecuteSync(const Command& cmd, String& out);

static std::atomic<bool> sStarted{false};    // publishes port/mutex lifecycle
static String sUartCLI;                     // line accumulator
static uint32_t sUartLineTransportEpoch = 0; // incarnation owning first byte
static unsigned long sLastNagMs = 0;         // rate limiter for unauth/garbage replies
static bool sDiscardingLine = false;         // true = swallow bytes until the next '\n'
// uartProcessLine runs only on the loop task and consumes at most one line per
// lap, so one fixed callback buffer is safe. It is deliberately separate from
// System_Cm5HostControl's cmd_exec reply buffer: direct machine callbacks may
// arrive while cmd_exec is serving another transport.
// Lives in PSRAM: filled by the control-plane intrinsics (snprintf, all
// outside their spinlocks) and read by strlcpy/strncmp on the loop task.
// Never handed to the UART driver — uartWriteLineForTransportSession copies
// it into its own internal line buffer first. Carries usernames, never
// passwords or tokens (verified 2026-08-19).
EXT_RAM_BSS_ATTR static char sUartControlReply[256];

// Deferred lifecycle request. Command handlers run on cmd_exec_task, which
// ticks concurrently with the loop task that owns the drain — calling
// end()/begin() there would free the port and the line accumulator underneath
// an in-flight uartLinkTick (concurrent String mutation = heap corruption).
// Handlers set this; the tick applies it on the loop task before draining.
enum UartLinkPending : uint8_t { UL_PENDING_NONE = 0, UL_PENDING_START, UL_PENDING_STOP, UL_PENDING_RESTART };
static std::atomic<uint8_t> sPending{UL_PENDING_NONE};

static void uartLinkRequeueIfNone(UartLinkPending request) {
  uint8_t expected = UL_PENDING_NONE;
  (void)sPending.compare_exchange_strong(
      expected, static_cast<uint8_t>(request),
      std::memory_order_release, std::memory_order_relaxed);
}

// Inbound line cap, set to the executor's own capacity (CMD_INPUT_MAX chars
// plus NUL in ExecReq::line). At exactly this bound an over-long line is
// discarded whole rather than silently strncpy-truncated into a shorter,
// still-executable command. Also stops a newline-less byte stream from
// growing the accumulator without bound — a machine peer hits that far more
// easily than a human on the USB console.
static const size_t kUartLineCap = CMD_INPUT_MAX;

// Brute-force tier key for the link. The serial console uses "local"; a
// separate key means a CM5 retry storm can't lock out the bench operator and
// vice versa.
static const char* kUartLockoutKey = "uart";

// ---------------------------------------------------------------------------
// TX serialization (P2). The HAL's own mutex is per-write()-call only, and a
// text reply used to be TWO calls (blob, then newline) — any second-task
// writer could split it. All TX now goes through ONE write() call under this
// link-level mutex, so frames from cmd_exec_task (voicefetch) and the loop
// task's replies can never interleave mid-message. Created lazily on first
// start and never deleted — deleting a mutex a writer may be blocked on is
// exactly the lifecycle race the sPending mechanism exists to avoid.
static SemaphoreHandle_t sTxMutex = nullptr;

static void uartTxEnsureMutex() {
  if (sTxMutex == nullptr) sTxMutex = xSemaphoreCreateMutex();
}

// One serialized port write. Returns false if the link is down or the mutex
// is unavailable within timeoutMs (a full TX ring drains in ~45ms at 921600,
// so 1000ms means something is genuinely wrong, not slow).
static bool uartTxLocked(const uint8_t* data, size_t len, uint32_t timeoutMs = 1000) {
  if (!sStarted.load(std::memory_order_acquire) || sTxMutex == nullptr)
    return false;
  if (xSemaphoreTake(sTxMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
  bool ok = sStarted.load(std::memory_order_acquire);
  // Re-check under the mutex (stop() takes it too).
  if (ok) ok = UART_LINK_PORT.write(data, len) == len;
  xSemaphoreGive(sTxMutex);
  return ok;
}

static bool uartTxLockedForTransportSession(const uint8_t* data, size_t len,
                                            uint32_t expectedEpoch,
                                            uint32_t timeoutMs = 1000) {
  UartLifecycleGuard lifecycle(pdMS_TO_TICKS(timeoutMs));
  if (!lifecycle) return false;
  // This helper is specifically for a command bound to an admitted transport
  // incarnation. Zero means capture/admission failed, so fail closed rather
  // than silently degrading to an unfenced write.
  if (expectedEpoch == 0) return false;
  if (uartLinkTransportSessionEpoch() != expectedEpoch || !sTxMutex)
    return false;
  if (xSemaphoreTake(sTxMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) return false;
  const bool sessionMatches =
      sStarted.load(std::memory_order_acquire) &&
      uartLinkTransportSessionEpoch() == expectedEpoch;
  bool ok = sessionMatches;
  if (ok) ok = UART_LINK_PORT.write(data, len) == len;
  xSemaphoreGive(sTxMutex);
  return ok;
}

static bool uartWriteLine(const char* s) {
  // Single-write text line (≤ small status/nag strings; replies go via the
  // String path in uartProcessLine).
  char buf[256];
  size_t n = strlcpy(buf, s, sizeof(buf) - 1);
  if (n > sizeof(buf) - 2) n = sizeof(buf) - 2;
  buf[n] = '\n';
  return uartTxLocked((const uint8_t*)buf, n + 1);
}

static bool uartWriteLineForTransportSession(const char* s,
                                             uint32_t expectedEpoch,
                                             uint32_t timeoutMs = 1000) {
  // This helper is loop-task-only (all call sites are in uartProcessLine), so
  // fixed storage avoids putting a worst-case fan-report/usage reply on the
  // loop stack while preserving the one-write, non-interleaving guarantee.
  //
  // Stays in internal DRAM on purpose: this buffer IS the uart_write_bytes()
  // source, which esp_ringbuf memcpys into the driver ring inside its spinlock
  // (interrupts masked on the writing core). Same decision as sFrameWire below
  // — PSRAM there is correct but adds interrupt latency per reply line, for
  // 258 B. Not worth it. (Reviewed 2026-08-19.)
  static char buf[sizeof(sUartControlReply) + 2];
  size_t n = strlcpy(buf, s, sizeof(buf) - 1);
  if (n > sizeof(buf) - 2) n = sizeof(buf) - 2;
  buf[n] = '\n';
  buf[n + 1] = '\0';
  return uartTxLockedForTransportSession(
      reinterpret_cast<const uint8_t*>(buf), n + 1, expectedEpoch,
      timeoutMs);
}

// Rate-limited variant for replies a garbage stream could trigger per-line
// (break noise from an unpowered host looks like an endless byte flood).
static void uartWriteLineNagLimited(const char* s) {
  unsigned long now = millis();
  if (now - sLastNagMs < 2000UL) return;
  sLastNagMs = now;
  uartWriteLine(s);
}

bool uartLinkIsRunning() {
  return sStarted.load(std::memory_order_acquire);
}

// Effective baud: 0 means "board default"; anything else is clamped into
// [9600, UART_LINK_BAUD_MAX]. The floor matters — a stray `uartlinkbaud 300`
// would persist and make every reply take minutes of blocking wire time.
int uartLinkEffectiveBaud() {
  int baud = gSettings.uartLinkBaud > 0 ? gSettings.uartLinkBaud : UART_LINK_BAUD_DEFAULT;
  if (baud < 9600) baud = 9600;
  if (baud > UART_LINK_BAUD_MAX) baud = UART_LINK_BAUD_MAX;
  return baud;
}

void uartLinkRequestStart() {
  sPending.store(UL_PENDING_START, std::memory_order_release);
}
void uartLinkRequestStop() {
  sPending.store(UL_PENDING_STOP, std::memory_order_release);
}
void uartLinkRequestRestart() {
  sPending.store(UL_PENDING_RESTART, std::memory_order_release);
}

bool uartLinkStart() {
  if (sStarted.load(std::memory_order_acquire)) return true;
  uartTxEnsureMutex();
  int baud = uartLinkEffectiveBaud();
  // Buffer sizing (verified numbers): request/response traffic peaks at one
  // 2047-byte command inbound, one 4095-byte reply outbound. RX 4096 absorbs
  // ~45ms of full-rate line input at 921600 — ample for a client that waits
  // for its reply. TX 4096 lets a full reply land in the ring so the loop
  // task doesn't sit through the wire time; the ring ALWAYS drains at wire
  // rate (no flow control exists), so writes are bounded, never indefinite.
  // Both calls must precede begin() or they no-op.
  UART_LINK_PORT.setRxBufferSize(4096);
  UART_LINK_PORT.setTxBufferSize(4096);
  // begin() returns void in this core; operator bool() reports whether the
  // driver actually came up (pin attach can fail on a bad pin map).
  UART_LINK_PORT.begin((unsigned long)baud, SERIAL_8N1, UART_LINK_RX_PIN, UART_LINK_TX_PIN);
  if (!UART_LINK_PORT) {
    DEBUG_UART_LIFECYCLEF("[uartlink] begin FAILED: uart%d tx=%d rx=%d baud=%d",
                          UART_LINK_UART_NUM, UART_LINK_TX_PIN,
                          UART_LINK_RX_PIN, baud);
    return false;
  }
  // The Arduino HAL enables no pull on the RX pin (verified: the only rx
  // pullup call in esp32-hal-uart.c is dead code). Idle-high keeps an empty
  // header or a powered-down host from feeding break/garbage into the drain.
  gpio_pullup_en((gpio_num_t)UART_LINK_RX_PIN);
  // Publish only after the port, buffers, pins, and persistent TX mutex are
  // fully initialized. Cross-task frame writers use this as their acquire
  // gate before touching any of those objects.
  sStarted.store(true, std::memory_order_release);
  {
    UartLifecycleGuard lifecycle;
    if (!lifecycle) {
      sStarted.store(false, std::memory_order_release);
      UART_LINK_PORT.end();
      return false;
    }
    portENTER_CRITICAL(&sUartSessionMux);
    uartLinkRotateTransportLocked(true);
    portEXIT_CRITICAL(&sUartSessionMux);
  }
  DEBUG_UART_LIFECYCLEF("[uartlink] started: uart%d tx=%d rx=%d baud=%d",
                        UART_LINK_UART_NUM, UART_LINK_TX_PIN,
                        UART_LINK_RX_PIN, baud);
  return true;
}

static bool uartLinkStopPhysical() {
  // Close authorization immediately, even if physical teardown must retry.
  // This makes every session-fenced producer fail at its next boundary.
  uartLinkSessionCleared(UartLinkSessionClearReason::LinkStop);
  if (!sStarted.load(std::memory_order_acquire)) return true;
  // Order matters: close the drain's gate BEFORE tearing the port down, so a
  // tick that already passed the entry check can't operate on a dead port.
  // Take the TX mutex across the teardown so a cross-task frame writer that
  // already passed its sStarted check finishes its write before end() frees
  // the driver underneath it (P2 lifecycle rule).
  bool locked = sTxMutex == nullptr;
  if (sTxMutex != nullptr) {
    locked = xSemaphoreTake(sTxMutex, pdMS_TO_TICKS(2000)) == pdTRUE;
  }
  if (!locked) {
    // Never end the UART driver beneath a writer or give a mutex this task
    // does not own. The loop retries the pending transition next lap.
    DEBUG_UART_LIFECYCLEF(
        "[uartlink] stop deferred — TX writer did not quiesce");
    return false;
  }
  sStarted.store(false, std::memory_order_release);
  UART_LINK_PORT.end();
  if (sTxMutex != nullptr) xSemaphoreGive(sTxMutex);
  sUartCLI = "";
  sUartLineTransportEpoch = 0;
  sDiscardingLine = false;
  sLastInteractionMs.store(0, std::memory_order_release);
  DEBUG_UART_LIFECYCLEF("[uartlink] stopped");
  return true;
}

void uartLinkStop() {
  if (!uartLinkStopPhysical()) uartLinkRequeueIfNone(UL_PENDING_STOP);
}

void uartLinkInitFromSettings() {
  if (!gSettings.uartLinkEnabled) {
    DEBUG_UART_LIFECYCLEF("[uartlink] boot: disabled by setting");
    return;
  }
  (void)uartLinkStart();
}

const char* uartLinkStatusLine() {
  EXT_RAM_BSS_ATTR static char buf[384];  // PSRAM: cmd_exec-only, copied into the executor's result String
  char sessionUser[kUartSessionUserMax + 1];
  const bool sessionAuthed = uartLinkSessionSnapshot(
      sessionUser, sizeof(sessionUser));
  const UartLinkSessionDiagnostics session = uartLinkSessionDiagnostics();
  const Cm5PresenceSnapshot cm5 =
      cm5PresenceSnapshotForSession(session.activeEpoch, millis());
  int baud = uartLinkEffectiveBaud();
  snprintf(buf, sizeof(buf),
           "UART link: %s enabled=%d epoch=%lu last_epoch=%lu last_event=%s "
           "uart%d tx=%d rx=%d baud=%d auth=%s user=%s idle=%lumin "
           "cm5=%s cm5_fresh=%d cm5_age_ms=%lu cm5_task=%s",
           sStarted.load(std::memory_order_acquire) ? "running" : "stopped",
           gSettings.uartLinkEnabled ? 1 : 0,
           (unsigned long)session.activeEpoch,
           (unsigned long)session.lastEpoch,
           session.lastEvent,
           UART_LINK_UART_NUM, UART_LINK_TX_PIN, UART_LINK_RX_PIN, baud,
           gSettings.uartRequireAuth ? "required" : "off",
           sessionAuthed ? sessionUser : "(none)",
           (unsigned long)gSettings.sessionIdleUart,
           cm5PresenceModeName(cm5.mode), cm5.fresh ? 1 : 0,
           (unsigned long)cm5.ageMs,
           cm5.taskRunning ? "running" : "dormant");
  return buf;
}

// Process one complete, trimmed line. Mirrors the serial drain
// (HardwareOne.cpp loop section 6) with two deliberate differences: every
// reply is written directly to the link port (this channel has no sink), and
// there is no "$ " prompt (machines parse the OK:/Error: contract, not
// prompts).
static void uartProcessLine(String& cmd, uint32_t admittedTransportEpoch) {
  // This is a short admission check, not a lifecycle hold: the loop task must
  // never wait on cmd_exec while owning the UART lifecycle mutex.
  if (admittedTransportEpoch == 0 ||
      uartLinkTransportSessionEpoch() != admittedTransportEpoch) return;
  // Idle logout — drop a stale session before processing this line; the line
  // then falls through to the login gate and is rejected, exactly like serial.
  if (uartLinkSessionEpoch() != 0 &&
      sessionIdleExpired(
          SOURCE_UART, sLastInteractionMs.load(std::memory_order_acquire))) {
    const uint32_t clearedTransportEpoch =
        uartLinkSessionCleared(UartLinkSessionClearReason::IdleTimeout,
                               admittedTransportEpoch);
    if (clearedTransportEpoch == 0) return;
    (void)uartWriteLineForTransportSession(
        "[uart] Signed out due to inactivity. Please log in again.",
        clearedTransportEpoch);
    return;  // the completed line belonged to the expired incarnation
  }

  // A current host honors the marked direct-renew cadence; legacy host/firmware
  // pairs retain a slower compatibility cadence. Once the initial command has
  // created the TX task and lease, handle healthy exact renewals like the CM5
  // heartbeat: on this UART control plane, before CommandArgs, cmd_exec, feed
  // allocation, and CM5 command-busy accounting. The handler returns
  // NotHandled for acquisition/repair/mismatch so those state changes still
  // use the fully authorized registry path below.
  // Shared caller-owned scratch for both mutually exclusive control-plane
  // intrinsics. Keeping one array avoids adding two 256-byte locals to this
  // main-loop call frame.
  uint32_t liveNamedEpoch = 0;
  uint32_t liveTransportEpoch = 0;
  bool liveControlAllowed = false;
  (void)uartLinkSessionSnapshot(
      nullptr, 0, &liveNamedEpoch, &liveTransportEpoch,
      &liveControlAllowed);
  if (liveTransportEpoch != admittedTransportEpoch) return;
  if (liveAudioHandleReadyIntrinsic(
          cmd.c_str(), liveNamedEpoch, liveControlAllowed, sUartControlReply,
          sizeof(sUartControlReply)) ==
      LiveAudioReadyIntrinsicResult::Handled) {
    // A concurrent logout/re-login/revoke may invalidate the request after
    // renewal.  The stored lease remains bound to the old named epoch, while
    // this exact transport fence prevents its ACK crossing into the new one.
    const bool sent = uartWriteLineForTransportSession(
        sUartControlReply, liveTransportEpoch);
    DEBUG_UART_CONTROLF("[UART-CTRL] RX liveaudio.ready epoch=%lu -> %s reply=%s",
                        static_cast<unsigned long>(liveNamedEpoch),
                        strncmp(sUartControlReply, "OK", 2) == 0
                            ? "OK" : "ERROR",
                        sent ? "sent" : "dropped");
    return;
  }

  // Bare in-band login is an intrinsic, NOT part of the auth gate: with
  // uartRequireAuth=0 the gate below is skipped entirely, and cmd_login
  // refuses SOURCE_UART callers (it would otherwise mint a session on the
  // physical console), so without this the host could never establish a named
  // session in auth-off mode. A fourth token is an explicit target and is
  // allowed to reach the registry only after this UART already has a named,
  // non-Guest login.
  CommandArgs uartLineArgs(cmd);
  const bool loginVerb =
      uartLineArgs.count() > 0 &&
      uartLineArgs.arg(0).equalsIgnoreCase("login");
  const bool wantsLocalLogin =
      loginVerb && !uartLineArgs.unterminatedQuote() &&
      uartLineArgs.count() == 3;
  if (wantsLocalLogin ||
      (gSettings.uartRequireAuth && uartLinkSessionEpoch() == 0)) {
    if (wantsLocalLogin) {
        const String u = uartLineArgs.arg(1);
        const String p = uartLineArgs.arg(2);
#if ENABLE_HTTP_SERVER
        unsigned long lockoutRemainingMs = 0;
        bool locked = isLoginLocked(kUartLockoutKey, &lockoutRemainingMs);
#else
        bool locked = false;
#endif
        if (locked) {
#if ENABLE_HTTP_SERVER
          char msg[80];
          snprintf(msg, sizeof(msg), "Error: login locked out. Retry in %lu seconds.",
                   lockoutRemainingMs / 1000UL);
          uartWriteLine(msg);
          recordLoginAttempt(SOURCE_UART, u, kUartLockoutKey, false, "Locked out");
#endif
        } else {
          bool loginSucceeded = false;
          uint32_t loginTransportEpoch = 0;
          // Keep the auth database stable from password validation through
          // exact UART session publication. Credential writers take this
          // same reentrant filesystem lock and revoke only after releasing
          // it, closing the old-password-validation/new-session race.
          {
            FsLockGuard authGuard("uart.inband_login");
            const bool authStoreHeld =
                authGuard.held || isFsLockedByCurrentTask();
            if (authStoreHeld && isValidUser(u, p)) {
              const bool sourceStillCurrent =
                  uartLinkTransportSessionBeginUse(admittedTransportEpoch);
              if (sourceStillCurrent) {
#if ENABLE_HTTP_SERVER
                clearLoginAttempts(kUartLockoutKey);
#endif
                const uint32_t namedEpoch =
                    publishUartAccountSession(u, &loginTransportEpoch);
                loginSucceeded = namedEpoch != 0 && loginTransportEpoch != 0;
                uartLinkTransportSessionEndUse();
              }
            }
          }
          if (loginSucceeded) {
          // Audit + event are OUTSIDE the guard: the security trail must not
          // depend on the web server being compiled in.
          recordLoginAttempt(SOURCE_UART, u, kUartLockoutKey, true, "Login successful");
          systemEventPost(SYSEVT_LOGIN_OK, u.c_str(), "uart");
          char msg[96];
          snprintf(msg, sizeof(msg), "OK: logged in as %s%s", u.c_str(),
                   isAdminUser(u) ? " (admin)" : "");
          (void)uartWriteLineForTransportSession(
              msg, loginTransportEpoch);
          } else {
#if ENABLE_HTTP_SERVER
            recordFailedLogin(kUartLockoutKey);
#endif
            recordLoginAttempt(SOURCE_UART, u, kUartLockoutKey, false, "Invalid credentials");
            systemEventPost(SYSEVT_LOGIN_FAIL, u.c_str(), "uart");
            uartWriteLine("Error: authentication failed");
          }
        }
    } else if (loginVerb) {
      uartWriteLine("Error: sign in first with bare: login <username> <password>");
    } else if (cmd.length() > 0) {
      // Rate-limited: break/garbage from an unpowered or resetting host
      // arrives as a line flood; one nag per window is plenty for a machine.
      uartWriteLineNagLimited("Error: authentication required. Use: login <username> <password>");
    }
    return;  // unauthenticated lines never fall through to the registry
  }

  // Authenticated (or auth disabled): in-band intrinsics, then the registry.
  if (uartLineArgs.count() == 1 &&
      uartLineArgs.arg(0).equalsIgnoreCase("logout")) {
    const uint32_t logoutTransportEpoch =
        uartLinkSessionCleared(UartLinkSessionClearReason::ExplicitLogout,
                               admittedTransportEpoch);
    if (logoutTransportEpoch == 0) return;
    (void)uartWriteLineForTransportSession(
        "OK: logged out", logoutTransportEpoch);
    return;
  }
  if (uartLineArgs.count() == 1 &&
      uartLineArgs.arg(0).equalsIgnoreCase("whoami")) {
    char msg[96];
    char sessionUser[kUartSessionUserMax + 1];
    uint32_t whoamiTransportEpoch = 0;
    const bool sessionAuthed = uartLinkSessionSnapshot(
        sessionUser, sizeof(sessionUser), nullptr, &whoamiTransportEpoch);
    if (whoamiTransportEpoch != admittedTransportEpoch) return;
    const char* name = sessionAuthed && sessionUser[0]
                           ? sessionUser : "AuthBypass";
    const String userName(name);
    snprintf(msg, sizeof(msg), "OK: %s%s", name,
             (sessionAuthed && isAdminUser(userName)) ? " (admin)" : "");
    (void)uartWriteLineForTransportSession(
        msg, whoamiTransportEpoch);
    return;
  }
  char heartbeatUser[kUartSessionUserMax + 1];
  uint32_t heartbeatNamedEpoch = 0;
  uint32_t heartbeatTransportEpoch = 0;
  bool heartbeatAllowed = false;
  (void)uartLinkSessionSnapshot(
      heartbeatUser, sizeof(heartbeatUser), &heartbeatNamedEpoch,
      &heartbeatTransportEpoch, &heartbeatAllowed);
  if (heartbeatTransportEpoch != admittedTransportEpoch) return;
  if (cm5PresenceHandleHeartbeatIntrinsic(
          cmd.c_str(), heartbeatNamedEpoch, heartbeatAllowed,
          sUartControlReply, sizeof(sUartControlReply)) ==
      Cm5HeartbeatIntrinsicResult::Handled) {
    // A concurrent logout/re-login/revoke may invalidate the heartbeat after
    // it was parsed. The record is already fail-closed by named-epoch
    // comparison; this second fence also prevents its ACK from crossing into
    // the replacement physical command session.
    const bool sent = uartWriteLineForTransportSession(
        sUartControlReply, heartbeatTransportEpoch);
    DEBUG_UART_CONTROLF("[UART-CTRL] RX cm5.heartbeat epoch=%lu -> %s reply=%s",
                        static_cast<unsigned long>(heartbeatNamedEpoch),
                        strncmp(sUartControlReply, "OK", 2) == 0
                            ? "OK" : "ERROR",
                        sent ? "sent" : "dropped");
    return;
  }
  // `cm5 time set ...`: the CM5's periodic clock push. Same control-plane
  // placement, auth snapshot, and transport-epoch fence as the heartbeat —
  // before CommandArgs/cmd_exec/feed/busy accounting. The intrinsic only
  // validates + stashes; cm5TimeSyncTick() applies it on the loop.
  if (cm5TimeHandleTimeIntrinsic(
          cmd.c_str(), heartbeatNamedEpoch, heartbeatAllowed,
          sUartControlReply, sizeof(sUartControlReply)) ==
      Cm5TimeIntrinsicResult::Handled) {
    const bool sent = uartWriteLineForTransportSession(
        sUartControlReply, heartbeatTransportEpoch);
    DEBUG_UART_CONTROLF("[UART-CTRL] RX cm5.time epoch=%lu -> %s reply=%s",
                        static_cast<unsigned long>(heartbeatNamedEpoch),
                        strncmp(sUartControlReply, "OK", 2) == 0
                            ? "OK" : "ERROR",
                        sent ? "sent" : "dropped");
    return;
  }
  // `cm5 linkhealth 1 <k=v> ...`: the host's fault tally, mirrored here so the
  // counters are readable from this device's CLI. Same control-plane placement
  // as the heartbeat and the clock push — a periodic diagnostic must not take
  // the command lock or write a durable audit line every 30s. Inert by
  // construction: nothing in firmware reads the stored values back.
  if (cm5LinkHealthHandleIntrinsic(
          cmd.c_str(), heartbeatNamedEpoch, heartbeatAllowed,
          sUartControlReply, sizeof(sUartControlReply)) ==
      Cm5LinkHealthIntrinsicResult::Handled) {
    const bool sent = uartWriteLineForTransportSession(
        sUartControlReply, heartbeatTransportEpoch);
    DEBUG_UART_CONTROLF("[UART-CTRL] RX cm5.linkhealth epoch=%lu -> %s reply=%s",
                        static_cast<unsigned long>(heartbeatNamedEpoch),
                        strncmp(sUartControlReply, "OK", 2) == 0
                            ? "OK" : "ERROR",
                        sent ? "sent" : "dropped");
    return;
  }
  // Keyboard dictation is a CM5 control protocol, not a human CLI command.
  // Keep it off cmd_exec/feed/audit just like heartbeat and streamed LLM
  // callbacks. Pin the physical incarnation across state mutation and reply;
  // the handler stores/compares only the coherent named epoch.
  if (dictationIsUartProtocolLine(cmd.c_str())) {
    const bool dictationSessionPinned =
        uartLinkTransportSessionBeginUse(heartbeatTransportEpoch);
    if (!dictationSessionPinned) return;
    uint32_t dictationNamedEpoch = 0;
    uint32_t dictationTransportEpoch = 0;
    bool dictationAllowed = false;
    (void)uartLinkSessionSnapshot(
        nullptr, 0, &dictationNamedEpoch, &dictationTransportEpoch,
        &dictationAllowed);
    if (dictationTransportEpoch != heartbeatTransportEpoch) {
      uartLinkTransportSessionEndUse();
      return;
    }
    const DictationUartIntrinsicResult dictationResult =
        dictationHandleUartIntrinsic(
            cmd.c_str(), dictationNamedEpoch, dictationAllowed,
            sUartControlReply, sizeof(sUartControlReply));
    if (dictationResult != DictationUartIntrinsicResult::Handled) {
      snprintf(sUartControlReply, sizeof(sUartControlReply),
               "Error: dictation protocol is not available");
    }
    const bool sent = uartWriteLineForTransportSession(
        sUartControlReply, dictationTransportEpoch);
    uartLinkTransportSessionEndUse();
    DEBUG_UART_CONTROLF(
        "[UART-CTRL] RX dictate epoch=%lu -> %s reply=%s",
        static_cast<unsigned long>(dictationNamedEpoch),
        strncmp(sUartControlReply, "OK", 2) == 0 ? "OK" : "ERROR",
        sent ? "sent" : "dropped");
    return;
  }
  const bool powerCallback =
      ENABLE_RASPBERRY_PI_HOST_POWER && uartLineArgs.count() >= 3 &&
      uartLineArgs.arg(0).equalsIgnoreCase("cm5") &&
      uartLineArgs.arg(1).equalsIgnoreCase("power") &&
      (uartLineArgs.arg(2).equalsIgnoreCase("ack") ||
       uartLineArgs.arg(2).equalsIgnoreCase("report"));
  const bool fanCallback =
      ENABLE_RASPBERRY_PI_HOST_FAN && uartLineArgs.count() >= 3 &&
      uartLineArgs.arg(0).equalsIgnoreCase("cm5") &&
      uartLineArgs.arg(1).equalsIgnoreCase("fan") &&
      (uartLineArgs.arg(2).equalsIgnoreCase("ack") ||
       uartLineArgs.arg(2).equalsIgnoreCase("report"));
#if ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_CM5
  // `cm5 llm …` — model catalog and streamed answer chunks. Consumed here, on
  // the same control plane and for the same reason as power/fan: a generation
  // pushes many chunks, and routing each through cmd_exec would take the
  // command lock and write a durable audit line per token group.
  //
  // Detected on the RAW line rather than through uartLineArgs: the tail of a
  // push is payload, not arguments, and the escaping contract that protects it
  // (System_LLMCm5.h) exists precisely so no tokenizer touches it.
  if (cm5LlmIsCallbackLine(cmd.c_str())) {
    const bool llmSessionPinned =
        uartLinkTransportSessionBeginUse(heartbeatTransportEpoch);
    if (!llmSessionPinned) return;
    uint32_t llmNamedEpoch = 0;
    uint32_t llmTransportEpoch = 0;
    bool llmAllowed = false;
    (void)uartLinkSessionSnapshot(
        nullptr, 0, &llmNamedEpoch, &llmTransportEpoch, &llmAllowed);
    if (llmTransportEpoch != heartbeatTransportEpoch) {
      uartLinkTransportSessionEndUse();
      return;
    }
    const Cm5LlmCallbackResult llmResult = cm5LlmHandleCallbackIntrinsic(
        cmd.c_str(), llmNamedEpoch, llmAllowed, sUartControlReply,
        sizeof(sUartControlReply));
    if (llmResult != Cm5LlmCallbackResult::Handled) {
      uartLinkTransportSessionEndUse();
      return;
    }
    const bool llmSent = uartWriteLineForTransportSession(
        sUartControlReply, llmTransportEpoch, 5000);
    uartLinkTransportSessionEndUse();
    DEBUG_UART_CONTROLF("[UART-CTRL] RX cm5.llm epoch=%lu -> %s reply=%s",
                        static_cast<unsigned long>(llmNamedEpoch),
                        strncmp(sUartControlReply, "OK", 2) == 0 ? "OK" : "ERROR",
                        llmSent ? "sent" : "dropped");
    return;
  }
#endif
  if (powerCallback || fanCallback) {
    // Hold the exact physical command-session incarnation across callback
    // state mutation and reply admission. Login/logout/revocation takes this
    // same recursive lifecycle mutex, so an old callback cannot mutate under
    // one identity and reply into its replacement. Only the four callback
    // prefixes take this lock; ordinary UART commands proceed directly to the
    // command boundary below.
    const bool callbackSessionPinned =
        uartLinkTransportSessionBeginUse(heartbeatTransportEpoch);
    if (!callbackSessionPinned) return;
    uint32_t callbackNamedEpoch = 0;
    uint32_t callbackTransportEpoch = 0;
    bool callbackAllowed = false;
    (void)uartLinkSessionSnapshot(
        nullptr, 0, &callbackNamedEpoch, &callbackTransportEpoch,
        &callbackAllowed);
    if (callbackTransportEpoch != heartbeatTransportEpoch) {
      uartLinkTransportSessionEndUse();
      return;
    }
    const Cm5HostCallbackIntrinsicResult callbackResult =
        cm5HostControlHandleCallbackIntrinsic(
            cmd.c_str(), callbackNamedEpoch, callbackAllowed,
            sUartControlReply, sizeof(sUartControlReply));
    if (callbackResult != Cm5HostCallbackIntrinsicResult::Handled) {
      uartLinkTransportSessionEndUse();
      return;
    }
    // Power/fan callback state is already bound to either the request ID or
    // the exact named epoch. The physical transport fence keeps its ACK from
    // crossing a concurrent re-login, just like heartbeat and ready above.
    const bool sent = uartWriteLineForTransportSession(
        sUartControlReply, callbackTransportEpoch, 5000);
    uartLinkTransportSessionEndUse();
    const char* callbackName =
        powerCallback
            ? (uartLineArgs.arg(2).equalsIgnoreCase("ack")
                   ? "cm5.power.ack" : "cm5.power.report")
            : (uartLineArgs.arg(2).equalsIgnoreCase("ack")
                   ? "cm5.fan.ack" : "cm5.fan.report");
    DEBUG_UART_CONTROLF("[UART-CTRL] RX %s epoch=%lu -> %s reply=%s",
                        callbackName,
                        static_cast<unsigned long>(callbackNamedEpoch),
                        strncmp(sUartControlReply, "OK", 2) == 0
                            ? "OK" : "ERROR",
                        sent ? "sent" : "dropped");
    return;
  }
  if (cmd.length() == 0) return;

  // CM5 and live-audio status/capabilities are read-only machine
  // housekeeping. They remain ordinary registry commands, but do not extend
  // application-busy state or evict useful human history. A healthy ready
  // renewal already returned through the intrinsic above; an initial/repair
  // ready intentionally reaches this ordinary, busy-accounted path.
  const bool cm5ProtocolCommand = cm5PresenceIsProtocolCommand(cmd.c_str());
  const bool liveAudioHousekeeping =
      liveAudioIsHousekeepingCommand(cmd.c_str());
  const bool hostHousekeeping = cm5ProtocolCommand || liveAudioHousekeeping;
  appendCommandToFeed("uart", cmd, String(), String());

  AuthContext actx;
  actx.transport = SOURCE_UART;
  // AuthBypass sentinel when auth is off and nobody logged in — same audit
  // convention as the serial/OLED drains (reserved username, non-admin).
  char sessionUser[kUartSessionUserMax + 1];
  uint32_t commandSessionEpoch = 0;
  uint32_t commandTransportEpoch = 0;
  const bool sessionAuthed = uartLinkSessionSnapshot(
      sessionUser, sizeof(sessionUser), &commandSessionEpoch,
      &commandTransportEpoch);
  if (commandTransportEpoch != admittedTransportEpoch) return;
  actx.user = sessionAuthed && sessionUser[0]
                  ? String(sessionUser) : String("AuthBypass");
  actx.ip = kUartLockoutKey;
  actx.path = "uart";
  actx.opaque = nullptr;   // not an HTTP request; matches the MQTT pattern

  Command uc;
  uc.line = cmd;
  uc.ctx.origin = ORIGIN_UART;
  uc.ctx.auth = actx;
  uc.ctx.id = (uint32_t)millis();
  uc.ctx.timestampMs = (uint32_t)millis();
  uc.ctx.transportSessionEpoch = commandTransportEpoch;
  uc.ctx.behaviorFlags |= COMMAND_CONTEXT_REQUIRE_LIVE_SESSION;
  uc.ctx.behaviorFlags |= COMMAND_CONTEXT_MODE_INDEPENDENT;
  // MSG_ROUTE_FILE only: the reply below is the one delivery this channel
  // gets; broadcast lines go to the audit log, never the wire. captureOutput
  // stays false — capture PREPENDS streamed lines to the return value, which
  // breaks the OK:/Error: framing machines parse (this is why MQTT is false).
  uc.ctx.outputMask = MSG_ROUTE_FILE;
  uc.ctx.validateOnly = false;
  uc.ctx.captureOutput = false;
  uc.ctx.replyHandle = nullptr;
  uc.ctx.httpReq = nullptr;

  String out;
  if (!hostHousekeeping) cm5PresenceCommandStarted(commandSessionEpoch);
  bool ok = submitAndExecuteSync(uc, out);
  bool replyAdmitted = false;
  if (out.length()) {
    // ONE write call for blob + newline: with cross-task frame writers live
    // (voicefetch), a two-call reply could be split mid-message.
    // concat() reports allocation failure; without the newline the peer has
    // no complete reply, so do not write it or grant post-command grace.
    if (out.concat('\n')) {
      replyAdmitted = uartTxLockedForTransportSession(
          (const uint8_t*)out.c_str(), out.length(),
          commandTransportEpoch,
          5000);  // replies are load-bearing; wait out a slow frame
    }
  } else {
    // Never leave the host hanging: an empty result still gets a status line.
    char fallback[32];
    size_t n = strlcpy(fallback, ok ? "OK" : "Error: command failed",
                       sizeof(fallback) - 1);
    fallback[n++] = '\n';
    replyAdmitted = uartTxLockedForTransportSession(
        reinterpret_cast<const uint8_t*>(fallback), n,
        commandTransportEpoch, 1000);
  }
  // Keep the bounded command extension through physical reply admission.
  // The post-command grace begins only now, giving the CM5 heartbeat actor a
  // chance to acquire its own serialized Session lock after this reply lands.
  if (!hostHousekeeping)
    cm5PresenceCommandFinished(commandSessionEpoch, replyAdmitted);
}

void uartLinkTick() {
  // Apply any lifecycle request from a command handler. Doing it here, on the
  // loop task that owns the drain, is what makes `uartlink off` / a baud
  // change safe while bytes are in flight.
  uint8_t pending = sPending.exchange(UL_PENDING_NONE,
                                      std::memory_order_acq_rel);
  if (pending != UL_PENDING_NONE) {
    switch (pending) {
      case UL_PENDING_START:
        (void)uartLinkStart();
        // A STOP/RESTART may have been posted while begin() ran. Honor it on
        // the next lap before accepting any input under the new port state.
        return;
      case UL_PENDING_STOP:
        if (!uartLinkStopPhysical())
          uartLinkRequeueIfNone(UL_PENDING_STOP);
        // Never resume draining in the same lap as a stop attempt. If the TX
        // writer did not quiesce, authorization is already closed and the
        // physical teardown remains pending for a later lap.
        return;
      case UL_PENDING_RESTART:
        if (uartLinkStopPhysical()) (void)uartLinkStart();
        else uartLinkRequeueIfNone(UL_PENDING_RESTART);
        // A request posted while stopPhysical() waited remains in sPending;
        // returning prevents this lap from accepting bytes under either the
        // old or freshly restarted session.
        return;
      default: break;
    }
  }

  if (!sStarted.load(std::memory_order_acquire)) return;
  // Park while the setup wizard owns the CLI: the wizard occupies cmd_exec
  // for its whole run, so a submitted link command would stall this loop task
  // in submitAndExecuteSync (up to 60s) and then time out anyway.
  if (gWizardOwnsSerial) return;

  while (UART_LINK_PORT.available()) {
    char c = (char)UART_LINK_PORT.read();
    if (c == '\r') continue;
    const uint32_t liveTransportEpoch = uartLinkTransportSessionEpoch();
    if (sUartLineTransportEpoch != 0 &&
        sUartLineTransportEpoch != liveTransportEpoch) {
      // Never splice bytes received under two physical command-session
      // incarnations. This also scrubs a partially typed password.
      sUartCLI = "";
      sDiscardingLine = false;
      sUartLineTransportEpoch = 0;
    }
    if (c == '\n') {
      if (sDiscardingLine) {
        // Tail of an over-long line — drop it whole. Executing the residue
        // would run an arbitrary fragment as a command under the live session.
        sDiscardingLine = false;
        sUartCLI = "";
        sUartLineTransportEpoch = 0;
        continue;
      }
      if (sUartLineTransportEpoch == 0 ||
          sUartLineTransportEpoch != liveTransportEpoch) {
        sUartCLI = "";
        sUartLineTransportEpoch = 0;
        continue;
      }
      String cmd = sUartCLI;
      sUartCLI = "";
      const uint32_t admittedTransportEpoch = sUartLineTransportEpoch;
      sUartLineTransportEpoch = 0;
      cmd.trim();
      uartProcessLine(cmd, admittedTransportEpoch);
      // Refresh the idle clock on any completed line while authenticated —
      // one stamp covers both a fresh login and a subsequent command.
      if (uartLinkSessionEpoch() != 0)
        sLastInteractionMs.store(sessionStampNow(),
                                 std::memory_order_release);
      break;  // at most one command per loop() lap, same as the serial drain
    } else {
      if (sDiscardingLine) continue;
      if (sUartCLI.length() >= kUartLineCap) {
        sUartCLI = "";
        sUartLineTransportEpoch = 0;
        sDiscardingLine = true;   // swallow the rest, including the newline
        uartWriteLineNagLimited("Error: line too long — discarded");
        continue;
      }
      if (sUartLineTransportEpoch == 0)
        sUartLineTransportEpoch = liveTransportEpoch;
      sUartCLI += c;
    }
  }
}

// ---------------------------------------------------------------------------
// P2 binary frame layer. Wire format in System_UartLink.h — the CM5 client
// (cm5/ai-service link/protocol.py) mirrors it byte-for-byte.

// CRC16-CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xor-out.
// Implemented bitwise here AND in the Python client from the same spec —
// deliberately not esp_rom_crc (whose reflected variants invite mismatch).
static uint16_t uartCrc16(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF) {
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// COBS encode: output holds no 0x00; worst case adds 1 byte per 254 + 1.
// Returns encoded length. dst must hold len + len/254 + 2.
static size_t uartCobsEncode(const uint8_t* src, size_t len, uint8_t* dst) {
  size_t out = 0, codePos = 0;
  uint8_t code = 1;
  dst[out++] = 0;  // placeholder for first code byte (codePos = 0)
  for (size_t i = 0; i < len; i++) {
    if (src[i] == 0) {
      dst[codePos] = code;
      codePos = out++;
      code = 1;
    } else {
      dst[out++] = src[i];
      if (++code == 0xFF) {
        dst[codePos] = code;
        codePos = out++;
        code = 1;
      }
    }
  }
  dst[codePos] = code;
  return out;
}

// Frame assembly buffers. Static (not stack): body+encoded is ~2.2KB, too
// heavy for cmd_exec/producer stacks. Guarded by the TX mutex — the frame is
// built AND written under one hold so two producers can't corrupt the scratch.
// sFrameBody lives in PSRAM: it is read/written only by flash-resident CPU code
// under the TX mutex, never inside a spinlock, never handed to the UART driver
// (it is CRC'd and COBS-encoded into sFrameWire first). Verified 2026-08-19.
//
// sFrameWire deliberately STAYS internal. uart_write_bytes() memcpys it into
// the driver ring inside the esp_ringbuf spinlock — interrupts masked on the
// writing core — in <=1 KB chunks. A PSRAM source there is correct but
// lengthens worst-case interrupt latency on core 0 during voicefetch, and this
// link is already marginal. ~1 KB is not worth that.
EXT_RAM_BSS_ATTR static uint8_t sFrameBody[5 + UARTLINK_FRAME_MAX_PAYLOAD + 2];
static uint8_t sFrameWire[2 + sizeof(sFrameBody) + sizeof(sFrameBody) / 254 + 2];

static bool uartLinkWriteFrameWithWait(uint8_t type, uint16_t seq,
                                       const uint8_t* payload, size_t len,
                                       TickType_t mutexWaitTicks,
                                       uint32_t expectedSessionEpoch = 0) {
  if (len > UARTLINK_FRAME_MAX_PAYLOAD) return false;
  UartLifecycleGuard lifecycle(mutexWaitTicks);
  if (!lifecycle) return false;
  // Session gate, per-frame: a revoked/blocked session stops the stream at
  // the next frame boundary (bounds leakage; see plan §7). Unconditional on
  // the active session epoch — every bulk/live producer requires a real login
  // START even when uartRequireAuth=0, so gating frames the same way keeps the
  // in BOTH auth modes.
  if (!sStarted.load(std::memory_order_acquire) || sTxMutex == nullptr)
    return false;
  const uint32_t activeEpoch = uartLinkSessionEpoch();
  if (activeEpoch == 0) return false;
  if (expectedSessionEpoch != 0 && activeEpoch != expectedSessionEpoch)
    return false;

  if (xSemaphoreTake(sTxMutex, mutexWaitTicks) != pdTRUE) return false;
  // Re-check both lifecycle and authorization under the same mutex that
  // linearizes the physical write. Revocation can still race one already
  // admitted frame, but it cannot sit behind another writer and then transmit
  // using a stale pre-mutex authorization observation.
  const uint32_t lockedEpoch = uartLinkSessionEpoch();
  bool ok = sStarted.load(std::memory_order_acquire) && lockedEpoch != 0;
  if (ok && expectedSessionEpoch != 0) {
    ok = lockedEpoch == expectedSessionEpoch;
  }
  if (ok) {
    size_t n = 0;
    sFrameBody[n++] = type;
    sFrameBody[n++] = (uint8_t)(seq & 0xFF);
    sFrameBody[n++] = (uint8_t)(seq >> 8);
    sFrameBody[n++] = (uint8_t)(len & 0xFF);
    sFrameBody[n++] = (uint8_t)(len >> 8);
    memcpy(&sFrameBody[n], payload, len);
    n += len;
    uint16_t crc = uartCrc16(sFrameBody, n);
    sFrameBody[n++] = (uint8_t)(crc & 0xFF);
    sFrameBody[n++] = (uint8_t)(crc >> 8);

    size_t w = 0;
    sFrameWire[w++] = 0x00;                                   // SOF
    w += uartCobsEncode(sFrameBody, n, &sFrameWire[w]);
    sFrameWire[w++] = 0x00;                                   // EOF
    if (mutexWaitTicks == 0 &&
        static_cast<size_t>(UART_LINK_PORT.availableForWrite()) < w) {
      ok = false;  // try-write callers never wait for TX-buffer space
    } else {
      UART_LINK_PORT.write(sFrameWire, w);                    // ONE write
    }
  }
  xSemaphoreGive(sTxMutex);
  return ok;
}

bool uartLinkWriteFrame(uint8_t type, uint16_t seq,
                        const uint8_t* payload, size_t len) {
  return uartLinkWriteFrameWithWait(type, seq, payload, len,
                                    pdMS_TO_TICKS(1000));
}

bool uartLinkWriteFrameForSession(uint32_t sessionEpoch,
                                  uint8_t type, uint16_t seq,
                                  const uint8_t* payload, size_t len) {
  if (sessionEpoch == 0) return false;
  return uartLinkWriteFrameWithWait(type, seq, payload, len,
                                    pdMS_TO_TICKS(1000), sessionEpoch);
}

bool uartLinkTryWriteFrame(uint8_t type, uint16_t seq,
                           const uint8_t* payload, size_t len) {
  return uartLinkWriteFrameWithWait(type, seq, payload, len, 0);
}

bool uartLinkTryWriteFrameForSession(uint32_t sessionEpoch,
                                     uint8_t type, uint16_t seq,
                                     const uint8_t* payload, size_t len) {
  if (sessionEpoch == 0) return false;
  return uartLinkWriteFrameWithWait(type, seq, payload, len, 0,
                                    sessionEpoch);
}

// Spontaneous event push. Own seq counter so the host can log gaps. Event
// producers run on more than one task, so protect the read/modify/write itself;
// the frame TX mutex only serializes bytes after the sequence argument has
// already been evaluated.
static uint16_t sEvtSeq = 0;
static portMUX_TYPE sEvtSeqMux = portMUX_INITIALIZER_UNLOCKED;

static uint16_t uartLinkNextEventSeq() {
  portENTER_CRITICAL(&sEvtSeqMux);
  const uint16_t seq = ++sEvtSeq;
  portEXIT_CRITICAL(&sEvtSeqMux);
  return seq;
}

bool uartLinkPushEvent(const char* text) {
  if (text == nullptr || text[0] == '\0') return false;
  const size_t len = strlen(text);
  if (len > UARTLINK_FRAME_MAX_PAYLOAD) return false;
  return uartLinkWriteFrame(UARTLINK_FRAME_EVT, uartLinkNextEventSeq(),
                            (const uint8_t*)text, len);
}

bool uartLinkPushEventForSession(uint32_t sessionEpoch, const char* text) {
  if (sessionEpoch == 0 || text == nullptr || text[0] == '\0') return false;
  const size_t len = strlen(text);
  if (len > UARTLINK_FRAME_MAX_PAYLOAD) return false;
  return uartLinkWriteFrameForSession(
      sessionEpoch, UARTLINK_FRAME_EVT, uartLinkNextEventSeq(),
      (const uint8_t*)text, len);
}

bool uartLinkTryPushEvent(const char* text) {
  if (text == nullptr || text[0] == '\0') return false;
  const size_t len = strlen(text);
  if (len > UARTLINK_FRAME_MAX_PAYLOAD) return false;
  return uartLinkWriteFrameWithWait(UARTLINK_FRAME_EVT, uartLinkNextEventSeq(),
                                    (const uint8_t*)text, len, 0);
}

bool uartLinkTryPushEventForSession(uint32_t sessionEpoch, const char* text) {
  if (sessionEpoch == 0 || text == nullptr || text[0] == '\0') return false;
  const size_t len = strlen(text);
  if (len > UARTLINK_FRAME_MAX_PAYLOAD) return false;
  return uartLinkWriteFrameWithWait(
      UARTLINK_FRAME_EVT, uartLinkNextEventSeq(), (const uint8_t*)text, len,
      0, sessionEpoch);
}

// ---------------------------------------------------------------------------
// voicefetch — stream a recording over the link as binary frames.
//
// The P2 replacement for the chunked base64 fileread loop: no JSON, no
// base64, no per-chunk command round trips. Scoped by design (plan D2): it
// reads ONLY from the recordings directories, runs at user tier, refuses
// AuthBypass, and only serves the UART transport (frames to any other
// caller would spray binary at a channel that didn't ask).
//
// Flow: slurp the file into PSRAM (FS lock held only for the read), then
// META frame (total size), AUDIO frames, and finally the normal text reply
// — which doubles as the client's end-of-stream signal and carries the
// whole-file CRC for end-to-end verification.
const char* cmd_voicefetch(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char sReply[128];  // PSRAM: cmd_exec only; copied twice before the wire

  const AuthContext& ctx = currentAuthContext();
  if (ctx.transport != SOURCE_UART)
    return "Error: voicefetch only serves the UART link";
  if (uartLinkSessionEpoch() == 0)
    return "Error: voicefetch requires a logged-in UART session";
  if (!uartLinkIsRunning())
    return "Error: UART link not running";
  const uint32_t sessionEpoch = uartLinkSessionEpoch();
  if (sessionEpoch == 0)
    return "Error: UART login epoch unavailable";

  CommandArgs a(argsInput);
  String path;
  if (requireQuotedPath(a, 0, path) != nullptr)
    return "Error: usage: voicefetch \"<path>\"";
  // Scoped read: recordings only, no traversal. This is the deliberate
  // privilege boundary that lets voicefetch register non-admin.
  if (path.indexOf("..") >= 0 ||
      !(path.startsWith("/recordings/") || path.startsWith("/sd/recordings/")))
    return "Error: voicefetch reads /recordings or /sd/recordings only";
#if ENABLE_MICROPHONE
  if (micRecordingBusy())
    return "Error: recording is still active/finalizing";
#endif
  // Atomically reserve the high-volume framed lane. A separate
  // liveAudioStreamActive() observation is a TOCTOU: a recorder could BEGIN
  // after the check while voicefetch is reading or emitting frames. The claim
  // makes live BEGIN fail/skip until every return path below releases it.
  if (!liveAudioTryBeginBulkTransfer())
    return "Error: live audio stream is active";
  struct VoicefetchBulkGuard {
    ~VoicefetchBulkGuard() { liveAudioEndBulkTransfer(); }
  } bulkGuard;

  const size_t kMaxFile = 2 * 1024 * 1024;  // 60s max recording ≈ 1.9MB
  uint8_t* buf = nullptr;
  size_t total = 0;
  {
    FsLockGuard _g("voicefetch");
    File f = VFS::openGuarded(path, "r", ctx);
    if (!f || f.isDirectory()) {
      if (f) f.close();
      return "Error: not found or access denied";
    }
    total = f.size();
    if (total == 0 || total > kMaxFile) {
      f.close();
      return "Error: file empty or larger than the 2MB voicefetch cap";
    }
    // Refuse a transfer that would outrun the 60s executor-abandon window:
    // voicefetch streams synchronously on cmd_exec_task, and if it ran past
    // ~60s the loop task's submitAndExecuteSync would abandon and write its
    // own reply INTO the middle of the frame stream (desync). At the classic
    // 230400 ceiling a full 2MB file is ~87s, so this bites there. ~1% frame
    // overhead + 10 wire-bits/byte; 45s ceiling leaves margin.
    int effBaud = uartLinkEffectiveBaud();
    if ((uint64_t)total * 10 * 100 > (uint64_t)effBaud * 45 * 99) {
      f.close();
      return "Error: file too large to stream within the link's time budget "
             "at this baud — raise uartlinkbaud or split the recording";
    }
    buf = (uint8_t*)ps_alloc(total, AllocPref::PreferPSRAM, "voicefetch.buf");
    if (!buf) {
      f.close();
      return "Error: out of memory for file buffer";
    }
    size_t got = f.read(buf, total);
    f.close();
    if (got != total) {
      free(buf);
      return "Error: short read from filesystem";
    }
  }

  uint8_t meta[4] = { (uint8_t)(total & 0xFF), (uint8_t)((total >> 8) & 0xFF),
                      (uint8_t)((total >> 16) & 0xFF), (uint8_t)((total >> 24) & 0xFF) };
  uint16_t fileCrc = uartCrc16(buf, total);
  bool ok = uartLinkWriteFrameForSession(
      sessionEpoch, UARTLINK_FRAME_META, 0, meta, sizeof(meta));

  uint16_t seq = 1;
  size_t off = 0, frames = 0;
  while (ok && off < total) {
    size_t chunk = total - off;
    if (chunk > UARTLINK_FRAME_MAX_PAYLOAD) chunk = UARTLINK_FRAME_MAX_PAYLOAD;
    ok = uartLinkWriteFrameForSession(
        sessionEpoch, UARTLINK_FRAME_AUDIO, seq++, buf + off, chunk);
    off += chunk;
    frames++;
  }
  free(buf);

  if (!ok)
    return "Error: frame write failed (link down or session revoked mid-stream)";
  snprintf(sReply, sizeof(sReply), "OK: voicefetch %u bytes in %u frames crc16=%04X",
           (unsigned)total, (unsigned)frames + 1, fileCrc);
  return sReply;
}

#endif  // UART_LINK_PORT

#ifndef UART_LINK_PORT
const char* cmd_voicefetch(const String&) {
  return "Error: UART link not supported on this board";
}
#endif
