#ifndef SYSTEM_CM5_PRESENCE_H
#define SYSTEM_CM5_PRESENCE_H

#include <Arduino.h>

struct CommandEntry;

// Small, authenticated application-level lease for the CM5 AI service.  This
// is deliberately not a UART owner: command execution, live audio, recording,
// and host-power keep their existing scheduling/ownership boundaries.
enum class Cm5PresenceMode : uint8_t {
  Unknown,
  Starting,
  Ready,
  Busy,
  Degraded,
};

// Bound for the OPTIONAL, OPAQUE reason token the daemon may append to a
// heartbeat, describing what it is busy with or why it is degraded. Retained
// for those two states only; see recordHeartbeat. Display-only: it never
// touches lease duration, freshness, session fencing, cmd_grace, or any LLM
// guard, so a malformed or hostile token cannot wedge anything. Advertised as
// reason_max / reason_states in `cm5 capabilities`; the host truncates to it.
//
// 96 rather than the 64 the host proposed. The host's stated worst case is two
// concurrent holders with a long GGUF basename
// ("llm:LFM2-8B-A1B-UD-Q3_K_XL,stt:moonshine" = 40), but three holders with a
// longer model name goes past 64 and clips the LAST holder — which is exactly
// the "and something else is also running" fact the token exists to convey.
// Costs 97 bytes of DRAM and leaves `cm5 status` at ~311 of its 384-byte reply
// buffer. Raising this later needs a firmware flash; the margin does not.
constexpr size_t CM5_PRESENCE_REASON_MAX = 96;

constexpr uint32_t CM5_PRESENCE_NORMAL_LEASE_MS = 15000;
constexpr uint32_t CM5_PRESENCE_BUSY_LEASE_MS = 75000;
constexpr uint32_t CM5_PRESENCE_COMMAND_GRACE_MS = 5000;

constexpr uint32_t cm5PresenceLeaseMs(Cm5PresenceMode mode) {
  return mode == Cm5PresenceMode::Busy ? CM5_PRESENCE_BUSY_LEASE_MS
                                       : CM5_PRESENCE_NORMAL_LEASE_MS;
}

// Wrap-safe lease predicate.  `seen` and an exact UART login-epoch match are
// separate gates in the snapshot API below.
constexpr bool cm5PresenceFreshAt(bool seen, uint32_t nowMs,
                                  uint32_t lastSeenMs, uint32_t leaseMs) {
  return seen && leaseMs != 0 &&
         static_cast<uint32_t>(nowMs - lastSeenMs) < leaseMs;
}

struct Cm5PresenceSnapshot {
  uint32_t sessionEpoch;
  uint32_t sequence;
  uint32_t lastSeenMs;
  uint32_t ageMs;
  uint32_t leaseMs;
  uint32_t commandAgeMs;
  uint32_t freshRemainingMs;
  uint32_t staleTransitions;
  uint32_t lastTransitionMs;
  uint32_t stackFreeMinBytes;
  Cm5PresenceMode mode;
  bool taskRunning;
  bool seenForSession;
  bool fresh;
  bool monitorFresh;
  bool commandInFlight;
  bool commandGrace;
};

enum class Cm5HeartbeatIntrinsicResult : uint8_t {
  NotHeartbeat,
  Handled,
};

// ---------------------------------------------------------------------------
// CM5 time anchor (Phase 1, caller-side authority).
//
// The CM5 carrier owns a battery-backed hardware RTC and (when online) NTP, so
// on this build — where the DS3231 is compiled out (ENABLE_RTC_SENSOR=0) and
// NTP needs WiFi — it is the only accurate wall-clock source on a dark boot.
// The daemon periodically pushes `cm5 time set 1 <epoch_sec_utc> <flags>` on
// the authenticated UART link; the intrinsic below validates + stashes it, and
// cm5TimeSyncTick() applies it on the main loop through the Clock::clockStepped
// chokepoint as SYNC_CM5 (never on the RX stack). Time flows to the G2/R1 via
// the existing corrective-push machinery with no changes there.
//
// Flags byte (Pi-computed confidence; only bit0/bit1 defined in Phase 1):
//   bit0 pi_clock_synced — the Pi's own clock is NTP-disciplined right now.
//   bit1 pi_rtc_valid    — the Pi read a plausible time from its battery RTC.
// Adopting a dark clock requires (bit0 || bit1) so a Pi with no basis for its
// time (dead battery + no NTP) cannot seed a plausible-but-wrong epoch that
// gets written into dated files. Correcting an already-valid clock requires
// bit0 (a battery-RTC-only Pi may seed, but not override a running clock).
constexpr uint8_t CM5_TIME_FLAG_PI_SYNCED    = 1u << 0;
constexpr uint8_t CM5_TIME_FLAG_PI_RTC_VALID = 1u << 1;

enum class Cm5TimeIntrinsicResult : uint8_t {
  NotTime,
  Handled,
};

// Recognize + validate the authenticated `cm5 time set 1 <epoch> <flags>`
// machine push. Auth-gated identically to the heartbeat (nonzero named session
// + non-Guest role); the epoch is bounded to [2020,2100) here. On success the
// value is STASHED (not applied) and ACKed "action=stashed" — the actual step
// happens one main-loop lap later in cm5TimeSyncTick(), so the ACK never
// implies the clock was set (it may still yield to a higher source or age out).
Cm5TimeIntrinsicResult cm5TimeHandleTimeIntrinsic(
    const char* line, uint32_t activeSessionEpoch,
    bool sessionMayPublishPresence,
    char* reply, size_t replySize);

// Main-loop policy engine: consume the latest stashed CM5 stamp and, gated on
// confidence + precedence + drift, step the system clock via SYNC_CM5. Cheap
// no-op when nothing is stashed. Main-loop task only (calls settimeofday).
void cm5TimeSyncTick();

// ---------------------------------------------------------------------------
// CM5 link health (host-reported diagnostics).
//
// The link's TEXT channel carries no integrity check — only the P2 binary
// frames get a CRC — so damage on it never announces itself. It arrives as a
// strange reply, and the daemon's actors recover from it quietly. Those
// recoveries are tallied on the HOST (ai-service link/health.py) and pushed
// here, so the numbers are readable from this device's own CLI instead of only
// from a journal on a Pi that may have no network at all. That was the whole
// problem on 2026-08-25: the counters that would have named the fault existed,
// and were unreachable from the bench.
//
// DIAGNOSTICS ONLY, and structurally so: these values are stored, printed, and
// read by nothing else. No lease, guard, freshness test or LLM decision looks
// at them, which is what makes a damaged or hostile push harmless — the worst
// it can do is print a wrong number.
//
// Wire form (host -> device), parsed BY KEY so a counter added on the host
// reaches an existing build without a firmware flash:
//
//   cm5 linkhealth 1 garbage=<u32> corrupt=<u32> timeouts=<u32> strays=<u32>
//                    logins=<u32> resets=<u32> tx=<u32> rx=<u32> up=<u32>
//
// A malformed token rejects the WHOLE report rather than storing a partial
// one. Damage is the thing these counters measure; a damaged report must not
// be displayed as truth.
struct Cm5LinkHealthSnapshot {
  bool     seen;          // a report has landed since boot
  uint32_t ageMs;         // since the last accepted report
  uint32_t sessionEpoch;  // UART login epoch it arrived under
  uint32_t reports;       // reports accepted since boot
  uint32_t garbage;       // host-side framing/COBS incidents
  uint32_t corrupt;       // replies that echoed a verb the host never wrote
  uint32_t timeouts;      // host commands that got no reply in time
  uint32_t strays;        // unmatched reply lines the host dropped
  uint32_t logins;        // successful UART logins the host performed
  uint32_t resets;        // reconnects the host observed
  uint32_t tx;            // command lines the host wrote
  uint32_t rx;            // text lines the host read
  uint32_t uptimeS;       // daemon uptime in seconds
  uint32_t unknownKeys;   // keys this build did not recognize (newer host)
};

Cm5LinkHealthSnapshot cm5LinkHealthSnapshot();

enum class Cm5LinkHealthIntrinsicResult : uint8_t {
  NotLinkHealth,
  Handled,
};

// Recognize + validate the authenticated `cm5 linkhealth 1 <k=v> ...` push.
// Auth-gated identically to the heartbeat and the clock push (nonzero named
// session + non-Guest role), and consumed on the control plane so a periodic
// diagnostic never takes the command lock or writes a durable audit line.
Cm5LinkHealthIntrinsicResult cm5LinkHealthHandleIntrinsic(
    const char* line, uint32_t activeSessionEpoch,
    bool sessionMayPublishPresence,
    char* reply, size_t replySize);

// Handle only the authenticated, allocation-free UART control-plane renewal:
//   cm5 heartbeat 1 <nonzero-u32-sequence> <starting|ready|busy|degraded>
// Status and capabilities are ordinary registry commands below. `reply`
// receives one complete text reply when Handled is returned.
Cm5HeartbeatIntrinsicResult cm5PresenceHandleHeartbeatIntrinsic(
    const char* line, uint32_t activeSessionEpoch,
    bool sessionMayPublishPresence,
    char* reply, size_t replySize);

// True for the CM5 machine-housekeeping verbs only — bare `cm5`, `cm5 status`,
// `cm5 capabilities`, `cm5 heartbeat`, and `cm5 time` (the periodic clock
// push). UART uses this to keep the daemon's own polling from extending its
// command grace or evicting human history from the command feed. `cm5 power
// ...` and `cm5 fan ...` are real operations and are deliberately NOT covered:
// they stay auditable. `cm5 time` is a mutation, but its provenance lives in
// the Clock ledger / SYSEVT_TIME_SYNCED, not the command feed.
bool cm5PresenceIsProtocolCommand(const char* line);

// Setup-agnostic inspection through the central command registry. These are
// intentionally available even when no G2 or physical UART link is compiled.
extern const CommandEntry cm5PresenceCommands[];
extern const size_t cm5PresenceCommandsCount;

// Wake the monitor after a login-epoch transition.  Exact epoch comparison in
// snapshots remains authoritative, so delayed notifications cannot revive an
// old session.
void cm5PresenceNotifySessionChanged();

// Ordinary UART commands can occupy the synchronous executor for up to 60 s.
// Extend an already-fresh lease while that exact session's command is in
// flight, followed by a bounded grace in which the serialized CM5 heartbeat
// actor can acquire the wire. This never creates or renews presence by itself.
void cm5PresenceCommandStarted(uint32_t sessionEpoch);
void cm5PresenceCommandFinished(uint32_t sessionEpoch, bool replyAdmitted);

Cm5PresenceSnapshot cm5PresenceSnapshotForSession(uint32_t activeSessionEpoch,
                                                  uint32_t nowMs);
Cm5PresenceSnapshot cm5PresenceSnapshot();

// Snapshot plus the display-only busy reason, read under ONE lock so the label
// cannot be torn away from the state it describes: sampling them separately
// leaves a window where a heartbeat lands between the two reads and a surface
// prints `state=ready reason=llm:<model>`, which is precisely the stale label
// this field is supposed to eliminate.
//
// The buffer is caller-supplied rather than a member of Cm5PresenceSnapshot so
// the ~97 bytes land only on stacks that actually want the reason. The presence
// monitor snapshots on a 2 KB stack (CM5_PRESENCE_STACK_BYTES) and does not.
// `reasonOut` is always NUL-terminated; an empty string means "no reason".
Cm5PresenceSnapshot cm5PresenceSnapshotWithReason(char* reasonOut,
                                                  size_t reasonCap);
const char* cm5PresenceModeName(Cm5PresenceMode mode);

#endif  // SYSTEM_CM5_PRESENCE_H
