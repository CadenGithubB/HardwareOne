// =============================================================================
// Even Realities R1 Ring — BLE central
// =============================================================================
// See G2_Ring.h. Connect, pairAuth / time / advStart, subscribe to notify,
// poll vitals, decode into the R1 cache. Shares bleCentralTx with G2 temples.

#include "G2_Ring.h"
#include <esp_attr.h>

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEAdvertisedDevice.h>

#include "System_Debug.h"
#include "System_Command.h"
#include "System_TaskUtils.h"  // APP_CORE / PRO_CORE task-placement constants
#include "System_Utils.h"
#include "System_MemUtil.h"
#include "WebServer_Server.h"  // broadcastEventToAllSessions() for SSE push
#include "System_Settings.h"   // gSettings + setSetting() for MAC persistence
#include "BLE_Events.h"        // CompactJson + blePushEvent
#include "BLE_Peers.h"         // peer registry
#include "BLE_CentralTx.h"     // controller-level TX gate (shared with G2)
#include "G2_Glasses.h"        // g2SetAllTemplesConnPriority, g2WaitForBothConnected
#include "System_R1_Protocol.h"  // R1Encoder + decoder (real wire format)
#include "System_G2_Protocol.h"  // g2BuildRingRawDataPush + G2RingPushFields
#include "G2_Health.h"           // history append + daily backfill
#include "System_Events.h"       // systemEventPost — ring connect/disconnect bus events
#include "System_Clock.h"        // isSynced/isValidEpoch — ring-clock custody gates

#include <freertos/FreeRTOS.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <ctype.h>

// =============================================================================
// Shared state with the G2 scanner
// =============================================================================
// The glasses' scan callback (G2_Glasses.cpp) also classifies EVEN R1_*
// adverts and stashes the advertisedDevice here. That saves us from running
// a second scan pass for the ring in the common case — when the user runs
// `openg2`, the glasses scan will surface both the temples and the ring in
// one pass.
BLEAdvertisedDevice* gRingAdvertisedDevice = nullptr;
String               gRingDeviceName;
String               gRingDeviceAddress;
volatile bool        gRingScanFound       = false;

// =============================================================================
// BLE peer registry binding
// =============================================================================
static bool ringPeerConnectSavedThunk() { return g2RingConnectSaved(); }
static void ringPeerDisconnectThunk() {
  // Intentional tear-down (registry / CLI paths that use ops->disconnect).
  blePeerNoteUserDisconnect(BLE_PEER_R1_RING);
  g2RingDisconnect();
}
static bool ringPeerIsConnectedThunk()  { return g2RingIsConnected(); }
static const BlePeerOps ringPeerOps = {
  ringPeerConnectSavedThunk,
  ringPeerDisconnectThunk,
  ringPeerIsConnectedThunk,
};
static const BlePeerSpec ringPeerSpec = {
  BLE_PEER_R1_RING,
  "r1-ring",
  "R1 Ring",
  /*macCount=*/1,
  /*connectable=*/true,
  &ringPeerOps,
};

// =============================================================================
// Private module state
// =============================================================================

struct G2RingState {
  BLEClient*               client          = nullptr;
  BLERemoteCharacteristic* writeChar       = nullptr;
  BLERemoteCharacteristic* notifyChar      = nullptr;
  bool                     initialized     = false;
  bool                     connected       = false;
  bool                     clientStale     = false;
  uint16_t                 mtu             = 23;
  uint32_t                 connectedSince  = 0;
  uint32_t                 packetsReceived = 0;
  uint32_t                 packetsSent     = 0;
  SemaphoreHandle_t        writeMutex      = nullptr;
};
static G2RingState gRing;

// Short central take — if the glasses hold the gate for an envelope, we
// enqueue rather than block for seconds (or drop into the ether).
static constexpr uint32_t RING_CENTRAL_TX_MS = 50;
static constexpr uint32_t RING_WRITE_MUTEX_MS = 1500;

// Pending TX while bleCentralTx is busy (image envelopes, heartbeats, …).
// coalesceKey != 0 → replace any queued frame with the same key (polls).
// coalesceKey == 0 → FIFO; if full, drop the oldest non-coalesced slot.
static constexpr size_t RING_TXQ_DEPTH = 8;
struct RingTxPending {
  uint8_t  bytes[R1_MAX_FRAME];
  uint16_t len;
  uint8_t  coalesceKey;  // 0 = unique; 1.. = replace same key
};
static RingTxPending gRingTxQ[RING_TXQ_DEPTH];
static uint8_t       gRingTxQCount = 0;
static portMUX_TYPE  gRingTxQMux = portMUX_INITIALIZER_UNLOCKED;

// Lock order: bleCentralTx → gRing.writeMutex (never reverse). All ring
// GATT writes go through here so image bursts on the glasses cannot race
// ring polls into the shared BT controller queue.
// dropClientPtr=true only when the underlying object is (about to be) freed
// by someone else — i.e. before BLEDevice::deinit, which deletes m_pClient.
// With the BLE stack still live, pass false: deleting here would race the
// async DISCONNECT_EVT (the object is still registered for events until that
// handler unregisters it), and nulling without delete leaks the client and
// its ~10-14 KB GATT cache. Keeping the pointer with clientStale=true lets
// the next connect's stale-replacement branch reap it safely.
static void ringClockCustodyReset();  // ring-clock custody state (defined below)

static void ringClearGattPointers(bool dropClientPtr) {
  gRing.connected  = false;
  gRing.writeChar  = nullptr;
  gRing.notifyChar = nullptr;
  if (dropClientPtr) gRing.client = nullptr;
  gRing.clientStale = true;
  ringClockCustodyReset();  // defined below — no cross-link state survives
}

static void ringTxQClear() {
  portENTER_CRITICAL(&gRingTxQMux);
  gRingTxQCount = 0;
  portEXIT_CRITICAL(&gRingTxQMux);
}

// Returns true if the frame was stored (or replaced a same-key entry).
static bool ringTxQEnqueue(const uint8_t* data, size_t len, uint8_t coalesceKey) {
  if (!data || len == 0 || len > R1_MAX_FRAME) return false;
  portENTER_CRITICAL(&gRingTxQMux);
  if (coalesceKey != 0) {
    for (uint8_t i = 0; i < gRingTxQCount; i++) {
      if (gRingTxQ[i].coalesceKey == coalesceKey) {
        memcpy(gRingTxQ[i].bytes, data, len);
        gRingTxQ[i].len = (uint16_t)len;
        portEXIT_CRITICAL(&gRingTxQMux);
        return true;
      }
    }
  }
  bool droppedOldest = false;
  if (gRingTxQCount >= RING_TXQ_DEPTH) {
    // Drop oldest; shift left.
    for (uint8_t i = 1; i < RING_TXQ_DEPTH; i++) gRingTxQ[i - 1] = gRingTxQ[i];
    gRingTxQCount = RING_TXQ_DEPTH - 1;
    droppedOldest = true;
  }
  RingTxPending& slot = gRingTxQ[gRingTxQCount++];
  memcpy(slot.bytes, data, len);
  slot.len = (uint16_t)len;
  slot.coalesceKey = coalesceKey;
  portEXIT_CRITICAL(&gRingTxQMux);
  if (droppedOldest) {
    DEBUG_G2F("[RING] TX queue full — dropped oldest pending");
  }
  return true;
}

// Pop front into out; returns false if empty. out must be R1_MAX_FRAME.
static bool ringTxQPop(uint8_t* out, uint16_t* outLen) {
  portENTER_CRITICAL(&gRingTxQMux);
  if (gRingTxQCount == 0) {
    portEXIT_CRITICAL(&gRingTxQMux);
    return false;
  }
  memcpy(out, gRingTxQ[0].bytes, gRingTxQ[0].len);
  *outLen = gRingTxQ[0].len;
  for (uint8_t i = 1; i < gRingTxQCount; i++) gRingTxQ[i - 1] = gRingTxQ[i];
  gRingTxQCount--;
  portEXIT_CRITICAL(&gRingTxQMux);
  return true;
}

static bool ringTxQPushFront(const uint8_t* data, size_t len, uint8_t coalesceKey) {
  if (!data || len == 0 || len > R1_MAX_FRAME) return false;
  portENTER_CRITICAL(&gRingTxQMux);
  if (gRingTxQCount >= RING_TXQ_DEPTH) {
    gRingTxQCount = RING_TXQ_DEPTH - 1;  // drop newest at end
  }
  for (uint8_t i = gRingTxQCount; i > 0; i--) gRingTxQ[i] = gRingTxQ[i - 1];
  memcpy(gRingTxQ[0].bytes, data, len);
  gRingTxQ[0].len = (uint16_t)len;
  gRingTxQ[0].coalesceKey = coalesceKey;
  gRingTxQCount++;
  portEXIT_CRITICAL(&gRingTxQMux);
  return true;
}

// Caller holds bleCentralTx. Sends one GATT write under writeMutex.
static bool ringWriteLocked(const uint8_t* data, size_t len) {
  if (!data || len == 0 || !gRing.connected || !gRing.writeChar) return false;
  if (!gRing.writeMutex || gRing.clientStale) return false;
  if (xSemaphoreTake(gRing.writeMutex, pdMS_TO_TICKS(RING_WRITE_MUTEX_MS)) !=
      pdTRUE) {
    DEBUG_G2F("[RING] Write mutex timeout; write dropped (%u B)", (unsigned)len);
    return false;
  }
  if (!gRing.connected || !gRing.writeChar || gRing.clientStale) {
    xSemaphoreGive(gRing.writeMutex);
    return false;
  }
  const bool ok =
      gRing.writeChar->writeValue(const_cast<uint8_t*>(data), len, false);
  if (ok) gRing.packetsSent++;
  xSemaphoreGive(gRing.writeMutex);
  return ok;
}

// Drain while holding bleCentralTx. Stops on first GATT failure and
// restores that frame to the front of the queue.
static void ringDrainPendingLocked() {
  uint8_t  buf[R1_MAX_FRAME];
  uint16_t len = 0;
  while (ringTxQPop(buf, &len)) {
    if (!ringWriteLocked(buf, len)) {
      (void)ringTxQPushFront(buf, len, /*coalesceKey=*/0);
      break;
    }
  }
}

void g2RingTryDrainPendingTx() {
  if (!gRing.connected || !gRing.writeChar || gRing.clientStale) {
    ringTxQClear();
    return;
  }
  if (!bleCentralTxTake(0)) return;
  ringDrainPendingLocked();
  bleCentralTxGive();
}

// coalesceKey: 0 = FIFO unique; non-zero = replace same key if already queued.
static bool ringWrite(const uint8_t* data, size_t len, uint8_t coalesceKey = 0) {
  // Do not call methods on writeChar to "probe" liveness — after
  // BLEDevice::deinit the pointer is dangling. Callers must
  // g2RingInvalidateLink() before tearing down the stack.
  if (!data || len == 0 || !gRing.connected || !gRing.writeChar) return false;
  if (!gRing.writeMutex || gRing.clientStale) return false;
  if (!bleCentralTxTake(RING_CENTRAL_TX_MS)) {
    if (ringTxQEnqueue(data, len, coalesceKey)) {
      DEBUG_G2F("[RING] Central TX busy; write queued (%u B key=%u)",
                (unsigned)len, (unsigned)coalesceKey);
      return true;
    }
    DEBUG_G2F("[RING] Central TX busy; write dropped (%u B)", (unsigned)len);
    return false;
  }
  ringDrainPendingLocked();
  const bool ok = ringWriteLocked(data, len);
  bleCentralTxGive();
  return ok;
}

// Runtime verbose flag — toggle with `ringverbose on/off` if we ever need
// to silence the byte-dump once we've characterised the stream. Default is
// on because we're still learning what the ring emits.
static bool gRingDumpVerbose = true;

// R1 protocol encoder — owns the per-session serial counter. Reset on every
// fresh connect so the ring sees serial=1 first (matches FlutterApp behaviour
// — they construct a new RingPacketEncoder per ring instance).
static R1Encoder gR1Encoder;

// =============================================================================
// Telemetry cache — what we've decoded from ring notify frames so far
// =============================================================================
// The `point` queries return cached samples — the ring's auto-recording
// cadence updates them every few minutes, not on demand. So we mirror them
// into our own cache and let the spoof-push task forward whatever's fresh
// to the glasses periodically. Each metric tracks its own "is valid" flag
// so a partial cache (e.g. HR known but HRV not yet seen) translates into
// a partial proto frame that omits the missing fields.
struct R1TelemetryCache {
  uint8_t  hr;           uint32_t hrTs;        uint32_t hrRxMs;       bool hrValid;
  int16_t  hrv;          uint32_t hrvTs;       uint32_t hrvRxMs;      bool hrvValid;
  uint8_t  spo2;         uint32_t spo2Ts;      uint32_t spo2RxMs;     bool spo2Valid;
  int16_t  tempTenths;   uint32_t tempTs;      uint32_t tempRxMs;     bool tempValid;
  uint8_t  battery;                            uint32_t batteryRxMs;  bool batteryValid;
  uint8_t  wear;                               uint32_t wearRxMs;     bool wearValid;
};
static R1TelemetryCache gR1Cache;

// Freshest ring timestamp SEEN on the wire, independent of the per-metric
// value gates below — an unworn ring answers an HR point query with hr=0
// (which the range checks rightly discard) but the frame still carries the
// ring's clock, and that is all ring-clock custody needs. Written from the
// BLE notify task (two plain u32 stores), read from the main-loop tick;
// a torn read only mis-ages the estimate by the gap between frames.
static volatile uint32_t sRingTsSeen     = 0;
static volatile uint32_t sRingTsSeenRxMs = 0;

// Last wear enum we posted a bus event for (1=notWear, 2=wear). 0 = none yet
// this link. Unknown (0) samples do not clear this — avoids re-firing on
// intermittent unknowns. Reset on disconnect so the next session can edge.
static uint8_t sRingWearPosted = 0;

// Update wear cache + edge-fire ring_worn / ring_not_worn (G2 worn analogue).
// wear: 0=unknown (ignored for events), 1=notWear, 2=wear.
static void ringNoteWear(uint8_t wear, uint32_t rxMs) {
  if (wear > 2) return;
  gR1Cache.wear      = wear;
  gR1Cache.wearRxMs  = rxMs;
  gR1Cache.wearValid = true;  // sample received (0=unknown still counts)
  if (wear == 0 || wear == sRingWearPosted) return;

  sRingWearPosted = wear;
  const char* name =
      gRingDeviceName.length() > 0 ? gRingDeviceName.c_str() : "R1";
  if (wear == 2) {
    systemEventPost(SYSEVT_RING_WORN, name);
  } else {
    systemEventPost(SYSEVT_RING_NOT_WORN, name);
  }
}

// Age in seconds for UI/JSON. Prefer ring epoch when both clocks look
// synced (Clock::isValidEpoch); else millis since local receive. −1 = unknown.
static int32_t ringSampleAgeSec(uint32_t ringTs, uint32_t rxMs) {
  const time_t now = time(nullptr);
  if (Clock::isValidEpoch((time_t)ringTs) && Clock::isValidEpoch(now)) {
    int64_t a = (int64_t)now - (int64_t)ringTs;
    if (a < 0) a = 0;
    if (a > 0x7fffffffLL) a = 0x7fffffffLL;
    return (int32_t)a;
  }
  if (rxMs != 0) {
    return (int32_t)((millis() - rxMs) / 1000u);
  }
  return -1;
}

// =============================================================================
// Ring-clock custody
// =============================================================================
// The ring keeps its own battery-backed clock and survives our reboots, so
// after a dark boot (no RTC, no WiFi/NTP) it is often the only component in
// reach that still knows the real time. Custody, not origination: the ring
// only ever holds what some synced host once pushed into it — but that is
// exactly what a DS3231 with a coin cell is, too. Two rules keep the chain
// alive:
//   1. NEVER overwrite the ring's clock with a dark epoch. The connect
//      ritual's systemTime frame is mandatory (RE'd sequence — frames are
//      not dropped from it), so when the host is dark, setup waits a bounded
//      window for g2RingTimeSyncTick() to adopt the ring's time first; the
//      frame then echoes the ring's own clock back at it.
//   2. Whenever the host clock disagrees with what the ring was last told
//      by more than 2 minutes, send a corrective systemTime. This covers
//      the dark push corrected by NTP, an adoption-echo (which carries the
//      ring's last MEASUREMENT time, stale by minutes) trued up by NTP,
//      and a manual timeset — one drift rule instead of one-shot flags.
// sRingSetupDone gates the corrective push so the tick can never inject a
// frame into the middle of the setup ritual.
static volatile bool sRingSetupDone        = false;  // standard setup finished this link
static volatile uint32_t sLastPushedEpoch  = 0;      // epoch of the last systemTime SET (0 = none yet)
static volatile uint32_t sLastPushedAtMs   = 0;      // millis() when that SET was written
static volatile uint8_t sDarkProbesSent    = 0;      // post-setup hr/point solicits this link (cap 3)
// Adoption is once per boot. Without this, a user who deliberately sets a
// pre-2020 clock (`timeset 1999-…`, which leaves Clock::isSynced() false)
// would have it silently overwritten by the ring within one 500 ms tick.
static bool sAdoptedThisBoot = false;

// Drop every per-link custody fact. Called from both teardown paths
// (ringClearGattPointers) and again at the top of the setup ritual, so a
// stamp, a push record, or a probe budget from a PREVIOUS link can never be
// mistaken for this one's. Deliberately does NOT clear sAdoptedThisBoot —
// that latch is per boot, not per link.
static void ringClockCustodyReset() {
  sRingSetupDone   = false;
  sLastPushedEpoch = 0;
  sLastPushedAtMs  = 0;
  sDarkProbesSent  = 0;
  sRingTsSeen      = 0;
  sRingTsSeenRxMs  = 0;
}

// Freshest ring-stamped epoch across the cached point samples, adjusted by
// time-since-receive so it reads as "now". 0 when no cached sample carries a
// plausible date — a factory-fresh or fully-drained ring reports ~1970 and
// must never be adopted. Upper bound mirrors rtcEarlyBootSync's 2099 cap.
// Longest receive-age we will age-adjust across. A stamp whose rxMs is older
// than this is not evidence of "now" any more (and a zero/stale rxMs would
// project by the whole uptime), so we refuse rather than guess.
static constexpr uint32_t kRingTsMaxAgeMs = 30u * 60u * 1000u;  // 30 min

static time_t ringBestKnownEpoch(void) {
  uint32_t ts = 0, rx = 0;
  // Read the volatile pair ONCE into locals: sRingTsSeen is published after
  // its rxMs, so a non-zero ts here always has its own rxMs already stored.
  const uint32_t seenTs = sRingTsSeen;
  const uint32_t seenRx = sRingTsSeenRxMs;
  if (seenTs != 0)                                { ts = seenTs;          rx = seenRx;            }
  if (gR1Cache.hrValid   && gR1Cache.hrTs   > ts) { ts = gR1Cache.hrTs;   rx = gR1Cache.hrRxMs;   }
  if (gR1Cache.hrvValid  && gR1Cache.hrvTs  > ts) { ts = gR1Cache.hrvTs;  rx = gR1Cache.hrvRxMs;  }
  if (gR1Cache.spo2Valid && gR1Cache.spo2Ts > ts) { ts = gR1Cache.spo2Ts; rx = gR1Cache.spo2RxMs; }
  if (gR1Cache.tempValid && gR1Cache.tempTs > ts) { ts = gR1Cache.tempTs; rx = gR1Cache.tempRxMs; }
  if (!Clock::isPlausibleEpoch((time_t)ts)) return 0;
  if (rx == 0) return 0;  // never received locally — cannot age-adjust it

  const uint32_t ageMs = (uint32_t)millis() - rx;   // unsigned: wrap-safe
  if (ageMs > kRingTsMaxAgeMs) return 0;            // too stale to call "now"

  const time_t projected = (time_t)ts + (time_t)(ageMs / 1000u);
  // Bound the RESULT too, not just the raw stamp: the age adjustment is what
  // could push a sane-looking stamp past the ceiling.
  if (!Clock::isPlausibleEpoch(projected)) return 0;
  return projected;
}

// Adopt the ring's clock when the host has no time source of its own.
// MAIN-LOOP CONTEXT ONLY: the clock-step chores this triggers include
// users.json I/O — too heavy for the BLE notify task or the connect
// worker's small stack, which is why setup WAITS on this instead of
// calling it directly. All post-step duties (TIME_SYNCED event, boot
// anchor, pending-user resolve, scheduler wake, RTC write-back) flow
// through Clock::clockStepped(); the tick that calls us drains them on
// the next lap via Clock::clockDutiesTick().
static bool ringAdoptClockIfDark(void) {
  if (sAdoptedThisBoot) return false;  // once per boot; see the latch's note
  if (Clock::isSynced()) return false;
  const time_t adopted = ringBestKnownEpoch();
  if (adopted == 0) return false;
  sAdoptedThisBoot = true;

  const time_t before = time(nullptr);  // pre-step, for the duty helper
  struct timeval tv = { .tv_sec = adopted, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  char tsStr[24] = "";
  struct tm tmNow;
  if (localtime_r(&adopted, &tmNow)) {
    strftime(tsStr, sizeof(tsStr), "%Y-%m-%d %H:%M", &tmNow);
  }
  BROADCAST_PRINTF("[RING] Adopted ring clock: %s (no local time source)", tsStr);

  Clock::clockStepped(Clock::SYNC_RING, before);
  return true;
}

// Main-loop tick (called next to timeAnchorsTick). Two duties: dark-clock
// adoption from the ring cache, and the one-shot corrective systemTime push
// once the clock is valid and the setup ritual has finished. Both branches
// are cheap gated no-ops in the steady state.
void g2RingTimeSyncTick(void) {
  static uint32_t sLastMs = 0;
  if (!everyMs(&sLastMs, 500)) return;

  ringAdoptClockIfDark();

  // Post-setup solicit: still dark with a live link — ask for an HR point
  // (the response's timestamp feeds sRingTsSeen even when unworn). Capped
  // per link: if the ring's own clock is dark too, re-asking won't help.
  if (!Clock::isSynced() && gRing.connected && sRingSetupDone &&
      sDarkProbesSent < 3) {
    static uint32_t sProbeMs = 0;
    if (everyMs(&sProbeMs, 5000) && g2RingPollVital(0)) {
      sDarkProbesSent = (uint8_t)(sDarkProbesSent + 1);
      DEBUG_G2F("[RING] dark-clock: TX hr/point solicit %u/3",
                (unsigned)sDarkProbesSent);
    }
  }

  // Corrective push: our clock is valid and disagrees with the projection
  // of what the ring was last told by >2 min — covers the dark push trued
  // by NTP, a stale adoption-echo (ring's last MEASUREMENT time) trued by
  // NTP, and a manual timeset. Self-quenching: drift ≈ 0 after each push.
  if (gRing.connected && sRingSetupDone && Clock::isSynced()) {
    const time_t now = time(nullptr);
    const uint32_t pushedEpoch = sLastPushedEpoch;
    const uint32_t pushedAtMs  = sLastPushedAtMs;
    const int64_t expected = (int64_t)pushedEpoch +
        (int64_t)(((uint32_t)millis() - pushedAtMs) / 1000u);
    int64_t drift = (int64_t)now - expected;
    if (drift < 0) drift = -drift;
    if (pushedEpoch == 0 || drift > 120) {
      R1Frame f = gR1Encoder.buildSyncTime(0, (uint32_t)now);
      if (f.length > 0 && ringWrite(f.bytes, f.length)) {
        sLastPushedEpoch = (uint32_t)now;
        sLastPushedAtMs  = (uint32_t)millis();
        BROADCAST_PRINTF("[RING] TX systemTime (corrective) epoch=%lu — ring clock "
                         "trued up (drift was %llds)",
                         (unsigned long)now,
                         (long long)(pushedEpoch == 0 ? 0 : drift));
      }
    }
  }
}

// Spoof-push task state. The task wakes every `gSpoofIntervalSec` seconds,
// polls the ring for fresh point samples, and synthesises a sid=0x90
// RingDataPackage frame to the glasses so their UI displays ring telemetry
// as if the official bridge were active.
static volatile bool   gSpoofEnabled       = false;
static uint32_t        gSpoofIntervalSec   = 30;
static TaskHandle_t    gSpoofTaskHandle    = nullptr;

// Per-family in-flight flag — set by the public g2RingConnect* wrappers
// before submitting to the unified BLE-connect worker (see G2_Glasses.h
// `g2SubmitBleConnect`), cleared by the worker's dispatch (via the
// g2RingConnectMarkComplete helper below, since the flag is file-static
// and the worker lives in a different TU). Producers check this to reject
// duplicate submissions. Group B retired the per-call task spawn; the
// handle that used to track each transient task is gone.
static volatile bool  gRingConnectTaskActive = false;
// Stamped whenever the flag flips true, so reject paths can report how long
// the in-flight attempt has been running — a normal saved-MAC attempt tops
// out around 53s (20s glasses wait + 3s settle + ~30s connect timeout);
// anything much past that means the worker is stuck.
static volatile uint32_t gRingConnectTaskSinceMs = 0;

void g2RingConnectMarkComplete() { gRingConnectTaskActive = false; }
bool g2RingConnectInFlight()     { return gRingConnectTaskActive; }

// Failed-connect visibility. Every failure broadcasts on regular output, but
// the persisted bus event is throttled to the first failure of a streak plus
// one per 10 min — an unreachable ring retries every ≤180s and would
// otherwise flood events.log. Streak resets on successful connect.
static uint8_t  sRingConnFailStreak    = 0;
static uint32_t sRingConnFailLastEvtMs = 0;

static void ringNoteConnectFailure(const char* reason, uint32_t elapsedMs) {
  if (sRingConnFailStreak < 255) sRingConnFailStreak++;
  const uint32_t now = millis();
  if (sRingConnFailStreak > 1 && (now - sRingConnFailLastEvtMs) < 600000UL) return;
  sRingConnFailLastEvtMs = now;
  char detail[48];
  snprintf(detail, sizeof(detail), "%s fail#%u %lus", reason,
           (unsigned)sRingConnFailStreak, (unsigned long)(elapsedMs / 1000));
  systemEventPost(SYSEVT_RING_RECONNECT_FAILED,
                  gRingDeviceName.length() > 0 ? gRingDeviceName.c_str() : "R1",
                  detail);
}

// Gate shared by the three public connect entry points. Returns true when
// clear to submit. Rejects while an attempt is in flight — but past the
// watchdog bound it assumes the worker lost its completion path, clears the
// flag loudly, and lets this attempt proceed. 240s is well beyond the worst
// legitimate job (20s glasses wait + 3s settle + 35s connect + 30s discovery
// + bounded subscribe/setup ≈ 100s). ringPerformConnect's already-connected
// guard makes a duplicate queued job harmless; if the worker is genuinely
// dead the queue backs up at depth 4 and submit failures surface it.
static bool ringConnectGateOpen() {
  if (!gRingConnectTaskActive) return true;
  const uint32_t inflightS = (millis() - gRingConnectTaskSinceMs) / 1000;
  if (inflightS > 240) {
    BROADCAST_PRINTF("[RING] WATCHDOG: connect attempt stuck for %lus — "
                     "clearing in-flight flag and retrying",
                     (unsigned long)inflightS);
    char d[40];
    snprintf(d, sizeof(d), "watchdog clear after %lus",
             (unsigned long)inflightS);
    systemEventPost(SYSEVT_RING_RECONNECT_FAILED,
                    gRingDeviceName.length() > 0 ? gRingDeviceName.c_str() : "R1",
                    d);
    gRingConnectTaskActive = false;
    return true;
  }
  BROADCAST_PRINTF("[RING] Connect skipped — attempt already in flight (%lus)",
                   (unsigned long)inflightS);
  if (inflightS > 120) ringNoteConnectFailure("stuck", inflightS * 1000);
  return false;
}

// Push a `ring-status` SSE on every meaningful transition: connect-ok,
// disconnect, scan-found. Compact keys because the SSE queue's data field
// is capped at 128 chars (EVENT_DATA_MAX in WebServer_Server.h) — the G2
// payload hits ~90 bytes, so we match that shape.
//
// The Bluetooth web page does NOT listen to this event — it polls
// /api/ble/status instead (see "Why not pure SSE" in WebPage_Bluetooth.h),
// so this event's only consumers are external SSE subscribers.
//
// Schema:
//   u  = up     (bool)  — BLE link live
//   n  = name   (str)   — advert name ("EVEN R1_XXXXXX")
//   a  = addr   (str)   — MAC
//   m  = mtu    (int)
//   rx = rx     (int)   — cumulative packet count
//   s  = scan   (str)   — "found" | "not-found"
//   w  = reason (str)   — short tag for the transition
static void ringPushStatusEvent(const char* reason) {
  // Length-bounded + escape-safe via CompactJson. See BLE_Events.h.
  char buf[128];
  CompactJson j(buf, sizeof(buf));
  j.kv("u",  (bool)gRing.connected)
   .kv("n",  gRingDeviceName.length()    ? gRingDeviceName.c_str()    : "")
   .kv("a",  gRingDeviceAddress.length() ? gRingDeviceAddress.c_str() : "")
   .kv("m",  (unsigned)gRing.mtu)
   .kv("rx", (unsigned long)gRing.packetsReceived)
   .kv("s",  gRingScanFound ? "found" : "not-found")
   .kv("w",  reason ? reason : "");
  blePushEvent("ring-status", j);
}

// =============================================================================
// Telemetry cache extraction
// =============================================================================
// Pull HR / HRV / SpO2 from health/{cmd}/point notifies, and the (likely)
// battery byte from system/system/{deviceStatus,heartbeatPack}. Mirrors the
// hypothesis encoded in r1AnnotatePayload(): for HR the BPM is extra_value
// (LE i16 at offset 7..8 when payloadLength >= 9). For HRV/SpO2 we follow
// the same slot — confidence is lower but it's the best guess we have until
// captures show otherwise. Temperature uses `value` (offset 0..1) per the
// existing annotator comment, but we don't push temp through the spoof yet.
//
// Each metric only updates the cache when the response is the expected
// shape (length / opcode), so a malformed/refused frame can't poison the
// cache with garbage. Caller (the spoof task) checks each `*Valid` flag
// before serialising — partial cache → partial proto frame.
static void ringExtractTelemetryCache(const R1Decoded& d) {
  // health/{hr,hrv,spo2,temp}/point — value/state/timestamp/extra layout.
  if (d.module == R1_MODULE_HEALTH && d.subCmd == R1_SUB_POINT &&
      d.payloadLength >= 7) {
    const uint8_t* p = d.payload;
    int16_t  value = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    uint32_t ts    = (uint32_t)p[2]        | ((uint32_t)p[3] << 8) |
                     ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24);
    long extra = 0;
    bool hasExtra = false;
    if (d.payloadLength >= 9) {
      extra = (long)(int16_t)((uint16_t)p[7] | ((uint16_t)p[8] << 8));
      hasExtra = true;
    } else if (d.payloadLength >= 8) {
      extra = (long)p[7];
      hasExtra = true;
    }

    const uint32_t rx = millis();
    if (ts != 0) {  // ring-clock custody: any point frame carries the clock
      // Publish rxMs FIRST: sRingTsSeen is the pair's validity flag, so a
      // main-loop read that lands between the two stores must see ts==0 (and
      // skip) rather than a fresh ts paired with a stale/zero rxMs — which
      // would project the adopted epoch too far ahead.
      sRingTsSeenRxMs = rx;
      sRingTsSeen     = ts;
    }
    switch (d.cmd) {
      case R1_CMD_HEARTRATE:
        if (hasExtra && extra > 0 && extra < 250) {
          gR1Cache.hr      = (uint8_t)extra;
          gR1Cache.hrTs    = ts;
          gR1Cache.hrRxMs  = rx;
          gR1Cache.hrValid = true;
          g2HealthNoteSample(HEALTH_METRIC_HR, (int16_t)extra, ts);
        }
        break;
      case R1_CMD_HRV:
        // Try extra first (HR convention), fall back to value. HRV in
        // milliseconds is a small positive number — anything else is junk.
        if (hasExtra && extra > 0 && extra < 1000) {
          gR1Cache.hrv      = (int16_t)extra;
          gR1Cache.hrvTs    = ts;
          gR1Cache.hrvRxMs  = rx;
          gR1Cache.hrvValid = true;
          g2HealthNoteSample(HEALTH_METRIC_HRV, (int16_t)extra, ts);
        } else if (value > 0 && value < 1000) {
          gR1Cache.hrv      = value;
          gR1Cache.hrvTs    = ts;
          gR1Cache.hrvRxMs  = rx;
          gR1Cache.hrvValid = true;
          g2HealthNoteSample(HEALTH_METRIC_HRV, value, ts);
        }
        break;
      case R1_CMD_SPO2:
        // SpO2 percent — 70..100 is the only realistic range.
        if (hasExtra && extra >= 70 && extra <= 100) {
          gR1Cache.spo2      = (uint8_t)extra;
          gR1Cache.spo2Ts    = ts;
          gR1Cache.spo2RxMs  = rx;
          gR1Cache.spo2Valid = true;
          g2HealthNoteSample(HEALTH_METRIC_SPO2, (int16_t)extra, ts);
        } else if (value >= 70 && value <= 100) {
          gR1Cache.spo2      = (uint8_t)value;
          gR1Cache.spo2Ts    = ts;
          gR1Cache.spo2RxMs  = rx;
          gR1Cache.spo2Valid = true;
          g2HealthNoteSample(HEALTH_METRIC_SPO2, (int16_t)value, ts);
        }
        break;
      case R1_CMD_TEMPERATURE:
        // Primary `value` is °C × 10 (codec hypothesis). Accept skin/body band.
        if (value >= 150 && value <= 450) {
          gR1Cache.tempTenths = value;
          gR1Cache.tempTs     = ts;
          gR1Cache.tempRxMs   = rx;
          gR1Cache.tempValid  = true;
          g2HealthNoteSample(HEALTH_METRIC_TEMP, value, ts);
        }
        break;
      default:
        break;
    }
    return;
  }

  // Daily history → Trends day series + thin live sparkline backfill.
  if (d.module == R1_MODULE_HEALTH && d.subCmd == R1_SUB_DAILY &&
      (d.cmd == R1_CMD_HEARTRATE || d.cmd == R1_CMD_HRV || d.cmd == R1_CMD_SPO2 ||
       d.cmd == R1_CMD_TEMPERATURE)) {
    R1DailyResult daily;
    if (r1ParseHealthDaily(d.payload, d.payloadLength, daily) && daily.count > 0) {
      if (daily.endTs != 0) {  // day-window end ≈ ring's clock at last sample
        sRingTsSeenRxMs = millis();  // rxMs first — see the point-frame note
        sRingTsSeen     = daily.endTs;
      }
      G2HealthMetric m = HEALTH_METRIC_HR;
      if (d.cmd == R1_CMD_HRV) m = HEALTH_METRIC_HRV;
      else if (d.cmd == R1_CMD_SPO2) m = HEALTH_METRIC_SPO2;
      else if (d.cmd == R1_CMD_TEMPERATURE) m = HEALTH_METRIC_TEMP;
      // Trends owns HR/HRV/SpO2 daily as a replaceable day series.
      if (m == HEALTH_METRIC_HR || m == HEALTH_METRIC_HRV || m == HEALTH_METRIC_SPO2) {
        g2HealthApplyTrendDaily(m, daily.values, daily.count, daily.startTs, daily.endTs);
      }
      g2HealthApplyDailyBackfill(m, daily.values, daily.count, daily.startTs, daily.endTs);
    }
    return;
  }

  // system/system/{deviceStatus, heartbeatPack} — byte 0 is the (likely)
  // battery percent; byte 1 is wear (0/1/2). See annotateDeviceStatus.
  if (d.module == R1_MODULE_SYSTEM && d.cmd == R1_CMD_SYSTEM &&
      (d.subCmd == R1_SUB_DEVICE_STATUS || d.subCmd == R1_SUB_HEARTBEAT) &&
      d.payloadLength >= 1) {
    const uint32_t rx = millis();
    uint8_t b = d.payload[0];
    if (b > 0 && b <= 100) {
      gR1Cache.battery      = b;
      gR1Cache.batteryRxMs  = rx;
      gR1Cache.batteryValid = true;
      g2HealthNoteSample(HEALTH_METRIC_BATTERY, (int16_t)b, 0);
    }
    if (d.payloadLength >= 2 && d.payload[1] <= 2) {
      ringNoteWear(d.payload[1], rx);
    }
    return;
  }

  // system/system/wearStatus — dedicated 1-byte wear probe.
  if (d.module == R1_MODULE_SYSTEM && d.cmd == R1_CMD_SYSTEM &&
      d.subCmd == R1_SUB_WEAR_STATUS && d.payloadLength >= 1 &&
      d.payload[0] <= 2) {
    ringNoteWear(d.payload[0], millis());
  }
}

// =============================================================================
// Forwarded-telemetry sink (called from G2_Glasses sid=0x90/0x91 RX)
// =============================================================================
// When the right temple's bridge is active (see ringbridge CLI), the temple
// connects to the ring directly and forwards RingDataPackage frames to us
// on sid=0x90 (UX_RING_ROW_DATA_ID). Decode the protobuf wrapper, walk into
// the nested RingRawData (field 4, wire-type 2), and copy each present field
// into gR1Cache so the same display / spoof / status code sees fresh data
// regardless of who's holding the ring's BLE link.
//
// Schema: docs/g2_proto/ring.proto. Field tags inside RingRawData:
//   1 battery, 2 chargeStates, 3 hr, 4 hrTs, 5 spo2, 6 spo2Ts,
//   7 hrv, 8 hrvTs, 9 temp, 10 tempTs, 11 actKcal, 12 actKcalTs,
//   13 allKcal, 14 allKcalTs, 15 steps, 16 stepsTs, 17 errorCode.
//
// We currently mirror battery / hr+hrTs / spo2+spo2Ts / hrv+hrvTs into the
// cache. Extend if the spoof builder ever grows new fields.
void g2RingNoteForwardedTelemetry(const uint8_t* pb, size_t pbLen) {
  if (!pb || pbLen == 0) return;

  // Walk the outer RingDataPackage to find field 4 (rawData, len-delim) or
  // field 3 (event, len-delim — currently unused; only logged).
  uint64_t cmdId = 0; bool hasCmdId = false;
  const uint8_t* raw = nullptr;
  size_t          rawLen = 0;
  size_t pos = 0;
  while (pos < pbLen) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(pb, pbLen, &pos, &field, &wire)) return;
    if (field == 1 && wire == G2_PB_WIRE_VARINT) {
      if (!g2PbReadVarint(pb, pbLen, &pos, &cmdId)) return;
      hasCmdId = true;
    } else if (field == 4 && wire == G2_PB_WIRE_LEN_DELIM) {
      uint64_t sl;
      if (!g2PbReadVarint(pb, pbLen, &pos, &sl)) return;
      if (pos + sl > pbLen) return;
      raw    = pb + pos;
      rawLen = (size_t)sl;
      pos += (size_t)sl;
    } else {
      if (!g2PbSkipField(pb, pbLen, &pos, wire)) return;
    }
  }
  if (!raw || rawLen == 0) return;
  // commandId == 2 means RAW_DATA per ring.proto — but the temple has been
  // observed to omit field 1 entirely on some firmware versions (proto3
  // default suppression), so we don't gate the parse on hasCmdId.
  (void)hasCmdId;

  // Parse the nested RingRawData. Only update the cache for fields whose
  // values fall in the realistic range — drops obviously-corrupt frames.
  uint64_t v;
  uint32_t now = (uint32_t)time(nullptr);
  size_t p = 0;
  while (p < rawLen) {
    uint32_t f; uint8_t w;
    if (!g2PbReadTag(raw, rawLen, &p, &f, &w)) return;
    if (w != G2_PB_WIRE_VARINT) {
      if (!g2PbSkipField(raw, rawLen, &p, w)) return;
      continue;
    }
    if (!g2PbReadVarint(raw, rawLen, &p, &v)) return;
    long sv = (long)(int32_t)v;  // RingRawData fields are int32
    const uint32_t rx = millis();
    switch (f) {
      case 1:  // battery
        if (sv > 0 && sv <= 100) {
          gR1Cache.battery      = (uint8_t)sv;
          gR1Cache.batteryRxMs  = rx;
          gR1Cache.batteryValid = true;
          g2HealthNoteSample(HEALTH_METRIC_BATTERY, (int16_t)sv, 0);
        }
        break;
      case 3:  // hr
        if (sv > 0 && sv < 250) {
          gR1Cache.hr      = (uint8_t)sv;
          gR1Cache.hrRxMs  = rx;
          gR1Cache.hrValid = true;
          g2HealthNoteSample(HEALTH_METRIC_HR, (int16_t)sv, now);
        }
        break;
      case 4:  // hrTs
        // Re-stamp rxMs with the stamp: proto3 omits zero fields, so a frame
        // can carry hrTs WITHOUT hr and would otherwise advance the ring
        // timestamp while leaving rxMs from an older frame — which makes the
        // age-adjusted projection in ringBestKnownEpoch() read too far ahead.
        if (gR1Cache.hrValid) {
          gR1Cache.hrTs   = (sv > 0) ? (uint32_t)sv : now;
          gR1Cache.hrRxMs = rx;
        }
        break;
      case 5:  // spo2
        if (sv >= 70 && sv <= 100) {
          gR1Cache.spo2      = (uint8_t)sv;
          gR1Cache.spo2RxMs  = rx;
          gR1Cache.spo2Valid = true;
          g2HealthNoteSample(HEALTH_METRIC_SPO2, (int16_t)sv, now);
        }
        break;
      case 6:  // spo2Ts — see case 4 on why rxMs moves with the stamp
        if (gR1Cache.spo2Valid) {
          gR1Cache.spo2Ts   = (sv > 0) ? (uint32_t)sv : now;
          gR1Cache.spo2RxMs = rx;
        }
        break;
      case 7:  // hrv
        if (sv > 0 && sv < 1000) {
          gR1Cache.hrv      = (int16_t)sv;
          gR1Cache.hrvRxMs  = rx;
          gR1Cache.hrvValid = true;
          g2HealthNoteSample(HEALTH_METRIC_HRV, (int16_t)sv, now);
        }
        break;
      case 8:  // hrvTs — see case 4 on why rxMs moves with the stamp
        if (gR1Cache.hrvValid) {
          gR1Cache.hrvTs   = (sv > 0) ? (uint32_t)sv : now;
          gR1Cache.hrvRxMs = rx;
        }
        break;
      case 9:  // temp (°C × 10 in RingRawData)
        if (sv >= 150 && sv <= 450) {
          gR1Cache.tempTenths = (int16_t)sv;
          gR1Cache.tempRxMs   = rx;
          gR1Cache.tempValid  = true;
          g2HealthNoteSample(HEALTH_METRIC_TEMP, (int16_t)sv, now);
        }
        break;
      case 10:  // tempTs — see case 4 on why rxMs moves with the stamp
        if (gR1Cache.tempValid) {
          gR1Cache.tempTs   = (sv > 0) ? (uint32_t)sv : now;
          gR1Cache.tempRxMs = rx;
        }
        break;
      default:
        break;  // chargeStates / kcal / steps not cached today
    }
  }

  DEBUG_G2F("[RING] cache (forwarded) batt=%s%u hr=%s%u hrv=%s%d spo2=%s%u temp=%s%d",
            gR1Cache.batteryValid ? "" : "?", (unsigned)gR1Cache.battery,
            gR1Cache.hrValid      ? "" : "?", (unsigned)gR1Cache.hr,
            gR1Cache.hrvValid     ? "" : "?", (int)gR1Cache.hrv,
            gR1Cache.spo2Valid    ? "" : "?", (unsigned)gR1Cache.spo2,
            gR1Cache.tempValid    ? "" : "?", (int)gR1Cache.tempTenths);
}

// =============================================================================
// Ring envelope decoding (no-write; receive-only)
// =============================================================================
// We log each incoming packet with a one-line header (decoded fields), a
// raw payload hex dump, and a speculative parsed annotation when we have
// one for that opcode. See System_R1_Protocol.{h,cpp} for the wire spec
// and per-opcode parsers (provenance documented in
// docs/g2_proto/R1_RE_Reference.h).

// Decode and print a single ring notify frame using the real R1 wire format
// (See System_R1_Protocol.h for the full envelope spec.) `data` is the raw
// BLE characteristic value — the ring writes one logical frame per notify
// in our usage so far.
//
static void ringDumpFrame(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;
  gRing.packetsReceived++;

  // Ring1Error short-frame fast-path. The ring emits this 6-byte form on
  // some malformed-input rejections instead of a full envelope: a single
  // transfer-type byte (0x00) followed by the offending request's CRC32
  // and a 1-byte errorCode. r1Decode rejects anything < 17 B, so without
  // this branch the meaning is lost (only visible as raw bytes under
  // `ringverbose`). Format observed in the community RE decoder at
  // docs/FlutterApp-main/lib/src/protocol/r1_messages.dart:390 with a
  // pinned test fixture `00 D4 C9 BA 70 07` → errorCode=0x07.
  if (len == 6 && data[0] == 0x00) {
    const uint32_t crc32 = (uint32_t)data[1]
                         | ((uint32_t)data[2] << 8)
                         | ((uint32_t)data[3] << 16)
                         | ((uint32_t)data[4] << 24);
    const uint8_t errorCode = data[5];
    DEBUG_G2F("[RING] RX Ring1Error errorCode=0x%02X crc32=0x%08lX",
              (unsigned)errorCode, (unsigned long)crc32);
    return;
  }

  R1Decoded d;
  if (r1Decode(data, len, d)) {
    const char* mod = r1ModuleName(d.module);
    const char* cmd = r1CmdName(d.module, d.cmd);
    const char* sub = r1SubCmdName(d.module, d.cmd, d.subCmd);
    // CRC16 mismatch is expected on every ring→phone frame (the firmware
    // emits a wrong CRC16 — see R1Decoded crc16Valid comment). Only flag
    // CRC?! when CRC32 (the real integrity check) actually fails.
    DEBUG_G2F("[RING] RX %s/%s/%s ser=%u status=%s/%s/%s pLen=%u%s%s",
              mod, cmd, sub,
              (unsigned)d.serial,
              r1StatusTypeName(d.statusType),
              r1StatusMethodName(d.statusMethod),
              r1StatusAckName(d.statusAck),
              (unsigned)d.payloadLength,
              d.crc32Valid ? "" : " CRC32?!",
              d.modelLengthValid ? "" : " LEN?!");

    // Hex-dump non-trivial payloads inline so we can decode unknown
    // health responses by eye while we RE the layout. Cap at 64 bytes —
    // larger frames already get the full hex dump from the verbose path.
    if (d.payloadLength > 0) {
      const size_t showMax = 64;
      const size_t show = d.payloadLength > showMax ? showMax : d.payloadLength;
      char pbuf[3 * showMax + 4];
      size_t off = 0;
      for (size_t i = 0; i < show && off + 3 < sizeof(pbuf); i++) {
        off += snprintf(pbuf + off, sizeof(pbuf) - off, "%02X ", d.payload[i]);
      }
      if (off > 0) pbuf[off - 1] = '\0';
      DEBUG_G2F("[RING]   payload[%u]=[%s%s]",
                (unsigned)d.payloadLength, pbuf,
                d.payloadLength > showMax ? " ..." : "");

      // Speculative parser — see r1AnnotatePayload() in System_R1_Protocol.cpp
      // for the per-opcode hypotheses and confidence notes. Returns 0 (skip
      // the log line) for opcodes without a parser yet.
      char abuf[256];
      size_t alen = r1AnnotatePayload(d, abuf, sizeof(abuf));
      if (alen > 0) {
        DEBUG_G2F("[RING]   parsed: %s", abuf);
      }
    }

    // Mirror decoded HR/HRV/SpO2/battery into the spoof-push cache. Cheap —
    // just a few range-checked writes when the opcode matches. See
    // ringExtractTelemetryCache() comments for confidence notes per metric.
    ringExtractTelemetryCache(d);
  } else {
    DEBUG_G2F("[RING] RX undecodeable len=%u (<17 B for envelope)",
              (unsigned)len);
  }

  if (!gRingDumpVerbose) return;
  // Hex dump capped at 64 bytes — typical ring packets are <40 B; the
  // largest captured fixture (nvRecover) is ~80 B. Truncated lines get
  // a "…" suffix so the log doesn't grow unbounded on a future surprise.
  const size_t showMax = 64;
  const size_t show = len > showMax ? showMax : len;
  char buf[3 * showMax + 4];
  size_t off = 0;
  for (size_t i = 0; i < show && off + 3 < sizeof(buf); i++) {
    off += snprintf(buf + off, sizeof(buf) - off, "%02X ", data[i]);
  }
  if (off > 0) buf[off - 1] = '\0';
  DEBUG_G2F("[RING] RX bytes=[%s%s]", buf, len > showMax ? " ..." : "");
}

// Notify callback shim — Arduino BLE hands us (char*, data, len, isNotify).
static void ringNotifyThunk(BLERemoteCharacteristic* /*c*/, uint8_t* data,
                            size_t len, bool /*isNotify*/) {
  ringDumpFrame(data, len);
}

// =============================================================================
// R1 standard setup sequence
// =============================================================================
// Port of buildStandardSetupSequence() in
// docs/FlutterApp-main/lib/src/services/r1_manager.dart. Three messages,
// fired ~200 ms apart (1 s after auth) over the ring writeChar:
//
//   1. pairAuth   — payload [0x01]. Unlocks the ring's notify stream. (No
//                   server-issued pkey is required, despite earlier notes
//                   in our docs to the contrary.)
//   2. systemTime — sets the ring clock to the ESP's wall time + tz offset
//                   in MINUTES (NOT quarter-hours like G2 TIME_SYNC). When
//                   the ESP is dark (no NTP/RTC yet) we first wait a bounded
//                   window for ring-clock adoption (see "Ring-clock custody"
//                   above) so this frame echoes the ring's own time instead
//                   of stomping it back to 1970. The frame itself is always
//                   sent — the RE'd sequence is never thinned.
//   3. advStart   — payload is the G2 right-temple MAC, byte-reversed. The
//                   FlutterApp author notes the MAC may not actually
//                   matter; we send it anyway for parity. Falls back to
//                   six zero bytes if no glasses are connected.
//
// Sends are write-without-response over the writeChar. We do NOT wait for
// acks here; the FlutterApp does, but for our use case the simple time-
// gated sequence is enough — the ring acks asynchronously and our notify
// handler will log them as they arrive.
//
// Returns true if all three writes were dispatched. False on missing
// writeChar (shouldn't happen — caller has just verified service lookup).
static bool ringRunStandardSetup() {
  if (!gRing.writeChar) {
    DEBUG_G2F("[RING] standard setup skipped: no writeChar");
    return false;
  }

  gR1Encoder.resetSerial();
  // Fresh link: drop any custody state a prior link left behind (step 2 below
  // restamps the push projection). Teardown clears this too, but a connect
  // that never saw a clean disconnect must not inherit stale facts.
  ringClockCustodyReset();

  // 1. pairAuth — single-byte payload [0x01]. Ring acks then begins
  //    pushing telemetry on the notify char.
  R1Frame auth = gR1Encoder.buildPairAuth();
  if (auth.length == 0) {
    DEBUG_G2F("[RING] standard setup: failed to encode pairAuth");
    return false;
  }
  if (!ringWrite(auth.bytes, auth.length)) {
    DEBUG_G2F("[RING] standard setup: pairAuth write failed");
    return false;
  }
  DEBUG_G2F("[RING] TX pairAuth ser=%u (%u B) — unlocking notify stream",
            (unsigned)auth.serial, (unsigned)auth.length);
  vTaskDelay(pdMS_TO_TICKS(1000));

  // Dark-clock window: our clock has no real time, but the ring's may — its
  // battery-backed clock survives our reboots. This ring never volunteers
  // telemetry unpolled (HW-verified 2026-07-29), so solicit the latest HR
  // point — a standard query used constantly in normal operation; the
  // response carries the ring's clock regardless of wear or vitals validity
  // (sRingTsSeen). g2RingTimeSyncTick() (main loop) adopts the timestamp
  // and flips Clock::isSynced(); the mandatory systemTime frame below then
  // echoes the ring's own clock instead of stomping it with a 1970 epoch
  // (which poisons the ring's daily-history bucketing — the Trends data
  // source). Lapses silently after ~3 s total: we then send our dark epoch
  // exactly as we always did, and the tick's corrective push repairs the
  // ring once real time arrives. The ritual's own frames stay present and
  // in order; the probe only shifts their serial numbers on dark boots.
  if (!Clock::isSynced()) {
    R1Frame probe = gR1Encoder.buildHealthQuery(R1_CMD_HEARTRATE, R1_SUB_POINT);
    if (probe.length > 0 && ringWrite(probe.bytes, probe.length)) {
      DEBUG_G2F("[RING] dark-clock window: TX hr/point probe ser=%u",
                (unsigned)probe.serial);
    }
    for (int i = 0; i < 20 && !Clock::isSynced(); i++) {
      vTaskDelay(pdMS_TO_TICKS(100));
      if (i == 9 && !Clock::isSynced()) {  // one mid-window retry
        probe = gR1Encoder.buildHealthQuery(R1_CMD_HEARTRATE, R1_SUB_POINT);
        if (probe.length > 0) ringWrite(probe.bytes, probe.length);
      }
    }
    DEBUG_G2F("[RING] dark-clock window: %s",
              Clock::isSynced() ? "adopted ring clock — echoing it back"
                                : "no dated sample seen — sending dark epoch");
  }

  // 2. systemTime — current epoch, tz=0 (UTC). The ring uses the timezone
  //    field to label its history-mode timestamps; for the live telemetry
  //    stream we care about (HR/HRV/temperature pushes) the timezone is
  //    irrelevant. We deliberately keep sending 0 even though
  //    Clock::tzOffsetMinutes() is now available everywhere: the daily
  //    windows Trends reads (startTs/endTs) are bucketed by the ring using
  //    this field, so changing it would silently shift every historical
  //    day boundary. Epoch stays UTC; only the display layer localizes.
  time_t now = time(nullptr);
  R1Frame timeFrame = gR1Encoder.buildSyncTime(0, (uint32_t)now);
  if (timeFrame.length > 0) {
    if (ringWrite(timeFrame.bytes, timeFrame.length)) {
      sLastPushedEpoch = (uint32_t)now;
      sLastPushedAtMs  = (uint32_t)millis();
      DEBUG_G2F("[RING] TX systemTime ser=%u tz=0min epoch=%lu%s",
                (unsigned)timeFrame.serial, (unsigned long)now,
                Clock::isSynced() ? "" : " (dark — corrective push pending)");
    }
  }
  vTaskDelay(pdMS_TO_TICKS(200));

  // 3. advStart — G2 right-temple MAC in BLE address order (NOT reversed).
  // Verified against FlutterApp r1_manager.dart:262 + Ring_Bridge_Sequence.h
  // Step 2.3 — both confirm "IN ORDER (NOT reversed)". An earlier version of
  // this firmware reversed the bytes, which made the ring directed-advertise
  // toward a non-existent MAC and caused the temple's bond attempts to fail
  // with connRet=8 (fail-terminal) every cycle.
  uint8_t mac[6] = {0};
  if (g2GetRightTempleMac(mac)) {
    DEBUG_G2F("[RING] standard setup: advStart target=%02X:%02X:%02X:%02X:%02X:%02X",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    DEBUG_G2F("[RING] standard setup: no right-temple MAC available — "
              "sending advStart with zero target (ring may ignore it)");
  }
  R1Frame advFrame = gR1Encoder.buildAdvStart(mac);
  if (advFrame.length > 0) {
    if (ringWrite(advFrame.bytes, advFrame.length)) {
      DEBUG_G2F("[RING] TX advStart ser=%u (%u B)",
                (unsigned)advFrame.serial, (unsigned)advFrame.length);
    }
  }
  vTaskDelay(pdMS_TO_TICKS(200));

  DEBUG_G2F("[RING] standard setup complete — ring should now emit telemetry "
            "(HR / HRV / temperature / activity) on the notify char");
  sRingSetupDone = true;  // opens the tick's corrective-push gate
  return true;
}

// =============================================================================
// Client callbacks
// =============================================================================

class RingClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* /*c*/) override {
    DEBUG_G2F("[RING] BLE onConnect callback fired");
  }
  void onDisconnect(BLEClient* /*c*/) override {
    const bool wasConnected = gRing.connected;
    DEBUG_G2F("[RING] BLE onDisconnect — connected-was=%d", wasConnected ? 1 : 0);
    // Drop all GATT handles; client object may still exist until reconnect
    // replaces it, but must not be used for writes after this.
    gRing.connected   = false;
    gRing.writeChar   = nullptr;
    gRing.notifyChar  = nullptr;
    gRing.clientStale = true;
    gR1Encoder.resetSerial();
    // No corrective time push into a dead link, and no ring timestamp from
    // THIS link surviving into the next one (this is the common teardown —
    // link lost — so it must clear as much as ringClearGattPointers does).
    ringClockCustodyReset();
    if (wasConnected) {
      BROADCAST_PRINTF("[RING] Dropped BLE link — ring is no longer connected");
      ringPushStatusEvent("disconnect");
      systemEventPost(SYSEVT_RING_DISCONNECTED,
                      gRingDeviceName.length() > 0 ? gRingDeviceName.c_str() : "R1",
                      gRingDeviceAddress.length() > 0 ? gRingDeviceAddress.c_str() : nullptr);
      sRingWearPosted = 0;
      blePeerNoteLinkLost(BLE_PEER_R1_RING);
    }
  }
};

// =============================================================================
// Ring-only scan
// =============================================================================
// The shared G2 scan callback (G2_Glasses.cpp) also stashes ring adverts as a
// side effect, but that scan early-terminates the moment both temples are
// found. The R1's slower advert cycle (battery-saving) often falls outside
// that ~3-5s window, so the ring goes undetected and `ringconnect` bails.
//
// This dedicated scan uses a separate callback that ONLY watches for
// "EVEN R1_XXXXXX" adverts. It runs for the full requested timeout (no
// early termination tied to glasses) so the ring has more chances to emit.
//
// Caveat: scanning while the glasses are connected splits BLE-controller
// time between scan and the active connections, lowering scan duty-cycle.
// If the ring stays elusive: physically tap it to wake, check battery, or
// run a longer scan (e.g. `ringscan 60`). If still nothing, the ring may
// already be paired with another central (phone) and refusing connectable
// advertising.

class RingScanCallbacks : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (!advertisedDevice.haveName()) return;
    String name = advertisedDevice.getName().c_str();
    // Inline mirror of classifyRingName() in G2_Glasses.cpp:
    //   /^EVEN\s+R1_[0-9A-F]{6}$/i
    if (name.length() < 10) return;
    const char* s = name.c_str();
    if (strncasecmp(s, "EVEN", 4) != 0) return;
    s += 4;
    if (*s != ' ' && *s != '\t') return;
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "R1_", 3) != 0) return;
    s += 3;
    int hex = 0;
    while (*s && hex < 7) {
      if (!isxdigit((unsigned char)*s)) return;
      s++; hex++;
    }
    if (hex != 6 || *s != '\0') return;

    if (gRingAdvertisedDevice) return;  // already stashed (perhaps by G2 scan)
    gRingAdvertisedDevice = new BLEAdvertisedDevice(advertisedDevice);
    gRingDeviceName       = name;
    gRingDeviceAddress    = advertisedDevice.getAddress().toString().c_str();
    gRingScanFound        = true;
    DEBUG_G2F("[RING] ringscan: Found %s @ %s (RSSI %d) — stashed",
              name.c_str(), gRingDeviceAddress.c_str(),
              advertisedDevice.getRSSI());
    BLEDevice::getScan()->stop();  // got it; bail out of remaining timeout
  }
};
static RingScanCallbacks gRingScanCallbacks;

bool g2RingScan(uint32_t timeoutSec) {
  if (!g2RingInit()) return false;
  if (gRingAdvertisedDevice) {
    DEBUG_G2F("[RING] ringscan: advert already stashed (%s @ %s); skipping",
              gRingDeviceName.c_str(), gRingDeviceAddress.c_str());
    return true;
  }
  if (timeoutSec == 0) timeoutSec = 1;
  if (timeoutSec > 300) timeoutSec = 300;

  DEBUG_G2F("[RING] ringscan: scanning for EVEN R1_* (timeout=%us)",
            (unsigned)timeoutSec);

  BLEScan* scan = BLEDevice::getScan();
  if (!scan) {
    DEBUG_G2F("[RING] ringscan: BLEDevice::getScan() returned null");
    return false;
  }

  scan->setAdvertisedDeviceCallbacks(&gRingScanCallbacks);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  // BLEScan::start(durationSec, continueScanning=false) blocks the calling
  // task until the scan completes — either by `timeoutSec` elapsing or by
  // a callback calling stop() (which we do on first ring match).
  scan->start(timeoutSec, /*continueScanning*/ false);
  scan->clearResults();

  if (gRingAdvertisedDevice) {
    BROADCAST_PRINTF("[RING] ringscan: found %s @ %s",
                     gRingDeviceName.c_str(), gRingDeviceAddress.c_str());
    return true;
  }

  DEBUG_G2F("[RING] ringscan: timed out after %us — ring not advertising. "
            "Try: tap the ring to wake it, check battery, move it closer, "
            "or run 'ringscan 60' for a longer window.",
            (unsigned)timeoutSec);
  return false;
}

// =============================================================================
// Connect flow
// =============================================================================

// RAII guard: drop glasses connection priority to BALANCED for the
// duration of a ring connect attempt, then restore HIGH on exit. Both
// glasses links at HIGH priority (~12 ms intervals) saturate the single-
// radio BLE controller; trying to add a 3rd connection (the ring) under
// that load reliably times out (`Unknown ESP_ERR error` after 30s) and
// can starve an existing temple link to supervision-timeout (rsn=0x8).
// BALANCED (~50 ms intervals) gives the controller ~4× more idle time
// to scan and complete the new connection. Restored on every exit path
// — destructor fires regardless of how ringDoConnect returns.
// Refcounted: asks the arbiter in G2_Glasses.cpp for BALANCED and releases on
// every exit path. Nesting-safe — if something else is also holding BALANCED,
// whichever guard leaves first no longer drags the links back to HIGH under the
// other one. (That was a real defect while both guards set absolute values.)
struct GlassesPriorityGuard {
  GlassesPriorityGuard()  { g2ConnPriRequestBalanced("ring-connect"); }
  ~GlassesPriorityGuard() { g2ConnPriReleaseBalanced("ring-connect"); }
  GlassesPriorityGuard(const GlassesPriorityGuard&) = delete;
  GlassesPriorityGuard& operator=(const GlassesPriorityGuard&) = delete;
};

// `savedMac`: when non-empty, do a directed connect to that MAC without
// requiring a prior scan-cached gRingAdvertisedDevice. Used by the boot
// auto-reconnect path. When empty, behaves as before (uses the cached
// advertisement from the most recent G2 scan).
// Promoted from `static` to a public helper as part of Group B so the
// unified BLE-connect worker (in G2_Glasses.cpp) can dispatch RING_*
// jobs to it. Internal callers (the now-deleted ring*TaskBody functions)
// are gone; only the worker calls this.
bool ringPerformConnect(const String& savedMac /* = String() */) {
  // Drop glasses to BALANCED for the entire connect attempt. Auto-restored
  // on any return path. Cheap no-op if no glasses are connected.
  GlassesPriorityGuard prio_guard;

  const bool useSavedMac = (savedMac.length() > 0);
  if (!useSavedMac && !gRingAdvertisedDevice) {
    // No prior scan stashed an advert. Self-scan rather than bailing —
    // matches the doc comment in G2_Ring.h:75 ("Scan for the ring advert
    // or use one already discovered"). The shared G2 scan early-terminates
    // on glasses-found which often misses the ring's slower advert cycle.
    DEBUG_G2F("[RING] connect: no advertisedDevice stashed; running "
              "dedicated ring scan (15s)...");
    if (!g2RingScan(15) || !gRingAdvertisedDevice) {
      DEBUG_G2F("[RING] connect: ring not visible — try 'ringscan 60' with "
                "the ring close + woken (tap it) first, then retry "
                "'ringconnect'");
      return false;
    }
  }
  if (gRing.connected) {
    DEBUG_G2F("[RING] connect: already connected");
    return true;
  }

  if (useSavedMac) {
    DEBUG_G2F("[RING] Connecting by saved MAC %s (heap=%u)",
              savedMac.c_str(), (unsigned)ESP.getFreeHeap());
  } else {
    DEBUG_G2F("[RING] Connecting to %s @ %s (heap=%u)",
              gRingDeviceName.c_str(), gRingDeviceAddress.c_str(),
              (unsigned)ESP.getFreeHeap());
  }

  // Replace stale client from a previous unexpected drop. Same pattern
  // as the glasses path — Arduino BLE's BLEClient doesn't reliably survive
  // peer-initiated disconnects.
  if (gRing.client && gRing.clientStale) {
    DEBUG_G2F("[RING] Replacing stale BLEClient from prior drop");
    // Nulling without delete orphans the client and its cached GATT tree
    // (measured ~10-14 KB per drop on the glasses path) — but freeing it is
    // only legal once nothing can still reach it:
    //
    // Gate 1 — the library. After a MANUAL ringdisconnect, clientStale was
    // set by us, not by onDisconnect, so the async DISCONNECT_EVT (which
    // unregisters the gattc app and pulls the client out of BLEDevice's
    // routing map) may still be in flight — the R1's power-save conn
    // interval makes 50-500 ms normal. That same handler drops getGattcIf()
    // to ESP_GATT_IF_NONE; poll for it, bounded.
    //
    // Gate 2 — app tasks. ringWriteLocked holds writeMutex across its
    // writeChar dereference (the char lives inside this client's GATT
    // cache); 2 s dwarfs a healthy sender's hold (one writeValue).
    //
    // If either gate fails, leak the object rather than free memory the BLE
    // dispatcher or a wedged writer may still touch.
    BLEClient* dead = gRing.client;
    gRing.client    = nullptr;
    bool libReleased = false;
    for (uint32_t waited = 0; ; waited += 50) {
      if (dead->getGattcIf() == ESP_GATT_IF_NONE) { libReleased = true; break; }
      if (waited >= 3000) break;
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    const bool locked = gRing.writeMutex &&
        xSemaphoreTake(gRing.writeMutex, pdMS_TO_TICKS(2000)) == pdTRUE;
    if (libReleased && locked) {
      // One extra poll interval: gattc_if drops mid-DISCONNECT-case on the
      // BTC task, a hair before the routing-map removal and onDisconnect
      // callback finish with the object (cross-core, so genuinely parallel).
      vTaskDelay(pdMS_TO_TICKS(50));
      delete dead;
    } else {
      DEBUG_G2F("[RING] Stale client NOT freed (libReleased=%d locked=%d) — "
                "leaked by design", (int)libReleased, (int)locked);
    }
    if (locked) xSemaphoreGive(gRing.writeMutex);
    gRing.clientStale = false;
  }
  if (!gRing.client) {
    gRing.client = BLEDevice::createClient();
    if (!gRing.client) {
      DEBUG_G2F("[RING] BLEDevice::createClient() returned null");
      return false;
    }
    // Static: the callbacks keep no per-connection state (they write gRing
    // globals), ~BLEClient never frees this pointer, and a heap `new` per
    // replacement cycle just leaked alongside the client.
    static RingClientCallbacks sRingClientCallbacks;
    gRing.client->setClientCallbacks(&sRingClientCallbacks);
  }

  const uint32_t t0 = millis();
  bool connOk;
  if (useSavedMac) {
    // Direct address connect — Arduino BLE supports this without a prior
    // advertisement scan. The peer must be advertising and in range.
    //
    // Address type matters: BLEClient::connect defaults to PUBLIC, but the
    // R1 ring uses a Random Static address (BT spec: top two bits of the
    // MSB are 0b11, i.e. first byte ≥ 0xC0). Asking the controller to open
    // a Public link to a Random address always times out at 30s. The
    // cached-advert path doesn't hit this because BLEAdvertisedDevice
    // carries the discovered address type.
    BLEAddress addr(savedMac.c_str());
    uint8_t addrType = BLE_ADDR_TYPE_PUBLIC;
    {
      unsigned msb = 0;
      if (sscanf(savedMac.c_str(), "%2x", &msb) == 1 && (msb & 0xC0) == 0xC0) {
        addrType = BLE_ADDR_TYPE_RANDOM;
      }
    }
    // Explicit timeout: the default is portMAX_DELAY, which turned a lost
    // OPEN event into a forever-wedged connect worker (root-cause candidate
    // for the 2026-07-28 2h no-reconnect window). 35s clears the ring's
    // normal ~30s direct-connect timeout with margin.
    connOk = gRing.client->connect(addr, addrType, 35000);
    if (connOk) {
      // Populate the cached descriptors so subsequent disconnect / status
      // paths print sensible names.
      gRingDeviceAddress = savedMac;
      if (gRingDeviceName.length() == 0) gRingDeviceName = "saved-ring";
    }
  } else {
    // Same 35s bound as the saved-MAC path (the plain connect(device)
    // overload hardwires the portMAX_DELAY default).
    connOk = gRing.client->connectTimeout(gRingAdvertisedDevice, 35000);
  }
  // Classify BOTH overloads above (the saved-MAC path takes a BLEAddress, so
  // bleConnectWatched's advertised-device wrapper can't cover it — and saved
  // MAC is exactly the unattended auto-reconnect path where a wedge builds).
  bleNoteConnectOutcome(connOk, millis() - t0, "RING");
  if (!connOk) {
    const uint32_t dt = millis() - t0;
    BROADCAST_PRINTF("[RING] BLE connect FAILED after %u ms (%s)",
                     (unsigned)dt,
                     useSavedMac ? savedMac.c_str() : "scan target");
    ringNoteConnectFailure("link", dt);
    return false;
  }
  DEBUG_G2F("[RING] BLE connect OK in %u ms", (unsigned)(millis() - t0));

  // Prefer the same high local ATT MTU as glasses (G2_BLE_LOCAL_MTU_PREF).
  // Never setMTU(64) — that lowered the process-global local MTU and broke
  // subsequent glasses discovery (PDU size: 64). Peer may still negotiate
  // this link down to ~64; that value lands in gRing.mtu.
  gRing.mtu = bleNegotiateConnMtu(gRing.client, G2_BLE_LOCAL_MTU_PREF, 1000, "RING");

  DEBUG_G2F("[RING] Looking up service %s", G2RING_SERVICE_UUID);
  BLERemoteService* svc = gRing.client->getService(BLEUUID(G2RING_SERVICE_UUID));
  if (!svc) {
    BROADCAST_PRINTF("[RING] Connect FAILED — ring service not found");
    ringNoteConnectFailure("service", millis() - t0);
    DEBUG_G2F("[RING] Service %s NOT FOUND (listing all services below)",
              G2RING_SERVICE_UUID);
    auto* services = gRing.client->getServices();
    if (services) {
      for (const auto& entry : *services) {
        DEBUG_G2F("[RING]   svc: %s", entry.first.c_str());
      }
    }
    gRing.client->disconnect();
    return false;
  }
  DEBUG_G2F("[RING] Service found, getting characteristics");

  gRing.writeChar  = svc->getCharacteristic(BLEUUID(G2RING_CHAR_WRITE_UUID));
  gRing.notifyChar = svc->getCharacteristic(BLEUUID(G2RING_CHAR_NOTIFY_UUID));
  DEBUG_G2F("[RING] writeChar=%p notifyChar=%p", gRing.writeChar, gRing.notifyChar);

  if (!gRing.notifyChar) {
    BROADCAST_PRINTF("[RING] Connect FAILED — notify characteristic not found");
    ringNoteConnectFailure("notify-char", millis() - t0);
    DEBUG_G2F("[RING] Notify char %s NOT FOUND (listing all chars):",
              G2RING_CHAR_NOTIFY_UUID);
    auto* chars = svc->getCharacteristics();
    if (chars) {
      for (const auto& entry : *chars) {
        DEBUG_G2F("[RING]   char: %s", entry.first.c_str());
      }
    }
    gRing.client->disconnect();
    return false;
  }
  DEBUG_G2F("[RING] Chars: write.canWriteNR=%d notify.canNotify=%d canIndicate=%d",
            gRing.writeChar  ? gRing.writeChar->canWriteNoResponse() : 0,
            gRing.notifyChar ? gRing.notifyChar->canNotify()         : 0,
            gRing.notifyChar ? gRing.notifyChar->canIndicate()       : 0);

  if (gRing.notifyChar->canNotify()) {
    DEBUG_G2F("[RING] Subscribing to notifications on %s", G2RING_CHAR_NOTIFY_UUID);
    gRing.notifyChar->registerForNotify(ringNotifyThunk);
  } else {
    DEBUG_G2F("[RING] WARN: notifyChar cannot notify — no incoming data");
  }

  gRing.connected      = true;
  // Belt for the invalidate path: g2RingInvalidateLink nulls the client and
  // leaves clientStale=true with nothing left to reap, so the fresh-client
  // branch above never runs the reset — without this line the next connect
  // would be mute (all TX gates check clientStale).
  gRing.clientStale    = false;
  gRing.connectedSince = millis();
  sRingConnFailStreak  = 0;

  // pairAuth → systemTime → advStart unlocks the notify/telemetry stream
  // (a bare subscribe without pairAuth stays muted).
  ringRunStandardSetup();

  BROADCAST_PRINTF("[RING] Connected to %s (auth + time sync sent)",
                   gRingDeviceName.c_str());
  ringPushStatusEvent("connect-ok");
  // Mirror G2_Glasses: SSE status push + typed bus event for automations /
  // `events` / system_events consumers. subject=name, detail=mac.
  systemEventPost(SYSEVT_RING_CONNECTED,
                  gRingDeviceName.length() > 0 ? gRingDeviceName.c_str() : "R1",
                  gRingDeviceAddress.length() > 0 ? gRingDeviceAddress.c_str() : nullptr);

  // Persist ring MAC into the BLE peer registry. bleSavePeerMac is a
  // no-op when the value already matches — auto-reconnect is gated
  // separately by the peer's autoReconnect flag.
  if (gRingDeviceAddress.length() > 0) {
    bleSavePeerMac(BLE_PEER_R1_RING, gRingDeviceAddress);
    blePeerNoteLinkUp(BLE_PEER_R1_RING);
  }
  return true;
}

// Ring connect dispatch:
// All three connect entry points (scan, saved, MAC) flow through the
// unified BLE-connect worker (see `g2SubmitBleConnect` in G2_Glasses.h).
// The worker dispatches the BleConnectJob `kind` to ringPerformConnect()
// above:
//   - BleConnectKind::RING_SCAN   — discover by name, connect first match
//   - BleConnectKind::RING_SAVED  — read saved MAC + wait for glasses,
//                                   then 3 s settle before connect
//   - BleConnectKind::RING_MAC    — connect to MAC supplied in the job's
//                                   payload (no static handoff)

// =============================================================================
// Public API
// =============================================================================

bool g2RingInit() {
  if (gRing.initialized) return true;
  bleCentralTxInit();
  if (!gRing.writeMutex) {
    gRing.writeMutex = xSemaphoreCreateMutex();
  }
  gRing.initialized = true;

  // Register with the peer registry. ringPeerSpec is a file-static (see
  // top of this file); registration just publishes a stable pointer.
  bleRegisterPeer(ringPeerSpec);

  // Cheap one-time validation that our CRC ports match the FlutterApp's
  // captured wire bytes. Logs PASS/FAIL inline; on FAIL the encoder is
  // unsafe to use against real hardware (we'd send malformed frames).
  r1ProtocolSelfTest();

  DEBUG_G2F("[RING] Module initialised (full R1 protocol — auth + telemetry)");
  return true;
}

bool g2RingConnect() {
  if (!g2RingInit()) return false;
  if (!ringConnectGateOpen()) return false;
  // Group B: submit to the unified worker. Active flag flips true here so
  // duplicate g2RingConnect* calls reject before we ever hit the queue;
  // the worker's dispatch clears it after ringPerformConnect returns.
  gRingConnectTaskActive = true;
  gRingConnectTaskSinceMs = millis();
  BleConnectJob job{};
  job.kind = BleConnectKind::RING_SCAN;
  if (!g2SubmitBleConnect(job)) {
    BROADCAST_PRINTF("[RING] Connect submit FAILED (scan) — worker queue full or BLE not ready");
    ringNoteConnectFailure("submit", 0);
    gRingConnectTaskActive = false;
    return false;
  }
  return true;
}

bool g2RingConnectSaved() {
  if (!g2RingInit()) return false;
  if (!ringConnectGateOpen()) return false;
  if (gBlePeerData[BLE_PEER_R1_RING].mac1.length() == 0) {
    DEBUG_G2F("[RING] g2RingConnectSaved: no saved MAC, skipping");
    return false;
  }
  gRingConnectTaskActive = true;
  gRingConnectTaskSinceMs = millis();
  BleConnectJob job{};
  job.kind = BleConnectKind::RING_SAVED;
  if (!g2SubmitBleConnect(job)) {
    BROADCAST_PRINTF("[RING] Connect submit FAILED (saved) — worker queue full or BLE not ready");
    ringNoteConnectFailure("submit", 0);
    gRingConnectTaskActive = false;
    return false;
  }
  return true;
}

bool g2RingConnectMac(const String& mac) {
  if (!g2RingInit()) return false;
  if (!ringConnectGateOpen()) return false;
  String m = mac;
  m.trim();
  // Loose validation — full BLEAddress parsing happens inside ringPerformConnect.
  // Just reject obviously-wrong inputs here (need at least "aa:bb:cc:dd:ee:ff"
  // = 17 chars; BleConnectJob.mac is a 18-byte buffer including NUL).
  if (m.length() < 17 || m.length() > 17) {
    DEBUG_G2F("[RING] connect-mac: invalid MAC '%s' (need aa:bb:cc:dd:ee:ff)",
              m.c_str());
    return false;
  }
  gRingConnectTaskActive = true;
  gRingConnectTaskSinceMs = millis();
  BleConnectJob job{};
  job.kind = BleConnectKind::RING_MAC;
  // 17 chars + NUL fits exactly in the 18-byte mac buffer.
  memcpy(job.mac, m.c_str(), 17);
  job.mac[17] = '\0';
  if (!g2SubmitBleConnect(job)) {
    BROADCAST_PRINTF("[RING] Connect submit FAILED (mac) — worker queue full or BLE not ready");
    ringNoteConnectFailure("submit", 0);
    gRingConnectTaskActive = false;
    return false;
  }
  return true;
}

void g2RingInvalidateLink() {
  // No GATT calls — stack may already be dead.
  const bool wasConnected = gRing.connected;
  ringTxQClear();
  // Drop the pointer: BLEDevice::deinit frees/invalidates the object, so
  // keeping it for reaping would leave a dangling pointer.
  ringClearGattPointers(/*dropClientPtr=*/true);
  gR1Encoder.resetSerial();
  if (wasConnected) {
    DEBUG_G2F("[RING] Link invalidated (BLE stack teardown)");
    ringPushStatusEvent("disconnect");
    systemEventPost(SYSEVT_RING_DISCONNECTED,
                    gRingDeviceName.length() > 0 ? gRingDeviceName.c_str() : "R1",
                    gRingDeviceAddress.length() > 0 ? gRingDeviceAddress.c_str() : nullptr);
    sRingWearPosted = 0;
    blePeerNoteLinkLost(BLE_PEER_R1_RING);
  }
}

void g2RingDisconnect() {
  // After BLEDevice::deinit the client pointer is dangling — never touch it.
  if (gRing.client && !gRing.clientStale && gRing.connected) {
    DEBUG_G2F("[RING] Disconnecting");
    gRing.client->disconnect();  // onDisconnect clears chars when stack is live
  }
  ringTxQClear();
  // Keep the client object for the next connect's stale-replacement reap:
  // deleting it here would race the async DISCONNECT_EVT still headed for it,
  // and the old null-without-delete leaked it (~10-14 KB) AND stranded
  // clientStale=true forever — the delete branch was the only reset, so a
  // manual ringdisconnect → ringconnect came up connected-but-mute (every TX
  // gate checks clientStale).
  ringClearGattPointers(/*dropClientPtr=*/false);
  gR1Encoder.resetSerial();
}

bool g2RingIsConnected() {
  return gRing.connected;
}

bool g2RingPollVital(uint8_t which) {
  if (!gRing.connected || !gRing.writeChar) return false;
  R1Frame q = {};
  const char* tag = "?";
  switch (which) {
    case 0: q = gR1Encoder.buildHealthQuery(R1_CMD_HEARTRATE, R1_SUB_POINT); tag = "hrPoint";   break;
    case 1: q = gR1Encoder.buildHealthQuery(R1_CMD_HRV,       R1_SUB_POINT); tag = "hrvPoint";  break;
    case 2: q = gR1Encoder.buildHealthQuery(R1_CMD_SPO2,      R1_SUB_POINT); tag = "spo2Point"; break;
    case 3: q = gR1Encoder.buildHealthQuery(R1_CMD_TEMPERATURE, R1_SUB_POINT); tag = "tempPoint"; break;
    case 4: q = gR1Encoder.buildGenericQuery(R1_MODULE_SYSTEM, R1_CMD_SYSTEM,
                                             R1_SUB_DEVICE_STATUS);          tag = "devStatus"; break;
    default: return false;
  }
  if (q.length == 0 || !gRing.writeChar) return false;
  // Coalesce key 1..5: repeated Poll Now / Track mines replace in-queue.
  if (!ringWrite(q.bytes, q.length, (uint8_t)(which + 1))) return false;
  DEBUG_G2F("[RING] page poll: TX %s ser=%u", tag, (unsigned)q.serial);
  return true;
}

bool g2RingQueryDaily(uint8_t cmd) {
  if (!gRing.connected || !gRing.writeChar) return false;
  if (cmd != R1_CMD_HEARTRATE && cmd != R1_CMD_HRV && cmd != R1_CMD_SPO2 &&
      cmd != R1_CMD_TEMPERATURE && cmd != R1_CMD_ACTIVITY && cmd != R1_CMD_SLEEP) {
    return false;
  }
  R1Frame q = gR1Encoder.buildHealthQuery(cmd, R1_SUB_DAILY);
  if (q.length == 0) return false;
  if (!ringWrite(q.bytes, q.length, (uint8_t)(0x20 + cmd))) return false;
  DEBUG_G2F("[RING] daily query: TX cmd=0x%02X ser=%u", (unsigned)cmd, (unsigned)q.serial);
  return true;
}

void g2RingPollVitalForLogging(void) {
  if (!gRing.connected || !gRing.writeChar) return;
  static uint8_t cursor = 0;
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (lastMs != 0 && (long)(now - lastMs) < 700) return;
  (void)g2RingPollVital(cursor);
  cursor = (uint8_t)((cursor + 1) % G2_RING_POLL_VITAL_COUNT);
  lastMs = now;
}

void g2RingGetTelemetry(G2RingTelemetry& out) {
  // gR1Cache is written from the BLE notify task; telemetry updates are
  // minute-scale, so a plain field copy is fine for a read-only dashboard
  // (no lock; worst case is one stale tick, corrected on the next).
  out.connected     = gRing.connected;
  out.hr            = gR1Cache.hr;          out.hrValid      = gR1Cache.hrValid;
  out.hrv           = gR1Cache.hrv;         out.hrvValid     = gR1Cache.hrvValid;
  out.spo2          = gR1Cache.spo2;        out.spo2Valid    = gR1Cache.spo2Valid;
  out.tempTenths    = gR1Cache.tempTenths;  out.tempValid    = gR1Cache.tempValid;
  out.battery       = gR1Cache.battery;     out.batteryValid = gR1Cache.batteryValid;
  out.wear          = gR1Cache.wear;        out.wearValid    = gR1Cache.wearValid;
  out.hrAgeSec      = gR1Cache.hrValid      ? ringSampleAgeSec(gR1Cache.hrTs, gR1Cache.hrRxMs) : -1;
  out.hrvAgeSec     = gR1Cache.hrvValid     ? ringSampleAgeSec(gR1Cache.hrvTs, gR1Cache.hrvRxMs) : -1;
  out.spo2AgeSec    = gR1Cache.spo2Valid    ? ringSampleAgeSec(gR1Cache.spo2Ts, gR1Cache.spo2RxMs) : -1;
  out.tempAgeSec    = gR1Cache.tempValid    ? ringSampleAgeSec(gR1Cache.tempTs, gR1Cache.tempRxMs) : -1;
  out.batteryAgeSec = gR1Cache.batteryValid ? ringSampleAgeSec(0, gR1Cache.batteryRxMs) : -1;
  out.wearAgeSec    = gR1Cache.wearValid    ? ringSampleAgeSec(0, gR1Cache.wearRxMs) : -1;
  // Raw local receive stamps, copied through unprocessed — history consumers
  // need a monotonic axis that ring-clock custody can't step (see G2_Ring.h).
  out.hrRxMs        = gR1Cache.hrRxMs;
  out.hrvRxMs       = gR1Cache.hrvRxMs;
  out.spo2RxMs      = gR1Cache.spo2RxMs;
  out.tempRxMs      = gR1Cache.tempRxMs;
  out.batteryRxMs   = gR1Cache.batteryRxMs;
}

void g2RingGetStatus(char* buf, size_t cap) {
  if (!buf || cap == 0) return;
  const uint32_t nowMs = millis();
  const uint32_t upMs  = gRing.connected && gRing.connectedSince
                         ? (nowMs - gRing.connectedSince) : 0;
  snprintf(buf, cap,
           "ring=%s name='%s' addr=%s mtu=%u rx=%lu up=%u.%03us (scan=%s)",
           gRing.connected ? "up" : "down",
           gRingDeviceName.length() ? gRingDeviceName.c_str() : "<unknown>",
           gRingDeviceAddress.length() ? gRingDeviceAddress.c_str() : "--",
           (unsigned)gRing.mtu,
           (unsigned long)gRing.packetsReceived,
           (unsigned)(upMs / 1000), (unsigned)(upMs % 1000),
           gRingScanFound ? "found" : "not-found");
}

// =============================================================================
// Spoof-push to glasses
// =============================================================================
// The glasses' built-in UI normally consumes ring telemetry via a phone-app
// bridge: the phone connects to both the ring and the glasses, polls the
// ring, then forwards each metric in a sid=0x90 RingDataPackage frame to
// the right temple. We're impersonating that bridge — we already have the
// ring connected directly to us, so we poll the ring ourselves and synthesize
// the same sid=0x90 frame to the glasses. The glasses don't know whether
// the data came from a real bridge or our spoof.
//
// Only sent to the right temple — the left one ignores ring frames in the
// official protocol. Send is best-effort: dropped silently if the right
// temple isn't connected.
static bool ringSpoofSendOnce(uint32_t magic) {
  if (!gRing.connected) {
    DEBUG_G2F("[RING] spoof: ring not connected, skipping push");
    return false;
  }

  G2RingPushFields f = {};
  uint32_t now = (uint32_t)time(nullptr);

  if (gR1Cache.batteryValid) {
    f.battery_valid = true;
    f.battery       = (int32_t)gR1Cache.battery;
  }
  if (gR1Cache.hrValid) {
    f.hr_valid = true;
    f.hr       = (int32_t)gR1Cache.hr;
    f.hrTs     = (int32_t)gR1Cache.hrTs;
  }
  if (gR1Cache.hrvValid) {
    f.hrv_valid = true;
    f.hrv       = (int32_t)gR1Cache.hrv;
    f.hrvTs     = (int32_t)gR1Cache.hrvTs;
  }
  if (gR1Cache.spo2Valid) {
    f.spo2_valid = true;
    f.spo2       = (int32_t)gR1Cache.spo2;
    f.spo2Ts     = (int32_t)gR1Cache.spo2Ts;
  }

  if (!f.battery_valid && !f.hr_valid && !f.hrv_valid && !f.spo2_valid) {
    DEBUG_G2F("[RING] spoof: cache empty (no fresh telemetry yet) "
              "— nothing to push at t=%lu", (unsigned long)now);
    return false;
  }

  uint8_t env[256];
  uint8_t seq = g2AllocSeq();
  size_t envLen = g2BuildRingRawDataPush(seq, magic, f, env, sizeof(env));
  if (envLen == 0) {
    DEBUG_G2F("[RING] spoof: builder failed (envelope buffer too small?)");
    return false;
  }
  bool ok = g2SendToRightTemple(env, envLen);
  DEBUG_G2F("[RING] spoof TX seq=0x%02X envLen=%u %s — "
            "batt=%s%d hr=%s%d hrv=%s%d spo2=%s%d",
            seq, (unsigned)envLen, ok ? "sent" : "DROPPED(R-temple down?)",
            f.battery_valid ? "" : "?", f.battery,
            f.hr_valid      ? "" : "?", f.hr,
            f.hrv_valid     ? "" : "?", f.hrv,
            f.spo2_valid    ? "" : "?", f.spo2);
  return ok;
}

// Background task — wakes every gSpoofIntervalSec, polls ring for fresh
// point samples (cache populates via ringExtractTelemetryCache from the
// notify handler), then synthesizes the sid=0x90 push to the right temple.
//
// Polling cadence per wake: hr → 700ms wait → hrv → 700ms → spo2 → 700ms
// → deviceStatus → 600ms → push. The waits give the ring time to respond
// (its point queries return cached samples in <500ms typically). Total
// polling block ~2.7s per cycle — leaves the rest of the interval for the
// task to suspend cheaply.
//
// We keep using vTaskDelay between writes (the user feedback rule about
// avoiding per-action tasks): one persistent task, multiple sequenced
// writes inside it. DRAM cost is one task TCB + 4KB stack.
static void ringSpoofTaskBody(void* /*arg*/) {
  DEBUG_G2F("[RING] spoof task: started, interval=%us", (unsigned)gSpoofIntervalSec);
  uint32_t magicCounter = 0;
  while (gSpoofEnabled) {
    if (gRing.connected && gRing.writeChar) {
      // Poll each metric. Ignore encode failures — cache simply won't
      // refresh for that metric this cycle.
      auto sendQ = [](uint8_t cmd, uint8_t sub, uint8_t ckey, const char* tag) {
        R1Frame q = gR1Encoder.buildHealthQuery(cmd, sub);
        if (q.length > 0 && gRing.writeChar &&
            ringWrite(q.bytes, q.length, ckey)) {
          DEBUG_G2F("[RING] spoof poll: TX %s ser=%u", tag, (unsigned)q.serial);
        }
      };
      sendQ(R1_CMD_HEARTRATE, R1_SUB_POINT, 1, "hrPoint");
      vTaskDelay(pdMS_TO_TICKS(700));
      sendQ(R1_CMD_HRV,       R1_SUB_POINT, 2, "hrvPoint");
      vTaskDelay(pdMS_TO_TICKS(700));
      sendQ(R1_CMD_SPO2,      R1_SUB_POINT, 3, "spo2Point");
      vTaskDelay(pdMS_TO_TICKS(700));

      // Battery comes from deviceStatus — generic system-module probe.
      {
        R1Frame q = gR1Encoder.buildGenericQuery(R1_MODULE_SYSTEM, R1_CMD_SYSTEM,
                                                 R1_SUB_DEVICE_STATUS);
        if (q.length > 0 && gRing.writeChar &&
            ringWrite(q.bytes, q.length, /*coalesceKey=*/5)) {
          DEBUG_G2F("[RING] spoof poll: TX deviceStatus ser=%u", (unsigned)q.serial);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(600));

      // Vary magic per cycle so a glasses-side dedup doesn't collapse
      // identical-payload pushes.
      uint32_t mag = G2_MAGIC_RING_RAW_PUSH + (magicCounter++ & 0x7F);
      ringSpoofSendOnce(mag);
    } else {
      DEBUG_G2F("[RING] spoof task: ring not connected — skipping cycle");
    }

    // Sleep the remainder of the interval. Small ticks so an `off` flip
    // gets noticed quickly without bashing the scheduler.
    uint32_t remain = gSpoofIntervalSec * 1000;
    if (remain < 3000) remain = 3000;  // already burned ~2.7s polling
    else remain -= 2700;
    while (remain > 0 && gSpoofEnabled) {
      uint32_t step = remain > 500 ? 500 : remain;
      vTaskDelay(pdMS_TO_TICKS(step));
      remain -= step;
    }
  }
  DEBUG_G2F("[RING] spoof task: exiting");
  gSpoofTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static bool ringSpoofStart(uint32_t intervalSec) {
  if (gSpoofEnabled) return false;
  if (intervalSec < 10)  intervalSec = 10;
  if (intervalSec > 600) intervalSec = 600;
  gSpoofIntervalSec = intervalSec;
  gSpoofEnabled     = true;
  BaseType_t rc = xTaskCreatePinnedToCore(ringSpoofTaskBody, "ring_spoof",
                              /*stack*/ 4096, nullptr,
                              /*prio*/  4,    &gSpoofTaskHandle, APP_CORE);
  if (rc != pdPASS) {
    DEBUG_G2F("[RING] spoof task: xTaskCreate failed (rc=%d)", (int)rc);
    gSpoofEnabled    = false;
    gSpoofTaskHandle = nullptr;
    return false;
  }
  return true;
}

static void ringSpoofStop() {
  // Task self-deletes on next loop iteration. We just flip the flag and
  // let it wind down — avoids a vTaskDelete race against the task body's
  // ring writes.
  gSpoofEnabled = false;
}

// =============================================================================
// CLI commands
// =============================================================================

static const char* cmd_ringstatus(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char buf[256];
  if (argWantsJson(args)) {
    const uint32_t upMs = (gRing.connected && gRing.connectedSince) ? (millis() - gRing.connectedSince) : 0;
    CompactJson j(buf, sizeof(buf));
    j.kv("schema", 1)
     .kv("connected", (bool)gRing.connected)
     .kv("name", gRingDeviceName.length() ? gRingDeviceName.c_str() : "")
     .kv("addr", gRingDeviceAddress.length() ? gRingDeviceAddress.c_str() : "")
     .kv("mtu", (unsigned)gRing.mtu)
     .kv("rx", (unsigned long)gRing.packetsReceived)
     .kv("upMs", (unsigned long)upMs)
     .kv("scanFound", (bool)gRingScanFound);
    return j.c_str();
  }
  g2RingGetStatus(buf, sizeof(buf));
  return buf;
}

// ringconnect [mac]
//   No args: scan-then-connect (uses the new self-scan in ringDoConnect).
//   With MAC: skip the scan entirely, attempt a direct BLE connection to
//   that address. Use this when the ring is bonded to another central
//   (e.g. your phone via the Even app) and so doesn't broadcast where
//   our scan can see it. The BLE controller can sometimes still establish
//   a connection by MAC if the peripheral is in directed-advertising mode
//   or accepts multi-central. Failure modes:
//     - "BLE connect FAILED after Nms" → ring not reachable / not advertising
//       to us / already at its central-count limit
//     - Hangs ~30s then times out → no response from peer at that address
static const char* cmd_ringconnect(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(args);
  String a0 = ca.arg(0);
  a0.toLowerCase();

  // `ringconnect reconnect` — glasses R1 "Reconnect Ring" path. Same heap
  // guard + disconnect settle as the old tap-inline helper; runs on
  // cmd_exec (which can afford the 500 ms delay) instead of g2_tap_disp.
  if (a0 == "reconnect") {
    const uint32_t freeNow = ESP.getFreeHeap();
    if (freeNow < 16 * 1024) {
      return "Error: RING: reconnect aborted — DRAM free < 16 KB "
             "(reboot or recover heap; Arduino BLE leak per reconnect)";
    }
    if (g2RingIsConnected()) {
      g2RingDisconnect();
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!g2RingConnect()) {
      return g2RingIsConnected() ? "RING: already connected"
                                 : "Error: RING: reconnect failed to start connect task";
    }
    return "RING: reconnect started — use ringstatus to watch";
  }

  if (ca.count() >= 1) {
    String mac = ca.arg(0);
    if (!g2RingConnectMac(mac)) {
      return "Error: RING: direct-MAC connect failed (already running? MAC format?)";
    }
    EXT_RAM_BSS_ATTR static char buf[200];
    snprintf(buf, sizeof(buf),
             "RING: direct-connect to %s started (no scan) — watch ringstatus / log",
             mac.c_str());
    return buf;
  }
  if (!g2RingConnect()) {
    return gRing.connected ? "RING: already connected"
                           : "RING: connect failed (see log)";
  }
  return "RING: connect task started — use ringstatus to watch";
}

static const char* cmd_ringdisconnect(const String& /*args*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  blePeerNoteUserDisconnect(BLE_PEER_R1_RING);
  g2RingDisconnect();
  return "RING: disconnect requested";
}

static const char* cmd_ringverbose(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(args);
  String a = ca.arg(0); a.toLowerCase();
  if (a == "on")        gRingDumpVerbose = true;
  else if (a == "off")  gRingDumpVerbose = false;
  else                  gRingDumpVerbose = !gRingDumpVerbose;
  return gRingDumpVerbose ? "RING verbose: on (full hex dump of every notify)"
                          : "RING verbose: off (header decode only)";
}

// ringscan [seconds]
// Dedicated ring-only scan. Default 30s, max 300s. Doesn't early-terminate
// on glasses-found like the shared g2scan does. Blocks the CLI worker for
// up to `seconds` seconds while scanning. Use a larger value if the ring
// is being elusive.
static const char* cmd_ringscan(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(args);
  uint32_t seconds = 30;
  if (ca.count() >= 1) {
    long n = ca.argInt(0, 30);
    if (n > 0 && n <= 300) seconds = (uint32_t)n;
  }
  EXT_RAM_BSS_ATTR static char buf[200];
  if (g2RingScan(seconds)) {
    snprintf(buf, sizeof(buf),
             "RING: scan found %s @ %s — run 'ringconnect' to connect",
             gRingDeviceName.c_str(), gRingDeviceAddress.c_str());
    return buf;
  }
  snprintf(buf, sizeof(buf),
           "RING: scan timed out after %us — ring not advertising. "
           "Tap to wake / check battery / move closer, then retry.",
           (unsigned)seconds);
  return buf;
}

// ringquery <subject> [type]
//
// Send an R1 health/status request and watch the notify stream for the
// response. Use this to RE the response payload format — the FlutterApp
// has the encoder framework but never wired it to anything that actually
// asks for HR/HRV/etc, so we don't know what the responses look like.
//
//   subject: wear         — wearStatus probe (response is 1 B: 0=unknown 1=notWear 2=wear)
//            health       — healthSettingsStatus probe (response is the active sensor bitmap)
//            hr | hrv | spo2 | temp | activity | sleep — health-data request
//            report on|off — toggle continuous-push (mask 0xFF / 0x00)
//            raw <mod> <cmd> <sub> — generic notify/get/ok escape hatch
//
//   type (for the 6 health subjects only):
//            daily     (default) — aggregated daily history
//            point               — recent measurement points
//            measure             — start a real-time sampling session
//            (the ring rejects unsupported pairs with status=ack/refuse —
//            you'll see that in the decoded log)
static const char* cmd_ringquery(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char ret[200];
  if (!gRing.connected || !gRing.writeChar) {
    return "Error: RING: not connected (run 'ringconnect' first)";
  }

  CommandArgs ca(args);
  if (ca.count() < 1) {
    return "Error: invalid arguments — Usage: ringquery <wear|health|hr|hrv|spo2|temp|activity|sleep|report|raw> [type]\n"
           "       ringquery hr [daily|point|measure]   (same shape for hrv/spo2/temp/activity/sleep)\n"
           "       ringquery report on|off|<byte>       (e.g. 0x10 to set bit 4 only — bit-bash to find what triggers push)\n"
           "       ringquery raw <module> <cmd> <subCmd> [hex_payload] [status=NN]\n"
           "         decimal mod/cmd/sub; hex payload optional; status byte hex (default 00 = notify/get/ok).\n"
           "         Common status bytes: 00=notify/get/ok (queries), 02=notify/set/ok (writes/SET),\n"
           "         03=ack/set/ok (echoing an ack). Bit layout: bit0 type(0=notify,1=ack), bit1 method\n"
           "         (0=get,1=set), bits2-3 ack(0=ok,1=err,2=refuse,3=notSup).";
  }
  String subject = ca.arg(0); subject.toLowerCase();

  R1Frame f = {};
  const char* tag = "?";

  auto resolveSubCmd = [&](const String& s) -> int {
    if (s.length() == 0)        return R1_SUB_DAILY;     // default
    String t = s; t.toLowerCase();
    if (t == "daily")           return R1_SUB_DAILY;
    if (t == "point")           return R1_SUB_POINT;
    if (t == "measure")         return R1_SUB_MEASURE;
    return -1;
  };

  if (subject == "wear") {
    f = gR1Encoder.buildWearStatusQuery();
    tag = "wearStatus";
  } else if (subject == "health") {
    f = gR1Encoder.buildHealthSettingsQuery();
    tag = "healthSettingsStatus";
  } else if (subject == "report") {
    // `ringquery report on|off|<hex>` — let the user bit-bash the
    // reportEnable mask byte. The healthSettingsStatus response showed bit 4
    // is normally on, so try `report 0x10`, `report 0x01`, etc. to discover
    // which bit (or combination) actually triggers continuous push.
    String mode = ca.arg(1); mode.toLowerCase();
    int maskInt = -1;
    if      (mode == "on")  maskInt = 0xFF;
    else if (mode == "off") maskInt = 0x00;
    else if (mode.startsWith("0x")) {
      maskInt = (int)strtol(mode.c_str() + 2, nullptr, 16);
    } else if (mode.length() > 0) {
      maskInt = ca.argInt(1, -1);
    }
    if (maskInt < 0 || maskInt > 0xFF) {
      return "ringquery report: expected 'on', 'off', or a byte 0..255 / 0xNN";
    }
    uint8_t mask = (uint8_t)maskInt;
    f = gR1Encoder.buildHealthReportEnable(mask);
    if (!ringWrite(f.bytes, f.length)) {
      return "Error: RING: report enable write failed";
    }
    snprintf(ret, sizeof(ret), "RING: report enable=0x%02X sent (set/SET/ok). Watch for ack.", mask);
    DEBUG_G2F("[RING] TX healthReportEnable ser=%u mask=0x%02X (%u B)",
              (unsigned)f.serial, mask, (unsigned)f.length);
    return ret;
  } else if (subject == "raw") {
    if (ca.count() < 4) return "Error: ringquery raw: need <module> <cmd> <subCmd> [hex_payload]";
    long mod = ca.argInt(1, -1);
    long cmd = ca.argInt(2, -1);
    long sub = ca.argInt(3, -1);
    if (mod < 0 || mod > 0xFF || cmd < 0 || cmd > 0xFF || sub < 0 || sub > 0xFF) {
      return "Error: ringquery raw: module/cmd/subCmd must be 0..255";
    }

    // Optional hex payload (e.g. "01" or "01FF" or "0x01 0xFF" — strip
    // 0x prefixes, spaces, and colons, then parse pairs of hex digits).
    //
    // Optional `status=NN` token: a single full status-byte override, hex.
    // Default 0x00 = notify/get/ok (the read-only path). Bit layout:
    //   bit 0   : type    0=notify, 1=ack
    //   bit 1   : method  0=get,    1=set
    //   bits 2-3: ack     0=ok, 1=error, 2=refuse, 3=notSupport
    // Common values:
    //   0x00 notify/get/ok  — default, used to read most opcodes
    //   0x02 notify/set/ok  — write/enable opcodes (e.g. touchSwitch with payload)
    //   0x03 ack/set/ok     — echoing the ring's own ack shape
    // Discovery context: until this knob existed, every probe was notify/get,
    // which is silently ignored or returns ack/set/error on write-only opcodes
    // (userInfo, touchSwitch, etc.). See R1_RING_PROTOCOL.md §6 for per-opcode
    // markings.
    uint8_t statusByte = 0x00;
    uint8_t payload[R1_MAX_PAYLOAD];
    size_t payloadLen = 0;
    if (ca.count() >= 5) {
      // Pass 1: peel off any `status=NN` token, concatenate the rest as the
      // payload hex string. Order-insensitive — `status=` can appear before,
      // after, or between payload-hex tokens.
      //
      // Detection uses indexOf('=') + case-insensitive key compare rather
      // than startsWith() because an earlier version of this code lost the
      // status= token silently (likely from String::startsWith with an
      // implicit const-char* prefix conversion). indexOf is unambiguous.
      String raw;
      for (int i = 4; i < ca.count(); i++) {
        String arg = ca.arg(i);
        int eqPos = arg.indexOf('=');
        bool isStatusToken = false;
        if (eqPos > 0) {
          String key = arg.substring(0, eqPos);
          key.toLowerCase();
          if (key == "status") {
            isStatusToken = true;
            String val = arg.substring(eqPos + 1);
            val.toLowerCase();
            if (val.startsWith("0x")) val = val.substring(2);
            if (val.length() == 0 || val.length() > 2) {
              return "Error: ringquery raw: status=NN must be 1-2 hex digits (00..FF)";
            }
            char* end = nullptr;
            long parsed = strtol(val.c_str(), &end, 16);
            if (end == val.c_str() || parsed < 0 || parsed > 0xFF) {
              return "Error: ringquery raw: status=NN must be a hex byte 00..FF";
            }
            statusByte = (uint8_t)parsed;
          }
        }
        if (!isStatusToken) {
          raw += arg;
        }
      }
      // Debug breadcrumb so future "status= didn't take" mysteries can be
      // verified at a glance instead of guessing.
      DEBUG_G2F("[RING] raw-args: count=%d payloadHex='%s' statusByte=0x%02X",
                ca.count(), raw.c_str(), (unsigned)statusByte);
      // Pass 2: parse the hex payload from whatever remains. Strip everything
      // that isn't a hex digit (handles "0x", spaces, colons in the input).
      String clean;
      for (size_t i = 0; i < raw.length(); i++) {
        char c = raw.charAt(i);
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')) {
          clean += c;
        }
      }
      if (clean.length() > 0) {
        if (clean.length() & 1) {
          return "Error: ringquery raw: hex payload must be an even number of hex digits";
        }
        payloadLen = clean.length() / 2;
        if (payloadLen > R1_MAX_PAYLOAD) {
          return "Error: ringquery raw: payload too large";
        }
        for (size_t i = 0; i < payloadLen; i++) {
          char hi = clean.charAt(2 * i);
          char lo = clean.charAt(2 * i + 1);
          auto nyb = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return 0;
          };
          payload[i] = (uint8_t)((nyb(hi) << 4) | nyb(lo));
        }
      }
    }

    // Decompose the status byte into the three fields the encoder takes.
    const uint8_t statusType   = (statusByte >> 0) & 0x01;
    const uint8_t statusMethod = (statusByte >> 1) & 0x01;
    const uint8_t statusAck    = (statusByte >> 2) & 0x03;

    f = gR1Encoder.build((uint8_t)mod, (uint8_t)cmd, (uint8_t)sub,
                         statusType, statusMethod, statusAck,
                         payloadLen ? payload : nullptr, payloadLen);
    tag = "raw";
    DEBUG_G2F("[RING] raw: mod=%lu cmd=%lu sub=%lu status=0x%02X (%s/%s/%s) payloadLen=%u",
              (unsigned long)mod, (unsigned long)cmd, (unsigned long)sub,
              (unsigned)statusByte,
              r1StatusTypeName(statusType),
              r1StatusMethodName(statusMethod),
              r1StatusAckName(statusAck),
              (unsigned)payloadLen);
  } else {
    int cmdId = -1;
    if      (subject == "hr"       || subject == "heartrate")   cmdId = R1_CMD_HEARTRATE;
    else if (subject == "hrv")                                  cmdId = R1_CMD_HRV;
    else if (subject == "spo2")                                 cmdId = R1_CMD_SPO2;
    else if (subject == "temp"     || subject == "temperature") cmdId = R1_CMD_TEMPERATURE;
    else if (subject == "activity")                             cmdId = R1_CMD_ACTIVITY;
    else if (subject == "sleep")                                cmdId = R1_CMD_SLEEP;
    else {
      snprintf(ret, sizeof(ret), "ringquery: unknown subject '%s'", subject.c_str());
      return ret;
    }
    int subCmd = resolveSubCmd(ca.arg(1));
    if (subCmd < 0) return "ringquery: type must be daily|point|measure";
    f = gR1Encoder.buildHealthQuery((uint8_t)cmdId, (uint8_t)subCmd);
    tag = subject.c_str();
  }

  if (f.length == 0) {
    return "Error: RING: failed to encode query frame";
  }

  if (!ringWrite(f.bytes, f.length)) {
    return "Error: RING: query write failed";
  }
  DEBUG_G2F("[RING] TX query '%s' ser=%u (%u B) — watch logs for response",
            tag, (unsigned)f.serial, (unsigned)f.length);
  snprintf(ret, sizeof(ret),
           "RING: query '%s' sent (%u B, ser=%u). Watch [RING] RX log for response.",
           tag, (unsigned)f.length, (unsigned)f.serial);
  return ret;
}

// ringtoglasses on [interval_sec] | off | now | status
//
// Spoof-push ring telemetry into the glasses' built-in UI by synthesizing
// sid=0x90 RingDataPackage frames to the right temple. We poll the ring
// directly (we already have it connected), cache decoded HR/HRV/SpO2/battery
// from the notifies, and forward whatever's fresh to the glasses on a
// periodic schedule. The glasses can't tell the data isn't coming from a
// real phone-app bridge.
//
//   on [sec]   start the background pusher. Default 30s, clamp [10..600].
//   off        stop pushing
//   now        send a one-shot push using whatever's currently in the cache
//              (won't poll first — useful to test the codec without waiting
//              a whole interval)
//   status     print enable flag, interval, and current cache contents
// DEPRECATED: unregistered from g2RingCommands — see registry table for why.
// Kept as compiled reference so the spoof-push plumbing isn't lost.
__attribute__((unused))
static const char* cmd_ringtoglasses(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char ret[300];
  CommandArgs ca(args);
  String sub = ca.arg(0); sub.toLowerCase();

  if (sub == "" || sub == "status") {
    snprintf(ret, sizeof(ret),
             "ringtoglasses: %s interval=%us cache: hr=%s%u hrv=%s%d spo2=%s%u batt=%s%u",
             gSpoofEnabled ? "ON" : "off",
             (unsigned)gSpoofIntervalSec,
             gR1Cache.hrValid      ? "" : "?", (unsigned)gR1Cache.hr,
             gR1Cache.hrvValid     ? "" : "?", (int)gR1Cache.hrv,
             gR1Cache.spo2Valid    ? "" : "?", (unsigned)gR1Cache.spo2,
             gR1Cache.batteryValid ? "" : "?", (unsigned)gR1Cache.battery);
    return ret;
  }

  if (sub == "on") {
    if (gSpoofEnabled) return "ringtoglasses: already running (use 'off' to stop first)";
    long secs = (ca.count() >= 2) ? ca.argInt(1, 30) : 30;
    if (secs < 10)  secs = 10;
    if (secs > 600) secs = 600;
    if (!ringSpoofStart((uint32_t)secs)) {
      return "Error: ringtoglasses: failed to start task (out of memory?)";
    }
    snprintf(ret, sizeof(ret),
             "ringtoglasses: started (interval=%lds). First push in ~%lds.",
             secs, secs);
    return ret;
  }

  if (sub == "off") {
    if (!gSpoofEnabled) return "ringtoglasses: not running";
    ringSpoofStop();
    return "ringtoglasses: stop requested (task winds down on next cycle)";
  }

  if (sub == "now") {
    if (!ringSpoofSendOnce(G2_MAGIC_RING_RAW_PUSH)) {
      return "Error: ringtoglasses: one-shot push failed (cache empty? right temple down?)";
    }
    return "ringtoglasses: one-shot push sent (see [RING] spoof TX log)";
  }

  return "Error: invalid arguments — Usage: ringtoglasses <on [sec]|off|now|status>";
}

// =============================================================================
// Bridge keepalive task
// =============================================================================
// Phase 5 of docs/g2_proto/Ring_Bridge_Sequence.h: while the bridge is active,
// the host maintains a 30-second heartbeat on sid=0x80 cmd=14
// (BASE_CONNECT_HEARTBEAT, empty body) to keep the temple's bridge state from
// timing out. Without this the bridge has been observed to fail to complete
// its bond with the ring — the temple ack's our RING_CONNECT_INFO trigger but
// then sits silent.
//
// Started by `ringbridge on`, stopped by `ringbridge off` (or any path that
// flips gBridgeRequested to false). Sends to BOTH temples — the doc is
// ambiguous about whether only the right one needs it; both is cheap and
// matches the existing g2devcfg-heartbeat broadcast pattern.
static volatile bool   gBridgeHbEnabled    = false;
static TaskHandle_t    gBridgeHbTaskHandle = nullptr;

static void ringBridgeHeartbeatBody(void* /*arg*/) {
  DEBUG_G2F("[RING] bridge-heartbeat task: started (30s cadence)");
  while (gBridgeHbEnabled) {
    uint8_t env[40];
    size_t envLen = g2BuildDevCfgHeartbeat(g2AllocSeq(),
                                           G2_MAGIC_DEVCFG_HEARTBEAT,
                                           env, sizeof(env));
    if (envLen > 0) {
      bool ok = g2SendToRightTemple(env, envLen);
      DEBUG_G2F("[RING] bridge-heartbeat → R: %s",
                ok ? "sent" : "DROPPED (R-temple down)");
    }
    // Sleep 30s in 500ms slices so an `off` flip lands quickly.
    uint32_t remain = 30000;
    while (remain > 0 && gBridgeHbEnabled) {
      uint32_t step = remain > 500 ? 500 : remain;
      vTaskDelay(pdMS_TO_TICKS(step));
      remain -= step;
    }
  }
  DEBUG_G2F("[RING] bridge-heartbeat task: exiting");
  gBridgeHbTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static bool ringBridgeHeartbeatStart() {
  if (gBridgeHbEnabled) return true;
  gBridgeHbEnabled = true;
  BaseType_t rc = xTaskCreatePinnedToCore(ringBridgeHeartbeatBody, "ring_bridge_hb",
                              /*stack*/ 3072, nullptr,
                              /*prio*/  3,    &gBridgeHbTaskHandle, APP_CORE);
  if (rc != pdPASS) {
    DEBUG_G2F("[RING] bridge-heartbeat: xTaskCreate failed (rc=%d)", (int)rc);
    gBridgeHbEnabled    = false;
    gBridgeHbTaskHandle = nullptr;
    return false;
  }
  return true;
}

static void ringBridgeHeartbeatStop() {
  // Task self-deletes on next loop iteration.
  gBridgeHbEnabled = false;
}

// =============================================================================
// Bridge progress mirror — populated by parseSid80Rx via a public hook
// =============================================================================
// The right temple reports bridge-attempt progress via the connRet field on
// sid=0x80 RING_CONNECT_INFO polls. We capture the most recent value here so
// `ringbridge status` can show it without forcing the user to scroll logs.
//
// connRet values seen in the wild (from R1_RE_Reference / live captures):
//   0  unknown / not reported
//   1  scanning (temple is looking for the ring)
//   8  fail-terminal (bond attempt gave up)
//   19 scanning? (alternative scanning code)
//   62 unknown error (sometimes seen during bridge teardown)
//   <other>  see connRetHint() in G2_Glasses.cpp for the full table
struct R1BridgeProgress {
  uint32_t lastConnRet;     uint32_t lastConnRetMs;     bool hasConnRet;
  uint32_t lastConnectRing; uint32_t lastConnectRingMs; bool hasConnectRing;
};
static R1BridgeProgress gBridgeProgress;

void g2RingNoteBridgePoll(uint64_t connRet, bool hasConnRet,
                          uint64_t connectRing, bool hasConnectRing) {
  uint32_t now = millis();
  if (hasConnRet) {
    gBridgeProgress.lastConnRet   = (uint32_t)connRet;
    gBridgeProgress.lastConnRetMs = now;
    gBridgeProgress.hasConnRet    = true;
  }
  if (hasConnectRing) {
    gBridgeProgress.lastConnectRing   = (uint32_t)connectRing;
    gBridgeProgress.lastConnectRingMs = now;
    gBridgeProgress.hasConnectRing    = true;
  }
}

// ringbridge on | off | status
//
// Hand the ring's BLE link off to the right temple's built-in bridge
// firmware so the glasses' OWN health UI displays ring data natively. The
// temple cannot bridge while we hold the ring (it's a single-central
// peripheral), so `on` first drops our link + disables auto-reconnect,
// then sends sid=0x80 cmd=6 RING_CONNECT_INFO with connectRing=true to the
// right temple. The temple then scans for the ring, bonds, and starts
// forwarding telemetry to us as sid=0x90 RingDataPackage frames — which
// our handleEnvelope→g2RingNoteForwardedTelemetry path catches and pushes
// into the same gR1Cache the direct path uses.
//
// `off` reverses the handoff: send connectRing=false to the temple to
// release the ring, re-enable auto-reconnect (so a future boot grabs it
// directly), and optionally `ringconnect` ourselves.
//
// `status` reports the current mode + cache state.
//
// We persist the mode in `gBridgeRequested` (RAM-only — survives the rest
// of the session but a reboot returns to the bleautoreconnect default).
static volatile bool gBridgeRequested = false;

// DEPRECATED: unregistered from g2RingCommands — see registry table for why.
// Kept as compiled reference so the RING_CONNECT_INFO + heartbeat plumbing
// isn't lost.
__attribute__((unused))
static const char* cmd_ringbridge(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char ret[300];
  CommandArgs ca(args);
  String sub = ca.arg(0); sub.toLowerCase();

  if (sub == "" || sub == "status") {
    // Decode the most recent connRet for a friendly hint.
    const char* connRetWord = "?";
    char ageBuf[24] = {0};
    if (gBridgeProgress.hasConnRet) {
      switch (gBridgeProgress.lastConnRet) {
        case 0:  connRetWord = "idle";          break;
        case 1:  connRetWord = "scanning";      break;
        case 8:  connRetWord = "fail-terminal"; break;
        case 19: connRetWord = "scanning?";     break;
        case 62: connRetWord = "err?";          break;
        default: connRetWord = "?";             break;
      }
      uint32_t ageMs = millis() - gBridgeProgress.lastConnRetMs;
      snprintf(ageBuf, sizeof(ageBuf), " %us-ago", (unsigned)(ageMs / 1000));
    }
    snprintf(ret, sizeof(ret),
             "ringbridge: mode=%s autoReconnect=%s ring-link=%s heartbeat=%s "
             "tempStatus: connectRing=%s%u connRet=%u(%s)%s "
             "fwd-cache: hr=%s%u hrv=%s%d spo2=%s%u batt=%s%u",
             gBridgeRequested ? "BRIDGE (temple owns ring)" : "DIRECT (we own ring)",
             gBlePeerData[BLE_PEER_R1_RING].autoReconnect ? "on" : "off",
             gRing.connected ? "up" : "down",
             gBridgeHbEnabled ? "on" : "off",
             gBridgeProgress.hasConnectRing ? "" : "?",
             (unsigned)gBridgeProgress.lastConnectRing,
             (unsigned)gBridgeProgress.lastConnRet, connRetWord, ageBuf,
             gR1Cache.hrValid      ? "" : "?", (unsigned)gR1Cache.hr,
             gR1Cache.hrvValid     ? "" : "?", (int)gR1Cache.hrv,
             gR1Cache.spo2Valid    ? "" : "?", (unsigned)gR1Cache.spo2,
             gR1Cache.batteryValid ? "" : "?", (unsigned)gR1Cache.battery);
    return ret;
  }

  if (sub == "on") {
    // Need a saved ring MAC + name to tell the temple what to bond with.
    // Prefer the live values if we're connected; fall back to the persisted
    // peer-registry entry.
    String mac;
    String name;
    if (gRingDeviceAddress.length() > 0) {
      mac  = gRingDeviceAddress;
      name = gRingDeviceName;
    } else {
      mac = gBlePeerData[BLE_PEER_R1_RING].mac1;
    }
    mac.trim(); name.trim();
    if (mac.length() < 17) {
      return "Error: ringbridge on: no ring MAC known — `ringconnect` once first "
             "(or `ringscan`) so we have something to tell the temple.";
    }
    if (name.length() == 0) {
      // FlutterApp comments suggest the name doesn't actually matter
      // operationally (the temple uses MAC for the connect), but the proto
      // requires non-empty bytes. Use a placeholder if we don't know it.
      name = "EVEN R1";
    }

    // Parse "f8:29:ca:ba:ac:1c" → byte array (BLE address order = high byte
    // first). g2BuildDevCfgRingConnect reverses internally to wire order.
    uint8_t macBle[6] = {0};
    {
      const char* s = mac.c_str();
      for (int i = 0; i < 6; i++) {
        unsigned v = 0;
        if (sscanf(s + i * 3, "%2x", &v) != 1) {
          return "Error: ringbridge on: failed to parse ring MAC";
        }
        macBle[i] = (uint8_t)v;
      }
    }

    // FlutterApp's ring_bridge_coordinator.dart sequence:
    //   if (!g2.isConnected || !r1.isConnected) return;
    //   await r1.sendAdvStart(g2RightId: g2.rightId!);
    //   await g2.sendConnectRing(...);
    // Two important properties:
    //   1. The ring MUST already be connected to us (the host). The R1 ring
    //      supports multi-central — the temple bonds with it in PARALLEL while
    //      we keep our link. An earlier version of this firmware disconnected
    //      from the ring before triggering, which appears to have killed the
    //      pairing context the ring needed to accept the temple's bond.
    //   2. The advStart payload is the temple's BLE MAC IN ORDER (NOT
    //      reversed). Reversing it (which an earlier version of this firmware
    //      did) makes the ring directed-advertise toward a non-existent MAC.
    if (!gRing.connected || !gRing.writeChar) {
      return "Error: ringbridge on: ring is not currently connected — run "
             "`ringconnect` first, then re-issue `ringbridge on`. The R1 must "
             "stay connected to us throughout the bridge (the temple bonds in "
             "parallel; multi-central is supported).";
    }

    {
      uint8_t templeMac[6] = {0};
      if (g2GetRightTempleMac(templeMac)) {
        DEBUG_G2F("[RING] ringbridge: refreshing advStart on ring → temple "
                  "%02X:%02X:%02X:%02X:%02X:%02X (BLE order, not reversed)",
                  templeMac[0], templeMac[1], templeMac[2],
                  templeMac[3], templeMac[4], templeMac[5]);
        R1Frame advStart = gR1Encoder.buildAdvStart(templeMac);
        if (advStart.length > 0) {
          if (ringWrite(advStart.bytes, advStart.length)) {
            vTaskDelay(pdMS_TO_TICKS(250));  // let the write land on the ring
          } else {
            DEBUG_G2F("[RING] ringbridge: WARN advStart write failed; "
                      "trigger will go without advStart refresh");
          }
        } else {
          DEBUG_G2F("[RING] ringbridge: WARN advStart frame build failed; "
                    "trigger will go without advStart refresh");
        }
      } else {
        DEBUG_G2F("[RING] ringbridge: WARN no right-temple MAC available — "
                  "trigger will go without advStart refresh; bridge may not bond");
      }
    }

    // Build + send RING_CONNECT_INFO to the right temple. seq=0 — we don't
    // expect to correlate replies with this since we're not tracking the
    // bridge-attempt response yet (the temple will report progress via the
    // sid=0x80 connRet polls we already log).
    uint8_t env[80];
    size_t envLen = g2BuildDevCfgRingConnect(g2AllocSeq(),
                                             G2_MAGIC_DEVCFG_RING_CONNECT,
                                             /*connect=*/true,
                                             macBle, name.c_str(),
                                             env, sizeof(env));
    if (envLen == 0) return "Error: ringbridge on: failed to build RING_CONNECT_INFO frame";
    if (!g2SendToRightTemple(env, envLen)) {
      return "Error: ringbridge on: send to right temple failed (R-temple down?)";
    }
    gBridgeRequested = true;
    // Start the 30s keepalive so the temple's bridge state doesn't time out.
    ringBridgeHeartbeatStart();
    snprintf(ret, sizeof(ret),
             "ringbridge: ON requested. We are STAYING connected to the ring "
             "(R1 supports multi-central — temple bonds in parallel). Temple "
             "should bond %s within ~10-20s; watch for [G2-R] sid=0x80 RX "
             "RING_CONNECT_INFO connRet=N transitions, then [G2-R] sid=0x90 "
             "forward frames once data is flowing. Re-run `ringbridge status` "
             "to check progress.",
             mac.c_str());
    return ret;
  }

  if (sub == "off") {
    // Same payload shape as `on`, but connectRing=false — releases the
    // ring on the temple side. Use cached MAC/name.
    String mac  = gRingDeviceAddress.length() ? gRingDeviceAddress
                                              : gBlePeerData[BLE_PEER_R1_RING].mac1;
    String name = gRingDeviceName.length() ? gRingDeviceName : String("EVEN R1");
    mac.trim();
    if (mac.length() < 17) {
      return "Error: ringbridge off: no MAC known to address the release — "
             "send `g2devcfg ring <mac> <name>` manually if you need to.";
    }
    uint8_t macBle[6] = {0};
    {
      const char* s = mac.c_str();
      for (int i = 0; i < 6; i++) {
        unsigned v = 0;
        if (sscanf(s + i * 3, "%2x", &v) != 1) {
          return "Error: ringbridge off: failed to parse ring MAC";
        }
        macBle[i] = (uint8_t)v;
      }
    }
    uint8_t env[80];
    size_t envLen = g2BuildDevCfgRingConnect(g2AllocSeq(),
                                             G2_MAGIC_DEVCFG_RING_CONNECT,
                                             /*connect=*/false,
                                             macBle, name.c_str(),
                                             env, sizeof(env));
    if (envLen == 0) return "Error: ringbridge off: failed to build release frame";
    if (!g2SendToRightTemple(env, envLen)) {
      return "Error: ringbridge off: send to right temple failed (R-temple down?)";
    }
    gBridgeRequested = false;
    ringBridgeHeartbeatStop();
    return "ringbridge: OFF requested. Temple should drop the ring within "
           "~5-10s. Our own ring link is unaffected (we kept it during the "
           "bridge), so direct `ringquery ...` commands continue to work.";
  }

  return "Error: invalid arguments — Usage: ringbridge <on|off|status>";
}

extern const CommandEntry g2RingCommands[] = {
  { "ringstatus",     "Show R1 ring connection status",            false, cmd_ringstatus     },
  { "ringscan",       "Scan for the R1 ring: ringscan [seconds] (default 30, max 300)", false, cmd_ringscan, "Usage: ringscan [seconds] (1..300, default 30)" },
  { "ringconnect",    "Connect to the R1 ring: ringconnect [mac|reconnect]", true, cmd_ringconnect, "Usage: ringconnect [mac|reconnect]  (no arg = scan-then-connect; mac = direct; reconnect = drop+settle+connect)" },
  { "ringdisconnect", "Disconnect from the R1 ring",               true, cmd_ringdisconnect },
  { "ringverbose",    "Toggle full hex dump of ring notify frames", false, cmd_ringverbose, "Usage: ringverbose [<on|off>]  (bare = toggle)" },
  { "ringquery",      "Send an R1 health/status request: ringquery <wear|health|hr|hrv|spo2|temp|activity|sleep|report|raw> [type] [hex_payload]", false, cmd_ringquery, "Usage: ringquery <wear|health|hr|hrv|spo2|temp|activity|sleep|report|raw> [args] | <hr|hrv|spo2|temp|activity|sleep> [daily|point|measure] | report <on|off|0xNN> | raw <module> <cmd> <subCmd> [hex_payload] [status=NN]" },
  // NOTE: `ringtoglasses` and `ringbridge` are UNREGISTERED on purpose.
  // Both targeted getting ring data onto the G2's built-in health UI, and
  // both are dead ends. See R1_RING_PROTOCOL.md §13 for the full writeup:
  //   * ringtoglasses (postman) — pushed RingDataPackage on sid=0x90 to the
  //     right temple. Temple silently ignores; the community RE codebases
  //     never send in that direction either (sid=144/145 are temple→host,
  //     not host→temple).
  //   * ringbridge   (matchmaker) — issued RING_CONNECT_INFO so the temple
  //     bonds with the ring directly. Fails with connRet=8 (terminal)
  //     because the R1 firmware accepts only one BLE central at a time
  //     and we still hold the link. Releasing the ESP32's link doesn't
  //     help either (verified empirically; see §13).
  // The implementations of `cmd_ringtoglasses` / `cmd_ringbridge` and their
  // helpers (spoof task, bridge-heartbeat task, telemetry cache,
  // g2BuildRingRawDataPush) are kept compiled-in but unreachable, so any
  // future investigator can re-register them and pick up where we stopped
  // without re-implementing the plumbing.
};
extern const size_t g2RingCommandsCount =
    sizeof(g2RingCommands) / sizeof(g2RingCommands[0]);

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
