#ifndef G2_RING_H
#define G2_RING_H

#include "System_BuildConfig.h"
#include <Arduino.h>

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
// "scan-then-connect any matching ring." Returns true on successful link.
// MUST run in a normal task context (allocations, blocking BLE calls).
bool ringPerformConnect(const String& savedMac = String());

// Clear the per-family in-flight flag that the public g2RingConnect*
// wrappers set before submitting to the unified worker. The worker calls
// this after ringPerformConnect returns, so producers can submit again.
// (gRingConnectTaskActive itself is static to G2_Ring.cpp; this helper is
// the worker's only handle on it.)
void g2RingConnectMarkComplete();

bool g2RingConnect();

// Dedicated ring-only scan. Watches for "EVEN R1_XXXXXX" adverts and
// stashes the first match into gRingAdvertisedDevice for a subsequent
// g2RingConnect() to use. Doesn't early-terminate on glasses-found like
// the shared `g2scan` does, which makes it more reliable for picking up
// the R1's slow advertising cycle. **Blocks the calling task** for up to
// `timeoutSec` seconds (or until a ring is found, whichever comes first).
// Returns true if a ring was stashed (either freshly seen or already
// present from a prior scan). `timeoutSec` is clamped to [1, 300].
bool g2RingScan(uint32_t timeoutSec);

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
void g2RingDisconnect();

// Drop ring GATT pointers without touching the controller. Call before
// BLEDevice::deinit / controller restart (e.g. openg2 tearing down server
// mode) so Health polls cannot write through a freed characteristic.
void g2RingInvalidateLink();

// Non-blocking: if bleCentralTx is free, flush any ring frames that were
// queued while an image burst (or other peer) held the controller gate.
// Safe from G2 send paths between envelopes; no-op when empty or busy.
void g2RingTryDrainPendingTx();

// True if we currently have a live BLE link to the ring.
bool g2RingIsConnected();

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
};
void g2RingGetTelemetry(G2RingTelemetry& out);

// Send ONE vitals point-query to the ring: 0=HR, 1=HRV, 2=SpO2, 3=Temp,
// 4=deviceStatus (battery + wear). Fire-and-forget write-no-response; the
// reply arrives via notify and lands in the telemetry cache above. Returns
// false when not connected or `which` is out of range. Callers wanting a
// full refresh send 0..G2_RING_POLL_VITAL_COUNT-1 spaced ≥700 ms apart.
bool g2RingPollVital(uint8_t which);

// Fire a health/{cmd}/daily query (empty payload). `cmd` is R1_CMD_HEARTRATE /
// HRV / SPO2 / etc. Reply arrives via notify; parsers may call into Health
// history backfill. Returns false when not connected.
bool g2RingQueryDaily(uint8_t cmd);

// Paced vital poll for background logging: advances an internal cursor
// (HR→HRV→SpO2→Temp→battery/wear) at most once per call when ≥700 ms have
// elapsed. No-op when not connected. Used by sensorLogTick when LOG_R1 is on.
void g2RingPollVitalForLogging(void);

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
inline bool g2RingScan(uint32_t /*timeoutSec*/) { return false; }
inline void g2RingDisconnect()   {}
inline void g2RingInvalidateLink() {}
inline void g2RingTryDrainPendingTx() {}
inline bool g2RingIsConnected()  { return false; }
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
};
inline void g2RingGetTelemetry(G2RingTelemetry& out) { out = G2RingTelemetry{}; }
inline bool g2RingPollVital(uint8_t) { return false; }
inline bool g2RingQueryDaily(uint8_t) { return false; }
inline void g2RingPollVitalForLogging(void) {}
inline void g2RingNoteForwardedTelemetry(const uint8_t*, size_t) {}
inline void g2RingNoteBridgePoll(uint64_t, bool, uint64_t, bool) {}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#endif  // G2_RING_H
