#include "System_Cm5HostControl.h"

#if ENABLE_RASPBERRY_PI_HOST_POWER || ENABLE_RASPBERRY_PI_HOST_FAN

#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>

#include "System_AuthIdentity.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_UartLink.h"
#include "System_User.h"
#include "System_Utils.h"

namespace {

// ═══════════════════════════════════════════════════════════════════════════
// Shared CM5 request/ACK/report plumbing
//
// `cm5 power` and `cm5 fan` are separate protocols with separate request records,
// but they are the same finite delivery machine: allocate a boot-unique ID,
// push a fixed EVT token to the authenticated UART session, retry on a bounded
// backoff, then let the host drive the record forward with ACK/report
// callbacks. Everything both protocols agree on lives here exactly once.
// ═══════════════════════════════════════════════════════════════════════════

constexpr const char* kProtocolVersion = "1";
// The CM5 serial session deliberately has a single command writer. Model
// startup or a long binary transfer can therefore delay a control ACK well
// beyond thirty seconds even though the reader has already received the EVT.
// Keep retrying through 127 seconds so those finite in-flight operations do
// not turn a valid request into an artificial delivery failure.
constexpr uint8_t kMaxDeliveryAttempts = 7;
// Once a non-destructive request is accepted the helper may still consume its
// 20-second bound and the final report can queue behind a 65-second UART
// command. Keep the record alive long enough to accept that finite readback.
constexpr uint32_t kCompletionTimeoutMs = 120000;
constexpr uint32_t kRetryBackoffMs[kMaxDeliveryAttempts] = {
    1000, 2000, 4000, 8000, 16000, 32000, 64000,
};

enum class RequestState : uint8_t {
  Idle,
  Queued,
  AwaitingAck,
  RetryWait,
  Accepted,
  Committed,  // destructive host-power only; the fan protocol never enters it
  Applied,
  Failed,
  DeliveryFailed,
  CompletionTimeout,
};

struct RequestId {
  uint32_t bootNonce = 0;
  uint32_t counter = 0;
};

// Delivery bookkeeping every request record carries. Protocol-specific records
// derive from this so the shared helpers below can advance either one.
struct RequestDelivery {
  RequestId id;
  RequestState state = RequestState::Idle;
  uint8_t attempts = 0;
  bool pending = false;
  bool retryEnabled = false;
  uint32_t createdMs = 0;
  uint32_t nextDueMs = 0;
};

// One spinlock and one ID space for the whole module. Sharing the counter is
// deliberate: a fan ID can then never collide with a power ID on the wire, so
// a stale callback for one protocol cannot be mistaken for the other's.
static portMUX_TYPE sStateMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t sBootNonce = 0;
static uint32_t sLastCounter = 0;
// Registry-command replies are serialized by cmd_exec_task, so both protocols
// format into one buffer rather than each holding its own 640 bytes of PSRAM.
// UART intrinsics always pass separate loop-owned storage into their shared
// callback cores and never touch this buffer.
EXT_RAM_BSS_ATTR static char sReply[640];

bool requestIdEqual(const RequestId& a, const RequestId& b) {
  return a.bootNonce == b.bootNonce && a.counter == b.counter;
}

bool timeDue(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

void seedRequestIdSpace() {
  uint32_t nonce = esp_random();
  if (nonce == 0) nonce = esp_random();
  if (nonce == 0) nonce = 1;  // defensive only; esp_random() is hardware-backed

  portENTER_CRITICAL(&sStateMux);
  // Both protocols call this from their own init; the first one to run owns
  // the nonce so the shared counter stays monotonic across the module.
  if (sBootNonce == 0) {
    sBootNonce = nonce;
    sLastCounter = 0;
  }
  portEXIT_CRITICAL(&sStateMux);
}

// Caller must already hold sStateMux.
bool allocateRequestId(RequestId& out) {
  if (sBootNonce == 0 || sLastCounter == UINT32_MAX) return false;
  out.bootNonce = sBootNonce;
  out.counter = ++sLastCounter;
  return true;
}

// Caller must already hold sStateMux with the record confirmed to still be the
// request it was sampled from.
void finishDelivery(RequestDelivery& record, RequestState state) {
  record.state = state;
  record.pending = false;
  record.retryEnabled = false;
  record.nextDueMs = 0;
}

// Caller must already hold sStateMux with the record confirmed to still be the
// request that was just pushed.
void recordDeliveryAttempt(RequestDelivery& record, bool sent, uint32_t now) {
  const uint8_t attemptIndex = record.attempts;
  if (record.attempts < kMaxDeliveryAttempts) ++record.attempts;
  record.state = sent ? RequestState::AwaitingAck : RequestState::RetryWait;
  record.nextDueMs = now + kRetryBackoffMs[attemptIndex];
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

bool parseBoundedUnsigned(const String& token, uint32_t maxValue,
                          uint32_t& out) {
  if (token.length() == 0 || token.length() > 10) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < token.length(); ++i) {
    const char c = token[i];
    if (c < '0' || c > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(c - '0');
    if (value > (maxValue - digit) / 10U) return false;
    value = value * 10U + digit;
  }
  out = value;
  return true;
}

bool exactPlainArgs(const CommandArgs& args, int count) {
  if (args.unterminatedQuote() || args.count() != count) return false;
  for (int i = 0; i < count; ++i) {
    if (args.argWasQuoted(i)) return false;
  }
  return true;
}

// Match the UART frame/voicefetch trust boundary: the captured command context
// must be UART-originated and the live session must still be logged in.
//
// requireNamedSession additionally pins the caller to the live session's
// username. The fan protocol stamps each request with the session epoch it was
// queued on and rejects callbacks from any other session, so it verifies the
// name too. Host power relies on the nonzero active login epoch alone — that
// is the UART module's atomic revocation gate — and its callbacks are matched
// by request ID instead.
bool realAuthenticatedUartCaller(uint32_t* sessionEpochOut,
                                 bool requireNamedSession) {
  const AuthContext& ctx = currentAuthContext();
  if (ctx.transport != SOURCE_UART || ctx.user.length() == 0 ||
      ctx.user == "AuthBypass") {
    return false;
  }

  if (!requireNamedSession) {
    const uint32_t epoch = uartLinkSessionEpoch();
    if (epoch == 0) return false;
    if (sessionEpochOut) *sessionEpochOut = epoch;
    return true;
  }

  char liveUser[65] = {};
  uint32_t namedEpoch = 0;
  if (!uartLinkSessionSnapshot(liveUser, sizeof(liveUser), &namedEpoch) ||
      namedEpoch == 0 || ctx.user != liveUser) {
    return false;
  }
  if (sessionEpochOut) *sessionEpochOut = namedEpoch;
  return true;
}

#if ENABLE_RASPBERRY_PI_HOST_POWER

// ═══════════════════════════════════════════════════════════════════════════
// cm5 power — CM5 power state and CPU profile
// ═══════════════════════════════════════════════════════════════════════════

constexpr uint16_t kMinSleepMinutes = 1;
constexpr uint16_t kMaxSleepMinutes = 1440;

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

struct HostBootTag {
  uint8_t bytes[16] = {};
  bool valid = false;
};

struct HostPowerRequest : RequestDelivery {
  HostPowerAction action = HostPowerAction::None;
  uint16_t sleepMinutes = 0;
};

struct HostReport {
  bool valid = false;
  RequestId requestId;
  HostState state = HostState::Unknown;
  HostProfile profile = HostProfile::Unknown;
  HostBootTag bootTag;
  uint32_t updatedMs = 0;
};

static HostPowerRequest sRequest;
static HostReport sHostReport;
// Destructive requests stop delivery retries as soon as the CM5 accepts
// responsibility, but that must not open a window for a second transition to
// queue before the first one finishes or the host returns after reboot/wake.
static bool sTransitionInFlight = false;
// Linux /proc/sys/kernel/random/boot_id captured when a destructive request
// is accepted. A later ready report can then distinguish a daemon-only
// restart from an actual host boot without trusting elapsed time.
static HostBootTag sTransitionBootTag;

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

bool parseMinutes(const String& token, uint16_t& minutesOut) {
  // Keep the tight 4-digit shape: this feeds a superadmin destructive request,
  // so a zero-padded or otherwise unusual spelling is rejected rather than
  // normalized.
  if (token.length() > 4) return false;
  uint32_t value = 0;
  if (!parseBoundedUnsigned(token, kMaxSleepMinutes, value)) return false;
  if (value < kMinSleepMinutes) return false;
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

bool formatEvent(const HostPowerRequest& request, char* out, size_t outSize) {
  char id[17];
  if (!formatRequestId(request.id, id, sizeof(id))) return false;

  int n = -1;
  switch (request.action) {
    case HostPowerAction::Status:
      n = snprintf(out, outSize, "cm5_power_status %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::ProfileEco:
      n = snprintf(out, outSize, "cm5_power_profile_eco %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::ProfileBalanced:
      n = snprintf(out, outSize, "cm5_power_profile_balanced %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::ProfilePerformance:
      n = snprintf(out, outSize, "cm5_power_profile_performance %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::ProfileAuto:
      n = snprintf(out, outSize, "cm5_power_profile_auto %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::Reboot:
      n = snprintf(out, outSize, "cm5_power_reboot %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::Halt:
      n = snprintf(out, outSize, "cm5_power_halt %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::Suspend:
      n = snprintf(out, outSize, "cm5_power_suspend %s %s", kProtocolVersion, id);
      break;
    case HostPowerAction::SleepFor:
      n = snprintf(out, outSize, "cm5_power_sleep_for %s %s %u",
                   kProtocolVersion, id,
                   static_cast<unsigned>(request.sleepMinutes));
      break;
    default:
      return false;
  }
  return n > 0 && static_cast<size_t>(n) < outSize;
}

const char* showState() {
  HostPowerRequest request;
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

  HostPowerRequest queued;
  const uint32_t now = millis();
  portENTER_CRITICAL(&sStateMux);
  if (sRequest.pending || sTransitionInFlight) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: a host-power request or host transition is already in flight; use 'cm5 power show'";
  }
  if (actionIsDestructive(action) && !sHostReport.bootTag.valid) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: CM5 has not published a valid Linux boot ID; destructive host-power control is not ready";
  }
  if (!allocateRequestId(queued.id)) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: host-power request ID space is unavailable until reboot";
  }

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
           "OK: queued host-power %s id=%s; use 'cm5 power show' for delivery state",
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

#endif  // ENABLE_RASPBERRY_PI_HOST_POWER

#if ENABLE_RASPBERRY_PI_HOST_FAN

// ═══════════════════════════════════════════════════════════════════════════
// cm5 fan — CM5 fan policy and bounded fan telemetry
//
// Deliberately independent of host power: a fan change must not be blocked by
// a pending profile request, and a fan report must never resolve one.
// ═══════════════════════════════════════════════════════════════════════════

constexpr uint32_t kMaxTemperatureMilliC = 200000;
constexpr uint32_t kMaxFanRpm = 100000;

enum class FanAction : uint8_t {
  None,
  Status,
  ModeQuiet,
  ModeAuto,
  ModeMax,
};

enum class FanMode : uint8_t {
  Auto,
  Quiet,
  Max,
};

enum class FanHealth : uint8_t {
  Ok,
  Boosting,
  TachUnavailable,
  SafetyTemp,
  SafetyStall,
  Unavailable,
  IoError,
};

struct FanRequest : RequestDelivery {
  FanAction action = FanAction::None;
  // The fan protocol pins every callback to the exact session the request was
  // queued on, so a reconnect fails delivery instead of accepting a stale ACK.
  uint32_t sessionEpoch = 0;
};

struct FanReport {
  bool valid = false;
  RequestId requestId;
  FanMode requestedMode = FanMode::Auto;
  FanMode effectiveMode = FanMode::Auto;
  int32_t temperatureMilliC = -1;
  uint8_t targetPwm = 0;
  uint8_t pwm = 0;
  int32_t rpm = -1;
  FanHealth health = FanHealth::Unavailable;
  uint32_t updatedMs = 0;
};

static FanRequest sFanRequest;
static FanReport sFanReport;

const char* fanActionName(FanAction action) {
  switch (action) {
    case FanAction::Status: return "status";
    case FanAction::ModeQuiet: return "mode_quiet";
    case FanAction::ModeAuto: return "mode_auto";
    case FanAction::ModeMax: return "mode_max";
    default: return "none";
  }
}

const char* fanModeName(FanMode mode) {
  switch (mode) {
    case FanMode::Auto: return "auto";
    case FanMode::Quiet: return "quiet";
    case FanMode::Max: return "max";
  }
  return "auto";
}

const char* fanHealthName(FanHealth health) {
  switch (health) {
    case FanHealth::Ok: return "ok";
    case FanHealth::Boosting: return "boosting";
    case FanHealth::TachUnavailable: return "tach_unavailable";
    case FanHealth::SafetyTemp: return "safety_temp";
    case FanHealth::SafetyStall: return "safety_stall";
    case FanHealth::Unavailable: return "unavailable";
    case FanHealth::IoError: return "io_error";
  }
  return "unavailable";
}

bool fanActionMode(FanAction action, FanMode& out) {
  switch (action) {
    case FanAction::ModeQuiet:
      out = FanMode::Quiet;
      return true;
    case FanAction::ModeAuto:
      out = FanMode::Auto;
      return true;
    case FanAction::ModeMax:
      out = FanMode::Max;
      return true;
    default:
      return false;
  }
}

bool parseSentinelMetric(const String& token, uint32_t maxValue,
                         int32_t& out) {
  if (token == "-1") {
    out = -1;
    return true;
  }
  uint32_t value = 0;
  if (!parseBoundedUnsigned(token, maxValue, value)) return false;
  out = static_cast<int32_t>(value);
  return true;
}

bool parseFanMode(const String& token, FanMode& out) {
  if (token == "auto") out = FanMode::Auto;
  else if (token == "quiet") out = FanMode::Quiet;
  else if (token == "max") out = FanMode::Max;
  else return false;
  return true;
}

bool parseFanHealth(const String& token, FanHealth& out) {
  if (token == "ok") out = FanHealth::Ok;
  else if (token == "boosting") out = FanHealth::Boosting;
  else if (token == "tach_unavailable") out = FanHealth::TachUnavailable;
  else if (token == "safety_temp") out = FanHealth::SafetyTemp;
  else if (token == "safety_stall") out = FanHealth::SafetyStall;
  else if (token == "unavailable") out = FanHealth::Unavailable;
  else if (token == "io_error") out = FanHealth::IoError;
  else return false;
  return true;
}

bool formatFanEvent(const FanRequest& request, char* out, size_t outSize) {
  char id[17];
  if (!formatRequestId(request.id, id, sizeof(id))) return false;

  int n = -1;
  switch (request.action) {
    case FanAction::Status:
      n = snprintf(out, outSize, "cm5_fan_status %s %s", kProtocolVersion, id);
      break;
    case FanAction::ModeQuiet:
      n = snprintf(out, outSize, "cm5_fan_mode_quiet %s %s", kProtocolVersion, id);
      break;
    case FanAction::ModeAuto:
      n = snprintf(out, outSize, "cm5_fan_mode_auto %s %s", kProtocolVersion, id);
      break;
    case FanAction::ModeMax:
      n = snprintf(out, outSize, "cm5_fan_mode_max %s %s", kProtocolVersion, id);
      break;
    default:
      return false;
  }
  return n > 0 && static_cast<size_t>(n) < outSize;
}

const char* showFanState() {
  FanRequest request;
  FanReport report;
  uint32_t bootNonce;
  const uint32_t now = millis();
  portENTER_CRITICAL(&sStateMux);
  request = sFanRequest;
  report = sFanReport;
  bootNonce = sBootNonce;
  portEXIT_CRITICAL(&sStateMux);

  char requestId[17] = "none";
  if (request.action != FanAction::None) {
    (void)formatRequestId(request.id, requestId, sizeof(requestId));
  }
  char reportId[17] = "none";
  if (report.valid) {
    (void)formatRequestId(report.requestId, reportId, sizeof(reportId));
  }

  const uint32_t requestAge = request.action == FanAction::None
                                  ? 0
                                  : now - request.createdMs;
  const uint32_t dueIn = request.pending && !timeDue(now, request.nextDueMs)
                             ? request.nextDueMs - now
                             : 0;
  const uint32_t reportAge = report.valid ? now - report.updatedMs : 0;

  snprintf(
      sReply, sizeof(sReply),
      "Host fan: boot=%08lx id=%s action=%s state=%s pending=%s session=%lu "
      "attempts=%u/%u age_ms=%lu next_due_ms=%lu; "
      "CM5 report: valid=%s id=%s requested=%s effective=%s temp_mc=%ld "
      "target_pwm=%u pwm=%u rpm=%ld health=%s age_ms=%lu",
      static_cast<unsigned long>(bootNonce), requestId,
      fanActionName(request.action), requestStateName(request.state),
      request.pending ? "yes" : "no",
      static_cast<unsigned long>(request.sessionEpoch),
      static_cast<unsigned>(request.attempts),
      static_cast<unsigned>(kMaxDeliveryAttempts),
      static_cast<unsigned long>(requestAge),
      static_cast<unsigned long>(dueIn), report.valid ? "yes" : "no",
      reportId, report.valid ? fanModeName(report.requestedMode) : "unknown",
      report.valid ? fanModeName(report.effectiveMode) : "unknown",
      static_cast<long>(report.temperatureMilliC),
      static_cast<unsigned>(report.targetPwm),
      static_cast<unsigned>(report.pwm), static_cast<long>(report.rpm),
      report.valid ? fanHealthName(report.health) : "unknown",
      static_cast<unsigned long>(reportAge));
  return sReply;
}

const char* queueFanRequest(FanAction action) {
  const uint32_t sessionEpoch = uartLinkSessionEpoch();
  if (!uartLinkIsRunning() || sessionEpoch == 0) {
    return "Error: CM5 UART link is not running with an authenticated session";
  }

  FanRequest queued;
  FanRequest superseded;
  bool didSupersede = false;
  const uint32_t now = millis();
  portENTER_CRITICAL(&sStateMux);
  if (sFanRequest.pending) {
    if (action != FanAction::ModeMax ||
        sFanRequest.action == FanAction::ModeMax) {
      portEXIT_CRITICAL(&sStateMux);
      return "Error: a host-fan request is already in flight; use 'cm5 fan show' (max may supersede a pending non-max request)";
    }
    superseded = sFanRequest;
    didSupersede = true;
  }
  if (!allocateRequestId(queued.id)) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: host-fan request ID space is unavailable until reboot";
  }

  queued.action = action;
  queued.state = RequestState::Queued;
  queued.sessionEpoch = sessionEpoch;
  queued.attempts = 0;
  queued.pending = true;
  queued.retryEnabled = true;
  queued.createdMs = now;
  queued.nextDueMs = now;
  sFanRequest = queued;
  portEXIT_CRITICAL(&sStateMux);

  char id[17];
  (void)formatRequestId(queued.id, id, sizeof(id));
  if (didSupersede) {
    char oldId[17];
    (void)formatRequestId(superseded.id, oldId, sizeof(oldId));
    snprintf(sReply, sizeof(sReply),
             "OK: queued host-fan %s id=%s; superseded pending %s id=%s; "
             "use 'cm5 fan show' for delivery state",
             fanActionName(action), id, fanActionName(superseded.action),
             oldId);
  } else {
    snprintf(sReply, sizeof(sReply),
             "OK: queued host-fan %s id=%s; use 'cm5 fan show' for delivery state",
             fanActionName(action), id);
  }
  return sReply;
}

#endif  // ENABLE_RASPBERRY_PI_HOST_FAN

}  // namespace

#if ENABLE_RASPBERRY_PI_HOST_POWER

// Command handlers have external linkage on purpose: the rows that bind
// them live in cm5PresenceCommands[] so the whole device shares one `cm5`
// namespace and one `help cm5` page.
const char* cmdCm5Power(const String& argsInput) {
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
  return "Usage: cm5 power [show|status|profile <eco|balanced|performance|auto>]";
}

const char* cmdCm5PowerReboot(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = requireLiteralConfirm(argsInput)) return error;
  return queueRequest(HostPowerAction::Reboot);
}

const char* cmdCm5PowerHalt(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = requireLiteralConfirm(argsInput)) return error;
  return queueRequest(HostPowerAction::Halt);
}

const char* cmdCm5PowerSuspend(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = requireLiteralConfirm(argsInput)) return error;
  return queueRequest(HostPowerAction::Suspend);
}

const char* cmdCm5PowerSleepFor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs args(argsInput);
  uint16_t minutes = 0;
  if (args.count() != 2 || !parseMinutes(args.arg(0), minutes) ||
      !args.arg(1).equalsIgnoreCase("confirm")) {
    return "Error: use cm5 power sleep_for <1..1440 minutes> confirm";
  }
  return queueRequest(HostPowerAction::SleepFor, minutes);
}

const char* cmdCm5PowerRecover(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = requireLiteralConfirm(argsInput)) return error;

  portENTER_CRITICAL(&sStateMux);
  if (!sTransitionInFlight || !actionIsDestructive(sRequest.action)) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: no uncertain host-power transition requires recovery";
  }
  finishDelivery(sRequest, RequestState::Failed);
  sTransitionInFlight = false;
  sTransitionBootTag = HostBootTag{};
  portEXIT_CRITICAL(&sStateMux);
  return "OK: cleared the uncertain host-power transition; verify CM5 state before issuing another destructive request";
}

static const char* handleCm5PowerAck(const String& argsInput,
                                     uint32_t callerSessionEpoch,
                                     char* reply, size_t replySize) {
  if (callerSessionEpoch == 0) {
    return "Error: host-power ACK is accepted only from the authenticated UART session";
  }

  CommandArgs args(argsInput);
  RequestId id;
  if (!exactPlainArgs(args, 3)) {
    return "Usage: cm5 power ack 1 <16-hex-id> <accepted|committed|applied|failed>";
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
  snprintf(reply, replySize, "OK: host-power ACK id=%s state=%s%s",
           idText, requestStateName(requestedState),
           duplicate ? " duplicate" : "");
  return reply;
}

const char* cmdCm5PowerAck(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  uint32_t callerSessionEpoch = 0;
  if (!realAuthenticatedUartCaller(&callerSessionEpoch,
                                   /*requireNamedSession=*/false)) {
    return "Error: host-power ACK is accepted only from the authenticated UART session";
  }
  return handleCm5PowerAck(argsInput, callerSessionEpoch,
                           sReply, sizeof(sReply));
}

static const char* handleCm5PowerReport(const String& argsInput,
                                        uint32_t callerSessionEpoch,
                                        char* reply, size_t replySize) {
  if (callerSessionEpoch == 0) {
    return "Error: CM5 status is accepted only from the authenticated UART session";
  }

  CommandArgs args(argsInput);
  RequestId id;
  HostState state;
  HostProfile profile;
  HostBootTag bootTag;
  if (!exactPlainArgs(args, 5)) {
    return "Usage: cm5 power report 1 <16-hex-id|0> <state> <profile> <32-hex-linux-boot-id>";
  }
  if (args.arg(0) != kProtocolVersion) {
    return "Error: unsupported host-power protocol version (expected 1)";
  }
  if (!parseRequestId(args.arg(1), id, true) ||
      !parseHostState(args.arg(2), state) ||
      !parseHostProfile(args.arg(3), profile) ||
      !parseHostBootTag(args.arg(4), bootTag)) {
    return "Usage: cm5 power report 1 <16-hex-id|0> <unknown|awake|sleeping|suspending|rebooting|halting|error> <unknown|eco|balanced|performance|auto> <32-hex-linux-boot-id>";
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
      finishDelivery(sRequest, RequestState::Failed);
      sTransitionInFlight = false;
      sTransitionBootTag = HostBootTag{};
    } else if (changedBoot &&
               (sRequest.state == RequestState::Committed ||
                sRequest.state == RequestState::Applied)) {
      // A real Linux boot boundary completed the requested transition even if
      // the no-block helper's final Applied reply was lost during shutdown.
      finishDelivery(sRequest, RequestState::Applied);
      sTransitionInFlight = false;
      sTransitionBootTag = HostBootTag{};
    } else if (sameBoot && sRequest.state == RequestState::Accepted) {
      // A new controller process is ready on the same Linux boot. Because the
      // CM5 never crossed the confirmed Committed boundary, the helper could
      // not legally have run; fail and reopen safely without re-execution.
      finishDelivery(sRequest, RequestState::Failed);
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
    finishDelivery(sRequest, RequestState::Failed);
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
      finishDelivery(sRequest, RequestState::Applied);
    }
  }
  portEXIT_CRITICAL(&sStateMux);

  char idText[17] = "0";
  if (id.counter != 0) (void)formatRequestId(id, idText, sizeof(idText));
  char bootText[33];
  formatHostBootTag(bootTag, bootText, sizeof(bootText));
  snprintf(reply, replySize,
           "OK: CM5 report id=%s state=%s profile=%s host_boot=%s", idText,
           hostStateName(state), profileName(profile),
           bootTag.valid ? bootText : "unknown");
  return reply;
}

const char* cmdCm5PowerReport(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  uint32_t callerSessionEpoch = 0;
  if (!realAuthenticatedUartCaller(&callerSessionEpoch,
                                   /*requireNamedSession=*/false)) {
    return "Error: CM5 status is accepted only from the authenticated UART session";
  }
  return handleCm5PowerReport(argsInput, callerSessionEpoch,
                              sReply, sizeof(sReply));
}

void cm5HostPowerInit() {
  seedRequestIdSpace();

  portENTER_CRITICAL(&sStateMux);
  sRequest = HostPowerRequest{};
  sHostReport = HostReport{};
  sTransitionInFlight = false;
  sTransitionBootTag = HostBootTag{};
  portEXIT_CRITICAL(&sStateMux);
}

void cm5HostPowerTick() {
  const uint32_t now = millis();
  HostPowerRequest request;

  portENTER_CRITICAL(&sStateMux);
  request = sRequest;
  if (request.pending && !request.retryEnabled &&
      request.state == RequestState::Accepted &&
      timeDue(now, request.nextDueMs)) {
    finishDelivery(sRequest, RequestState::CompletionTimeout);
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
      finishDelivery(sRequest, RequestState::DeliveryFailed);
    }
    portEXIT_CRITICAL(&sStateMux);
    return;
  }

  char event[80];
  if (!formatEvent(request, event, sizeof(event))) return;
  const bool sent = uartLinkTryPushEvent(event);
  DEBUG_UART_CONTROLF("[UART-CTRL] TX EVT %s -> %s", event,
                      sent ? "sent" : "deferred");

  portENTER_CRITICAL(&sStateMux);
  if (sRequest.pending && sRequest.retryEnabled &&
      requestIdEqual(sRequest.id, request.id)) {
    recordDeliveryAttempt(sRequest, sent, now);
  }
  portEXIT_CRITICAL(&sStateMux);
}

#endif  // ENABLE_RASPBERRY_PI_HOST_POWER

#if ENABLE_RASPBERRY_PI_HOST_FAN

// Command handlers have external linkage on purpose: the rows that bind
// them live in cm5PresenceCommands[] so the whole device shares one `cm5`
// namespace and one `help cm5` page.
const char* cmdCm5Fan(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs args(argsInput);
  if (args.unterminatedQuote()) {
    return "Usage: cm5 fan [show|status|quiet|auto|max]";
  }
  if (args.count() == 0 ||
      (args.count() == 1 && args.arg(0).equalsIgnoreCase("show"))) {
    return showFanState();
  }
  if (args.count() == 1 && args.arg(0).equalsIgnoreCase("status")) {
    return queueFanRequest(FanAction::Status);
  }
  if (args.count() == 1 && args.arg(0).equalsIgnoreCase("quiet")) {
    return queueFanRequest(FanAction::ModeQuiet);
  }
  if (args.count() == 1 && args.arg(0).equalsIgnoreCase("auto")) {
    return queueFanRequest(FanAction::ModeAuto);
  }
  if (args.count() == 1 && args.arg(0).equalsIgnoreCase("max")) {
    return queueFanRequest(FanAction::ModeMax);
  }
  return "Usage: cm5 fan [show|status|quiet|auto|max]";
}

static const char* handleCm5FanAck(const String& argsInput,
                                   uint32_t callerSessionEpoch,
                                   char* reply, size_t replySize) {
  if (callerSessionEpoch == 0) {
    return "Error: host-fan ACK is accepted only from the authenticated UART session";
  }

  CommandArgs args(argsInput);
  RequestId id;
  if (!exactPlainArgs(args, 3)) {
    return "Usage: cm5 fan ack 1 <16-hex-id> <accepted|applied|failed>";
  }
  if (args.arg(0) != kProtocolVersion) {
    return "Error: unsupported host-fan protocol version (expected 1)";
  }
  if (!parseRequestId(args.arg(1), id, false)) {
    return "Error: host-fan request ID must be exactly 16 hexadecimal characters";
  }

  RequestState requestedState;
  if (args.arg(2) == "accepted") requestedState = RequestState::Accepted;
  else if (args.arg(2) == "applied") requestedState = RequestState::Applied;
  else if (args.arg(2) == "failed") requestedState = RequestState::Failed;
  else return "Error: ACK state must be accepted, applied, or failed";

  const uint32_t now = millis();
  bool duplicate = false;
  bool conflict = false;
  portENTER_CRITICAL(&sStateMux);
  if (sFanRequest.action == FanAction::None ||
      !requestIdEqual(sFanRequest.id, id)) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: ACK request ID does not match the current host-fan request";
  }
  if (sFanRequest.sessionEpoch != callerSessionEpoch) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: ACK belongs to a different authenticated UART session";
  }

  if (sFanRequest.state == requestedState) {
    duplicate = true;
  } else if (sFanRequest.state == RequestState::Applied ||
             sFanRequest.state == RequestState::Failed ||
             sFanRequest.state == RequestState::DeliveryFailed ||
             sFanRequest.state == RequestState::CompletionTimeout) {
    conflict = true;
  } else {
    const bool validTransition =
        requestedState == RequestState::Failed ||
        (requestedState == RequestState::Accepted &&
         (sFanRequest.state == RequestState::Queued ||
          sFanRequest.state == RequestState::AwaitingAck ||
          sFanRequest.state == RequestState::RetryWait)) ||
        (requestedState == RequestState::Applied &&
         sFanRequest.state == RequestState::Accepted);
    if (!validTransition) {
      conflict = true;
    } else {
      sFanRequest.state = requestedState;
      sFanRequest.retryEnabled = false;
      if (requestedState == RequestState::Accepted) {
        sFanRequest.pending = true;
        sFanRequest.nextDueMs = now + kCompletionTimeoutMs;
      } else {
        sFanRequest.pending = false;
        sFanRequest.nextDueMs = 0;
      }
    }
  }
  portEXIT_CRITICAL(&sStateMux);

  if (conflict) return "Error: ACK conflicts with the current host-fan request state";
  char idText[17];
  (void)formatRequestId(id, idText, sizeof(idText));
  snprintf(reply, replySize, "OK: host-fan ACK id=%s state=%s%s",
           idText, requestStateName(requestedState),
           duplicate ? " duplicate" : "");
  return reply;
}

const char* cmdCm5FanAck(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  uint32_t callerSessionEpoch = 0;
  if (!realAuthenticatedUartCaller(&callerSessionEpoch,
                                   /*requireNamedSession=*/true)) {
    return "Error: host-fan ACK is accepted only from the authenticated UART session";
  }
  return handleCm5FanAck(argsInput, callerSessionEpoch,
                         sReply, sizeof(sReply));
}

static const char* handleCm5FanReport(const String& argsInput,
                                      uint32_t callerSessionEpoch,
                                      char* reply, size_t replySize) {
  if (callerSessionEpoch == 0) {
    return "Error: host-fan report is accepted only from the authenticated UART session";
  }

  CommandArgs args(argsInput);
  RequestId id;
  FanMode requestedMode;
  FanMode effectiveMode;
  int32_t temperatureMilliC = -1;
  uint32_t targetPwm = 0;
  uint32_t pwm = 0;
  int32_t rpm = -1;
  FanHealth health;
  if (!exactPlainArgs(args, 9) || args.arg(0) != kProtocolVersion ||
      !parseRequestId(args.arg(1), id, false) ||
      !parseFanMode(args.arg(2), requestedMode) ||
      !parseFanMode(args.arg(3), effectiveMode) ||
      !parseSentinelMetric(args.arg(4), kMaxTemperatureMilliC,
                           temperatureMilliC) ||
      !parseBoundedUnsigned(args.arg(5), 255, targetPwm) ||
      !parseBoundedUnsigned(args.arg(6), 255, pwm) ||
      !parseSentinelMetric(args.arg(7), kMaxFanRpm, rpm) ||
      !parseFanHealth(args.arg(8), health)) {
    return "Usage: cm5 fan report 1 <16-hex-id> <auto|quiet|max> <auto|quiet|max> <-1|0..200000-temp-mc> <0..255-target-pwm> <0..255-pwm> <-1|0..100000-rpm> <ok|boosting|tach_unavailable|safety_temp|safety_stall|unavailable|io_error>";
  }

  const uint32_t now = millis();
  portENTER_CRITICAL(&sStateMux);
  if (sFanRequest.action == FanAction::None ||
      !requestIdEqual(sFanRequest.id, id)) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: report request ID does not match the current host-fan request";
  }
  if (sFanRequest.sessionEpoch != callerSessionEpoch) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: report belongs to a different authenticated UART session";
  }
  FanMode expectedMode;
  if (fanActionMode(sFanRequest.action, expectedMode) &&
      expectedMode != requestedMode) {
    portEXIT_CRITICAL(&sStateMux);
    return "Error: reported requested mode does not match the current host-fan request";
  }

  sFanReport.valid = true;
  sFanReport.requestId = id;
  sFanReport.requestedMode = requestedMode;
  sFanReport.effectiveMode = effectiveMode;
  sFanReport.temperatureMilliC = temperatureMilliC;
  sFanReport.targetPwm = static_cast<uint8_t>(targetPwm);
  sFanReport.pwm = static_cast<uint8_t>(pwm);
  sFanReport.rpm = rpm;
  sFanReport.health = health;
  sFanReport.updatedMs = now;

  if (sFanRequest.pending && sFanRequest.state == RequestState::Accepted) {
    finishDelivery(sFanRequest, RequestState::Applied);
  }
  portEXIT_CRITICAL(&sStateMux);

  char idText[17];
  (void)formatRequestId(id, idText, sizeof(idText));
  snprintf(reply, replySize,
           "OK: host-fan report id=%s requested=%s effective=%s temp_mc=%ld "
           "target_pwm=%lu pwm=%lu rpm=%ld health=%s",
           idText, fanModeName(requestedMode), fanModeName(effectiveMode),
           static_cast<long>(temperatureMilliC),
           static_cast<unsigned long>(targetPwm),
           static_cast<unsigned long>(pwm), static_cast<long>(rpm),
           fanHealthName(health));
  return reply;
}

const char* cmdCm5FanReport(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  uint32_t callerSessionEpoch = 0;
  if (!realAuthenticatedUartCaller(&callerSessionEpoch,
                                   /*requireNamedSession=*/true)) {
    return "Error: host-fan report is accepted only from the authenticated UART session";
  }
  return handleCm5FanReport(argsInput, callerSessionEpoch,
                            sReply, sizeof(sReply));
}

void cm5HostFanInit() {
  seedRequestIdSpace();

  portENTER_CRITICAL(&sStateMux);
  sFanRequest = FanRequest{};
  sFanReport = FanReport{};
  portEXIT_CRITICAL(&sStateMux);
}

void cm5HostFanTick() {
  const uint32_t now = millis();
  FanRequest request;

  portENTER_CRITICAL(&sStateMux);
  request = sFanRequest;
  if (request.pending && !request.retryEnabled &&
      request.state == RequestState::Accepted &&
      timeDue(now, request.nextDueMs)) {
    finishDelivery(sFanRequest, RequestState::CompletionTimeout);
    portEXIT_CRITICAL(&sStateMux);
    return;
  }
  portEXIT_CRITICAL(&sStateMux);

  // A reconnect invalidates the session this request was pinned to. Fail it
  // rather than let a later session's ACK land on a stale record.
  if (request.pending &&
      (request.sessionEpoch == 0 ||
       uartLinkSessionEpoch() != request.sessionEpoch)) {
    portENTER_CRITICAL(&sStateMux);
    if (sFanRequest.pending && requestIdEqual(sFanRequest.id, request.id)) {
      finishDelivery(sFanRequest, RequestState::DeliveryFailed);
    }
    portEXIT_CRITICAL(&sStateMux);
    return;
  }

  if (!request.pending || !request.retryEnabled ||
      !timeDue(now, request.nextDueMs)) {
    return;
  }

  if (request.attempts >= kMaxDeliveryAttempts) {
    portENTER_CRITICAL(&sStateMux);
    if (sFanRequest.pending && sFanRequest.retryEnabled &&
        requestIdEqual(sFanRequest.id, request.id)) {
      finishDelivery(sFanRequest, RequestState::DeliveryFailed);
    }
    portEXIT_CRITICAL(&sStateMux);
    return;
  }

  char event[64];
  if (!formatFanEvent(request, event, sizeof(event))) {
    portENTER_CRITICAL(&sStateMux);
    if (sFanRequest.pending && requestIdEqual(sFanRequest.id, request.id)) {
      finishDelivery(sFanRequest, RequestState::DeliveryFailed);
    }
    portEXIT_CRITICAL(&sStateMux);
    return;
  }
  const bool sent =
      uartLinkTryPushEventForSession(request.sessionEpoch, event);
  DEBUG_UART_CONTROLF("[UART-CTRL] TX EVT %s -> %s", event,
                      sent ? "sent" : "deferred");

  portENTER_CRITICAL(&sStateMux);
  if (sFanRequest.pending && sFanRequest.retryEnabled &&
      requestIdEqual(sFanRequest.id, request.id)) {
    recordDeliveryAttempt(sFanRequest, sent, now);
  }
  portEXIT_CRITICAL(&sStateMux);
}

#endif  // ENABLE_RASPBERRY_PI_HOST_FAN

Cm5HostCallbackIntrinsicResult cm5HostControlHandleCallbackIntrinsic(
    const char* line, uint32_t namedSessionEpoch, bool sessionMayControl,
    char* reply, size_t replySize) {
  if (line == nullptr || reply == nullptr || replySize == 0) {
    return Cm5HostCallbackIntrinsicResult::NotCallback;
  }
  // uartProcessLine already trims the line. Avoid constructing any Arduino
  // Strings for the overwhelmingly common non-CM5 command; a leading quote
  // stays eligible so malformed/quoted callback prefixes are still consumed
  // rather than leaking into cmd_exec.
  if (line[0] != 'c' && line[0] != 'C' && line[0] != '"') {
    return Cm5HostCallbackIntrinsicResult::NotCallback;
  }

  // Inspect only the fixed three-token machine prefix. The suffix is handed
  // unchanged to the same parser/state-transition core used by the registry
  // wrappers, so valid wire grammar and replies remain byte-for-byte identical.
  const String fullLine(line);
  CommandArgs args(fullLine);
  if (args.count() < 3 || !args.arg(0).equalsIgnoreCase("cm5")) {
    return Cm5HostCallbackIntrinsicResult::NotCallback;
  }

  enum class Callback : uint8_t {
    None,
    PowerAck,
    PowerReport,
    FanAck,
    FanReport,
  } callback = Callback::None;

#if ENABLE_RASPBERRY_PI_HOST_POWER
  if (args.arg(1).equalsIgnoreCase("power")) {
    if (args.arg(2).equalsIgnoreCase("ack")) callback = Callback::PowerAck;
    else if (args.arg(2).equalsIgnoreCase("report")) callback = Callback::PowerReport;
  }
#endif
#if ENABLE_RASPBERRY_PI_HOST_FAN
  if (args.arg(1).equalsIgnoreCase("fan")) {
    if (args.arg(2).equalsIgnoreCase("ack")) callback = Callback::FanAck;
    else if (args.arg(2).equalsIgnoreCase("report")) callback = Callback::FanReport;
  }
#endif
  if (callback == Callback::None) {
    return Cm5HostCallbackIntrinsicResult::NotCallback;
  }

  const char* result = nullptr;
  if (args.argWasQuoted(0) || args.argWasQuoted(1) ||
      args.argWasQuoted(2)) {
    result = "Error: CM5 callback prefix must be unquoted";
  } else if (namedSessionEpoch == 0) {
    switch (callback) {
      case Callback::PowerAck:
        result = "Error: host-power ACK is accepted only from the authenticated UART session";
        break;
      case Callback::PowerReport:
        result = "Error: CM5 status is accepted only from the authenticated UART session";
        break;
      case Callback::FanAck:
        result = "Error: host-fan ACK is accepted only from the authenticated UART session";
        break;
      case Callback::FanReport:
        result = "Error: host-fan report is accepted only from the authenticated UART session";
        break;
      default:
        break;
    }
  } else if (!sessionMayControl) {
    result = "Error: Guest accounts are view-only. Only local login/logout and whoami are allowed.";
  } else {
    const String callbackArgs = args.remaining(2);
    switch (callback) {
#if ENABLE_RASPBERRY_PI_HOST_POWER
      case Callback::PowerAck:
        result = handleCm5PowerAck(callbackArgs, namedSessionEpoch,
                                   reply, replySize);
        break;
      case Callback::PowerReport:
        result = handleCm5PowerReport(callbackArgs, namedSessionEpoch,
                                      reply, replySize);
        break;
#endif
#if ENABLE_RASPBERRY_PI_HOST_FAN
      case Callback::FanAck:
        result = handleCm5FanAck(callbackArgs, namedSessionEpoch,
                                 reply, replySize);
        break;
      case Callback::FanReport:
        result = handleCm5FanReport(callbackArgs, namedSessionEpoch,
                                    reply, replySize);
        break;
#endif
      default:
        break;
    }
  }

  if (result == nullptr) result = "Error: CM5 callback unavailable";
  if (result != reply) {
    // The command executor historically stamps bare `Usage:` handler returns
    // into a terminal status line. Direct replies have no such outer layer;
    // normalize them here or the host's status collector would wait forever.
    if (strncmp(result, "OK", 2) != 0 &&
        strncmp(result, "Error", 5) != 0 &&
        strncmp(result, "ERROR", 5) != 0) {
      snprintf(reply, replySize, "Error: %s", result);
    } else {
      strlcpy(reply, result, replySize);
    }
  }
  return Cm5HostCallbackIntrinsicResult::Handled;
}

#endif  // ENABLE_RASPBERRY_PI_HOST_POWER || ENABLE_RASPBERRY_PI_HOST_FAN
