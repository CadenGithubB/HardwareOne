#include "System_Cm5Presence.h"

#include <atomic>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "G2_Glasses.h"
#include "System_Clock.h"          // SYNC_CM5 step chokepoint + epoch validity
#include "System_Cm5HostControl.h"  // cmdCm5Power*/cmdCm5Fan* — registered below
#include "System_Debug.h"          // BROADCAST_PRINTF
#include "System_TaskUtils.h"
#include "System_UartLink.h"
#include "System_Utils.h"

// Keep the root usage line honest about what this build actually compiled in,
// rather than advertising a verb that would answer "Unknown command".
#if ENABLE_RASPBERRY_PI_HOST_POWER && ENABLE_RASPBERRY_PI_HOST_FAN
  #define CM5_ROOT_VERB_HINT "|power|fan"
#elif ENABLE_RASPBERRY_PI_HOST_POWER
  #define CM5_ROOT_VERB_HINT "|power"
#elif ENABLE_RASPBERRY_PI_HOST_FAN
  #define CM5_ROOT_VERB_HINT "|fan"
#else
  #define CM5_ROOT_VERB_HINT ""
#endif

namespace {

struct PresenceRecord {
  uint32_t sessionEpoch = 0;
  uint32_t sequence = 0;
  uint32_t lastSeenMs = 0;
  uint32_t generation = 0;
  uint32_t commandStartedMs = 0;
  uint32_t commandFinishedMs = 0;
  uint32_t staleTransitions = 0;
  uint32_t lastTransitionMs = 0;
  uint32_t stackFreeMinBytes = 0;
  Cm5PresenceMode mode = Cm5PresenceMode::Unknown;
  bool seen = false;
  bool monitorFresh = false;
  bool commandInFlight = false;
  bool commandGrace = false;
  bool commandWasFresh = false;
};

static PresenceRecord sPresence;

// Display-only work reason, deliberately NOT a member of PresenceRecord.
// Every snapshot copies that record wholesale under the spinlock, and the
// presence monitor snapshots on a 2 KB stack — putting ~97 bytes of string in
// there would tax every reader for a field almost none of them want. Guarded by
// sPresenceMux exactly like sPresence, so the two stay coherent. Empty means
// "no reason"; only a Busy or Degraded heartbeat carrying a token ever makes it
// non-empty (see recordHeartbeat for why those two and not the others).
static char sPresenceReason[CM5_PRESENCE_REASON_MAX + 1] = {0};

static portMUX_TYPE sPresenceMux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t sPresenceTask = nullptr;
static std::atomic<bool> sTaskCreateClaim{false};

struct Token {
  const char* ptr = nullptr;
  size_t len = 0;
};

static bool nextToken(const char*& cursor, Token& token) {
  while (*cursor && isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  if (!*cursor) {
    token = {};
    return false;
  }
  token.ptr = cursor;
  while (*cursor && !isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  token.len = static_cast<size_t>(cursor - token.ptr);
  return true;
}

static bool tokenEquals(const Token& token, const char* literal) {
  const size_t n = strlen(literal);
  return token.len == n && memcmp(token.ptr, literal, n) == 0;
}

static bool tokenEqualsNoCase(const Token& token, const char* literal) {
  const size_t n = strlen(literal);
  if (token.len != n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (tolower(static_cast<unsigned char>(token.ptr[i])) !=
        tolower(static_cast<unsigned char>(literal[i]))) {
      return false;
    }
  }
  return true;
}

// Copy an opaque reason token into `out`, bounded and printable.
//
// OPAQUE means opaque: no vocabulary, no validation, no rejection. The token is
// whatever the host wants to say and firmware attaches no meaning to it, which
// is what lets the host add or rename busy holders without a firmware flash.
//
// It is still sanitized, because it lands in `cm5 status` and in logs. Anything
// outside printable ASCII becomes '?', so a stray control byte cannot smuggle an
// escape sequence into somebody's terminal or forge a field separator in a line
// another tool parses. Over-long input is TRUNCATED, never refused: refusing
// would fail the heartbeat, and a failed heartbeat lets the lease go stale and
// abandons an in-flight generation. No content of this token may ever do that.
static void copyReason(const Token& token, char* out, size_t outCap) {
  if (!out || outCap == 0) return;
  size_t n = 0;
  const size_t limit = outCap - 1;
  for (size_t i = 0; token.ptr && i < token.len && n < limit; ++i) {
    const unsigned char c = static_cast<unsigned char>(token.ptr[i]);
    out[n++] = (c >= 0x21 && c <= 0x7e) ? static_cast<char>(c) : '?';
  }
  out[n] = '\0';
}

static bool parseU32(const Token& token, uint32_t& out) {
  if (!token.ptr || token.len == 0) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < token.len; ++i) {
    const unsigned char c = static_cast<unsigned char>(token.ptr[i]);
    if (c < '0' || c > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(c - '0');
    if (value > (UINT32_MAX - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  out = value;
  return true;
}

static bool parseMode(const Token& token, Cm5PresenceMode& mode) {
  if (tokenEqualsNoCase(token, "starting")) mode = Cm5PresenceMode::Starting;
  else if (tokenEqualsNoCase(token, "ready")) mode = Cm5PresenceMode::Ready;
  else if (tokenEqualsNoCase(token, "busy")) mode = Cm5PresenceMode::Busy;
  else if (tokenEqualsNoCase(token, "degraded")) mode = Cm5PresenceMode::Degraded;
  else return false;
  return true;
}

static void notifyMonitor() {
  TaskHandle_t task = nullptr;
  portENTER_CRITICAL(&sPresenceMux);
  task = sPresenceTask;
  portEXIT_CRITICAL(&sPresenceMux);
  if (task) xTaskNotifyGive(task);
}

static bool observeMonitorFreshness(uint32_t generation,
                                    uint32_t sessionEpoch,
                                    bool fresh, uint32_t nowMs) {
  bool changed = false;
  portENTER_CRITICAL(&sPresenceMux);
  if (sPresence.generation == generation &&
      sPresence.sessionEpoch == sessionEpoch &&
      sPresence.monitorFresh != fresh) {
    sPresence.monitorFresh = fresh;
    sPresence.lastTransitionMs = nowMs;
    if (!fresh) ++sPresence.staleTransitions;
    changed = true;
  }
  portEXIT_CRITICAL(&sPresenceMux);
  return changed;
}

static void observeTaskStackHighWater() {
  const uint32_t freeBytes =
      static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  portENTER_CRITICAL(&sPresenceMux);
  if (sPresence.stackFreeMinBytes == 0 ||
      freeBytes < sPresence.stackFreeMinBytes) {
    sPresence.stackFreeMinBytes = freeBytes;
  }
  portEXIT_CRITICAL(&sPresenceMux);
}

static void cm5PresenceTaskBody(void*) {
  for (;;) {
    PresenceRecord record{};
    portENTER_CRITICAL(&sPresenceMux);
    record = sPresence;
    portEXIT_CRITICAL(&sPresenceMux);

    const uint32_t activeEpoch = uartLinkSessionEpoch();
    const uint32_t nowMs = millis();
    const Cm5PresenceSnapshot snapshot =
        cm5PresenceSnapshotForSession(activeEpoch, nowMs);
    observeTaskStackHighWater();

    // The task owns observable fresh/stale transitions and promptly wakes the
    // G2 control owner on either edge. Security consumers still recompute the
    // lease from timestamps, so a starved task can never preserve authority.
    if (record.seen && uartLinkSessionEpoch() == activeEpoch &&
        observeMonitorFreshness(record.generation, record.sessionEpoch,
                                snapshot.fresh, nowMs)) {
      g2EvenAiHostStateChanged();
    }
    if (!snapshot.fresh) {
      // No polling while absent/stale.  A heartbeat, command edge, or session
      // transition wakes us to recompute the exact deadline.
      (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    uint32_t waitMs = snapshot.freshRemainingMs;
    if (waitMs == 0) waitMs = 1;
    TickType_t waitTicks = pdMS_TO_TICKS(waitMs);
    if (waitTicks == 0) waitTicks = 1;
    (void)ulTaskNotifyTake(pdTRUE, waitTicks);
  }
}

static bool ensurePresenceTask() {
  portENTER_CRITICAL(&sPresenceMux);
  const bool alreadyRunning = sPresenceTask != nullptr;
  portEXIT_CRITICAL(&sPresenceMux);
  if (alreadyRunning) return true;

  bool expected = false;
  if (!sTaskCreateClaim.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) return false;

  TaskHandle_t created = nullptr;
  const BaseType_t rc = xTaskCreateLogged(
      cm5PresenceTaskBody, "cm5_presence", CM5_PRESENCE_STACK_BYTES,
      nullptr, TASK_PRIORITY_LOW, &created, "cm5.presence", PRO_CORE);
  if (rc == pdPASS && created != nullptr) {
    portENTER_CRITICAL(&sPresenceMux);
    sPresenceTask = created;
    portEXIT_CRITICAL(&sPresenceMux);
  }
  sTaskCreateClaim.store(false, std::memory_order_release);
  return rc == pdPASS && created != nullptr;
}

static bool recordHeartbeat(uint32_t sessionEpoch, uint32_t sequence,
                            Cm5PresenceMode mode, const char* reason) {
  if (sessionEpoch == 0 || sequence == 0 ||
      mode == Cm5PresenceMode::Unknown) return false;
  if (!ensurePresenceTask()) return false;

  const uint32_t nowMs = millis();
  portENTER_CRITICAL(&sPresenceMux);
  sPresence.sessionEpoch = sessionEpoch;
  sPresence.sequence = sequence;
  sPresence.lastSeenMs = nowMs;
  sPresence.mode = mode;
  sPresence.seen = true;
  // The label describes a condition that holds NOW, so it is rewritten from
  // scratch on every heartbeat — there is no decay path to get wrong, and a
  // label can never be more than one 5 s interval old.
  //
  // Retained for BUSY and DEGRADED only. Busy is the obvious case ("what is it
  // doing?"). Degraded is the one worth having deliberately: "degraded" alone
  // says the host is unhappy without saying why, and "degraded + thermal" is the
  // difference between a diagnosis and a shrug. Starting and Ready describe a
  // host that is not doing anything worth labelling, so a token on those is
  // dropped rather than shown — as is a heartbeat that carries no token at all,
  // in any state.
  if ((mode == Cm5PresenceMode::Busy || mode == Cm5PresenceMode::Degraded) &&
      reason && reason[0] != '\0') {
    // Already sanitized and bounded by copyReason before the lock, so this is a
    // short bounded copy rather than a scan and the spinlock stays cheap.
    strncpy(sPresenceReason, reason, CM5_PRESENCE_REASON_MAX);
    sPresenceReason[CM5_PRESENCE_REASON_MAX] = '\0';
  } else {
    sPresenceReason[0] = '\0';
  }
  ++sPresence.generation;
  portEXIT_CRITICAL(&sPresenceMux);
  notifyMonitor();
  return true;
}

static void writeUsage(char* reply, size_t replySize) {
  snprintf(reply, replySize,
           "Error: Usage: cm5 heartbeat 1 <sequence> "
           "<starting|ready|busy|degraded> [reason]");
}

static void writeCapabilities(char* reply, size_t replySize) {
  snprintf(reply, replySize,
           "OK: cm5-presence-v1 heartbeat_modes="
           "starting,ready,busy,degraded interval_ms=5000 "
           "lease_ms=%lu busy_lease_ms=%lu cmd_grace_ms=%lu "
           "heartbeat_reason=1 reason_max=%u reason_states=busy,degraded",
           (unsigned long)CM5_PRESENCE_NORMAL_LEASE_MS,
           (unsigned long)CM5_PRESENCE_BUSY_LEASE_MS,
           (unsigned long)CM5_PRESENCE_COMMAND_GRACE_MS,
           (unsigned)CM5_PRESENCE_REASON_MAX);
}

static void writeStatus(char* reply, size_t replySize) {
  char reason[CM5_PRESENCE_REASON_MAX + 1] = {0};
  const Cm5PresenceSnapshot s = cm5PresenceSnapshotWithReason(reason,
                                                              sizeof(reason));
  snprintf(reply, replySize,
           "OK: CM5 presence task=%s state=%s fresh=%d seen=%d "
           "epoch=%lu seq=%lu age_ms=%lu lease_ms=%lu cmd_busy=%d "
           "cmd_grace=%d monitor=%d stale_n=%lu stack_free_min=%lu "
           "reason=%s",
           s.taskRunning ? "running" : "dormant",
           cm5PresenceModeName(s.mode), s.fresh ? 1 : 0,
           s.seenForSession ? 1 : 0,
           (unsigned long)s.sessionEpoch, (unsigned long)s.sequence,
           (unsigned long)s.ageMs, (unsigned long)s.leaseMs,
           s.commandInFlight ? 1 : 0, s.commandGrace ? 1 : 0,
           s.monitorFresh ? 1 : 0,
           (unsigned long)s.staleTransitions,
           (unsigned long)s.stackFreeMinBytes,
           reason[0] ? reason : "-");
}

}  // namespace

static_assert(cm5PresenceLeaseMs(Cm5PresenceMode::Ready) == 15000);
static_assert(cm5PresenceLeaseMs(Cm5PresenceMode::Busy) == 75000);
static_assert(CM5_PRESENCE_COMMAND_GRACE_MS <
              CM5_PRESENCE_BUSY_LEASE_MS);
static_assert(cm5PresenceFreshAt(true, 14999, 0, 15000));
static_assert(!cm5PresenceFreshAt(true, 15000, 0, 15000));
static_assert(cm5PresenceFreshAt(true, 4u, UINT32_MAX - 4u, 10u));
static_assert(!cm5PresenceFreshAt(true, 5u, UINT32_MAX - 4u, 10u));

const char* cm5PresenceModeName(Cm5PresenceMode mode) {
  switch (mode) {
    case Cm5PresenceMode::Starting: return "starting";
    case Cm5PresenceMode::Ready: return "ready";
    case Cm5PresenceMode::Busy: return "busy";
    case Cm5PresenceMode::Degraded: return "degraded";
    case Cm5PresenceMode::Unknown: break;
  }
  return "unknown";
}

// Shared body. `reasonOut` may be null when the caller does not want the
// reason, which is the common case; when it is not null it is filled from the
// SAME critical section that samples the record, so the label can never be torn
// away from the state it describes.
static Cm5PresenceSnapshot snapshotForSessionImpl(uint32_t activeSessionEpoch,
                                                  uint32_t nowMs,
                                                  char* reasonOut,
                                                  size_t reasonCap) {
  PresenceRecord record{};
  TaskHandle_t task = nullptr;
  portENTER_CRITICAL(&sPresenceMux);
  record = sPresence;
  task = sPresenceTask;
  if (reasonOut && reasonCap > 0) {
    strncpy(reasonOut, sPresenceReason, reasonCap - 1);
    reasonOut[reasonCap - 1] = '\0';
  }
  portEXIT_CRITICAL(&sPresenceMux);

  Cm5PresenceSnapshot out{};
  out.sessionEpoch = record.sessionEpoch;
  out.sequence = record.sequence;
  out.lastSeenMs = record.lastSeenMs;
  out.ageMs = record.seen ? static_cast<uint32_t>(nowMs - record.lastSeenMs) : 0;
  out.leaseMs = cm5PresenceLeaseMs(record.mode);
  out.commandAgeMs = record.commandInFlight
                         || record.commandGrace
                     ? static_cast<uint32_t>(nowMs - record.commandStartedMs)
                     : 0;
  out.staleTransitions = record.staleTransitions;
  out.lastTransitionMs = record.lastTransitionMs;
  out.stackFreeMinBytes = record.stackFreeMinBytes;
  out.mode = record.mode;
  out.taskRunning = task != nullptr;
  out.monitorFresh = record.monitorFresh;
  out.seenForSession = record.seen && activeSessionEpoch != 0 &&
                       record.sessionEpoch == activeSessionEpoch;
  const bool heartbeatFresh = out.seenForSession &&
      cm5PresenceFreshAt(true, nowMs, record.lastSeenMs, out.leaseMs);
  const bool commandWithinCap = out.seenForSession && record.commandWasFresh &&
      cm5PresenceFreshAt(true, nowMs, record.commandStartedMs,
                         CM5_PRESENCE_BUSY_LEASE_MS);
  const bool commandGraceFresh = commandWithinCap && record.commandGrace &&
      cm5PresenceFreshAt(true, nowMs, record.commandFinishedMs,
                         CM5_PRESENCE_COMMAND_GRACE_MS);
  const bool commandFresh = commandWithinCap &&
      (record.commandInFlight || commandGraceFresh);
  out.fresh = heartbeatFresh || commandFresh;
  out.commandInFlight = out.seenForSession && record.commandInFlight;
  out.commandGrace = commandGraceFresh;

  uint32_t heartbeatRemaining = 0;
  if (heartbeatFresh) heartbeatRemaining = out.leaseMs - out.ageMs;
  uint32_t commandRemaining = 0;
  if (commandFresh) {
    const uint32_t capRemaining =
        CM5_PRESENCE_BUSY_LEASE_MS - out.commandAgeMs;
    commandRemaining = capRemaining;
    if (commandGraceFresh) {
      const uint32_t graceAge =
          static_cast<uint32_t>(nowMs - record.commandFinishedMs);
      const uint32_t graceRemaining =
          CM5_PRESENCE_COMMAND_GRACE_MS - graceAge;
      if (graceRemaining < commandRemaining)
        commandRemaining = graceRemaining;
    }
  }
  out.freshRemainingMs = heartbeatRemaining > commandRemaining
                             ? heartbeatRemaining : commandRemaining;
  return out;
}

Cm5PresenceSnapshot cm5PresenceSnapshotForSession(uint32_t activeSessionEpoch,
                                                  uint32_t nowMs) {
  return snapshotForSessionImpl(activeSessionEpoch, nowMs, nullptr, 0);
}

Cm5PresenceSnapshot cm5PresenceSnapshot() {
  return cm5PresenceSnapshotForSession(uartLinkSessionEpoch(), millis());
}

Cm5PresenceSnapshot cm5PresenceSnapshotWithReason(char* reasonOut,
                                                  size_t reasonCap) {
  return snapshotForSessionImpl(uartLinkSessionEpoch(), millis(),
                                reasonOut, reasonCap);
}

void cm5PresenceNotifySessionChanged() { notifyMonitor(); }

void cm5PresenceCommandStarted(uint32_t sessionEpoch) {
  if (sessionEpoch == 0) return;
  const uint32_t nowMs = millis();
  bool changed = false;
  portENTER_CRITICAL(&sPresenceMux);
  const uint32_t leaseMs = cm5PresenceLeaseMs(sPresence.mode);
  const bool freshNow = sPresence.seen &&
      sPresence.sessionEpoch == sessionEpoch &&
      cm5PresenceFreshAt(true, nowMs, sPresence.lastSeenMs, leaseMs);
  if (sPresence.sessionEpoch == sessionEpoch && sPresence.commandGrace) {
    sPresence.commandGrace = false;
    sPresence.commandWasFresh = false;
    changed = true;
  }
  if (freshNow) {
    sPresence.commandInFlight = true;
    sPresence.commandWasFresh = true;
    sPresence.commandStartedMs = nowMs;
    changed = true;
  }
  if (changed) ++sPresence.generation;
  portEXIT_CRITICAL(&sPresenceMux);
  if (changed) notifyMonitor();
}

void cm5PresenceCommandFinished(uint32_t sessionEpoch, bool replyAdmitted) {
  const uint32_t nowMs = millis();
  bool changed = false;
  portENTER_CRITICAL(&sPresenceMux);
  if (sPresence.sessionEpoch == sessionEpoch && sPresence.commandInFlight) {
    sPresence.commandInFlight = false;
    sPresence.commandFinishedMs = nowMs;
    const uint32_t commandAge =
        static_cast<uint32_t>(nowMs - sPresence.commandStartedMs);
    sPresence.commandGrace = replyAdmitted && sPresence.commandWasFresh &&
                             commandAge < CM5_PRESENCE_BUSY_LEASE_MS;
    if (!sPresence.commandGrace) sPresence.commandWasFresh = false;
    ++sPresence.generation;
    changed = true;
  }
  portEXIT_CRITICAL(&sPresenceMux);
  if (changed) notifyMonitor();
}

bool cm5PresenceIsProtocolCommand(const char* line) {
  if (!line) return false;

  const char* cursor = line;
  Token root{};
  if (!nextToken(cursor, root) || !tokenEqualsNoCase(root, "cm5")) return false;

  // Presence housekeeping ONLY. This deliberately does not cover the whole
  // `cm5` namespace: human-issued power/fan operations stay in the command
  // feed and audit (a superadmin host reboot must leave a trail). The four
  // host-generated ACK/report callbacks never reach this classifier because
  // System_UartLink consumes them through the direct control-plane intrinsic.
  Token verb{};
  if (!nextToken(cursor, verb)) return true;  // bare `cm5` — usage/help
  return tokenEqualsNoCase(verb, "status") ||
         tokenEqualsNoCase(verb, "capabilities") ||
         tokenEqualsNoCase(verb, "heartbeat") ||
         tokenEqualsNoCase(verb, "linkhealth") ||
         tokenEqualsNoCase(verb, "time");
}

Cm5HeartbeatIntrinsicResult cm5PresenceHandleHeartbeatIntrinsic(
    const char* line, uint32_t activeSessionEpoch,
    bool sessionMayPublishPresence,
    char* reply, size_t replySize) {
  if (!line || !reply || replySize == 0)
    return Cm5HeartbeatIntrinsicResult::NotHeartbeat;

  const char* cursor = line;
  Token root{};
  if (!nextToken(cursor, root) || !tokenEqualsNoCase(root, "cm5"))
    return Cm5HeartbeatIntrinsicResult::NotHeartbeat;

  Token verb{};
  if (!nextToken(cursor, verb) || !tokenEqualsNoCase(verb, "heartbeat"))
    return Cm5HeartbeatIntrinsicResult::NotHeartbeat;

  Token version{}, sequenceToken{}, modeToken{}, reasonToken{}, extra{};
  uint32_t sequence = 0;
  Cm5PresenceMode mode = Cm5PresenceMode::Unknown;
  if (!nextToken(cursor, version) || !tokenEquals(version, "1") ||
      !nextToken(cursor, sequenceToken) ||
      !parseU32(sequenceToken, sequence) || sequence == 0 ||
      !nextToken(cursor, modeToken) || !parseMode(modeToken, mode)) {
    writeUsage(reply, replySize);
    return Cm5HeartbeatIntrinsicResult::Handled;
  }

  // Optional 5th token: an opaque, display-only reason. See copyReason.
  char reason[CM5_PRESENCE_REASON_MAX + 1] = {0};
  if (nextToken(cursor, reasonToken)) copyReason(reasonToken, reason, sizeof(reason));

  // DELIBERATE ASYMMETRY WITH THE SPEC, which said "one optional trailing
  // token" and would have this reject a 6th. Anything past the reason is
  // ignored instead, because of what a rejection COSTS here: a refused
  // heartbeat is a heartbeat that did not renew, and the bench failure that
  // prompted this whole feature was a lease going stale and abandoning a
  // generation mid-answer. Trading that risk for strictness about a field
  // nobody parses is a bad trade.
  //
  // It also mirrors what the daemon is doing in the other direction — relaxing
  // its reply regex to tolerate unknown trailing fields — so a future protocol
  // addition can land on either side first without wedging the other. That
  // symmetry is the point: the tolerant side is never the one that breaks.
  //
  // Ignored, not invisible: the remainder is reported once per occurrence so a
  // daemon that is accidentally sending two tokens is diagnosable rather than
  // silently half-read.
  // Rate-limited: a daemon that gets this wrong gets it wrong on EVERY
  // heartbeat, and 5-second log spam would bury the thing being diagnosed.
  if (nextToken(cursor, extra)) {
    static uint32_t sTrailingLogMs = 0;
    if (everyMs(&sTrailingLogMs, 60000)) {
      BROADCAST_PRINTF("[CM5] heartbeat seq=%lu carried unparsed trailing text "
                       "after the reason; ignored (logged at most once a minute)\n",
                       (unsigned long)sequence);
    }
  }
  if (activeSessionEpoch == 0) {
    snprintf(reply, replySize,
             "Error: cm5 heartbeat requires a named authenticated UART session");
    return Cm5HeartbeatIntrinsicResult::Handled;
  }
  // The normal registry enforces the same guest restriction before invoking
  // a handler. Heartbeat intentionally bypasses that queue, so carry the
  // role decision in the coherent UART-login snapshot and enforce it here.
  if (!sessionMayPublishPresence) {
    snprintf(reply, replySize,
             "Error: Guest accounts are view-only. Only login/logout are allowed.");
    return Cm5HeartbeatIntrinsicResult::Handled;
  }
  if (!recordHeartbeat(activeSessionEpoch, sequence, mode, reason)) {
    snprintf(reply, replySize,
             "Error: CM5 presence task unavailable");
    return Cm5HeartbeatIntrinsicResult::Handled;
  }

  // DO NOT ADD FIELDS HERE. Deployed daemons parse this reply with a regex
  // anchored on '$' (cm5_presence.py:_REPLY_RE), so one extra trailing field
  // breaks every one of them the moment this firmware is flashed. The reason is
  // echoed in `cm5 status`, which nothing parses, precisely so this line can
  // stay frozen.
  snprintf(reply, replySize,
           "OK: cm5 heartbeat version=1 seq=%lu state=%s "
           "session_epoch=%lu lease_ms=%lu",
           (unsigned long)sequence, cm5PresenceModeName(mode),
           (unsigned long)activeSessionEpoch,
           (unsigned long)cm5PresenceLeaseMs(mode));
  return Cm5HeartbeatIntrinsicResult::Handled;
}

// ===========================================================================
// CM5 time anchor (Phase 1) — stash-and-step.
//
// The intrinsic (UART RX task) only validates + stashes the freshest CM5 stamp
// and ACKs "stashed". cm5TimeSyncTick() (main loop) owns ALL policy — adopt vs
// correct, confidence gating, precedence, drift-quench — and is the only place
// that touches settimeofday()/Clock::clockStepped(). This mirrors the SNTP
// store-on-callback / step-on-main-loop discipline and the ring's project-
// forward corrective tick; clock mutation never happens on the small RX stack.
// ===========================================================================

namespace {

struct Cm5TimeStash {
  uint32_t epochSec = 0;   // UTC unix seconds as reported by the CM5
  uint32_t rxMs = 0;       // millis() when the push was admitted
  uint8_t flags = 0;       // CM5_TIME_FLAG_* confidence bits
  bool pending = false;    // a fresh, not-yet-evaluated stamp is present
};

static Cm5TimeStash sCm5Time;
static portMUX_TYPE sCm5TimeMux = portMUX_INITIALIZER_UNLOCKED;

// Reject a stash the main loop failed to consume for this long: its projection
// error has grown and a fresher push almost certainly superseded it. Generous
// because each push overwrites the last, so a healthy link keeps rxMs recent.
constexpr uint32_t kCm5TimeMaxStashAgeMs = 600000;  // 10 min

// Same 120 s drift threshold the ring/glasses corrective pushes use, so a
// slowly-advancing real clock never trips a correction on every push.
constexpr int64_t kCm5TimeDriftThresholdSec = 120;

// Clear the pending flag iff no newer push landed since the caller's snapshot
// (matched on rxMs), so a concurrent stash is never silently dropped.
static void cm5TimeConsumeStash(uint32_t rxMs) {
  portENTER_CRITICAL(&sCm5TimeMux);
  if (sCm5Time.rxMs == rxMs) sCm5Time.pending = false;
  portEXIT_CRITICAL(&sCm5TimeMux);
}

}  // namespace

Cm5TimeIntrinsicResult cm5TimeHandleTimeIntrinsic(
    const char* line, uint32_t activeSessionEpoch,
    bool sessionMayPublishPresence,
    char* reply, size_t replySize) {
  if (!line || !reply || replySize == 0)
    return Cm5TimeIntrinsicResult::NotTime;

  const char* cursor = line;
  Token root{};
  if (!nextToken(cursor, root) || !tokenEqualsNoCase(root, "cm5"))
    return Cm5TimeIntrinsicResult::NotTime;

  Token verb{};
  if (!nextToken(cursor, verb) || !tokenEqualsNoCase(verb, "time"))
    return Cm5TimeIntrinsicResult::NotTime;

  // From here the line is ours: every malformed/denied form is Handled with an
  // error reply, never NotTime (which would fall through to the registry).
  Token sub{}, version{}, epochToken{}, flagsToken{}, extra{};
  uint32_t epochSec = 0, flags = 0;
  if (!nextToken(cursor, sub) || !tokenEqualsNoCase(sub, "set") ||
      !nextToken(cursor, version) || !tokenEquals(version, "1") ||
      !nextToken(cursor, epochToken) || !parseU32(epochToken, epochSec) ||
      !nextToken(cursor, flagsToken) || !parseU32(flagsToken, flags) ||
      flags > 0xFFu || nextToken(cursor, extra)) {
    snprintf(reply, replySize,
             "Error: Usage: cm5 time set 1 <epoch_sec_utc> <flags_u8>");
    return Cm5TimeIntrinsicResult::Handled;
  }
  if (activeSessionEpoch == 0) {
    snprintf(reply, replySize,
             "Error: cm5 time set requires a named authenticated UART session");
    return Cm5TimeIntrinsicResult::Handled;
  }
  // Same guest restriction the registry enforces; carried in the coherent UART
  // login snapshot because this intrinsic bypasses the command queue.
  if (!sessionMayPublishPresence) {
    snprintf(reply, replySize,
             "Error: Guest accounts are view-only. Only login/logout are allowed.");
    return Cm5TimeIntrinsicResult::Handled;
  }
  if (!Clock::isPlausibleEpoch(static_cast<time_t>(epochSec))) {
    snprintf(reply, replySize,
             "Error: cm5 time timestamp must be within 2020-2099");
    return Cm5TimeIntrinsicResult::Handled;
  }

  portENTER_CRITICAL(&sCm5TimeMux);
  sCm5Time.epochSec = epochSec;
  sCm5Time.rxMs = millis();
  sCm5Time.flags = static_cast<uint8_t>(flags);
  sCm5Time.pending = true;
  portEXIT_CRITICAL(&sCm5TimeMux);

  // "stashed", not "accepted": the step happens a lap later in the tick and may
  // still yield to a higher-precedence source or age out. Provenance of an
  // actual step is the Clock ledger (cmd_time source=cm5) + SYSEVT_TIME_SYNCED.
  snprintf(reply, replySize,
           "OK: cm5 time set epoch=%lu flags=%lu action=stashed session_epoch=%lu",
           (unsigned long)epochSec, (unsigned long)flags,
           (unsigned long)activeSessionEpoch);
  return Cm5TimeIntrinsicResult::Handled;
}

void cm5TimeSyncTick() {
  // Snapshot the latest stash under the mux; near-zero cost when idle.
  Cm5TimeStash stash;
  bool have;
  portENTER_CRITICAL(&sCm5TimeMux);
  stash = sCm5Time;
  have = sCm5Time.pending;
  portEXIT_CRITICAL(&sCm5TimeMux);
  if (!have) return;

  const uint32_t ageMs = Clock::ageMs(stash.rxMs);
  if (ageMs > kCm5TimeMaxStashAgeMs) { cm5TimeConsumeStash(stash.rxMs); return; }

  // Project the stamp forward by however long it sat, mirroring the ring.
  const time_t projected =
      static_cast<time_t>(stash.epochSec) + static_cast<time_t>(ageMs / 1000u);
  if (!Clock::isPlausibleEpoch(projected)) {
    cm5TimeConsumeStash(stash.rxMs);
    return;
  }

  const time_t before = time(nullptr);
  bool step = false;
  bool adopt = false;
  if (!Clock::isSynced()) {
    // Dark boot: adopt only if the Pi has SOME real basis for its clock (live
    // NTP or a readable battery RTC). A Pi with neither cannot seed a
    // plausible-but-wrong epoch into dated files.
    if (stash.flags & (CM5_TIME_FLAG_PI_SYNCED | CM5_TIME_FLAG_PI_RTC_VALID)) {
      step = true;
      adopt = true;
    }
  } else {
    // Already valid: CM5 is authoritative over the fallback sources actually
    // present on a carrier (ring / RTC-carryover / a prior CM5 step), but in
    // Phase 1 it YIELDS to a live NTP fix and to a manual operator set. To make
    // CM5 the full grandmaster (override NTP too), drop the SYNC_NTP term here
    // — that ordering decision belongs to the Phase 2 central arbiter. A
    // battery-RTC-only Pi (bit0 clear) may seed a dark clock but not override a
    // running one. Drift gate self-quenches steady state.
    const Clock::SyncSource src = Clock::syncSource();
    const bool correctable =
        src != Clock::SYNC_MANUAL && src != Clock::SYNC_NTP;
    if (correctable && (stash.flags & CM5_TIME_FLAG_PI_SYNCED)) {
      int64_t drift = static_cast<int64_t>(projected) -
                      static_cast<int64_t>(before);
      if (drift < 0) drift = -drift;
      if (drift > kCm5TimeDriftThresholdSec) step = true;
    }
  }

  if (step) {
    struct timeval tv = { .tv_sec = projected, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    Clock::clockStepped(Clock::SYNC_CM5, before);
    if (adopt) {
      char tsStr[24] = "";
      struct tm tmNow;
      if (localtime_r(&projected, &tmNow))
        strftime(tsStr, sizeof(tsStr), "%Y-%m-%d %H:%M", &tmNow);
      BROADCAST_PRINTF("[CM5] Adopted CM5 clock: %s (no local time source)",
                       tsStr);
    }
  }
  cm5TimeConsumeStash(stash.rxMs);
}

// ===========================================================================
// CM5 LINK HEALTH — the host's fault tally, mirrored here for the CLI.
// See System_Cm5Presence.h for the contract and for why this is inert.
// ===========================================================================

namespace {

struct Cm5LinkHealthRecord {
  bool     seen = false;
  uint32_t rxMs = 0;
  uint32_t sessionEpoch = 0;
  uint32_t reports = 0;
  uint32_t garbage = 0;
  uint32_t corrupt = 0;
  uint32_t timeouts = 0;
  uint32_t strays = 0;
  uint32_t logins = 0;
  uint32_t resets = 0;
  uint32_t tx = 0;
  uint32_t rx = 0;
  uint32_t uptimeS = 0;
  uint32_t unknownKeys = 0;
};

// ~56 bytes of internal .bss. Deliberately NOT in PSRAM: it is copied inside a
// critical section, and the point of the section is to be short.
static Cm5LinkHealthRecord sLinkHealth;
static portMUX_TYPE sLinkHealthMux = portMUX_INITIALIZER_UNLOCKED;

// Split one `key=value` token. Returns false for anything that is not exactly
// one '=' with a non-empty canonical-decimal u32 on the right.
static bool parseKeyValue(const Token& token, Token& key, uint32_t& value) {
  if (!token.ptr || token.len == 0) return false;
  size_t eq = 0;
  while (eq < token.len && token.ptr[eq] != '=') ++eq;
  if (eq == 0 || eq >= token.len - 1) return false;
  for (size_t i = eq + 1; i < token.len; ++i) {
    if (token.ptr[i] == '=') return false;      // a second '=' is malformed
  }
  key.ptr = token.ptr;
  key.len = eq;
  Token rhs{token.ptr + eq + 1, token.len - eq - 1};
  return parseU32(rhs, value);
}

}  // namespace

Cm5LinkHealthSnapshot cm5LinkHealthSnapshot() {
  Cm5LinkHealthRecord record;
  portENTER_CRITICAL(&sLinkHealthMux);
  record = sLinkHealth;
  portEXIT_CRITICAL(&sLinkHealthMux);

  Cm5LinkHealthSnapshot out{};
  out.seen = record.seen;
  out.ageMs = record.seen
                  ? static_cast<uint32_t>(millis() - record.rxMs) : 0;
  out.sessionEpoch = record.sessionEpoch;
  out.reports = record.reports;
  out.garbage = record.garbage;
  out.corrupt = record.corrupt;
  out.timeouts = record.timeouts;
  out.strays = record.strays;
  out.logins = record.logins;
  out.resets = record.resets;
  out.tx = record.tx;
  out.rx = record.rx;
  out.uptimeS = record.uptimeS;
  out.unknownKeys = record.unknownKeys;
  return out;
}

Cm5LinkHealthIntrinsicResult cm5LinkHealthHandleIntrinsic(
    const char* line, uint32_t activeSessionEpoch,
    bool sessionMayPublishPresence,
    char* reply, size_t replySize) {
  if (!line || !reply || replySize == 0)
    return Cm5LinkHealthIntrinsicResult::NotLinkHealth;

  const char* cursor = line;
  Token root{}, verb{};
  if (!nextToken(cursor, root) || !tokenEqualsNoCase(root, "cm5"))
    return Cm5LinkHealthIntrinsicResult::NotLinkHealth;
  if (!nextToken(cursor, verb) || !tokenEqualsNoCase(verb, "linkhealth"))
    return Cm5LinkHealthIntrinsicResult::NotLinkHealth;

  // A bare `cm5 linkhealth` is the HUMAN read command and belongs to the
  // registry row, not to this control-plane intrinsic. Only a versioned push
  // is ours.
  Token version{};
  if (!nextToken(cursor, version))
    return Cm5LinkHealthIntrinsicResult::NotLinkHealth;

  // From here the line is OURS on every path, refusals included: a host retry
  // must never fall through to cmd_exec and land in the durable audit.
  auto fail = [&](const char* msg) {
    snprintf(reply, replySize, "Error: %s", msg);
    return Cm5LinkHealthIntrinsicResult::Handled;
  };

  if (!tokenEquals(version, "1"))
    return fail("Usage: cm5 linkhealth 1 <key>=<u32> ...");
  if (activeSessionEpoch == 0)
    return fail("cm5 linkhealth requires an authenticated uart session");
  if (!sessionMayPublishPresence)
    return fail("this session may not publish CM5 link health");

  Cm5LinkHealthRecord parsed;
  uint32_t known = 0;
  Token token{};
  while (nextToken(cursor, token)) {
    Token key{};
    uint32_t value = 0;
    if (!parseKeyValue(token, key, value))
      return fail("Usage: cm5 linkhealth 1 <key>=<u32> ...");
    if      (tokenEqualsNoCase(key, "garbage"))  parsed.garbage  = value;
    else if (tokenEqualsNoCase(key, "corrupt"))  parsed.corrupt  = value;
    else if (tokenEqualsNoCase(key, "timeouts")) parsed.timeouts = value;
    else if (tokenEqualsNoCase(key, "strays"))   parsed.strays   = value;
    else if (tokenEqualsNoCase(key, "logins"))   parsed.logins   = value;
    else if (tokenEqualsNoCase(key, "resets"))   parsed.resets   = value;
    else if (tokenEqualsNoCase(key, "tx"))       parsed.tx       = value;
    else if (tokenEqualsNoCase(key, "rx"))       parsed.rx       = value;
    else if (tokenEqualsNoCase(key, "up"))       parsed.uptimeS  = value;
    else { ++parsed.unknownKeys; continue; }
    ++known;
  }
  if (known == 0) return fail("Usage: cm5 linkhealth 1 <key>=<u32> ...");

  parsed.seen = true;
  parsed.rxMs = millis();
  parsed.sessionEpoch = activeSessionEpoch;

  portENTER_CRITICAL(&sLinkHealthMux);
  // Reports are counted HERE, not carried on the wire: this is the device's
  // own count of what it actually accepted, which is the number that says
  // whether the reporter is reaching it.
  parsed.reports = sLinkHealth.reports + 1u;
  sLinkHealth = parsed;
  portEXIT_CRITICAL(&sLinkHealthMux);

  snprintf(reply, replySize, "OK: cm5 linkhealth version=1 keys=%lu unknown=%lu",
           static_cast<unsigned long>(known),
           static_cast<unsigned long>(parsed.unknownKeys));
  return Cm5LinkHealthIntrinsicResult::Handled;
}

namespace {
EXT_RAM_BSS_ATTR static char sCm5CommandReply[384];

static const char* cmdCm5Status(const String& argsInput) {
  if (argsInput.length() != 0) return "Error: Usage: cm5 status";
  writeStatus(sCm5CommandReply, sizeof(sCm5CommandReply));
  return sCm5CommandReply;
}

static void writeLinkHealth(char* reply, size_t replySize, bool json) {
  const Cm5LinkHealthSnapshot h = cm5LinkHealthSnapshot();
  if (json) {
    snprintf(reply, replySize,
             "{\"seen\":%s,\"age_ms\":%lu,\"session_epoch\":%lu,"
             "\"reports\":%lu,\"garbage\":%lu,\"corrupt\":%lu,"
             "\"timeouts\":%lu,\"strays\":%lu,\"logins\":%lu,"
             "\"resets\":%lu,\"tx\":%lu,\"rx\":%lu,\"host_up_s\":%lu,"
             "\"unknown_keys\":%lu}",
             h.seen ? "true" : "false",
             (unsigned long)h.ageMs, (unsigned long)h.sessionEpoch,
             (unsigned long)h.reports, (unsigned long)h.garbage,
             (unsigned long)h.corrupt, (unsigned long)h.timeouts,
             (unsigned long)h.strays, (unsigned long)h.logins,
             (unsigned long)h.resets, (unsigned long)h.tx,
             (unsigned long)h.rx, (unsigned long)h.uptimeS,
             (unsigned long)h.unknownKeys);
    return;
  }
  if (!h.seen) {
    // Name the two ordinary reasons, because "no data" here is far more often
    // a daemon that has not reported yet than a link that is broken.
    snprintf(reply, replySize,
             "CM5 link health: no report yet (the host pushes one at link-up, "
             "every 30s, and after a fault; an older daemon never will)");
    return;
  }
  snprintf(reply, replySize,
           "CM5 link health: age=%lums epoch=%lu reports=%lu | garbage=%lu "
           "corrupt=%lu timeouts=%lu strays=%lu | logins=%lu resets=%lu | "
           "tx=%lu rx=%lu host_up=%lus unknown_keys=%lu",
           (unsigned long)h.ageMs, (unsigned long)h.sessionEpoch,
           (unsigned long)h.reports, (unsigned long)h.garbage,
           (unsigned long)h.corrupt, (unsigned long)h.timeouts,
           (unsigned long)h.strays, (unsigned long)h.logins,
           (unsigned long)h.resets, (unsigned long)h.tx, (unsigned long)h.rx,
           (unsigned long)h.uptimeS, (unsigned long)h.unknownKeys);
}

static const char* cmdCm5LinkHealth(const String& argsInput) {
  String a = argsInput;
  a.trim();
  // The host's own push is a control-plane line consumed before cmd_exec. If a
  // human types one here, say so rather than answering with a usage line that
  // reads like the push was malformed — same courtesy `cm5 heartbeat` gets.
  if (a.startsWith("1 ") || a == "1") {
    return "Error: the cm5 linkhealth report is accepted only on the "
           "authenticated UART host link";
  }
  a.toLowerCase();
  if (a.length() && a != "json") return "Error: Usage: cm5 linkhealth [json]";
  writeLinkHealth(sCm5CommandReply, sizeof(sCm5CommandReply), a == "json");
  return sCm5CommandReply;
}

static const char* cmdCm5Capabilities(const String& argsInput) {
  if (argsInput.length() != 0) return "Error: Usage: cm5 capabilities";
  writeCapabilities(sCm5CommandReply, sizeof(sCm5CommandReply));
  return sCm5CommandReply;
}

static const char* cmdCm5Root(const String& argsInput) {
  String verb = argsInput;
  verb.trim();
  verb.toLowerCase();
  if (verb == "heartbeat" || verb.startsWith("heartbeat ")) {
    return "Error: cm5 heartbeat is available only on the authenticated UART host link";
  }
  if (verb == "status") return cmdCm5Status(String());
  if (verb == "capabilities") return cmdCm5Capabilities(String());
  if (verb == "linkhealth") return cmdCm5LinkHealth(String());
  return "Error: Usage: cm5 <status|capabilities|linkhealth" CM5_ROOT_VERB_HINT
         "> (heartbeat is UART control-plane only)";
}
}  // namespace

// The whole CM5 surface registers here so `help cm5` describes the device in
// one page: presence lease, plus host power/fan when those are compiled in.
// Dispatch is longest-prefix, so the deeper rows win over the bare `cm5` root.
//
// Privilege boundaries stay explicit: these callback rows retain help,
// validation, and explicit non-UART errors, while canonical UART callbacks
// are consumed earlier by the direct intrinsic. Initiation is admin-tier;
// every action that can make the host disappear is superadmin + a same-line
// confirm token (never the process-global interactive confirmation slot).
const CommandEntry cm5PresenceCommands[] = {
    {"cm5 status", "Inspect the setup-agnostic CM5 service-presence lease.",
     false, cmdCm5Status, "Usage: cm5 status"},
    {"cm5 capabilities", "Show the CM5 presence protocol capabilities.",
     false, cmdCm5Capabilities, "Usage: cm5 capabilities"},
    {"cm5 linkhealth", "Show the CM5 host's UART link fault tally.",
     false, cmdCm5LinkHealth, "Usage: cm5 linkhealth [json]"},
#if ENABLE_RASPBERRY_PI_HOST_POWER
    {"cm5 power ack", "Accept a CM5 delivery/application ACK (UART session only).",
     false, cmdCm5PowerAck,
     "Usage: cm5 power ack 1 <16-hex-id> <accepted|committed|applied|failed>"},
    {"cm5 power report", "Accept finite CM5 power state/profile readback (UART session only).",
     false, cmdCm5PowerReport,
     "Usage: cm5 power report 1 <16-hex-id|0> <state> <profile> <32-hex-linux-boot-id>"},
    {"cm5 power reboot", "Request a confirmed CM5 reboot.", true,
     cmdCm5PowerReboot, "Usage: cm5 power reboot confirm",
     /*requiresSuperAdmin=*/true},
    {"cm5 power halt", "Request a confirmed CM5 halt.", true,
     cmdCm5PowerHalt, "Usage: cm5 power halt confirm",
     /*requiresSuperAdmin=*/true},
    {"cm5 power suspend", "Request confirmed CM5 system suspend (host may reject it).",
     true, cmdCm5PowerSuspend, "Usage: cm5 power suspend confirm",
     /*requiresSuperAdmin=*/true},
    {"cm5 power sleep_for", "Request confirmed CM5 timed sleep in bounded minutes.",
     true, cmdCm5PowerSleepFor,
     "Usage: cm5 power sleep_for <1..1440 minutes> confirm",
     /*requiresSuperAdmin=*/true},
    {"cm5 power recover", "Clear a fail-closed uncertain CM5 transition after inspection.",
     true, cmdCm5PowerRecover, "Usage: cm5 power recover confirm",
     /*requiresSuperAdmin=*/true},
    {"cm5 power", "Inspect or request CM5 host power/profile state.", true,
     cmdCm5Power,
     "Usage: cm5 power [show|status|profile <eco|balanced|performance|auto>]"},
#endif
#if ENABLE_RASPBERRY_PI_HOST_FAN
    {"cm5 fan ack", "Accept a CM5 fan ACK (authenticated UART session only).",
     false, cmdCm5FanAck,
     "Usage: cm5 fan ack 1 <16-hex-id> <accepted|applied|failed>"},
    {"cm5 fan report", "Accept bounded CM5 fan readback (authenticated UART session only).",
     false, cmdCm5FanReport,
     "Usage: cm5 fan report 1 <id> <requested-mode> <effective-mode> <temp-mc|-1> <target-pwm> <pwm> <rpm|-1> <health>"},
    {"cm5 fan", "Inspect or request CM5 fan mode/readback.", true,
     cmdCm5Fan, "Usage: cm5 fan [show|status|quiet|auto|max]"},
#endif
    {"cm5", "Inspect CM5 service presence, link health, and host power/fan control.",
     false, cmdCm5Root,
     "Usage: cm5 <status|capabilities|linkhealth" CM5_ROOT_VERB_HINT
     "> (heartbeat is UART control-plane only)"},
};

const size_t cm5PresenceCommandsCount =
    sizeof(cm5PresenceCommands) / sizeof(cm5PresenceCommands[0]);
