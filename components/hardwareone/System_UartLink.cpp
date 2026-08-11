// System_UartLink.cpp — UART host link drain. See System_UartLink.h for the
// design charter and docs/UART_HOST_LINK_PLAN.md for the verified plan.
#include "System_UartLink.h"

#include <atomic>

// Session gate lives here.
// Defined unconditionally so the auth plumbing (revocation sweeps, session
// list, tgRequireAuth) links on every board, including ones without link pins.
// The active epoch is the single atomic authority token: zero means logged
// out; every login publishes a new nonzero value. The username is fixed
// storage under sUartSessionMux and is never exposed as a mutable String.
static constexpr size_t kUartSessionUserMax = 64;
static char sUartSessionUser[kUartSessionUserMax + 1] = {};
static portMUX_TYPE sUartSessionMux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<uint32_t> sUartSessionGeneration{0};
static std::atomic<uint32_t> sUartActiveSessionEpoch{0};

uint32_t uartLinkSessionEpoch() {
  return sUartActiveSessionEpoch.load(std::memory_order_acquire);
}

void uartLinkSessionAuthenticated(const String& user) {
  // Publish the new identity and generation before opening the auth gate. A
  // producer from the previous login may have one physical frame already
  // admitted, but every later session-fenced admission observes the new epoch.
  char nextUser[kUartSessionUserMax + 1] = {};
  strlcpy(nextUser, user.length() ? user.c_str() : "uart",
          sizeof(nextUser));
  portENTER_CRITICAL(&sUartSessionMux);
  sUartActiveSessionEpoch.store(0, std::memory_order_release);
  memcpy(sUartSessionUser, nextUser, sizeof(sUartSessionUser));
  uint32_t current = sUartSessionGeneration.load(std::memory_order_relaxed);
  uint32_t next = 0;
  do {
    next = current + 1u;
    if (next == 0) next = 1;  // zero is permanently reserved for no session
  } while (!sUartSessionGeneration.compare_exchange_weak(
      current, next, std::memory_order_release, std::memory_order_relaxed));
  sUartActiveSessionEpoch.store(next, std::memory_order_release);
  portEXIT_CRITICAL(&sUartSessionMux);
}

void uartLinkSessionCleared() {
  // Close the fast auth gate first. The epoch advances on the next successful
  // login; keeping the last value while logged out makes status diagnostics
  // useful without granting any authority.
  portENTER_CRITICAL(&sUartSessionMux);
  sUartActiveSessionEpoch.store(0, std::memory_order_release);
  sUartSessionUser[0] = '\0';
  portEXIT_CRITICAL(&sUartSessionMux);
}

bool uartLinkSessionSnapshot(char* userOut, size_t userOutSize,
                             uint32_t* epochOut) {
  if (userOut && userOutSize) userOut[0] = '\0';
  portENTER_CRITICAL(&sUartSessionMux);
  const uint32_t activeEpoch =
      sUartActiveSessionEpoch.load(std::memory_order_relaxed);
  const bool authed = activeEpoch != 0;
  if (userOut && userOutSize)
    strlcpy(userOut, sUartSessionUser, userOutSize);
  if (epochOut)
    *epochOut = activeEpoch;
  portEXIT_CRITICAL(&sUartSessionMux);
  return authed;
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
  char target[kUartSessionUserMax + 1] = {};
  strlcpy(target, user.c_str(), sizeof(target));
  bool cleared = false;
  portENTER_CRITICAL(&sUartSessionMux);
  if (sUartActiveSessionEpoch.load(std::memory_order_relaxed) != 0 &&
      uartSessionUserEqualsAsciiNoCase(sUartSessionUser, target)) {
    sUartActiveSessionEpoch.store(0, std::memory_order_release);
    sUartSessionUser[0] = '\0';
    cleared = true;
  }
  portEXIT_CRITICAL(&sUartSessionMux);
  return cleared;
}

#ifdef UART_LINK_PORT

#include "System_BuildConfig.h"
#include "System_CommandTypes.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_Debug.h"
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
static unsigned long sLastInteractionMs = 0; // idle clock (0 = never stamped)
static unsigned long sLastNagMs = 0;         // rate limiter for unauth/garbage replies
static bool sDiscardingLine = false;         // true = swallow bytes until the next '\n'

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

// Inbound line cap, set to the executor's own capacity (ExecReq::line[2048]
// holds 2047 chars + NUL). At exactly this bound an over-long line is
// discarded whole rather than silently strncpy-truncated into a shorter,
// still-executable command. Also stops a newline-less byte stream from
// growing the accumulator without bound — a machine peer hits that far more
// easily than a human on the USB console.
static const size_t kUartLineCap = 2047;

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
  if (ok) UART_LINK_PORT.write(data, len);
  xSemaphoreGive(sTxMutex);
  return ok;
}

static void uartWriteLine(const char* s) {
  // Single-write text line (≤ small status/nag strings; replies go via the
  // String path in uartProcessLine).
  char buf[256];
  size_t n = strlcpy(buf, s, sizeof(buf) - 1);
  if (n > sizeof(buf) - 2) n = sizeof(buf) - 2;
  buf[n] = '\n';
  uartTxLocked((const uint8_t*)buf, n + 1);
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
    DEBUG_SYSTEMF("[uartlink] begin FAILED: uart%d tx=%d rx=%d baud=%d",
                  UART_LINK_UART_NUM, UART_LINK_TX_PIN, UART_LINK_RX_PIN, baud);
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
  DEBUG_SYSTEMF("[uartlink] started: uart%d tx=%d rx=%d baud=%d",
                UART_LINK_UART_NUM, UART_LINK_TX_PIN, UART_LINK_RX_PIN, baud);
  return true;
}

static bool uartLinkStopPhysical() {
  // Close authorization immediately, even if physical teardown must retry.
  // This makes every session-fenced producer fail at its next boundary.
  uartLinkSessionCleared();
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
    DEBUG_SYSTEMF("[uartlink] stop deferred — TX writer did not quiesce");
    return false;
  }
  sStarted.store(false, std::memory_order_release);
  UART_LINK_PORT.end();
  if (sTxMutex != nullptr) xSemaphoreGive(sTxMutex);
  sUartCLI = "";
  sDiscardingLine = false;
  sLastInteractionMs = 0;
  DEBUG_SYSTEMF("[uartlink] stopped");
  return true;
}

void uartLinkStop() {
  if (!uartLinkStopPhysical()) uartLinkRequeueIfNone(UL_PENDING_STOP);
}

void uartLinkInitFromSettings() {
  if (gSettings.uartLinkEnabled) (void)uartLinkStart();
}

const char* uartLinkStatusLine() {
  static char buf[192];
  char sessionUser[kUartSessionUserMax + 1];
  const bool sessionAuthed = uartLinkSessionSnapshot(
      sessionUser, sizeof(sessionUser));
  int baud = uartLinkEffectiveBaud();
  snprintf(buf, sizeof(buf),
           "UART link: %s (enabled=%d) uart%d tx=%d rx=%d baud=%d auth=%s user=%s idle=%lumin",
           sStarted.load(std::memory_order_acquire) ? "running" : "stopped",
           gSettings.uartLinkEnabled ? 1 : 0,
           UART_LINK_UART_NUM, UART_LINK_TX_PIN, UART_LINK_RX_PIN, baud,
           gSettings.uartRequireAuth ? "required" : "off",
           sessionAuthed ? sessionUser : "(none)",
           (unsigned long)gSettings.sessionIdleUart);
  return buf;
}

// Process one complete, trimmed line. Mirrors the serial drain
// (HardwareOne.cpp loop section 6) with two deliberate differences: every
// reply is written directly to the link port (this channel has no sink), and
// there is no "$ " prompt (machines parse the OK:/Error: contract, not
// prompts).
static void uartProcessLine(String& cmd) {
  // Idle logout — drop a stale session before processing this line; the line
  // then falls through to the login gate and is rejected, exactly like serial.
  if (uartLinkSessionEpoch() != 0 &&
      sessionIdleExpired(SOURCE_UART, sLastInteractionMs)) {
    uartLinkSessionCleared();
    uartWriteLine("[uart] Signed out due to inactivity. Please log in again.");
  }

  // In-band login is an intrinsic, NOT part of the auth gate: with
  // uartRequireAuth=0 the gate below is skipped entirely, and cmd_login
  // refuses SOURCE_UART callers (it would otherwise mint a session on the
  // physical console), so without this the host could never establish a named
  // session in auth-off mode — it would be pinned to non-admin AuthBypass
  // while being told to "use the in-band login" it had just used.
  const bool wantsLogin = cmd.startsWith("login ");
  if (wantsLogin ||
      (gSettings.uartRequireAuth && uartLinkSessionEpoch() == 0)) {
    if (wantsLogin) {
      String rest = cmd.substring(6);
      rest.trim();
      int sp = rest.indexOf(' ');
      if (sp <= 0) {
        uartWriteLine("Usage: login <username> <password>");
      } else {
        String u = rest.substring(0, sp);
        String p = rest.substring(sp + 1);
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
        } else if (isValidUser(u, p)) {
          AuthContext ctx;
          ctx.transport = SOURCE_UART;
          ctx.user = u;
          ctx.ip = kUartLockoutKey;
          ctx.path = "uart/login";
          ctx.sid = String();
          ctx.opaque = nullptr;
#if ENABLE_HTTP_SERVER
          // authSuccessUnified publishes the synchronized UART session.
          clearLoginAttempts(kUartLockoutKey);
          authSuccessUnified(ctx, nullptr);
#else
          uartLinkSessionAuthenticated(u);
#endif
          // Audit + event are OUTSIDE the guard: the security trail must not
          // depend on the web server being compiled in.
          recordLoginAttempt(SOURCE_UART, u, kUartLockoutKey, true, "Login successful");
          systemEventPost(SYSEVT_LOGIN_OK, u.c_str(), "uart");
          char msg[96];
          snprintf(msg, sizeof(msg), "OK: logged in as %s%s", u.c_str(),
                   isAdminUser(u) ? " (admin)" : "");
          uartWriteLine(msg);
        } else {
#if ENABLE_HTTP_SERVER
          recordFailedLogin(kUartLockoutKey);
#endif
          recordLoginAttempt(SOURCE_UART, u, kUartLockoutKey, false, "Invalid credentials");
          systemEventPost(SYSEVT_LOGIN_FAIL, u.c_str(), "uart");
          uartWriteLine("Error: authentication failed");
        }
      }
    } else if (cmd.length() > 0) {
      // Rate-limited: break/garbage from an unpowered or resetting host
      // arrives as a line flood; one nag per window is plenty for a machine.
      uartWriteLineNagLimited("Error: authentication required. Use: login <username> <password>");
    }
    return;  // login lines never fall through to the registry
  }

  // Authenticated (or auth disabled): in-band intrinsics, then the registry.
  if (cmd == "logout") {
    uartLinkSessionCleared();
    uartWriteLine("OK: logged out");
    return;
  }
  if (cmd == "whoami") {
    char msg[96];
    char sessionUser[kUartSessionUserMax + 1];
    const bool sessionAuthed = uartLinkSessionSnapshot(
        sessionUser, sizeof(sessionUser));
    const char* name = sessionAuthed && sessionUser[0]
                           ? sessionUser : "AuthBypass";
    const String userName(name);
    snprintf(msg, sizeof(msg), "OK: %s%s", name,
             (sessionAuthed && isAdminUser(userName)) ? " (admin)" : "");
    uartWriteLine(msg);
    return;
  }
  if (cmd.length() == 0) return;

  appendCommandToFeed("uart", cmd, String(), String());

  AuthContext actx;
  actx.transport = SOURCE_UART;
  // AuthBypass sentinel when auth is off and nobody logged in — same audit
  // convention as the serial/OLED drains (reserved username, non-admin).
  char sessionUser[kUartSessionUserMax + 1];
  const bool sessionAuthed = uartLinkSessionSnapshot(
      sessionUser, sizeof(sessionUser));
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
  bool ok = submitAndExecuteSync(uc, out);
  if (out.length()) {
    // ONE write call for blob + newline: with cross-task frame writers live
    // (voicefetch), a two-call reply could be split mid-message.
    out += '\n';
    uartTxLocked((const uint8_t*)out.c_str(), out.length(),
                 5000);  // replies are load-bearing; wait out a slow frame
  } else {
    // Never leave the host hanging: an empty result still gets a status line.
    uartWriteLine(ok ? "OK" : "Error: command failed");
  }
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
    if (c == '\n') {
      if (sDiscardingLine) {
        // Tail of an over-long line — drop it whole. Executing the residue
        // would run an arbitrary fragment as a command under the live session.
        sDiscardingLine = false;
        sUartCLI = "";
        continue;
      }
      String cmd = sUartCLI;
      sUartCLI = "";
      cmd.trim();
      uartProcessLine(cmd);
      // Refresh the idle clock on any completed line while authenticated —
      // one stamp covers both a fresh login and a subsequent command.
      if (uartLinkSessionEpoch() != 0)
        sLastInteractionMs = sessionStampNow();
      break;  // at most one command per loop() lap, same as the serial drain
    } else {
      if (sDiscardingLine) continue;
      if (sUartCLI.length() >= kUartLineCap) {
        sUartCLI = "";
        sDiscardingLine = true;   // swallow the rest, including the newline
        uartWriteLineNagLimited("Error: line too long — discarded");
        continue;
      }
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
static uint8_t sFrameBody[5 + UARTLINK_FRAME_MAX_PAYLOAD + 2];
static uint8_t sFrameWire[2 + sizeof(sFrameBody) + sizeof(sFrameBody) / 254 + 2];

static bool uartLinkWriteFrameWithWait(uint8_t type, uint16_t seq,
                                       const uint8_t* payload, size_t len,
                                       TickType_t mutexWaitTicks,
                                       uint32_t expectedSessionEpoch = 0) {
  if (len > UARTLINK_FRAME_MAX_PAYLOAD) return false;
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
  static char sReply[128];

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
