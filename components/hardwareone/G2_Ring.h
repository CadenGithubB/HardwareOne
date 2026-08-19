#ifndef G2_RING_H
#define G2_RING_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

struct BlePeerConnectRequest;

// =============================================================================
// Even Realities R1 Ring — BLE central module
// =============================================================================
// The R1 ring is a SEPARATE BLE peripheral from the G2 glasses. When paired
// with Even's phone app, the phone is the BLE central for both. When this
// ESP32 is the glasses' central, it also connects to the ring: pairAuth +
// time sync, vitals polls, notify decode into the R1 telemetry cache
// (Apps → Health / OLED / sensorlog). GATT writes share bleCentralTx with
// the temples and enqueue when the gate is busy.
//
// Ring-to-glasses gesture relay via the phone hub is out of scope; gestures
// that already reach the glasses as SysEvent src=2 are observed only.
//
// Reference: ble/ring.ts in https://github.com/Commute773/g2-kit-unofficial
//
// Requires: ENABLE_BLUETOOTH=1 AND ENABLE_G2_GLASSES=1 (ring lives under
// the G2 umbrella — no separate compile flag because nobody has a ring
// without glasses).
// =============================================================================

// A request being accepted is deliberately distinct from the ring having
// applied it. Callers keep the handle and inspect the transaction snapshot;
// no setter below reports synchronous success.
enum G2RingTransactionState : uint8_t {
  G2_RING_TX_INVALID = 0,
  G2_RING_TX_QUEUED,
  G2_RING_TX_WRITTEN,
  G2_RING_TX_ACKED,
  G2_RING_TX_VERIFIED,
  G2_RING_TX_REFUSED,
  G2_RING_TX_TIMEOUT,
  G2_RING_TX_DISCONNECTED,
};

enum G2RingTransactionError : uint8_t {
  G2_RING_ERR_NONE = 0,
  G2_RING_ERR_NOT_CONNECTED,
  G2_RING_ERR_QUEUE_FULL,
  G2_RING_ERR_ENCODE,
  G2_RING_ERR_WRITE,
  G2_RING_ERR_ACK_ERROR,
  G2_RING_ERR_ACK_REFUSED,
  G2_RING_ERR_ACK_NOT_SUPPORTED,
  G2_RING_ERR_TIMEOUT,
  G2_RING_ERR_DISCONNECTED,
  G2_RING_ERR_VERIFY_MISMATCH,
  G2_RING_ERR_PROFILE_UNKNOWN,
  G2_RING_ERR_IDENTITY_UNKNOWN,
  G2_RING_ERR_CLOCK_UNAVAILABLE,
};

enum G2RingDesiredState : uint8_t {
  G2_RING_PRESERVE = 0,
  G2_RING_OFF = 1,
  G2_RING_ON = 2,
};

enum G2RingObservedState : uint8_t {
  G2_RING_OBS_UNKNOWN = 0,
  G2_RING_OBS_OFF = 1,
  G2_RING_OBS_ON = 2,
};

enum G2RingSetupState : uint8_t {
  G2_RING_SETUP_IDLE = 0,
  G2_RING_SETUP_AUTH,
  G2_RING_SETUP_DEVICE_INFO,
  G2_RING_SETUP_PROFILE,
  G2_RING_SETUP_TIME,
  G2_RING_SETUP_ADV_START,
  G2_RING_SETUP_READY,
  G2_RING_SETUP_ERROR,
};

// Public, dependency-free view of the protocol profile selected from the
// ring's exact deviceInfo firmware string. Values intentionally mirror the
// private System_R1_Protocol profile identifiers, but callers do not need to
// include the encoder/decoder header.
enum G2RingProtocolProfile : uint8_t {
  G2_RING_PROFILE_UNKNOWN = 0,
  G2_RING_PROFILE_FW_2_2_7_0005 = 1,
};

struct G2RingTransactionHandle {
  uint32_t id;
  uint32_t generation;
};

struct G2RingTransactionStatus {
  G2RingTransactionHandle handle;
  G2RingTransactionState  state;
  uint8_t                 module;
  uint8_t                 cmd;
  uint8_t                 subCmd;
  uint8_t                 ackCode;       // R1 wire ACK code, 0 when absent
  uint8_t                 errorCode;     // G2RingTransactionError
  uint32_t                queuedAtMs;
  uint32_t                writtenAtMs;
  uint32_t                completedAtMs;
};

struct G2RingControlStatus {
  uint32_t            generation;
  G2RingSetupState    setupState;
  uint8_t             setupLastError;    // G2RingTransactionError
  G2RingProtocolProfile protocolProfile;
  bool                protocolProfileKnown;
  bool                advIdentityKnown;

  G2RingDesiredState  healthDesired;
  G2RingObservedState healthObserved;
  bool                healthPending;
  uint8_t             healthLastError;
  uint32_t            healthObservedAtMs;
  G2RingTransactionHandle healthTransaction;

  G2RingDesiredState  lowPowerDesired;
  G2RingObservedState lowPowerObserved;
  bool                lowPowerPending;
  uint8_t             lowPowerLastError;
  uint32_t            lowPowerObservedAtMs;
  G2RingTransactionHandle lowPowerTransaction;
};

inline const char* g2RingTransactionStateName(G2RingTransactionState state) {
  switch (state) {
    case G2_RING_TX_QUEUED:       return "queued";
    case G2_RING_TX_WRITTEN:      return "written";
    case G2_RING_TX_ACKED:        return "acked";
    case G2_RING_TX_VERIFIED:     return "verified";
    case G2_RING_TX_REFUSED:      return "refused";
    case G2_RING_TX_TIMEOUT:      return "timeout";
    case G2_RING_TX_DISCONNECTED: return "disconnected";
    default:                      return "invalid";
  }
}

inline const char* g2RingTransactionErrorName(uint8_t errorCode) {
  switch ((G2RingTransactionError)errorCode) {
    case G2_RING_ERR_NONE:              return "none";
    case G2_RING_ERR_NOT_CONNECTED:     return "not-connected";
    case G2_RING_ERR_QUEUE_FULL:        return "queue-full";
    case G2_RING_ERR_ENCODE:            return "encode";
    case G2_RING_ERR_WRITE:             return "write";
    case G2_RING_ERR_ACK_ERROR:         return "ack-error";
    case G2_RING_ERR_ACK_REFUSED:       return "ack-refused";
    case G2_RING_ERR_ACK_NOT_SUPPORTED: return "ack-not-supported";
    case G2_RING_ERR_TIMEOUT:           return "timeout";
    case G2_RING_ERR_DISCONNECTED:      return "disconnected";
    case G2_RING_ERR_VERIFY_MISMATCH:   return "verify-mismatch";
    case G2_RING_ERR_PROFILE_UNKNOWN:   return "profile-unknown";
    case G2_RING_ERR_IDENTITY_UNKNOWN:  return "identity-unknown";
    case G2_RING_ERR_CLOCK_UNAVAILABLE: return "clock-unavailable";
    default:                            return "unknown";
  }
}

inline const char* g2RingDesiredStateName(G2RingDesiredState state) {
  switch (state) {
    case G2_RING_OFF: return "off";
    case G2_RING_ON:  return "on";
    default:          return "preserve";
  }
}

inline const char* g2RingObservedStateName(G2RingObservedState state) {
  switch (state) {
    case G2_RING_OBS_OFF: return "off";
    case G2_RING_OBS_ON:  return "on";
    default:              return "unknown";
  }
}

inline const char* g2RingSetupStateName(G2RingSetupState state) {
  switch (state) {
    case G2_RING_SETUP_AUTH:        return "auth";
    case G2_RING_SETUP_DEVICE_INFO: return "device-info";
    case G2_RING_SETUP_PROFILE:     return "profile";
    case G2_RING_SETUP_TIME:        return "time";
    case G2_RING_SETUP_ADV_START:   return "adv-start";
    case G2_RING_SETUP_READY:       return "ready";
    case G2_RING_SETUP_ERROR:       return "error";
    default:                        return "idle";
  }
}

inline const char* g2RingProtocolProfileName(G2RingProtocolProfile profile) {
  switch (profile) {
    case G2_RING_PROFILE_FW_2_2_7_0005: return "2.2.7.0005";
    default:                            return "unknown";
  }
}

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// Ring BLE UUIDs (reference ble/ring.ts:25-27).
#define G2RING_SERVICE_UUID    "bae80001-4f05-4503-8e65-3af1f7329d1f"
#define G2RING_CHAR_WRITE_UUID "bae80012-4f05-4503-8e65-3af1f7329d1f"
#define G2RING_CHAR_NOTIFY_UUID "bae80013-4f05-4503-8e65-3af1f7329d1f"

// Advert name format: "EVEN R1_XXXXXX" where XXXXXX = last 3 bytes of MAC
// in hex (e.g. mac AA:BB:CC:11:22:33 → name "EVEN R1_112233").

// Ring command opcodes (reference ble/ring.ts §R1_CMD). Not exhaustive —
// subset we encode for setup / polls / queries.
#define G2RING_CMD_HEARTRATE   0x01
#define G2RING_CMD_FIRMWARE    0x02
#define G2RING_CMD_TEMPERATURE 0x03
#define G2RING_CMD_HRV         0x04
#define G2RING_CMD_ACTIVITY    0x05
#define G2RING_CMD_SLEEP       0x06
#define G2RING_CMD_PAIR_AUTH   0x08
#define G2RING_CMD_HEALTH_SET  0x09
#define G2RING_CMD_LINK_G2     0x0A
#define G2RING_CMD_ALGO_KEY    0x0B
#define G2RING_CMD_CONFIG1     0x0E
#define G2RING_CMD_CONFIG2     0x0F
#define G2RING_CMD_SERIAL      0x11
#define G2RING_CMD_PHONE_STAT  0x7E
#define G2RING_CMD_PHONE_ACK   0x7F

// Ring flag values (envelope bytes [9..10] BE).
#define G2RING_FLAG_REQUEST    0x0000
#define G2RING_FLAG_SET        0x0001
#define G2RING_FLAG_PUSH       0x0002
#define G2RING_FLAG_RESPONSE   0x0003

// -----------------------------------------------------------------------------
// Public API (all return false / 0 / null when ring subsystem is disabled)
// -----------------------------------------------------------------------------

// Initialise the ring client (idempotent). Does NOT scan or connect —
// that happens via the CLI command or g2RingConnect().
bool g2RingInit();

// Scan for the ring advert (or use one already discovered by the main
// glasses scan) and connect to it. Non-blocking: spawns a background
// task and returns immediately, mirroring how g2Connect works for the
// glasses. The background task auto-runs g2RingScan(15) if no advert is
// stashed, so a prior `g2scan` is no longer required. Use
// g2RingIsConnected() / g2RingGetStatus() to poll.
// Synchronous connect helper used by the unified BLE-connect worker
// (see G2_Glasses.h `g2SubmitBleConnect`). Public so the worker — which
// lives in G2_Glasses.cpp — can dispatch RING_* job kinds to it without
// reaching into Ring's static implementation. If `savedMac` is non-empty,
// skips scan and tries to connect directly to that address; empty means
// "scan-then-connect any matching ring." If an actual G2 recording is active,
// the already-queued job waits for recorder IDLE before touching BLE; an
// idle-open/autostarted mic does not block it. Returns true on successful link.
// MUST run in a normal task context (allocations, delays, blocking BLE calls).
bool ringPerformConnect(const String& savedMac = String(),
                        const BlePeerConnectRequest* expectedRequest = nullptr,
                        uint32_t cancelGeneration = 0);

// Clear the per-family in-flight flag that the public g2RingConnect*
// wrappers set before submitting to the unified worker. The worker calls
// this after ringPerformConnect returns, so producers can submit again.
// (gRingConnectTaskActive itself is static to G2_Ring.cpp; this helper is
// the worker's only handle on it.)
void g2RingConnectMarkComplete();

// True while a ring connect job is queued or running on the BLE-connect
// worker (producers set the flag before submitting; the worker clears it
// when the *Sync body returns). Lets teardown paths avoid nulling
// gRing.client out from under an in-flight connect.
bool g2RingConnectInFlight();

bool g2RingConnect();

// Dedicated ring-only scan. Watches for "EVEN R1_XXXXXX" adverts and
// stashes the first match into gRingAdvertisedDevice for a subsequent
// g2RingConnect() to use. Doesn't early-terminate on glasses-found like
// the shared `g2scan` does, which makes it more reliable for picking up
// the R1's slow advertising cycle. **Blocks the calling task** for up to
// `timeoutSec` seconds (or until a ring is found, whichever comes first).
// Returns true if a ring was stashed (either freshly seen or already
// present from a prior scan). `timeoutSec` is clamped to [1, 300].
bool g2RingScan(uint32_t timeoutSec, uint32_t cancelGeneration = 0);

// Queue a ring-only scan on the shared central-operation worker. This is the
// public/manual entry point; it prevents a CLI scan from replacing the
// process-global BLEScan callback while a temple connect is in flight.
bool g2RingScanAsync(uint32_t timeoutSec);

// Saved-MAC reconnect. Reads gSettings.bleRingMAC and connects directly
// without requiring a prior scan-cached advertisement. Used by the boot
// auto-reconnect hook. Returns false if no MAC saved or a connect is
// already in flight.
bool g2RingConnectSaved();

// Caller-supplied MAC connect. Bypasses scan entirely; opens a direct
// BLE connection to the address provided. Useful when the ring is bonded
// to another central (phone) and not visible to broad scans, but the
// peripheral may still accept a connection initiated to its known address.
// Format: "aa:bb:cc:dd:ee:ff" (17 chars, colons required).
//
// Will fail (BLE connect timeout, ~30s) if the ring is sleeping, busy
// with another central that doesn't allow co-occupancy, or simply out
// of range. Async — spawns a connect task and returns immediately.
bool g2RingConnectMac(const String& mac);

// Tear down the BLE connection. Safe to call from any context.
void g2RingDisconnect(bool userInitiated = false);

// Drop ring GATT pointers without touching the controller. Call before
// BLEDevice::deinit / controller restart (e.g. openg2 tearing down server
// mode) so Health polls cannot write through a freed characteristic.
void g2RingInvalidateLink();

// Close Ring producer admission and wait until the persistent owner cannot
// dereference GATT objects, then invalidate them under the normal TX lock
// order. Required before a full BLE host teardown. Returns false rather than
// allowing teardown to free objects still reachable by r1_owner.
bool g2RingPrepareForStackTeardown(uint32_t timeoutMs);

// Non-blocking compatibility hook: wake the serialized ring owner so queued
// semantic transactions can retry the shared controller gate.
void g2RingTryDrainPendingTx();

// True if we currently have a live BLE link to the ring.
bool g2RingIsConnected();

// Fixed-capacity transaction/control snapshots. `true` from a setter means
// the desired policy was accepted for asynchronous reconciliation; inspect
// the returned handle/status (or G2RingControlStatus) for the outcome.
bool g2RingGetTransactionStatus(const G2RingTransactionHandle& handle,
                                G2RingTransactionStatus& out);
void g2RingGetControlStatus(G2RingControlStatus& out);
bool g2RingSetHealthCollectionDesired(
    G2RingDesiredState desired,
    G2RingTransactionHandle* outHandle = nullptr);
bool g2RingSetLowPowerDesired(
    G2RingDesiredState desired,
    G2RingTransactionHandle* outHandle = nullptr);
bool g2RingRefreshControlStatus(
    G2RingTransactionHandle* outHealth = nullptr,
    G2RingTransactionHandle* outLowPower = nullptr);

// Ask the transport-owned history coordinator to fetch the exact-profile
// daily metrics in sequence. Repeated requests collapse into one sweep;
// `force` is retained when any caller requests it.
bool g2RingRequestHistoryRefresh(bool force = false);

// Internal/diagnostic escape hatch. This only submits a transaction; policy
// gates (admin + confirmation for SETs) live at the CLI boundary.
bool g2RingSubmitRawTransaction(
    uint8_t module, uint8_t cmd, uint8_t subCmd,
    uint8_t statusType, uint8_t statusMethod, uint8_t statusAck,
    const uint8_t* payload, size_t payloadLen,
    G2RingTransactionHandle* outHandle = nullptr);

// Fill `buf` with a short human-readable status line. Used by the
// `ringstatus` CLI command and the web UI Ring panel.
void g2RingGetStatus(char* buf, size_t cap);

// Snapshot of the live R1 telemetry cache for read-only display (e.g. the
// G2 Ring dashboard). Copies the notify-updated values; a *Valid flag of
// false means "no sample seen yet" — render "--", not 0. Telemetry updates
// are minute-scale so a plain field copy (no lock) is fine.
// Poll cursor count for a full vitals refresh (HR→HRV→SpO2→Temp→deviceStatus).
static constexpr uint8_t G2_RING_POLL_VITAL_COUNT = 5;

struct G2RingTelemetry {
  bool     connected;
  uint8_t  hr;            bool hrValid;
  int16_t  hrv;           bool hrvValid;
  uint8_t  spo2;          bool spo2Valid;
  int16_t  tempTenths;    bool tempValid;     // °C × 10 (skin/body from point)
  uint8_t  battery;       bool batteryValid;  // byte[0] of deviceStatus
  uint8_t  wear;          bool wearValid;     // 0=unknown 1=notWear 2=wear
  // Sample age in seconds (−1 = unknown). Prefer ring epoch ts when wall
  // clock looks synced; else millis since local receive.
  int32_t  hrAgeSec;
  int32_t  hrvAgeSec;
  int32_t  spo2AgeSec;
  int32_t  tempAgeSec;
  int32_t  batteryAgeSec;
  int32_t  wearAgeSec;
  // millis() at local receive, 0 = unknown. Monotonic and immune to
  // ring-clock custody, unlike *AgeSec which prefers the ring epoch — a
  // ring stepped forward pins its age at 0 and a ring stepped back parks
  // the sample hours in the past. History consumers that need a stable
  // time axis (G2_Health's series) must use these, not *AgeSec.
  uint32_t hrRxMs;
  uint32_t hrvRxMs;
  uint32_t spo2RxMs;
  uint32_t tempRxMs;
  uint32_t batteryRxMs;
};
void g2RingGetTelemetry(G2RingTelemetry& out);

// Send ONE vitals point-query to the ring: 0=HR, 1=HRV, 2=SpO2, 3=Temp,
// 4=deviceStatus (battery + wear). The request is queued to the transaction
// owner; the reply arrives via notify and lands in the telemetry cache above.
// Returns false when not connected/queueable or `which` is out of range. Callers wanting a
// full refresh send 0..G2_RING_POLL_VITAL_COUNT-1 spaced ≥700 ms apart.
bool g2RingPollVital(uint8_t which);

// Fire a health/{cmd}/daily query (empty payload). `cmd` is R1_CMD_HEARTRATE /
// HRV / SPO2 / etc. Reply arrives via notify; parsers may call into Health
// history backfill. Returns false when not connected or not queueable.
bool g2RingQueryDaily(uint8_t cmd);

// Paced vital poll for background logging: advances an internal cursor
// (HR→HRV→SpO2→Temp→battery/wear) at most once per call when ≥700 ms have
// elapsed. No-op when not connected. Used by sensorLogTick when LOG_R1 is on.
void g2RingPollVitalForLogging(void);

// Main-loop tick: ring-clock custody. When the host clock is dark (no
// NTP/RTC yet) it adopts the ring's battery-backed time from cached point
// samples — the ring acts as an external RTC we merely echo. When the host
// clock first flips valid mid-session it sends one corrective systemTime
// push so the ring's daily-history bucketing lands on real days. Cheap
// self-throttled no-op in the steady state. Call next to timeAnchorsTick().
void g2RingTimeSyncTick(void);

// Bridge-progress hook. Called by parseSid80Rx() when a RING_CONNECT_INFO
// poll arrives from either temple. We mirror the most recent connRet /
// connectRing values so `ringbridge status` can show "scanning" / "fail" /
// "ok" without the user having to scroll the log. Pass 0 / false for fields
// that weren't present in the inbound payload.
void g2RingNoteBridgePoll(uint64_t connRet, bool hasConnRet,
                          uint64_t connectRing, bool hasConnectRing);

// Forwarded-telemetry sink. Called by the G2 sid=0x90/0x91 RX handler when
// the right temple's bridge is active and is forwarding RingDataPackage
// frames to us. Parses the protobuf wrapper, extracts each populated
// RingRawData field (battery / hr / spo2 / hrv / etc.), and updates the
// internal R1TelemetryCache so the existing status / spoof-push code
// stays useful regardless of which side actually owns the ring's BLE
// link. `pb` / `pbLen` is the envelope body (post-header proto bytes).
// Safe to call with malformed / partial frames — bails on first parse error.
void g2RingNoteForwardedTelemetry(const uint8_t* pb, size_t pbLen);

// Ring state shared with the G2 scan callback (defined in
// G2_Ring.cpp). Declared extern here so the scanner can
// stash a discovered ring advert before our module's own scan runs.
// Guarded behind the compile flag so stubs don't drag these in.
class BLEAdvertisedDevice;
extern BLEAdvertisedDevice* gRingAdvertisedDevice;
extern String               gRingDeviceName;
extern String               gRingDeviceAddress;
extern volatile bool        gRingScanFound;

// Command table entry — wired into System_Utils.cpp alongside g2Commands.
// The ring lives under the G2 umbrella but keeps its own command array for
// clarity (ringstatus, ringconnect, ringdisconnect, ringdump).
struct CommandEntry;
extern const CommandEntry g2RingCommands[];
extern const size_t       g2RingCommandsCount;

#else  // !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)

static constexpr uint8_t G2_RING_POLL_VITAL_COUNT = 5;

inline bool g2RingInit()         { return false; }
inline bool g2RingConnect()      { return false; }
inline bool g2RingConnectSaved() { return false; }
inline bool g2RingConnectMac(const String& /*mac*/) { return false; }
inline bool g2RingScan(uint32_t /*timeoutSec*/, uint32_t /*cancelGeneration*/ = 0) { return false; }
inline bool g2RingScanAsync(uint32_t /*timeoutSec*/) { return false; }
inline void g2RingDisconnect(bool = false)   {}
inline void g2RingInvalidateLink() {}
inline bool g2RingPrepareForStackTeardown(uint32_t /*timeoutMs*/) { return true; }
inline bool g2RingConnectInFlight() { return false; }
inline void g2RingTryDrainPendingTx() {}
inline bool g2RingIsConnected()  { return false; }
inline bool g2RingGetTransactionStatus(const G2RingTransactionHandle&,
                                       G2RingTransactionStatus&) { return false; }
inline void g2RingGetControlStatus(G2RingControlStatus& out) {
  out = G2RingControlStatus{};
}
inline bool g2RingSetHealthCollectionDesired(
    G2RingDesiredState, G2RingTransactionHandle* = nullptr) { return false; }
inline bool g2RingSetLowPowerDesired(
    G2RingDesiredState, G2RingTransactionHandle* = nullptr) { return false; }
inline bool g2RingRefreshControlStatus(
    G2RingTransactionHandle* = nullptr,
    G2RingTransactionHandle* = nullptr) { return false; }
inline bool g2RingRequestHistoryRefresh(bool = false) { return false; }
inline bool g2RingSubmitRawTransaction(
    uint8_t, uint8_t, uint8_t, uint8_t, uint8_t, uint8_t,
    const uint8_t*, size_t, G2RingTransactionHandle* = nullptr) { return false; }
inline void g2RingGetStatus(char* buf, size_t cap) {
  if (buf && cap > 0) buf[0] = '\0';
}
struct G2RingTelemetry {
  bool     connected;
  uint8_t  hr;            bool hrValid;
  int16_t  hrv;           bool hrvValid;
  uint8_t  spo2;          bool spo2Valid;
  int16_t  tempTenths;    bool tempValid;
  uint8_t  battery;       bool batteryValid;
  uint8_t  wear;          bool wearValid;
  int32_t  hrAgeSec;
  int32_t  hrvAgeSec;
  int32_t  spo2AgeSec;
  int32_t  tempAgeSec;
  int32_t  batteryAgeSec;
  int32_t  wearAgeSec;
  uint32_t hrRxMs;
  uint32_t hrvRxMs;
  uint32_t spo2RxMs;
  uint32_t tempRxMs;
  uint32_t batteryRxMs;
};
inline void g2RingGetTelemetry(G2RingTelemetry& out) { out = G2RingTelemetry{}; }
inline bool g2RingPollVital(uint8_t) { return false; }
inline bool g2RingQueryDaily(uint8_t) { return false; }
inline void g2RingPollVitalForLogging(void) {}
inline void g2RingTimeSyncTick(void) {}
inline void g2RingNoteForwardedTelemetry(const uint8_t*, size_t) {}
inline void g2RingNoteBridgePoll(uint64_t, bool, uint64_t, bool) {}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#endif  // G2_RING_H
