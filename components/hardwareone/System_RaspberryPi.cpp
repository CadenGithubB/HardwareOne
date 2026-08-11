#include "System_RaspberryPi.h"

#if ENABLE_RASPBERRY_PI_HOST_POWER

#include <esp_system.h>
#include <freertos/FreeRTOS.h>

#include "System_AuthIdentity.h"
#include "System_Command.h"
#include "System_UartLink.h"
#include "System_User.h"
#include "System_Utils.h"

namespace {

constexpr uint16_t kMinSleepMinutes = 1;
constexpr uint16_t kMaxSleepMinutes = 1440;
constexpr const char* kProtocolVersion = "1";
// The CM5 serial session deliberately has a single command writer. Model
// startup or a long binary transfer can therefore delay a control ACK well
// beyond thirty seconds even though the reader has already received the EVT.
// Keep retrying through 127 seconds so those finite in-flight operations do
// not turn a valid power request into an artificial delivery failure.
constexpr uint8_t kMaxDeliveryAttempts = 7;
// Once a non-destructive request is accepted the helper may still consume its
// 20-second bound and the final report can queue behind a 65-second UART
// command. Keep the record alive long enough to accept that finite readback.
constexpr uint32_t kCompletionTimeoutMs = 120000;
constexpr uint32_t kRetryBackoffMs[kMaxDeliveryAttempts] = {
    1000, 2000, 4000, 8000, 16000, 32000, 64000,
};

enum class HostPowerAction : uint8_t {
  None,
  Status,
  ProfileEco,
  ProfileBalanced,
  ProfilePerformance,
  ProfileAuto,
  Reboot,
  Halt,
  Suspend,
  SleepFor,
};

enum class RequestState : uint8_t {
  Idle,
  Queued,
  AwaitingAck,
  RetryWait,
  Accepted,
  Committed,
  Applied,
  Failed,
  DeliveryFailed,
  CompletionTimeout,
};

enum class HostState : uint8_t {
  Unknown,
  Awake,
  Sleeping,
  Suspending,
  Rebooting,
  Halting,
  Error,
};

enum class HostProfile : uint8_t {
  Unknown,
  Eco,
  Balanced,
  Performance,
  Auto,
};

struct RequestId {
  uint32_t bootNonce = 0;
  uint32_t counter = 0;
};

struct HostBootTag {
  uint8_t bytes[16] = {};
  bool valid = false;
};

struct RequestRecord {
  RequestId id;
  HostPowerAction action = HostPowerAction::None;
  RequestState state = RequestState::Idle;
  uint16_t sleepMinutes = 0;
  uint8_t attempts = 0;
  bool pending = false;
  bool retryEnabled = false;
  uint32_t createdMs = 0;
  uint32_t nextDueMs = 0;
};

struct HostReport {
  bool valid = false;
  RequestId requestId;
  HostState state = HostState::Unknown;
  HostProfile profile = HostProfile::Unknown;
  HostBootTag bootTag;
  uint32_t updatedMs = 0;
};

static portMUX_TYPE sStateMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t sBootNonce = 0;
static uint32_t sLastCounter = 0;
static RequestRecord sRequest;
static HostReport sHostReport;
// Destructive requests stop delivery retries as soon as the CM5 accepts
// responsibility, but that must not open a window for a second transition to
// queue before the first one finishes or the host returns after reboot/wake.
static bool sTransitionInFlight = false;
// Linux /proc/sys/kernel/random/boot_id captured when a destructive request
// is accepted. A later ready report can then distinguish a daemon-only
// restart from an actual host boot without trusting elapsed time.
static HostBootTag sTransitionBootTag;
static char sReply[640];

bool requestIdEqual(const RequestId& a, const RequestId& b) {
  return a.bootNonce == b.bootNonce && a.counter == b.counter;
}

bool timeDue(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

const char* actionName(HostPowerAction action) {
  switch (action) {
    case HostPowerAction::Status: return "status";
    case HostPowerAction::ProfileEco: return "profile_eco";
    case HostPowerAction::ProfileBalanced: return "profile_balanced";
    case HostPowerAction::ProfilePerformance: return "profile_performance";
    case HostPowerAction::ProfileAuto: return "profile_auto";
    case HostPowerAction::Reboot: return "reboot";
    case HostPowerAction::Halt: return "halt";
    case HostPowerAction::Suspend: return "suspend";
    case HostPowerAction::SleepFor: return "sleep_for";
    default: return "none";
  }
}

const char* requestStateName(RequestState state) {
  switch (state) {
    case RequestState::Queued: return "queued";
    case RequestState::AwaitingAck: return "awaiting_ack";
    case RequestState::RetryWait: return "retry_wait";
    case RequestState::Accepted: return "accepted";
    case RequestState::Committed: return "committed";
    case RequestState::Applied: return "applied";
    case RequestState::Failed: return "failed";
    case RequestState::DeliveryFailed: return "delivery_failed";
    case RequestState::CompletionTimeout: return "completion_timeout";
    default: return "idle";
  }
}

const char* hostStateName(HostState state) {
  switch (state) {
    case HostState::Awake: return "awake";
    case HostState::Sleeping: return "sleeping";
    case HostState::Suspending: return "suspending";
    case HostState::Rebooting: return "rebooting";
    case HostState::Halting: return "halting";
    case HostState::Error: return "error";
    default: return "unknown";
  }
}

const char* profileName(HostProfile profile) {
  switch (profile) {
    case HostProfile::Eco: return "eco";
    case HostProfile::Balanced: return "balanced";
    case HostProfile::Performance: return "performance";
    case HostProfile::Auto: return "auto";
    default: return "unknown";
  }
}

bool actionIsDestructive(HostPowerAction action) {
  return action == HostPowerAction::Reboot ||
         action == HostPowerAction::Halt ||
         action == HostPowerAction::Suspend ||
         action == HostPowerAction::SleepFor;
}

bool appliedAckCompletesTransition(HostPowerAction action) {
  // The suspend helper returns only after resume. reboot/halt/timed-sleep use
  // systemd --no-block, so their Applied ACK means "job queued", not "host
  // has completed the transition"; those stay latched until a later boot/wake
  // report.
  return action == HostPowerAction::Suspend;
}

HostProfile actionProfile(HostPowerAction action) {
  switch (action) {
    case HostPowerAction::ProfileEco: return HostProfile::Eco;
    case HostPowerAction::ProfileBalanced: return HostProfile::Balanced;
    case HostPowerAction::ProfilePerformance: return HostProfile::Performance;
    case HostPowerAction::ProfileAuto: return HostProfile::Auto;
    default: return HostProfile::Unknown;
  }
}

bool formatRequestId(const RequestId& id, char* out, size_t outSize) {
  if (out == nullptr || outSize < 17 || id.bootNonce == 0 || id.counter == 0) {
    return false;
  }
  const int n = snprintf(out, outSize, "%08lx%08lx",
                         static_cast<unsigned long>(id.bootNonce),
                         static_cast<unsigned long>(id.counter));
  return n == 16;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseHostBootTag(const String& token, HostBootTag& out) {
  if (token.length() != 32) return false;
  HostBootTag parsed;
  bool nonzero = false;
  for (size_t i = 0; i < sizeof(parsed.bytes); ++i) {
    const int hi = hexNibble(token[i * 2]);
    const int lo = hexNibble(token[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    parsed.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    nonzero = nonzero || parsed.bytes[i] != 0;
  }
  // All zeroes is a syntactically valid fail-closed "unknown" tag. It lets
  // status/profile reports work on a damaged Linux image, but can never prove
  // a boot boundary or authorize a destructive request.
  parsed.valid = nonzero;
  out = parsed;
  return true;
}

bool hostBootTagEqual(const HostBootTag& a, const HostBootTag& b) {
  if (!a.valid || !b.valid) return false;
  for (size_t i = 0; i < sizeof(a.bytes); ++i) {
    if (a.bytes[i] != b.bytes[i]) return false;
  }
  return true;
}

void formatHostBootTag(const HostBootTag& tag, char* out, size_t outSize) {
  static constexpr char kHex[] = "0123456789abcdef";
  if (out == nullptr || outSize < 33) return;
  for (size_t i = 0; i < sizeof(tag.bytes); ++i) {
    out[i * 2] = kHex[tag.bytes[i] >> 4];
    out[i * 2 + 1] = kHex[tag.bytes[i] & 0x0f];
  }
  out[32] = '\0';
}

bool parseRequestId(const String& token, RequestId& out, bool allowZero) {
  if (allowZero && token == "0") {
    out = RequestId{};
    return true;
  }
  if (token.length() != 16) return false;

  uint32_t hi = 0;
  uint32_t lo = 0;
  for (size_t i = 0; i < 16; ++i) {
    const int nibble = hexNibble(token[i]);
    if (nibble < 0) return false;
    if (i < 8) hi = (hi << 4) | static_cast<uint32_t>(nibble);
    else lo = (lo << 4) | static_cast<uint32_t>(nibble);
  }
  if (hi == 0 || lo == 0) return false;
  out.bootNonce = hi;
  out.counter = lo;
  return true;
}

bool parseMinutes(const String& token, uint16_t& minutesOut) {
  if (token.length() == 0 || token.length() > 4) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < token.length(); ++i) {
    const char c = token[i];
    if (c < '0' || c > '9') return false;
    value = value * 10U + static_cast<uint32_t>(c - '0');
  }
  if (value < kMinSleepMinutes || value > kMaxSleepMinutes) return false;
  minutesOut = static_cast<uint16_t>(value);
  return true;
}

bool parseHostState(const String& token, HostState& out) {
  if (token.equalsIgnoreCase("unknown")) out = HostState::Unknown;
  else if (token.equalsIgnoreCase("awake")) out = HostState::Awake;
  else if (token.equalsIgnoreCase("sleeping")) out = HostState::Sleeping;
  else if (token.equalsIgnoreCase("suspending")) out = HostState::Suspending;
  else if (token.equalsIgnoreCase("rebooting")) out = HostState::Rebooting;
  else if (token.equalsIgnoreCase("halting")) out = HostState::Halting;
  else if (token.equalsIgnoreCase("error")) out = HostState::Error;
  else return false;
  return true;
}

bool parseHostProfile(const String& token, HostProfile& out) {
  if (token.equalsIgnoreCase("unknown")) out = HostProfile::Unknown;
  else if (token.equalsIgnoreCase("eco")) out = HostProfile::Eco;
  else if (token.equalsIgnoreCase("balanced")) out = HostProfile::Balanced;
  else if (token.equalsIgnoreCase("performance")) out = HostProfile::Performance;
  else if (token.equalsIgnoreCase("auto")) out = HostProfile::Auto;
  else return false;
  return true;
}

bool realAuthenticatedUartCaller() {
  const AuthContext& ctx = currentAuthContext();
  // Match the UART frame/voicefetch trust boundary: the captured command
  // context must be UART-originated and the live session must still be logged
  // in. The synchronized username snapshot is not needed here; the nonzero
  // active login epoch is the UART module's atomic revocation gate.
  return ctx.transport == SOURCE_UART && uartLinkSessionEpoch() != 0 &&
         ctx.user.length() > 0 && ctx.user != "AuthBypass";
}

bool formatEvent(const RequestRecord& request, char* out, size_t outSize) {
  char id[17];
  if (!formatRequestId(request.id, id, sizeof(id))) return false;

  int n = -1;
  switch (request.action) {
    case HostPowerAction::Status:
      n = snprintf(out, outSize, "hostpower_status %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::ProfileEco:
      n = snprintf(out, outSize, "hostpower_profile_eco %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::ProfileBalanced:
      n = snprintf(out, outSize, "hostpower_profile_balanced %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::ProfilePerformance:
      n = snprintf(out, outSize, "hostpower_profile_performance %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::ProfileAuto:
      n = snprintf(out, outSize, "hostpower_profile_auto %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::Reboot:
      n = snprintf(out, outSize, "hostpower_reboot %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::Halt:
      n = snprintf(out, outSize, "hostpower_halt %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::Suspend:
      n = snprintf(out, outSize, "hostpower_suspend %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::SleepFor:
      n = snprintf(out, outSize, "hostpower_sleep_for %s %s %u",
                   kProtocolVersion, id,
                   static_cast<unsigned>(request.sleepMinutes));
      break;
    default:
      return false;
  }
  return n > 0 && static_cast<size_t>(n) < outSize;
}

const char* showState() {
  RequestRecord request;
  HostReport report;
  uint32_t bootNonce;
  bool transitionInFlight;
  HostBootTag transitionBootTag;
  const uint32_t now = millis();
  portENTER_CRITICAL(&sStateMux);
  request = sRequest;
  report = sHostReport;
  bootNonce = sBootNonce;
  transitionInFlight = sTransitionInFlight;
  transitionBootTag = sTransitionBootTag;
  portEXIT_CRITICAL(&sStateMux);

  char requestId[17] = "none";
  if (request.action != HostPowerAction::None) {
    (void)formatRequestId(request.id, requestId, sizeof(requestId));
  }
  char reportId[17] = "0";
  if (report.valid && report.requestId.counter != 0) {
    (void)formatRequestId(report.requestId, reportId, sizeof(reportId));
  }
  char hostBootTag[33];
  char transitionTag[33];
  formatHostBootTag(report.bootTag, hostBootTag, sizeof(hostBootTag));
  formatHostBootTag(transitionBootTag, transitionTag, sizeof(transitionTag));

  const uint32_t requestAge = request.action == HostPowerAction::None
                                  ? 0
                                  : now - request.createdMs;
  const uint32_t dueIn = request.pending && !timeDue(now, request.nextDueMs)
                             ? request.nextDueMs - now
                             : 0;
  const uint32_t reportAge = report.valid ? now - report.updatedMs : 0;

  snprintf(sReply, sizeof(sReply),
           "Host power: boot=%08lx id=%s action=%s state=%s pending=%s transition=%s "
           "transition_host_boot=%s "
           "attempts=%u/%u age_ms=%lu next_due_ms=%lu; "
           "CM5 report: valid=%s id=%s state=%s profile=%s host_boot=%s age_ms=%lu",
           static_cast<unsigned long>(bootNonce), requestId,
           actionName(request.action), requestStateName(request.state),
           request.pending ? "yes" : "no",
           transitionInFlight ? "in_flight" : "idle",
           transitionBootTag.valid ? transitionTag : "unknown",
           static_cast<unsigned>(request.attempts),
           static_cast<unsigned>(kMaxDeliveryAttempts),
           static_cast<unsigned long>(requestAge),
           static_cast<unsigned long>(dueIn), report.valid ? "yes" : "no",
           reportId, hostStateName(report.state), profileName(report.profile),
           report.bootTag.valid ? hostBootTag : "unknown",
           static_cast<unsigned long>(reportAge));
  return sReply;
}

const char* queueRequest(HostPowerAction action, uint16_t sleepMinutes = 0) {
  if (!uartLinkIsRunning() || uartLinkSessionEpoch() == 0) {
    return "Error: CM5 UART link is not running with an authenticated session";
  }

  RequestRecord queued;
  const uint32_t now = millis();
  portENTER_CRITICAL(&sStateMux);
  if (sRequest.pending || sTransitionInFlight) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: a host-power request or host transition is already in flight; use 'hostpower show'";
  }
  if (actionIsDestructive(action) && !sHostReport.bootTag.valid) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: CM5 has not published a valid Linux boot ID; destructive host-power control is not ready";
  }
  if (sBootNonce == 0 || sLastCounter == UINT32_MAX) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: host-power request ID space is unavailable until reboot";
  }

  queued.id.bootNonce = sBootNonce;
  queued.id.counter = ++sLastCounter;
  queued.action = action;
  queued.state = RequestState::Queued;
  queued.sleepMinutes = sleepMinutes;
  queued.attempts = 0;
  queued.pending = true;
  queued.retryEnabled = true;
  queued.createdMs = now;
  queued.nextDueMs = now;
  sRequest = queued;
  portEXIT_CRITICAL(&sStateMux);

  char id[17];
  (void)formatRequestId(queued.id, id, sizeof(id));
  snprintf(sReply, sizeof(sReply),
           "OK: queued host-power %s id=%s; use 'hostpower show' for delivery state",
           actionName(action), id);
  return sReply;
}

const char* requireLiteralConfirm(const String& args) {
  CommandArgs parsed(args);
  if (parsed.count() != 1 || !parsed.arg(0).equalsIgnoreCase("confirm")) {
    return "Error: destructive host-power operation requires literal 'confirm'";
  }
  return nullptr;
}

const char* cmdHostPower(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs args(argsInput);
  if (args.count() == 0 ||
      (args.count() == 1 && args.arg(0).equalsIgnoreCase("show"))) {
    return showState();
  }
  if (args.count() == 1 && args.arg(0).equalsIgnoreCase("status")) {
    return queueRequest(HostPowerAction::Status);
  }
  if (args.count() == 2 && args.arg(0).equalsIgnoreCase("profile")) {
    if (args.arg(1).equalsIgnoreCase("eco")) {
      return queueRequest(HostPowerAction::ProfileEco);
    }
    if (args.arg(1).equalsIgnoreCase("balanced")) {
      return queueRequest(HostPowerAction::ProfileBalanced);
    }
    if (args.arg(1).equalsIgnoreCase("performance")) {
      return queueRequest(HostPowerAction::ProfilePerformance);
    }
    if (args.arg(1).equalsIgnoreCase("auto")) {
      return queueRequest(HostPowerAction::ProfileAuto);
    }
    return "Error: profile must be eco, balanced, performance, or auto";
  }
  return "Usage: hostpower [show|status|profile <eco|balanced|performance|auto>]";
}

const char* cmdHostPowerReboot(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = requireLiteralConfirm(argsInput)) return error;
  return queueRequest(HostPowerAction::Reboot);
}

const char* cmdHostPowerHalt(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = requireLiteralConfirm(argsInput)) return error;
  return queueRequest(HostPowerAction::Halt);
}

const char* cmdHostPowerSuspend(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = requireLiteralConfirm(argsInput)) return error;
  return queueRequest(HostPowerAction::Suspend);
}

const char* cmdHostPowerSleepFor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs args(argsInput);
  uint16_t minutes = 0;
  if (args.count() != 2 || !parseMinutes(args.arg(0), minutes) ||
      !args.arg(1).equalsIgnoreCase("confirm")) {
    return "Error: use sleep_for <1..1440 minutes> confirm";
  }
  return queueRequest(HostPowerAction::SleepFor, minutes);
}

const char* cmdHostPowerRecover(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = requireLiteralConfirm(argsInput)) return error;

  portENTER_CRITICAL(&sStateMux);
  if (!sTransitionInFlight || !actionIsDestructive(sRequest.action)) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: no uncertain host-power transition requires recovery";
  }
  sRequest.state = RequestState::Failed;
  sRequest.pending = false;
  sRequest.retryEnabled = false;
  sRequest.nextDueMs = 0;
  sTransitionInFlight = false;
  sTransitionBootTag = HostBootTag{};
  portEXIT_CRITICAL(&sStateMux);
  return "OK: cleared the uncertain host-power transition; verify CM5 state before issuing another destructive request";
}

const char* cmdHostPowerAck(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!realAuthenticatedUartCaller()) {
    return "Error: host-power ACK is accepted only from the authenticated UART session";
  }

  CommandArgs args(argsInput);
  RequestId id;
  if (args.count() != 3) {
    return "Usage: hostpower ack 1 <16-hex-id> <accepted|committed|applied|failed>";
  }
  if (args.arg(0) != kProtocolVersion) {
    return "Error: unsupported host-power protocol version (expected 1)";
  }
  if (!parseRequestId(args.arg(1), id, false)) {
    return "Error: host-power request ID must be exactly 16 hexadecimal characters";
  }

  RequestState requestedState;
  if (args.arg(2).equalsIgnoreCase("accepted")) {
    requestedState = RequestState::Accepted;
  } else if (args.arg(2).equalsIgnoreCase("committed")) {
    requestedState = RequestState::Committed;
  } else if (args.arg(2).equalsIgnoreCase("applied")) {
    requestedState = RequestState::Applied;
  } else if (args.arg(2).equalsIgnoreCase("failed")) {
    requestedState = RequestState::Failed;
  } else {
    return "Error: ACK state must be accepted, committed, applied, or failed";
  }

  const uint32_t now = millis();
  bool duplicate = false;
  bool conflict = false;
  portENTER_CRITICAL(&sStateMux);
  if (sRequest.action == HostPowerAction::None ||
      !requestIdEqual(sRequest.id, id)) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: ACK request ID does not match the current host-power request";
  }

  if (sRequest.state == requestedState) {
    duplicate = true;
    if (actionIsDestructive(sRequest.action)) {
      if (requestedState == RequestState::Accepted ||
          requestedState == RequestState::Committed) {
        sTransitionInFlight = true;
      } else if (requestedState == RequestState::Failed ||
                 (requestedState == RequestState::Applied &&
                  appliedAckCompletesTransition(sRequest.action))) {
        sTransitionInFlight = false;
        sTransitionBootTag = HostBootTag{};
      }
    }
  } else if (sRequest.state == RequestState::Applied ||
             sRequest.state == RequestState::Failed ||
             sRequest.state == RequestState::DeliveryFailed ||
             sRequest.state == RequestState::CompletionTimeout) {
    conflict = true;
  } else {
    const bool destructive = actionIsDestructive(sRequest.action);
    // Monotonic contract: all work is first Accepted. Destructive work then
    // crosses a second, confirmed Committed boundary immediately before the
    // helper may run. This lets a later same-boot daemon startup safely clear
    // Accepted while keeping Committed/Applied fail-closed.
    const bool validTransition =
        requestedState == RequestState::Failed ||
        (requestedState == RequestState::Accepted &&
         (sRequest.state == RequestState::Queued ||
          sRequest.state == RequestState::AwaitingAck ||
          sRequest.state == RequestState::RetryWait)) ||
        (requestedState == RequestState::Committed && destructive &&
         sRequest.state == RequestState::Accepted) ||
        (requestedState == RequestState::Applied &&
         sRequest.state == (destructive ? RequestState::Committed
                                        : RequestState::Accepted));
    if (!validTransition) {
      conflict = true;
    } else {
      sRequest.state = requestedState;
      sRequest.retryEnabled = false;
      if (requestedState == RequestState::Accepted && destructive) {
        sTransitionInFlight = true;
        sTransitionBootTag = sHostReport.bootTag;
      } else if (requestedState == RequestState::Failed && destructive) {
        sTransitionInFlight = false;
        sTransitionBootTag = HostBootTag{};
      } else if (requestedState == RequestState::Applied && destructive &&
                 appliedAckCompletesTransition(sRequest.action)) {
        sTransitionInFlight = false;
        sTransitionBootTag = HostBootTag{};
      }
      if (requestedState == RequestState::Accepted && !destructive) {
        sRequest.pending = true;
        sRequest.nextDueMs = now + kCompletionTimeoutMs;
      } else {
        // Destructive delivery retries stop at Accepted and then progress by
        // explicit callbacks; profile/status remain pending for completion.
        sRequest.pending = false;
        sRequest.nextDueMs = 0;
      }
    }
  }
  portEXIT_CRITICAL(&sStateMux);

  if (conflict) return "Error: ACK conflicts with the current request state";
  char idText[17];
  (void)formatRequestId(id, idText, sizeof(idText));
  snprintf(sReply, sizeof(sReply), "OK: host-power ACK id=%s state=%s%s",
           idText, requestStateName(requestedState),
           duplicate ? " duplicate" : "");
  return sReply;
}

const char* cmdHostPowerReport(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!realAuthenticatedUartCaller()) {
    return "Error: CM5 status is accepted only from the authenticated UART session";
  }

  CommandArgs args(argsInput);
  RequestId id;
  HostState state;
  HostProfile profile;
  HostBootTag bootTag;
  if (args.count() != 5) {
    return "Usage: hostpower report 1 <16-hex-id|0> <state> <profile> <32-hex-linux-boot-id>";
  }
  if (args.arg(0) != kProtocolVersion) {
    return "Error: unsupported host-power protocol version (expected 1)";
  }
  if (!parseRequestId(args.arg(1), id, true) ||
      !parseHostState(args.arg(2), state) ||
      !parseHostProfile(args.arg(3), profile) ||
      !parseHostBootTag(args.arg(4), bootTag)) {
    return "Usage: hostpower report 1 <16-hex-id|0> <unknown|awake|sleeping|suspending|rebooting|halting|error> <unknown|eco|balanced|performance|auto> <32-hex-linux-boot-id>";
  }

  const uint32_t now = millis();
  portENTER_CRITICAL(&sStateMux);
  sHostReport.valid = true;
  sHostReport.requestId = id;
  sHostReport.state = state;
  sHostReport.profile = profile;
  sHostReport.bootTag = bootTag;
  sHostReport.updatedMs = now;

  const bool matchesRequest = id.counter != 0 &&
                              requestIdEqual(sRequest.id, id);
  if (id.counter == 0 && state == HostState::Awake &&
      sTransitionInFlight && actionIsDestructive(sRequest.action)) {
    const bool sameBoot = hostBootTagEqual(bootTag, sTransitionBootTag);
    const bool changedBoot = bootTag.valid && sTransitionBootTag.valid &&
                             !sameBoot;
    if (changedBoot && sRequest.state == RequestState::Accepted) {
      // This request never crossed Committed, so it cannot have invoked the
      // helper. The host reboot was unrelated; recover safely but do not
      // misreport a timed sleep/reboot request as applied.
      sRequest.state = RequestState::Failed;
      sRequest.pending = false;
      sRequest.retryEnabled = false;
      sRequest.nextDueMs = 0;
      sTransitionInFlight = false;
      sTransitionBootTag = HostBootTag{};
    } else if (changedBoot &&
               (sRequest.state == RequestState::Committed ||
                sRequest.state == RequestState::Applied)) {
      // A real Linux boot boundary completed the requested transition even if
      // the no-block helper's final Applied reply was lost during shutdown.
      sRequest.state = RequestState::Applied;
      sRequest.pending = false;
      sRequest.retryEnabled = false;
      sRequest.nextDueMs = 0;
      sTransitionInFlight = false;
      sTransitionBootTag = HostBootTag{};
    } else if (sameBoot && sRequest.state == RequestState::Accepted) {
      // A new controller process is ready on the same Linux boot. Because the
      // CM5 never crossed the confirmed Committed boundary, the helper could
      // not legally have run; fail and reopen safely without re-execution.
      sRequest.state = RequestState::Failed;
      sRequest.pending = false;
      sRequest.retryEnabled = false;
      sRequest.nextDueMs = 0;
      sTransitionInFlight = false;
      sTransitionBootTag = HostBootTag{};
    }
    // Same-boot Committed/Applied stays fail-closed: systemd may already have
    // queued the transition. A superadmin can inspect and explicitly recover
    // it if the host proves stable and no transition is pending.
  }
  const bool committedDestructive =
      actionIsDestructive(sRequest.action) &&
      (sRequest.state == RequestState::Committed ||
       sRequest.state == RequestState::Applied);
  if (matchesRequest && state == HostState::Error &&
      !committedDestructive) {
    sRequest.state = RequestState::Failed;
    sRequest.pending = false;
    sRequest.retryEnabled = false;
    sRequest.nextDueMs = 0;
    if (actionIsDestructive(sRequest.action)) {
      sTransitionInFlight = false;
      sTransitionBootTag = HostBootTag{};
    }
  } else if (matchesRequest && state != HostState::Error &&
             sRequest.pending &&
             sRequest.state == RequestState::Accepted) {
    const HostProfile requestedProfile = actionProfile(sRequest.action);
    if (sRequest.action == HostPowerAction::Status ||
        (requestedProfile != HostProfile::Unknown &&
         requestedProfile == profile)) {
      // A matching finite readback is equivalent to an applied ACK.
      sRequest.state = RequestState::Applied;
      sRequest.pending = false;
      sRequest.retryEnabled = false;
      sRequest.nextDueMs = 0;
    }
  }
  portEXIT_CRITICAL(&sStateMux);

  char idText[17] = "0";
  if (id.counter != 0) (void)formatRequestId(id, idText, sizeof(idText));
  char bootText[33];
  formatHostBootTag(bootTag, bootText, sizeof(bootText));
  snprintf(sReply, sizeof(sReply),
           "OK: CM5 report id=%s state=%s profile=%s host_boot=%s", idText,
           hostStateName(state), profileName(profile),
           bootTag.valid ? bootText : "unknown");
  return sReply;
}

}  // namespace

void raspberryPiHostPowerInit() {
  uint32_t nonce = esp_random();
  if (nonce == 0) nonce = esp_random();
  if (nonce == 0) nonce = 1;  // defensive only; esp_random() is hardware-backed

  portENTER_CRITICAL(&sStateMux);
  sBootNonce = nonce;
  sLastCounter = 0;
  sRequest = RequestRecord{};
  sHostReport = HostReport{};
  sTransitionInFlight = false;
  sTransitionBootTag = HostBootTag{};
  portEXIT_CRITICAL(&sStateMux);
}

void raspberryPiHostPowerTick() {
  const uint32_t now = millis();
  RequestRecord request;

  portENTER_CRITICAL(&sStateMux);
  request = sRequest;
  if (request.pending && !request.retryEnabled &&
      request.state == RequestState::Accepted &&
      timeDue(now, request.nextDueMs)) {
    sRequest.state = RequestState::CompletionTimeout;
    sRequest.pending = false;
    sRequest.nextDueMs = 0;
    portEXIT_CRITICAL(&sStateMux);
    return;
  }
  portEXIT_CRITICAL(&sStateMux);

  if (!request.pending || !request.retryEnabled ||
      !timeDue(now, request.nextDueMs)) {
    return;
  }

  if (request.attempts >= kMaxDeliveryAttempts) {
    portENTER_CRITICAL(&sStateMux);
    if (sRequest.pending && sRequest.retryEnabled &&
        requestIdEqual(sRequest.id, request.id)) {
      sRequest.state = RequestState::DeliveryFailed;
      sRequest.pending = false;
      sRequest.retryEnabled = false;
      sRequest.nextDueMs = 0;
    }
    portEXIT_CRITICAL(&sStateMux);
    return;
  }

  char event[80];
  if (!formatEvent(request, event, sizeof(event))) return;
  const bool sent = uartLinkTryPushEvent(event);

  portENTER_CRITICAL(&sStateMux);
  if (sRequest.pending && sRequest.retryEnabled &&
      requestIdEqual(sRequest.id, request.id)) {
    const uint8_t attemptIndex = sRequest.attempts;
    if (sRequest.attempts < kMaxDeliveryAttempts) ++sRequest.attempts;
    sRequest.state = sent ? RequestState::AwaitingAck : RequestState::RetryWait;
    sRequest.nextDueMs = now + kRetryBackoffMs[attemptIndex];
  }
  portEXIT_CRITICAL(&sStateMux);
}

// Longest-match registry entries make privilege boundaries explicit: CM5
// callbacks are user-tier but UART-session-only; initiation is admin-tier;
// every action that can make the host disappear is superadmin + same-line
// confirmation (never the process-global interactive confirmation slot).
const CommandEntry raspberryPiHostPowerCommands[] = {
    {"hostpower ack", "Accept a CM5 delivery/application ACK (UART session only).",
     false, cmdHostPowerAck,
     "Usage: hostpower ack 1 <16-hex-id> <accepted|committed|applied|failed>"},
    {"hostpower report", "Accept finite CM5 power state/profile readback (UART session only).",
     false, cmdHostPowerReport,
     "Usage: hostpower report 1 <16-hex-id|0> <state> <profile> <32-hex-linux-boot-id>"},
    {"hostpower reboot", "Request a confirmed CM5 reboot.", true,
     cmdHostPowerReboot, "Usage: hostpower reboot confirm",
     /*requiresSuperAdmin=*/true},
    {"hostpower halt", "Request a confirmed CM5 halt.", true,
     cmdHostPowerHalt, "Usage: hostpower halt confirm",
     /*requiresSuperAdmin=*/true},
    {"hostpower suspend", "Request confirmed CM5 system suspend (host may reject it).",
     true, cmdHostPowerSuspend, "Usage: hostpower suspend confirm",
     /*requiresSuperAdmin=*/true},
    {"hostpower sleep_for", "Request confirmed CM5 timed sleep in bounded minutes.",
     true, cmdHostPowerSleepFor,
     "Usage: hostpower sleep_for <1..1440 minutes> confirm",
     /*requiresSuperAdmin=*/true},
    {"hostpower recover", "Clear a fail-closed uncertain CM5 transition after inspection.",
     true, cmdHostPowerRecover, "Usage: hostpower recover confirm",
     /*requiresSuperAdmin=*/true},
    {"hostpower", "Inspect or request CM5 host power/profile state.", true,
     cmdHostPower,
     "Usage: hostpower [show|status|profile <eco|balanced|performance|auto>]"},
};

const size_t raspberryPiHostPowerCommandsCount =
    sizeof(raspberryPiHostPowerCommands) /
    sizeof(raspberryPiHostPowerCommands[0]);

#endif  // ENABLE_RASPBERRY_PI_HOST_POWER
