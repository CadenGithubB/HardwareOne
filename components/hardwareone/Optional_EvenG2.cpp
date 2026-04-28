// =============================================================================
// Even Realities G2 glasses — BLE client
// =============================================================================
// Rewrite of the earlier stub-level integration against the protocol described
// in https://github.com/Commute773/g2-kit-unofficial — wire primitives live in
// System_G2_Protocol.{h,cpp}; this file handles the BLE state machine,
// dual-temple connection management, session prelude, and heartbeat task.

#include "Optional_EvenG2.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <stdarg.h>

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_gap_ble_api.h>  // esp_ble_gap_update_conn_params (HIGH priority)

#include "System_G2_Protocol.h"
#include "Optional_Bluetooth.h"
#include "System_Debug.h"
#include "System_Command.h"
#include "System_Utils.h"
#include "System_Notifications.h"
#include "System_MemUtil.h"
#include "System_VFS.h"
#include "WebServer_Server.h"  // broadcastEventToAllSessions() for SSE push
#include "BLE_Events.h"        // CompactJson + blePushEvent
#include "BLE_Peers.h"         // peer registry + saved-MAC reconnect
#include "Optional_EvenG2_Ring.h"  // g2RingInit (eager registration in initG2Client)
#include "G2_Page_Sensors.h"   // g2ShowSensorList() (per-page module)
#include "G2_Page_System.h"    // g2ShowSystemPage()
#include "G2_Page_Network.h"   // g2ShowNetworkMenu / g2NetworkHandleTap
#include "G2_Page_Settings.h"  // g2ShowSettingsMenu / g2SettingsHandleTap
#include "G2_Page_Files.h"     // g2ShowFilesMenu / g2FilesHandleTap / g2FilesTick
#include "G2_Page_TestSuite.h" // g2ShowTestSuiteMenu / g2TestSuiteHandleTap
#include "System_Settings.h"
#if ENABLE_WIFI
#include <WiFi.h>
#endif
#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/semphr.h>
#include <ctype.h>
#include <string.h>

// =============================================================================
// Constants
// =============================================================================

// GATT UUIDs — verified against a real pair of Even G2 glasses. The command
// characteristics (write 5401, notify 5402) live under service 5450, NOT
// under a ".._0000" service as some earlier notes suggested.
//
// The glasses also expose:
//   …e6450   — render/audio service (write 6401 / notify 6402 — LC3 mic)
//   …e7450   — unknown (not yet used)
//   …e1001   — unknown (not yet used)
//   6e400001-…  — Nordic UART Service (standard, not used by this integration)
static constexpr const char* SERVICE_UUID      = "00002760-08c2-11e1-9073-0e8ac72e5450";
static constexpr const char* CHAR_WRITE_UUID   = "00002760-08c2-11e1-9073-0e8ac72e5401";
static constexpr const char* CHAR_NOTIFY_UUID  = "00002760-08c2-11e1-9073-0e8ac72e5402";

// Diagnostic services. Both reference repos (g2-kit-unofficial,
// even-g2-protocol) ignore these — their characteristic UUIDs and
// payload semantics aren't documented anywhere public. We subscribe
// blind: enumerate notify chars, register a logger, and hex-dump
// whatever arrives. Goal is to spot async events (wear-detect, motion,
// proximity) that aren't surfacing on the main command channel —
// e.g. why "put glasses back on" doesn't wake the plugin task.
//
// 6450 is included even though we know it carries LC3 mic audio on
// 6402 — a partial third-party reference flagged a separate notify
// char at ATT handle 0x0884 (UUID unidentified) whose handle position
// suggests it lives on 6450 alongside 6402. Audio-off is our default
// state so the diag dump won't flood; once audio is enabled the diag
// path will pick up LC3 frames (one log line each, 205 B preview) and
// we'll want to gate that off to avoid log saturation.
static constexpr const char* DIAG_SVC_6450     = "00002760-08c2-11e1-9073-0e8ac72e6450";
static constexpr const char* DIAG_SVC_7450     = "00002760-08c2-11e1-9073-0e8ac72e7450";
static constexpr const char* DIAG_SVC_1001     = "00002760-08c2-11e1-9073-0e8ac72e1001";

// Heartbeat cadence — glasses kill the plugin task after ~10 s of silence.
// 5 s gives headroom without hammering the radio.
static constexpr uint32_t HEARTBEAT_PERIOD_MS = 5000;

// Primary text container name. Fixed per-session; rebuild semantics depend
// on reusing the same name. Keep ≤14 chars, case-sensitive.
static constexpr const char* CONTAINER_NAME = "app";

// Requested ATT MTU. The glasses typically negotiate down to 247/244.
static constexpr uint16_t MTU_TARGET = 244;

// Reassembly buffer size per temple. The largest envelope we expect is an
// image raw-data fragment (~4 KB body + header). Round up.
static constexpr size_t RX_ASSEMBLY_CAP = 8192;

// Scan window (milliseconds). The scan itself runs on the BLE task, not on
// the command-handler task, so a longer window is fine. We also spawn the
// whole connect flow into a background FreeRTOS task so `openg2` returns
// immediately from the CLI.
static constexpr uint32_t SCAN_DURATION_MS = 12000;

// =============================================================================
// Per-temple state
// =============================================================================

struct G2Temple {
  char                         side;            // 'L' or 'R'
  BLEClient*                   client;
  BLERemoteCharacteristic*     writeChar;
  BLERemoteCharacteristic*     notifyChar;
  BLEAdvertisedDevice*         advertisedDevice;
  String                       deviceName;
  String                       deviceAddress;
  uint16_t                     mtu;
  bool                         connected;
  // Set true when the peer disconnects unexpectedly — the next connect
  // attempt will destroy and recreate the BLEClient rather than reuse the
  // stale one (which Arduino BLE handles unreliably).
  bool                         clientStale;

  // Reassembly — the firmware groups fragments by the 'seq' byte. Start
  // collecting on the first fragment; dispatch when totalLen bytes received.
  uint8_t*                     rxBuf;
  size_t                       rxCap;
  size_t                       rxHave;
  size_t                       rxExpected;
  uint8_t                      rxSeq;
  bool                         rxActive;

  // Write serialisation — BLE writes must not interleave mid-envelope, and
  // heartbeat must not step on a text rebuild in progress.
  SemaphoreHandle_t            writeMutex;

  // Monotonic counters.
  uint32_t                     packetsSent;
  uint32_t                     packetsReceived;
  uint32_t                     heartbeatCounter;

  // Heartbeat-ack watchdog. Incremented each time the periodic timer sends
  // a heartbeat without seeing an ack since the previous send; reset to 0
  // on every incoming HeartbeatAck (Cmd=12 → res=12 HeartbeatSuccess) on
  // this temple. Three consecutive misses (~15 s) means the plugin task
  // has died — typically from a bad REBUILD/UPDATE against the wrong
  // container type — and we should stop spamming commands until reconnect.
  // Caught us before: UPDATE_TEXT on a ListContainer froze every ack
  // including heartbeats; without this counter that state went silent
  // forever. (2026-04-24 incident.)
  uint32_t                     heartbeatMissed;
  bool                         pluginDead;

  // True once a CREATE_STARTUP_PAGE has been sent and acked this session.
  // Subsequent g2ShowText calls send REBUILD_PAGE against the existing
  // container when set; otherwise they send a fresh CREATE and block on its
  // ack. Cleared on disconnect and on DISPLAY_OFF (the firmware tears the
  // container down server-side at that point). Only the right temple is
  // ever asked to render, so in practice only gR.containerReady is
  // consulted, but we track it per-temple for symmetry with future L-drive
  // experiments.
  bool                         containerReady;
  // True when the live container was CREATEd as a List widget (hijack
  // menu), false when CREATEd as a Text widget (g2show / g2notify path).
  // The firmware rejects REBUILDs that change widget type — REBUILDing
  // a TEXT into a list container makes it bail out with "lost connection"
  // on the lens. Used by g2ShowText to auto-route through
  // g2ShowTextAsList when the live container is a list. Only meaningful
  // when containerReady=true.
  bool                         containerIsList;
};

// =============================================================================
// BLE peer registry binding
// =============================================================================
// Stable file-statics so bleRegisterPeer can hold pointers across the
// session without us juggling lifetimes. See BLE_Peers.h for the
// registry contract. Thunks adapt our internal API (which can't be
// pointed to directly because g2Connect/g2Disconnect take/return G2-
// specific types) to the registry's plain function-pointer slots.
static bool g2PeerConnectSavedThunk() { return g2ConnectSaved(); }
static void g2PeerDisconnectThunk()   { g2Disconnect(); }
static bool g2PeerIsConnectedThunk()  { return isG2Connected(); }
static const BlePeerOps g2PeerOps = {
  g2PeerConnectSavedThunk,
  g2PeerDisconnectThunk,
  g2PeerIsConnectedThunk,
};
static const BlePeerSpec g2PeerSpec = {
  BLE_PEER_G2_GLASSES,
  "g2-glasses",
  "G2 Glasses",
  /*macCount=*/2,
  /*connectable=*/true,
  &g2PeerOps,
};

// Registers the built-in G2 pages with the page registry. Called from
// initG2Client. Defined further down (alongside the kPage specs).
static void registerG2Pages(void);

static G2Temple gL;
static G2Temple gR;

// =============================================================================
// Frame ring buffer — post-mortem debugging
// =============================================================================
//
// When the glasses' firmware bails ("connection issue" on-lens), we need
// to see which packet(s) it took issue with. BLE disconnect callback fires
// before we learn why; by the time the user checks the log, the interesting
// frames have scrolled off.
//
// Every envelope we TX or RX gets a one-line record in a small ring.
// Recorded fields are cheap to capture; head bytes help recognise the pb
// shape at a glance without a full decoder. Ring dumps on:
//   * BLE disconnect (any arm)
//   * Heartbeat watchdog transition (plugin declared dead)
//   * Any EvenCore response with res != 0 (Rebuild/Text/Shutdown failures)
//   * Manual `g2dumpframes` CLI command
//
// Size: 32 entries × ~32 bytes = 1 KB static. Fine.

struct G2FrameEvent {
  uint32_t tsMs;
  char     side;        // 'L', 'R', or '-' if unknown
  char     dir;          // 'T' (TX), 'R' (RX)
  uint8_t  sid;
  uint8_t  flag;
  uint16_t pbLen;
  uint32_t cmd;          // first pb varint (typically Cmd) — 0 if not applicable
  uint32_t magic;        // second pb varint — 0 if not present
  uint8_t  headLen;
  uint8_t  head[12];
};

static constexpr size_t G2_FRAME_RING_CAP = 32;
static G2FrameEvent gFrameRing[G2_FRAME_RING_CAP];
static size_t       gFrameRingHead  = 0;     // next write slot
static size_t       gFrameRingCount = 0;     // saturates at CAP

// Best-effort extraction of (cmd, magic) from an envelope body. Only
// meaningful for sid=0xE0 frames that start with `08 <cmd> 10 <magic>`,
// but safe to call on anything — returns (0,0) when the bytes don't match
// that shape.
static void g2RingPeekCmdMagic(const uint8_t* pb, size_t pbLen,
                               uint32_t* outCmd, uint32_t* outMagic) {
  *outCmd = 0;
  *outMagic = 0;
  if (!pb || pbLen < 4) return;
  size_t pos = 0;
  uint32_t field; uint8_t wire;
  uint64_t v;
  if (!g2PbReadTag(pb, pbLen, &pos, &field, &wire)) return;
  if (field != 1 || wire != G2_PB_WIRE_VARINT) return;
  if (!g2PbReadVarint(pb, pbLen, &pos, &v)) return;
  *outCmd = (uint32_t)v;
  if (!g2PbReadTag(pb, pbLen, &pos, &field, &wire)) return;
  if (field != 2 || wire != G2_PB_WIRE_VARINT) return;
  if (!g2PbReadVarint(pb, pbLen, &pos, &v)) return;
  *outMagic = (uint32_t)v;
}

// Record one TX or RX envelope in the ring. `env` points at the full
// on-wire envelope (starts with AA 21 or AA 12). Safe to call from notify
// callbacks, write path, and CLI context.
static void g2RingRecord(char side, char dir,
                         const uint8_t* env, size_t envLen) {
  if (!env || envLen < G2_ENVELOPE_HDR_LEN) return;
  G2FrameEvent& e = gFrameRing[gFrameRingHead];
  gFrameRingHead = (gFrameRingHead + 1) % G2_FRAME_RING_CAP;
  if (gFrameRingCount < G2_FRAME_RING_CAP) gFrameRingCount++;
  e.tsMs   = millis();
  e.side   = side ? side : '-';
  e.dir    = dir;
  e.sid    = env[6];
  e.flag   = env[7];
  // pb body sits between the 8-byte header and the 2-byte CRC trailer.
  const size_t declared = env[3];
  const size_t pbBytes  = (declared > G2_ENVELOPE_CRC_LEN)
                          ? (declared - G2_ENVELOPE_CRC_LEN) : 0;
  e.pbLen = (uint16_t)pbBytes;
  const uint8_t* pb = env + G2_ENVELOPE_HDR_LEN;
  g2RingPeekCmdMagic(pb, pbBytes, &e.cmd, &e.magic);
  const size_t copy = pbBytes < sizeof(e.head) ? pbBytes : sizeof(e.head);
  e.headLen = (uint8_t)copy;
  memcpy(e.head, pb, copy);
}

// Dump the ring oldest → newest. `reason` is free-form context ("BLE
// disconnect", "plugin dead", etc.) shown in the header.
// Forward decls for the handful of later-defined globals we'd like to
// surface in the ring dump. C++ doesn't let us forward-declare
// file-scope statics, so we read them via a tiny accessor that's
// defined later in the file where the actual storage is in scope.
// Returns 0 for sessionStartMs / hijackStartMs when unavailable, false
// for hijackActive.
static void g2GetSessionContext(uint32_t* sessionStartMs,
                                uint32_t* hijackStartMs,
                                bool* hijackActive);

static void g2RingDump(const char* reason) {
  if (gFrameRingCount == 0) {
    DEBUG_G2F("[G2-DUMP] (empty) reason=%s", reason ? reason : "?");
    return;
  }
  const uint32_t now = millis();
  uint32_t sessStartMs = 0, hijackStartMs = 0;
  bool hijackActive = false;
  g2GetSessionContext(&sessStartMs, &hijackStartMs, &hijackActive);
  const uint32_t sessionMs = (sessStartMs > 0) ? (now - sessStartMs) : 0;
  const uint32_t hijackMs  = (hijackActive && hijackStartMs > 0)
                             ? (now - hijackStartMs) : 0;
  DEBUG_G2F("[G2-DUMP] reason=%s count=%u session=%u.%03us hijack=%s",
            reason ? reason : "?",
            (unsigned)gFrameRingCount,
            (unsigned)(sessionMs / 1000), (unsigned)(sessionMs % 1000),
            hijackActive ? "active" : "off");
  if (hijackActive) {
    DEBUG_G2F("[G2-DUMP]   hijack-alive=%u.%03us",
              (unsigned)(hijackMs / 1000), (unsigned)(hijackMs % 1000));
  }
  const size_t start = (gFrameRingHead + G2_FRAME_RING_CAP - gFrameRingCount)
                       % G2_FRAME_RING_CAP;
  for (size_t i = 0; i < gFrameRingCount; i++) {
    const G2FrameEvent& e = gFrameRing[(start + i) % G2_FRAME_RING_CAP];
    char hex[64]; size_t hp = 0;
    for (size_t b = 0; b < e.headLen && hp + 3 < sizeof(hex); b++) {
      hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X ", e.head[b]);
    }
    hex[hp > 0 ? hp - 1 : 0] = '\0';
    const uint32_t ageMs = now - e.tsMs;
    const uint32_t sessT = (sessStartMs > 0 && e.tsMs >= sessStartMs)
                           ? (e.tsMs - sessStartMs) : 0;
    DEBUG_G2F("[G2-DUMP]   %c%c t-%ums T+%u.%03us sid=%02X flag=%02X "
              "cmd=%u magic=%u pb=%u [%s]",
              e.dir, e.side, (unsigned)ageMs,
              (unsigned)(sessT / 1000), (unsigned)(sessT % 1000),
              e.sid, e.flag,
              (unsigned)e.cmd, (unsigned)e.magic,
              (unsigned)e.pbLen, hex);
  }
}

// =============================================================================
// Aggregate state
// =============================================================================

static G2ClientState* gG2State = nullptr;
static BLEScan*       gScan    = nullptr;

// Navigation-mode toggle — consumed by Optional_Bluetooth.cpp. Kept as an
// extern so the old UI wiring continues to work.
bool gG2MenuNavEnabled = false;

// Heartbeat timer. One timer beats both temples on the same tick.
static TimerHandle_t  gHeartbeatTimer = nullptr;

// Seq allocator — incrementing u8 wraps naturally. Each new logical message
// uses a fresh seq so the firmware can group its fragments.
static uint8_t        gNextSeq = 1;

// Magic allocator — effectively u8. Wrap at 0xFF → 1 (avoid 0).
static uint8_t        gNextMagic = 1;

// Scan target during g2Connect.
static G2Eye          gConnectTarget = G2_EYE_AUTO;
static volatile bool  gScanFoundL = false;
static volatile bool  gScanFoundR = false;

// Ring discovery — the R1 ring advertises as "EVEN R1_<6 hex>" alongside
// the temple adverts. We tag finds in the same scan pass so ringConnect()
// can pick up without a second scan, but we don't auto-connect (ring
// needs its own handshake separate from the glasses' AppLaunch flow).
// The ring's BLE layer lives in Optional_EvenG2_Ring.cpp; here we just
// stash the advertisedDevice pointer for it to claim.
extern BLEAdvertisedDevice* gRingAdvertisedDevice;  // defined in _Ring.cpp
extern String               gRingDeviceName;
extern String               gRingDeviceAddress;
extern volatile bool        gRingScanFound;

// Async connect-task state. cmd_g2connect spawns a background task so the
// CLI handler returns immediately — otherwise a scan+connect longer than
// 10 s triggers the command-exec timeout, which has a use-after-free on
// the request struct (pre-existing codebase bug).
static volatile bool  gConnectTaskActive = false;
static volatile bool  gConnectCancel     = false;
static TaskHandle_t   gConnectTaskHandle = nullptr;

// Event callback registered by the caller.
static G2EventCallback gEventCallback = nullptr;

// Last-known battery percentage per temple, -1 if unknown. Updated from the
// sid=0x09 response handler; queried via g2battery CLI / status string.
static int8_t gBatteryL = -1;
static int8_t gBatteryR = -1;
// Silent / DND mode mirror. -1 = unknown (no push received yet),
// 0 = silent off, 1 = silent on. Updated when the firmware pushes a
// sid=0x09 cmd=3 deviceSendInfoToApp{f2=N} packet — happens whenever
// the user toggles silent mode via tap-and-hold-both-temples gesture.
// Surfaced in the g2-status SSE so web clients can render a DND
// indicator and route notifications accordingly.
static int8_t gSilentMode = -1;

// Forward decl for handleDevEvent's SYSTEM_EXIT shutdown-echo guard. The
// variable's full definition lives down with the page-swap worker — it's
// stamped to millis() right before our own intentional ShutdownPage so the
// SYSTEM_EXIT handler can distinguish "firmware acknowledging our teardown"
// (transient, hijack stays active) from "firmware tore down on its own"
// (real exit, clear state). See pageSwapListWorker.
static volatile uint32_t gOurShutdownAtMs;

// Firmware version string (shared across temples — they report the same
// version, so one cache suffices). Populated from sid=0x09 pushes via
// g2ParseSettingVersion. Empty until the firmware sends its first
// version-bearing settings push (not every settings push carries it).
static char gFwVersion[32] = {0};

// Runtime toggle for the settings-field verbose dumper. When on, each
// incoming sid=0x09 push is walked field-by-field and every unrecognised
// field logged — useful for discovering what firmware data we haven't
// mapped yet. Default off so normal operation isn't noisy.
static bool gG2SettingsVerbose = false;

// ── Blocks-widget hijack state ──────────────────────────────────────────────
// When the user taps "Blocks" in the G2 side menu, the glasses emit a
// sid=0xE0 cmd=17 MenuStartUpEvent with widgetId=10509. We hijack that tap
// to push a dashboard-style status page instead of letting the built-in
// Blocks mini-app run. The hijack is unconditional (see plan §Scope) —
// when the ESP32 is connected to the glasses, the phone isn't, so tapping
// Blocks is effectively a shortcut to the ESP32's own status dashboard.
//
// Lifecycle:
//   gHijackActive = true   once g2ShowText(snapshot) returns OK
//   cleared on DISPLAY_OFF (the firmware tears down the container on its
//   side, so we send a matching Cmd=9 ShutdownPage to free the cached
//   containerReady flag), on BLE disconnect, or by a 60-second safety
//   fallback piggybacked on the heartbeat timer for the rare case where
//   DISPLAY_OFF is missed.
static constexpr uint32_t BLOCKS_WIDGET_ID   = 10509;
static constexpr uint32_t HIJACK_SAFETY_MS   = 60000;
static volatile bool      gHijackActive      = false;
static uint32_t           gHijackStartedMs   = 0;

// Last millis() at which we observed a SysEvent with EventSource=2
// (ring). Set by the SysEvent dispatcher in handleDevEvent. Read by
// buildG2StatusSnapshot to render the "ring linked to glasses" line.
// Distinct from gRing.connected (direct BLE-to-ring); this tracks
// ring↔glasses pairing health, which is what actually matters for
// gestures-via-glasses since that's where ring inputs surface.
//
// Initialized to 0 = "never seen." Wraps after 49.7 days; the
// "last seen" math uses unsigned subtraction so wrap-around stays
// monotonic.
uint32_t gRingViaGlassesLastSeenMs = 0;

// Forward declarations for text-view state, defined alongside the
// page-swap worker further down. Referenced earlier by the SysEvent /
// USER_ACTIVITY exit hooks in handleDevEvent and dispatchEventPayload.
static volatile bool     gTextViewActive;

// Image-probe hold state. When an image probe wants to display a BMP
// "until the user taps", it sets gImgProbeHoldActive=true and polls
// gImgProbeHoldTapPending. The SysEvent handler, when it sees a
// CLICK/SCROLL/DOUBLE_CLICK on sid=0xE0 during the hold, sets the
// pending flag — same pattern as gTextViewActive but without an
// exit-callback (the probe worker reads the flag itself).
static volatile bool     gImgProbeHoldActive     = false;
static volatile bool     gImgProbeHoldTapPending = false;
// Wall-clock millis() when gTextViewActive went true. The firmware fires
// a spontaneous USER_ACTIVITY beacon ~150-300 ms after CREATE-text acks
// (observed 2026-04-26: text view dismissed itself before the user
// could read it). Anything arriving inside this grace window after
// activation is treated as the firmware's own settle-event and ignored
// — only events that arrive AFTER the grace count as "user dismissed".
static volatile uint32_t gTextViewActivatedMs;
static const  uint32_t kTextViewExitGraceMs = 600;
static void (*gTextViewExitFn)();
// Optional page-navigation handler for paginated TEXT views. Receives a
// G2TapKind directional hint (NEXT/PREV) so the consumer can map ring
// scroll-down/up to forward/backward navigation. Each tap re-stamps
// gTextViewActivatedMs so successive gestures each get their own grace
// window. Real exit gestures (DOUBLE_CLICK on sid=0xE0) still route to
// gTextViewExitFn.
static G2TapFn gTextViewTapFn;

// Implements the forward-declared accessor used by g2RingDump (which
// lives above this section so its own TX/RX hooks wire up early in the
// file). Simple wrapper around the static globals.
static void g2GetSessionContext(uint32_t* sessionStartMs,
                                uint32_t* hijackStartMs,
                                bool* hijackActive) {
  if (sessionStartMs) *sessionStartMs = (gG2State) ? gG2State->connectedSince : 0;
  if (hijackStartMs)  *hijackStartMs  = gHijackStartedMs;
  if (hijackActive)   *hijackActive   = gHijackActive;
}

// Single-slot ack waiter for the first CREATE_STARTUP_PAGE of a session.
// Reference: `await session.sendPb(...)` in g2-kit-unofficial/examples/
// hello-text.ts — the first CREATE MUST ack before subsequent page ops
// will land. We block g2ShowText's first call on this.
//
// Only one CREATE is ever in flight at a time (guarded by the plan's
// lifecycle: CREATE is only sent when containerReady is false, and the
// waiter clears that flag only after ack-or-timeout completes), so a
// single shared slot is sufficient. The notify task signals via
// gCreateAckSem inside handleEnvelope() when it sees a Cmd=1 response
// whose MagicRandom low byte matches gExpectMagic.
static SemaphoreHandle_t gCreateAckSem = nullptr;
static volatile uint8_t  gExpectMagic  = 0;
static volatile bool     gCreateOk     = false;

// REBUILD ack tracking — counterpart to gCreateAckSem for Cmd=8
// RebuildResp. Used by the experimental REBUILD-list path gated by
// gG2ListRebuildEnabled. Only one REBUILD in flight at a time
// (single-slot, same as CREATE).
static SemaphoreHandle_t gRebuildAckSem      = nullptr;
static volatile uint8_t  gExpectRebuildMagic = 0;
static volatile bool     gRebuildOk          = false;

// Forward declarations for live-list page primitive (defined further
// below near g2ShowTextAsList). The SysEvent handler in handleEnvelope
// — which sits well above the primitive's definition — checks whether
// a live page is active and kicks an immediate refresh on double-tap.
// Function indirection avoids forward-declaring file-scope statics.
static bool livePageIsActive();
static void livePageKickRefresh();

// Page-swap fast path toggle: when true, worker uses Cmd=7 REBUILD-list
// to swap items in place on an existing list container instead of the
// standard SHUTDOWN+CREATE cycle. **Default ON as of 2026-04-27.** The
// prior 2026-04-25 doc claim that REBUILD-list with mismatched item sets
// crashes the firmware plugin task was disproven empirically against
// firmware 2.2.0.24: tested 7→2, 2→7, 7→6, 6→7, 7→9, 9→7, 7→12, 12→7
// in rapid succession with all RebuildResp res=6 (RebuildSuccess) and
// no firmware wedge. REBUILD-list completes in ~70 ms vs ~700 ms for
// SHUTDOWN+CREATE — visibly flicker-free menu navigation. The original
// goal was scroll-position preservation across nav; firmware does NOT
// honor that on REBUILD (cursor resets to row 0), but the no-flicker
// side benefit is reason enough to keep the path on. Toggle remains
// runtime-tunable as a safety hatch in case a future firmware revision
// regresses.
static volatile bool gG2ListRebuildEnabled = true;

// Image-push ack tracking. Counterpart to gCreateAckSem but for the
// Cmd=4 ImageRawResp acks that follow each Cmd=3 push. Each push in a
// burst gets its own MagicRandom; the firmware echoes the low byte
// back. We register an inclusive low/high window of expected magics
// and signal gImgPushAckSem once gImgPushAcked reaches gImgPushTarget.
// All values written-once by the worker before the push burst, then
// read by the BLE notify task; volatile is sufficient because the
// worker does a sync-wait on the sem (no concurrent write/read after
// arming).
static SemaphoreHandle_t gImgPushAckSem    = nullptr;
static volatile uint8_t  gImgPushExpectLo  = 0;
static volatile uint8_t  gImgPushExpectHi  = 0;
static volatile unsigned gImgPushTarget    = 0;
static volatile unsigned gImgPushAcked     = 0;

// =============================================================================
// Forward decls
// =============================================================================

static bool connectTemple(G2Temple& t);
static void disconnectTemple(G2Temple& t);
static bool sendEnvelope(G2Temple& t, const uint8_t* data, size_t len);
static bool sendToBoth(const uint8_t* data, size_t len);
static void handleNotify(G2Temple& t, const uint8_t* data, size_t len);
static void handleEnvelope(G2Temple& t, const G2EnvelopeView& env);
static void heartbeatTimerCallback(TimerHandle_t xTimer);
static void startHeartbeatTimer();
static void stopHeartbeatTimer();
static void templeInit(G2Temple& t, char side);
static void templeReset(G2Temple& t);
static void g2PushStatusEvent(const char* reason);
static const char* osEventTypeName(uint32_t ev);
static bool shouldDedupHijackTap(uint32_t idx);
static void handleHijackMenuTap(uint32_t idx);
// Shutdown+CREATE handshake helpers — defined alongside g2ShowText
// (bottom of file) but referenced earlier by the Blocks-hijack worker.
static bool sendCreateAndWait(G2Temple& arm, const char* text, uint32_t widgetId);
static bool sendCreateListAndWait(G2Temple& arm,
                                  const char* const* items, size_t itemCount,
                                  uint32_t widgetId,
                                  const G2ContainerGeom& geom);
static bool sendCreateTextAndWait(G2Temple& arm,
                                  const char* text,
                                  uint32_t widgetId,
                                  const G2ContainerGeom& geom,
                                  bool eventCapture);
static bool sendShutdownAndSettle(G2Temple& arm, uint32_t settleMs);
static bool sendMenuFailedAndSettle(G2Temple& arm, uint32_t settleMs);

// =============================================================================
// Seq / magic allocation
// =============================================================================

static uint8_t allocSeq() {
  uint8_t s = gNextSeq++;
  if (s == 0) s = gNextSeq++;  // 0 is reserved for "no active reassembly"
  return s;
}

// Retained for future image-streaming use: `UpdateImageRawData` (Cmd=3)
// fragments each need a unique magic so the caller can match acks per
// fragment. All other message types now use the stable G2_MAGIC_* constants
// in System_G2_Protocol.h.
static uint8_t __attribute__((unused)) allocMagic() {
  uint8_t m = gNextMagic++;
  if (m == 0) m = gNextMagic++;
  return m;
}

// =============================================================================
// Advertisement name matching
// =============================================================================

// Matches "Even G1_12345_L_...", "G2_67890_R_...", etc. Returns 'L', 'R',
// or 0 if the name isn't a G2 temple advert. Case-insensitive on "even"
// prefix; temple side letter is case-sensitive uppercase per observed
// adverts.
static char classifyG2Name(const String& name) {
  if (name.length() == 0) return 0;
  const char* s = name.c_str();
  // Skip optional "Even " prefix
  if (strncasecmp(s, "Even ", 5) == 0) s += 5;
  // Expect "G<digits>_"
  if (*s != 'G') return 0;
  s++;
  if (!isdigit((unsigned char)*s)) return 0;
  while (isdigit((unsigned char)*s)) s++;
  if (*s != '_') return 0;
  s++;
  // digits (pair ID)
  if (!isdigit((unsigned char)*s)) return 0;
  while (isdigit((unsigned char)*s)) s++;
  if (*s != '_') return 0;
  s++;
  char side = *s++;
  if (side != 'L' && side != 'R') return 0;
  if (*s != '_') return 0;
  return side;
}

// Returns true if `name` matches the Even R1 ring advertising pattern.
// Reference regex: /^EVEN\s+R1_([0-9A-F]{6})$/i
// Example: "EVEN R1_BAAC1C" → last 6 hex = low 3 bytes of the ring's MAC.
// Case-insensitive on "EVEN" prefix per the reference (which uses /i).
static bool classifyRingName(const String& name) {
  if (name.length() == 0) return false;
  const char* s = name.c_str();
  // "EVEN" (any case)
  if (strncasecmp(s, "EVEN", 4) != 0) return false;
  s += 4;
  // Whitespace separator
  if (*s != ' ' && *s != '\t') return false;
  while (*s == ' ' || *s == '\t') s++;
  // "R1_"
  if (strncmp(s, "R1_", 3) != 0) return false;
  s += 3;
  // 6 hex digits to end-of-string
  int count = 0;
  while (*s) {
    if (!isxdigit((unsigned char)*s)) return false;
    s++;
    count++;
  }
  return count == 6;
}

// =============================================================================
// Scan callbacks
// =============================================================================

// Runtime-toggleable verbose scan logging. When on, every advert that has
// a name is printed — useful while debugging "I can't find my glasses"
// symptoms (regex mismatch vs. not advertising vs. scan not running).
static bool gG2ScanVerbose = true;

// Saved-MAC filter for the auto-reconnect path. When non-empty, only adverts
// whose MAC matches one of these (case-insensitive) are accepted. Name
// classification is bypassed entirely — even if the temple isn't broadcasting
// a recognizable name we can still reconnect to a previously paired pair.
// Cleared by g2ConnectSavedSync after each use so subsequent normal scans
// (pairing path) use the name-based behavior.
static String gG2FilterMacL = "";
static String gG2FilterMacR = "";

static bool macEqualsIgnoreCase(const String& a, const String& b) {
  if (a.length() == 0 || b.length() != a.length()) return false;
  for (size_t i = 0; i < a.length(); i++) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

class G2ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    const bool hasName = advertisedDevice.haveName();
    String name = hasName ? String(advertisedDevice.getName().c_str()) : String();
    char side = hasName ? classifyG2Name(name) : 0;
    bool isRing = hasName ? classifyRingName(name) : false;

    // Saved-MAC reconnect: override name-based classification with MAC match.
    // We trust the saved MAC over the advertised name in case the firmware
    // uses a different name across reboots (observed: name format changes
    // between firmware versions).
    if (gG2FilterMacL.length() > 0 || gG2FilterMacR.length() > 0) {
      String addr = advertisedDevice.getAddress().toString().c_str();
      if      (macEqualsIgnoreCase(addr, gG2FilterMacL)) { side = 'L'; isRing = false; }
      else if (macEqualsIgnoreCase(addr, gG2FilterMacR)) { side = 'R'; isRing = false; }
      else                                                { side = 0;   isRing = false; }
    }

    if (gG2ScanVerbose) {
      const char* matchStr = side ? (side == 'L' ? "LEFT" : "RIGHT")
                                  : (isRing ? "RING" : "no");
      DEBUG_G2F("[G2-SCAN] advert name='%s' addr=%s rssi=%d match=%s",
                hasName ? name.c_str() : "<unnamed>",
                advertisedDevice.getAddress().toString().c_str(),
                advertisedDevice.getRSSI(),
                matchStr);
    }

    // Ring hits: stash for the ring module to claim. Doesn't stop the
    // scan on its own (we might still be looking for a temple), but any
    // in-flight ringConnect() can pick it up.
    if (isRing && !gRingAdvertisedDevice) {
      gRingAdvertisedDevice = new BLEAdvertisedDevice(advertisedDevice);
      gRingDeviceName       = name;
      gRingDeviceAddress    = advertisedDevice.getAddress().toString().c_str();
      gRingScanFound        = true;
      DEBUG_G2F("[G2-SCAN] Found RING: %s @ %s (RSSI %d) — stashed",
                name.c_str(), gRingDeviceAddress.c_str(),
                advertisedDevice.getRSSI());
      // Don't return — fall through in case this callback came with
      // multiple matches on the same advert (won't happen in practice
      // but defensive against future scan-driver weirdness).
    }

    if (!side) return;
    DEBUG_G2F("[G2-SCAN] Found %c: %s (RSSI %d)", side, name.c_str(),
              advertisedDevice.getRSSI());

    G2Temple& t = (side == 'L') ? gL : gR;
    if (t.advertisedDevice) return;  // already have this side

    // Copy to heap — the scan system reuses its per-result object after this
    // callback returns, so we keep our own copy for later connect().
    t.advertisedDevice = new BLEAdvertisedDevice(advertisedDevice);
    t.deviceName    = name;
    t.deviceAddress = advertisedDevice.getAddress().toString().c_str();

    if (side == 'L') gScanFoundL = true;
    else             gScanFoundR = true;

    // Stop scanning early if we've found all we need.
    bool needL = (gConnectTarget != G2_EYE_RIGHT);
    bool needR = (gConnectTarget != G2_EYE_LEFT);
    if ((!needL || gScanFoundL) && (!needR || gScanFoundR)) {
      DEBUG_G2F("[G2-SCAN] All required temples found; stopping scan early");
      gScan->stop();
    }
  }
};

// =============================================================================
// Client callbacks (per temple)
// =============================================================================

class TempleClientCallbacks : public BLEClientCallbacks {
public:
  explicit TempleClientCallbacks(G2Temple* t) : temple(t) {}

  void onConnect(BLEClient* /*pClient*/) override {
    DEBUG_G2F("[G2-%c] BLE connected", temple->side);
  }

  void onDisconnect(BLEClient* /*pClient*/) override {
    const bool wasConnected = temple->connected;
    DEBUG_G2F("[G2-%c] BLE disconnected", temple->side);
    // Dump the recent-frame ring so we can see what we and the glasses
    // were exchanging right before the drop. Particularly useful when the
    // firmware bails with an on-lens "connection issue" — that's an
    // app-layer quit, not a BLE-link failure, and the packets leading up
    // to it tell us which op it took issue with.
    if (wasConnected) g2RingDump("BLE disconnect");
    temple->connected = false;
    temple->writeChar = nullptr;
    temple->notifyChar = nullptr;
    // Container is tied to the plugin task — task dies with the BLE link,
    // so force a fresh CREATE on reconnect.
    temple->containerReady = false;
    // Blocks-hijack state depends on the right temple being reachable to
    // send its Cmd=9 ShutdownPage on lens-close. If R drops (or both
    // drop), drop the flag here so the next reconnect + tap redoes the
    // full CREATE rather than assuming the container still exists.
    if (temple->side == 'R') gHijackActive = false;
    // Mark the client stale — next connect attempt will build a fresh one.
    // Reusing a BLEClient after an unexpected peer drop is unreliable on
    // Arduino BLE; the safer path is always a new client per reconnect.
    temple->clientStale = true;

    // Surface the drop at info level so it shows up in serial/web without
    // needing to poll g2status. Only broadcast if we were actually in a
    // connected state — skip the noise from init-time "not yet connected"
    // transitions.
    if (wasConnected) {
      BROADCAST_PRINTF("[G2] %s temple dropped",
                       temple->side == 'L' ? "LEFT" : "RIGHT");
    }

    // If neither temple is connected, roll back global state so the
    // heartbeat timer can stop itself.
    if (!gL.connected && !gR.connected) {
      if (gG2State) gG2State->state = G2_STATE_IDLE;
      stopHeartbeatTimer();
    }
    // Push the state blob to any open browsers so they can flip the
    // panel from green → gray without needing a page refresh.
    char reason[16];
    snprintf(reason, sizeof(reason), "disconnect-%c", temple->side);
    g2PushStatusEvent(reason);
  }

private:
  G2Temple* temple;
};

// =============================================================================
// Notify callback shim → handleNotify()
// =============================================================================

static void notifyThunkL(BLERemoteCharacteristic* /*c*/, uint8_t* data,
                         size_t len, bool /*isNotify*/) {
  handleNotify(gL, data, len);
}
static void notifyThunkR(BLERemoteCharacteristic* /*c*/, uint8_t* data,
                         size_t len, bool /*isNotify*/) {
  handleNotify(gR, data, len);
}

// =============================================================================
// Diagnostic enumeration of undocumented services 6450 / 7450 / 1001
// =============================================================================
// We previously subscribed to every notify char on these services to
// hex-dump traffic, hoping to catch wear-detect / motion / proximity
// events. Result: zero traffic across multiple test sessions, even
// during heavy interaction (taps, page swaps, SYSTEM_EXIT echoes,
// DISPLAY_OFF cycles). The chars exist, they advertise notify
// capability, the firmware just never emits anything on them. Likely
// vestigial. Runtime subscriptions stripped to keep the BLE notify
// task clean.
//
// One-shot enumeration kept — at connect time we still log each
// service's characteristic list (UUID + property bits), so future
// firmware revisions that DO start emitting on these channels are
// visible in the connect-log diff and we can re-add subscriptions
// targeted at the specific char.
static void enumerateDiagService(G2Temple& t, const char* svcUuid,
                                  const char* svcTag) {
  if (!t.client) return;
  BLERemoteService* svc = t.client->getService(BLEUUID(svcUuid));
  if (!svc) {
    DEBUG_G2F("[G2-%c] Diag svc %s: not present on peer", t.side, svcTag);
    return;
  }
  auto* chars = svc->getCharacteristics();
  if (!chars || chars->empty()) {
    DEBUG_G2F("[G2-%c] Diag svc %s: no characteristics", t.side, svcTag);
    return;
  }
  for (const auto& entry : *chars) {
    BLERemoteCharacteristic* ch = entry.second;
    if (!ch) continue;
    DEBUG_G2F("[G2-%c] Diag svc %s: char %s notify=%d indicate=%d "
              "read=%d write=%d writeNR=%d",
              t.side, svcTag, entry.first.c_str(),
              ch->canNotify() ? 1 : 0, ch->canIndicate() ? 1 : 0,
              ch->canRead() ? 1 : 0, ch->canWrite() ? 1 : 0,
              ch->canWriteNoResponse() ? 1 : 0);
  }
}

// =============================================================================
// Fragment reassembly + dispatch
// =============================================================================

static void handleNotify(G2Temple& t, const uint8_t* data, size_t len) {
  t.packetsReceived++;
  if (!t.rxBuf || !data || len == 0) {
    DEBUG_G2F("[G2-%c] RX ignored (rxBuf=%p data=%p len=%u)",
              t.side, t.rxBuf, data, (unsigned)len);
    return;
  }
  // Log raw notification bytes — show up to 32 so we can read response codes
  // at the tail of small EvenCore acks.
  char hex[128];
  size_t shown = len > 32 ? 32 : len;
  size_t hp = 0;
  for (size_t i = 0; i < shown && hp + 3 < sizeof(hex); i++) {
    hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X ", data[i]);
  }
  hex[hp > 0 ? hp - 1 : 0] = '\0';
  DEBUG_G2F("[G2-%c] RX notify len=%u head=[%s%s]",
            t.side, (unsigned)len, hex, len > shown ? "..." : "");

  // New envelope layout:
  //   [AA 12][seq][len][totFrags][fragIdx][sid][flag][pb...][crcLE on last frag]
  // where `len` = fragment data bytes (pb + CRC if last). For our current
  // single-fragment messages totFrags==1 and we can parse immediately.
  const bool looksLikeStart = (len >= G2_ENVELOPE_HDR_LEN &&
                               data[0] == G2_PREAMBLE_0 &&
                               (data[1] == G2_PREAMBLE_TX || data[1] == G2_PREAMBLE_RX));

  if (looksLikeStart) {
    const uint8_t declared = data[3];         // fragment data bytes
    const uint8_t totFrags = data[4];
    const uint8_t fragIdx  = data[5];
    const size_t total = G2_ENVELOPE_HDR_LEN + declared;
    if (total > t.rxCap) {
      DEBUG_G2F("[G2-%c] RX envelope too large (%u > %u)", t.side,
                (unsigned)total, (unsigned)t.rxCap);
      t.rxActive = false;
      return;
    }
    size_t take = len;
    if (take > total) take = total;
    memcpy(t.rxBuf, data, take);
    t.rxHave     = take;
    t.rxExpected = total;
    t.rxSeq      = data[2];
    t.rxActive   = true;
    // For multi-fragment messages we'd need to keep collecting here — the
    // reference firmware emits these rarely, so we only handle single
    // fragments for now. If totFrags>1 we still try to parse what we have
    // but the CRC check will fail, yielding a clear log.
    if (totFrags > 1) {
      DEBUG_G2F("[G2-%c] RX multi-fragment message (%u/%u) — not yet handled",
                t.side, (unsigned)fragIdx, (unsigned)totFrags);
    }
  } else if (t.rxActive) {
    if (t.rxHave + len > t.rxCap) {
      DEBUG_G2F("[G2-%c] RX reassembly overflow; dropping", t.side);
      t.rxActive = false;
      return;
    }
    memcpy(t.rxBuf + t.rxHave, data, len);
    t.rxHave += len;
  } else {
    return;
  }

  if (t.rxActive && t.rxHave >= t.rxExpected) {
    G2EnvelopeView env;
    if (g2ParseEnvelope(t.rxBuf, t.rxExpected, &env)) {
      DEBUG_G2F("[G2-%c] RX env seq=0x%02X %u/%u sid=0x%02X flag=0x%02X pbLen=%u",
                t.side, env.seq, env.fragIdx, env.totalFrags,
                env.sid, env.flag, (unsigned)env.payloadLen);
      g2statsRecordRx(env.sid, env.flag, env.payload, env.payloadLen);
      g2RingRecord(t.side, 'R', t.rxBuf, t.rxExpected);
      // ANY well-formed incoming envelope is proof the plugin task on
      // this arm is alive and servicing the BLE link. Clear the silent
      // watchdog state unconditionally — user put the glasses on and
      // woke the plugin up, or some other firmware activity brought it
      // back. More reliable than only keying off Cmd=12 HeartbeatAck,
      // which doesn't fire in all recovery scenarios (e.g. user
      // interaction on sid=0x0D while our heartbeat is mid-cycle).
      if (t.pluginDead || t.heartbeatMissed > 0) {
        if (t.pluginDead) {
          BROADCAST_PRINTF("[G2] %s temple plugin alive again "
                           "(RX on sid=0x%02X cleared silent flag)",
                           t.side == 'L' ? "LEFT" : "RIGHT",
                           (unsigned)env.sid);
          g2PushStatusEvent(t.side == 'L' ? "plugin-alive-L"
                                           : "plugin-alive-R");
        }
        t.pluginDead      = false;
        t.heartbeatMissed = 0;
      }
      handleEnvelope(t, env);
    } else {
      DEBUG_G2F("[G2-%c] RX parse FAILED (%u bytes) — CRC or length mismatch",
                t.side, (unsigned)t.rxHave);
    }
    t.rxActive = false;
    t.rxHave   = 0;
  }
}

// =============================================================================
// Envelope dispatch — decode the sid and either consume it or raise an event
// =============================================================================

// Cross-arm event dedup. Both temples mirror every state event within a
// few tens of milliseconds, so without this every user gesture would fire
// the callback twice. Key is (payloadLen, first ~8 payload bytes) + wall
// time; within a ~150 ms window from either arm, the second copy is
// dropped. This is loose on purpose: the firmware never emits legitimate
// distinct events that close together on the same channel.
static uint32_t gLastEventTimeMs = 0;
static uint8_t  gLastEventBytes[8] = {0};
static size_t   gLastEventLen = 0;

static bool shouldDedupEvent(const uint8_t* payload, size_t len) {
  const uint32_t now = millis();
  const size_t keep = len < sizeof(gLastEventBytes) ? len : sizeof(gLastEventBytes);
  const bool sameShape = (gLastEventLen == len) &&
                         (memcmp(gLastEventBytes, payload, keep) == 0);
  const bool withinWindow = (now - gLastEventTimeMs) < 150;
  if (sameShape && withinWindow) return true;
  gLastEventTimeMs = now;
  gLastEventLen = len;
  memcpy(gLastEventBytes, payload, keep);
  return false;
}

// Pull the multi-byte varint at offset `off` inside `payload`. Returns 0
// if out of bounds or malformed. Used by the sid=0x0D classifier to peek
// at inner field values without spinning up the full pb reader.
static uint32_t peekVarint(const uint8_t* payload, size_t len, size_t off) {
  uint32_t v = 0;
  uint32_t shift = 0;
  for (size_t i = 0; i < 5 && off + i < len; i++) {
    uint8_t b = payload[off + i];
    v |= (uint32_t)(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return v;
    shift += 7;
  }
  return 0;
}

// Classify sid=0x0D SysEvent payloads from empirical capture labelling
// (2026-04-24). Returns G2_EVENT_UNKNOWN for anything we haven't
// classified, so unfamiliar shapes stay visible in logs without firing
// a bogus callback. `outHint` (optional) is filled with a short label
// describing the specific subtype — useful in log strings when the enum
// itself collapses multiple wire shapes into the same G2EventType.
//
// Known shapes (all start with `08 01 1A <n>`):
//   n=0, 4B  → display off/on transition (empty body)
//   n=2, 6B  → SysEvent{EventType=1} — "generic user input"
//   n=3, 7B  → SysEvent{EventType=1, field_3={field_1=<varint>}} —
//              inner varint classes (empirical):
//                224  (0xE0)   — seen around widget-lifecycle boundaries
//                4094 (0x0FFE) — seen around custom/app boundaries
//                others        — unclassified, report raw
//   n=4, 8B  → SysEvent{EventType=1, EventSource=<n>}
static G2EventType classifyStateEvent(const uint8_t* payload, size_t len,
                                      char* outHint, size_t outHintCap) {
  if (outHint && outHintCap) outHint[0] = '\0';
  // All known shapes start with `08 01 1A <n>`.
  const bool headOk = (len >= 4 && payload[0] == 0x08 &&
                       payload[1] == 0x01 && payload[2] == 0x1A);
  if (!headOk) return G2_EVENT_UNKNOWN;
  const uint8_t innerLen = payload[3];

  // 4B empty body: display state transition.
  if (innerLen == 0 && len == 4) {
    if (outHint) snprintf(outHint, outHintCap, "display-off");
    return G2_EVENT_DISPLAY_OFF;
  }
  // 6B: SysEvent { field_3 = nested { f1=<code> } } (single-byte code).
  // Layout `08 01 1A 02 08 <code>`. Earlier the inner code was hardcoded
  // to 1 ("user-activity") which dropped real captures like
  // `08 01 1A 02 08 08` (code=8 = NAVIGATION app source) into UNKNOWN.
  // Allow any single-byte code; surface code=1 with the legacy
  // "user-activity" label so existing tooling that greps for it still
  // matches.
  if (innerLen == 2 && len == 6 && payload[4] == 0x08) {
    const uint8_t code = payload[5];
    if (outHint) {
      if (code == 1) snprintf(outHint, outHintCap, "user-activity");
      else           snprintf(outHint, outHintCap, "state-6B code=%u", (unsigned)code);
    }
    return G2_EVENT_USER_ACTIVITY;
  }
  // 6B with EventSource only (no EventType): `08 01 1A 02 10 <n>`. Observed
  // around "Hey Even" wake-word activations (src=7); other src values not
  // yet seen. Surface the source code in the hint so labelled captures can
  // pin down what each value means.
  if (innerLen == 2 && len == 6 && payload[4] == 0x10) {
    if (outHint) snprintf(outHint, outHintCap, "voice-source src=%u", (unsigned)payload[5]);
    return G2_EVENT_USER_ACTIVITY;
  }
  // 7B: SysEvent { field_3 = nested { f1=<code> } }.
  // Layout: `08 01 1A 03 08 <varint>` where the inner field-1 varint can
  // be one or more bytes (e.g. 1, 224 = `E0 01`, 4094 = `FE 1F`). The
  // earlier check `payload[5] == 0x01` hardcoded the inner value to 1
  // and dropped multi-byte codes into UNKNOWN — observed empirically when
  // wake-word fires inside a hijacked widget (code=224).
  if (innerLen == 3 && len == 7 && payload[4] == 0x08) {
    uint32_t inner = peekVarint(payload, len, 5);
    if (outHint) {
      // Known codes — empirical from labelled captures. Unknown codes
      // fall through to the generic "state-7B code=NNN" form so they
      // remain grep-able for future labelling.
      switch (inner) {
        case 224:
          // Wake-word activation inside a hijacked widget (firmware
          // signals "user just spoke 'Hey Even'" to the hosting app).
          snprintf(outHint, outHintCap, "wake-word code=%u", (unsigned)inner);
          break;
        case 266:
          // Tap-and-hold both temples — the silent-mode toggle gesture.
          // Always paired with a sid=0x09 cmd=3 deviceSendInfoToApp push
          // a few seconds later carrying the new on/off state.
          snprintf(outHint, outHintCap, "tap-hold-both code=%u (silent-mode toggle)",
                   (unsigned)inner);
          break;
        default:
          snprintf(outHint, outHintCap, "state-7B code=%u", (unsigned)inner);
          break;
      }
    }
    return G2_EVENT_USER_ACTIVITY;
  }
  // 8B: EventType=1 with explicit EventSource.
  if (innerLen == 4 && len == 8 && payload[4] == 0x08 && payload[5] == 0x01 &&
      payload[6] == 0x10) {
    if (outHint) snprintf(outHint, outHintCap, "user-activity src=%u", (unsigned)payload[7]);
    return G2_EVENT_USER_ACTIVITY;
  }
  // 9B: SysEvent { field_3 = nested { f1=<code>, f2=<src> } }. Observed
  // when wake-word fires while our hijack is up — firmware emits the
  // widget-lifecycle code (224) along with src=7 (voice). The code
  // varint is multi-byte so we walk past its continuation bytes to find
  // the f2 (`10`) tag rather than indexing at fixed offsets.
  if (innerLen == 5 && len == 9 && payload[4] == 0x08) {
    const uint32_t code = peekVarint(payload, len, 5);
    size_t off = 5;
    while (off < len && (payload[off] & 0x80)) off++;
    off++;  // step past last byte of the varint
    uint32_t src = 0;
    if (off + 1 < len && payload[off] == 0x10) {
      src = peekVarint(payload, len, off + 1);
    }
    if (outHint) snprintf(outHint, outHintCap, "state-9B code=%u src=%u",
                          (unsigned)code, (unsigned)src);
    return G2_EVENT_USER_ACTIVITY;
  }
  return G2_EVENT_UNKNOWN;
}

// Decode a Cmd=2 DevEvent (OS_NOITY_EVENT_TO_APP_PACKET) payload. This is
// the wrapper field 13 body, which is a SendDeviceEvent protobuf:
//   SendDeviceEvent { ListEvent=1, TextEvent=2, SysEvent=3 }
//
// Each sub-message's schema (EvenHub_pb.ts:861-1001):
//   List_ItemEvent { ContainerID=1, ContainerName=2, CurrentSelectItemName=3,
//                    CurrentSelectItemIndex=4, EventType=5 (OsEventTypeList) }
//   Text_ItemEvent { ContainerID=1, ContainerName=2, EventType=3 }
//   Sys_ItemEvent  { EventType=1, EventSource=2, IMUData=3, systemExitReason=4 }
//
// Returns true if the payload parses and something actionable fires; side
// effects: log one line per sub-event, and on SYSTEM_EXIT_EVENT(7) clear
// our hijack/container state because the firmware has torn the widget
// down and is about to show "Connection lost" on-lens.
// Decode a sid=0x01 flag=0x01 async frame. The firmware bursts these
// around every user gesture — typical shape is 19-21 byte pb bodies that
// we previously just logged as "sid=0x01 async pb=N" and ignored. This
// decoder pulls out the internal fields so labelled captures can tell us
// whether swipes / double-taps / etc. are distinguishable in this
// channel.
//
// Observed structure (pb, all fields optional because firmware varies):
//   outer { field_1 varint (cmd),
//           field_2 varint (large timestamp-like),
//           field_6 len-delim body:
//             { field_1 varint (counter, monotonically increases),
//               EITHER:
//                 field_3 len-delim { field_1 bytes (empty),
//                                     field_2 string (empty) }   ← idle beacon
//               OR:
//                 field_5 len-delim { field_1 varint (flag),
//                                     field_2 len-delim:
//                                       { field_1 varint (codeA),
//                                         field_2 varint (codeB) } }  ← gesture
//             }
//         }
//
// The {codeA, codeB} pair is where per-gesture discrimination MIGHT live.
// Empty values in unseen fields are left at 0 — caller logs whatever got
// filled.
static void decodeAppAsync(char side, uint8_t flag,
                           const uint8_t* payload, size_t len) {
  // Outer walk: capture cmd and the field-6 body.
  uint32_t cmd = 0;
  const uint8_t* body = nullptr;
  size_t bodyLen = 0;
  size_t pos = 0;
  while (pos < len) {
    uint32_t f; uint8_t w;
    if (!g2PbReadTag(payload, len, &pos, &f, &w)) break;
    if (f == 1 && w == G2_PB_WIRE_VARINT) {
      uint64_t v;
      if (!g2PbReadVarint(payload, len, &pos, &v)) break;
      cmd = (uint32_t)v;
    } else if (f == 6 && w == G2_PB_WIRE_LEN_DELIM) {
      uint64_t bl;
      if (!g2PbReadVarint(payload, len, &pos, &bl)) break;
      if (pos + bl > len) break;
      body = payload + pos;
      bodyLen = (size_t)bl;
      pos += (size_t)bl;
    } else {
      if (!g2PbSkipField(payload, len, &pos, w)) break;
    }
  }
  if (!body) {
    DEBUG_G2F("[G2-%c] sid=0x01 async flag=0x%02X cmd=%u pb=%u (no body field 6)",
              side, flag, (unsigned)cmd, (unsigned)len);
    return;
  }

  // Body walk: counter + either field 3 (idle beacon) or field 5 (gesture).
  uint32_t counter = 0;
  const uint8_t* sub = nullptr;
  size_t subLen = 0;
  uint32_t subField = 0;
  size_t bp = 0;
  while (bp < bodyLen) {
    uint32_t bf; uint8_t bw;
    if (!g2PbReadTag(body, bodyLen, &bp, &bf, &bw)) break;
    if (bf == 1 && bw == G2_PB_WIRE_VARINT) {
      uint64_t v;
      if (!g2PbReadVarint(body, bodyLen, &bp, &v)) break;
      counter = (uint32_t)v;
    } else if ((bf == 3 || bf == 5) && bw == G2_PB_WIRE_LEN_DELIM) {
      uint64_t sl;
      if (!g2PbReadVarint(body, bodyLen, &bp, &sl)) break;
      if (bp + sl > bodyLen) break;
      sub = body + bp;
      subLen = (size_t)sl;
      subField = bf;
      bp += (size_t)sl;
    } else {
      if (!g2PbSkipField(body, bodyLen, &bp, bw)) break;
    }
  }

  if (!sub) {
    DEBUG_G2F("[G2-%c] sid=0x01 async cmd=%u counter=%u pb=%u (no inner)",
              side, (unsigned)cmd, (unsigned)counter, (unsigned)len);
    return;
  }
  if (subField == 3) {
    DEBUG_G2F("[G2-%c] sid=0x01 async cmd=%u counter=%u idle-beacon",
              side, (unsigned)cmd, (unsigned)counter);
    return;
  }

  // subField == 5: gesture. Walk inner flag + codes{A,B}.
  uint32_t innerFlag = 0;
  uint32_t codeA = 0, codeB = 0;
  bool haveCodes = false;
  size_t sp = 0;
  while (sp < subLen) {
    uint32_t sf; uint8_t sw;
    if (!g2PbReadTag(sub, subLen, &sp, &sf, &sw)) break;
    if (sf == 1 && sw == G2_PB_WIRE_VARINT) {
      uint64_t v;
      if (!g2PbReadVarint(sub, subLen, &sp, &v)) break;
      innerFlag = (uint32_t)v;
    } else if (sf == 2 && sw == G2_PB_WIRE_LEN_DELIM) {
      uint64_t cl;
      if (!g2PbReadVarint(sub, subLen, &sp, &cl)) break;
      if (sp + cl > subLen) break;
      const uint8_t* codes = sub + sp;
      size_t cp = 0;
      while (cp < (size_t)cl) {
        uint32_t cf; uint8_t cw;
        if (!g2PbReadTag(codes, (size_t)cl, &cp, &cf, &cw)) break;
        if (cw == G2_PB_WIRE_VARINT) {
          uint64_t cv;
          if (!g2PbReadVarint(codes, (size_t)cl, &cp, &cv)) break;
          if      (cf == 1) codeA = (uint32_t)cv;
          else if (cf == 2) codeB = (uint32_t)cv;
        } else {
          if (!g2PbSkipField(codes, (size_t)cl, &cp, cw)) break;
        }
      }
      haveCodes = true;
      sp += (size_t)cl;
    } else {
      if (!g2PbSkipField(sub, subLen, &sp, sw)) break;
    }
  }

  if (haveCodes) {
    DEBUG_G2F("[G2-%c] sid=0x01 GESTURE cmd=%u counter=%u flag=%u codes={%u,%u}",
              side, (unsigned)cmd, (unsigned)counter,
              (unsigned)innerFlag, (unsigned)codeA, (unsigned)codeB);
    // All ring-flavoured input events observed against firmware
    // 2.2.0.242 arrived on the RIGHT temple — the right arm is the
    // ring's BLE relay (the Even app pairs ring↔glasses, then the
    // glasses replay ring events onto whichever temple has the
    // notify subscription). If we ever see one on LEFT, that's
    // either a fallback channel or a firmware change worth noticing
    // immediately. Loud banner so it's impossible to miss in the
    // serial scrollback. Doesn't block dispatch — caller's normal
    // handling continues.
    if (side == 'L') {
      DEBUG_G2F("[G2] !!! GESTURE on LEFT temple (cmd=%u counter=%u "
                "codes={%u,%u}) — first observed; possible ring "
                "fallback channel or firmware behaviour change",
                (unsigned)cmd, (unsigned)counter,
                (unsigned)codeA, (unsigned)codeB);
      g2RingDump("gesture-on-LEFT");
    }
  } else {
    DEBUG_G2F("[G2-%c] sid=0x01 async cmd=%u counter=%u flag=%u (no codes)",
              side, (unsigned)cmd, (unsigned)counter, (unsigned)innerFlag);
  }
}

static void handleDevEvent(G2Temple& t, const uint8_t* devBody, size_t devLen) {
  // Inter-arrival timing for DevEvent frames. These arrive at whatever
  // cadence the firmware decides (burst for clustered taps, idle between),
  // so the Δ is a good signal for "is this a probe?" vs "is this a user
  // gesture?". Per-temple so both arms' cadences show independently.
  const uint32_t nowMs = millis();
  static uint32_t gLastDevEventMsL = 0;
  static uint32_t gLastDevEventMsR = 0;
  uint32_t& lastRef = (t.side == 'L') ? gLastDevEventMsL : gLastDevEventMsR;
  const uint32_t dMs = (lastRef == 0) ? 0 : (nowMs - lastRef);
  lastRef = nowMs;
  DEBUG_G2F("[G2-%c] DevEvent (%u B) Δ=%ums", t.side, (unsigned)devLen, (unsigned)dMs);

  // devBody is the inner SendDeviceEvent bytes (already stripped of the
  // wrapper's field-13 tag + length by parseEvenCoreResponse's body walk).
  size_t pos = 0;
  while (pos < devLen) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(devBody, devLen, &pos, &field, &wire)) break;
    if (wire != G2_PB_WIRE_LEN_DELIM) {
      if (!g2PbSkipField(devBody, devLen, &pos, wire)) break;
      continue;
    }
    uint64_t sublen;
    if (!g2PbReadVarint(devBody, devLen, &pos, &sublen)) break;
    if (pos + sublen > devLen) break;
    const uint8_t* sub = devBody + pos;
    const size_t   subN = (size_t)sublen;
    pos += subN;

    if (field == 1) {
      // List_ItemEvent
      uint32_t cid = 0, idx = 0, etype = 0;
      char cname[32] = {0}, iname[32] = {0};
      size_t sp = 0;
      while (sp < subN) {
        uint32_t sf; uint8_t sw;
        if (!g2PbReadTag(sub, subN, &sp, &sf, &sw)) break;
        if (sw == G2_PB_WIRE_VARINT) {
          uint64_t v;
          if (!g2PbReadVarint(sub, subN, &sp, &v)) break;
          if      (sf == 1) cid   = (uint32_t)v;
          else if (sf == 4) idx   = (uint32_t)v;
          else if (sf == 5) etype = (uint32_t)v;
        } else if (sw == G2_PB_WIRE_LEN_DELIM) {
          uint64_t sl;
          if (!g2PbReadVarint(sub, subN, &sp, &sl)) break;
          if (sp + sl > subN) break;
          if (sf == 2 || sf == 3) {
            char* dst = (sf == 2) ? cname : iname;
            size_t copy = (sl < 31) ? (size_t)sl : 31;
            memcpy(dst, sub + sp, copy); dst[copy] = '\0';
          }
          sp += (size_t)sl;
        } else {
          if (!g2PbSkipField(sub, subN, &sp, sw)) break;
        }
      }
      DEBUG_G2F("[G2-%c] ListEvent: container='%s' cid=%u idx=%u item='%s' type=%s(%u)",
                t.side, cname, (unsigned)cid, (unsigned)idx, iname,
                osEventTypeName(etype), (unsigned)etype);
      // Image-probe hold dismissal — single-tap path. When a probe surfaces
      // a List widget (e.g. mixed list+image probes Q16/Q17/Q18), single
      // taps on rows arrive here as ListEvent CLICK(0) instead of the
      // SysEvent DOUBLE_CLICK channel watched above. Treat any list tap
      // during probe-hold as the dismiss gesture so the user doesn't have
      // to double-tap to escape.
      if (gImgProbeHoldActive && etype == 0) {
        if (gHijackActive) gHijackStartedMs = millis();
        DEBUG_G2F("[G2] IMG probe hold: ListEvent CLICK on '%s' idx=%u → dismiss",
                  cname, (unsigned)idx);
        gImgProbeHoldTapPending = true;
        return;
      }
      // If user tapped an item on our hijacked "app" container, route it
      // to the hijack action handler. Dedup across arms first — both L
      // and R fire this within ~20 ms of each other for every tap.
      if (etype == 0 && strcmp(cname, "app") == 0) {
        if (shouldDedupHijackTap(idx)) {
          DEBUG_G2F("[G2-%c] Hijack tap dedup-dropped (idx=%u)",
                    t.side, (unsigned)idx);
        } else {
          BROADCAST_PRINTF("[G2] Hijack tap: item %u (%s)",
                           (unsigned)idx, iname);
          // Refresh the safety-timeout window on every real user tap.
          // The 60 s cap exists to recover from stuck hijacks where
          // we've lost track of state; an actively-tapping user is, by
          // definition, not stuck. Idle hijacks still time out
          // normally because gHijackStartedMs only advances on input.
          if (gHijackActive) gHijackStartedMs = millis();
          handleHijackMenuTap(idx);
        }
      }
    } else if (field == 2) {
      // Text_ItemEvent
      uint32_t cid = 0, etype = 0;
      char cname[32] = {0};
      size_t sp = 0;
      while (sp < subN) {
        uint32_t sf; uint8_t sw;
        if (!g2PbReadTag(sub, subN, &sp, &sf, &sw)) break;
        if (sw == G2_PB_WIRE_VARINT) {
          uint64_t v;
          if (!g2PbReadVarint(sub, subN, &sp, &v)) break;
          if (sf == 1) cid   = (uint32_t)v;
          if (sf == 3) etype = (uint32_t)v;
        } else if (sw == G2_PB_WIRE_LEN_DELIM && sf == 2) {
          uint64_t sl;
          if (!g2PbReadVarint(sub, subN, &sp, &sl)) break;
          size_t copy = (sl < 31) ? (size_t)sl : 31;
          if (sp + sl > subN) break;
          memcpy(cname, sub + sp, copy); cname[copy] = '\0';
          sp += (size_t)sl;
        } else {
          if (!g2PbSkipField(sub, subN, &sp, sw)) break;
        }
      }
      DEBUG_G2F("[G2-%c] TextEvent: container='%s' cid=%u type=%s(%u)",
                t.side, cname, (unsigned)cid,
                osEventTypeName(etype), (unsigned)etype);
      // TextEvent CLICK is the firmware's "the user tapped the text
      // widget" event — but only fires if IsEventCapture=1 was set on
      // CREATE. Reference docs say this never fires; we set the flag
      // anyway in g2BuildCreateTextPagePb in case 2.2.0.242 honors it.
      // If we DO get one here while gTextViewActive, that's the clean
      // path: invoke the exit handler immediately, no need to wait for
      // the sid=0x0D USER_ACTIVITY fallback.
      if (gTextViewActive) {
        // Any TextEvent counts as user input — refresh the hijack
        // safety-timeout so a long read of a paginated JSON view
        // doesn't get torn down at the 60 s mark.
        if (gHijackActive) gHijackStartedMs = millis();
        // Paginated views: map gestures directionally —
        //   etype==0 CLICK         → next page (lens tap, no direction)
        //   etype==1 SCROLL_TOP    → previous page (ring scroll up)
        //   etype==2 SCROLL_BOTTOM → next page (ring scroll down)
        // Single-page views (tapFn=null) preserve legacy "any tap exits".
        // No DOUBLE_CLICK on this channel — that lives in SysEvent.
        if (gTextViewTapFn) {
          const G2TapKind kind = (etype == 1) ? G2_TAP_PAGE_PREV
                                              : G2_TAP_PAGE_NEXT;
          DEBUG_G2F("[G2] TEXT view: TextEvent %s(%u) → page-%s handler",
                    osEventTypeName(etype), (unsigned)etype,
                    kind == G2_TAP_PAGE_PREV ? "prev" : "next");
          gTextViewActivatedMs = millis();
          gTextViewTapFn(kind);
        } else if (gTextViewExitFn) {
          DEBUG_G2F("[G2] TEXT view: TextEvent CLICK → invoking exit handler");
          void (*fn)() = gTextViewExitFn;
          gTextViewActive = false;
          gTextViewExitFn = nullptr;
          gTextViewTapFn  = nullptr;
          fn();
        }
      }
    } else if (field == 3) {
      // Sys_ItemEvent — most importantly EventType=7 SYSTEM_EXIT means the
      // firmware is tearing our widget down. This is the wire event that
      // precedes the on-lens "Connection lost" message.
      uint32_t etype = 0, src = 0, reason = 0;
      size_t sp = 0;
      while (sp < subN) {
        uint32_t sf; uint8_t sw;
        if (!g2PbReadTag(sub, subN, &sp, &sf, &sw)) break;
        if (sw == G2_PB_WIRE_VARINT) {
          uint64_t v;
          if (!g2PbReadVarint(sub, subN, &sp, &v)) break;
          if      (sf == 1) etype  = (uint32_t)v;
          else if (sf == 2) src    = (uint32_t)v;
          else if (sf == 4) reason = (uint32_t)v;
        } else {
          if (!g2PbSkipField(sub, subN, &sp, sw)) break;
        }
      }
      DEBUG_G2F("[G2-%c] SysEvent: type=%s(%u) src=%u reason=%u",
                t.side, osEventTypeName(etype), (unsigned)etype,
                (unsigned)src, (unsigned)reason);

      // Ring-via-glasses presence. EventSourceType=2 = ring (verified
      // 2026-04-25: ring double-tap fires SysEvent.DOUBLE_CLICK with
      // src=2 only when the ring is BLE-paired with the glasses and
      // the linkToGlasses handshake is in place — i.e. this is the
      // canonical "the ring is wired up to the glasses right now"
      // signal. Stash the timestamp so the Status page can surface
      // "ring linked: yes (last seen Ns ago)". Distinct from the
      // direct-BLE-to-ring connection state (gRing.connected).
      extern uint32_t gRingViaGlassesLastSeenMs;
      if (src == 2) gRingViaGlassesLastSeenMs = millis();

      // TEXT-view exit. When we have a TEXT widget on the lens
      // (gTextViewActive) and the user performs a tap/double-tap/swipe
      // that generates a SysEvent on sid=0xE0, treat it as "exit text
      // view" and invoke the registered back-handler. Verified
      // 2026-04-25 against firmware 2.2.0.242: ring double-tap on a
      // TEXT widget fires SysEvent.DOUBLE_CLICK(3) src=2, NOT
      // TextEvent CLICK and NOT sid=0x0D USER_ACTIVITY (the firmware
      // ignores both our IsEventCapture=1 flag and its own sid=0x0D
      // path while a TEXT widget is foreground). So this is the right
      // hook.
      //
      // We accept any of CLICK(0)/DOUBLE_CLICK(3)/SCROLL_TOP(1)/
      // SCROLL_BOTTOM(2) — anything that's user-initiated input
      // rather than a firmware-internal transition (FG_ENTER/FG_EXIT/
      // SYSTEM_EXIT/etc.). Trying to whitelist a single event type
      // would be brittle: future firmware revisions might emit a
      // different one for the same gesture, and the user's intent is
      // unambiguous either way (they touched something).
      // Image-probe hold dismissal — same gesture set as text view
      // (CLICK/SCROLL/DOUBLE_CLICK), just sets a flag the worker polls.
      // No exitFn callback because the worker is on a different task and
      // owns its own teardown sequence.
      if (gImgProbeHoldActive &&
          (etype == 0 || etype == 1 || etype == 2 || etype == 3)) {
        if (gHijackActive) gHijackStartedMs = millis();
        DEBUG_G2F("[G2] IMG probe hold: SysEvent %s(%u) src=%u → dismiss",
                  osEventTypeName(etype), (unsigned)etype, (unsigned)src);
        gImgProbeHoldTapPending = true;
        return;
      }

      // Live-list page double-tap manual refresh. On firmware 2.2.0.24
      // (verified 2026-04-27), List widgets emit DOUBLE_CLICK(3) src=2
      // as a SysEvent on sid=0xE0 — distinct from the single-tap
      // ListEvent CLICK(0) channel. Use it as a "refresh now" gesture
      // for live status pages: kick the refresh sem so the worker
      // rebuilds immediately instead of waiting out the interval.
      // Single tap still goes through the ListEvent path → row tap
      // dispatch → page-swap, which calls g2StopLiveListPage to cancel.
      if (livePageIsActive() && etype == 3 && src == 2) {
        if (gHijackActive) gHijackStartedMs = millis();
        DEBUG_G2F("[G2] live-page: SysEvent DOUBLE_CLICK src=2 → "
                  "kicking refresh");
        livePageKickRefresh();
        return;
      }

      if (gTextViewActive &&
          (etype == 0 || etype == 1 || etype == 2 || etype == 3)) {
        // Any input gesture on a TEXT view refreshes the hijack
        // safety-timeout — same rationale as the TextEvent path above.
        if (gHijackActive) gHijackStartedMs = millis();
        // Paginated views (tapFn != null) map gestures directionally:
        //   CLICK(0)         → next page (lens tap, no direction)
        //   SCROLL_TOP(1)    → previous page (ring scroll up)
        //   SCROLL_BOTTOM(2) → next page (ring scroll down)
        //   DOUBLE_CLICK(3)  → exit
        // Single-page views (tapFn=null) preserve legacy "any tap exits".
        const bool isExitGesture = (etype == 3);  // DOUBLE_CLICK
        if (gTextViewTapFn && !isExitGesture) {
          const G2TapKind kind = (etype == 1) ? G2_TAP_PAGE_PREV
                                              : G2_TAP_PAGE_NEXT;
          DEBUG_G2F("[G2] TEXT view: SysEvent %s(%u) src=%u → page-%s",
                    osEventTypeName(etype), (unsigned)etype,
                    (unsigned)src,
                    kind == G2_TAP_PAGE_PREV ? "prev" : "next");
          gTextViewActivatedMs = millis();
          gTextViewTapFn(kind);
          return;
        }
        if (gTextViewExitFn) {
          DEBUG_G2F("[G2] TEXT view: SysEvent %s(%u) src=%u → invoking exit "
                    "handler", osEventTypeName(etype), (unsigned)etype,
                    (unsigned)src);
          void (*fn)() = gTextViewExitFn;
          gTextViewActive = false;
          gTextViewExitFn = nullptr;
          gTextViewTapFn  = nullptr;
          fn();
          return;
        }
      }

      if (etype == 7) {  // SYSTEM_EXIT_EVENT
        // If we initiated a Shutdown ourselves within the last ~2 s
        // (page swap in flight), the firmware is just echoing our own
        // teardown. The hijack itself isn't ending — we're about to
        // CREATE a fresh widget with new content. Don't clear state.
        const uint32_t ours = gOurShutdownAtMs;
        if (ours != 0 && (millis() - ours) < 2000) {
          DEBUG_G2F("[G2-%c] SYSTEM_EXIT during our own page-swap "
                    "(reason=%u) — ignoring, hijack stays active",
                    t.side, (unsigned)reason);
          // Don't fall through. The page-swap worker is the source of
          // truth for state across this transition.
        } else
        // Firmware has torn down our container. Don't keep thinking we
        // have a hijack live — clear state so the next MenuStartUp tap
        // starts clean. Log the widget's lifetime so timeout patterns
        // become obvious (e.g. "always dies at ~15s regardless of input"
        // vs "dies immediately after specific action").
        if (gHijackActive) {
          const uint32_t lifeMs = (gHijackStartedMs > 0)
                                  ? (millis() - gHijackStartedMs) : 0;
          BROADCAST_PRINTF("[G2] Firmware tore down widget "
                           "(SYSTEM_EXIT reason=%u) after %u.%03us alive "
                           "— on-lens shows 'Connection lost'. "
                           "Clearing hijack state.",
                           (unsigned)reason,
                           (unsigned)(lifeMs / 1000),
                           (unsigned)(lifeMs % 1000));
          gHijackActive = false;
          gHijackStartedMs = 0;
          gR.containerReady = false;
          g2LensSetHijackActive(false);
          g2LensClearContainer();
          g2LensClearOverlay();
          g2PushStatusEvent("fw-system-exit");
          g2RingDump("firmware SYSTEM_EXIT");
        } else {
          DEBUG_G2F("[G2-%c] SYSTEM_EXIT while no hijack was active "
                    "(reason=%u)", t.side, (unsigned)reason);
        }
      }
    }
  }
}

[[maybe_unused]] static const char* eventName(G2EventType e) {
  switch (e) {
    case G2_EVENT_USER_ACTIVITY: return "USER_ACTIVITY";
    case G2_EVENT_DISPLAY_OFF:   return "DISPLAY_OFF";
    case G2_EVENT_TAP:           return "TAP";
    case G2_EVENT_DOUBLE_TAP:    return "DOUBLE_TAP";
    case G2_EVENT_SWIPE_UP:      return "SWIPE_UP";
    case G2_EVENT_SWIPE_DOWN:    return "SWIPE_DOWN";
    case G2_EVENT_SWIPE_LEFT:    return "SWIPE_LEFT";
    case G2_EVENT_SWIPE_RIGHT:   return "SWIPE_RIGHT";
    case G2_EVENT_LONG_PRESS:    return "LONG_PRESS";
    default:                     return "UNKNOWN";
  }
}

// Fwd-decl — dispatchEventPayload calls into the hijack-exit path on the
// first-seen copy of each DISPLAY_OFF event.
static void onDisplayOffWhileHijacked(char side);

static void dispatchEventPayload(char side, const uint8_t* payload, size_t len) {
  if (!payload || len == 0) return;
  if (shouldDedupEvent(payload, len)) {
    DEBUG_G2F("[G2-%c] Event dedup-dropped (%u B)", side, (unsigned)len);
    return;
  }
  // Inter-arrival timing — the gap between successive non-deduped events
  // on sid=0x0D is the clearest signal for "this is a periodic probe"
  // vs "this is a user gesture." Update AFTER the dedup so we're not
  // reporting the 20 ms L↔R mirror gap.
  static uint32_t gLastStateEventMs = 0;
  const uint32_t nowMs = millis();
  const uint32_t dMs = (gLastStateEventMs == 0) ? 0 : (nowMs - gLastStateEventMs);
  gLastStateEventMs = nowMs;

  char hint[48] = {0};
  const G2EventType ev = classifyStateEvent(payload, len, hint, sizeof(hint));
  DEBUG_G2F("[G2-%c] Event → %s (%u B) hint='%s' Δ=%ums",
            side, eventName(ev), (unsigned)len, hint, (unsigned)dMs);
  if (ev != G2_EVENT_UNKNOWN && gEventCallback) {
    gEventCallback(ev);
  }

  // Fallback exit for TEXT view. Reference says firmware doesn't fire
  // TextEvent on tap of a TextContainer regardless of IsEventCapture
  // (we set it to 1 anyway, as a hopeful guess). If TextEvent never
  // arrives, the user has no way to back out of the JSON view via tap
  // — they'd have to tap-and-hold to invoke the firmware's "Exit?"
  // gesture, which leaves the hijack entirely.
  //
  // Workaround: when gTextViewActive is true, the next USER_ACTIVITY
  // event (any tap, swipe, or head-up wake — they all collapse to the
  // same bytes per docs/G2_PROTOCOL.md) triggers the registered
  // exitFn. The page-swap worker only sets gTextViewActive after the
  // CREATE-text ack arrives, which is 600-800 ms after the entry tap;
  // by that time the entry tap's USER_ACTIVITY echo has already been
  // dispatched and ignored. So this wires "any subsequent user input
  // exits the text view," which is the intuitive UX the user
  // requested.
  if (ev == G2_EVENT_USER_ACTIVITY && gTextViewActive) {
    // Grace window: the firmware fires a spontaneous USER_ACTIVITY
    // ~150-300 ms after the page renders, before the human has a chance
    // to look at it. Without this gate the JSON view dismissed itself
    // immediately on render. After the grace, real user input is
    // treated as either page-advance (gTextViewTapFn set, paginated
    // view) or exit (legacy single-page UX).
    const uint32_t now = millis();
    if (now - gTextViewActivatedMs < kTextViewExitGraceMs) {
      DEBUG_G2F("[G2] TEXT view: ignoring user-activity inside %u ms grace "
                "(elapsed=%u ms — firmware settle event, not a tap)",
                (unsigned)kTextViewExitGraceMs,
                (unsigned)(now - gTextViewActivatedMs));
    } else if (gTextViewTapFn) {
      // Paginated TEXT view: USER_ACTIVITY (lens tap) is non-directional;
      // map to "next page" by convention. SCROLL_TOP/BOTTOM via SysEvent
      // give the user backward navigation.
      DEBUG_G2F("[G2] TEXT view: user-activity → page-next handler");
      gTextViewActivatedMs = now;
      gTextViewTapFn(G2_TAP_PAGE_NEXT);
    } else if (gTextViewExitFn) {
      DEBUG_G2F("[G2] TEXT view: user-activity → invoking exit handler");
      void (*fn)() = gTextViewExitFn;
      gTextViewActive = false;
      gTextViewExitFn = nullptr;
      gTextViewTapFn  = nullptr;
      fn();
    }
  }
  // DISPLAY_OFF invalidates every cached container on the firmware side —
  // not just the hijack container, ANY StartUpPage we've CREATEd. The
  // firmware reclaims the RAM the moment the display blanks. If we leave
  // containerReady set, the next g2ShowText() sends REBUILD_PAGE against a
  // container that no longer exists → firmware silently drops it, user
  // sees nothing. (This bit every caller, not just the Blocks hijack; was
  // the root cause of "second g2show doesn't show" when the display
  // auto-timed-out between calls.)
  //
  // Caveat: the firmware also emits 4-byte DISPLAY_OFF mid-sequence when
  // swapping content (menu → app, app → menu). Treating that as an
  // invalidation is still correct — the firmware really does tear down
  // the old container in that transition. Worst case we pay the cost of
  // a fresh CREATE on the next show, which is fine.
  if (ev == G2_EVENT_DISPLAY_OFF) {
    // Page-swap echo guard: DISPLAY_OFF is a normal side-effect of our
    // own intentional ShutdownPage during a page swap. Treating it as a
    // hijack-ending event would clear gHijackActive AND send another
    // Shutdown that contends with the worker's pending CREATE on the
    // write mutex (observed: CREATE-list send failed → "Write mutex
    // timeout"). Within ~2 s of our own shutdown, ignore the echo.
    const uint32_t ours = gOurShutdownAtMs;
    if (ours != 0 && (millis() - ours) < 2000) {
      DEBUG_G2F("[G2] DISPLAY_OFF during our own page-swap — ignoring "
                "(hijack stays active, worker handles state)");
      return;
    }
    if (gL.containerReady || gR.containerReady) {
      DEBUG_G2F("[G2] DISPLAY_OFF — invalidating cached containerReady");
    }
    gL.containerReady = false;
    gR.containerReady = false;
    g2LensClearContainer();
    g2LensClearOverlay();
    // Blocks-hijack exit path — fire ShutdownPage from whichever arm
    // wins the dedup race so the firmware frees its plugin-task state
    // cleanly. Guarded internally by gHijackActive so normal flows
    // don't send spurious Shutdowns.
    if (gHijackActive) {
      onDisplayOffWhileHijacked(side);
    }
  }
}

// Decode the ResCmdMsg byte from an EvenCore ack payload. Valid only for
// page-op response Cmds (1 / 6 / 8 / 10) — heartbeat and audio acks carry
// counters/state in their nested bodies, not a ResCmdMsg enum. Values
// mirror the EvenHub_ErrorCode_List enum from the reference's generated
// pb schema (`ble/gen/EvenHub_pb.ts`).
// Per EvenHub_ErrorCode_List in EvenHub_pb.ts. Codes are NOT a single
// success/failure namespace — they pair (success, failure) per response
// type. So `res=10 on a ShutdownResp` means SHUTDOWN_SUCCESS, not "err".
// Earlier mappings here were guesses; replaced with the verbatim enum.
static const char* evenCoreResCode(uint32_t code) {
  switch (code) {
    case 0:  return "CreateSuccess";
    case 1:  return "CreateInvalidContainer";
    case 2:  return "CreateOversize";
    case 3:  return "CreateOOM";
    case 4:  return "ImageRawSuccess";
    case 5:  return "ImageRawFailed";
    case 6:  return "RebuildSuccess";
    case 7:  return "RebuildFailed";
    case 8:  return "TextSuccess";
    case 9:  return "TextFailed";
    case 10: return "ShutdownSuccess";
    case 11: return "ShutdownFailed";
    case 12: return "HeartbeatSuccess";
    case 13: return "AudioSuccess";
    case 14: return "AudioFailed";
    default: return "err";
  }
}

// True iff `code` is a failure response. Used to gate the ring-buffer
// dump so successful non-zero codes (RebuildSuccess=6, ShutdownSuccess=10,
// HeartbeatSuccess=12, etc.) don't fire spurious diagnostic dumps.
static bool evenCoreResIsFailure(uint32_t code) {
  switch (code) {
    case 1: case 2: case 3:        // Create failures
    case 5:                        // ImageRaw failure
    case 7:                        // Rebuild failure
    case 9:                        // Text failure
    case 11:                       // Shutdown failure
    case 14:                       // Audio failure
      return true;
    default:
      return false;
  }
}

// Outer-wrapper field carrying the response body for each response Cmd.
// `0` means "this Cmd has no ResCmdMsg-shaped body we should interpret" —
// used for heartbeats (Cmd 12) and other state echoes.
// Mapping derived from `ble/gen/EvenHub_pb.ts` in g2-kit-unofficial.
static uint32_t evenCoreResBodyField(uint32_t cmd) {
  switch (cmd) {
    case 1:  return 4;   // CreateResp     → ResponseCreateStartupCmd
    case 6:  return 10;  // TextResp       → ResponseTextUpgradeCmd
    case 8:  return 8;   // RebuildResp    → ResponseRebuildCmd
    case 10: return 12;  // ShutdownResp   → ResponseShutDownCmd
    default: return 0;
  }
}

static const char* evenCoreCmdName(uint32_t cmd) {
  switch (cmd) {
    case 0:  return "Create";
    case 1:  return "CreateResp";
    case 2:  return "DevEvent";      // OS_NOITY_EVENT_TO_APP_PACKET (firmware typo)
    case 3:  return "ImageRaw";
    case 4:  return "ImageRawResp";
    case 5:  return "TextUpgrade";
    case 6:  return "TextResp";
    case 7:  return "Rebuild";
    case 8:  return "RebuildResp";
    case 9:  return "Shutdown";
    case 10: return "ShutdownResp";
    case 12: return "HeartbeatAck";
    case 13: return "AudioCtrlAck";
    case 15: return "AudioCtrl";
    case 17: return "MenuStartup";   // OS_NOTIFY_MENU_STARTUP_PACKET
    case 18: return "MenuFailed";    // APP_RESPONSE_MENU_STARTUP_FAILED_PACKET
    default: return "?";
  }
}

// OsEventTypeList values (EvenHub_pb.ts:1181-1226). Used as the EventType
// field inside ListEvent / TextEvent / SysEvent sub-messages of a Cmd=2
// DevEvent frame.
static const char* osEventTypeName(uint32_t ev) {
  switch (ev) {
    case 0:  return "CLICK";
    case 1:  return "SCROLL_TOP";
    case 2:  return "SCROLL_BOTTOM";
    case 3:  return "DOUBLE_CLICK";
    case 4:  return "FG_ENTER";
    case 5:  return "FG_EXIT";
    case 6:  return "ABNORMAL_EXIT";
    case 7:  return "SYSTEM_EXIT";
    case 8:  return "IMU_DATA_REPORT";
    default: return "?";
  }
}

// Classification of a parsed EvenCore response.
enum class G2ResKind : uint8_t {
  None,       // couldn't decode wrapper at all
  Response,   // known response Cmd with ResCmdMsg (res valid)
  AckOnly,    // known Cmd but not a ResCmdMsg-shaped ack (heartbeat etc.)
};

// Walk an EvenCore wrapper pb. Always captures Cmd (field 1) and
// MagicRandom (field 2). For the subset of response Cmds that carry a
// ResponseFooCmd body at a known field, pulls the first inner varint as
// `res` — and if that body is present-but-empty, reports res=0
// (CreatePageSuccess), which is the protobuf default.
//
// Observed traffic:
//   CREATE ack (success, empty body):
//     08 01 10 C9 01 22 00
//     {Cmd=1, Magic=201, field 4 len=0}
//   REBUILD ack (failure):
//     08 08 10 CA 01 42 02 08 07
//     {Cmd=8, Magic=202, field 8 len=2 {field 1 varint=7}}
//   HEARTBEAT ack (counter body, NOT a ResCmdMsg):
//     08 0C 10 CD 01 7A 04 08 07 10 0C
//     {Cmd=12, Magic=205, field 15 len=4 {field 1 = counter, field 2 = 12}}
//
// The earlier implementation blindly descended into any len-delim inner
// message, which misread the heartbeat counter as a RebuildFailed/Text*
// code and then cycled through the enum on every heartbeat tick.
static G2ResKind parseEvenCoreResponse(const uint8_t* payload, size_t len,
                                       uint32_t* outCmd,
                                       uint32_t* outMagic,
                                       uint32_t* outRes) {
  if (outCmd)   *outCmd   = 0;
  if (outMagic) *outMagic = 0;
  if (outRes)   *outRes   = 0;
  if (!payload) return G2ResKind::None;

  // Pass 1 — identify Cmd + Magic.
  uint32_t cmd = 0, magic = 0;
  size_t pos = 0;
  while (pos < len) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(payload, len, &pos, &field, &wire)) return G2ResKind::None;
    if (wire == G2_PB_WIRE_VARINT) {
      uint64_t v;
      if (!g2PbReadVarint(payload, len, &pos, &v)) return G2ResKind::None;
      if      (field == 1) cmd   = (uint32_t)v;
      else if (field == 2) magic = (uint32_t)v;
    } else {
      if (!g2PbSkipField(payload, len, &pos, wire)) return G2ResKind::None;
    }
  }
  if (outCmd)   *outCmd   = cmd;
  if (outMagic) *outMagic = magic;

  // Pass 2 — only page-op response Cmds carry a ResCmdMsg body we should
  // decode; heartbeats (Cmd=12) do not.
  const uint32_t bodyField = evenCoreResBodyField(cmd);
  if (bodyField == 0) return G2ResKind::AckOnly;

  uint32_t res = 0;
  pos = 0;
  while (pos < len) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(payload, len, &pos, &field, &wire)) break;
    if (field == bodyField && wire == G2_PB_WIRE_LEN_DELIM) {
      uint64_t sublen;
      if (!g2PbReadVarint(payload, len, &pos, &sublen)) break;
      if (pos + sublen > len) break;
      const uint8_t* sub = payload + pos;
      size_t subpos = 0;
      while (subpos < (size_t)sublen) {
        uint32_t sf; uint8_t sw;
        if (!g2PbReadTag(sub, (size_t)sublen, &subpos, &sf, &sw)) break;
        if (sf == 1 && sw == G2_PB_WIRE_VARINT) {
          uint64_t sv;
          if (!g2PbReadVarint(sub, (size_t)sublen, &subpos, &sv)) break;
          res = (uint32_t)sv;
          break;
        }
        if (!g2PbSkipField(sub, (size_t)sublen, &subpos, sw)) break;
      }
      // Body present, possibly empty → leave res at 0 (pb default == Success).
      if (outRes) *outRes = res;
      return G2ResKind::Response;
    }
    if (!g2PbSkipField(payload, len, &pos, wire)) break;
  }
  // Body field not present — treat as default (Success).
  if (outRes) *outRes = 0;
  return G2ResKind::Response;
}

// =============================================================================
// cmd=17 MenuStartUpEvent — the Blocks-tap we hijack
// =============================================================================

// Decode an EvenCore wrapper of the form observed in the live capture:
//
//     08 11 A2 01 03 08 8D 52
//     {Cmd=17, MenuStartEv(field 20) = {widgetId(field 1)=10509}}
//
// Tag 0xA2 0x01 is field 20 wire=2 (`(20<<3)|2 = 162 = 0xA2`; 0x01 is the
// varint continuation byte). The inner message has a single varint at
// field 1 which is the widgetId. We don't currently care about any other
// inner fields (menu index, extras), so the parser stops there.
//
// Returns true and writes *outWidgetId on success; false if the wrapper
// isn't a Cmd=17 MenuStartUpEvent we recognise. This is called ahead of
// parseEvenCoreResponse in handleEnvelope so cmd=17 frames stop being
// mislabelled as generic AckOnly.
static bool parseMenuStartUpEvent(const uint8_t* payload, size_t len,
                                  uint32_t* outWidgetId) {
  if (outWidgetId) *outWidgetId = 0;
  if (!payload) return false;

  uint32_t cmd = 0;
  bool     haveMenu = false;
  uint32_t widgetId = 0;

  size_t pos = 0;
  while (pos < len) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(payload, len, &pos, &field, &wire)) return false;
    if (field == 1 && wire == G2_PB_WIRE_VARINT) {
      uint64_t v;
      if (!g2PbReadVarint(payload, len, &pos, &v)) return false;
      cmd = (uint32_t)v;
      continue;
    }
    if (field == 20 && wire == G2_PB_WIRE_LEN_DELIM) {
      uint64_t sublen;
      if (!g2PbReadVarint(payload, len, &pos, &sublen)) return false;
      if (pos + sublen > len) return false;
      const uint8_t* sub = payload + pos;
      size_t subpos = 0;
      while (subpos < (size_t)sublen) {
        uint32_t sf; uint8_t sw;
        if (!g2PbReadTag(sub, (size_t)sublen, &subpos, &sf, &sw)) break;
        if (sf == 1 && sw == G2_PB_WIRE_VARINT) {
          uint64_t sv;
          if (!g2PbReadVarint(sub, (size_t)sublen, &subpos, &sv)) break;
          widgetId = (uint32_t)sv;
          haveMenu = true;
          break;
        }
        if (!g2PbSkipField(sub, (size_t)sublen, &subpos, sw)) break;
      }
      pos += (size_t)sublen;
      continue;
    }
    if (!g2PbSkipField(payload, len, &pos, wire)) return false;
  }

  if (cmd != G2_CMD_MENU_STARTUP || !haveMenu) return false;
  if (outWidgetId) *outWidgetId = widgetId;
  return true;
}

// Same pattern as parseMenuStartUpEvent but for Cmd=2 DevEvent frames
// (OS_NOITY_EVENT_TO_APP_PACKET). Walks the wrapper, pulls the field=13
// SendDeviceEvent body, and on success hands it to handleDevEvent which
// decodes the ListEvent/TextEvent/SysEvent sub-message and acts on it.
// Returns true if the frame was a Cmd=2 and we dispatched.
static bool parseAndDispatchDevEvent(G2Temple& t,
                                     const uint8_t* payload, size_t len) {
  if (!payload) return false;
  uint32_t cmd = 0;
  const uint8_t* devBody = nullptr;
  size_t devLen = 0;

  size_t pos = 0;
  while (pos < len) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(payload, len, &pos, &field, &wire)) return false;
    if (field == 1 && wire == G2_PB_WIRE_VARINT) {
      uint64_t v;
      if (!g2PbReadVarint(payload, len, &pos, &v)) return false;
      cmd = (uint32_t)v;
      continue;
    }
    if (field == 13 && wire == G2_PB_WIRE_LEN_DELIM) {
      uint64_t sublen;
      if (!g2PbReadVarint(payload, len, &pos, &sublen)) return false;
      if (pos + sublen > len) return false;
      devBody = payload + pos;
      devLen  = (size_t)sublen;
      pos += (size_t)sublen;
      continue;
    }
    if (!g2PbSkipField(payload, len, &pos, wire)) return false;
  }

  if (cmd != 2 || !devBody) return false;
  handleDevEvent(t, devBody, devLen);
  return true;
}

// =============================================================================
// Dashboard status snapshot — rendered on the lens when hijack fires
// =============================================================================

// Build a compact multi-line snapshot of the ESP32's current status, sized
// to fit on the G2 lens. Mirrors the key fields of the web-UI dashboard
// (`buildSystemInfoJson`) but collapsed into ~6 short lines. Sections are
// compile-guarded against the System_BuildConfig flags so a minimal build
// still produces a coherent page.
//
// Example output:
//   HardwareOne
//   Up 2h15m · Heap 45K · 42C
//   WiFi MyNet · 192.168.1.42
//   MAC ..DDEEFF
//   ESPNow mesh · 5p · 1234tx/5678rx
//   G2 batt 79%
static void __attribute__((unused))
buildG2StatusSnapshot(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';

  // Use a String builder so section order is trivially clear and we never
  // have to hand-thread a cursor through snprintf offsets. Final copy into
  // the caller's buffer truncates cleanly on overflow.
  String s;
  s.reserve(256);

  const char* name = gSettings.espnowDeviceName.length() > 0
                     ? gSettings.espnowDeviceName.c_str()
                     : "HardwareOne";
  s += name;
  s += '\n';

  unsigned long seconds = millis() / 1000UL;
  unsigned long hours   = seconds / 3600UL;
  unsigned long minutes = (seconds / 60UL) % 60UL;
  unsigned heapKb = (unsigned)(ESP.getFreeHeap() / 1024);
  float tempC = temperatureRead();
  {
    char line[96];
    // Use " - " as the section separator; the G2 font doesn't carry the
    // · middot and would drop it, so plain ASCII is safer.
    snprintf(line, sizeof(line),
             "Up %luh%lum - Heap %uK - %.0fC\n",
             hours, minutes, heapKb, (double)tempC);
    s += line;
  }

#if ENABLE_WIFI
  if (WiFi.isConnected()) {
    char line[96];
    String ssid = WiFi.SSID();
    String ip   = WiFi.localIP().toString();
    snprintf(line, sizeof(line), "WiFi %s - %s\n", ssid.c_str(), ip.c_str());
    s += line;
  } else {
    s += "WiFi offline\n";
  }
  {
    char line[32];
    snprintf(line, sizeof(line), "MAC %s\n", WiFi.macAddress().c_str());
    s += line;
  }
#endif

#if ENABLE_ESPNOW
  if (gEspNow && gEspNow->initialized) {
    char line[96];
    int peerCount = gEspNow->peerHistoryCount;
    snprintf(line, sizeof(line),
             "ESPNow %s - %dp - %lutx/%lurx\n",
             getEspNowModeString(), peerCount,
             (unsigned long)gEspNow->routerMetrics.messagesSent,
             (unsigned long)gEspNow->routerMetrics.messagesReceived);
    s += line;
  } else {
    s += "ESPNow off\n";
  }
#endif

#if ENABLE_G2_GLASSES
  {
    char line[32];
    if (gBatteryR < 0) snprintf(line, sizeof(line), "G2 batt ?%%\n");
    else               snprintf(line, sizeof(line), "G2 batt %d%%\n", (int)gBatteryR);
    s += line;
  }

  // Ring status. We currently run the ring in info-only mode — we have
  // a live BLE link and we're subscribed to its notify char, but we
  // don't decode the notify frames yet (just hex-dump them at debug
  // level). That means battery percentage, heart rate, HRV, etc. are
  // all reachable in principle (the ring pushes them on the notify
  // stream) but require a frame decoder we haven't written. They also
  // may require completing the pairAuth handshake — the reference
  // suggests health pushes only start after auth, but we haven't
  // verified that empirically.
  //
  // What we CAN show now from the BLE-layer state alone:
  //   * connected up/down
  //   * MTU
  //   * packets received since connect (proxy for "is data flowing?")
  //   * connection age (how long the link has been up)
  //
  // When the frame decoder lands, swap "Ring N rx" for an actual
  // "Ring batt %d%% - HR %d" line. Tracked under (Future) tasks.
  // Two distinct ring states; both are useful to know:
  //   1. Direct BLE — our ESP32 has its own BLE link to the ring on
  //      service bae80001. Future home for pairAuth + health-data
  //      decoding (battery, HR, HRV). Currently info-only.
  //   2. Linked-to-glasses — the Even app has paired the ring with
  //      the glasses (linkToGlasses handshake), so ring gestures
  //      flow through the glasses' BLE connection as SysEvent src=2.
  //      We can only OBSERVE this state, not control it. Detected
  //      by tracking the last SysEvent with src=2 (see
  //      gRingViaGlassesLastSeenMs in handleDevEvent).
  // The two are independent: a ring can be linked to glasses without
  // being directly connected to us, and vice versa.
  if (g2RingIsConnected()) {
    extern void g2RingGetStatus(char* buf, size_t cap);
    char rs[160];
    g2RingGetStatus(rs, sizeof(rs));
    unsigned rxCount = 0;
    char upStr[16] = "?";
    {
      const char* p;
      if ((p = strstr(rs, "rx=")) != nullptr) sscanf(p + 3, "%u", &rxCount);
      if ((p = strstr(rs, "up="))  != nullptr) {
        size_t k = 0;
        for (const char* q = p + 3; *q && *q != ' ' && k < sizeof(upStr) - 1; q++) {
          upStr[k++] = *q;
        }
        upStr[k] = '\0';
      }
    }
    char line[64];
    snprintf(line, sizeof(line), "Ring direct - %u rx - %s\n", rxCount, upStr);
    s += line;
  } else {
    s += "Ring direct: offline\n";
  }

  // Linked-to-glasses freshness window. SysEvents only fire on user
  // input, so a ring that's linked but idle won't keep this stamp
  // current — the meaningful question is "did we see a ring event in
  // a reasonable recency window?" 2 minutes is generous (the Even
  // app's pairing persists indefinitely once stored, but the user
  // might not have used the ring recently enough for us to confirm).
  // After the window, we honestly say "unknown" instead of asserting
  // a state we can't actually verify without input.
  {
    const uint32_t lastMs = gRingViaGlassesLastSeenMs;
    if (lastMs == 0) {
      s += "Ring linked: unknown (no events yet)\n";
    } else {
      const uint32_t agoMs = millis() - lastMs;   // unsigned: wrap-safe
      char line[80];
      if (agoMs < 120000) {
        snprintf(line, sizeof(line),
                 "Ring linked: yes (seen %lus ago)\n",
                 (unsigned long)(agoMs / 1000));
      } else {
        snprintf(line, sizeof(line),
                 "Ring linked: stale (last %lus ago)\n",
                 (unsigned long)(agoMs / 1000));
      }
      s += line;
    }
  }
#endif

  strncpy(out, s.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

// =============================================================================
// G2 page module registry — runtime
// =============================================================================
// Stores up to G2_PAGE_REGISTRY_MAX page module pointers. Pages register
// at startup; the hijack dispatcher and the templated CLI handler iterate
// the table.

#define G2_PAGE_REGISTRY_MAX 16
static const G2PageModule* gPageRegistry[G2_PAGE_REGISTRY_MAX] = { nullptr };
static size_t              gPageRegistryCount = 0;

bool g2RegisterPage(const G2PageModule& spec) {
  if (!spec.name || !spec.buildText) {
    DEBUG_G2F("[G2-Pages] Reject: spec missing name or buildText");
    return false;
  }
  // Dedup by name — re-registration overwrites the prior pointer.
  for (size_t i = 0; i < gPageRegistryCount; i++) {
    if (gPageRegistry[i] && strcmp(gPageRegistry[i]->name, spec.name) == 0) {
      gPageRegistry[i] = &spec;
      DEBUG_G2F("[G2-Pages] Re-registered '%s'", spec.name);
      return true;
    }
  }
  if (gPageRegistryCount >= G2_PAGE_REGISTRY_MAX) {
    DEBUG_G2F("[G2-Pages] Registry full, dropping '%s'", spec.name);
    return false;
  }
  gPageRegistry[gPageRegistryCount++] = &spec;
  DEBUG_G2F("[G2-Pages] Registered '%s' (label='%s', stateful=%s)",
            spec.name,
            spec.hijackLabel ? spec.hijackLabel : "(hidden)",
            spec.handleTap ? "yes" : "no");
  return true;
}

size_t g2RegisteredPageCount(void) { return gPageRegistryCount; }

const G2PageModule* g2RegisteredPageAt(size_t i) {
  return (i < gPageRegistryCount) ? gPageRegistry[i] : nullptr;
}

// Fill `out` with pointers to each registered page's hijackLabel string,
// in registration order. Returns count written. Hidden pages (no label)
// are skipped. Used by both the cmd=17 hijack worker (initial menu) and
// the page-swap worker's auto-recovery path (re-CREATE after a swap that
// torn down the live container but failed to land its replacement).
static size_t populateHijackMenuItems(const char** out, size_t outCap) {
  size_t n = 0;
  for (size_t i = 0; i < gPageRegistryCount && n < outCap; i++) {
    const G2PageModule* p = gPageRegistry[i];
    if (p && p->hijackLabel) out[n++] = p->hijackLabel;
  }
  return n;
}

const G2PageModule* g2FindPageByName(const char* name) {
  if (!name) return nullptr;
  for (size_t i = 0; i < gPageRegistryCount; i++) {
    if (gPageRegistry[i] && strcmp(gPageRegistry[i]->name, name) == 0) {
      return gPageRegistry[i];
    }
  }
  return nullptr;
}

const G2PageModule* g2FindPageByHijackPage(G2HijackPage page) {
  for (size_t i = 0; i < gPageRegistryCount; i++) {
    if (gPageRegistry[i] && gPageRegistry[i]->hijackPage == page) {
      return gPageRegistry[i];
    }
  }
  return nullptr;
}

// -----------------------------------------------------------------------------
// Built-in page registrations
// -----------------------------------------------------------------------------
// File-static specs so the registry can hold stable pointers across the
// session. Status doesn't have its own G2_Page_*.cpp file; its
// buildG2StatusSnapshot lives in this file. Other pages have their own
// modules (G2_Page_System, G2_Page_Sensors, etc.) and just supply their
// build/show functions here.

static const G2PageModule kStatusPage = {
  "status", "Status",
  "Show G2 status snapshot on the lens",
  buildG2StatusSnapshot,          // already file-local in this same .cpp
  /*showMenu=*/  nullptr,         // read-only — uses live-page renderer
  /*handleTap=*/ nullptr,
  G2_HIJACK_PAGE_TEXT_VIEW,
  /*liveIntervalMs=*/ 5000,       // refresh every 5s; double-tap = manual
};

static const G2PageModule kSensorsPage = {
  "sensors", "Sensors",
  "Show device sensor list on the lens",
  g2BuildSensorList,
  /*showMenu=*/  nullptr,
  /*handleTap=*/ nullptr,
  G2_HIJACK_PAGE_TEXT_VIEW,
};

static const G2PageModule kSystemPage = {
  "system", "System",
  "Show System info page on the lens",
  g2BuildSystemPage,
  /*showMenu=*/  nullptr,
  /*handleTap=*/ nullptr,
  G2_HIJACK_PAGE_TEXT_VIEW,
};

static const G2PageModule kNetworkPage = {
  "network", "Network",
  "Show Network info page on the lens",
  g2BuildNetworkInfo,
  g2ShowNetworkMenu,
  g2NetworkHandleTap,
  G2_HIJACK_PAGE_NETWORK,
};

static const G2PageModule kFilesPage = {
  "files", "Files",
  "Show Files browser on the lens",
  g2BuildFilesInfo,
  g2ShowFilesMenu,
  g2FilesHandleTap,
  G2_HIJACK_PAGE_FILES,
};

static const G2PageModule kSettingsPage = {
  // CLI suffix is "settingspage" not "settings" — `g2settings` already
  // exists for the protocol-debug verbose toggle (see cmd_g2settings).
  "settingspage", "Settings",
  "Show Settings inspector on the lens",
  g2BuildSettingsInfo,
  g2ShowSettingsMenu,
  g2SettingsHandleTap,
  G2_HIJACK_PAGE_SETTINGS,
};

static const G2PageModule kTestSuitePage = {
  "tests", "Tests",
  "Show on-glasses transport test bench",
  g2BuildTestSuiteInfo,
  g2ShowTestSuiteMenu,
  g2TestSuiteHandleTap,
  G2_HIJACK_PAGE_TESTS,
};

static void registerG2Pages(void) {
  // Order = menu order in the hijack list.
  g2RegisterPage(kStatusPage);
  g2RegisterPage(kSensorsPage);
  g2RegisterPage(kSystemPage);
  g2RegisterPage(kNetworkPage);
  g2RegisterPage(kFilesPage);
  g2RegisterPage(kSettingsPage);
  g2RegisterPage(kTestSuitePage);
}

// =============================================================================
// Hijack entry / exit
// =============================================================================

// Hijack worker — runs the Shutdown+CREATE handshake on its own short-lived
// task because the CREATE path blocks up to 1.5 s waiting for the ack to
// arrive on the R temple's notify pipe. If we ran this synchronously from
// handleEnvelope we'd be pinning the same notify task we depend on to
// deliver that ack → deadlock / 1500 ms timeout every time.
//
// Sequence (critical — don't reorder):
//   1. Cmd=18 MenuStartUpFailed: cancels the in-flight Blocks launch so
//      the firmware releases the widgetId=10509 slot it just reserved.
//      Previously we sent Cmd=9 Shutdown here — that returned
//      res=11 APP_REQUEST_UPGRADE_SHUTDOWN_FAILED because no container
//      had been instantiated yet; the firmware was in a "menu launch
//      pending" state waiting for exactly this response packet, and our
//      follow-up CREATE got dropped. Reference: `MenuStartUpResPonse`
//      at wrapper field 21 of evenhub_main_msg_ctx
//      (ble/gen/EvenHub_pb.ts:120). Fire-and-forget — no ack.
//   2. Short settle delay (300 ms): lets the firmware finish whatever
//      "launch failed" animation it runs before we try to claim the
//      display. Empirically calibrated.
//   3. Clear gR.containerReady: no container exists after cancel, so we
//      MUST go through CREATE (not REBUILD) next.
//   4. Cmd=0 CREATE with widgetId=10509: the firmware associates our
//      new container with the same widget it just announced in cmd=17,
//      so it's accepted without collision.
static void hijackWorkerTask(void* /*arg*/) {
  if (!gR.connected) {
    DEBUG_G2F("[G2] Hijack: right temple not connected — aborting");
    vTaskDelete(nullptr);
    return;
  }

  // TEMPORARY: two-item selectable list, for touchpad-interaction testing.
  // The firmware draws a native selection highlight and routes gestures
  // into List_ItemEvent sub-messages on sid=0x0D. Those currently show
  // up in the log as `Event → UNKNOWN (N B)` with a head dump — use the
  // byte shapes to confirm which gesture maps to which selection change
  // before promoting this into a proper scrollable menu widget.
  //
  // To return to the status-snapshot page, swap the list CREATE below
  // back to `sendCreateAndWait(gR, snapshot, BLOCKS_WIDGET_ID)` and
  // restore the buildG2StatusSnapshot() call. Leaving both paths in code
  // while we iterate.
  // Menu items are built from the page registry. Each registered page
  // with a non-null hijackLabel becomes one menu entry. New pages don't
  // require touching this code — register them via g2RegisterPage().
  static const char* hijackMenuItems[G2_PAGE_REGISTRY_MAX];
  size_t kHijackMenuCount = populateHijackMenuItems(hijackMenuItems,
                                                    G2_PAGE_REGISTRY_MAX);
  const char* const* kHijackMenuItems = hijackMenuItems;

  DEBUG_G2F("[G2] Hijack Blocks → cancelling pending launch, then showing "
            "%u-item menu", (unsigned)kHijackMenuCount);

  // Step 1 + 2: cancel the pending Blocks launch, give firmware time to settle.
  if (!sendMenuFailedAndSettle(gR, /*settleMs*/ 300)) {
    DEBUG_G2F("[G2] Hijack: MenuStartupFailed send failed");
    vTaskDelete(nullptr);
    return;
  }

  // Step 3: no container exists after cancel — we must CREATE.
  gR.containerReady = false;

  // Step 4: CREATE-list tagged with the Blocks widgetId so firmware
  // associates it with the launch it just announced.
  if (sendCreateListAndWait(gR, kHijackMenuItems, kHijackMenuCount,
                            BLOCKS_WIDGET_ID, G2_GEOM_LARGE)) {
    gR.containerReady   = true;
    gR.containerIsList  = true;   // hijack menu is a list widget
    gHijackActive       = true;
    gHijackStartedMs  = millis();
    // Mirror into the lens state struct (single source of truth for
    // external readers — see Optional_EvenG2.h G2LensState).
    g2LensSetContainer(true, true, BLOCKS_WIDGET_ID);
    g2LensSetHijackActive(true);
    g2SetHijackPage(G2_HIJACK_PAGE_MAIN);  // fresh hijack always starts at MAIN
    BROADCAST_PRINTF("[G2] Blocks hijack: %u-item menu shown",
                     (unsigned)kHijackMenuCount);
    g2PushStatusEvent("hijack-on");
  } else {
    DEBUG_G2F("[G2] Hijack: CREATE-list failed — menu NOT displayed");
    g2PushStatusEvent("hijack-fail");
  }
  vTaskDelete(nullptr);
}

// Per-arm dedup for hijack taps. The firmware routes every List/Text
// click through BOTH temples — we get the same event on L and R within
// ~20 ms. Without this dedup, every user tap triggers the hijack
// handler twice, which would queue two g2ShowText calls back-to-back
// and race the write mutex. 150 ms window matches the existing
// sid=0x0D dedup we use for state events.
static uint32_t gLastHijackTapMs  = 0;
static uint32_t gLastHijackTapIdx = 0xFFFFFFFF;

static bool shouldDedupHijackTap(uint32_t idx) {
  const uint32_t now = millis();
  if (idx == gLastHijackTapIdx && (now - gLastHijackTapMs) < 150) return true;
  gLastHijackTapIdx = idx;
  gLastHijackTapMs  = now;
  return false;
}

// Called from handleDevEvent when a ListEvent CLICK on container "app"
// is seen AND dedup passes. Runs on the BLE notify task, so any send
// must be non-blocking (no ack waits). g2ShowText on a hijack-CREATEd
// container takes the REBUILD fast path (containerReady=true), which
// is send-and-forget — safe from this context.
//
// Return-to-list UX: the firmware's normal display-auto-off handles
// it. When the user stops interacting, the display blanks, we get
// DISPLAY_OFF, we clear containerReady + hijackActive, and the next
// tap on Blocks starts a fresh hijack from scratch. If the user
// tap-and-holds, the firmware's own Exit? dialog takes over (see
// docs/G2_PROTOCOL.md — Widget lifecycle events).
// (Hijack menu items come from populateHijackMenuItems above — single
// definition shared by the cmd=17 worker, the page-swap auto-recovery
// branch, and the redraw path below.)

// Helper: invoke the right rendering path for a page when the user taps
// it from the MAIN hijack menu. If the page provides a custom showMenu,
// use it (stateful pages with their own list layout). Otherwise build
// text and render via g2ShowTextAsList (read-only info views).
static void invokePageFromMain(const G2PageModule& p) {
  if (p.showMenu) {
    p.showMenu();
    BROADCAST_PRINTF("[G2] Hijack: %s tapped — opened sub-page",
                     p.hijackLabel ? p.hijackLabel : p.name);
    return;
  }
  // Live page: start the auto-refresh worker. Still renders an initial
  // list synchronously; subsequent ticks REBUILD in place via Cmd=7.
  if (p.liveIntervalMs > 0 && p.buildText) {
    if (g2StartLiveListPage(p.buildText, p.liveIntervalMs)) {
      g2SetHijackPage(p.hijackPage);
      BROADCAST_PRINTF("[G2] Hijack: %s tapped — live page (every %u ms)",
                       p.hijackLabel ? p.hijackLabel : p.name,
                       (unsigned)p.liveIntervalMs);
    } else {
      DEBUG_G2F("[G2] Hijack: %s tap — live page start failed", p.name);
    }
    return;
  }
  // Default: build text content and render as list-as-text. Buffer is
  // generous because Status / System contain a handful of lines each;
  // sized once for the worst case rather than per-page.
  char buf[512];
  if (p.buildText) p.buildText(buf, sizeof(buf));
  else             buf[0] = '\0';
  if (g2ShowTextAsList(buf)) {
    BROADCAST_PRINTF("[G2] Hijack: %s tapped — list shown (%u B)",
                     p.hijackLabel ? p.hijackLabel : p.name,
                     (unsigned)strlen(buf));
  } else {
    DEBUG_G2F("[G2] Hijack: %s tap — show failed", p.name);
  }
}

static void handleHijackMenuTap(uint32_t idx) {
  // Sub-page dispatch: if a registered page claims this hijackPage,
  // route the tap to its handleTap. Pages without a custom handleTap
  // (read-only views) get the TEXT_VIEW default: idx 0 → MAIN.
  G2HijackPage current = g2GetHijackPage();
  if (current != G2_HIJACK_PAGE_MAIN) {
    const G2PageModule* p = g2FindPageByHijackPage(current);
    if (p && p->handleTap) {
      p->handleTap(idx);
      return;
    }
    // No custom handler (or the page that owns this hijackPage isn't
    // registered any more) — fall back to the read-only TEXT_VIEW
    // default: idx 0 returns to the main menu, rest are no-ops.
    if (idx == 0) {
      g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
      extern void g2RedrawHijackMainMenu();
      g2RedrawHijackMainMenu();
    }
    return;
  }

  // MAIN page dispatch. Walk the registry's hijack-visible entries in
  // order; idx is the visual position in the menu.
  size_t visibleIdx = 0;
  for (size_t i = 0; i < gPageRegistryCount; i++) {
    const G2PageModule* p = gPageRegistry[i];
    if (!p || !p->hijackLabel) continue;
    if (visibleIdx == idx) {
      invokePageFromMain(*p);
      return;
    }
    visibleIdx++;
  }
  DEBUG_G2F("[G2] Hijack: tap idx=%u out of menu range (%u items)",
            (unsigned)idx, (unsigned)visibleIdx);
}

// Re-render the top-level hijack menu. Used by sub-pages when the user taps
// "<- Back". The container is still ours since we haven't shut it down,
// so this is just a fast REBUILD send-and-forget. Caller resets
// hijackPage to MAIN before calling.
void g2RedrawHijackMainMenu() {
  const char* items[G2_PAGE_REGISTRY_MAX];
  size_t n = populateHijackMenuItems(items, G2_PAGE_REGISTRY_MAX);
  if (g2ShowListPage(items, n)) {
    DEBUG_G2F("[G2] Hijack: main menu redrawn (%u items)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Hijack: main menu redraw FAILED");
  }
}

// Called from handleEnvelope() on the right temple only when a cmd=17
// MenuStartUpEvent is decoded. Any widget other than BLOCKS_WIDGET_ID is
// logged and ignored so the remaining built-in mini-apps still function.
static void handleMenuStartUp(G2Temple& t, uint32_t widgetId) {
  DEBUG_G2F("[G2-%c] MenuStartUp widgetId=%u", t.side, (unsigned)widgetId);
  if (widgetId != BLOCKS_WIDGET_ID) return;
  if (t.side != 'R') return;  // right-arm master (match reference convention)
  if (gHijackActive) {
    // A fresh cmd=17 for the same widget definitively means the prior
    // launch is gone on the firmware side — the glasses don't re-announce
    // a widget that's already running. If we still think we're active,
    // our exit path didn't fire: most commonly because the firmware
    // tore down the app with a "connection issue" error instead of a
    // DISPLAY_OFF event (observed 2026-04-24: swiped/tapped in the
    // hijacked menu a few times, glasses gave up, our flag stuck). Force-
    // clear and proceed with a fresh hijack rather than silently
    // ignoring the user's tap.
    DEBUG_G2F("[G2] Hijack: stale active flag (firmware relaunch) — "
              "clearing and proceeding with fresh hijack");
    gHijackActive     = false;
    gR.containerReady = false;
  }

  // Spawn a detached worker; caller (BLE notify task) returns immediately
  // so the CREATE ack that the worker waits on can reach handleEnvelope.
  BaseType_t rc = xTaskCreate(hijackWorkerTask, "g2_hijack",
                              /*stack*/ 4096, nullptr,
                              /*prio*/  tskIDLE_PRIORITY + 2,
                              nullptr);
  if (rc != pdPASS) {
    DEBUG_G2F("[G2] Hijack: xTaskCreate failed (rc=%d) — skipping "
              "(inline fallback would deadlock the notify task)", (int)rc);
    // No inline fallback: running the Shutdown+CREATE handshake directly
    // on the BLE notify task would block it for up to 1.5 s waiting for
    // the CREATE ack that must arrive on that same task → guaranteed
    // deadlock. Better to drop the hijack than to stall the stack.
  }
}

// Hijack teardown. Called when the lens swipes closed (DISPLAY_OFF on
// sid=0x0D) or by the safety fallback in the heartbeat tick. Sends a Cmd=9 ShutdownPage
// to the right temple so the plugin task releases its container slot,
// and clears the cached containerReady so the next tap re-CREATEs.
// Safe to call from any context — only the first caller per hijack does
// real work thanks to the gHijackActive guard.
static void sendHijackShutdown(const char* reason) {
  if (!gHijackActive) return;
  gHijackActive = false;
  g2LensSetHijackActive(false);
  g2LensClearContainer();
  g2LensClearOverlay();
  DEBUG_G2F("[G2] Hijack exit (%s) — sending ShutdownPage",
            reason ? reason : "?");
  g2PushStatusEvent(reason ? reason : "hijack-off");
  if (gR.connected) {
    uint8_t buf[32];
    size_t n = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN,
                               /*exitMode*/ 0, buf, sizeof(buf));
    if (n) sendEnvelope(gR, buf, n);
    // Firmware tore the container down on its side the moment the display
    // went off — mirror that locally so the next hijack / g2show starts
    // clean with a fresh CREATE.
    gR.containerReady = false;
  }
}

static void onDisplayOffWhileHijacked(char side) {
  (void)side;
  sendHijackShutdown("DISPLAY_OFF");
}

// Log every top-level pb field in `payload` as a single line per field.
// Use for unmapped sids where we don't yet know the schema — labelled
// captures of (gesture, voice command, etc.) → field values are how we
// reverse-engineer meaning. Caps len-delim hex at 16 B to keep lines short.
static void logPbFlat(char side, const char* tag,
                      const uint8_t* payload, size_t payloadLen) {
  size_t pos = 0;
  while (pos < payloadLen) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(payload, payloadLen, &pos, &field, &wire)) {
      DEBUG_G2F("[G2-%c] %s: tag parse failed at off=%u",
                side, tag, (unsigned)pos);
      return;
    }
    if (wire == G2_PB_WIRE_VARINT) {
      uint64_t v;
      if (!g2PbReadVarint(payload, payloadLen, &pos, &v)) return;
      DEBUG_G2F("[G2-%c] %s f%u varint=%llu",
                side, tag, (unsigned)field, (unsigned long long)v);
    } else if (wire == G2_PB_WIRE_LEN_DELIM) {
      uint64_t sl;
      if (!g2PbReadVarint(payload, payloadLen, &pos, &sl)) return;
      if (pos + sl > payloadLen) return;
      char hex[3 * 16 + 1]; size_t bp = 0;
      const size_t show = sl < 16 ? (size_t)sl : 16;
      for (size_t i = 0; i < show && bp + 3 < sizeof(hex); i++) {
        bp += snprintf(hex + bp, sizeof(hex) - bp, "%02X ", payload[pos + i]);
      }
      if (bp > 0) hex[bp - 1] = '\0'; else hex[0] = '\0';
      DEBUG_G2F("[G2-%c] %s f%u bytes(%u)=[%s]%s",
                side, tag, (unsigned)field, (unsigned)sl, hex,
                (size_t)sl > show ? " ..." : "");
      pos += (size_t)sl;
    } else {
      if (!g2PbSkipField(payload, payloadLen, &pos, wire)) return;
    }
  }
}

// Per-subsystem symbolic name lookups for the voice/language sids
// (sid=0x05/0x06/0x0A/0x0B). All four use the same wrapper shape —
// commandId at f1, magicRandom at f2, body at varying field number — but
// each has its own commandId enum. Keep these tight: lookup-only, no
// fallback strings (default "?" handled by the caller logger).
static const char* translateCmdName(uint32_t c) {
  switch (c) {
    case 0:   return "NONE";
    case 1:   return "CTRL";
    case 2:   return "RESULT";
    case 161: return "NOTIFY";
    case 162: return "COMM_RSP";
    case 163: return "MODE_SWITCH";
    case 255: return "HEARTBEAT";
    default:  return "?";
  }
}
static const char* telepromptCmdName(uint32_t c) {
  switch (c) {
    case 0:   return "NONE";
    case 1:   return "CONTROL";
    case 2:   return "FILE_LIST";
    case 3:   return "PAGE_DATA";
    case 4:   return "PAGE_AI_SYNC";
    case 161: return "STATUS_NOTIFY";
    case 162: return "FILE_LIST_REQUEST";
    case 163: return "FILE_SELECT";
    case 164: return "PAGE_DATA_REQUEST";
    case 165: return "PAGE_SCROLL_SYNC";
    case 166: return "COMM_RSP";
    case 255: return "HEARTBEAT";
    default:  return "?";
  }
}
static const char* transcribeCmdName(uint32_t c) {
  switch (c) {
    case 0:   return "NONE";
    case 1:   return "CTRL";
    case 2:   return "RESULT";
    case 161: return "NOTIFY";
    case 162: return "COMM_RSP";
    case 255: return "HEARTBEAT";
    default:  return "?";
  }
}
static const char* conversateCmdName(uint32_t c) {
  switch (c) {
    case 0:   return "NONE";
    case 1:   return "CONTROL";
    case 2:   return "PREP_NOTE_LIST_REQUEST";
    case 3:   return "PREP_NOTE_LIST";
    case 4:   return "PREP_NOTE_SELECT";
    case 5:   return "TAG_DATA";
    case 6:   return "TRANSCRIBE_DATA";
    case 7:   return "PREP_NOTE_PACKET";
    case 161: return "STATUS_NOTIFY";
    case 162: return "COMM_RSP";
    case 163: return "TAG_TRACKING_DATA";
    case 255: return "HEARTBEAT";
    default:  return "?";
  }
}

// Shared decode + log for sid=0x05/0x06/0x0A/0x0B. Pulls cmd (f1) and
// magic (f2) and emits one symbolic log line. If a nested body is
// present and its first child is a varint at field 1 (the common
// errorCode shape for COMM_RSP frames), surface that as `code=N`.
static void voiceLangLog(char side, const char* sysName,
                         const char* (*cmdName)(uint32_t),
                         const uint8_t* pb, size_t pbLen) {
  uint32_t cmd = 0, magic = 0;
  if (pbLen >= 2 && pb[0] == 0x08) cmd = pb[1] & 0x7F;  // common single-byte case
  // Multi-byte commandId (e.g. 162 = 0xA2 0x01).
  if (pbLen >= 2 && pb[0] == 0x08 && (pb[1] & 0x80)) {
    cmd = peekVarint(pb, pbLen, 1);
  }
  // Walk past cmd's varint to find the f2 magic tag.
  size_t off = 1;
  while (off < pbLen && (pb[off] & 0x80)) off++;
  off++;
  if (off + 1 < pbLen && pb[off] == 0x10) {
    magic = peekVarint(pb, pbLen, off + 1);
  }
  // Look for a nested-body errorCode (any nested f3..f15 whose body is
  // `08 <varint>`). Cheap pattern match: scan for `0x08` immediately
  // following a length-delim len byte.
  bool haveCode = false;
  uint32_t code = 0;
  for (size_t i = 0; i + 3 < pbLen; i++) {
    // length-delim tag bytes for fields 3..15: 0x1A,0x22,0x2A,0x32,0x3A,
    // 0x42,0x4A,0x52,0x5A,0x62,0x6A,0x72,0x7A
    const uint8_t tag = pb[i];
    if ((tag & 0x07) != 2) continue;            // wire 2?
    const uint32_t fld = tag >> 3;
    if (fld < 3 || fld > 15) continue;
    const uint8_t len = pb[i + 1];
    if (len < 2 || i + 2 + len > pbLen) continue;
    if (pb[i + 2] == 0x08) {
      code = peekVarint(pb, pbLen, i + 3);
      haveCode = true;
      break;
    }
  }
  if (haveCode) {
    DEBUG_G2F("[G2-%c] %s %s(%u) magic=%u code=%u (%u B)",
              side, sysName, cmdName(cmd), (unsigned)cmd,
              (unsigned)magic, (unsigned)code, (unsigned)pbLen);
  } else {
    DEBUG_G2F("[G2-%c] %s %s(%u) magic=%u (%u B)",
              side, sysName, cmdName(cmd), (unsigned)cmd,
              (unsigned)magic, (unsigned)pbLen);
  }
}

static void handleEnvelope(G2Temple& t, const G2EnvelopeView& env) {
  switch (env.sid) {
    case G2_SID_EVEN_CORE: {
      // cmd=17 arrives as flag=0x01 async (the firmware does NOT expect
      // us to ack it). Decode and route before the response parser so
      // menu-startup frames aren't logged as generic "AckOnly".
      if (env.flag == G2_FLAG_NOTIFY || env.flag == G2_FLAG_NOTIFY_ALT) {
        uint32_t widgetId = 0;
        if (parseMenuStartUpEvent(env.payload, env.payloadLen, &widgetId)) {
          handleMenuStartUp(t, widgetId);
          break;
        }
        // Cmd=2 = OS_NOITY_EVENT_TO_APP_PACKET with DevEvent body — user
        // taps on list items, firmware-initiated SYSTEM_EXIT_EVENT before
        // teardown, etc. Reference receives-only; no reply needed, we
        // just decode and act on subtypes we care about.
        if (parseAndDispatchDevEvent(t, env.payload, env.payloadLen)) {
          break;
        }
      }
      // Decode the wrapper so callers can see at a glance whether the
      // last rebuild / create / text / shutdown actually landed. Only
      // page-op response Cmds carry a ResCmdMsg we should interpret;
      // everything else (heartbeat acks, audio-ctrl acks, …) is logged
      // as a plain ack so we don't pretend to know a res code we didn't
      // actually decode.
      uint32_t cmd = 0, magic = 0, res = 0;
      const G2ResKind kind = parseEvenCoreResponse(env.payload, env.payloadLen,
                                                   &cmd, &magic, &res);
      switch (kind) {
        case G2ResKind::Response:
          DEBUG_G2F("[G2-%c] EvenCore %s (cmd=%u magic=%u) res=%u (%s)",
                    t.side, evenCoreCmdName(cmd),
                    (unsigned)cmd, (unsigned)magic,
                    (unsigned)res, evenCoreResCode(res));
          // Dump the ring buffer ONLY on actual failures. Earlier this
          // tripped on every non-zero code — but res codes are paired
          // (success/failure) per response type, so RebuildSuccess=6,
          // ShutdownSuccess=10, HeartbeatSuccess=12 etc. were firing
          // spurious dumps.
          if (evenCoreResIsFailure(res)) {
            char reason[64];
            snprintf(reason, sizeof(reason),
                     "EvenCore %s res=%u (%s)",
                     evenCoreCmdName(cmd), (unsigned)res,
                     evenCoreResCode(res));
            g2RingDump(reason);
          }
          // Signal the CREATE ack waiter in g2ShowText if this is the
          // response we've blocked on. Cmd=1 is OS_RESPONSE_CREATE_STARTUP;
          // an empty body (pb default) counts as res=0 = Success.
          if (cmd == 1 && gCreateAckSem &&
              (uint8_t)magic == gExpectMagic) {
            gCreateOk = (res == 0);
            xSemaphoreGive(gCreateAckSem);
          }
          // Cmd=8 RebuildResp — signals sendRebuildListAndWait when the
          // experimental REBUILD-list path is in use. res=6 = RebuildSuccess.
          if (cmd == 8 && gRebuildAckSem &&
              (uint8_t)magic == gExpectRebuildMagic) {
            gRebuildOk = (res == 6);
            xSemaphoreGive(gRebuildAckSem);
          }
          break;
        case G2ResKind::AckOnly:
          DEBUG_G2F("[G2-%c] EvenCore %s ack (cmd=%u magic=%u, %u B)",
                    t.side, evenCoreCmdName(cmd),
                    (unsigned)cmd, (unsigned)magic,
                    (unsigned)env.payloadLen);
          // Cmd=4 ImageRawResp — count toward the expected push-ack
          // window if registered. Magic comparison uses the low byte
          // because the firmware truncates to uint8 (see G2_PROTOCOL.md
          // "Push magic uint8 constraint"). Range comparison handles
          // sub-256 magics correctly even when Lo == Hi.
          if (cmd == 4 && gImgPushAckSem && gImgPushTarget > 0) {
            const uint8_t mlo = (uint8_t)magic;
            const bool inRange =
                (gImgPushExpectLo <= gImgPushExpectHi)
                    ? (mlo >= gImgPushExpectLo && mlo <= gImgPushExpectHi)
                    : (mlo >= gImgPushExpectLo || mlo <= gImgPushExpectHi);
            if (inRange) {
              gImgPushAcked++;
              if (gImgPushAcked >= gImgPushTarget) {
                xSemaphoreGive(gImgPushAckSem);
              }
            }
          }
          // Cmd=12 HeartbeatAck confirms the plugin task on this temple is
          // alive. Reset the miss counter so the watchdog doesn't trip.
          // See the heartbeatMissed comment on G2Temple for the why.
          if (cmd == 12) {
            if (t.heartbeatMissed > 0 || t.pluginDead) {
              DEBUG_G2F("[G2-%c] Plugin heartbeat recovered (was %u miss%s%s)",
                        t.side, (unsigned)t.heartbeatMissed,
                        t.heartbeatMissed == 1 ? "" : "es",
                        t.pluginDead ? ", dead flag cleared" : "");
            }
            t.heartbeatMissed = 0;
            t.pluginDead = false;

            // Decode the trailing field 15 sub-message (`7A 04 08 NN 10 0C`).
            // Empirical: f1 = monotonically increasing seq, f2 = constant
            // 12 (cmd echo). Logged at DEBUG_G2_DUMP so we can confirm the
            // pattern across firmware revisions without spamming the
            // normal output. Drop this once it's been proven static long
            // enough — keeping it for the protocol-exploration phase.
            uint64_t hbSeq = 0, hbEcho = 0;
            if (g2DecodeHeartbeatAckTail(env.payload, env.payloadLen,
                                         &hbSeq, &hbEcho)) {
              DEBUG_G2_DUMPF("[G2-%c] HeartbeatAck tail seq=%llu echo=%llu",
                             t.side,
                             (unsigned long long)hbSeq,
                             (unsigned long long)hbEcho);
            }
          }
          break;
        case G2ResKind::None:
          DEBUG_G2F("[G2-%c] EvenCore resp (%u B) — unparseable",
                    t.side, (unsigned)env.payloadLen);
          break;
      }
      break;
    }

    case G2_SID_STATE_EVENT:
      // Empirical decoding from labelled hardware captures (2026-04-24).
      // The sid=0x0D channel reports two coarse-grained things:
      //
      //   6 B `08 01 1A 02 08 01`   SysEvent{EventType=1} — "user input
      //       happened." Fires for any of: head-up wake, single tap, double
      //       tap, swipe up/down. The firmware does NOT distinguish gesture
      //       types at this channel — they all emit the same bytes.
      //
      //   4 B `08 01 1A 00`         SysEvent{} (empty body) — "display
      //       transitioned from on → off." Fires for BOTH causes:
      //         • inactivity timeout
      //         • user gesture that closed the display (e.g. double-tap off)
      //       In other words: absence of a 4-byte event within a few seconds
      //       of the 6-byte event means the display is still on.
      //
      //   8 B `08 01 1A 04 08 01 10 03`  SysEvent{EventType=1,
      //       EventSource=3} — occasionally seen with explicit source
      //       (3 = GLASSES_L per the reference's EventSourceType enum).
      //
      //   7-9 B with inner field 1 = 4094  custom-app events — only fire
      //       when a third-party Even mini-app is running on the glasses.
      //       We ignore these for built-in gesture handling.
      //
      // To distinguish individual gestures (tap vs double-tap vs swipe
      // direction) we'd need to decode the parallel sid=0x01 flag=0x01
      // chatter that clusters around every event — that's where richer
      // input data is suspected to live. Not yet decoded.
      //
      // Both arms emit the same event simultaneously. Caller-side dedupe
      // is a future fix.
      dispatchEventPayload(t.side, env.payload, env.payloadLen);
      break;

    case G2_SID_SETTINGS: {
      // Settings arrive in two flavours:
      //   flag=0x00 → direct response to our g2BuildSettingBasicRequest
      //   flag=0x01 → unsolicited async push (battery tick, etc.)
      // Both have the same pb shape, so we just parse unconditionally.
      // This keeps gBatteryL/gBatteryR + gFwVersion fresh without poll.
      uint8_t pct = 0;
      if (g2ParseSettingBattery(env.payload, env.payloadLen, &pct)) {
        DEBUG_G2F("[G2-%c] Battery: %u%% (%s)", t.side, (unsigned)pct,
                  env.flag == G2_FLAG_RESPONSE ? "response" : "async");
        const int8_t prev = (t.side == 'L') ? gBatteryL : gBatteryR;
        const int8_t newPct = (int8_t)pct;
        if (t.side == 'L') gBatteryL = newPct;
        else               gBatteryR = newPct;
        // Only push if the displayed value actually changed — avoids
        // hammering browsers with identical events every ~5 s.
        if (newPct != prev) {
          g2PushStatusEvent(t.side == 'L' ? "batt-L" : "batt-R");
        }
      }

      // Silent / DND mode toggle — firmware pushes
      // `deviceSendInfoToApp{f2=0|1}` whenever the user tap-and-holds
      // both temples to enable / disable silent mode. We mirror locally
      // so any client (web UI, automation rules) can react. Most
      // settings pushes (battery, version) DON'T carry this field, so
      // a `false` return is "no state change reported here" not
      // "silent off".
      uint8_t silentFlag = 0;
      if (g2ParseSettingSilentMode(env.payload, env.payloadLen, &silentFlag)) {
        const int8_t newSilent = silentFlag ? 1 : 0;
        if (newSilent != gSilentMode) {
          DEBUG_G2F("[G2-%c] Silent/DND %s (was %s)",
                    t.side,
                    newSilent ? "ON" : "OFF",
                    gSilentMode < 0 ? "unknown"
                      : (gSilentMode ? "ON" : "OFF"));
          gSilentMode = newSilent;
          g2PushStatusEvent(newSilent ? "silent-on" : "silent-off");
        }
      }

      // Firmware version extraction — the string doesn't appear in every
      // settings push, but when it does we cache it for `g2info` / the
      // web UI. Only log on transitions so we don't spam the serial on
      // every battery tick that happens to include the version.
      char ver[32] = {0};
      if (g2ParseSettingVersion(env.payload, env.payloadLen, ver, sizeof(ver))) {
        if (strcmp(ver, gFwVersion) != 0) {
          DEBUG_G2F("[G2-%c] Firmware version: '%s' (was '%s')",
                    t.side, ver, gFwVersion);
          strncpy(gFwVersion, ver, sizeof(gFwVersion) - 1);
          gFwVersion[sizeof(gFwVersion) - 1] = '\0';
          g2PushStatusEvent("fw-ver");
        }
      }

      // Verbose field dump — opt-in via `g2settings verbose on`. Walks
      // every inner field of the settings body and logs the numeric
      // value + first 16 bytes of any len-delim. Use this to identify
      // what fields mean beyond battery/version that we haven't mapped.
      if (gG2SettingsVerbose) {
        // The logger callback can't capture t.side via closure (plain C
        // fn pointer), so stash it in a file-scope var just before the
        // dump. Single-threaded within the notify task so this is safe.
        static char   dumpSide = '-';
        static uint8_t dumpFlag = 0;
        dumpSide = t.side; dumpFlag = env.flag;
        g2DumpSettingFields(env.payload, env.payloadLen,
          [](uint32_t field, uint8_t wire, uint64_t v,
             const uint8_t* bytes, size_t byteLen) {
            if (wire == G2_PB_WIRE_VARINT) {
              DEBUG_G2F("[G2-%c] setting field %u varint=%llu%s",
                        dumpSide, (unsigned)field, (unsigned long long)v,
                        (field == G2_SET_REQ_F_BATT) ? " [BATT]" : "");
            } else if (wire == G2_PB_WIRE_LEN_DELIM) {
              // Print bytes as ASCII if printable, else hex.
              char buf[64]; size_t bp = 0;
              bool isAscii = true;
              for (size_t i = 0; i < byteLen && i < 24; i++) {
                uint8_t b = bytes[i];
                if (b < 0x20 || b > 0x7E) { isAscii = false; break; }
              }
              if (isAscii && byteLen > 0) {
                size_t copy = byteLen < sizeof(buf) - 1 ? byteLen : sizeof(buf) - 1;
                memcpy(buf, bytes, copy); buf[copy] = '\0';
                DEBUG_G2F("[G2-%c] setting field %u str='%s'%s",
                          dumpSide, (unsigned)field, buf,
                          (field == G2_SET_REQ_F_VER) ? " [VER]" : "");
              } else {
                for (size_t i = 0; i < byteLen && i < 16 && bp + 3 < sizeof(buf); i++) {
                  bp += snprintf(buf + bp, sizeof(buf) - bp, "%02X ", bytes[i]);
                }
                if (bp > 0) buf[bp - 1] = '\0';
                DEBUG_G2F("[G2-%c] setting field %u bytes(%u)=[%s]",
                          dumpSide, (unsigned)field, (unsigned)byteLen, buf);
              }
            }
          });
      }
      break;
    }

    case G2_SID_APP_LAUNCH:
      // sid=0x01 is the "app" channel broadly. Responses (flag=0x00) are
      // direct replies to our AppLaunch prelude and other host-initiated
      // app-channel commands — brief log, no decoder for now. Flag 0x01
      // is the interesting one: the firmware bursts these around every
      // user gesture and they MAY carry per-gesture discriminators we
      // can't see on sid=0x0D (which collapses tap/double-tap/swipe into
      // one "user activity" bucket). Route through decodeAppAsync so
      // labelled captures can tell us which codes map to which gestures.
      if (env.flag == G2_FLAG_RESPONSE) {
        DEBUG_G2F("[G2-%c] sid=0x01 response pb=%u",
                  t.side, (unsigned)env.payloadLen);
      } else {
        decodeAppAsync(t.side, env.flag, env.payload, env.payloadLen);
      }
      break;

    case G2_SID_TRANSLATE:
      voiceLangLog(t.side, "Translate",  translateCmdName,  env.payload, env.payloadLen);
      break;
    case G2_SID_TELEPROMPT:
      voiceLangLog(t.side, "Teleprompt", telepromptCmdName, env.payload, env.payloadLen);
      break;
    case G2_SID_TRANSCRIBE:
      voiceLangLog(t.side, "Transcribe", transcribeCmdName, env.payload, env.payloadLen);
      break;
    case G2_SID_CONVERSATE:
      voiceLangLog(t.side, "Conversate", conversateCmdName, env.payload, env.payloadLen);
      break;

    case G2_SID_EVEN_AI: {
      // Front-pane Even-AI subsystem (`UI_FOREGROUND_EVEN_AI_ID`). Carries
      // `EvenAIDataPackage` from `even_ai_pb.ts`. Decode the cmd byte and
      // surface frames by symbolic name; unknown shapes drop into
      // logPbFlat so labelled captures continue to feed RE work.
      const uint8_t* pb = env.payload;
      const size_t   pbLen = env.payloadLen;
      uint32_t cmd = 0;
      if (pbLen >= 2 && pb[0] == 0x08) {
        cmd = (uint32_t)pb[1];  // single-byte varint covers all known cmd ids
      }
      // Pull magic from `10 <varint>` if present so logs are correlatable
      // with what we sent (matches the format used for sid=0xE0).
      uint32_t magic = 0;
      if (pbLen >= 4 && pb[2] == 0x10) {
        magic = peekVarint(pb, pbLen, 3);
      }

      auto cmdName = [](uint32_t c) -> const char* {
        switch (c) {
          case G2_AI_CMD_CTRL:      return "CTRL";
          case 2:                   return "VAD_INFO";
          case G2_AI_CMD_ASK:       return "ASK";
          case G2_AI_CMD_ANALYSE:   return "ANALYSE";
          case G2_AI_CMD_REPLY:     return "REPLY";
          case 6:                   return "SKILL";
          case 7:                   return "PROMPT";
          case 8:                   return "EVENT";
          case G2_AI_CMD_HEARTBEAT: return "HEARTBEAT";
          case 10:                  return "CONFIG";
          case 12:                  return "COMM_RSP";
          default:                  return "?";
        }
      };
      auto statusName = [](uint32_t s) -> const char* {
        switch (s) {
          case G2_AI_STATUS_WAKE_UP: return "WAKE_UP";
          case G2_AI_STATUS_ENTER:   return "ENTER";
          case G2_AI_STATUS_EXIT:    return "EXIT";
          default:                   return "?";
        }
      };
      auto eventName = [](uint32_t e) -> const char* {
        switch (e) {
          case 0: return "NONE";
          case 1: return "SCROLL";
          case 2: return "STREAM_COMPLETE";
          default: return "?";
        }
      };
      auto promptName = [](uint32_t p) -> const char* {
        switch (p) {
          case 0: return "NONE";
          case 1: return "NETWORK_ERR";
          case 2: return "BLE_DISCONNECT";
          case 3: return "SERVER_ERR";
          case 4: return "TROUBLE_UNDERSTAND";
          case 5: return "COMMAND_UNSUPPORT";
          case 6: return "AUDIO_ERROR";
          case 7: return "ASR_SERVER_ERR";
          case 8: return "AI_SERVER_ERR";
          case 9: return "COMMAND_EXE_FAIL";
          default: return "?";
        }
      };

      // CTRL: walk for `1A 02 08 <status>` (field 3 nested with field 1 varint).
      if (cmd == G2_AI_CMD_CTRL) {
        uint32_t status = 0;
        for (size_t i = 0; i + 3 < pbLen; i++) {
          if (pb[i] == 0x1A && pb[i+1] == 0x02 && pb[i+2] == 0x08) {
            status = pb[i+3];
            break;
          }
        }
        DEBUG_G2F("[G2-%c] EvenAI CTRL status=%s(%u) magic=%u (%u B)",
                  t.side, statusName(status), (unsigned)status,
                  (unsigned)magic, (unsigned)pbLen);
        break;
      }
      // EVENT: walk for `52 02 08 <type>` (field 10 nested with field 1 varint).
      if (cmd == 8 /*EVENT*/) {
        uint32_t evtype = 0;
        for (size_t i = 0; i + 3 < pbLen; i++) {
          if (pb[i] == 0x52 && pb[i+1] == 0x02 && pb[i+2] == 0x08) {
            evtype = pb[i+3];
            break;
          }
        }
        DEBUG_G2F("[G2-%c] EvenAI EVENT type=%s(%u) magic=%u (%u B)",
                  t.side, eventName(evtype), (unsigned)evtype,
                  (unsigned)magic, (unsigned)pbLen);
        break;
      }
      // PROMPT: walk for `4A 02 08 <code>` (field 9 nested with field 1 varint).
      if (cmd == 7 /*PROMPT*/) {
        uint32_t code = 0;
        for (size_t i = 0; i + 3 < pbLen; i++) {
          if (pb[i] == 0x4A && pb[i+1] == 0x02 && pb[i+2] == 0x08) {
            code = pb[i+3];
            break;
          }
        }
        DEBUG_G2F("[G2-%c] EvenAI PROMPT type=%s(%u) magic=%u (%u B)",
                  t.side, promptName(code), (unsigned)code,
                  (unsigned)magic, (unsigned)pbLen);
        break;
      }
      // ASK / ANALYSE / REPLY / HEARTBEAT / CONFIG / COMM_RSP / others —
      // log by name with magic, then dump fields for any non-trivial body.
      DEBUG_G2F("[G2-%c] EvenAI %s magic=%u (%u B)",
                t.side, cmdName(cmd), (unsigned)magic, (unsigned)pbLen);
      // Body fields (skip the cmd+magic prefix we already named).
      logPbFlat(t.side, "  body", env.payload, env.payloadLen);
      break;
    }

    case G2_SID_HEARTBEAT:
      // Async push from glasses on the heartbeat channel (distinct from
      // the EvenCore Cmd=12 ack we send out on sid=0xE0). Observed shape
      // includes `08 06 10 <varint>` bursts — likely device→host status
      // pings. Walk fields rather than dumping hex so we can correlate
      // them against UI events in labelled captures.
      logPbFlat(t.side, "sid=0x80 hb-rx", env.payload, env.payloadLen);
      break;

    default:
      DEBUG_G2F("[G2-%c] sid=0x%02X flag=0x%02X len=%u", t.side,
                env.sid, env.flag, (unsigned)env.payloadLen);
      break;
  }
}

// =============================================================================
// Write path — MTU fragmentation + write serialisation
// =============================================================================

// MTU-chunk and write the envelope. Caller MUST already hold
// `t.writeMutex`. Split out from sendEnvelope so multi-fragment senders
// can hold the mutex once across an entire burst (heartbeats and other
// writers wait until the burst finishes), preventing the
// `esp_ble_gattc_write_char rc=-1` queue-saturation cascade observed
// when the heartbeat task slipped between fragments of a 16-frag image
// push and overflowed the BLE controller's pending-write queue.
//
// Returns true if every MTU-chunk was accepted by the GATT client.
static bool sendEnvelopeNoMutex(G2Temple& t, const uint8_t* data, size_t len) {
  // Decode the 8-byte envelope header for logging:
  //   [AA 21][seq][len][totFrags][fragIdx][sid][flag]
  if (len >= G2_ENVELOPE_HDR_LEN) {
    DEBUG_G2F("[G2-%c] TX env total=%u seq=0x%02X len=%u %u/%u sid=0x%02X flag=0x%02X",
              t.side, (unsigned)len, data[2], data[3], data[5], data[4],
              data[6], data[7]);
    // Stats hook — payload length here is `data[3]` (envelope's len byte
    // includes the 2-byte CRC for the last fragment, but for our single-
    // fragment use this is close enough to the pb body length).
    const size_t pbApprox = (data[3] > G2_ENVELOPE_CRC_LEN)
                            ? (size_t)(data[3] - G2_ENVELOPE_CRC_LEN) : 0;
    g2statsRecordTx(data[6], data[7], pbApprox);
  }

  // mtu-3 for ATT header. If MTU didn't negotiate for any reason, fall back
  // to the spec minimum (23 → 20 bytes payload).
  const size_t mtu = (t.mtu > 23) ? t.mtu : 23;
  const size_t chunk = mtu - 3;

  // The firmware uses a single per-link reassembly buffer: it sees the
  // AA 21 preamble, starts accumulating, and dispatches once the declared
  // length (bytes 2..3) is reached. Continuation fragments are raw payload
  // bytes with no per-fragment framing — BLE GATT guarantees ordering within
  // a characteristic, so concatenating is sufficient.
  bool ok = true;
  size_t off = 0;
  while (ok && off < len) {
    size_t take = len - off;
    if (take > chunk) take = chunk;
    bool wrote = t.writeChar->writeValue(const_cast<uint8_t*>(data + off),
                                         take, false);
    if (!wrote) {
      // `esp_ble_gattc_write_char rc=-1` — observed transient during long
      // image bursts (Q11 / Q10 / QGlizzy / g2bmp), roughly once per ~100
      // BLE writes. The BT controller's TX queue momentarily refuses; a
      // brief pause lets it drain, and the same write succeeds on a
      // single retry. Without this, every multi-fragment image push
      // becomes a coin flip and a failed write strands the GATT mutex,
      // cascading into shutdown / picker-rebuild timeouts and a firmware
      // teardown. One retry with 50 ms backoff catches ~all of these.
      DEBUG_G2F("[G2-%c] writeValue transient fail at offset %u/%u — "
                "retrying after 50 ms", t.side, (unsigned)off, (unsigned)len);
      vTaskDelay(pdMS_TO_TICKS(50));
      wrote = t.writeChar->writeValue(const_cast<uint8_t*>(data + off),
                                      take, false);
      if (wrote) {
        DEBUG_G2F("[G2-%c] writeValue retry OK at offset %u/%u",
                  t.side, (unsigned)off, (unsigned)len);
      }
    }
    ok = wrote;
    off += take;
    // Yield 1 tick (~10 ms) between fragments. The BT controller's
    // flow-control bookkeeping can desync if we push fragments back-to-back
    // — observed as a LoadProhibited deep in btsnd_hcic_host_num_xmitted_pkts
    // when the controller's "completed packets" event arrived mid-write.
    // 1 tick is invisible to the user (~10 ms total for a 3-fragment send)
    // but gives the controller the breathing room it apparently needs.
    if (ok && off < len) vTaskDelay(1);
  }

  if (ok) {
    t.packetsSent++;
    g2RingRecord(t.side, 'T', data, len);
  } else {
    DEBUG_G2F("[G2-%c] writeValue failed at offset %u of %u",
              t.side, (unsigned)off, (unsigned)len);
  }
  return ok;
}

// Public single-envelope send. Acquires `t.writeMutex`, calls the
// no-mutex variant, releases. Used by every code path that ships a
// single envelope (heartbeats, single-frag CREATE, settings query, …).
// Multi-fragment senders should call sendEnvelopeNoMutex directly with
// the mutex held across the whole burst — see sendPbFragmented.
static bool sendEnvelope(G2Temple& t, const uint8_t* data, size_t len) {
  if (!t.connected || !t.writeChar) {
    DEBUG_G2F("[G2-%c] sendEnvelope: not ready (connected=%d writeChar=%p)",
              t.side, t.connected ? 1 : 0, t.writeChar);
    return false;
  }
  if (!t.writeMutex) return false;
  if (xSemaphoreTake(t.writeMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    DEBUG_G2F("[G2-%c] Write mutex timeout; envelope dropped", t.side);
    return false;
  }
  bool ok = sendEnvelopeNoMutex(t, data, len);
  xSemaphoreGive(t.writeMutex);
  return ok;
}

static bool sendToBoth(const uint8_t* data, size_t len) {
  bool any = false;
  if (gL.connected) any |= sendEnvelope(gL, data, len);
  if (gR.connected) any |= sendEnvelope(gR, data, len);
  return any;
}

// Send a pb body of arbitrary length as a sequence of envelope fragments.
// Matches the reference's framePb (g2-kit-unofficial/ble/envelope.ts):
//   * One CRC computed over the FULL pb body, appended LE only to the last
//     fragment.
//   * All fragments share the same `seq` byte — firmware reassembles by
//     seq, not arrival order. Distinct envelopes for distinct messages
//     must use distinct seqs.
//   * `len` per fragment = chunk bytes (mid frags) or chunk + 2 CRC (last).
//     The u8 ceiling on `len` is the reason this multi-fragment path
//     exists at all — single-fragment maxes out at 253 pb bytes.
//   * `totFrags` and `fragIdx` (1-based) are populated identically across
//     each fragment of the message group.
//
// Mutex policy: hold writeMutex for the WHOLE burst. Earlier we acquired
// per-fragment, releasing between each, on the theory that heartbeats
// or unrelated sends could safely interleave (firmware groups by seq, so
// reassembly is unaffected by interleave). In practice that opened a
// failure window: a heartbeat tick coinciding with a long burst (e.g.
// a 16-fragment 3500 B image push) acquired the mutex between our
// fragments, fired writeValue, and got `esp_ble_gattc_write_char rc=-1`
// because the BLE controller's pending-write queue was already saturated
// by our in-flight chunks. From there every subsequent acquire timed out
// for ~30 s ("Write mutex timeout; envelope dropped" cascade) until the
// firmware gave up on the half-complete reassembly and the controller
// drained, by which time we'd lost the hijack.
//
// Holding the mutex across the burst means heartbeats wait at the
// xSemaphoreTake site (their 500 ms timeout drops the beat — fine, the
// firmware tolerates ≥10 s of beat silence before tearing down, and our
// longest burst case of ~1 s for a full image tile is well under that).
// The 20 ms inter-fragment delay was already empirically tuned for the
// single-writer case (reference's TypeScript event-loop pacing), so
// serialising all writers behind the burst's pacing is correct without
// further tuning.
static bool sendPbFragmented(G2Temple& arm, uint8_t seq, uint8_t sid, uint8_t flag,
                             const uint8_t* pb, size_t pbLen) {
  if (!pb || pbLen == 0) return false;
  if (!arm.connected || !arm.writeChar || !arm.writeMutex) {
    DEBUG_G2F("[G2-%c] sendPbFragmented: arm not ready", arm.side);
    return false;
  }

  const uint16_t crc = g2CrcCcittFalse(pb, pbLen);
  const size_t   chunkSize    = G2_FRAG_CHUNK_PB;
  const size_t   totalWithCrc = pbLen + G2_ENVELOPE_CRC_LEN;
  uint8_t totFrags = (uint8_t)((totalWithCrc + chunkSize - 1) / chunkSize);
  if (totFrags == 0) totFrags = 1;

  // Take mutex ONCE for the whole burst. 2 s timeout covers the worst
  // case where a previous burst is finishing (≤1 s) plus some headroom;
  // anything beyond that is a stuck-state we can't safely paper over.
  if (xSemaphoreTake(arm.writeMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    DEBUG_G2F("[G2-%c] sendPbFragmented: mutex timeout (burst aborted)",
              arm.side);
    return false;
  }

  // Per-frame stack buffer: header (8) + chunk (≤232) + CRC (2 on last).
  // Keep on stack — comfortably under the 4 KB worker stack and saves a
  // heap alloc per fragment.
  uint8_t frame[G2_ENVELOPE_HDR_LEN + G2_FRAG_CHUNK_PB + G2_ENVELOPE_CRC_LEN];

  bool ok = true;
  size_t off = 0;
  for (uint8_t i = 0; i < totFrags && ok; i++) {
    const bool isLast = (i + 1 == totFrags);
    size_t chunkLen;       // bytes after the 8-byte header (pb chunk + maybe CRC)
    size_t pbChunk;        // pb bytes copied this fragment

    if (isLast) {
      pbChunk  = pbLen - off;
      chunkLen = pbChunk + G2_ENVELOPE_CRC_LEN;
    } else {
      pbChunk  = chunkSize;
      chunkLen = chunkSize;
    }

    frame[0] = G2_PREAMBLE_0;
    frame[1] = G2_PREAMBLE_TX;
    frame[2] = seq;
    frame[3] = (uint8_t)chunkLen;
    frame[4] = totFrags;
    frame[5] = (uint8_t)(i + 1);   // 1-based index per the reference
    frame[6] = sid;
    frame[7] = flag;
    memcpy(frame + G2_ENVELOPE_HDR_LEN, pb + off, pbChunk);
    if (isLast) {
      frame[G2_ENVELOPE_HDR_LEN + pbChunk]     = (uint8_t)(crc & 0xFF);
      frame[G2_ENVELOPE_HDR_LEN + pbChunk + 1] = (uint8_t)((crc >> 8) & 0xFF);
    }
    off += pbChunk;

    if (!sendEnvelopeNoMutex(arm, frame, G2_ENVELOPE_HDR_LEN + chunkLen)) {
      DEBUG_G2F("[G2-%c] sendPbFragmented: write failed at frag %u/%u",
                arm.side, (unsigned)(i + 1), (unsigned)totFrags);
      ok = false;
      break;
    }

    // Inter-fragment breathing room. Empirically, sending the 5 fragments
    // of a 935 B Settings CREATE within ~5 ms got radio silence from the
    // firmware (no CreateResp at all, 1500 ms timeout) — same fragments,
    // same CRC, same seq as a working single-fragment CREATE, just back
    // to back. The reference is TypeScript-paced (event loop turn between
    // each writeValue) so its callers never see this; our tight C loop
    // does. 20 ms per gap gives the firmware's reassembler time to copy
    // each fragment into its slot and arm for the next one without
    // measurable user-visible latency (a 14-frag Settings push pays
    // ≤280 ms total). Keep this skip on the last frag — no point delaying
    // after the message is complete.
    if (i + 1 < totFrags) vTaskDelay(pdMS_TO_TICKS(20));
  }
  xSemaphoreGive(arm.writeMutex);
  if (!ok) return false;
  return true;
}

// =============================================================================
// Heartbeat
// =============================================================================

// Consecutive unacked heartbeats that imply the plugin task is dead. At 5 s
// period that's ~15 s of silence — plenty for transient BLE hiccups to
// resolve, short enough that we're not spamming a dead pipe for a minute.
static constexpr uint32_t HEARTBEAT_DEAD_THRESHOLD = 3;

// Why this runs on a dedicated task, not the timer callback:
//
//   Arduino BLE's writeValue() goes deep into the ESP-IDF BT host stack —
//   many nested frames, plus a 500 ms mutex wait on our writeMutex. That's
//   unsafe on the FreeRTOS `Tmr Svc` daemon task (shared, small stack
//   ~4 KB). When the plugin stopped acking heartbeats the BLE write path
//   got slower / deeper and blew the Tmr Svc stack, hard-crashing the
//   device (observed 2026-04-24, heartbeat #6 then heartbeat #1 after a
//   hijack failure made the plugin silent from connect).
//
//   Fix: the timer callback does nothing but give a binary semaphore. A
//   dedicated `g2_hb_worker` task waits on the semaphore, does the
//   heartbeat TX with its own ~4 KB stack (isolated from system timers),
//   and loops. Same 5 s cadence, same logic, safer stack.

static SemaphoreHandle_t gBeatSem        = nullptr;
static TaskHandle_t      gBeatTaskHandle = nullptr;
static volatile bool     gBeatTaskStop   = false;

// Helper — beat one temple, bump its miss counter, and flag the plugin
// "silent" if the threshold is crossed. The miss counter is cleared on
// every HeartbeatAck or any other incoming frame (see handleEnvelope).
// Runs on gBeatTaskHandle (NOT on Tmr Svc — see startHeartbeatTimer
// comment for the history).
//
// Keeps pinging even after we've marked the plugin silent, because the
// most common cause is "glasses aren't being worn, plugin task is
// dormant." When the user puts the glasses on, the plugin wakes up and
// starts acking again — we need the heartbeat stream to stay live so it
// has something to ack against. The previous "stop sending when dead"
// logic made recovery require a full reconnect, which was bad UX.
static void beatOne(G2Temple& t) {
  if (!t.connected) return;

  uint8_t buf[64];
  uint8_t seq = allocSeq();
  size_t n = g2BuildHeartbeat(seq, G2_MAGIC_HEARTBEAT,
                              ++t.heartbeatCounter, buf, sizeof(buf));
  DEBUG_G2F("[G2-%c] Heartbeat #%lu seq=0x%02X (%u bytes)",
            t.side, (unsigned long)t.heartbeatCounter, seq, (unsigned)n);
  if (n) sendEnvelope(t, buf, n);

  // Saturating pre-increment. The ack handler resets to 0 on receipt. If
  // N sends go unacked, the plugin has stopped servicing sid=0xE0 — stop
  // sending further render commands from g2ShowText et al. until
  // reconnect. Cap at THRESHOLD+1 so the transition condition fires
  // exactly once.
  if (t.heartbeatMissed < HEARTBEAT_DEAD_THRESHOLD + 1) t.heartbeatMissed++;
  if (t.heartbeatMissed == HEARTBEAT_DEAD_THRESHOLD && !t.pluginDead) {
    t.pluginDead = true;
    t.containerReady = false;
    // "Silent" not "dead" — most common cause is the glasses aren't being
    // worn, so the firmware's plugin task is dormant. We keep pinging; the
    // moment anything lands on our RX pipe (wear detect, tap, a resumed
    // heartbeat ack) we flip back to alive without needing a reconnect.
    BROADCAST_PRINTF("[G2] %s temple plugin silent — %u heartbeats unacked "
                     "(likely idle / not worn). Still pinging; will recover "
                     "automatically when firmware responds.",
                     t.side == 'L' ? "LEFT" : "RIGHT",
                     (unsigned)t.heartbeatMissed);
    g2RingDump(t.side == 'L' ? "plugin silent (L)" : "plugin silent (R)");
    g2PushStatusEvent(t.side == 'L' ? "plugin-silent-L" : "plugin-silent-R");
  }
}

static void heartbeatWorkerTask(void* /*arg*/) {
  while (!gBeatTaskStop) {
    // Block until the 5 s timer kicks us (or stopHeartbeatTimer gives a
    // final "wake and exit" signal). pdMS_TO_TICKS(6000) upper bound
    // guards against a missed tick; the gBeatTaskStop check catches clean
    // shutdown even if the sem never fires.
    if (xSemaphoreTake(gBeatSem, pdMS_TO_TICKS(6000)) == pdTRUE) {
      if (gBeatTaskStop) break;
      beatOne(gL);
      beatOne(gR);
      if (gHijackActive && (millis() - gHijackStartedMs) > HIJACK_SAFETY_MS) {
        sendHijackShutdown("safety-timeout");
      }
    }
  }
  gBeatTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static void heartbeatTimerCallback(TimerHandle_t /*xTimer*/) {
  // Trivial: just kick the worker. This runs on Tmr Svc so must stay
  // small — no BLE writes, no printfs, no mutex waits.
  if (gBeatSem) xSemaphoreGive(gBeatSem);
}

static void startHeartbeatTimer() {
  if (gHeartbeatTimer) return;
  if (!gBeatSem) gBeatSem = xSemaphoreCreateBinary();
  if (!gBeatSem) {
    DEBUG_G2F("[G2] heartbeat: sem alloc failed — timer NOT started");
    return;
  }
  gBeatTaskStop = false;
  if (!gBeatTaskHandle) {
    BaseType_t rc = xTaskCreate(heartbeatWorkerTask, "g2_hb_worker",
                                /*stack*/ 4096, nullptr,
                                /*prio*/ 5, &gBeatTaskHandle);
    if (rc != pdPASS) {
      DEBUG_G2F("[G2] heartbeat: worker task create failed");
      gBeatTaskHandle = nullptr;
      return;
    }
  }
  gHeartbeatTimer = xTimerCreate("g2_hb", pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS),
                                 pdTRUE, nullptr, heartbeatTimerCallback);
  if (gHeartbeatTimer) xTimerStart(gHeartbeatTimer, 0);
}

static void stopHeartbeatTimer() {
  if (gHeartbeatTimer) {
    xTimerStop(gHeartbeatTimer, 0);
    xTimerDelete(gHeartbeatTimer, 0);
    gHeartbeatTimer = nullptr;
  }
  // Signal the worker to exit, then wake it so it observes the flag.
  if (gBeatTaskHandle) {
    gBeatTaskStop = true;
    if (gBeatSem) xSemaphoreGive(gBeatSem);
    // The task self-deletes; don't join here (calling task might be the
    // BLE notify thread). Brief yield is enough.
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  // Leave gBeatSem allocated — cheap to keep, safe to reuse on next start.
}

// =============================================================================
// Session prelude (AppLaunch + CREATE startup text)
// =============================================================================

static bool runSessionPrelude(G2Temple& t) {
  // The reference sends ONLY the fixed PRELUDE_F5872 byte literal here — no
  // subsequent CREATE. The plugin task gets primed later when we send our
  // first REBUILD (e.g. from g2ShowText). Sending a CREATE at prelude time
  // was my earlier mistake and is what appears to have rebooted the glasses.
  uint8_t buf[64];
  size_t n = g2BuildAppLaunch(buf, sizeof(buf));
  DEBUG_G2F("[G2-%c] Prelude: AppLaunch (%u bytes, fixed literal)", t.side, (unsigned)n);
  if (n == 0 || !sendEnvelope(t, buf, n)) {
    DEBUG_G2F("[G2-%c] Prelude: AppLaunch send failed", t.side);
    return false;
  }
  DEBUG_G2F("[G2-%c] AppLaunch sent; settling 800ms", t.side);
  // Reference settles 800ms after both arms connect before the first
  // EvenCore command — mirror that here per temple.
  vTaskDelay(pdMS_TO_TICKS(800));
  DEBUG_G2F("[G2-%c] Prelude complete", t.side);
  return true;
}

// =============================================================================
// Per-temple lifecycle
// =============================================================================

static void templeInit(G2Temple& t, char side) {
  memset(&t, 0, sizeof(t));
  t.side = side;
  t.rxCap = RX_ASSEMBLY_CAP;
  t.rxBuf = (uint8_t*)ps_alloc(t.rxCap, AllocPref::PreferPSRAM, "g2.rxbuf");
  t.writeMutex = xSemaphoreCreateMutex();
  t.mtu = 23;  // default until negotiated
}

static void templeReset(G2Temple& t) {
  if (t.rxBuf) { free(t.rxBuf); t.rxBuf = nullptr; }
  if (t.writeMutex) { vSemaphoreDelete(t.writeMutex); t.writeMutex = nullptr; }
  if (t.advertisedDevice) { delete t.advertisedDevice; t.advertisedDevice = nullptr; }
  if (t.client) {
    if (t.client->isConnected()) t.client->disconnect();
    // Arduino BLE's BLEClient is owned by the library. Null our pointer and
    // let the next createClient() build a fresh one if needed.
    t.client = nullptr;
  }
  t.writeChar = nullptr;
  t.notifyChar = nullptr;
  t.connected = false;
  t.containerReady = false;
  // deinit wipes both temples, which invariably drops hijack state too.
  gHijackActive = false;
}

static bool connectTemple(G2Temple& t) {
  if (!t.advertisedDevice) {
    DEBUG_G2F("[G2-%c] connectTemple: no advertisedDevice cached", t.side);
    return false;
  }
  if (t.connected) {
    DEBUG_G2F("[G2-%c] connectTemple: already connected", t.side);
    return true;
  }

  DEBUG_G2F("[G2-%c] Connecting to %s @ %s (heap=%u)",
            t.side, t.deviceName.c_str(), t.deviceAddress.c_str(),
            (unsigned)ESP.getFreeHeap());
  // Tear down any stale client from a previous unexpected drop. Arduino BLE
  // doesn't reliably reuse a client whose peer disappeared mid-session.
  if (t.client && t.clientStale) {
    DEBUG_G2F("[G2-%c] Replacing stale BLEClient from prior drop", t.side);
    // Arduino BLE's BLEClient is owned by the library; simply null our
    // pointer and let createClient allocate a fresh one. Any residual
    // object leaks at the library level — acceptable trade for reliability.
    t.client = nullptr;
    t.clientStale = false;
  }
  if (!t.client) {
    DEBUG_G2F("[G2-%c] Creating new BLEClient", t.side);
    t.client = BLEDevice::createClient();
    if (!t.client) {
      DEBUG_G2F("[G2-%c] BLEDevice::createClient() returned null", t.side);
      return false;
    }
    t.client->setClientCallbacks(new TempleClientCallbacks(&t));
  }
  uint32_t tConnStart = millis();
  if (!t.client->connect(t.advertisedDevice)) {
    DEBUG_G2F("[G2-%c] BLE connect failed after %u ms",
              t.side, (unsigned)(millis() - tConnStart));
    return false;
  }
  DEBUG_G2F("[G2-%c] BLE connect OK in %u ms",
            t.side, (unsigned)(millis() - tConnStart));

  t.client->setMTU(MTU_TARGET);
  t.mtu = t.client->getMTU();
  DEBUG_G2F("[G2-%c] Requested MTU %u, got %u",
            t.side, (unsigned)MTU_TARGET, (unsigned)t.mtu);

  // Request a tight connection interval — equivalent to Android's
  // BluetoothGatt.CONNECTION_PRIORITY_HIGH that faceclaw uses.
  // ESP-IDF's default GAP connection negotiates ~30–50 ms; bringing
  // it down to ~11.25–15 ms triples the connection events available
  // for TX, materially boosting throughput during multi-fragment
  // image bursts (Q6/Q11/QGlizzy/g2bmp). The peer can refuse and
  // counter-offer; the actual negotiated values land in the
  // ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT we already log.
  //
  // Units: 1.25 ms per interval-tick, 10 ms per timeout-tick.
  esp_ble_conn_update_params_t hi = {};
  esp_bd_addr_t peer;
  memcpy(peer, t.client->getPeerAddress().getNative(), sizeof(esp_bd_addr_t));
  memcpy(hi.bda, peer, sizeof(esp_bd_addr_t));
  hi.min_int = 9;     //  9 × 1.25 ms = 11.25 ms
  hi.max_int = 12;    // 12 × 1.25 ms = 15 ms
  hi.latency = 0;
  hi.timeout = 500;   // 500 × 10 ms = 5 s
  esp_err_t ce = esp_ble_gap_update_conn_params(&hi);
  DEBUG_G2F("[G2-%c] Requesting HIGH conn priority "
            "(min=11.25ms max=15ms latency=0 timeout=5s) → err=%d",
            t.side, (int)ce);

  DEBUG_G2F("[G2-%c] Looking up service %s", t.side, SERVICE_UUID);
  BLERemoteService* svc = t.client->getService(BLEUUID(SERVICE_UUID));
  if (!svc) {
    DEBUG_G2F("[G2-%c] Service not found on peer (listing all services below)", t.side);
    auto* services = t.client->getServices();
    if (services) {
      for (const auto& entry : *services) {
        DEBUG_G2F("[G2-%c]   svc: %s", t.side, entry.first.c_str());
      }
    }
    t.client->disconnect();
    return false;
  }
  DEBUG_G2F("[G2-%c] Service found, looking up characteristics", t.side);
  t.writeChar  = svc->getCharacteristic(BLEUUID(CHAR_WRITE_UUID));
  t.notifyChar = svc->getCharacteristic(BLEUUID(CHAR_NOTIFY_UUID));
  DEBUG_G2F("[G2-%c] writeChar=%p notifyChar=%p", t.side, t.writeChar, t.notifyChar);
  if (!t.writeChar || !t.notifyChar) {
    DEBUG_G2F("[G2-%c] Characteristic lookup failed (listing all chars):", t.side);
    auto* chars = svc->getCharacteristics();
    if (chars) {
      for (const auto& entry : *chars) {
        DEBUG_G2F("[G2-%c]   char: %s", t.side, entry.first.c_str());
      }
    }
    t.client->disconnect();
    return false;
  }
  DEBUG_G2F("[G2-%c] Characteristics: writeChar.canWrite=%d canWriteNR=%d, "
            "notifyChar.canNotify=%d canIndicate=%d",
            t.side,
            t.writeChar->canWrite() ? 1 : 0,
            t.writeChar->canWriteNoResponse() ? 1 : 0,
            t.notifyChar->canNotify() ? 1 : 0,
            t.notifyChar->canIndicate() ? 1 : 0);
  if (t.notifyChar->canNotify()) {
    DEBUG_G2F("[G2-%c] Subscribing to notifications", t.side);
    t.notifyChar->registerForNotify((t.side == 'L') ? notifyThunkL : notifyThunkR);
  } else {
    DEBUG_G2F("[G2-%c] WARN: notifyChar cannot notify — will not receive responses", t.side);
  }

  // One-shot enumeration of undocumented services. Logs the char list +
  // properties for visibility into firmware revisions that might add
  // new behaviour here. No runtime subscriptions — see
  // enumerateDiagService comment.
  enumerateDiagService(t, DIAG_SVC_6450, "6450");
  enumerateDiagService(t, DIAG_SVC_7450, "7450");
  enumerateDiagService(t, DIAG_SVC_1001, "1001");

  t.connected = true;
  t.heartbeatCounter = 0;
  t.heartbeatMissed  = 0;
  t.pluginDead       = false;

  DEBUG_G2F("[G2-%c] Running session prelude (AppLaunch)", t.side);
  if (!runSessionPrelude(t)) {
    DEBUG_G2F("[G2-%c] Prelude failed; disconnecting", t.side);
    t.client->disconnect();
    t.connected = false;
    return false;
  }

  DEBUG_G2F("[G2-%c] Ready (heap=%u)", t.side, (unsigned)ESP.getFreeHeap());
  return true;
}

static void disconnectTemple(G2Temple& t) {
  if (!t.connected) return;
  // Best-effort clean shutdown so the plugin task tears down the page.
  uint8_t buf[32];
  uint8_t seq = allocSeq();
  size_t n = g2BuildShutdown(seq, G2_MAGIC_SHUTDOWN, 0, buf, sizeof(buf));
  if (n) sendEnvelope(t, buf, n);
  if (t.client && t.client->isConnected()) t.client->disconnect();
  t.connected = false;
  t.containerReady = false;
  // Explicit-disconnect path clears the hijack regardless of which arm —
  // either way, the right-side container we rely on is gone.
  if (t.side == 'R') gHijackActive = false;
}

// =============================================================================
// Public API
// =============================================================================

bool initG2Client() {
  if (gG2State && gG2State->initialized) return true;

  DEBUG_G2F("[G2] Initializing client mode");
  broadcastOutput("[G2] Initializing client mode");

  // BLE server mode (phone profile) uses the same controller and won't
  // coexist with client scanning. Tear it down if it's running.
  if (isBLERunning()) {
    DEBUG_G2F("[G2] Stopping BLE server mode first");
    broadcastOutput("[G2] Stopping BLE server");
    deinitBluetooth();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (btStarted()) {
    btStop();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (!btStart()) {
    broadcastOutput("[G2] BT controller start failed");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(100));

  gG2State = (G2ClientState*)calloc(1, sizeof(G2ClientState));
  if (!gG2State) {
    broadcastOutput("[G2] State alloc failed");
    return false;
  }

  BLEDevice::init("HardwareOne");
  BLEDevice::setMTU(MTU_TARGET);
  gScan = BLEDevice::getScan();
  if (!gScan) {
    free(gG2State); gG2State = nullptr;
    broadcastOutput("[G2] Scan init failed");
    return false;
  }
  gScan->setActiveScan(true);
  gScan->setInterval(100);
  gScan->setWindow(99);

  templeInit(gL, 'L');
  templeInit(gR, 'R');

  if (!gCreateAckSem) {
    gCreateAckSem = xSemaphoreCreateBinary();
  }
  if (!gRebuildAckSem) {
    gRebuildAckSem = xSemaphoreCreateBinary();
  }
  if (!gImgPushAckSem) {
    gImgPushAckSem = xSemaphoreCreateBinary();
  }

  gG2State->initialized = true;
  gG2State->state = G2_STATE_IDLE;

  // Register with the BLE peer registry. The spec/ops pair is file-static
  // (see g2PeerSpec / g2PeerOps below) so registration is just publishing
  // a stable pointer. Idempotent on re-init.
  bleRegisterPeer(g2PeerSpec);

  // Eagerly init the ring module so its peer also registers now. Ring
  // shares the BLE central stack we just brought up; its init is just a
  // mutex + spec-publish, no BLE work. Without this call, the ring peer
  // would only register on first ringconnect(), making it invisible to
  // bleBootReconnect at boot.
  g2RingInit();

  // Register the built-in G2 page modules. The order here is the order
  // they appear in the hijack menu and the order they get CLI commands.
  // Adding a new page is a single g2RegisterPage() call here (or in the
  // owning module's init function — the registry is global).
  registerG2Pages();

  DEBUG_G2F("[G2] Client initialized");
  return true;
}

void deinitG2Client() {
  if (!gG2State) return;
  g2Disconnect();
  stopHeartbeatTimer();
  templeReset(gL);
  templeReset(gR);
  free(gG2State);
  gG2State = nullptr;
  gScan = nullptr;
  DEBUG_G2F("[G2] Client deinitialized");
}

bool isG2ClientInitialized() {
  return gG2State && gG2State->initialized;
}

// Synchronous connect — scans, connects both requested temples, runs prelude.
// May block for up to SCAN_DURATION_MS + per-temple connect time. Do NOT call
// from the command-handler task; use g2ConnectAsync() for CLI paths.
static bool g2ConnectSync(G2Eye eye) {
  if (!gG2State && !initG2Client()) return false;
  gConnectTarget = eye;
  gG2State->state = G2_STATE_SCANNING;
  g2PushStatusEvent("scan-start");

  // Clear any stale scan residue.
  if (gL.advertisedDevice) { delete gL.advertisedDevice; gL.advertisedDevice = nullptr; }
  if (gR.advertisedDevice) { delete gR.advertisedDevice; gR.advertisedDevice = nullptr; }
  gScanFoundL = false;
  gScanFoundR = false;

  DEBUG_G2F("[G2] Scanning for %s temple(s)",
            eye == G2_EYE_LEFT ? "LEFT" :
            eye == G2_EYE_RIGHT ? "RIGHT" : "BOTH");
  broadcastOutput("[G2] Scanning...");

  gScan->setAdvertisedDeviceCallbacks(new G2ScanCallbacks(), true);
  // Arduino BLE has two `start()` overloads:
  //   start(duration, is_continue)                  → BLOCKING, returns results
  //   start(duration, completeCb, is_continue)      → non-blocking, fires cb
  // Use the non-blocking form so this call doesn't hog the command handler.
  const uint32_t scanSec = (SCAN_DURATION_MS + 999) / 1000;
  gScan->start(scanSec, [](BLEScanResults) {
    // No-op — we react to individual results in the advertised-device
    // callback. This just exists so start() takes the non-blocking path.
  }, false);

  // Poll for a found-both condition, up to the scan window + a small cushion.
  const uint32_t deadline = millis() + (scanSec * 1000) + 1000;
  while (millis() < deadline) {
    bool needL = (eye != G2_EYE_RIGHT);
    bool needR = (eye != G2_EYE_LEFT);
    if ((!needL || gScanFoundL) && (!needR || gScanFoundR)) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  gScan->stop();

  if (gConnectCancel) {
    DEBUG_G2F("[G2] Connect cancelled after scan");
    gG2State->state = G2_STATE_IDLE;
    return false;
  }

  bool needL = (eye != G2_EYE_RIGHT);
  bool needR = (eye != G2_EYE_LEFT);
  DEBUG_G2F("[G2] Scan complete: foundL=%d foundR=%d needL=%d needR=%d",
            gScanFoundL ? 1 : 0, gScanFoundR ? 1 : 0,
            needL ? 1 : 0, needR ? 1 : 0);
  bool haveAny = (needL && gScanFoundL) || (needR && gScanFoundR);
  if (!haveAny) {
    gG2State->state = G2_STATE_IDLE;
    DEBUG_G2F("[G2] No matching temples found in scan window");
    broadcastOutput("[G2] No glasses found");
    return false;
  }

  gG2State->state = G2_STATE_CONNECTING;
  bool ok = false;
  if (needL && gScanFoundL && !gConnectCancel) {
    DEBUG_G2F("[G2] Starting LEFT temple connect");
    bool lOk = connectTemple(gL);
    DEBUG_G2F("[G2] LEFT temple connect result: %s", lOk ? "OK" : "FAIL");
    ok |= lOk;
  }
  if (needR && gScanFoundR && !gConnectCancel) {
    DEBUG_G2F("[G2] Starting RIGHT temple connect");
    bool rOk = connectTemple(gR);
    DEBUG_G2F("[G2] RIGHT temple connect result: %s", rOk ? "OK" : "FAIL");
    ok |= rOk;
  }

  if (gConnectCancel) {
    DEBUG_G2F("[G2] Connect cancelled during temple connects");
    gG2State->state = G2_STATE_IDLE;
    return false;
  }

  if (!ok) {
    gG2State->state = G2_STATE_IDLE;
    broadcastOutput("[G2] Connect failed");
    return false;
  }

  gG2State->state = G2_STATE_CONNECTED;
  gG2State->connectedSince = millis();
  startHeartbeatTimer();
  broadcastOutput("[G2] Connected");
  g2PushStatusEvent("connect-ok");

  // Persist temple MACs for boot-time auto-reconnect. bleSavePeerMac is
  // a no-op when the values match what's already stored, so calling on
  // every successful connect is cheap. Auto-reconnect is gated separately
  // by the peer's autoConnect flag (gBlePeerData[BLE_PEER_G2_GLASSES]) —
  // saving the MAC here is just bookkeeping.
  bleSavePeerMac(BLE_PEER_G2_GLASSES,
                 gL.connected ? gL.deviceAddress : String(),
                 gR.connected ? gR.deviceAddress : String());
  return true;
}

// Background task body — runs the sync connect and terminates.
static void g2ConnectTaskBody(void* param) {
  G2Eye eye = (G2Eye)(intptr_t)param;
  g2ConnectSync(eye);
  gConnectTaskActive = false;
  gConnectTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// Public API: non-blocking connect. Returns immediately after starting the
// background task. Use g2status to observe progress (scanning → connecting
// → connected | idle).
bool g2Connect(G2Eye eye) {
  if (gConnectTaskActive) {
    DEBUG_G2F("[G2] Connect already in progress");
    return false;
  }
  if (!gG2State && !initG2Client()) return false;
  gConnectCancel = false;
  gConnectTaskActive = true;
  BaseType_t ok = xTaskCreate(g2ConnectTaskBody, "g2_connect",
                              6144, (void*)(intptr_t)eye,
                              /*priority*/ 5, &gConnectTaskHandle);
  if (ok != pdPASS) {
    gConnectTaskActive = false;
    gConnectTaskHandle = nullptr;
    return false;
  }
  return true;
}

// Saved-MAC reconnect path. Reads gSettings.bleGlasses{Left,Right}MAC and
// runs the same scan-then-connect pipeline as g2ConnectSync, but with the
// MAC filter installed so we'll only latch onto adverts that match one of
// the saved addresses. Used by the boot-time auto-reconnect and any future
// "reconnect now" UI affordance. Returns true if at least one temple came
// up. Caller owns task management — this is the sync body, not a wrapper.
static bool g2ConnectSavedSync() {
  String macL = gBlePeerData[BLE_PEER_G2_GLASSES].mac1;
  String macR = gBlePeerData[BLE_PEER_G2_GLASSES].mac2;
  macL.trim();
  macR.trim();
  if (macL.length() == 0 && macR.length() == 0) {
    DEBUG_G2F("[G2] Auto-reconnect: no saved MACs — skipping");
    return false;
  }

  // Pick the eye target based on which MACs are saved. If only one side
  // is saved we still connect to that one — better than nothing.
  G2Eye eye = G2_EYE_AUTO;
  if (macL.length() > 0 && macR.length() == 0) eye = G2_EYE_LEFT;
  else if (macR.length() > 0 && macL.length() == 0) eye = G2_EYE_RIGHT;

  DEBUG_G2F("[G2] Auto-reconnect: targeting L='%s' R='%s'",
            macL.length() ? macL.c_str() : "(none)",
            macR.length() ? macR.c_str() : "(none)");

  gG2FilterMacL = macL;
  gG2FilterMacR = macR;
  bool ok = g2ConnectSync(eye);
  // Always clear the filter when we're done so the next pairing-path
  // scan (`openg2 auto`) goes back to name-based matching.
  gG2FilterMacL = "";
  gG2FilterMacR = "";
  return ok;
}

// Background task wrapper for g2ConnectSavedSync — same lifecycle pattern
// as g2ConnectTaskBody.
static void g2ConnectSavedTaskBody(void* /*arg*/) {
  g2ConnectSavedSync();
  gConnectTaskActive = false;
  gConnectTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// Public API: non-blocking auto-reconnect from saved MACs. Used by the
// boot hook; safe to call from any context.
bool g2ConnectSaved() {
  if (gConnectTaskActive) {
    DEBUG_G2F("[G2] g2ConnectSaved: connect already in progress");
    return false;
  }
  if (!gG2State && !initG2Client()) return false;
  gConnectCancel = false;
  gConnectTaskActive = true;
  BaseType_t ok = xTaskCreate(g2ConnectSavedTaskBody, "g2_reconnect",
                              6144, nullptr,
                              /*priority*/ 5, &gConnectTaskHandle);
  if (ok != pdPASS) {
    gConnectTaskActive = false;
    gConnectTaskHandle = nullptr;
    return false;
  }
  return true;
}

void g2Disconnect() {
  // If a connect task is running (e.g. mid-scan or mid-service-discovery),
  // ask it to cancel. Also force-disconnect any BLE links so any blocking
  // GATT call inside the task unblocks and returns an error.
  if (gConnectTaskActive) {
    DEBUG_G2F("[G2] Cancelling in-flight connect task");
    gConnectCancel = true;
    if (gL.client && gL.client->isConnected()) gL.client->disconnect();
    if (gR.client && gR.client->isConnected()) gR.client->disconnect();
    // Give the task up to 2 s to notice and exit cleanly.
    const uint32_t deadline = millis() + 2000;
    while (gConnectTaskActive && millis() < deadline) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    // If it's still stuck inside an unbounded BLE call, nuke it. This may
    // leak a mutex or advertisedDevice copy — acceptable for an escape
    // hatch that only fires when the stack is already misbehaving.
    if (gConnectTaskActive && gConnectTaskHandle) {
      DEBUG_G2F("[G2] Connect task did not exit — force-deleting");
      vTaskDelete(gConnectTaskHandle);
      gConnectTaskHandle = nullptr;
      gConnectTaskActive = false;
    }
  }

  disconnectTemple(gL);
  disconnectTemple(gR);
  stopHeartbeatTimer();
  if (gG2State) gG2State->state = G2_STATE_IDLE;
}

bool isG2Connected() {
  return gL.connected || gR.connected;
}

G2State getG2State() {
  return gG2State ? gG2State->state : G2_STATE_IDLE;
}

const char* getG2StateString() {
  switch (getG2State()) {
    case G2_STATE_IDLE:           return "idle";
    case G2_STATE_SCANNING:       return "scanning";
    case G2_STATE_CONNECTING:     return "connecting";
    case G2_STATE_AUTHENTICATING: return "authenticating";
    case G2_STATE_CONNECTED:      return "connected";
    case G2_STATE_DISCONNECTING:  return "disconnecting";
    default:                      return "error";
  }
}

bool g2StartScan(uint32_t /*durationMs*/) { return g2Connect(G2_EYE_AUTO); }
void g2StopScan() { if (gScan) gScan->stop(); }

// Default widgetId for CREATE_STARTUP_PAGE — matches the reference's
// default. Overridden only by the Blocks hijack path (widgetId=10509) so
// the firmware recognises our CREATE as the Blocks widget it just
// announced via cmd=17 and doesn't reject it as a same-name collision.
static constexpr uint32_t G2_DEFAULT_WIDGET_ID = 10000;

// Internal: build+send CREATE_STARTUP_PAGE and block on its ack. Caller
// decides the widgetId. Returns true if the firmware acked res=0. On
// success the caller is responsible for updating `arm->containerReady`
// (we don't do it here because the hijack flow also wants to stamp
// other state atomically with the successful CREATE).
static bool sendCreateAndWait(G2Temple& arm, const char* text,
                              uint32_t widgetId) {
  if (!gCreateAckSem) {
    DEBUG_G2F("[G2] CREATE: ack sem not ready (init path broken?)");
    return false;
  }
  gExpectMagic = (uint8_t)G2_MAGIC_CREATE;
  gCreateOk    = false;
  // Drain any stale signal from a previous (timed-out) CREATE so the new
  // take below actually waits for THIS ack.
  xSemaphoreTake(gCreateAckSem, 0);

  uint8_t buf[1024];
  size_t n = g2BuildCreateStartupPage(allocSeq(), G2_MAGIC_CREATE,
                                      CONTAINER_NAME, text ? text : "",
                                      buf, sizeof(buf), widgetId);
  if (n == 0) {
    DEBUG_G2F("[G2] CREATE: build failed");
    return false;
  }
  if (!sendEnvelope(arm, buf, n)) {
    DEBUG_G2F("[G2] CREATE: send failed");
    return false;
  }
  // 1500 ms matches the reference's observed ack latency with generous
  // headroom. The ack arrives on arm's notify pipe and is posted by
  // handleEnvelope() when cmd=1 matches gExpectMagic.
  if (xSemaphoreTake(gCreateAckSem, pdMS_TO_TICKS(1500)) != pdTRUE) {
    DEBUG_G2F("[G2] CREATE timeout — container not primed (widgetId=%u)",
              (unsigned)widgetId);
    return false;
  }
  if (!gCreateOk) {
    DEBUG_G2F("[G2] CREATE rejected by firmware (widgetId=%u)",
              (unsigned)widgetId);
    return false;
  }
  return true;
}

// List-flavoured CREATE: ListContainerProperty with N items and native
// touchpad capture. Same ack-and-wait contract as sendCreateAndWait.
// Firmware draws the selection box; touchpad gestures route to
// List_ItemEvent sub-messages on sid=0x0D (currently logged as UNKNOWN
// events until we extend dispatchEventPayload to decode them).
static bool sendCreateListAndWait(G2Temple& arm,
                                  const char* const* items, size_t itemCount,
                                  uint32_t widgetId,
                                  const G2ContainerGeom& geom) {
  if (!gCreateAckSem) {
    DEBUG_G2F("[G2] CREATE-list: ack sem not ready (init path broken?)");
    return false;
  }
  gExpectMagic = (uint8_t)G2_MAGIC_CREATE;
  gCreateOk    = false;
  xSemaphoreTake(gCreateAckSem, 0);

  // Heap-allocate the pb body. 8 KB headroom comfortably handles the
  // longest list page we generate today (Settings PRETTY view at 60
  // rows × ~50 chars ≈ 3 KB pb, plus pb tag overhead). Anything larger
  // implies UX-level pagination is overdue regardless of transport.
  // The send path below fragments this body into N envelopes per the
  // wire protocol — see sendPbFragmented.
  constexpr size_t kPbCap = 8192;
  uint8_t* pb = (uint8_t*)malloc(kPbCap);
  if (!pb) {
    DEBUG_G2F("[G2] CREATE-list: malloc(%u) failed", (unsigned)kPbCap);
    return false;
  }
  size_t pbLen = g2BuildCreateListPagePb(G2_MAGIC_CREATE, CONTAINER_NAME,
                                          items, itemCount, widgetId, geom,
                                          pb, kPbCap);
  if (pbLen == 0) {
    DEBUG_G2F("[G2] CREATE-list: pb build failed (%u items)",
              (unsigned)itemCount);
    free(pb);
    return false;
  }
  bool sentOk = sendPbFragmented(arm, allocSeq(), G2_SID_EVEN_CORE,
                                  G2_FLAG_REQUEST, pb, pbLen);
  if (sentOk) {
    DEBUG_G2F("[G2] CREATE-list: %u items, pb=%u B sent",
              (unsigned)itemCount, (unsigned)pbLen);
  }
  free(pb);
  if (!sentOk) {
    DEBUG_G2F("[G2] CREATE-list: send failed");
    return false;
  }

  if (xSemaphoreTake(gCreateAckSem, pdMS_TO_TICKS(1500)) != pdTRUE) {
    DEBUG_G2F("[G2] CREATE-list timeout — container not primed (widgetId=%u)",
              (unsigned)widgetId);
    return false;
  }
  if (!gCreateOk) {
    DEBUG_G2F("[G2] CREATE-list rejected by firmware (widgetId=%u)",
              (unsigned)widgetId);
    return false;
  }
  return true;
}

// Experimental REBUILD-list path. Same envelope shape as the CREATE
// helper, but Cmd=7 REBUILD_PAGE instead of Cmd=0 CREATE_STARTUP_PAGE.
// Skips the SHUTDOWN+CREATE cycle — items get swapped in place on the
// already-CREATEd list container, hopefully preserving the firmware's
// internal scroll-position state.
//
// CAVEAT (per the existing code comment above pageSwapWorker, dated
// 2026-04-25): REBUILD-list with a different item set than the original
// CREATE has been observed to crash the firmware plugin task. Same-item-
// set REBUILD remains untested. Caller must ensure both flow control
// (gG2ListRebuildEnabled gate) and the operator's awareness of the
// risk. Returns true only if the firmware acks RebuildSuccess (res=6).
static bool sendRebuildListAndWait(G2Temple& arm,
                                   const char* const* items, size_t itemCount,
                                   const G2ContainerGeom& geom) {
  if (!gRebuildAckSem) {
    DEBUG_G2F("[G2] REBUILD-list: ack sem not ready");
    return false;
  }
  gExpectRebuildMagic = (uint8_t)G2_MAGIC_REBUILD;
  gRebuildOk          = false;
  xSemaphoreTake(gRebuildAckSem, 0);

  constexpr size_t kPbCap = 8192;
  uint8_t* pb = (uint8_t*)malloc(kPbCap);
  if (!pb) {
    DEBUG_G2F("[G2] REBUILD-list: malloc(%u) failed", (unsigned)kPbCap);
    return false;
  }

  // The g2BuildRebuildList helper writes the complete envelope (preamble
  // through CRC) into a single buffer. We then re-read the pb body out
  // of it via sendPbFragmented? No — RebuildList builder works at the
  // envelope level, not pb-only. Use it directly with sendEnvelope-style
  // path. Looking at the helper signature it returns envelope length and
  // we ship via the same wire path as g2BuildShutdown.
  size_t envLen = g2BuildRebuildList(allocSeq(), G2_MAGIC_REBUILD,
                                     CONTAINER_NAME,
                                     items, itemCount,
                                     pb, kPbCap, geom);
  if (envLen == 0) {
    DEBUG_G2F("[G2] REBUILD-list: build failed (%u items)", (unsigned)itemCount);
    free(pb);
    return false;
  }
  bool sentOk = sendEnvelope(arm, pb, envLen);
  if (sentOk) {
    DEBUG_G2F("[G2] REBUILD-list: %u items, env=%u B sent",
              (unsigned)itemCount, (unsigned)envLen);
  }
  free(pb);
  if (!sentOk) {
    DEBUG_G2F("[G2] REBUILD-list: send failed");
    return false;
  }

  if (xSemaphoreTake(gRebuildAckSem, pdMS_TO_TICKS(1500)) != pdTRUE) {
    DEBUG_G2F("[G2] REBUILD-list timeout — no RebuildResp");
    return false;
  }
  if (!gRebuildOk) {
    DEBUG_G2F("[G2] REBUILD-list rejected by firmware");
    return false;
  }
  return true;
}

// Same shape as sendCreateListAndWait but emits a TextContainerProperty
// (TextObject, wrapper field 3) instead of a ListContainerProperty.
// Used by the page-swap worker when args->kind == PSK_TEXT.
//
// `eventCapture=true` sets IsEventCapture=1 in the pb. Speculative —
// the reference says text containers don't tap, but if the firmware
// honors the flag we'll get TextEvent CLICK on tap and can route it
// the same way we route ListEvent CLICK. Caller should arrange a
// fallback exit (gTextViewExitFn) for the case where firmware ignores
// the flag.
static bool sendCreateTextAndWait(G2Temple& arm,
                                  const char* text,
                                  uint32_t widgetId,
                                  const G2ContainerGeom& geom,
                                  bool eventCapture) {
  if (!gCreateAckSem) {
    DEBUG_G2F("[G2] CREATE-text: ack sem not ready (init path broken?)");
    return false;
  }
  gExpectMagic = (uint8_t)G2_MAGIC_CREATE;
  gCreateOk    = false;
  xSemaphoreTake(gCreateAckSem, 0);

  // Same 8 KB cap as the list path — handles JSON dumps up to that size
  // before the multi-fragment send refuses. Per-module JSON typically
  // sits well under 1 KB, so this is mostly headroom.
  constexpr size_t kPbCap = 8192;
  uint8_t* pb = (uint8_t*)malloc(kPbCap);
  if (!pb) {
    DEBUG_G2F("[G2] CREATE-text: malloc(%u) failed", (unsigned)kPbCap);
    return false;
  }
  size_t pbLen = g2BuildCreateTextPagePb(G2_MAGIC_CREATE, CONTAINER_NAME,
                                          text ? text : "", widgetId, geom,
                                          eventCapture, pb, kPbCap);
  if (pbLen == 0) {
    DEBUG_G2F("[G2] CREATE-text: pb build failed (content_len=%u)",
              (unsigned)(text ? strlen(text) : 0));
    free(pb);
    return false;
  }
  bool sentOk = sendPbFragmented(arm, allocSeq(), G2_SID_EVEN_CORE,
                                  G2_FLAG_REQUEST, pb, pbLen);
  if (sentOk) {
    DEBUG_G2F("[G2] CREATE-text: pb=%u B sent (evcap=%d)",
              (unsigned)pbLen, eventCapture ? 1 : 0);
  }
  free(pb);
  if (!sentOk) {
    DEBUG_G2F("[G2] CREATE-text: send failed");
    return false;
  }

  if (xSemaphoreTake(gCreateAckSem, pdMS_TO_TICKS(1500)) != pdTRUE) {
    DEBUG_G2F("[G2] CREATE-text timeout — container not primed (widgetId=%u)",
              (unsigned)widgetId);
    return false;
  }
  if (!gCreateOk) {
    DEBUG_G2F("[G2] CREATE-text rejected by firmware (widgetId=%u)",
              (unsigned)widgetId);
    return false;
  }
  return true;
}

// Internal: send Cmd=9 Shutdown, then wait a short fixed period to let
// the firmware tear down the current container before we issue a fresh
// CREATE. The write-mutex already serialises our own TX queue, so the
// Shutdown is guaranteed to leave the wire before the next envelope —
// the wait is for the firmware-side cleanup, not for the transport.
// We don't have a dedicated semaphore on the Cmd=10 ShutdownResp (would
// require another waiter slot), so a short conservative delay is used.
// Currently unused — the hijack path switched to sendMenuFailedAndSettle
// (Cmd=18) because Shutdown against a not-yet-instantiated container returns
// res=11 APP_REQUEST_UPGRADE_SHUTDOWN_FAILED. Retained for future "generic
// live-container teardown with settle" use where a Shutdown IS the correct
// op (e.g. tearing down our own app container before reconfiguring it).
static bool __attribute__((unused))
sendShutdownAndSettle(G2Temple& arm, uint32_t settleMs) {
  uint8_t buf[32];
  size_t n = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN, 0, buf, sizeof(buf));
  if (n == 0) return false;
  if (!sendEnvelope(arm, buf, n)) return false;
  vTaskDelay(pdMS_TO_TICKS(settleMs));
  return true;
}

// Internal: send Cmd=18 MenuStartUpFailed (cancel a pending menu-widget
// launch), then wait a short fixed period for the firmware to release the
// widget slot. Fire-and-forget — the reference schema exposes no response
// packet for this command, so there is nothing to wait for on the ack side.
// Used by the Blocks-hijack path where a Shutdown would be rejected with
// APP_REQUEST_UPGRADE_SHUTDOWN_FAILED (res=11) because no container has
// been instantiated yet — the firmware is sitting in "menu launch pending"
// after emitting cmd=17, waiting for this exact packet.
//
// `errorCode` is advisory; any nonzero value cancels the launch. We pass 1
// (generic "not handled") so the firmware's own error-recovery path runs
// briefly before our CREATE overwrites the lens.
static bool sendMenuFailedAndSettle(G2Temple& arm, uint32_t settleMs) {
  uint8_t buf[64];
  size_t n = g2BuildMenuStartupFailed(allocSeq(), G2_MAGIC_MENU_FAILED,
                                      /*errorCode*/ 1, /*errorString*/ nullptr,
                                      buf, sizeof(buf));
  if (n == 0) return false;
  if (!sendEnvelope(arm, buf, n)) return false;
  vTaskDelay(pdMS_TO_TICKS(settleMs));
  return true;
}

bool g2ShowText(const char* text) {
  if (!text) return false;
  // Prefer the right temple — verified working path per the reference.
  // Fall back to left if right is absent: we can't *prove* L alone can
  // drive the display (reference never tested it), but both arms have been
  // primed with AppLaunch in connectTemple() so we have nothing to lose
  // by trying. A visible log line makes it obvious if the user is in the
  // "L-only" regime and text doesn't appear.
  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) {
    // Distinguish "not connected" from "connected but plugin dead" so the
    // user knows whether to reconnect or just power-cycle the glasses.
    if (gR.pluginDead || gL.pluginDead) {
      DEBUG_G2F("[G2] g2ShowText: plugin silent on all connected temples — "
                "try putting the glasses on (wear detect / head-up wakes "
                "the firmware plugin) or reconnect if that doesn't recover");
    } else {
      DEBUG_G2F("[G2] g2ShowText: not connected");
    }
    return false;
  }
  if (arm == &gL) {
    DEBUG_G2F("[G2] g2ShowText: RIGHT down, falling back to LEFT "
              "(may not render — untested configuration)");
  }

  // First show of the session: CREATE_STARTUP_PAGE (Cmd=0) must ack before
  // the firmware will accept REBUILD against this container. The CREATE
  // now embeds a TextContainerProperty (TextObject, wrapper field 3) so the
  // slot is REBUILD-compatible immediately.
  if (!arm->containerReady) {
    if (!sendCreateAndWait(*arm, text, G2_DEFAULT_WIDGET_ID)) return false;
    arm->containerReady  = true;
    arm->containerIsList = false;  // CREATEd as a text widget
    g2LensSetContainer(true, false, G2_DEFAULT_WIDGET_ID);
    return true;
  }

  // Self-heal: if the live container was CREATEd as a list (hijack menu),
  // a text REBUILD would crash the firmware plugin with a "lost connection"
  // overlay. Route through the list-as-text helper which renders each line
  // as an item and keeps the widget type consistent. Read via lens state
  // for a single source of truth.
  if (g2LensGetState().containerIsList) {
    DEBUG_G2F("[G2] g2ShowText: live container is list — auto-routing "
              "via g2ShowTextAsList to avoid widget-type mismatch");
    return g2ShowTextAsList(text);
  }

  // Subsequent shows — REBUILD_PAGE (Cmd=7) with a fresh TextContainerProperty.
  //
  // Why not UPDATE_TEXT (Cmd=5): it's a partial-patch op
  // (`TextContainerUpgrade {ContentOffset, ContentLength, Content}`) whose
  // exact semantics we never fully nailed down, and we've watched it crash
  // the firmware's plugin task (2026-04-24: second g2show froze every
  // subsequent ack including heartbeats, forcing a power-cycle of the
  // glasses). REBUILD_PAGE is documented full-replace, well-exercised by the
  // reference for exactly this case, and costs only ~20 extra bytes on the
  // wire — worth it for a known-safe path. If we ever want an optimised
  // update path, revisit UPDATE_TEXT only after we've confirmed its patch
  // semantics against a TextContainer (not a ListContainer) on real hardware.
  // Heap-allocate to keep the BTC task's 3-8 KB stack budget intact —
  // g2ShowText is sometimes called from the BLE notify callback (e.g.
  // when a hijack-menu tap dispatches into a per-page handler that
  // wants to show text). 1 KB on stack + buildG2StatusSnapshot's 384 B
  // + Bluedroid's own usage was reliably overflowing BTC_TASK.
  constexpr size_t kBufSize = 1024;
  uint8_t* buf = (uint8_t*)malloc(kBufSize);
  if (!buf) {
    DEBUG_G2F("[G2] g2ShowText: malloc(%u) failed", (unsigned)kBufSize);
    return false;
  }
  size_t n = g2BuildRebuildText(allocSeq(), G2_MAGIC_REBUILD,
                                CONTAINER_NAME, text, buf, kBufSize);
  bool ok = (n != 0) && sendEnvelope(*arm, buf, n);
  free(buf);
  return ok;
}

bool g2ShowMultiLine(const char* lines[], size_t lineCount) {
  if (!lines || lineCount == 0) return false;
  // Join with \n — the G2 TextContainer renders \n as a line break.
  String joined;
  for (size_t i = 0; i < lineCount; i++) {
    if (i > 0) joined += '\n';
    joined += lines[i] ? lines[i] : "";
  }
  return g2ShowText(joined.c_str());
}

bool g2ClearDisplay() {
  // Earlier impl was `return g2ShowText("");` which sent Cmd=7 REBUILD_PAGE
  // with an empty TextContainerProperty body. The firmware rejects that with
  // res=6 and parks the widget in a zombie state — taps still arrive at us
  // as DevEvents but the widget can't render or navigate, so the lens looks
  // unresponsive until BLE eventually drops. Use the documented teardown op
  // (Cmd=9 SHUTDOWN_PAGE) instead. The next g2ShowText will CREATE fresh.
  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) {
    DEBUG_G2F("[G2] g2ClearDisplay: not connected");
    return false;
  }
  uint8_t buf[32];
  size_t n = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN, 0, buf, sizeof(buf));
  if (n == 0) return false;
  if (!sendEnvelope(*arm, buf, n)) return false;
  // Brief settle so the firmware tears the container down before any
  // follow-up CREATE arrives. Mirrors sendShutdownAndSettle's contract.
  vTaskDelay(pdMS_TO_TICKS(200));
  arm->containerReady  = false;
  arm->containerIsList = false;
  g2LensSetContainer(false, false, 0);
  return true;
}

// =============================================================================
// Page swap (SHUTDOWN + CREATE-list)
// =============================================================================
// REBUILD-list (Cmd=7) with a different item set than the original CREATE
// reliably crashes the firmware plugin task — the lens shows "Connection
// lost" and the Blocks widget slot becomes unusable until the user power-
// cycles the glasses. Confirmed across content shapes (Status / Network /
// Files / System) on 2026-04-25. The wire op itself is the trigger; size
// and content don't matter.
//
// Workaround: every "swap content" gesture goes through a SHUTDOWN+CREATE
// sequence:
//   1. Send Cmd=9 ShutdownPage  (releases the firmware widget instance)
//   2. Wait ~200 ms for the firmware to tear down
//   3. Send Cmd=0 CREATE_STARTUP_PAGE with the new items + same
//      widgetId=10509 (BLOCKS_WIDGET_ID)
//   4. Block on the CreateResp ack
//
// Why a worker task: sendCreateListAndWait blocks on a semaphore signaled
// by the BLE notify task. Calling it directly from BTC_TASK (where tap
// dispatch runs) would deadlock waiting for an ack that arrives on the
// same task. Hence the spawn-and-return pattern.

// Args carried across to the worker. We deep-copy strings into the
// worker's own heap allocation so the caller's static row buffer
// (g2ShowTextAsList's gRows, G2_Page_Files's gFilesRows, etc.) can be
// reused freely after g2ShowListPage / g2ShowTextPage returns. Without
// this, a second tap that gets dropped at the gPageSwapActive guard
// would still have rewritten the page module's static buffer —
// corrupting the in-flight worker's view of the content.
//
// The worker handles two widget kinds:
//   PSK_LIST — items[] points to a heap-owned array of strings; itemCount
//              is the row count. Renders as a ListContainerProperty with
//              one selectable row per string (current default for hijack
//              menus, sub-pages, etc.).
//   PSK_TEXT — text points to a single heap-owned string carrying the
//              entire page content (newlines render as line breaks via
//              the firmware's text widget). Renders as a TextContainer-
//              Property — no per-row selection borders, content flows
//              freely. Used by Settings JSON view and any future "free
//              text" page that doesn't want list chrome.
enum PageSwapKind : uint8_t {
  PSK_LIST = 0,
  PSK_TEXT = 1,
};

struct PageSwapArgs {
  PageSwapKind     kind;
  // PSK_LIST fields:
  char**           items;        // heap-owned; freed by the worker
  size_t           itemCount;
  // PSK_TEXT fields:
  char*            text;         // heap-owned; freed by the worker
  // Shared:
  G2ContainerGeom  geom;         // on-lens rectangle for the new container
};

// Deep-copy `src[0..n-1]` into a heap-owned char* array. Returns nullptr
// on malloc failure (partial allocations are freed). Caller frees both
// the strings and the outer array via freePageSwapItems().
static char** dupPageSwapItems(const char* const* src, size_t n) {
  if (n == 0) return nullptr;
  char** out = (char**)calloc(n, sizeof(char*));
  if (!out) return nullptr;
  for (size_t i = 0; i < n; i++) {
    const char* s = src[i] ? src[i] : "";
    size_t len = strlen(s);
    char* dst = (char*)malloc(len + 1);
    if (!dst) {
      // Roll back partial allocations.
      for (size_t j = 0; j < i; j++) free(out[j]);
      free(out);
      return nullptr;
    }
    memcpy(dst, s, len + 1);
    out[i] = dst;
  }
  return out;
}

static void freePageSwapItems(char** items, size_t n) {
  if (!items) return;
  for (size_t i = 0; i < n; i++) free(items[i]);
  free(items);
}

// gTextViewActive / gTextViewExitFn are defined at the top-of-file
// forward declarations (zero-initialized by static storage rules).
// Lifecycle: gTextViewExitFn is armed by g2ShowTextPage before the
// worker starts; gTextViewActive is set true when the CREATE-text ack
// arrives, cleared when the user-input event handlers fire the exit
// (or when a subsequent LIST swap reuses the lens).
static volatile bool     gPageSwapActive = false;
// gOurShutdownAtMs is forward-declared near the top of this file so the
// SYSTEM_EXIT handler can read it. Initial value 0 (no shutdown in flight)
// is the BSS default; we don't need an explicit `= 0` here.

static void pageSwapListWorker(void* param) {
  PageSwapArgs* args = (PageSwapArgs*)param;

  // Snapshot at worker entry: did the user have an active hijack when
  // they tapped this menu item? Used to gate auto-recovery — we only
  // recover hijacks we actually broke ourselves, not, say, a tap that
  // arrived while gHijackActive was already false (which shouldn't
  // happen, but be defensive).
  const bool hijackWasActive = gHijackActive;
  bool didShutdown = false;   // set true once we successfully sent Shutdown
  bool createOk = false;      // declared up here so `goto cleanup` can't
                              // cross its initialization

  // Cancel any active live-list page before swapping content. The live
  // worker rebuilds the same widget on a tick; if it ran concurrently
  // with our swap, the two REBUILD streams would race. Calling
  // g2StopLiveListPage is cheap (no-op when nothing's live) and waits
  // briefly for the worker to drain before we proceed.
  g2StopLiveListPage();

  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) {
    DEBUG_G2F("[G2] page-swap: no eligible temple");
    goto cleanup;
  }

  // Step 1a (experimental): if the user enabled `g2listrebuild on` AND
  // we're swapping a list onto a list with the same widget, try the
  // REBUILD-list fast path first. Skips the SHUTDOWN+CREATE cycle to see
  // if the firmware preserves its internal scroll-position state across
  // the rebuild. Risk: REBUILD-list with mismatched item sets has been
  // observed to crash the firmware plugin task — toggle is opt-in.
  if (gG2ListRebuildEnabled
      && arm->containerReady
      && arm->containerIsList
      && args->kind == PSK_LIST) {
    DEBUG_G2F("[G2] page-swap: attempting REBUILD-list fast path "
              "(items=%u, toggle=on)", (unsigned)args->itemCount);
    if (sendRebuildListAndWait(*arm, (const char* const*)args->items,
                               args->itemCount, args->geom)) {
      DEBUG_G2F("[G2] page-swap: REBUILD-list acked, %u items live "
                "(scroll-position preserved if firmware honours it)",
                (unsigned)args->itemCount);
      createOk = true;
      // Container stays live & list-typed — no flag changes needed.
      // gTextViewActive flags also unchanged because we're list→list.
      goto cleanup;
    }
    DEBUG_G2F("[G2] page-swap: REBUILD-list failed — falling back to "
              "SHUTDOWN+CREATE");
  }

  // Step 1: shutdown the existing widget so the firmware reaps the slot
  // before we ask it to create a new one. Skipped when no container is
  // live (e.g. very first call after hijack handshake). Mark our own
  // shutdown so the SYSTEM_EXIT handler can ignore the echo.
  if (arm->containerReady) {
    gOurShutdownAtMs = millis();
    uint8_t shutBuf[32];
    size_t shutN = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN,
                                   /*exitMode*/ 0, shutBuf, sizeof(shutBuf));
    if (shutN && sendEnvelope(*arm, shutBuf, shutN)) {
      DEBUG_G2F("[G2] page-swap: ShutdownPage sent");
      didShutdown = true;
    }
    // Empirical settle window between Shutdown and CREATE. 500 ms is a
    // conservative starting value to make sure the firmware has fully
    // released the widget slot before we ask it to recreate. Can be
    // tuned down later if testing shows the firmware is fast enough at
    // teardown — short windows (50–200 ms) have been observed to race
    // the prior teardown and get the new CREATE rejected.
    vTaskDelay(pdMS_TO_TICKS(500));
    arm->containerReady   = false;
    arm->containerIsList  = false;
    g2LensClearContainer();
  }

  // Step 2: CREATE the new widget. Branch on kind so the same worker
  // handles both LIST and TEXT swaps. Same widgetId either way so the
  // firmware sees this as the Blocks app re-launching with new content
  // — same identity, fresh state. (`createOk` declared above the
  // first `goto cleanup` so the jump doesn't skip its initialization.)
  if (args->kind == PSK_LIST) {
    createOk = sendCreateListAndWait(*arm, (const char* const*)args->items,
                                     args->itemCount, BLOCKS_WIDGET_ID, args->geom);
    if (createOk) {
      arm->containerReady   = true;
      arm->containerIsList  = true;
      g2LensSetContainer(true, true, BLOCKS_WIDGET_ID);
      DEBUG_G2F("[G2] page-swap: CREATE-list acked, %u items live",
                (unsigned)args->itemCount);
      // Coming back from a TEXT view to a LIST view — clear the
      // text-view tracking flags so the next user-input event isn't
      // misinterpreted as a "exit text view" trigger.
      gTextViewActive  = false;
      gTextViewExitFn  = nullptr;
    }
  } else /* PSK_TEXT */ {
    // eventCapture=true is speculative; see sendCreateTextAndWait
    // comment. The text-view-active flag arms the fallback exit path
    // for the case where firmware ignores it.
    createOk = sendCreateTextAndWait(*arm, args->text,
                                     BLOCKS_WIDGET_ID, args->geom,
                                     /*eventCapture=*/ true);
    if (createOk) {
      arm->containerReady   = true;
      arm->containerIsList  = false;        // text widget — not a list
      g2LensSetContainer(true, false, BLOCKS_WIDGET_ID);
      gTextViewActive       = true;
      gTextViewActivatedMs  = millis();
      DEBUG_G2F("[G2] page-swap: CREATE-text acked (%u B content)",
                (unsigned)(args->text ? strlen(args->text) : 0));
    }
  }

  if (!createOk) {
    DEBUG_G2F("[G2] page-swap: CREATE-%s failed — hijack state "
              "may be inconsistent",
              args->kind == PSK_LIST ? "list" : "text");

    // Auto-recovery: this fires ONLY when (a) we successfully sent our
    // own Shutdown earlier in this worker, (b) the arm is still healthy
    // (plugin not dead, no disconnect mid-swap), and (c) the user had
    // an active hijack at the moment of the tap that triggered this
    // swap. Recovery always re-CREATEs the root hijack menu (LIST), no
    // matter which kind failed — getting the user back to a known
    // working page is more valuable than trying to re-attempt the
    // failed kind.
    if (didShutdown && hijackWasActive && !arm->pluginDead) {
      DEBUG_G2F("[G2] page-swap: auto-recovery — re-CREATE root hijack menu");
      const char* fallback[G2_PAGE_REGISTRY_MAX];
      const size_t fallN = populateHijackMenuItems(fallback,
                                                    G2_PAGE_REGISTRY_MAX);
      if (fallN > 0 &&
          sendCreateListAndWait(*arm, fallback, fallN,
                                BLOCKS_WIDGET_ID, G2_GEOM_LARGE)) {
        arm->containerReady   = true;
        arm->containerIsList  = true;
        g2LensSetContainer(true, true, BLOCKS_WIDGET_ID);
        g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
        gTextViewActive       = false;
        gTextViewExitFn       = nullptr;
        DEBUG_G2F("[G2] page-swap: auto-recovery succeeded — back at root menu");
      } else {
        DEBUG_G2F("[G2] page-swap: auto-recovery FAILED — user must hit "
                  "Re-open hijack or reconnect");
      }
    }
  }

cleanup:
  gOurShutdownAtMs = 0;
  if (args->kind == PSK_LIST) {
    freePageSwapItems(args->items, args->itemCount);
  } else {
    free(args->text);
  }
  delete args;
  gPageSwapActive = false;
  vTaskDelete(nullptr);
}

// Public entry. Spawns the worker and returns immediately. `items` must
// stay valid until the swap completes — page modules satisfy this by
// using file-static row buffers, with gPageSwapActive blocking concurrent
// taps that would rewrite the buffer mid-swap.
bool g2ShowListPage(const char* const* items, size_t itemCount,
                    const G2ContainerGeom& geom) {
  if (!items || itemCount == 0) return false;
  if (gPageSwapActive) {
    DEBUG_G2F("[G2] g2ShowListPage: swap already in flight, dropping tap");
    return false;
  }
  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) {
    DEBUG_G2F("[G2] g2ShowListPage: no eligible temple");
    return false;
  }

  // Deep-copy items immediately so the caller can reuse its row buffer.
  char** itemsCopy = dupPageSwapItems(items, itemCount);
  if (!itemsCopy) {
    DEBUG_G2F("[G2] g2ShowListPage: item-copy alloc failed (%u items)",
              (unsigned)itemCount);
    return false;
  }
  PageSwapArgs* args = new PageSwapArgs;
  args->kind      = PSK_LIST;
  args->items     = itemsCopy;
  args->itemCount = itemCount;
  args->text      = nullptr;
  args->geom      = geom;
  gPageSwapActive = true;
  // Stack 4 KB matches hijackWorkerTask — sendCreateListAndWait now
  // heap-allocates its own packet buffer so there's no per-call stack
  // pressure beyond the FreeRTOS task glue.
  BaseType_t rc = xTaskCreate(pageSwapListWorker, "g2_page_swap",
                              /*stack*/ 4096, args,
                              /*prio*/  tskIDLE_PRIORITY + 2, nullptr);
  if (rc != pdPASS) {
    DEBUG_G2F("[G2] g2ShowListPage: xTaskCreate failed (rc=%d)", (int)rc);
    freePageSwapItems(itemsCopy, itemCount);
    delete args;
    gPageSwapActive = false;
    return false;
  }
  return true;
}

// Public entry — render `content` on the lens as a TextContainer
// (rather than ListContainer with one row per logical line). No
// per-row selection borders; text flows freely. Used by Settings
// JSON view and any future "free text" page that doesn't want list
// chrome.
//
// `exitFn` (optional) is called from the user-input handlers when
// firmware doesn't fire TextEvent CLICK on tap — see the page-swap
// worker's PSK_TEXT branch for the gTextViewExitFn arming. Pass
// nullptr if you don't need a fallback exit (e.g. you want the user
// to tap-and-hold to exit via the firmware's native gesture).
bool g2ShowTextPage(const char* content, const G2ContainerGeom& geom,
                    void (*exitFn)(), G2TapFn tapFn) {
  if (!content) return false;
  if (gPageSwapActive) {
    DEBUG_G2F("[G2] g2ShowTextPage: swap already in flight, dropping tap");
    return false;
  }
  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) {
    DEBUG_G2F("[G2] g2ShowTextPage: no eligible temple");
    return false;
  }

  // Heap-copy the content so the caller's buffer can be reused while
  // the worker is still running. Same lifetime model as the list path.
  const size_t len = strlen(content);
  char* copy = (char*)malloc(len + 1);
  if (!copy) {
    DEBUG_G2F("[G2] g2ShowTextPage: copy alloc failed (%u B)", (unsigned)len);
    return false;
  }
  memcpy(copy, content, len + 1);

  PageSwapArgs* args = new PageSwapArgs;
  args->kind      = PSK_TEXT;
  args->items     = nullptr;
  args->itemCount = 0;
  args->text      = copy;
  args->geom      = geom;

  // Arm the fallback exit + optional tap handler before flagging the
  // swap active so the dispatcher can't see a stale state even briefly.
  // The worker sets gTextViewActive after CREATE acks; we just stash
  // the function pointers up front.
  gTextViewExitFn = exitFn;
  gTextViewTapFn  = tapFn;

  gPageSwapActive = true;
  BaseType_t rc = xTaskCreate(pageSwapListWorker, "g2_page_swap",
                              /*stack*/ 4096, args,
                              /*prio*/  tskIDLE_PRIORITY + 2, nullptr);
  if (rc != pdPASS) {
    DEBUG_G2F("[G2] g2ShowTextPage: xTaskCreate failed (rc=%d)", (int)rc);
    free(copy);
    delete args;
    gPageSwapActive = false;
    gTextViewExitFn = nullptr;
    gTextViewTapFn  = nullptr;
    return false;
  }
  return true;
}

// Render a newline-separated text blob as a list page. First item is always
// "<- Back" (returns to MAIN); subsequent items are the text lines. Mutates
// a private buffer in place to NUL-terminate each line. Returns true if the
// REBUILD frame went out.
bool g2ShowTextAsList(const char* text) {
  if (!text) return false;

  // Cap: list widget renders ~6-8 lines comfortably. Beyond that the user
  // would need to scroll — which works, but very long lists waste the row
  // buffer. 32 is plenty for the current Status/Sensors/System payloads.
  static constexpr size_t kMaxRows = 32;
  static char        gRows[kMaxRows][64];
  static const char* gPtrs[kMaxRows];

  // Row 0: back affordance.
  strncpy(gRows[0], "<- Back", sizeof(gRows[0]));
  gRows[0][sizeof(gRows[0]) - 1] = '\0';
  gPtrs[0] = gRows[0];
  size_t n = 1;

  // Walk the input, copy non-empty lines into the row buffer.
  const char* p = text;
  while (*p && n < kMaxRows) {
    const char* nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len > 0) {
      size_t take = len < (sizeof(gRows[0]) - 1) ? len : (sizeof(gRows[0]) - 1);
      memcpy(gRows[n], p, take);
      gRows[n][take] = '\0';
      gPtrs[n] = gRows[n];
      n++;
    }
    if (!nl) break;
    p = nl + 1;
  }

  if (g2ShowListPage(gPtrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_TEXT_VIEW);
    DEBUG_G2F("[G2] g2ShowTextAsList: %u rows shown", (unsigned)n);
    return true;
  }
  return false;
}

// =============================================================================
// Live-list page primitive
// =============================================================================
// Periodically rebuilds a list-shaped status page in place via Cmd=7
// REBUILD-list. Caller supplies a buildText callback (same shape as the
// G2PageModule.buildText hook) and an interval; the worker calls the
// callback every interval, splits the resulting newline-separated text
// into rows, prepends "<- Back", and ships a REBUILD-list. Each tick is
// one Cmd=7 envelope (~70 ms) so the lens shows no flicker between
// updates — see the g2listrebuild toggle comment for empirical proof.
//
// Lifecycle:
//   1. Caller hits the page handler (e.g. user taps Status from main).
//      Page handler calls g2StartLiveListPage(buildFn, interval).
//   2. Initial render uses the standard g2ShowListPage path so the
//      widget gets CREATEd cleanly; subsequent ticks REBUILD in place.
//   3. Worker spins on its own task. Each loop:
//        - vTaskDelay(interval) OR wake on the refresh-kick semaphore
//        - call buildFn → fresh rows
//        - sendRebuildListAndWait → REBUILD-list
//   4. SysEvent DOUBLE_CLICK(3) src=2 while a live page is active gives
//      the refresh-kick sem so the next tick fires immediately. (Single
//      tap goes through the normal ListEvent path → row-tap → page-swap,
//      which calls g2StopLiveListPage to cancel before swapping.)
//   5. Page-swap worker calls g2StopLiveListPage at entry — any tap that
//      drills out of the live page cancels the worker cleanly.
//
// Single slot only; calling g2StartLiveListPage while another live page
// is active stops the previous one first.

typedef void (*G2LivePageBuildFn)(char* out, size_t cap);

static volatile bool       gLivePageActive    = false;
static volatile bool       gLivePageStopFlag  = false;
static G2LivePageBuildFn   gLivePageBuildFn   = nullptr;
static volatile uint32_t   gLivePageIntervalMs = 5000;
static SemaphoreHandle_t   gLivePageRefreshSem = nullptr;

// Render `text` into the same row-shape g2ShowTextAsList uses (Back row
// prepended, newline-split). Caller owns the row storage.
static size_t splitTextIntoRows(const char* text,
                                char rows[][64], const char** ptrs,
                                size_t maxRows) {
  if (!text || maxRows == 0) return 0;
  strncpy(rows[0], "<- Back", sizeof(rows[0]));
  rows[0][sizeof(rows[0]) - 1] = '\0';
  ptrs[0] = rows[0];
  size_t n = 1;
  const char* p = text;
  while (*p && n < maxRows) {
    const char* nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len > 0) {
      size_t take = len < (sizeof(rows[0]) - 1) ? len : (sizeof(rows[0]) - 1);
      memcpy(rows[n], p, take);
      rows[n][take] = '\0';
      ptrs[n] = rows[n];
      n++;
    }
    if (!nl) break;
    p = nl + 1;
  }
  return n;
}

// Worker task — runs the tick loop until gLivePageStopFlag is set.
static void livePageWorker(void* /*arg*/) {
  // Heap-owned because the rows can be 32 × 64 = 2 KB which is more
  // stack than we want to chew, and PSRAM is plentiful.
  constexpr size_t kMaxRows = 32;
  char (*rows)[64]  = (char(*)[64])heap_caps_malloc(sizeof(char[kMaxRows][64]),
                                                    MALLOC_CAP_8BIT);
  const char** ptrs = (const char**)heap_caps_malloc(sizeof(const char*) * kMaxRows,
                                                      MALLOC_CAP_8BIT);
  char* textBuf     = (char*)heap_caps_malloc(2048, MALLOC_CAP_8BIT);
  if (!rows || !ptrs || !textBuf) {
    DEBUG_G2F("[G2] live-page: worker heap alloc failed");
    if (rows)    free(rows);
    if (ptrs)    free(ptrs);
    if (textBuf) free(textBuf);
    gLivePageActive = false;
    vTaskDelete(nullptr);
    return;
  }

  while (!gLivePageStopFlag) {
    // Wait for the next tick OR an early kick from double-tap. Poll the
    // stop flag in 100 ms slices so worker termination is responsive.
    // The same semaphore is used for both refresh kicks and stop kicks
    // (g2StopLiveListPage gives it to wake the worker immediately) — so
    // when we wake from the sem, we have to distinguish: if the stop
    // flag is set, fall through silently to the outer-loop exit; only
    // log "refresh kicked" when this is genuinely a double-tap kick.
    const uint32_t waitMs = gLivePageIntervalMs;
    const uint32_t startMs = millis();
    while ((millis() - startMs) < waitMs && !gLivePageStopFlag) {
      if (xSemaphoreTake(gLivePageRefreshSem, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!gLivePageStopFlag) {
          DEBUG_G2F("[G2] live-page: refresh kicked by double-tap");
        }
        break;
      }
    }
    if (gLivePageStopFlag) break;

    if (!gLivePageBuildFn) continue;
    textBuf[0] = '\0';
    gLivePageBuildFn(textBuf, 2048);
    size_t n = splitTextIntoRows(textBuf, rows, ptrs, kMaxRows);
    if (n == 0) continue;

    G2Temple* arm = nullptr;
    if (gR.connected && !gR.pluginDead)      arm = &gR;
    else if (gL.connected && !gL.pluginDead) arm = &gL;
    if (!arm) {
      DEBUG_G2F("[G2] live-page: no eligible temple, ending worker");
      break;
    }

    if (sendRebuildListAndWait(*arm, ptrs, n, G2_GEOM_LARGE)) {
      DEBUG_G2F("[G2] live-page: REBUILD-list tick (%u rows)", (unsigned)n);
    } else {
      DEBUG_G2F("[G2] live-page: REBUILD-list failed — aborting worker");
      break;
    }
  }

  free(rows);
  free(ptrs);
  free(textBuf);
  gLivePageActive   = false;
  gLivePageStopFlag = false;
  gLivePageBuildFn  = nullptr;
  DEBUG_G2F("[G2] live-page: worker exited");
  vTaskDelete(nullptr);
}

// Public API — start a live list page.
bool g2StartLiveListPage(G2LivePageBuildFn buildFn, uint32_t intervalMs) {
  if (!buildFn) return false;
  if (intervalMs < 500) intervalMs = 500;  // floor to avoid hammering

  // If a live page is already running, stop it first.
  if (gLivePageActive) g2StopLiveListPage();

  if (!gLivePageRefreshSem) {
    gLivePageRefreshSem = xSemaphoreCreateBinary();
    if (!gLivePageRefreshSem) return false;
  }
  // Drain any stale signal from a prior session.
  xSemaphoreTake(gLivePageRefreshSem, 0);

  // Initial render via the standard path so the widget gets CREATEd
  // cleanly; this also seeds containerReady so subsequent REBUILDs
  // succeed. Build the text once here and ship via g2ShowTextAsList.
  char seed[2048];
  seed[0] = '\0';
  buildFn(seed, sizeof(seed));
  if (!g2ShowTextAsList(seed)) {
    DEBUG_G2F("[G2] live-page: initial CREATE-list failed");
    return false;
  }

  gLivePageBuildFn    = buildFn;
  gLivePageIntervalMs = intervalMs;
  gLivePageStopFlag   = false;
  gLivePageActive     = true;

  // 4 KB stack is enough — the worker uses heap-allocated buffers for
  // the row storage and text buffer, so the stack only carries the
  // worker's local frames + sendRebuildListAndWait's small pb buffer.
  if (xTaskCreate(livePageWorker, "g2_live_page", 4096, nullptr,
                  /*prio*/ 5, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] live-page: xTaskCreate failed");
    gLivePageActive = false;
    gLivePageBuildFn = nullptr;
    return false;
  }
  DEBUG_G2F("[G2] live-page: started (interval=%u ms)", (unsigned)intervalMs);
  return true;
}

// Public API — stop the active live page (no-op if none).
void g2StopLiveListPage() {
  if (!gLivePageActive) return;
  gLivePageStopFlag = true;
  // Kick the refresh sem so the worker wakes immediately and notices
  // the stop flag rather than waiting out the full interval.
  if (gLivePageRefreshSem) xSemaphoreGive(gLivePageRefreshSem);
  // Worker self-deletes; we don't join. Spin-wait briefly so the
  // worker's cleanup ordering is observable to the next caller.
  for (int i = 0; i < 30 && gLivePageActive; i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Internal — give the refresh sem if a live page is active. Called from
// the SysEvent handler when DOUBLE_CLICK(3) src=2 fires.
static void livePageKickRefresh() {
  if (gLivePageActive && gLivePageRefreshSem) {
    xSemaphoreGive(gLivePageRefreshSem);
  }
}

// Forward-declared above handleEnvelope so the SysEvent handler can
// peek at the live-page state without a variable forward-declaration
// (which C++ doesn't permit cleanly for file-scope statics).
static bool livePageIsActive() {
  return gLivePageActive;
}

// =============================================================================
// Hijack page-mode tracker
// =============================================================================
// Stateful pages (Files navigation, Settings toggle) need to know which
// page is currently active so handleHijackMenuTap() can route the tap to
// the right per-page handler. The MAIN page is the top-level menu list
// (Status / Sensors / System / Network / Files / Settings / (future));
// the others are nested screens entered from MAIN.
//
// State change rules:
//   * gHijackPage = MAIN on every fresh hijack entry (set by hijackWorkerTask)
//   * gHijackPage flips to <P> when the user taps an item that opens a
//     sub-page; the per-page enter() function is responsible for the
//     REBUILD-with-list (or whatever rendering) and updating this flag
//   * gHijackPage flips back to MAIN when the user taps the special
//     "<- Back" item that every sub-page includes as item 0

// =============================================================================
// G2 lens state — single source of truth (see Optional_EvenG2.h G2LensState)
// =============================================================================

// Internal storage. Only this file mutates it directly; everyone else goes
// through the g2Lens* accessors so transitions are logged and side-effects
// (SSE push, overlay expiry callbacks) are consistent.
static struct {
  bool          hijackActive       = false;
  uint32_t      hijackStartedMs    = 0;
  G2HijackPage  hijackPage         = G2_HIJACK_PAGE_MAIN;
  bool          containerReady     = false;
  bool          containerIsList    = false;
  uint32_t      containerWidgetId  = 0;
  G2OverlayKind overlayKind        = G2_OVERLAY_NONE;
  uint32_t      overlayDeadlineMs  = 0;
} gLens;

static G2OverlayExpiredCb gOverlayExpiredCb = nullptr;

G2LensState g2LensGetState() {
  G2LensState s;
  s.hijackActive       = gLens.hijackActive;
  s.hijackStartedMs    = gLens.hijackStartedMs;
  s.hijackPage         = gLens.hijackPage;
  s.containerReady     = gLens.containerReady;
  s.containerIsList    = gLens.containerIsList;
  s.containerWidgetId  = gLens.containerWidgetId;
  s.overlayKind        = gLens.overlayKind;
  s.overlayDeadlineMs  = gLens.overlayDeadlineMs;
  return s;
}

void g2LensSetHijackActive(bool active) {
  if (gLens.hijackActive == active) return;
  gLens.hijackActive = active;
  if (active) gLens.hijackStartedMs = millis();
  else        gLens.hijackStartedMs = 0;
  DEBUG_G2F("[G2] lens.hijackActive → %d", active ? 1 : 0);
}

void g2LensSetContainer(bool ready, bool isList, uint32_t widgetId) {
  gLens.containerReady    = ready;
  gLens.containerIsList   = isList;
  gLens.containerWidgetId = widgetId;
  DEBUG_G2F("[G2] lens.container ready=%d isList=%d wid=%u",
            ready ? 1 : 0, isList ? 1 : 0, (unsigned)widgetId);
}

void g2LensClearContainer() {
  // Don't clear hijackPage — even if the container went away, the page
  // tracker reflects "where the user was" and the next CREATE will reset
  // it to MAIN explicitly.
  gLens.containerReady    = false;
  gLens.containerIsList   = false;
  gLens.containerWidgetId = 0;
  DEBUG_G2F("[G2] lens.container cleared");
}

void g2LensStartOverlay(G2OverlayKind kind, uint32_t durationMs) {
  gLens.overlayKind       = kind;
  gLens.overlayDeadlineMs = millis() + durationMs;
  DEBUG_G2F("[G2] lens.overlay kind=%u for %u ms",
            (unsigned)kind, (unsigned)durationMs);
}

void g2LensClearOverlay() {
  if (gLens.overlayKind == G2_OVERLAY_NONE) return;
  DEBUG_G2F("[G2] lens.overlay cleared (was kind=%u)",
            (unsigned)gLens.overlayKind);
  gLens.overlayKind       = G2_OVERLAY_NONE;
  gLens.overlayDeadlineMs = 0;
}

void g2LensSetOverlayExpiredCb(G2OverlayExpiredCb cb) {
  gOverlayExpiredCb = cb;
}

// Called from g2Tick. Cheap when no overlay is active. When an overlay's
// deadline passes, fire the registered callback (if any) with the kind
// that just expired, then clear the overlay state. Page modules use the
// callback to redraw their underlying list.
static void g2LensTickOverlay() {
  if (gLens.overlayDeadlineMs == 0) return;
  if ((int32_t)(millis() - gLens.overlayDeadlineMs) < 0) return;
  G2OverlayKind expired = gLens.overlayKind;
  g2LensClearOverlay();
  if (gOverlayExpiredCb) gOverlayExpiredCb(expired);
}

// =============================================================================
// Hijack page tracker — backward-compat wrappers around lens state
// =============================================================================
// Existing call sites use g2GetHijackPage / g2SetHijackPage. Both routes go
// through gLens.hijackPage so external readers see the canonical view.

G2HijackPage g2GetHijackPage() {
  return gLens.hijackPage;
}
void g2SetHijackPage(G2HijackPage p) {
  if (gLens.hijackPage == p) return;
  gLens.hijackPage = p;
  DEBUG_G2F("[G2] lens.hijackPage → %u", (unsigned)p);
}

// Notification generation counter — each g2ShowNotification call bumps
// this, and the clear-timer task only clears if its generation is still
// current. This lets a newer notification overwrite an older one without
// the older one's timer prematurely wiping the new content.
static volatile uint32_t gNotifyGen = 0;

struct NotifyClearArgs {
  uint32_t gen;
  uint32_t delayMs;
};

static void notifyClearTaskBody(void* arg) {
  NotifyClearArgs* a = (NotifyClearArgs*)arg;
  const uint32_t myGen = a->gen;
  const uint32_t delay = a->delayMs;
  free(a);
  vTaskDelay(pdMS_TO_TICKS(delay));
  if (gNotifyGen == myGen) {
    DEBUG_G2F("[G2] Notification timer expired (gen=%u) — clearing display",
              (unsigned)myGen);
    g2ClearDisplay();
  } else {
    DEBUG_G2F("[G2] Notification timer skipped (gen=%u, current=%u) — "
              "newer notification replaced this one",
              (unsigned)myGen, (unsigned)gNotifyGen);
  }
  vTaskDelete(nullptr);
}

bool g2ShowNotification(const char* text, uint32_t durationMs) {
  if (!text) return false;
  // Bump the generation BEFORE showing so any stale clear timer sees
  // gNotifyGen != its own and no-ops immediately.
  const uint32_t myGen = ++gNotifyGen;
  if (!g2ShowText(text)) {
    DEBUG_G2F("[G2] Notification show failed (text=%u B)",
              (unsigned)strlen(text));
    return false;
  }
  if (durationMs == 0) {
    DEBUG_G2F("[G2] Notification shown (gen=%u, no auto-clear)",
              (unsigned)myGen);
    return true;
  }
  NotifyClearArgs* args = (NotifyClearArgs*)malloc(sizeof(NotifyClearArgs));
  if (!args) {
    DEBUG_G2F("[G2] Notification: alloc failed — display won't auto-clear");
    return true;  // show succeeded; clear won't
  }
  args->gen     = myGen;
  args->delayMs = durationMs;
  BaseType_t rc = xTaskCreate(notifyClearTaskBody, "g2_notify_clear",
                              /*stack*/ 3072, args,
                              /*prio*/ tskIDLE_PRIORITY + 1, nullptr);
  if (rc != pdPASS) {
    DEBUG_G2F("[G2] Notification: xTaskCreate failed — won't auto-clear");
    free(args);
    return true;
  }
  DEBUG_G2F("[G2] Notification shown (gen=%u, clear-in=%u ms)",
            (unsigned)myGen, (unsigned)durationMs);
  return true;
}

void g2SetEventCallback(G2EventCallback callback) {
  gEventCallback = callback;
}

void g2Tick() {
  // Heartbeat runs on its own timer and notifications arrive via the BLE
  // library's task. This tick handles purely time-based lens state —
  // currently just overlay auto-dismiss. Page modules register an
  // expiry callback via g2LensSetOverlayExpiredCb to redraw their
  // underlying view when an overlay times out.
  g2LensTickOverlay();
}

void getG2Status(char* buffer, size_t bufferSize) {
  if (!buffer || bufferSize == 0) return;
  const char* stateStr = getG2StateString();
  snprintf(buffer, bufferSize,
           "state=%s L=%s R=%s mtu=L%u/R%u batt=L%d/R%d tx=L%lu/R%lu rx=L%lu/R%lu",
           stateStr,
           gL.connected ? "up" : "down",
           gR.connected ? "up" : "down",
           (unsigned)gL.mtu, (unsigned)gR.mtu,
           (int)gBatteryL, (int)gBatteryR,
           (unsigned long)gL.packetsSent, (unsigned long)gR.packetsSent,
           (unsigned long)gL.packetsReceived, (unsigned long)gR.packetsReceived);
}

// Push a `g2-status` Server-Sent Event to every logged-in browser so the
// Bluetooth web page can live-update without a manual refresh. One event
// type carries the whole state blob as JSON — simpler than a zoo of
// per-field events, and the SSE burst coalescer (see WebServer_Events.cpp)
// will drop stale copies if we fire faster than the link can drain.
//
// Compact keys are used because the SSE queue's data field is capped at
// EVENT_DATA_MAX = 128 chars (see WebServer_Server.h). An earlier version
// with full-word keys ran to ~155 chars and got silently truncated into
// malformed JSON — every SSE push arrived, the client's JSON.parse threw,
// and the catch block swallowed it, leaving the UI stuck on whatever the
// initial CLI refresh had fetched. Short keys keep the worst case
// (state="authenticating", reason="plugin-dead-R", both batteries at -99)
// under ~95 chars with margin.
//
// Schema (client must match):
//   s   state string ("idle", "scanning", "connected", etc.)
//   L   left  temple: "up" | "down" | "dead"
//   R   right temple: "up" | "down" | "dead"
//   bL  left  battery % (-1 = unknown)
//   bR  right battery % (-1 = unknown)
//   h   hijack active (bool)
//   w   reason tag (free-form short string)
//
// Called from every state-change point: temple connect OK, disconnect,
// plugin-dead transition, hijack activate/deactivate, scan state shifts,
// battery update. Safe to call from BLE callbacks, the hijack worker, and
// the heartbeat task. MUST NOT be called from the Tmr Svc timer — use
// the heartbeat worker task instead.
static const char* g2TempleStatus(const G2Temple& t) {
  if (t.pluginDead) return "dead";
  return t.connected ? "up" : "down";
}

static void g2PushStatusEvent(const char* reason) {
  // Compact keys + length-bounded builder — see BLE_Events.h for the
  // why. CompactJson handles escaping and stays brace-balanced even on
  // overflow, so we don't have to worry about a stray quote in `reason`
  // corrupting the SSE line.
  char buf[128];
  CompactJson j(buf, sizeof(buf));
  j.kv("s",  getG2StateString())
   .kv("L",  g2TempleStatus(gL))
   .kv("R",  g2TempleStatus(gR))
   .kv("bL", (int)gBatteryL)
   .kv("bR", (int)gBatteryR)
   .kv("h",  (bool)gHijackActive)
   // silent / DND state mirror. -1 (unknown) until the firmware pushes
   // its first `deviceSendInfoToApp.silentMode` notification — clients
   // should treat <0 as "indeterminate, render nothing" rather than
   // "off". Once we've seen one push it tracks every subsequent toggle.
   .kv("sm", (int)gSilentMode)
   .kv("w",  reason ? reason : "");
  blePushEvent("g2-status", j);
}

// =============================================================================
// Legacy low-level exports — retained for ABI compatibility with any
// external callers. These wrap the new primitives.
// =============================================================================

uint16_t g2CalcCRC16(const uint8_t* data, size_t len) {
  return g2CrcCcittFalse(data, len);
}

size_t g2EncodeVarint(uint32_t value, uint8_t* buffer) {
  size_t pos = 0;
  // Can't fail — caller owns buffer sizing. Worst case 5 bytes for uint32.
  g2PbWriteVarint(buffer, 10, &pos, (uint64_t)value);
  return pos;
}

bool g2SendPacket(uint8_t /*serviceHi*/, uint8_t /*serviceLo*/,
                  const uint8_t* /*payload*/, size_t /*payloadLen*/) {
  // Legacy API — the old service-id scheme doesn't map to the real protocol.
  // Callers should migrate to g2ShowText / g2ClearDisplay. Return false so
  // any remaining callers fail loudly.
  DEBUG_G2F("[G2] g2SendPacket() called — legacy API, ignored");
  return false;
}

// =============================================================================
// CLI commands
// =============================================================================

static const char* cmd_g2connect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String arg = ca.arg(0);
  arg.toLowerCase();
  G2Eye eye = G2_EYE_AUTO;
  if (arg == "left")       eye = G2_EYE_LEFT;
  else if (arg == "right") eye = G2_EYE_RIGHT;
  if (!g2Connect(eye)) {
    return gConnectTaskActive
           ? "G2: connect already in progress — wait or use closeg2"
           : "G2: failed to start connect task";
  }
  return "G2: scan/connect started in background — use g2status to watch";
}

static const char* cmd_g2disconnect(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  g2Disconnect();
  return "G2: disconnected";
}

static const char* cmd_g2status(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char buf[256];
  getG2Status(buf, sizeof(buf));
  return buf;
}

// Dump everything we've learned about the connected glasses. Populated
// from the settings-push decoder (firmware version) + temple connection
// state (MAC, MTU, battery). Expand as we identify more fields.
static const char* cmd_g2info(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char buf[512];
  snprintf(buf, sizeof(buf),
           "G2 device info:\n"
           "  firmware: %s\n"
           "  Left:  connected=%d mtu=%u batt=%d mac=%s name='%s'\n"
           "  Right: connected=%d mtu=%u batt=%d mac=%s name='%s'\n"
           "  hijack=%s settingsVerbose=%s\n"
           "  (run `g2settings verbose on` to dump unidentified fields)",
           gFwVersion[0] ? gFwVersion : "(unknown — waiting for firmware to push a settings frame)",
           gL.connected ? 1 : 0, (unsigned)gL.mtu, (int)gBatteryL,
           gL.deviceAddress.length() ? gL.deviceAddress.c_str() : "--",
           gL.deviceName.length()    ? gL.deviceName.c_str()    : "--",
           gR.connected ? 1 : 0, (unsigned)gR.mtu, (int)gBatteryR,
           gR.deviceAddress.length() ? gR.deviceAddress.c_str() : "--",
           gR.deviceName.length()    ? gR.deviceName.c_str()    : "--",
           gHijackActive ? "active" : "off",
           gG2SettingsVerbose ? "on" : "off");
  return buf;
}

// Toggle verbose settings-field dumping. When on, every inner field of
// every sid=0x09 push is logged with its field number + value, so the
// user can spot patterns and label unknown fields. Off by default.
static const char* cmd_g2settings(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String sub = ca.arg(0); sub.toLowerCase();
  String val = ca.arg(1); val.toLowerCase();
  if (sub == "verbose") {
    if (val == "on")       gG2SettingsVerbose = true;
    else if (val == "off") gG2SettingsVerbose = false;
    else                   gG2SettingsVerbose = !gG2SettingsVerbose;
    return gG2SettingsVerbose
           ? "G2 settings verbose: ON (every field of every sid=0x09 push)"
           : "G2 settings verbose: OFF";
  }
  return "Usage: g2settings verbose [on|off]";
}

static const char* cmd_g2show(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return g2ShowText(argsInput.c_str()) ? "G2: text sent" : "G2: send failed";
}

// Live-update probe cadence — read by Q13 (image-tile streaming) and Q14
// (text REBUILD streaming). Runtime-only; not persisted across boots.
// Sane lower bound 100 ms — below that the firmware ack queue saturates
// and the probe in-flight cap kicks in immediately. No upper clamp.
static volatile uint32_t gG2LiveRateMs = 1000;

// Toggle the REBUILD-list fast path in the page-swap worker. Default ON
// as of 2026-04-27: empirical testing on firmware 2.2.0.24 showed
// REBUILD-list is safe across item-count changes (contradicting the
// original 2026-04-25 doc claim) and cuts page-swap time from ~700 ms
// to ~70 ms — visibly flicker-free menu navigation. Selection does NOT
// persist across REBUILD; cursor resets to row 0 every swap. Toggle
// remains runtime-tunable as a safety hatch in case a future firmware
// regresses.
static const char* cmd_g2listrebuild(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char out[120];
  CommandArgs ca(argsInput);
  String v = ca.arg(0); v.toLowerCase();
  if (v == "on" || v == "1" || v == "true") {
    gG2ListRebuildEnabled = true;
  } else if (v == "off" || v == "0" || v == "false") {
    gG2ListRebuildEnabled = false;
  } else if (v.length() == 0) {
    // No arg — show current state.
  } else {
    return "Usage: g2listrebuild [on|off]";
  }
  snprintf(out, sizeof(out),
           "G2 list-rebuild fast path: %s (%s)",
           gG2ListRebuildEnabled ? "ON" : "OFF",
           gG2ListRebuildEnabled
               ? "no-flicker page-swap; selector resets to row 0"
               : "fallback to SHUTDOWN+CREATE; brief flicker on nav");
  return out;
}

static const char* cmd_g2liverate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char out[80];
  CommandArgs ca(argsInput);
  String v = ca.arg(0);
  if (v.length() == 0) {
    snprintf(out, sizeof(out), "G2 live-update rate: %u ms",
             (unsigned)gG2LiveRateMs);
    return out;
  }
  long n = v.toInt();
  if (n < 100) {
    return "Usage: g2liverate [ms>=100]";
  }
  gG2LiveRateMs = (uint32_t)n;
  snprintf(out, sizeof(out), "G2 live-update rate set to %u ms",
           (unsigned)gG2LiveRateMs);
  return out;
}

// Pick a temple for AI-subsystem TX. Right is preferred (verified
// receiver of all Even-AI traffic in our captures); left is a fallback
// for the rare R-down-L-up case.
static G2Temple* pickEvenAIArm(const char* tag) {
  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) {
    DEBUG_G2F("[G2] %s: no reachable temple", tag);
  }
  return arm;
}

// Send one Even-AI step. `n` is the builder's return value; on 0 we log
// the build-side failure, otherwise we forward to sendEnvelope and log
// the TX-side failure. Returns false on any failure so the caller can
// abort the pipeline.
static bool sendEvenAIStep(G2Temple& arm, size_t n, const uint8_t* buf,
                           const char* tag, const char* step) {
  if (n == 0) {
    DEBUG_G2F("[G2] %s: %s build failed", tag, step);
    return false;
  }
  if (!sendEnvelope(arm, buf, n)) {
    DEBUG_G2F("[G2] %s: %s send failed", tag, step);
    return false;
  }
  return true;
}

// Host-driven front-pane card via the Even-AI subsystem (sid=0x07).
// Verified on hardware 2026-04-26 — the full pipeline below paints the
// answer card and the firmware ends with a STREAM_COMPLETE event.
//
//   1. CTRL{status=ENTER}    — bring the front-pane app up
//   2. ASK{text="(host)"}    — populate the question panel; transitions
//                              the FSM out of LISTENING
//   3. ANALYSE{errorCode=0}  — "thinking" transition
//   4. REPLY{text=<user>,    — paint the answer card
//            fTextEnd=1}
//
// The 200 ms settles between sends mirror SHUTDOWN_PAGE's settle in the
// back-pane teardown path. Each step uses a distinct magicRandom so the
// firmware's ack-correlation logic doesn't collapse them.
bool g2ShowEvenAIReply(const char* heading, const char* body) {
  if (!body) return false;
  G2Temple* arm = pickEvenAIArm("g2ShowEvenAIReply");
  if (!arm) return false;

  uint8_t buf[320];
  size_t n;

  n = g2BuildEvenAICtrl(allocSeq(), G2_MAGIC_EVEN_AI_CTRL,
                        G2_AI_STATUS_ENTER, buf, sizeof(buf));
  if (!sendEvenAIStep(*arm, n, buf, "g2ShowEvenAIReply", "CTRL ENTER")) return false;
  vTaskDelay(pdMS_TO_TICKS(200));

  // ASK populates the heading panel. Empty/null → "(host)" so the FSM
  // still transitions out of LISTEN; passing literal "" might be parsed
  // as a missing field by the firmware.
  const char* askText = (heading && *heading) ? heading : "(host)";
  n = g2BuildEvenAIAsk(allocSeq(), G2_MAGIC_EVEN_AI_ASK,
                       /*cmdCnt*/ 0, askText, buf, sizeof(buf));
  if (!sendEvenAIStep(*arm, n, buf, "g2ShowEvenAIReply", "ASK")) return false;
  vTaskDelay(pdMS_TO_TICKS(200));

  n = g2BuildEvenAIAnalyse(allocSeq(), G2_MAGIC_EVEN_AI_ANALYSE,
                           buf, sizeof(buf));
  if (!sendEvenAIStep(*arm, n, buf, "g2ShowEvenAIReply", "ANALYSE")) return false;
  vTaskDelay(pdMS_TO_TICKS(200));

  n = g2BuildEvenAIReply(allocSeq(), G2_MAGIC_EVEN_AI_REPLY,
                         /*cmdCnt*/ 0, body, /*isLast*/ true,
                         buf, sizeof(buf));
  return sendEvenAIStep(*arm, n, buf, "g2ShowEvenAIReply", "REPLY");
}

bool g2ShowEvenAIReply(const char* body) {
  return g2ShowEvenAIReply("(host)", body);
}

// Variant: skip ASK. Hypothesis — ANALYSE alone may transition the
// firmware FSM out of LISTEN, hiding the brief heading panel that the
// full pipeline shows. Heading parameter is unused (ASK is what carries
// it) but kept in the signature for API uniformity.
bool g2ShowEvenAIReplyNoAsk(const char* heading, const char* body) {
  (void)heading;
  if (!body) return false;
  G2Temple* arm = pickEvenAIArm("g2ShowEvenAIReplyNoAsk");
  if (!arm) return false;

  uint8_t buf[320];
  size_t n;

  n = g2BuildEvenAICtrl(allocSeq(), G2_MAGIC_EVEN_AI_CTRL,
                        G2_AI_STATUS_ENTER, buf, sizeof(buf));
  if (!sendEvenAIStep(*arm, n, buf, "g2ShowEvenAIReplyNoAsk", "CTRL ENTER")) return false;
  vTaskDelay(pdMS_TO_TICKS(200));

  n = g2BuildEvenAIAnalyse(allocSeq(), G2_MAGIC_EVEN_AI_ANALYSE,
                           buf, sizeof(buf));
  if (!sendEvenAIStep(*arm, n, buf, "g2ShowEvenAIReplyNoAsk", "ANALYSE")) return false;
  vTaskDelay(pdMS_TO_TICKS(200));

  n = g2BuildEvenAIReply(allocSeq(), G2_MAGIC_EVEN_AI_REPLY,
                         /*cmdCnt*/ 0, body, /*isLast*/ true,
                         buf, sizeof(buf));
  return sendEvenAIStep(*arm, n, buf, "g2ShowEvenAIReplyNoAsk", "REPLY");
}

bool g2ShowEvenAIReplyNoAsk(const char* body) {
  return g2ShowEvenAIReplyNoAsk(nullptr, body);
}

bool g2HideEvenAICard() {
  G2Temple* arm = pickEvenAIArm("g2HideEvenAICard");
  if (!arm) return false;
  uint8_t buf[64];
  size_t n = g2BuildEvenAICtrl(allocSeq(), G2_MAGIC_EVEN_AI_CTRL,
                               G2_AI_STATUS_EXIT, buf, sizeof(buf));
  if (n == 0) return false;
  return sendEnvelope(*arm, buf, n);
}

// Variant: skip both ASK and ANALYSE — the original CTRL+REPLY approach
// that left the firmware stuck in LISTEN. Kept as a control so we can
// confirm on a fresh test that the longer pipelines really are required.
// Expect this to render the listening UI and ignore the body content.
bool g2ShowEvenAIReplyDirect(const char* heading, const char* body) {
  (void)heading;
  if (!body) return false;
  G2Temple* arm = pickEvenAIArm("g2ShowEvenAIReplyDirect");
  if (!arm) return false;

  uint8_t buf[320];
  size_t n;

  n = g2BuildEvenAICtrl(allocSeq(), G2_MAGIC_EVEN_AI_CTRL,
                        G2_AI_STATUS_ENTER, buf, sizeof(buf));
  if (!sendEvenAIStep(*arm, n, buf, "g2ShowEvenAIReplyDirect", "CTRL ENTER")) return false;
  vTaskDelay(pdMS_TO_TICKS(200));

  n = g2BuildEvenAIReply(allocSeq(), G2_MAGIC_EVEN_AI_REPLY,
                         /*cmdCnt*/ 0, body, /*isLast*/ true,
                         buf, sizeof(buf));
  return sendEvenAIStep(*arm, n, buf, "g2ShowEvenAIReplyDirect", "REPLY");
}

bool g2ShowEvenAIReplyDirect(const char* body) {
  return g2ShowEvenAIReplyDirect(nullptr, body);
}

static const char* cmd_g2ai(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // Default `g2ai <text>` uses the No-ASK pipeline so the lens shows just
  // the answer card with no phantom question heading. Verified on hardware
  // 2026-04-26 — ANALYSE alone transitions the FSM out of LISTEN so REPLY
  // renders cleanly. Use `g2aih <heading>|<body>` if you want a labeled
  // card with a separate heading panel.
  return g2ShowEvenAIReplyNoAsk(argsInput.c_str())
         ? "G2: AI reply sent (CTRL+ANALYSE+REPLY — no question panel)"
         : "G2: AI reply failed";
}

static const char* cmd_g2ai_noask(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return g2ShowEvenAIReplyNoAsk(argsInput.c_str())
         ? "G2: AI reply sent (CTRL+ANALYSE+REPLY — skip ASK)"
         : "G2: AI reply failed";
}

static const char* cmd_g2ai_direct(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return g2ShowEvenAIReplyDirect(argsInput.c_str())
         ? "G2: AI reply sent (CTRL+REPLY — original failing path)"
         : "G2: AI reply failed";
}

// =============================================================================
// Protocol exploration helpers — g2protostats + g2probe
// =============================================================================
// Both run from cmd_exec_task (CLI/web), not the BLE notify task, so the
// reentrancy concern that bit our on-glasses AI tap handler doesn't apply
// here. Probe is allow-listed against a small block of dangerous sids.

static const char* cmd_g2protostats(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const bool verbose = argsInput.indexOf("verbose") >= 0 ||
                       argsInput.indexOf("-v") >= 0;
  // CLI return strings get stuffed into a single 256-byte DebugMessage
  // slot, so we can't return one giant table — it would truncate. Emit
  // each line as its own broadcast so every line gets its own slot, then
  // return a short summary as the formal command result.
  BROADCAST_PRINTF("G2 protocol stats (live):");
  BROADCAST_PRINTF("SID    Name                          TX    RX  LastFlag  LastPb  LastSample");
  const size_t n = g2statsCount();
  if (n == 0) {
    BROADCAST_PRINTF("  (none yet — run any g2 op or wait for an inbound frame)");
  }
  for (size_t i = 0; i < n; i++) {
    const G2SidStat* s = g2statsAt(i);
    if (!s) break;
    char hex[3 * 8 + 1] = {0};
    size_t hp = 0;
    for (size_t j = 0; j < s->lastSampleLen && hp + 3 < sizeof(hex); j++) {
      hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X ", s->lastSample[j]);
    }
    if (hp > 0) hex[hp - 1] = '\0';
    BROADCAST_PRINTF("0x%02X   %-29s %4u  %4u   0x%02X      %5u   [%s]",
                     s->sid, g2sidName(s->sid),
                     (unsigned)s->txCount, (unsigned)s->rxCount,
                     (unsigned)s->lastFlag, (unsigned)s->lastPbLen, hex);
  }

  if (verbose) {
    BROADCAST_PRINTF(" ");
    BROADCAST_PRINTF("Known sids (firmware service_id_def_pb.ts enum):");
    static const uint8_t kKnownSids[] = {
      0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
      0x0D, 0x0E, 0x10, 0x21, 0x22, 0x80, 0x90, 0xC0, 0xC1, 0xC4, 0xC5, 0xE0,
    };
    for (size_t i = 0; i < sizeof(kKnownSids); i++) {
      BROADCAST_PRINTF("  0x%02X  %s", kKnownSids[i], g2sidName(kKnownSids[i]));
    }
    BROADCAST_PRINTF(" ");
    BROADCAST_PRINTF("EvenAI (sid=0x07) commandIds (even_ai_pb.ts):");
    BROADCAST_PRINTF("  0 NONE  1 CTRL  2 VAD_INFO  3 ASK  4 ANALYSE  5 REPLY");
    BROADCAST_PRINTF("  6 SKILL  7 PROMPT  8 EVENT  9 HEARTBEAT  10 CONFIG  12 COMM_RSP");
    BROADCAST_PRINTF(" ");
    BROADCAST_PRINTF("EvenHub (sid=0xE0) cmds (EvenHub_pb.ts EvenHub_Cmd_List):");
    BROADCAST_PRINTF("  0 CREATE  1 CreateResp  2 DevEvent  3 ImageRaw  5 UpdateText");
    BROADCAST_PRINTF("  7 Rebuild  8 RebuildResp  9 Shutdown  10 ShutdownResp");
    BROADCAST_PRINTF("  12 Heartbeat  17 MenuStartup  18 MenuFailed");
  } else {
    BROADCAST_PRINTF(" ");
    BROADCAST_PRINTF("Use 'g2protostats verbose' for the full sid + cmd reference.");
  }
  static char ret[80];
  snprintf(ret, sizeof(ret),
           "G2 protostats: %u sid%s tracked",
           (unsigned)n, n == 1 ? "" : "s");
  return ret;
}

// g2probe <sid_hex> <cmd_dec> [body_hex]
// Builds a minimal `EvenHub`-style wrapper (`08 <cmd> 10 <magic>` plus
// optional body), sends on the given sid with flag=0x20 (request). Use to
// experimentally fire individual commands without writing C. Refuses
// known-dangerous sids (0x80 dev_config; documented in the reference's
// gotchas as having non-terminally bricked a pair during RE work).
static int parseHexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}
static size_t parseHexBytes(const char* s, uint8_t* out, size_t outCap) {
  size_t got = 0;
  while (*s && got < outCap) {
    while (*s == ' ' || *s == ':' || *s == '_') s++;  // tolerate separators
    if (!*s) break;
    int hi = parseHexNibble(*s++);
    if (hi < 0 || !*s) return SIZE_MAX;
    int lo = parseHexNibble(*s++);
    if (lo < 0) return SIZE_MAX;
    out[got++] = (uint8_t)((hi << 4) | lo);
  }
  return got;
}

static const char* cmd_g2probe(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char ret[160];
  CommandArgs ca(argsInput);
  if (ca.count() < 2) {
    return "Usage: g2probe <sid_hex> <cmd_dec> [body_hex]\n"
           "  Fires `08 <cmd> 10 <magic> [body]` on the given sid (flag=0x20).\n"
           "  Example: g2probe 07 9              -- EvenAI HEARTBEAT\n"
           "           g2probe 07 10 080110A001  -- EvenAI CONFIG voiceSwitch=0,streamSpeed=160\n"
           "  sid=0x80 (dev_config) is blocked — known to brick.";
  }
  const uint8_t sid = (uint8_t)strtoul(ca.arg(0).c_str(), nullptr, 16);
  const uint32_t cmd = (uint32_t)ca.argInt(1, 0);

  if (sid == 0x80) {
    return "G2: probe denied — sid=0x80 (dev_config) is in the brick blocklist.";
  }

  uint8_t body[200];
  size_t bodyLen = 0;
  if (ca.count() >= 3) {
    String hex = ca.arg(2);
    bodyLen = parseHexBytes(hex.c_str(), body, sizeof(body));
    if (bodyLen == SIZE_MAX) return "G2: probe — bad hex body (need pairs of nibbles)";
  }

  // Build pb: f1 cmd, f2 magic=250, then raw body.
  uint8_t pb[256];
  size_t pos = 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, /*field*/ 1, cmd)) return "G2: probe build failed (cmd)";
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, /*field*/ 2, 250)) return "G2: probe build failed (magic)";
  if (bodyLen) {
    if (pos + bodyLen > sizeof(pb)) return "G2: probe — body too large for single fragment";
    memcpy(pb + pos, body, bodyLen);
    pos += bodyLen;
  }

  uint8_t env[256];
  size_t n = g2BuildEnvelope(allocSeq(), sid, G2_FLAG_REQUEST, pb, pos, env, sizeof(env));
  if (n == 0) return "G2: probe — envelope build failed";

  G2Temple* arm = pickEvenAIArm("g2probe");
  if (!arm) return "G2: probe — no reachable temple";
  if (!sendEnvelope(*arm, env, n)) return "G2: probe — send failed (mutex timeout?)";

  snprintf(ret, sizeof(ret),
           "G2: probe sid=0x%02X (%s) cmd=%u body=%u B sent — watch logs for response",
           sid, g2sidName(sid), (unsigned)cmd, (unsigned)bodyLen);
  return ret;
}

// Image-streaming wire-path probe. Builds an EvenCore Cmd=3
// (UPDATE_IMAGE_RAW_DATA) body carrying `<size>` bytes of test pattern
// and ships it via the multi-fragment sender. We do NOT issue a
// CREATE-image first (that schema is unknown — see g2BuildCreateImage
// TODO in System_G2_Protocol.h), so the firmware will almost certainly
// reject this with `ImgRawFailed (5)` because there's no destination
// container. That rejection IS the useful outcome: it confirms our
// multi-fragment Cmd=3 wire path is plumbed correctly end-to-end (TX
// → reassembler → app-layer reject) and tells us the firmware's
// failure shape so we can iterate once we have the schema.
//
// Usage: g2imgprobe [size_bytes]   (defaults to 1024 B)
//
// Pattern: alternating 0xF0 / 0x0F bytes — visible as vertical stripes
// if the firmware ever DID happen to render this against a 4-bpp
// container. Mostly useful as a non-zero, non-repeating-byte stream
// that won't be optimised away by any zero-padding reassembly trick.
static const char* cmd_g2imgprobe(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char ret[200];
  CommandArgs ca(argsInput);

  size_t sizeBytes = 1024;
  if (ca.count() >= 1) {
    int v = ca.argInt(0, (int)sizeBytes);
    if (v > 0 && v <= 4096) sizeBytes = (size_t)v;
    else return "G2: imgprobe — size must be 1..4096 bytes";
  }

  // Build the test pattern on the heap; 4 KB on stack would crowd the
  // BTC task's budget. Free at the end of this function.
  uint8_t* pattern = (uint8_t*)malloc(sizeBytes);
  if (!pattern) return "G2: imgprobe — pattern alloc failed";
  for (size_t i = 0; i < sizeBytes; i++) {
    pattern[i] = (i & 1) ? 0x0F : 0xF0;
  }

  // Image bodies can be much larger than a single fragment, so we
  // build the pb body into a separate buffer and let sendPbFragmented
  // chunk it. Size budget: pb wrapper overhead (~10 B) + nested
  // ImgRawMsg overhead (~6 B) + dataLen.
  const size_t bodyCap = sizeBytes + 64;
  uint8_t* body = (uint8_t*)malloc(bodyCap);
  if (!body) {
    free(pattern);
    return "G2: imgprobe — body alloc failed";
  }

  // Schema-correct probe: pretend we have a container named "img" with
  // ID=1 and that this is fragment 0 of a single-chunk session. Without
  // a CREATE-image first the firmware should respond with
  // `ImgRawFailed(5)` or `InvalidContainer(1)` — either ack confirms
  // the wire path is plumbed end-to-end.
  const size_t bodyLen = g2BuildImageRawBody(
      G2_MAGIC_IMAGE_BASE,
      /*containerId*/ 1, /*containerName*/ "img",
      /*mapSessionId*/ 1, /*mapTotalSize*/ (uint32_t)sizeBytes,
      /*mapFragmentIndex*/ 0,
      pattern, sizeBytes,
      body, bodyCap);
  free(pattern);
  if (bodyLen == 0) {
    free(body);
    return "G2: imgprobe — body build failed";
  }

  G2Temple* arm = pickEvenAIArm("g2imgprobe");
  if (!arm) {
    free(body);
    return "G2: imgprobe — no reachable temple";
  }

  const bool ok = sendPbFragmented(*arm, allocSeq(), G2_SID_EVEN_CORE,
                                   G2_FLAG_REQUEST, body, bodyLen);
  free(body);
  if (!ok) return "G2: imgprobe — fragmented send failed";

  snprintf(ret, sizeof(ret),
           "G2: imgprobe sent %u B body via Cmd=3 multi-frag — "
           "expect ImgRawFailed(5) on Cmd=4 (no CREATE-image yet); "
           "watch logs for OS_RESPONSE_IMAGE_RAW_DATA",
           (unsigned)bodyLen);
  return ret;
}

// EvenAI CONFIG probe (Cmd=10). Fires a single CONFIG message with
// caller-specified voiceSwitch / streamSpeed values. Use to characterise
// what the firmware accepts vs. rejects on this sub-command — the
// reference docs name CONFIG=10 but never ship a worked example, so we
// learn its schema by trying and watching COMM_RSP.
//
// Usage:
//   g2aiconfig                 -> empty body (just cmd+magic) — does the
//                                 firmware ack a no-payload CONFIG?
//   g2aiconfig 0               -> voiceSwitch=0 only
//   g2aiconfig 1 160           -> voiceSwitch=1, streamSpeed=160 (the
//                                 example string from g2-kit-unofficial)
//   g2aiconfig - 200           -> streamSpeed only (use - to skip
//                                 voiceSwitch)
//
// Watch the next inbound sid=0x07 frame for COMM_RSP errorCode — 0
// means the firmware liked the body, anything else tells us we
// guessed a field number wrong.
static const char* cmd_g2aiconfig(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char ret[160];
  CommandArgs ca(argsInput);

  uint64_t voiceSwitch = UINT64_MAX;  // sentinel = omit
  uint64_t streamSpeed = UINT64_MAX;

  if (ca.count() >= 1) {
    String a = ca.arg(0);
    if (a != "-" && a.length() > 0) voiceSwitch = (uint64_t)a.toInt();
  }
  if (ca.count() >= 2) {
    String a = ca.arg(1);
    if (a != "-" && a.length() > 0) streamSpeed = (uint64_t)a.toInt();
  }

  uint8_t env[128];
  size_t n = g2BuildEvenAIConfig(allocSeq(), G2_MAGIC_EVEN_AI_CTRL,
                                 voiceSwitch, streamSpeed,
                                 env, sizeof(env));
  if (n == 0) return "G2: aiconfig — envelope build failed";

  G2Temple* arm = pickEvenAIArm("g2aiconfig");
  if (!arm) return "G2: aiconfig — no reachable temple";
  if (!sendEnvelope(*arm, env, n)) return "G2: aiconfig — send failed (mutex timeout?)";

  // Format what we sent so the user can correlate with the next RX log.
  char vsBuf[16] = "(omit)";
  char ssBuf[16] = "(omit)";
  if (voiceSwitch != UINT64_MAX) snprintf(vsBuf, sizeof(vsBuf), "%llu",
                                          (unsigned long long)voiceSwitch);
  if (streamSpeed != UINT64_MAX) snprintf(ssBuf, sizeof(ssBuf), "%llu",
                                          (unsigned long long)streamSpeed);
  snprintf(ret, sizeof(ret),
           "G2: aiconfig sent — voiceSwitch=%s streamSpeed=%s — "
           "watch logs for sid=0x07 COMM_RSP errorCode",
           vsBuf, ssBuf);
  return ret;
}

// Two-string variant — splits on '|'. Usage: g2aih <heading>|<body>
// Empty heading falls back to the default. Use this to give the front-
// pane card a custom title above the body, e.g. "Weather|72 °F sunny".
static const char* cmd_g2aih(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  int sep = argsInput.indexOf('|');
  if (sep < 0) {
    // No separator: treat the whole input as the body, default heading.
    return g2ShowEvenAIReply(argsInput.c_str())
           ? "G2: AI reply sent"
           : "G2: AI reply failed";
  }
  String heading = argsInput.substring(0, sep);
  String body    = argsInput.substring(sep + 1);
  heading.trim();
  body.trim();
  return g2ShowEvenAIReply(heading.c_str(), body.c_str())
         ? "G2: AI reply sent (custom heading)"
         : "G2: AI reply failed";
}

// Generic page-display helper. Looks up a page in the registry, builds
// its content, and pushes it via g2ShowText (which auto-routes through
// g2ShowTextAsList when the lens has a list container active — see the
// containerIsList self-heal in g2ShowText).
//
// Used by all the per-page CLI commands below. New pages get a one-line
// command shim; the common logic stays here.
static const char* cmd_g2page_run(const char* pageName) {
  static char buf[80];
  const G2PageModule* p = g2FindPageByName(pageName);
  if (!p) {
    snprintf(buf, sizeof(buf), "[G2] Page '%s' not registered", pageName);
    return buf;
  }
  if (!p->buildText) {
    snprintf(buf, sizeof(buf), "[G2] Page '%s' has no text renderer", pageName);
    return buf;
  }
  char content[512];
  p->buildText(content, sizeof(content));
  if (g2ShowText(content)) {
    snprintf(buf, sizeof(buf), "[G2] %s page sent to lens",
             p->hijackLabel ? p->hijackLabel : p->name);
    return buf;
  }
  snprintf(buf, sizeof(buf),
           "[G2] %s page send failed (not connected?)",
           p->hijackLabel ? p->hijackLabel : p->name);
  return buf;
}

#define G2_PAGE_CMD(cmdsuffix, pageName)                              \
  static const char* cmd_g2##cmdsuffix(const String& /*args*/) {      \
    RETURN_VALID_IF_VALIDATE_CSTR();                                  \
    return cmd_g2page_run(pageName);                                  \
  }

G2_PAGE_CMD(sensors,      "sensors")
G2_PAGE_CMD(system,       "system")
G2_PAGE_CMD(network,      "network")
G2_PAGE_CMD(files,        "files")
// NB: `g2settings` is taken by the protocol-debug verbose toggle. Use
// `g2settingspage` for the on-glasses settings inspector.
G2_PAGE_CMD(settingspage, "settingspage")

// Placeholder notification: shows text for a fixed duration then clears.
// Format: `g2notify [<seconds>] <text>`
//   g2notify hello world        → 5s default
//   g2notify 10 hello world     → 10s duration
// Caveat: this is NOT a real overlay notification — it uses the full-screen
// text path, so it wipes whatever is currently on the lens. Replace the
// implementation once the JSON-over-EFS notification protocol is reversed.
static const char* cmd_g2notify(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();
  if (args.length() == 0) return "Usage: g2notify [<seconds>] <text>";

  // Look for a leading integer for duration. If found, consume it.
  uint32_t duration = 5000;
  int firstSpace = args.indexOf(' ');
  if (firstSpace > 0) {
    String maybeDur = args.substring(0, firstSpace);
    bool allDigits = maybeDur.length() > 0;
    for (size_t i = 0; i < maybeDur.length(); i++) {
      if (!isdigit((unsigned char)maybeDur[i])) { allDigits = false; break; }
    }
    if (allDigits) {
      const long secs = maybeDur.toInt();
      if (secs > 0 && secs < 600) {  // sanity-cap at 10 minutes
        duration = (uint32_t)secs * 1000;
        args = args.substring(firstSpace + 1);
        args.trim();
      }
    }
  }
  if (args.length() == 0) return "Usage: g2notify [<seconds>] <text>";
  return g2ShowNotification(args.c_str(), duration)
         ? "G2 notify: shown (placeholder — not a real overlay, full-screen)"
         : "G2 notify: show failed";
}

static const char* cmd_g2clear(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return g2ClearDisplay() ? "G2: cleared" : "G2: clear failed";
}

static const char* cmd_g2scan(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return g2Connect(G2_EYE_AUTO) ? "G2: found" : "G2: not found";
}

static const char* cmd_g2init(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return initG2Client() ? "G2: init ok" : "G2: init failed";
}

static const char* cmd_g2deinit(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  deinitG2Client();
  return "G2: deinit ok";
}

static const char* cmd_g2battery(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!isG2Connected()) return "G2: not connected";
  // Kick a request on each connected temple; responses land asynchronously
  // and update gBatteryL/gBatteryR. Print whatever we have cached now — the
  // fresh response will show up on subsequent status queries.
  uint8_t buf[64];
  if (gL.connected) {
    uint8_t seq = allocSeq();
    size_t n = g2BuildSettingBasicRequest(seq, G2_MAGIC_SETTINGS,
                                          buf, sizeof(buf));
    if (n) sendEnvelope(gL, buf, n);
  }
  if (gR.connected) {
    uint8_t seq = allocSeq();
    size_t n = g2BuildSettingBasicRequest(seq, G2_MAGIC_SETTINGS,
                                          buf, sizeof(buf));
    if (n) sendEnvelope(gR, buf, n);
  }
  static char out[64];
  snprintf(out, sizeof(out), "G2 battery: L=%s R=%s (refresh in progress)",
           gBatteryL < 0 ? "?" : String(gBatteryL).c_str(),
           gBatteryR < 0 ? "?" : String(gBatteryR).c_str());
  return out;
}

static const char* cmd_g2mic(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!isG2Connected()) return "G2: not connected";
  CommandArgs ca(argsInput);
  String a = ca.arg(0); a.toLowerCase();
  bool enable = (a == "on" || a == "start" || a == "1");
  uint8_t buf[64];
  uint8_t seq = allocSeq();
  size_t n = g2BuildAudioCtrl(seq, G2_MAGIC_AUDIO_CTRL, enable,
                              buf, sizeof(buf));
  if (n == 0 || !sendToBoth(buf, n)) return "G2 mic: send failed";
  // NOTE: audio frames are delivered on the left temple's render-notify
  // (…e6402), not on the command-notify (…e5402) we currently subscribe
  // to. Enabling the mic here is harmless but no audio will arrive until
  // a future phase subscribes to the render-notify characteristic and
  // ports an LC3 decoder.
  return enable ? "G2 mic: requested ON (LC3 decode not yet wired)"
                : "G2 mic: requested OFF";
}

static const char* cmd_g2nav(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String a = ca.arg(0); a.toLowerCase();
  // Bare command = report current state (used by the web UI on page
  // load to populate the toggle without flipping it). on/off sets,
  // toggle flips.
  if (a == "on")          gG2MenuNavEnabled = true;
  else if (a == "off")    gG2MenuNavEnabled = false;
  else if (a == "toggle") gG2MenuNavEnabled = !gG2MenuNavEnabled;
  return gG2MenuNavEnabled ? "G2 nav: on" : "G2 nav: off";
}

static const char* cmd_g2verbose(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String a = ca.arg(0); a.toLowerCase();
  if (a == "on")          gG2ScanVerbose = true;
  else if (a == "off")    gG2ScanVerbose = false;
  else if (a == "toggle") gG2ScanVerbose = !gG2ScanVerbose;
  return gG2ScanVerbose ? "G2 scan-verbose: on" : "G2 scan-verbose: off";
}

// Fake a Blocks menu tap to exercise the hijack path without needing to
// physically reach into the G2 side menu for every test. Invokes the
// same handleMenuStartUp() the real cmd=17 frame would — so if this CLI
// command shows the snapshot, the wire path will too.
static const char* cmd_g2hijacktest(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gR.connected) return "G2: right temple not connected";
  handleMenuStartUp(gR, BLOCKS_WIDGET_ID);
  return gHijackActive ? "G2 hijack: fired (status page shown)"
                       : "G2 hijack: fire attempted — check logs";
}

// Manual recovery for the "hijack ended abnormally and won't re-launch"
// case (firmware-side state stuck, lens dark, tapping Blocks in the
// glasses menu does nothing because the firmware thinks the app is
// still running). Force-clears the local hijack flag and runs the same
// Cmd=18 + CREATE handshake as the real cmd=17 path. Cmd=18 is harmless
// when no launch is pending — it's the same fire-and-forget cancel —
// so this is safe to invoke even when the hijack ended cleanly and
// nothing's stuck.
//
// This is what the web UI's "Re-open hijack" button calls. Won't help
// if the plugin task is dead (would need a reconnect) or the right
// temple is disconnected; both states are surfaced in the return
// string.
static const char* cmd_g2reopen(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gR.connected) return "G2: right temple not connected — reconnect first";
  if (gR.pluginDead) return "G2: plugin task silent — reconnect to recover";
  handleMenuStartUp(gR, BLOCKS_WIDGET_ID);
  return "G2: hijack re-launch dispatched (watch logs for CREATE ack)";
}

// Print the last ~32 G2 envelopes (TX + RX) to the log. Handy for
// forensics after the glasses bail with an on-lens "connection issue" —
// the frames right before the BLE disconnect tell us which outgoing op
// the firmware took issue with. Also dumps automatically on: BLE
// disconnect, plugin-dead transition, any EvenCore res != 0 response.
static const char* cmd_g2dumpframes(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  g2RingDump("manual dump");
  return "G2: frame ring dumped to log";
}

static const char* cmd_g2bmp(const String& argsInput);

// `extern` on both this table and its count below because System_Utils.cpp
// refers to them as `extern const ...` — without it, C++ gives file-scope
// const objects internal linkage and the link fails.
extern const CommandEntry g2Commands[] = {
  { "openg2",       "Connect to G2 glasses: openg2 [left|right|auto]", false, cmd_g2connect },
  { "closeg2",      "Disconnect from G2 glasses",                      false, cmd_g2disconnect },
  { "g2status",     "Show G2 connection status",                       false, cmd_g2status },
  { "g2info",       "Dump device info (firmware, MAC, battery, etc.)",  false, cmd_g2info },
  { "g2settings",   "Settings debug: g2settings verbose [on|off]",     false, cmd_g2settings },
  { "g2liverate",   "Get/set live-update probe cadence (ms): g2liverate [N]", false, cmd_g2liverate },
  { "g2listrebuild","REBUILD-list fast path on page-swap [on|off] (default ON; no-flicker nav, selector resets to row 0)", false, cmd_g2listrebuild },
  { "g2show",       "Display text: g2show <text>",                     false, cmd_g2show },
  { "g2ai",         "Front-pane AI card (full pipeline): g2ai <text>", false, cmd_g2ai },
  { "g2ai-noask",   "Variant: skip ASK step: g2ai-noask <text>",       false, cmd_g2ai_noask },
  { "g2ai-direct",  "Variant: CTRL+REPLY only: g2ai-direct <text>",    false, cmd_g2ai_direct },
  { "g2aih",        "Front-pane card with custom heading: g2aih <heading>|<body>", false, cmd_g2aih },
  { "g2aiconfig",   "Probe EvenAI CONFIG (cmd=10): g2aiconfig [voiceSwitch] [streamSpeed], use - to omit",  false, cmd_g2aiconfig },
  { "g2imgprobe",   "Probe Cmd=3 multi-frag wire path: g2imgprobe [size_bytes]",                            false, cmd_g2imgprobe },
  { "g2protostats", "Show G2 protocol stats per sid: g2protostats [verbose]",      false, cmd_g2protostats },
  { "g2probe",      "Fire arbitrary pb cmd: g2probe <sid_hex> <cmd_dec> [body_hex]", false, cmd_g2probe },
  { "g2notify",     "Transient text (placeholder): g2notify [secs] <text>", false, cmd_g2notify },
  { "g2bmp",        "Display BMP: g2bmp </path.bmp> [brightness -100..100] [contrast -100..100] [holdSeconds 0..120]", false, cmd_g2bmp },
  { "g2sensors",    "Show device's sensor list on the G2 lens",        false, cmd_g2sensors },
  { "g2system",     "Show System info page on the G2 lens",            false, cmd_g2system },
  { "g2network",    "Show Network info page on the G2 lens",           false, cmd_g2network },
  { "g2settingspage","Show Settings inspector page on the G2 lens",    false, cmd_g2settingspage },
  { "g2files",      "Show Files browser page on the G2 lens",          false, cmd_g2files },
  { "g2clear",      "Clear G2 display",                                false, cmd_g2clear },
  { "g2scan",       "Scan for G2 glasses",                             false, cmd_g2scan },
  { "g2init",       "Initialize G2 client mode",                       false, cmd_g2init },
  { "g2deinit",     "Deinitialize G2 client mode",                     false, cmd_g2deinit },
  { "g2nav",        "Menu navigation mode: g2nav [on|off|toggle] (bare = report state)", false, cmd_g2nav },
  { "g2battery",    "Query G2 battery % on connected temples",         false, cmd_g2battery },
  { "g2mic",        "Enable/disable G2 mic capture: g2mic <on|off>",   false, cmd_g2mic },
  { "g2verbose",    "Scan-verbose logging: g2verbose [on|off|toggle] (bare = report state)", false, cmd_g2verbose },
  { "g2hijacktest", "Simulate a Blocks tap (status-page hijack)",      false, cmd_g2hijacktest },
  { "g2reopen",     "Re-open the hijacked Blocks app after an abnormal exit", false, cmd_g2reopen },
  { "g2dumpframes", "Print the recent G2 envelope ring buffer",        false, cmd_g2dumpframes },
};
extern const size_t g2CommandsCount = sizeof(g2Commands) / sizeof(g2Commands[0]);

// =============================================================================
// Image-streaming discovery probes
// =============================================================================
// See header for goals. Each probe builds candidate pb shapes and ships
// them via the existing fragmenting transport so the response stream
// (decoded by handleEnvelope/decodeEvenCoreResp) tells us whether the
// firmware liked the body shape, gave up at the body decoder, or
// rejected for a higher-layer reason (no container, oversize, etc.).

// Fill `pat` with an alternating 0xF0/0x0F byte pattern. Visually that's
// vertical stripes if 4-bpp packing matches the documented "high nibble
// first" — and it's a non-zero, non-repeating-byte stream that won't be
// optimised away by any zero-padding reassembly trick. Caller owns
// allocation.
static void fillStripePattern(uint8_t* pat, size_t n) {
  for (size_t i = 0; i < n; i++) pat[i] = (i & 1) ? 0x0F : 0xF0;
}

// Build a minimal 4-bpp grayscale BMP file in `out`. Returns total bytes
// written, or 0 if outCap is too small.
//
// Why BMP and not raw 4-bpp pixels: empirical claim from the
// g2-kit-unofficial Discord (jimrandomh, 2026-04-25) is that the G2
// firmware does format detection on `MapRawData` and only accepts data
// starting with the "BM" file-header magic — i.e., the contents of a
// real BMP file, not a raw nibble stream. The doc's earlier "4-bpp
// indexed, high-nibble-first" description was about the BMP encoding,
// not a raw format. CompressMode=0 confirmed (~100 values tried by
// jimrandomh, none had effect).
//
// `width` / `height` are signed: positive height means bottom-up
// (BMP default); negative means top-down (data starts at top row),
// which is more intuitive for our test patterns. Palette is fixed at
// 16 grayscale entries (index 0=black, 15=white, evenly spaced).
typedef enum {
  BMP_PAT_STRIPES,    // 0x0F / 0xF0 alternating bytes (vertical stripes)
  BMP_PAT_ALL_BLACK,  // all-zero pixels (used to test the "all-black
                      // ack faster" claim from jimrandomh)
} BmpPattern;

static size_t buildBmp4bpp(uint8_t* out, size_t outCap,
                           int32_t width, int32_t height,
                           BmpPattern pattern) {
  if (!out || outCap == 0) return 0;
  const uint32_t aw = (uint32_t)(width  < 0 ? -width  : width);
  const uint32_t ah = (uint32_t)(height < 0 ? -height : height);
  if (aw == 0 || ah == 0) return 0;
  // 4-bpp = 2 px/byte. Row stride is 4-byte aligned per BMP spec.
  const uint32_t rowStride = ((aw * 4 + 31) / 32) * 4;
  const uint32_t pixelSize = rowStride * ah;
  const uint32_t headerSize = 14 + 40 + 64;  // file + DIB + palette
  const uint32_t total = headerSize + pixelSize;
  if (total > outCap) return 0;

  // Helpers to write little-endian fields.
  auto wr16 = [](uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
  };
  auto wr32 = [](uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);          p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);  p[3] = (uint8_t)((v >> 24) & 0xff);
  };

  // ── BITMAPFILEHEADER (14 B) ──
  out[0] = 'B'; out[1] = 'M';
  wr32(out + 2,  total);          // bfSize
  wr16(out + 6,  0);              // bfReserved1
  wr16(out + 8,  0);              // bfReserved2
  wr32(out + 10, headerSize);     // bfOffBits

  // ── BITMAPINFOHEADER (40 B) ──
  wr32(out + 14, 40);             // biSize
  wr32(out + 18, (uint32_t)width);  // biWidth (signed bit-pattern)
  wr32(out + 22, (uint32_t)height); // biHeight (sign matters: <0 = top-down)
  wr16(out + 26, 1);              // biPlanes
  wr16(out + 28, 4);              // biBitCount = 4-bpp
  wr32(out + 30, 0);              // biCompression = BI_RGB
  wr32(out + 34, pixelSize);      // biSizeImage
  wr32(out + 38, 2835);           // biXPelsPerMeter (~72 DPI)
  wr32(out + 42, 2835);           // biYPelsPerMeter
  wr32(out + 46, 16);             // biClrUsed
  wr32(out + 50, 0);              // biClrImportant

  // ── Palette (16 × BGRA = 64 B) ──
  // 16 grayscale shades, evenly spaced 0..255.
  for (int i = 0; i < 16; i++) {
    const uint8_t v = (uint8_t)((i * 255) / 15);
    out[54 + i*4 + 0] = v;  // B
    out[54 + i*4 + 1] = v;  // G
    out[54 + i*4 + 2] = v;  // R
    out[54 + i*4 + 3] = 0;  // reserved
  }

  // ── Pixel data ──
  uint8_t* pixels = out + headerSize;
  if (pattern == BMP_PAT_ALL_BLACK) {
    memset(pixels, 0, pixelSize);
  } else {
    for (uint32_t i = 0; i < pixelSize; i++) {
      pixels[i] = (i & 1) ? 0x0F : 0xF0;
    }
  }
  return total;
}

// Pretty-print up to `cap` bytes of pb body to the debug log as hex
// pairs. Probes call this so each TX is grep-able alongside its
// response in the next log section.
static void logPbHex(const char* tag, const uint8_t* pb, size_t pbLen) {
  // 32 bytes is plenty to identify which candidate fired without
  // flooding the ring buffer when payloads are kilobytes.
  const size_t cap = pbLen < 32 ? pbLen : 32;
  char hex[3 * 32 + 1];
  size_t hp = 0;
  for (size_t i = 0; i < cap; i++) {
    hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X ", pb[i]);
  }
  if (cap < pbLen) snprintf(hex + hp, sizeof(hex) - hp, "...");
  DEBUG_G2F("[ImgProbe] %s pbLen=%u head=[%s]", tag, (unsigned)pbLen, hex);
}

// Tear down the active hijack container before a probe burst.
//
// Why this exists: the test-suite picker that hosts the probe entries is
// itself a LIST container on widget=10509. Sending Cmd=3 (ImageRaw) at
// that container hangs the firmware's plugin task — same crash class as
// the documented "UPDATE_TEXT against a list container" bug
// (G2_PROTOCOL.md). Symptom: TX goes out, no Cmd=4 ack, "Connection
// lost" appears on lens, hijack drops.
//
// Mitigation: shut the picker down before firing probe candidates so
// the firmware sees Cmd=3 / Cmd=0 against NO container — those reject
// cleanly with `ImgRawFailed(5)` or `InvalidContainer(1)`, no crash.
//
// Mechanics mirror sendPageSwapShutdown in the page-swap worker:
//   1) stamp gOurShutdownAtMs so the SYSTEM_EXIT echo handler ignores
//      the firmware's downstream events (otherwise the hijack manager
//      would interpret them as a user-initiated exit and tear down)
//   2) send Cmd=9 SHUTDOWN_PAGE
//   3) settle 500 ms — empirical floor; shorter has been observed to
//      race the next CREATE
//   4) clear local container state so the test-suite dispatcher's
//      follow-up g2ShowListPage skips its own (now-redundant) shutdown
//      and goes straight to CREATE when rebuilding the picker
// ─────────────────────────────────────────────────────────────────────
// Image-pipeline timing constants. Centralised so a single edit
// rebalances every image probe + g2bmp + future pushTile().
// Values cross-checked against jimrandomh's faceclaw and the
// g2-kit-unofficial reference (2026-04-27).
// ─────────────────────────────────────────────────────────────────────

// CREATE-image ack wait. Faceclaw calls this "warmup timeout"; their
// 2 s matches our empirical sweet-spot. The firmware acks within ~60 ms
// in healthy runs, so 2 s is generous BUT short enough that a silent-
// drop (e.g. magic > 255 — see G2_PROTOCOL.md uint8 constraint) is
// caught before the operator gives up.
static constexpr uint32_t kImgCreateAckTimeoutMs = 2000;

// Per-image push-ack completion wait. Faceclaw uses 3.5 s for the full
// fragment-set ack; we match. A 7-fragment Q6 burst sees its last ack
// arrive ~150 ms after the last write; 3.5 s gives ~20× headroom for
// firmware queue jitter. Larger images (Q10's 21 frags) need ~600 ms
// of acks at our pacing — still well within budget.
static constexpr uint32_t kImgPushAckTimeoutMs = 3500;

// Inter-fragment gap during a Cmd=3 burst. 100 ms paces the BT
// controller well enough that our rc=-1 retry handles the rare
// transient. Faceclaw uses smaller fragments (1000 B) and runs
// faster; revisit if we ever drop our 3 KB chunk size.
static constexpr uint32_t kImgFragGapMs = 100;

// Hold the image visible after the last fragment, before SHUTDOWN.
// The lens's 4-tile sync settles for ~2 s post-last-frag (per the
// Discord throughput-floor claim) so 3 s gives the operator a clean
// look before tear-down. Probes that use tap-to-dismiss override
// this with probeHoldUntilTapOrTimeout(60000).
static constexpr uint32_t kImgPostFragHoldMs = 3000;

// Settle window after a SHUTDOWN before the next pipeline step. The
// firmware fires SYSTEM_EXIT(7) ~300 ms after our shutdown; 500 ms
// keeps the log readable and gives the page-swap worker time to
// resync containerReady state.
static constexpr uint32_t kImgShutdownSettleMs = 500;

// Firmware reassembly window: max number of Cmd=3 push fragments we
// can have in flight (sent but unacked) at once. Faceclaw and
// g2-kit-unofficial both establish 3 as the hard ceiling — sending a
// 4th unacked fragment risks silent drops on the firmware side. With
// our 100 ms inter-fragment pacing the in-flight count usually peaks
// at 1 (acks arrive within ~150 ms), so this cap is a safety net for
// the case where acks lag and we'd otherwise lap the window.
static constexpr unsigned kImgInFlightCap = 3;

static bool probeTearDownActiveContainer(G2Temple& arm) {
  if (!arm.containerReady) {
    DEBUG_G2F("[ImgProbe] tear-down: no live container, skipping shutdown");
    return true;
  }
  gOurShutdownAtMs = millis();
  uint8_t buf[32];
  size_t n = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN,
                             /*exitMode*/ 0, buf, sizeof(buf));
  if (n == 0 || !sendEnvelope(arm, buf, n)) {
    DEBUG_G2F("[ImgProbe] tear-down: SHUTDOWN send failed");
    return false;
  }
  DEBUG_G2F("[ImgProbe] tear-down: SHUTDOWN sent, settling %u ms",
            (unsigned)kImgShutdownSettleMs);
  vTaskDelay(pdMS_TO_TICKS(kImgShutdownSettleMs));
  arm.containerReady  = false;
  arm.containerIsList = false;
  g2LensClearContainer();
  return true;
}

// Synchronously wait for the firmware's CreateResp on a CREATE-image
// probe. Reuses the existing gExpectMagic / gCreateOk / gCreateAckSem
// infrastructure that g2ShowText already uses for list/text CREATEs —
// the response handler (handleEnvelope) signals the same semaphore for
// any cmd=1 whose magic matches gExpectMagic, regardless of container
// type. Caller MUST call this PAIR around the CREATE-image TX:
//
//   probePrepImageCreateAck(magic);
//   sendEnvelope(*arm, createBuf, createLen);
//   if (!probeWaitImageCreateAck(magic, 2000)) {
//     // CREATE rejected or silently dropped — abort the probe before
//     // pushing pixel fragments into a non-existent container.
//     return ...;
//   }
//
// Why we need this: empirical 2026-04-26 observed Q6 (multi-fragment
// BMP push) silently drop the CREATE-image and proceed to ship 7×14=98
// envelope fragments into a void. The firmware likely couldn't service
// its response queue while reassembling our fragment flood, so the
// CreateResp never arrived. Fragments went to a non-existent container
// and the user saw nothing render. Sync-waiting on the ack stops the
// fragment burst dead at the symptom and tells us whether the CREATE
// itself is the issue or the post-CREATE pipeline.
static void probePrepImageCreateAck(uint32_t magic) {
  gExpectMagic = (uint8_t)magic;  // handler matches low byte of magic
  gCreateOk = false;
  if (gCreateAckSem) xSemaphoreTake(gCreateAckSem, 0);  // drain stale
}

// Arm the push-ack counter. magicLo and magicHi are the inclusive
// low-byte range of MagicRandom values we expect to see acked by
// Cmd=4 ImageRawResp. The handler increments on every match and
// signals the sem when count == target. Drain any stale signal first.
static void probePrepImagePushAcks(uint32_t magicLo, uint32_t magicHi,
                                    unsigned target) {
  gImgPushExpectLo = (uint8_t)magicLo;
  gImgPushExpectHi = (uint8_t)magicHi;
  gImgPushTarget   = target;
  gImgPushAcked    = 0;
  if (gImgPushAckSem) xSemaphoreTake(gImgPushAckSem, 0);
}

// Block up to timeoutMs for the registered push-ack count to reach
// target. Returns true if every ack arrived in time, false on timeout.
// On false the caller should treat the image as not-rendered (firmware
// either dropped fragments or got stuck mid-render). Always disarms
// the counter on exit so a late ack doesn't double-fire next probe.
static bool probeWaitImagePushAcks(uint32_t timeoutMs,
                                    unsigned* outAcked,
                                    unsigned* outTarget) {
  if (outTarget) *outTarget = gImgPushTarget;
  if (!gImgPushAckSem || gImgPushTarget == 0) {
    if (outAcked) *outAcked = gImgPushAcked;
    gImgPushTarget = 0;  // disarm
    return false;
  }
  const bool ok =
      (xSemaphoreTake(gImgPushAckSem, pdMS_TO_TICKS(timeoutMs)) == pdTRUE);
  if (outAcked) *outAcked = gImgPushAcked;
  if (!ok) {
    DEBUG_G2F("[ImgProbe] push-ack TIMEOUT after %u ms — got %u/%u "
              "(magic range %u..%u). Image may not have rendered.",
              (unsigned)timeoutMs, (unsigned)gImgPushAcked,
              (unsigned)gImgPushTarget,
              (unsigned)gImgPushExpectLo, (unsigned)gImgPushExpectHi);
  }
  gImgPushTarget = 0;  // disarm; late acks are now ignored
  return ok;
}

static bool probeWaitImageCreateAck(uint32_t magic, uint32_t timeoutMs) {
  if (!gCreateAckSem) {
    DEBUG_G2F("[ImgProbe] CreateResp wait: ack sem not initialised");
    return false;
  }
  if (xSemaphoreTake(gCreateAckSem, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    DEBUG_G2F("[ImgProbe] CreateResp TIMEOUT after %u ms (magic=%u) — "
              "firmware silently dropped or didn't service in time",
              (unsigned)timeoutMs, (unsigned)magic);
    return false;
  }
  if (!gCreateOk) {
    DEBUG_G2F("[ImgProbe] CreateResp arrived but res!=0 (magic=%u) — "
              "firmware rejected the CREATE", (unsigned)magic);
    return false;
  }
  DEBUG_G2F("[ImgProbe] CreateResp OK (magic=%u, res=0 CreateSuccess)",
            (unsigned)magic);
  return true;
}

// Cleanup after a CREATE-image probe. Verified empirically 2026-04-26:
// a successful CREATE-image leaves a live image container at the
// hijack widget (10509). The picker-rebuild that follows the probe
// then tries to CREATE-list at the same widgetId without first
// SHUTDOWN-ing — because our local `arm.containerReady` is still
// false (we cleared it pre-probe and the response handler doesn't
// flip it back for image-shaped CreateResps). The firmware refuses to
// replace the image with a list and the rebuild times out, leaving
// the lens stuck until the operator reboots the glasses.
//
// Fix: explicitly SHUTDOWN at the end of each CREATE-image probe.
// SHUTDOWN against a no-longer-existing container is a benign no-op
// (firmware returns ShutdownFailed res=11), so calling this even
// after a silently-dropped CREATE doesn't make anything worse. Then
// clear local state so the page-swap worker correctly skips its own
// (now-redundant) shutdown when rebuilding the picker.
static void probePostProbeShutdown(G2Temple& arm) {
  gOurShutdownAtMs = millis();
  uint8_t buf[32];
  size_t n = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN, 0, buf, sizeof(buf));
  if (n != 0 && sendEnvelope(arm, buf, n)) {
    DEBUG_G2F("[ImgProbe] post-probe: SHUTDOWN sent, settling %u ms "
              "(clears any container the probe may have created)",
              (unsigned)kImgShutdownSettleMs);
    vTaskDelay(pdMS_TO_TICKS(kImgShutdownSettleMs));
  } else {
    DEBUG_G2F("[ImgProbe] post-probe: SHUTDOWN send failed (proceeding anyway)");
  }
  arm.containerReady  = false;
  arm.containerIsList = false;
  g2LensClearContainer();
}

// Pretty banner — gives a visual divider in the log so the operator can
// scroll up to a probe's "begin" marker and read forward to its "done".
// The "what to grep for" lines name the magic range each probe uses; the
// firmware echoes our magic in every response, so grep on `magic=NNN` to
// pull out the matching ack from the rest of the BLE chatter.
static void probeBanner(const char* probe, uint32_t magicLo, uint32_t magicHi,
                         const char* expect) {
  DEBUG_G2F("[ImgProbe] ───────── %s ─────────", probe);
  DEBUG_G2F("[ImgProbe] match RX magic=%u..%u in 'cmd=4' (Cmd=3 acks) and 'cmd=1/8' (CREATE/REBUILD acks)",
            (unsigned)magicLo, (unsigned)magicHi);
  DEBUG_G2F("[ImgProbe] expect: %s", expect);
  DEBUG_G2F("[ImgProbe] decode hint: in 'RX notify ... head=[AA 12 .. 08 CC 10 MM ...]'");
  DEBUG_G2F("[ImgProbe]   CC=01 CreateResp, CC=04 ImageRawResp, CC=08 RebuildResp;");
  DEBUG_G2F("[ImgProbe]   MM=our magic (low byte). Bytes after MM = response body —");
  DEBUG_G2F("[ImgProbe]   look for varint after a tag byte; that IS the error code:");
  DEBUG_G2F("[ImgProbe]   0=CreateOK 1=InvalidContainer 2=Oversize 3=OOM 5=ImgRawFail");
  DEBUG_G2F("[ImgProbe]   6=RebuildOK 7=RebuildFailed.");
}

static void probeFooter(const char* probe, unsigned okCount, unsigned total) {
  DEBUG_G2F("[ImgProbe] %s done — %u/%u candidates shipped", probe, okCount, total);
  DEBUG_G2F("[ImgProbe] ─────────────────────────────────────");
}

// Q4 — CREATE-image with full-tile dimensions and a fresh CID. NO
// follow-on Cmd=3 burst. Why: empirical test on 2026-04-26 showed that
// a CREATE-image with 32×12 dims + CID=1 was rejected with
// `CreateResp res=1 CreateInvalidContainer`, which the firmware
// followed with `ABNORMAL_EXIT(6)` + `SYSTEM_EXIT(7) reason=0`,
// killing the hijacked Blocks widget's plugin task. The follow-on
// Cmd=3 frames went into a dead pipe, the picker rebuild timed out,
// BLE disconnected — operator hard-restart required. Lesson: a failed
// CREATE-image kills the entire hijack context. So Q4 now does ONLY
// the CREATE step with safer parameters (full-tile dimensions per the
// doc's stated minimum, CID=2 to avoid conflict with the picker list
// at CID=1) so we can isolate which parameter the firmware objects to
// without cascading into a disconnect.
//
// If THIS Q4 also returns CreateInvalidContainer, the issue is the
// Blocks widget's slot-type lock (image containers may only be
// accepted by a different widget context entirely) — at which point
// we need a different reverse-engineering angle (HCI snoop the phone
// app's image push to see what widgetId/Cmd=17 sequence it uses).
const char* g2ProbeImageQ4Lifecycle() {
  static char ret[200];

  G2Temple* arm = pickEvenAIArm("imgQ4");
  if (!arm) return "Img Q4: no reachable temple";

  const uint32_t kCreateMagic = G2_MAGIC_IMAGE_BASE + 0x20;
  const uint32_t kImageW      = 288;
  const uint32_t kImageH      = 144;   // full single tile (per doc)
  const uint32_t kCID         = 2;     // distinct from picker list/text

  probeBanner("Q4: CREATE-image (full tile, CID=2)",
              kCreateMagic, kCreateMagic,
              "expect Cmd=1 CreateResp magic=290 ErrorCode=0 (CreateSuccess). "
              "Differs from Q4b only in CID (Q4=2, Q4b=1). If Q4 succeeds "
              "and Q4b doesn't, the picker's CID=1 list residue is "
              "blocking image re-use. If both fail with res=1, the "
              "Blocks widget slot rejects image containers entirely — "
              "next step is HCI-snooping the phone app's image flow "
              "to find an image-capable widget context.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q4: pre-burst SHUTDOWN failed";

  uint8_t buf[128];
  size_t envLen = g2BuildCreateImage(
      allocSeq(), kCreateMagic,
      /*containerName*/ "imgQ4", /*containerId*/ kCID,
      /*x*/ 0, /*y*/ 0, /*w*/ kImageW, /*h*/ kImageH,
      /*widgetId*/ BLOCKS_WIDGET_ID,
      buf, sizeof(buf));
  if (envLen == 0) return "Img Q4: CREATE-image build failed";

  gOurShutdownAtMs = millis();
  DEBUG_G2F("[ImgProbe] Q4 CREATE-image magic=%u (%u×%u @ 0,0, CID=%u, "
            "name='imgQ4', widgetId=%u)",
            (unsigned)kCreateMagic,
            (unsigned)kImageW, (unsigned)kImageH,
            (unsigned)kCID, (unsigned)BLOCKS_WIDGET_ID);
  logPbHex("Q4 env", buf, envLen);

  bool ok = sendEnvelope(*arm, buf, envLen);
  if (!ok) DEBUG_G2F("[ImgProbe] Q4 — TX failed");

  // Hold visible 2 s so an accepted CREATE has time to render its empty
  // rectangle on lens before we tear it down.
  vTaskDelay(pdMS_TO_TICKS(2000));

  // Clean up the image container before the worker rebuilds the picker.
  // Otherwise CREATE-list on the same widgetId fails (image still live).
  probePostProbeShutdown(*arm);

  probeFooter("Q4", ok ? 1 : 0, 1);
  snprintf(ret, sizeof(ret),
           "Img Q4: %s — watch RX cmd=1 magic=%u ErrorCode (0=success)",
           ok ? "1/1 sent" : "TX failed", (unsigned)kCreateMagic);
  return ret;
}

const char* g2ProbeImageDocSummary() {
  // No TX — dumps the verified image-protocol schema and the now-known
  // facts so the operator can read them next to a probe's response in
  // the same log stream. Schema fields confirmed against
  // g2-kit-unofficial/ble/gen/EvenHub_pb.ts (verified 2026-04-26).
  DEBUG_G2F("[ImgProbe] === Image protocol — verified schema ===");
  DEBUG_G2F("[ImgProbe]   Cmd=0 CREATE_STARTUP carries CreateStartUpPageContainer{");
  DEBUG_G2F("[ImgProbe]     f1=ContainerTotalNum, f2=ListObject, f3=TextObject,");
  DEBUG_G2F("[ImgProbe]     f4=ImageObject, f5=widgetId }");
  DEBUG_G2F("[ImgProbe]   ImageContainerProperty (inside f4) {");
  DEBUG_G2F("[ImgProbe]     f1=XPosition, f2=YPosition, f3=Width, f4=Height,");
  DEBUG_G2F("[ImgProbe]     f5=ContainerID, f6=ContainerName }");
  DEBUG_G2F("[ImgProbe]     — NO Format/bpp field; 4-bpp indexed is implicit.");
  DEBUG_G2F("[ImgProbe]   Cmd=3 ImgRawMsg (wrapper f5) ImageRawDataUpdate {");
  DEBUG_G2F("[ImgProbe]     f1=ContainerID, f2=ContainerName, f3=MapSessionId,");
  DEBUG_G2F("[ImgProbe]     f4=MapTotalSize, f5=CompressMode (0=raw),");
  DEBUG_G2F("[ImgProbe]     f6=MapFragmentIndex, f7=MapFragmentPacketSize,");
  DEBUG_G2F("[ImgProbe]     f8=MapRawData (bytes) }");
  DEBUG_G2F("[ImgProbe]   Cmd=4 ImgResCmd (wrapper f6 — NOT f5)");
  DEBUG_G2F("[ImgProbe]     ResponseImageRawDataCmd { f1..f7 echo of request,");
  DEBUG_G2F("[ImgProbe]     f8=ErrorCode (varint, EvenHub_ErrorCode_List) }.");
  DEBUG_G2F("[ImgProbe]     NOTE: image responses don't use ResCmdMsg — read");
  DEBUG_G2F("[ImgProbe]     ErrorCode at inner f8 directly.");
  DEBUG_G2F("[ImgProbe]   Pixel format: 4-bpp indexed, 16-entry palette,");
  DEBUG_G2F("[ImgProbe]     2 px/byte, HIGH NIBBLE FIRST, row-major top-down.");
  DEBUG_G2F("[ImgProbe]   Full lens 576x288 = 2x2 grid of 288x144 tiles.");
  DEBUG_G2F("[ImgProbe]   Per-Cmd=3 pb body <=4 KB (firmware reassembly cap).");
  DEBUG_G2F("[ImgProbe]   Image-layer fragmentation via MapSessionId/Index/Total");
  DEBUG_G2F("[ImgProbe]     is SEPARATE from envelope-layer fragmentation.");
  DEBUG_G2F("[ImgProbe]   *** MapRawData MUST be a real BMP file (starts with 'BM').");
  DEBUG_G2F("[ImgProbe]     The firmware does format detection — only BMP accepted.");
  DEBUG_G2F("[ImgProbe]     4-bpp indexed grayscale, BGRA palette, top- or bottom-up.");
  DEBUG_G2F("[ImgProbe]     [Source: jimrandomh @ G2/R1 dev Discord, 2026-04-25.]");
  DEBUG_G2F("[ImgProbe]   CompressMode=0 only — ~100 values probed, all no-op.");
  DEBUG_G2F("[ImgProbe]   Length-mismatch trick: declared len > actual data => firmware");
  DEBUG_G2F("[ImgProbe]     zero-pads up to declared. RLE-of-zeros for tile boundaries.");
  DEBUG_G2F("[ImgProbe]   App-layer ack disabled by setting magic=0 in the wrapper.");
  DEBUG_G2F("[ImgProbe]     WARNING: combining magic=0 with rapid sends overflows the");
  DEBUG_G2F("[ImgProbe]     headset buffer and DROPS the BLE connection.");
  DEBUG_G2F("[ImgProbe]   First image after CREATE silently dropped (warmup needed).");
  DEBUG_G2F("[ImgProbe]   Observed ceiling ~2.5 s for full 4-tile update — firmware");
  DEBUG_G2F("[ImgProbe]     bottleneck is the post-fragment ack delay (lens sync).");
  DEBUG_G2F("[ImgProbe]   All-black images ack faster than non-zero patterns.");
  DEBUG_G2F("[ImgProbe]   4-tile sync trick: send N-1 frags of each of 4 tiles,");
  DEBUG_G2F("[ImgProbe]     then send the final frag of each — synchronises the flip.");
  DEBUG_G2F("[ImgProbe]   Error codes (EvenHub_ErrorCode_List):");
  DEBUG_G2F("[ImgProbe]     0=CreateSuccess 1=InvalidContainer 2=Oversize 3=OOM");
  DEBUG_G2F("[ImgProbe]     4=ImgRawSuccess 5=ImgRawFailed 6=RebuildSuccess");
  DEBUG_G2F("[ImgProbe]     7=RebuildFailed 8=TextSuccess 9=TextFailed");
  DEBUG_G2F("[ImgProbe]     10=ShutdownSuccess 11=ShutdownFailed.");
  return "Img Doc: verified schema dumped to log";
}

// ─────────────────────────────────────────────────────────────────────
// Pixel-push probes (Q5/Q6/Q7/Q8) — shared helper.
// ─────────────────────────────────────────────────────────────────────
// All four probes do the same scaffolding: SHUTDOWN picker → CREATE-image
// using Q4's empirically-proven-good shape (CID=2, name="imgQ4",
// 288×144, widget=10509) → wait for CreateResp → push a Cmd=3 BMP
// → hold visible → SHUTDOWN cleanup. Only the BMP differs between
// probes (size, pattern, biHeight sign). Centralising the
// scaffolding keeps each probe at ~30 lines of intent-only code and
// guarantees they all use byte-identical CREATE-image bytes — so any
// difference in outcome is attributable to the BMP, not the
// container.
//
// Returns:  "" on success, or a short error string suitable for
// returning from the probe entry point.
// Block the calling worker until either (a) the user double-taps the
// ring or temple (the SysEvent handler sets gImgProbeHoldTapPending),
// or (b) maxMs elapses as a safety cap so a forgotten image doesn't
// lock up the picker. Returns true if a tap arrived, false if the
// timeout fired.
//
// Confirmed 2026-04-27 (firmware 2.2.0.24): during a pure image-up
// state (no list/text widget), the firmware only emits SysEvent
// DOUBLE_CLICK(3); single CLICK / SCROLL events do NOT fire. The
// hook below still accepts CLICK/SCROLL/DOUBLE_CLICK in case future
// firmware revisions emit them, but in practice **only double-tap
// dismisses**. Update probe banners accordingly.
static bool probeHoldUntilTapOrTimeout(uint32_t maxMs) {
  gImgProbeHoldTapPending = false;
  gImgProbeHoldActive     = true;
  const uint32_t startMs = millis();
  while ((millis() - startMs) < maxMs) {
    if (gImgProbeHoldTapPending) {
      gImgProbeHoldActive = false;
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  gImgProbeHoldActive = false;
  return false;
}

// Shared image transport helper for any prebuilt BMP payload.
// Sends CREATE-image with Q4-known-good shape, then multi-fragment Cmd=3
// payload chunks against a single image session.
static bool sendImageBmpMultiFragment(G2Temple& arm,
                                      const char* tag,
                                      uint32_t createMagic,
                                      uint32_t pushMagicBase,
                                      const uint8_t* bmp,
                                      size_t bmpLen,
                                      unsigned* outOkFrags,
                                      unsigned* outTotalFrags) {
  if (outOkFrags) *outOkFrags = 0;
  if (outTotalFrags) *outTotalFrags = 0;
  if (!bmp || bmpLen == 0) return false;

  const uint32_t kCID        = 2;
  const char*    kCName      = "imgQ4";
  const int32_t  kImgW       = 288;
  const int32_t  kImgH       = 144;
  const size_t   kChunkBytes = 3000;

  const unsigned kFrags = (unsigned)((bmpLen + kChunkBytes - 1) / kChunkBytes);
  if (outTotalFrags) *outTotalFrags = kFrags;

  uint8_t createBuf[128];
  size_t createLen = g2BuildCreateImage(
      allocSeq(), createMagic, kCName, kCID, 0, 0, (uint32_t)kImgW, (uint32_t)kImgH,
      BLOCKS_WIDGET_ID, createBuf, sizeof(createBuf));
  if (createLen == 0) return false;

  gOurShutdownAtMs = millis();
  DEBUG_G2F("[ImgProbe] %s CREATE-image magic=%u (%dx%d CID=%u name='%s')",
            tag, (unsigned)createMagic, kImgW, kImgH, (unsigned)kCID, kCName);
  probePrepImageCreateAck(createMagic);
  if (!sendEnvelope(arm, createBuf, createLen)) return false;
  if (!probeWaitImageCreateAck(createMagic, kImgCreateAckTimeoutMs)) return false;

  // Arm the push-ack window before the burst so any acks that arrive
  // mid-burst are counted. Magics span pushMagicBase..pushMagicBase+kFrags-1
  // — caller is responsible for keeping the whole range ≤ 255 (uint8
  // constraint).
  probePrepImagePushAcks(pushMagicBase, pushMagicBase + kFrags - 1, kFrags);

  uint8_t* pbBuf = (uint8_t*)malloc(kChunkBytes + 96);
  if (!pbBuf) return false;

  unsigned okFrags = 0;
  size_t off = 0;
  const uint32_t burstStartMs = millis();
  bool aborted = false;
  for (unsigned i = 0; i < kFrags && !aborted; i++) {
    // Sliding-window throttle. Hold off if we'd exceed the firmware's
    // reassembly window of kImgInFlightCap unacked fragments.
    // In-flight count = (fragments sent so far) - (acks received).
    // Watchdog: if acks stop entirely we'd block forever, so bail
    // after kImgPushAckTimeoutMs * 2 of no progress.
    const uint32_t throttleStartMs = millis();
    while (i > (gImgPushAcked + kImgInFlightCap - 1u)) {
      if ((millis() - throttleStartMs) > kImgPushAckTimeoutMs * 2) {
        DEBUG_G2F("[ImgProbe] %s in-flight cap stuck at %u acked / %u sent "
                  "— aborting burst at frag %u/%u",
                  tag, (unsigned)gImgPushAcked, i, i + 1, kFrags);
        aborted = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (aborted) break;

    const size_t chunk = (off + kChunkBytes <= bmpLen) ? kChunkBytes : (bmpLen - off);
    const uint32_t magic = pushMagicBase + i;
    const size_t pbLen = g2BuildImageRawBody(
        magic, kCID, kCName,
        /*sessionId*/ 1, /*totalSize*/ (uint32_t)bmpLen,
        /*fragIndex*/ i, bmp + off, chunk,
        pbBuf, kChunkBytes + 96);
    if (pbLen == 0) break;

    gOurShutdownAtMs = millis();
    DEBUG_G2F("[ImgProbe] %s frag %u/%u magic=%u fragIdx=%u packet=%u (offset=%u/%u, in-flight=%u)",
              tag, i + 1, kFrags, (unsigned)magic, i,
              (unsigned)chunk, (unsigned)off, (unsigned)bmpLen,
              (unsigned)(i + 1u - gImgPushAcked));
    if (!sendPbFragmented(arm, allocSeq(), G2_SID_EVEN_CORE,
                          G2_FLAG_REQUEST, pbBuf, pbLen)) {
      break;
    }
    okFrags++;
    off += chunk;
    vTaskDelay(pdMS_TO_TICKS(kImgFragGapMs));
  }

  free(pbBuf);
  if (outOkFrags) *outOkFrags = okFrags;

  // Block for the full ack set with a generous timeout. A successful
  // burst sees the last ack ~150 ms after the last write; 3.5 s gives
  // ~20× headroom for firmware queue jitter. Returns false if any
  // expected ack didn't arrive — that's the canonical "image probably
  // didn't render" signal that callers can act on.
  unsigned ackedCount = 0;
  unsigned ackTarget = 0;
  const bool allAcked = probeWaitImagePushAcks(kImgPushAckTimeoutMs,
                                                &ackedCount, &ackTarget);
  const uint32_t burstMs = millis() - burstStartMs;
  DEBUG_G2F("[ImgProbe] %s push complete: %u/%u sent, %u/%u acked in %u ms (%s)",
            tag, okFrags, kFrags, ackedCount, ackTarget,
            (unsigned)burstMs, allAcked ? "OK" : "ACK TIMEOUT");
  return okFrags == kFrags && allAcked;
}

// Q5/Q7/Q8 REMOVED 2026-04-27. All three pushed sub-tile BMPs (32×32)
// into a 288×144 container, which the firmware acks but doesn't render
// (verified 2026-04-26: only full-tile BMPs trigger redraw). Q6 covers
// the working path; sub-tile rendering is documented as not supported
// in docs/G2_PROTOCOL.md "Render only fires when BMP dimensions match
// container dimensions."

// ─────────────────────────────────────────────────────────────────────
// Q6 — full-tile (288×144) BMP via multi-fragment Cmd=3 session.
// Container is the same Q4 known-good shape; BMP fills it at native
// container resolution.
// ─────────────────────────────────────────────────────────────────────
// 288×144 4-bpp = 14+40+64+(144*144) = 20854 B, comfortably exceeding
// the ~4 KB per-Cmd=3 reassembly cap. Has to be split into ~3 KB
// chunks and shipped as a sequential image-layer session: same
// MapSessionId, MapFragmentIndex 0..N-1, MapTotalSize=20854 on every
// fragment, sum of MapFragmentPacketSize equals MapTotalSize on the
// last fragment.
//
// Tests:
//   - image-layer fragmentation works (same session, sequential idx)
//   - real-frame throughput (expect ~2.5 s per discord)
//   - all chunks ack with ErrorCode=4, with a cumulative ImgRawSuccess
//     that triggers the actual lens render
//
// Each Cmd=3 has ~50 B of pb overhead on top of MapRawData, so we
// keep MapRawData chunks at 3000 B → Cmd=3 pb body ≈ 3050 B (under
// the 3500 B safety threshold below the 4 KB firmware cap).
const char* g2ProbeImageQ6BmpMultiFragment() {
  static char ret[220];

  G2Temple* arm = pickEvenAIArm("imgQ6");
  if (!arm) return "Img Q6: no reachable temple";

  // CREATE magic must fit in uint8 (≤255) — see G2_PROTOCOL.md. Push
  // base 0x27..0x2D covers up to 7 fragments, all ≤255. (Was 0x50/0x51
  // = 290/291, CREATE overflowed and silent-dropped.)
  const uint32_t kCreateMagic    = G2_MAGIC_IMAGE_BASE + 0x26;  // 248
  const uint32_t kPushMagicBase  = G2_MAGIC_IMAGE_BASE + 0x27;  // 249..255
  // Container parameters — match Q4's known-good shape exactly. CID=2
  // and name="imgQ4" are the values empirically observed to ack
  // (2026-04-26); other CID/name combos silently dropped CREATE.
  const uint32_t kCID            = 2;
  const char*    kCName          = "imgQ4";
  const int32_t  kImgW           = 288;
  const int32_t  kImgH           = 144;
  const size_t   kChunkBytes     = 3000;

  probeBanner("Q6: multi-fragment full-tile BMP (288×144)",
              kCreateMagic, kPushMagicBase + 7,
              "expect Cmd=1 res=0 then a stream of Cmd=4 acks (one per "
              "fragment) all ErrorCode=4. Total ~20.8 KB BMP shipped as "
              "~7 chunks. Throughput ceiling per Discord is ~2.5 s for "
              "a full-tile render.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q6: pre-burst SHUTDOWN failed";

  // Step 1: build the full-tile BMP into the heap. ~21 KB so we can't
  // stack-allocate it.
  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)malloc(kBmpCap);
  if (!bmp) {
    probePostProbeShutdown(*arm);
    return "Img Q6: BMP heap alloc failed";
  }
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(*arm);
    return "Img Q6: BMP build failed";
  }
  const unsigned kFrags = (unsigned)((bmpLen + kChunkBytes - 1) / kChunkBytes);
  DEBUG_G2F("[ImgProbe] Q6 BMP built: %u B (%dx%d 4bpp), splitting into %u chunks of <=%u B",
            (unsigned)bmpLen, kImgW, kImgH, kFrags, (unsigned)kChunkBytes);
  logPbHex("Q6 BMP head", bmp, bmpLen);

  unsigned okFrags = 0;
  (void)sendImageBmpMultiFragment(*arm, "Q6", kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, nullptr);
  free(bmp);

  // Hold visible — the firmware's 4-tile sync delay is on the order of
  // seconds, so render may keep evolving for ~2 s after the last frag.
  vTaskDelay(pdMS_TO_TICKS(kImgPostFragHoldMs));

  probePostProbeShutdown(*arm);

  probeFooter("Q6", okFrags, kFrags);
  snprintf(ret, sizeof(ret),
           "Img Q6: CREATE + %u/%u frags (magic %u then %u..%u) — "
           "watch for cmd=4 stream with ErrorCode=4 across every fragment",
           okFrags, kFrags, (unsigned)kCreateMagic,
           (unsigned)kPushMagicBase, (unsigned)(kPushMagicBase + kFrags - 1));
  return ret;
}

// Q6b — clone of Q6 with double-tap-to-dismiss instead of fixed 3 s
// hold. Same BMP, same transport, same magics shifted; the only
// change is the hold mechanism. Validated 2026-04-27 with firmware
// 2.2.0.24: hold ends on SysEvent DOUBLE_CLICK(3) (single CLICK does
// not fire while a pure image is on-lens) or 60 s safety cap.
const char* g2ProbeImageQ6bBmpTapDismiss() {
  static char ret[220];

  G2Temple* arm = pickEvenAIArm("imgQ6b");
  if (!arm) return "Img Q6b: no reachable temple";

  // Use offsets that don't collide with Q6's 0x26..0x2D range. Q6b
  // CREATE=0x14 (228), push base 0x15 (229..235 for up to 7 frags) —
  // all ≤255 per the uint8 CREATE-magic constraint.
  const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x14;  // 228
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x15;  // 229..235
  const int32_t  kImgW          = 288;
  const int32_t  kImgH          = 144;

  probeBanner("Q6b: full-tile BMP with double-tap-to-dismiss",
              kCreateMagic, kPushMagicBase + 7,
              "same as Q6, but image stays visible until you "
              "DOUBLE-TAP the ring or temple, or the 60 s safety cap "
              "fires. Single tap won't fire SysEvent during image-up "
              "(firmware 2.2.0.24). Watch [G2] IMG probe hold: log "
              "line for dismiss detection.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q6b: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)malloc(kBmpCap);
  if (!bmp) {
    probePostProbeShutdown(*arm);
    return "Img Q6b: BMP heap alloc failed";
  }
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(*arm);
    return "Img Q6b: BMP build failed";
  }

  unsigned okFrags = 0;
  unsigned totalFrags = 0;
  (void)sendImageBmpMultiFragment(*arm, "Q6b", kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, &totalFrags);
  free(bmp);

  // Tap-to-dismiss with 60 s safety cap. If the firmware doesn't fire
  // SysEvents during a pure image-up state, the cap fires and we tear
  // down anyway.
  DEBUG_G2F("[ImgProbe] Q6b — image up, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q6b — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(*arm);
  probeFooter("Q6b", okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img Q6b: %u/%u frags, hold ended via %s",
           okFrags, totalFrags, tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q9 — frame builder API. Builds a full-tile 288×144 BMP from
// composed regions (banner band, body band, footer band) instead of a
// uniform pattern. Demonstrates the shape a real "draw a card on the
// lens" API would take: a writable 4bpp pixel buffer with helpers
// that fill rectangles by palette index. Once Q9 renders cleanly,
// the same bufferDrawRect path can be lifted into a public
// pushTile(arm, bmp288x144) API for feature code.
//
// Layout (palette indices):
//   rows   0..23   index 15 (white)  — header band
//   rows  24..119  stripes pattern   — body band
//   rows 120..143  index 0  (black)  — footer band
// ─────────────────────────────────────────────────────────────────────
static void bmpDrawRect4bpp(uint8_t* pixelRows, int32_t imgW, int32_t imgH,
                            int32_t x, int32_t y, int32_t w, int32_t h,
                            uint8_t paletteIdx) {
  const int32_t absH = imgH < 0 ? -imgH : imgH;
  if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
  if (x + w > imgW || y + h > absH) return;
  // Row stride: 4-bpp packed, 2 px/byte, padded to 4 bytes per row.
  const size_t rawBytesPerRow = (size_t)((imgW + 1) / 2);
  const size_t stride = (rawBytesPerRow + 3) & ~3u;
  const uint8_t pix2 = (uint8_t)((paletteIdx & 0x0F) << 4 | (paletteIdx & 0x0F));
  for (int32_t row = y; row < y + h; row++) {
    uint8_t* rowPtr = pixelRows + (size_t)row * stride;
    int32_t xPx = x;
    while (xPx < x + w) {
      const bool hi = (xPx & 1) == 0;
      const size_t bytePos = (size_t)(xPx / 2);
      if (hi && (xPx + 1) < x + w) {
        rowPtr[bytePos] = pix2;
        xPx += 2;
      } else {
        const uint8_t cur = rowPtr[bytePos];
        if (hi) {
          rowPtr[bytePos] = (uint8_t)((paletteIdx & 0x0F) << 4 | (cur & 0x0F));
        } else {
          rowPtr[bytePos] = (uint8_t)((cur & 0xF0) | (paletteIdx & 0x0F));
        }
        xPx += 1;
      }
    }
  }
}

const char* g2ProbeImageQ9FrameBuilder() {
  static char ret[220];

  G2Temple* arm = pickEvenAIArm("imgQ9");
  if (!arm) return "Img Q9: no reachable temple";

  const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x16;  // 230
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x17;  // 231..237
  const int32_t  kImgW          = 288;
  const int32_t  kImgH          = 144;

  probeBanner("Q9: frame builder (3-band composed BMP)",
              kCreateMagic, kPushMagicBase + 7,
              "builds a 288×144 BMP from rect primitives (white header, "
              "stripe body, black footer). Validates the draw API we'll "
              "use for real feature content.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q9: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)malloc(kBmpCap);
  if (!bmp) {
    probePostProbeShutdown(*arm);
    return "Img Q9: BMP heap alloc failed";
  }
  // Start from the stripes baseline (so the body band has stripes for
  // free), then overlay header/footer rects with bmpDrawRect4bpp.
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(*arm);
    return "Img Q9: BMP build failed";
  }
  // Pixel rows live after the file header (14 B) + DIB header (40 B) +
  // 16-entry palette (16*4=64 B) = offset 118. buildBmp4bpp's bfOffBits
  // field is at offset 10 of the file header, so we could read that;
  // for the known layout we hardcode 118.
  const size_t kPixOffset = 14 + 40 + 16 * 4;
  uint8_t* rows = bmp + kPixOffset;
  bmpDrawRect4bpp(rows, kImgW, -kImgH, /*x*/0, /*y*/0,   /*w*/kImgW, /*h*/24,  /*idx*/15);
  bmpDrawRect4bpp(rows, kImgW, -kImgH, /*x*/0, /*y*/120, /*w*/kImgW, /*h*/24,  /*idx*/0);

  unsigned okFrags = 0;
  unsigned totalFrags = 0;
  (void)sendImageBmpMultiFragment(*arm, "Q9", kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, &totalFrags);
  free(bmp);

  DEBUG_G2F("[ImgProbe] Q9 — image up, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q9 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(*arm);
  probeFooter("Q9", okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img Q9: %u/%u frags (3-band frame), hold ended via %s",
           okFrags, totalFrags, tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Streaming helper — push a fresh BMP into the existing image
// container without re-CREATE. Returns true if all fragments acked.
// Used by Q10 and Q11 for the second/third frames after Q6's CREATE.
// ─────────────────────────────────────────────────────────────────────
static bool sendImageBmpFragmentsNoCreate(G2Temple& arm,
                                          const char* tag,
                                          uint32_t pushMagicBase,
                                          uint32_t cid,
                                          const char* cname,
                                          const uint8_t* bmp, size_t bmpLen,
                                          unsigned* outOkFrags,
                                          unsigned* outTotalFrags) {
  if (outOkFrags) *outOkFrags = 0;
  if (outTotalFrags) *outTotalFrags = 0;
  if (!bmp || bmpLen == 0 || !cname) return false;

  const uint32_t kCID    = cid;
  const char*    kCName  = cname;
  const size_t   kChunkBytes = 3000;
  const unsigned kFrags = (unsigned)((bmpLen + kChunkBytes - 1) / kChunkBytes);
  if (outTotalFrags) *outTotalFrags = kFrags;

  // Same ack-tracking pattern as sendImageBmpMultiFragment, minus the
  // CREATE step. Each push in the new session gets its own magic in
  // [pushMagicBase, pushMagicBase + kFrags - 1]; we count Cmd=4 acks
  // in that range and block for completion at the end.
  probePrepImagePushAcks(pushMagicBase, pushMagicBase + kFrags - 1, kFrags);

  uint8_t* pbBuf = (uint8_t*)malloc(kChunkBytes + 96);
  if (!pbBuf) return false;

  unsigned okFrags = 0;
  size_t off = 0;
  const uint32_t burstStartMs = millis();
  bool aborted = false;
  for (unsigned i = 0; i < kFrags && !aborted; i++) {
    // Same sliding-window throttle as sendImageBmpMultiFragment — see
    // there for rationale. Cap at kImgInFlightCap unacked fragments.
    const uint32_t throttleStartMs = millis();
    while (i > (gImgPushAcked + kImgInFlightCap - 1u)) {
      if ((millis() - throttleStartMs) > kImgPushAckTimeoutMs * 2) {
        DEBUG_G2F("[ImgProbe] %s in-flight cap stuck at %u acked / %u sent "
                  "— aborting burst at frag %u/%u",
                  tag, (unsigned)gImgPushAcked, i, i + 1, kFrags);
        aborted = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (aborted) break;

    const size_t chunk = (off + kChunkBytes <= bmpLen) ? kChunkBytes : (bmpLen - off);
    const uint32_t magic = pushMagicBase + i;
    const size_t pbLen = g2BuildImageRawBody(
        magic, kCID, kCName,
        /*sessionId*/ 1, /*totalSize*/ (uint32_t)bmpLen,
        /*fragIndex*/ i, bmp + off, chunk,
        pbBuf, kChunkBytes + 96);
    if (pbLen == 0) break;
    gOurShutdownAtMs = millis();
    DEBUG_G2F("[ImgProbe] %s frag %u/%u magic=%u fragIdx=%u packet=%u (offset=%u/%u, in-flight=%u)",
              tag, i + 1, kFrags, (unsigned)magic, i,
              (unsigned)chunk, (unsigned)off, (unsigned)bmpLen,
              (unsigned)(i + 1u - gImgPushAcked));
    if (!sendPbFragmented(arm, allocSeq(), G2_SID_EVEN_CORE,
                          G2_FLAG_REQUEST, pbBuf, pbLen)) {
      break;
    }
    okFrags++;
    off += chunk;
    vTaskDelay(pdMS_TO_TICKS(kImgFragGapMs));
  }
  free(pbBuf);
  if (outOkFrags) *outOkFrags = okFrags;

  unsigned ackedCount = 0;
  unsigned ackTarget = 0;
  const bool allAcked = probeWaitImagePushAcks(kImgPushAckTimeoutMs,
                                                &ackedCount, &ackTarget);
  const uint32_t burstMs = millis() - burstStartMs;
  DEBUG_G2F("[ImgProbe] %s push complete: %u/%u sent, %u/%u acked in %u ms (%s)",
            tag, okFrags, kFrags, ackedCount, ackTarget,
            (unsigned)burstMs, allAcked ? "OK" : "ACK TIMEOUT");
  return okFrags == kFrags && allAcked;
}

// ─────────────────────────────────────────────────────────────────────
// Q11 — minimal streaming swap. CREATE once, push frame A, wait, push
// frame B (different content) into the same container. If B replaces
// A cleanly, we have a streaming pipeline; per-frame cost drops from
// ~3 s (with re-CREATE) to ~2.5 s (push-only).
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ11SimpleSwap() {
  static char ret[240];

  G2Temple* arm = pickEvenAIArm("imgQ11");
  if (!arm) return "Img Q11: no reachable temple";

  // CREATE 0x18=232, frame A push base 0x19, frame B push base 0x20.
  // Frame A uses 0x19..0x1F (7 magics), B uses 0x20..0x26 (7 magics).
  const uint32_t kCreateMagic    = G2_MAGIC_IMAGE_BASE + 0x18;  // 232
  const uint32_t kPushAMagicBase = G2_MAGIC_IMAGE_BASE + 0x19;  // 233..239
  const uint32_t kPushBMagicBase = G2_MAGIC_IMAGE_BASE + 0x20;  // 242..248
  const int32_t  kImgW           = 288;
  const int32_t  kImgH           = 144;

  probeBanner("Q11: simple swap (push A → wait → push B, no re-CREATE)",
              kCreateMagic, kPushBMagicBase + 7,
              "expect Cmd=1 res=0 then 7 cmd=4 acks for A, ~3s hold, "
              "then 7 more cmd=4 acks for B. If B's content replaces "
              "A on the lens, the firmware accepts back-to-back "
              "Cmd=3 sessions on a single CREATE — streaming works.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q11: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmpA = (uint8_t*)malloc(kBmpCap);
  uint8_t* bmpB = (uint8_t*)malloc(kBmpCap);
  if (!bmpA || !bmpB) {
    if (bmpA) free(bmpA);
    if (bmpB) free(bmpB);
    probePostProbeShutdown(*arm);
    return "Img Q11: BMP heap alloc failed";
  }
  const size_t bmpALen = buildBmp4bpp(bmpA, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  // Frame B: stripes baseline, then a black header band so it's
  // visually distinct from A. (No primitives lib in flight — reuse Q9's
  // helper.)
  const size_t bmpBLen = buildBmp4bpp(bmpB, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpALen == 0 || bmpBLen == 0) {
    free(bmpA); free(bmpB);
    probePostProbeShutdown(*arm);
    return "Img Q11: BMP build failed";
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;
  bmpDrawRect4bpp(bmpB + kPixOffset, kImgW, -kImgH,
                  /*x*/0, /*y*/0, /*w*/kImgW, /*h*/72, /*idx*/0);

  // Frame A: full CREATE + push.
  unsigned okA = 0, totalA = 0;
  (void)sendImageBmpMultiFragment(*arm, "Q11/A", kCreateMagic, kPushAMagicBase,
                                  bmpA, bmpALen, &okA, &totalA);
  DEBUG_G2F("[ImgProbe] Q11 — frame A shipped (%u/%u), holding 3 s before B",
            okA, totalA);
  vTaskDelay(pdMS_TO_TICKS(3000));

  // Frame B: push only, same container.
  unsigned okB = 0, totalB = 0;
  (void)sendImageBmpFragmentsNoCreate(*arm, "Q11/B", kPushBMagicBase,
                                       /*cid*/ 2, /*cname*/ "imgQ4",
                                       bmpB, bmpBLen, &okB, &totalB);
  free(bmpA); free(bmpB);

  DEBUG_G2F("[ImgProbe] Q11 — both frames shipped, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q11 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(*arm);
  probeFooter("Q11", okA + okB, totalA + totalB);
  snprintf(ret, sizeof(ret),
           "Img Q11: A=%u/%u, B=%u/%u, hold via %s — if B replaced A, "
           "streaming works",
           okA, totalA, okB, totalB,
           tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q10 — streaming with intermediate black-clear. CREATE → push A →
// wait → push all-black → wait → push B. Tests whether an explicit
// black frame between content frames helps clean transitions; only
// meaningful if Q11 leaves visible artifacts.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ10ClearThenPush() {
  static char ret[260];

  G2Temple* arm = pickEvenAIArm("imgQ10");
  if (!arm) return "Img Q10: no reachable temple";

  // ALL magics (CREATE + every push) must fit in uint8 ≤ 255.
  // Confirmed 2026-04-27: a previous Q10 allocation that crossed magic
  // 256 had every push >255 silently dropped — only frags with magic
  // 251..255 acked, the rest never reached the lens. Q10 needs 22 slots
  // (1 CREATE + 21 pushes), so we pack tightly into 226..247.
  const uint32_t kCreateMagic       = G2_MAGIC_IMAGE_BASE + 0x10;  // 226
  const uint32_t kPushAMagicBase    = G2_MAGIC_IMAGE_BASE + 0x11;  // 227..233
  const uint32_t kPushBlackMagicBase= G2_MAGIC_IMAGE_BASE + 0x18;  // 234..240
  const uint32_t kPushBMagicBase    = G2_MAGIC_IMAGE_BASE + 0x1F;  // 241..247
  const int32_t  kImgW              = 288;
  const int32_t  kImgH              = 144;

  probeBanner("Q10: clear-then-push (A → black → B, no re-CREATE)",
              kCreateMagic, kPushBMagicBase + 7,
              "tests whether an intermediate black frame between A and "
              "B improves transition quality. Only meaningful if Q11 "
              "shows tearing or ghosting.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q10: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmpA = (uint8_t*)malloc(kBmpCap);
  uint8_t* bmpBlack = (uint8_t*)malloc(kBmpCap);
  uint8_t* bmpB = (uint8_t*)malloc(kBmpCap);
  if (!bmpA || !bmpBlack || !bmpB) {
    if (bmpA) free(bmpA);
    if (bmpBlack) free(bmpBlack);
    if (bmpB) free(bmpB);
    probePostProbeShutdown(*arm);
    return "Img Q10: BMP heap alloc failed";
  }
  const size_t bmpALen     = buildBmp4bpp(bmpA, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  const size_t bmpBlackLen = buildBmp4bpp(bmpBlack, kBmpCap, kImgW, -kImgH, BMP_PAT_ALL_BLACK);
  const size_t bmpBLen     = buildBmp4bpp(bmpB, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpALen == 0 || bmpBlackLen == 0 || bmpBLen == 0) {
    free(bmpA); free(bmpBlack); free(bmpB);
    probePostProbeShutdown(*arm);
    return "Img Q10: BMP build failed";
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;
  bmpDrawRect4bpp(bmpB + kPixOffset, kImgW, -kImgH,
                  /*x*/0, /*y*/0, /*w*/kImgW, /*h*/72, /*idx*/0);

  unsigned okA = 0, totalA = 0;
  (void)sendImageBmpMultiFragment(*arm, "Q10/A", kCreateMagic, kPushAMagicBase,
                                  bmpA, bmpALen, &okA, &totalA);
  DEBUG_G2F("[ImgProbe] Q10 — A shipped (%u/%u), 2 s hold then black", okA, totalA);
  vTaskDelay(pdMS_TO_TICKS(2000));

  unsigned okBlack = 0, totalBlack = 0;
  (void)sendImageBmpFragmentsNoCreate(*arm, "Q10/black", kPushBlackMagicBase,
                                       /*cid*/ 2, /*cname*/ "imgQ4",
                                       bmpBlack, bmpBlackLen, &okBlack, &totalBlack);
  DEBUG_G2F("[ImgProbe] Q10 — black shipped (%u/%u), 1 s hold then B", okBlack, totalBlack);
  vTaskDelay(pdMS_TO_TICKS(1000));

  unsigned okB = 0, totalB = 0;
  (void)sendImageBmpFragmentsNoCreate(*arm, "Q10/B", kPushBMagicBase,
                                       /*cid*/ 2, /*cname*/ "imgQ4",
                                       bmpB, bmpBLen, &okB, &totalB);
  free(bmpA); free(bmpBlack); free(bmpB);

  DEBUG_G2F("[ImgProbe] Q10 — all 3 frames shipped, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q10 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(*arm);
  probeFooter("Q10", okA + okBlack + okB, totalA + totalBlack + totalB);
  snprintf(ret, sizeof(ret),
           "Img Q10: A=%u/%u, black=%u/%u, B=%u/%u, hold via %s",
           okA, totalA, okBlack, totalBlack, okB, totalB,
           tapped ? "user tap" : "60s timeout");
  return ret;
}

static bool readBmpFromVfs(const String& rawPath,
                           uint8_t** outData,
                           size_t* outLen,
                           int32_t* outW,
                           int32_t* outH,
                           const char** outErr) {
  if (outData) *outData = nullptr;
  if (outLen) *outLen = 0;
  if (outW) *outW = 0;
  if (outH) *outH = 0;
  if (outErr) *outErr = "unknown error";

  String path = rawPath;
  path.trim();
  if (path.length() == 0) { if (outErr) *outErr = "empty path"; return false; }
  if (!path.startsWith("/")) path = "/" + path;
  if (!VFS::exists(path)) { if (outErr) *outErr = "file not found"; return false; }

  File f = VFS::open(path, FILE_READ);
  if (!f || !f.available()) { if (outErr) *outErr = "failed to open file"; return false; }
  const size_t len = (size_t)f.size();
  const size_t kMaxBmpBytes = 65536;
  if (len < 70) { f.close(); if (outErr) *outErr = "file too small for BMP"; return false; }
  if (len > kMaxBmpBytes) { f.close(); if (outErr) *outErr = "BMP too large (>64KB)"; return false; }

  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) { f.close(); if (outErr) *outErr = "out of memory reading BMP"; return false; }
  const size_t rd = f.read(buf, len);
  f.close();
  if (rd != len) { free(buf); if (outErr) *outErr = "short read from BMP file"; return false; }

  if (!(buf[0] == 'B' && buf[1] == 'M')) {
    free(buf); if (outErr) *outErr = "not a BMP (missing BM header)"; return false;
  }
  const uint32_t dibSize = (uint32_t)buf[14] | ((uint32_t)buf[15] << 8) |
                           ((uint32_t)buf[16] << 16) | ((uint32_t)buf[17] << 24);
  if (dibSize < 40) { free(buf); if (outErr) *outErr = "unsupported BMP DIB header"; return false; }

  const int32_t w = (int32_t)((uint32_t)buf[18] | ((uint32_t)buf[19] << 8) |
                              ((uint32_t)buf[20] << 16) | ((uint32_t)buf[21] << 24));
  const int32_t h = (int32_t)((uint32_t)buf[22] | ((uint32_t)buf[23] << 8) |
                              ((uint32_t)buf[24] << 16) | ((uint32_t)buf[25] << 24));
  const uint16_t planes = (uint16_t)buf[26] | ((uint16_t)buf[27] << 8);
  const uint16_t bpp = (uint16_t)buf[28] | ((uint16_t)buf[29] << 8);
  const uint32_t compression = (uint32_t)buf[30] | ((uint32_t)buf[31] << 8) |
                               ((uint32_t)buf[32] << 16) | ((uint32_t)buf[33] << 24);
  if (planes != 1) { free(buf); if (outErr) *outErr = "invalid BMP planes"; return false; }
  if (bpp != 4) { free(buf); if (outErr) *outErr = "only 4bpp BMP supported"; return false; }
  if (compression != 0) { free(buf); if (outErr) *outErr = "compressed BMP not supported"; return false; }
  if ((w < 0 ? -w : w) != 288 || (h < 0 ? -h : h) != 144) {
    free(buf); if (outErr) *outErr = "BMP must be 288x144"; return false;
  }

  if (outData) *outData = buf;
  if (outLen) *outLen = len;
  if (outW) *outW = w;
  if (outH) *outH = h;
  if (outErr) *outErr = "";
  return true;
}

static void applyBmpPaletteTuningInPlace(uint8_t* bmp, size_t bmpLen,
                                         int brightnessPct,
                                         int contrastPct) {
  if (!bmp || bmpLen < 70) return;
  if (!(bmp[0] == 'B' && bmp[1] == 'M')) return;

  const uint32_t dibSize = (uint32_t)bmp[14] | ((uint32_t)bmp[15] << 8) |
                           ((uint32_t)bmp[16] << 16) | ((uint32_t)bmp[17] << 24);
  const uint32_t pxOff   = (uint32_t)bmp[10] | ((uint32_t)bmp[11] << 8) |
                           ((uint32_t)bmp[12] << 16) | ((uint32_t)bmp[13] << 24);
  const uint32_t palOff  = 14 + dibSize;
  if (pxOff <= palOff || pxOff > bmpLen) return;

  const uint32_t palBytes = pxOff - palOff;
  if (palBytes < 64) return;  // Need at least 16 BGRA entries for 4bpp.

  const float c = (float)contrastPct / 100.0f;
  const float b = ((float)brightnessPct * 255.0f) / 100.0f;
  const float scale = 1.0f + c;

  for (int i = 0; i < 16; i++) {
    const uint32_t p = palOff + (uint32_t)i * 4U;
    if (p + 2 >= bmpLen) break;
    const float base = (float)(i * 17);  // grayscale ramp 0..255
    float v = ((base - 128.0f) * scale) + 128.0f + b;
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    const uint8_t u = (uint8_t)(v + 0.5f);
    bmp[p + 0] = u; // B
    bmp[p + 1] = u; // G
    bmp[p + 2] = u; // R
  }
}

static const char* cmd_g2bmp(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs ca(argsInput);
  String path = ca.arg(0);
  path.trim();
  if (path.length() == 0) return "Usage: g2bmp </path/to/file.bmp> [brightness -100..100] [contrast -100..100] [holdSeconds 0..120]";
  int brightness = 0;
  int contrast = 0;
  int holdSeconds = 3;
  if (ca.count() >= 2) brightness = atoi(ca.arg(1).c_str());
  if (ca.count() >= 3) contrast = atoi(ca.arg(2).c_str());
  if (ca.count() >= 4) holdSeconds = atoi(ca.arg(3).c_str());
  if (brightness < -100) brightness = -100;
  if (brightness > 100)  brightness = 100;
  if (contrast < -100)   contrast = -100;
  if (contrast > 100)    contrast = 100;
  if (holdSeconds < 0)   holdSeconds = 0;
  if (holdSeconds > 120) holdSeconds = 120;

  G2Temple* arm = pickEvenAIArm("g2bmp");
  if (!arm) return "G2 BMP: no reachable temple";

  const char* loadErr = "";
  uint8_t* bmp = nullptr;
  size_t bmpLen = 0;
  int32_t bmpW = 0;
  int32_t bmpH = 0;
  if (!readBmpFromVfs(path, &bmp, &bmpLen, &bmpW, &bmpH, &loadErr)) {
    static char err[160];
    snprintf(err, sizeof(err), "G2 BMP: %s", loadErr ? loadErr : "failed to load BMP");
    return err;
  }
  applyBmpPaletteTuningInPlace(bmp, bmpLen, brightness, contrast);

  const unsigned estimatedFrags = (unsigned)((bmpLen + 3000 - 1) / 3000);
  const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x20;  // 242
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x21;  // 243+
  if (kPushMagicBase + estimatedFrags >= 256) {
    free(bmp);
    return "G2 BMP: too many fragments for magic window";
  }

  DEBUG_G2F("[G2] g2bmp: path='%s' bytes=%u dims=%dx%d frags=%u bright=%d contrast=%d hold=%ds",
            path.c_str(), (unsigned)bmpLen, (int)bmpW, (int)bmpH, estimatedFrags,
            brightness, contrast, holdSeconds);
  if (!probeTearDownActiveContainer(*arm)) {
    free(bmp);
    return "G2 BMP: pre-push SHUTDOWN failed";
  }

  unsigned okFrags = 0;
  unsigned totalFrags = 0;
  const bool ok = sendImageBmpMultiFragment(*arm, "g2bmp",
                                            kCreateMagic, kPushMagicBase,
                                            bmp, bmpLen, &okFrags, &totalFrags);
  free(bmp);
  vTaskDelay(pdMS_TO_TICKS((uint32_t)holdSeconds * 1000U));
  probePostProbeShutdown(*arm);

  static char out[220];
  if (!ok) {
    snprintf(out, sizeof(out),
             "G2 BMP: push incomplete (%u/%u fragments, bright=%d contrast=%d hold=%ds)",
             okFrags, totalFrags, brightness, contrast, holdSeconds);
    return out;
  }
  snprintf(out, sizeof(out),
           "G2 BMP: sent %u bytes (%u/%u frags, bright=%d contrast=%d hold=%ds).",
           (unsigned)bmpLen, okFrags, totalFrags, brightness, contrast, holdSeconds);
  return out;
}

// ─────────────────────────────────────────────────────────────────────
// QGlizzy — static-image probe with a hardcoded path. Loads
// /sd/PICTURES/test.bmp from VFS, validates it through readBmpFromVfs
// (BM header, 4bpp, 288×144), and pushes via the standard
// sendImageBmpMultiFragment helper. Same transport as Q6/g2bmp; the
// only difference is no operator input — useful as a quick static-
// image canary on the Static Tests sub-menu.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQGlizzy() {
  static char ret[220];

  G2Temple* arm = pickEvenAIArm("imgQGlizzy");
  if (!arm) return "Img QGlizzy: no reachable temple";

  // 7 push frags max for a 288×144 4bpp BMP. CREATE 238 + pushes
  // 239..245 — distinct from Q9's range (230..237) and Q11's A range
  // (235..241), all comfortably ≤ 255 per the uint8 push-magic rule.
  const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x1C;  // 238
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x1D;  // 239..245

  probeBanner("QGlizzy: load /sd/PICTURES/test.bmp and push",
              kCreateMagic, kPushMagicBase + 7,
              "loads a 288×144 4bpp BMP from SD and ships it through "
              "the same pipeline as Q6/g2bmp. Hold until double-tap "
              "(60 s safety cap). Useful canary for SD-backed image "
              "delivery — no operator input required.");

  const char* loadErr = "";
  uint8_t* bmp = nullptr;
  size_t bmpLen = 0;
  int32_t bmpW = 0;
  int32_t bmpH = 0;
  if (!readBmpFromVfs(String("/sd/PICTURES/test.bmp"),
                      &bmp, &bmpLen, &bmpW, &bmpH, &loadErr)) {
    DEBUG_G2F("[ImgProbe] QGlizzy — load failed: %s", loadErr ? loadErr : "?");
    snprintf(ret, sizeof(ret),
             "Img QGlizzy: load failed — %s", loadErr ? loadErr : "unknown");
    return ret;
  }
  DEBUG_G2F("[ImgProbe] QGlizzy loaded: %u B (%dx%d)",
            (unsigned)bmpLen, (int)bmpW, (int)bmpH);

  if (!probeTearDownActiveContainer(*arm)) {
    free(bmp);
    return "Img QGlizzy: pre-burst SHUTDOWN failed";
  }

  unsigned okFrags = 0;
  unsigned totalFrags = 0;
  (void)sendImageBmpMultiFragment(*arm, "QGlizzy",
                                  kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, &totalFrags);
  free(bmp);

  DEBUG_G2F("[ImgProbe] QGlizzy — image up, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] QGlizzy — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(*arm);
  probeFooter("QGlizzy", okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img QGlizzy: %u/%u frags from /sd/PICTURES/test.bmp, hold via %s",
           okFrags, totalFrags, tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q12 — full-display 576×288 image as 2×2 grid of 288×144 tiles.
// Single CREATE declares all 4 ImageObject children at distinct
// positions/CIDs; subsequent Cmd=3 pushes target each tile by its
// CID/name pair. Each tile carries the same striped baseline plus a
// 24×24 white square at the corner facing the display centre — when
// alignment is correct the four squares form a single 48×48 white
// block at the dead centre of the lens.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ12FullScreen() {
  static char ret[260];

  G2Temple* arm = pickEvenAIArm("imgQ12");
  if (!arm) return "Img Q12: no reachable temple";

  // 1 CREATE + 4 tiles × 7 pushes = 29 magics. Pack into 210..238 so
  // every magic stays ≤ 255 (uint8 firmware constraint).
  const uint32_t kCreateMagic    = G2_MAGIC_IMAGE_BASE + 0x00;  // 210
  const uint32_t kPushBase[4]    = {
      G2_MAGIC_IMAGE_BASE + 0x01,  // 211..217  TL
      G2_MAGIC_IMAGE_BASE + 0x08,  // 218..224  TR
      G2_MAGIC_IMAGE_BASE + 0x0F,  // 225..231  BL
      G2_MAGIC_IMAGE_BASE + 0x16,  // 232..238  BR
  };
  const int32_t  kTileW          = 288;
  const int32_t  kTileH          = 144;
  const int32_t  kFullW          = 576;
  const int32_t  kFullH          = 288;
  (void)kFullW; (void)kFullH;  // documented for the operator; not sent on wire

  // Tile descriptors — declared in the single CREATE, referenced
  // individually by subsequent Cmd=3 pushes.
  const G2ImageTile kTiles[4] = {
    { /*x*/ 0,                       /*y*/ 0,
      /*w*/ (uint32_t)kTileW,        /*h*/ (uint32_t)kTileH,
      /*cid*/ 2, /*name*/ "tileTL" },
    { /*x*/ (uint32_t)kTileW,        /*y*/ 0,
      /*w*/ (uint32_t)kTileW,        /*h*/ (uint32_t)kTileH,
      /*cid*/ 3, /*name*/ "tileTR" },
    { /*x*/ 0,                       /*y*/ (uint32_t)kTileH,
      /*w*/ (uint32_t)kTileW,        /*h*/ (uint32_t)kTileH,
      /*cid*/ 4, /*name*/ "tileBL" },
    { /*x*/ (uint32_t)kTileW,        /*y*/ (uint32_t)kTileH,
      /*w*/ (uint32_t)kTileW,        /*h*/ (uint32_t)kTileH,
      /*cid*/ 5, /*name*/ "tileBR" },
  };

  probeBanner("Q12: full-screen 576×288 (2×2 grid of 288×144 tiles)",
              kCreateMagic, kPushBase[3] + 7,
              "single CREATE with 4 ImageObject children, then 4 "
              "sequential Cmd=3 streams (one per tile). When alignment "
              "is correct the four 24×24 corner squares converge to a "
              "single 48×48 white block at lens centre.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q12: pre-burst SHUTDOWN failed";

  // Build + send the multi-tile CREATE.
  uint8_t createBuf[256];
  size_t createLen = g2BuildCreateImageMulti(
      allocSeq(), kCreateMagic, kTiles, /*tileCount*/ 4,
      BLOCKS_WIDGET_ID, createBuf, sizeof(createBuf));
  if (createLen == 0) return "Img Q12: CREATE-multi build failed";

  gOurShutdownAtMs = millis();
  DEBUG_G2F("[ImgProbe] Q12 CREATE-multi magic=%u (4 tiles, full %dx%d, widgetId=%u)",
            (unsigned)kCreateMagic, (int)kFullW, (int)kFullH,
            (unsigned)BLOCKS_WIDGET_ID);
  logPbHex("Q12 env", createBuf, createLen);
  probePrepImageCreateAck(kCreateMagic);
  if (!sendEnvelope(*arm, createBuf, createLen)) {
    probePostProbeShutdown(*arm);
    return "Img Q12: CREATE-multi TX failed";
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs)) {
    probePostProbeShutdown(*arm);
    return "Img Q12: CREATE-multi ack timeout (4-tile geometry rejected?)";
  }
  DEBUG_G2F("[ImgProbe] Q12 CREATE-multi acked — pushing 4 tiles");

  // Single repaint buffer reused across all 4 tile pushes.
  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)malloc(kBmpCap);
  if (!bmp) {
    probePostProbeShutdown(*arm);
    return "Img Q12: BMP heap alloc failed";
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;

  // Per-tile inside-corner anchors. The corner that faces the centre
  // of the full display, in the tile's own (288×144) coordinate space.
  struct CornerXY { int32_t x, y; };
  const CornerXY kCorner[4] = {
    { kTileW - 24, kTileH - 24 },  // TL → bottom-right of tile
    { 0,           kTileH - 24 },  // TR → bottom-left of tile
    { kTileW - 24, 0           },  // BL → top-right of tile
    { 0,           0           },  // BR → top-left of tile
  };
  const char* kTileTag[4] = { "Q12/TL", "Q12/TR", "Q12/BL", "Q12/BR" };

  unsigned okTotal = 0, fragTotal = 0;
  for (size_t i = 0; i < 4; i++) {
    size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kTileW, -kTileH, BMP_PAT_STRIPES);
    if (bmpLen == 0) { free(bmp); probePostProbeShutdown(*arm); return "Img Q12: BMP build failed"; }
    bmpDrawRect4bpp(bmp + kPixOffset, kTileW, -kTileH,
                    kCorner[i].x, kCorner[i].y, /*w*/ 24, /*h*/ 24, /*idx*/ 15);

    unsigned ok = 0, total = 0;
    (void)sendImageBmpFragmentsNoCreate(*arm, kTileTag[i], kPushBase[i],
                                         /*cid*/ kTiles[i].containerId,
                                         /*cname*/ kTiles[i].containerName,
                                         bmp, bmpLen, &ok, &total);
    okTotal   += ok;
    fragTotal += total;
    DEBUG_G2F("[ImgProbe] Q12 tile %u/4 (%s) shipped %u/%u",
              (unsigned)(i + 1), kTileTag[i], ok, total);
    // Brief inter-tile gap so the firmware's reassembly window for tile N
    // drains before tile N+1 starts piling up. 100 ms is enough at the
    // current per-tile baseline (~2.6 s burst, so the channel is mostly idle
    // by the time we reach this gap).
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  free(bmp);

  DEBUG_G2F("[ImgProbe] Q12 — full-display image up (%u/%u total frags), "
            "double-tap to dismiss (60 s cap)",
            okTotal, fragTotal);
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q12 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(*arm);
  probeFooter("Q12", okTotal, fragTotal);
  snprintf(ret, sizeof(ret),
           "Img Q12: 4-tile full-screen %u/%u frags, hold via %s",
           okTotal, fragTotal, tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q13 — live image tile pipeline. Single 288×144 container; each tick
// rasterises a fresh BMP (stripes pattern with a moving horizontal bar
// indexed by frame counter) and pushes it without re-CREATE. Loops
// until the user double-taps or a safety cap fires. Tick cadence is
// driven by `gG2LiveRateMs` (CLI: `g2liverate`); achievable cadence is
// gated by per-push burst time (~2.6 s per 288×144 frame on this stack).
// Logs the requested vs measured cadence per frame so the operator can
// characterise sustained throughput.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ13LiveTile() {
  static char ret[260];

  G2Temple* arm = pickEvenAIArm("imgQ13");
  if (!arm) return "Img Q13: no reachable temple";

  const uint32_t kCreateMagic     = G2_MAGIC_IMAGE_BASE + 0x00;  // 210
  const uint32_t kFirstPushBase   = G2_MAGIC_IMAGE_BASE + 0x01;  // 211
  const int32_t  kImgW            = 288;
  const int32_t  kImgH            = 144;
  const uint32_t kSafetyCapMs     = 120000;  // 2 min hard stop
  const uint32_t kSafetyCapFrames = 240;     // sanity cap on frame counter

  probeBanner("Q13: live image tile (push @ rate, no re-CREATE)",
              kCreateMagic, kFirstPushBase + 6,
              "single 288x144 container, push a fresh BMP every "
              "g2liverate ms (default 1000). Each frame shifts a "
              "horizontal bar to make the update visible. Double-tap "
              "to dismiss; auto-stops at 2 min or 240 frames.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q13: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)malloc(kBmpCap);
  if (!bmp) {
    probePostProbeShutdown(*arm);
    return "Img Q13: BMP heap alloc failed";
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;

  // Arm the tap-hold sentinel so the SysEvent handler will set
  // gImgProbeHoldTapPending when the user double-taps. We poll it
  // between frames rather than blocking on probeHoldUntilTapOrTimeout.
  gImgProbeHoldTapPending = false;
  gImgProbeHoldActive     = true;

  // Wrap push-magic base across frames so we never exceed uint8.
  // 7 magics per push; wrap to 211 if next 7 would cross 255.
  uint32_t pushMagicBase = kFirstPushBase;
  bool createdOnce = false;
  unsigned framesOk = 0;
  unsigned framesAttempted = 0;
  const uint32_t startMs = millis();
  bool dismissed = false;

  while ((millis() - startMs) < kSafetyCapMs &&
         framesAttempted < kSafetyCapFrames) {
    if (gImgProbeHoldTapPending) { dismissed = true; break; }

    // Build BMP for this frame. Bar is 12 px tall and walks down by
    // 12 px per frame, wrapping at the bottom.
    size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
    if (bmpLen == 0) break;
    const int32_t barY = (int32_t)((framesAttempted * 12) % (uint32_t)(kImgH - 12));
    bmpDrawRect4bpp(bmp + kPixOffset, kImgW, -kImgH,
                    /*x*/ 0, /*y*/ barY, /*w*/ kImgW, /*h*/ 12, /*idx*/ 15);

    // Wrap push-magic window if we'd cross uint8.
    if (pushMagicBase + 7 > 256) pushMagicBase = kFirstPushBase;

    const uint32_t frameStartMs = millis();
    bool ok = false;
    if (!createdOnce) {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpMultiFragment(*arm, "Q13",
                                     kCreateMagic, pushMagicBase,
                                     bmp, bmpLen, &okFrags, &totalFrags);
      createdOnce = ok;
    } else {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpFragmentsNoCreate(*arm, "Q13",
                                          pushMagicBase,
                                          /*cid*/ 2, /*cname*/ "imgQ4",
                                          bmp, bmpLen, &okFrags, &totalFrags);
    }
    pushMagicBase += 7;
    framesAttempted++;
    if (ok) framesOk++;

    const uint32_t frameMs = millis() - frameStartMs;
    DEBUG_G2F("[ImgProbe] Q13 frame %u: %s in %u ms (req=%u ms)",
              framesAttempted, ok ? "OK" : "FAIL", (unsigned)frameMs,
              (unsigned)gG2LiveRateMs);

    // Sleep to honour the requested cadence, but skip the sleep if the
    // frame already took longer than the rate. Poll the tap sentinel
    // during the sleep so dismiss is responsive.
    if (frameMs < gG2LiveRateMs) {
      const uint32_t sleepMs = gG2LiveRateMs - frameMs;
      const uint32_t sleepStart = millis();
      while ((millis() - sleepStart) < sleepMs) {
        if (gImgProbeHoldTapPending) { dismissed = true; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      if (dismissed) break;
    }
  }
  gImgProbeHoldActive = false;
  free(bmp);

  const uint32_t totalMs = millis() - startMs;
  DEBUG_G2F("[ImgProbe] Q13 ended (%s): %u/%u frames in %u ms (avg %u ms/frame)",
            dismissed ? "user tap" : "safety cap",
            framesOk, framesAttempted, (unsigned)totalMs,
            framesAttempted ? (unsigned)(totalMs / framesAttempted) : 0u);

  probePostProbeShutdown(*arm);
  probeFooter("Q13", framesOk, framesAttempted);
  snprintf(ret, sizeof(ret),
           "Img Q13: %u/%u frames in %u ms, end via %s",
           framesOk, framesAttempted, (unsigned)totalMs,
           dismissed ? "user tap" : "safety cap");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q14 — live TEXT REBUILD pipeline. Uses g2ShowText which CREATEs once
// then issues Cmd=7 REBUILD_PAGE on subsequent calls (~one envelope per
// update vs Q13's ~7-fragment multi-push). Each tick fires a new
// content string with frame counter + uptime so the operator can see
// the surface ticking. Cadence governed by gG2LiveRateMs.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ14LiveText() {
  static char ret[260];

  if (!gR.connected && !gL.connected) {
    return "Img Q14: no reachable temple";
  }

  const uint32_t kSafetyCapMs     = 120000;
  const uint32_t kSafetyCapFrames = 1000;

  probeBanner("Q14: live TEXT (REBUILD @ rate)",
              /*magicLo*/ G2_MAGIC_CREATE, /*magicHi*/ G2_MAGIC_REBUILD,
              "g2ShowText() loop. First call CREATEs the TEXT widget, "
              "subsequent calls REBUILD_PAGE — single envelope per "
              "update. Cadence via g2liverate (default 1000 ms). "
              "Double-tap to dismiss; auto-stops at 2 min or 1000 frames.");

  // Tear down the hijack list before the loop. Without this, g2ShowText
  // sees the live container is a List widget (the test-suite menu) and
  // auto-routes to g2ShowTextAsList, which does SHUTDOWN+CREATE-list
  // every frame — visible flicker. Tearing down here lets the first
  // g2ShowText below CREATE a fresh TEXT widget; subsequent calls then
  // hit the Cmd=7 REBUILD_PAGE fast path with no visible teardown.
  G2Temple* armPre = pickEvenAIArm("imgQ14");
  if (!armPre) return "Img Q14: no reachable temple";
  if (!probeTearDownActiveContainer(*armPre)) {
    return "Img Q14: pre-loop SHUTDOWN failed";
  }

  // Same dismiss-via-double-tap pattern as Q13. The TEXT widget on
  // 2.2.0.24 ALSO emits DOUBLE_CLICK_EVENT(3) src=2 from the ring per
  // the protocol doc, so the existing tap-hold sentinel works for
  // both image and text surfaces.
  gImgProbeHoldTapPending = false;
  gImgProbeHoldActive     = true;

  unsigned framesOk = 0;
  unsigned framesAttempted = 0;
  const uint32_t startMs = millis();
  bool dismissed = false;

  while ((millis() - startMs) < kSafetyCapMs &&
         framesAttempted < kSafetyCapFrames) {
    if (gImgProbeHoldTapPending) { dismissed = true; break; }

    char line[96];
    const uint32_t upS = (millis() - startMs) / 1000u;
    snprintf(line, sizeof(line),
             "Live #%u\nup=%u s\nrate=%u ms",
             framesAttempted + 1, (unsigned)upS, (unsigned)gG2LiveRateMs);

    const uint32_t frameStartMs = millis();
    const bool ok = g2ShowText(line);
    framesAttempted++;
    if (ok) framesOk++;

    const uint32_t frameMs = millis() - frameStartMs;
    DEBUG_G2F("[ImgProbe] Q14 frame %u: %s in %u ms (req=%u ms)",
              framesAttempted, ok ? "OK" : "FAIL", (unsigned)frameMs,
              (unsigned)gG2LiveRateMs);

    if (frameMs < gG2LiveRateMs) {
      const uint32_t sleepMs = gG2LiveRateMs - frameMs;
      const uint32_t sleepStart = millis();
      while ((millis() - sleepStart) < sleepMs) {
        if (gImgProbeHoldTapPending) { dismissed = true; break; }
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      if (dismissed) break;
    }
  }
  gImgProbeHoldActive = false;

  const uint32_t totalMs = millis() - startMs;
  DEBUG_G2F("[ImgProbe] Q14 ended (%s): %u/%u rebuilds in %u ms (avg %u ms/frame)",
            dismissed ? "user tap" : "safety cap",
            framesOk, framesAttempted, (unsigned)totalMs,
            framesAttempted ? (unsigned)(totalMs / framesAttempted) : 0u);

  // Q14 uses a TEXT widget — clean it up via the standard hijack path
  // so the next probe / picker rebuild starts from a known state.
  G2Temple* arm = pickEvenAIArm("imgQ14");
  if (arm) probePostProbeShutdown(*arm);
  probeFooter("Q14", framesOk, framesAttempted);
  snprintf(ret, sizeof(ret),
           "Img Q14: %u/%u rebuilds in %u ms, end via %s",
           framesOk, framesAttempted, (unsigned)totalMs,
           dismissed ? "user tap" : "safety cap");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q15 — left-arm image push test. Same BMP and transport as Q6 (single
// 288×144 tile, 7-fragment burst), but explicitly targets the LEFT
// temple instead of letting pickEvenAIArm pick the RIGHT default.
//
// Motivation: 2026-04-28 third-party report (jimrandomh on Discord)
// claimed "significant performance improvement if you send image
// fragments to the left arm instead of the right." Test framework:
// run Q6 (or Q15 paired) for the right-arm baseline (~2.6 s), then
// Q15 for the left-arm timing, and compare. If left is meaningfully
// faster (e.g. <2.0 s), that's an opportunity to change the default
// arm-picker for image-push helpers.
//
// Caveats observed in our setup:
//   * The left temple's plugin task often goes silent (heartbeat acks
//     stop) when the user isn't wearing the glasses or the user has
//     been inactive — we surface this as `pluginDead`. Q15 refuses to
//     run when LEFT is plugin-dead, since image pushes there will
//     queue indefinitely without acks.
//   * On firmware 2.2.0.24, ALL notify traffic (incl. cmd=4 ImageRawResp
//     acks) currently arrives on the RIGHT pipe — see
//     docs/G2_PROTOCOL.md "Notify channel topology on firmware 2.2.0.24
//     — right-only". So even if we TX to LEFT, acks come back via
//     RIGHT and the existing ack-tracking path still works.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ15LeftArm() {
  static char ret[260];

  if (!gL.connected) {
    return "Img Q15: LEFT temple not connected";
  }
  if (gL.pluginDead) {
    return "Img Q15: LEFT temple plugin silent — heartbeat acks not arriving; "
           "wear the glasses or wake them, then retry";
  }
  G2Temple* arm = &gL;

  const uint32_t kCreateMagic    = G2_MAGIC_IMAGE_BASE + 0x00;  // 210
  const uint32_t kPushMagicBase  = G2_MAGIC_IMAGE_BASE + 0x01;  // 211..217
  const int32_t  kImgW           = 288;
  const int32_t  kImgH           = 144;
  const size_t   kChunkBytes     = 3000;

  probeBanner("Q15: LEFT-arm image push (288x144, baseline-vs-LEFT test)",
              kCreateMagic, kPushMagicBase + 7,
              "same BMP / transport / fragmentation as Q6 but TXed via "
              "LEFT temple. Compare burst time to Q6's ~2.6 s baseline. "
              "Per Discord 2026-04-28 left may be faster — verify.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q15: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)malloc(kBmpCap);
  if (!bmp) {
    probePostProbeShutdown(*arm);
    return "Img Q15: BMP heap alloc failed";
  }
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(*arm);
    return "Img Q15: BMP build failed";
  }
  const unsigned kFrags = (unsigned)((bmpLen + kChunkBytes - 1) / kChunkBytes);
  DEBUG_G2F("[ImgProbe] Q15 LEFT-arm: BMP %u B (%dx%d 4bpp), %u chunks of <=%u B",
            (unsigned)bmpLen, kImgW, kImgH, kFrags, (unsigned)kChunkBytes);

  unsigned okFrags = 0, totalFrags = 0;
  const uint32_t burstStartMs = millis();
  (void)sendImageBmpMultiFragment(*arm, "Q15/L",
                                  kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, &totalFrags);
  const uint32_t burstMs = millis() - burstStartMs;
  free(bmp);

  DEBUG_G2F("[ImgProbe] Q15 LEFT-arm result: %u/%u frags in %u ms "
            "(Q6 right-arm baseline ~2.6 s — left %s)",
            okFrags, totalFrags, (unsigned)burstMs,
            burstMs < 2400 ? "FASTER" :
            burstMs > 2800 ? "SLOWER" : "comparable");

  DEBUG_G2F("[ImgProbe] Q15 — image up, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q15 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(*arm);
  probeFooter("Q15", okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img Q15: LEFT-arm %u/%u frags in %u ms (vs Q6 right ~2600 ms)",
           okFrags, totalFrags, (unsigned)burstMs);
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q16/Q17/Q18 — mixed CREATE (List + Image) composition probes.
// Tests whether the firmware accepts multi-type widget declarations in
// a single CreateStartUpPageContainer (schema-allowed but never tested
// in this codebase before 2026-04-28). Each probe varies the list and
// image geometry to characterise:
//   Q16: side-by-side, no overlap — does mixed CREATE ack at all?
//   Q17: image overlapping middle of list — what's the z-order?
//   Q18: small image in corner of full-screen list — does the firmware
//        accept non-288×144 image containers?
// ─────────────────────────────────────────────────────────────────────

// Shared helper — does the heavy lifting for all three probes. Caller
// passes the geometry and image dimensions; helper handles CREATE+ack
// wait, BMP build+push, hold-for-tap, teardown.
static const char* runMixedListImageProbe(const char* tag,
                                          const G2ContainerGeom& listGeom,
                                          int32_t imgX, int32_t imgY,
                                          int32_t imgW, int32_t imgH,
                                          char* retBuf, size_t retCap) {
  G2Temple* arm = pickEvenAIArm(tag);
  if (!arm) {
    snprintf(retBuf, retCap, "%s: no reachable temple", tag);
    return retBuf;
  }

  const uint32_t kCreateMagic    = G2_MAGIC_IMAGE_BASE + 0x10;  // 226
  const uint32_t kPushMagicBase  = G2_MAGIC_IMAGE_BASE + 0x11;  // 227..233
  const uint32_t kListCID        = 1;
  const char*    kListName       = "mixL";
  const uint32_t kImageCID       = 2;
  const char*    kImageName      = "mixI";

  // Concise list payload — keep it short so the CREATE envelope stays
  // under the builder's 1 KB payload budget. 5 rows is plenty to verify
  // the list renders.
  const char* listItems[] = {
    "<- Back",
    "Mixed CREATE",
    "list+image test",
    "row 4",
    "row 5",
  };
  const size_t listItemCount = sizeof(listItems) / sizeof(listItems[0]);

  if (!probeTearDownActiveContainer(*arm)) {
    snprintf(retBuf, retCap, "%s: pre-burst SHUTDOWN failed", tag);
    return retBuf;
  }

  // Build and send the mixed CREATE.
  G2ImageTile imgTile;
  imgTile.x = (uint32_t)imgX;
  imgTile.y = (uint32_t)imgY;
  imgTile.w = (uint32_t)imgW;
  imgTile.h = (uint32_t)imgH;
  imgTile.containerId = kImageCID;
  imgTile.containerName = kImageName;

  uint8_t createBuf[1024];
  size_t createLen = g2BuildCreateMixedListImage(
      allocSeq(), kCreateMagic,
      kListName, listItems, listItemCount, listGeom,
      imgTile, BLOCKS_WIDGET_ID,
      createBuf, sizeof(createBuf));
  if (createLen == 0) {
    snprintf(retBuf, retCap, "%s: CREATE-mixed build failed", tag);
    return retBuf;
  }

  DEBUG_G2F("[ImgProbe] %s CREATE-mixed magic=%u "
            "(list@(%u,%u,%u,%u) image@(%d,%d,%dx%d))",
            tag, (unsigned)kCreateMagic,
            (unsigned)listGeom.x, (unsigned)listGeom.y,
            (unsigned)listGeom.w, (unsigned)listGeom.h,
            (int)imgX, (int)imgY, (int)imgW, (int)imgH);
  logPbHex("CREATE-mixed env", createBuf, createLen);

  probePrepImageCreateAck(kCreateMagic);
  if (!sendEnvelope(*arm, createBuf, createLen)) {
    probePostProbeShutdown(*arm);
    snprintf(retBuf, retCap, "%s: CREATE-mixed TX failed", tag);
    return retBuf;
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs)) {
    probePostProbeShutdown(*arm);
    snprintf(retBuf, retCap, "%s: CREATE-mixed ack timeout — firmware "
                             "may not accept list+image composition",
             tag);
    return retBuf;
  }
  DEBUG_G2F("[ImgProbe] %s CREATE-mixed acked — pushing image to '%s' "
            "(CID=%u, %dx%d)",
            tag, kImageName, (unsigned)kImageCID, (int)imgW, (int)imgH);

  // Build and push the BMP. Negative height = top-down for intuitive
  // pattern testing.
  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)malloc(kBmpCap);
  if (!bmp) {
    probePostProbeShutdown(*arm);
    snprintf(retBuf, retCap, "%s: BMP heap alloc failed", tag);
    return retBuf;
  }
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, imgW, -imgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(*arm);
    snprintf(retBuf, retCap, "%s: BMP build failed (%dx%d)",
             tag, (int)imgW, (int)imgH);
    return retBuf;
  }
  DEBUG_G2F("[ImgProbe] %s BMP %u B (%dx%d 4bpp)",
            tag, (unsigned)bmpLen, (int)imgW, (int)imgH);

  unsigned okFrags = 0, totalFrags = 0;
  (void)sendImageBmpFragmentsNoCreate(*arm, tag, kPushMagicBase,
                                       /*cid*/ kImageCID,
                                       /*cname*/ kImageName,
                                       bmp, bmpLen, &okFrags, &totalFrags);
  free(bmp);
  DEBUG_G2F("[ImgProbe] %s push complete: %u/%u frags acked",
            tag, okFrags, totalFrags);

  DEBUG_G2F("[ImgProbe] %s — both widgets up; observe lens for "
            "list+image composition. Double-tap to dismiss (60 s cap).",
            tag);
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] %s — hold ended via %s",
            tag, tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(*arm);
  probeFooter(tag, okFrags, totalFrags);
  snprintf(retBuf, retCap,
           "Img %s: CREATE-mixed acked, image %u/%u frags (list@%dx%d, "
           "image@%dx%d) — observe lens for composition",
           tag, okFrags, totalFrags,
           (int)listGeom.w, (int)listGeom.h, (int)imgW, (int)imgH);
  return retBuf;
}

// Q16: side-by-side. List occupies the top half; image occupies the
// bottom half (centered horizontally). No overlap. The most likely-to-
// work shape — if this fails, mixed CREATE is rejected outright.
const char* g2ProbeImageQ16MixedSideBySide() {
  static char ret[260];
  // List in top half (8..568 wide, 8..138 tall) — leaves room for ~5
  // text rows.
  const G2ContainerGeom listGeom = { 8, 8, 560, 130 };
  // Image in bottom half — 288×144 tile centered horizontally so it
  // sits flush against the bottom edge.
  return runMixedListImageProbe("Q16", listGeom,
                                /*imgX*/ 144, /*imgY*/ 144,
                                /*imgW*/ 288, /*imgH*/ 144,
                                ret, sizeof(ret));
}

// Q17: image overlapping the right half of a full-screen list. Tests
// whether the firmware paints the image on top of the list (image
// visible, list partly hidden), or under it (list visible, image
// obscured), or rejects the overlap entirely.
//
// 2026-04-27: original geometry centered the image at x=144, which buried
// list text under the tile. Pushed it to x=280 (right edge of the 560-wide
// list area) so the left half stays fully readable while still exercising
// the overlap codepath.
const char* g2ProbeImageQ17MixedOverlap() {
  static char ret[260];
  // List spans most of the lens (G2_GEOM_LARGE = 8,8,560,272).
  const G2ContainerGeom listGeom = G2_GEOM_LARGE;
  // Image 288×144 tile positioned on the right half of the list:
  // x=280 puts the right edge flush at 568 (matching the list right edge),
  // y=72 keeps it vertically centered. Left ~half of every list row stays
  // visible so the user can read the text and see the overlap behavior.
  return runMixedListImageProbe("Q17", listGeom,
                                /*imgX*/ 280, /*imgY*/ 72,
                                /*imgW*/ 288, /*imgH*/ 144,
                                ret, sizeof(ret));
}

// Q18: small icon-sized image in the top-right corner of a full-screen
// list. Tests whether the firmware accepts non-standard image container
// dimensions (we've only ever shipped 288×144 tiles before — Q5/Q7
// established that BMPs don't render unless they match the container
// size; this probe tests both directions of that constraint at a
// smaller size).
const char* g2ProbeImageQ18MixedIcon() {
  static char ret[260];
  const G2ContainerGeom listGeom = G2_GEOM_LARGE;
  // 80×80 icon in the top-right corner. 80 px @ 4 bpp = 40 bytes/row,
  // already 4-byte aligned for BMP stride; total pixel data = 3200 B,
  // header + palette = 118 B → 3318 B BMP = single Cmd=3 fragment.
  return runMixedListImageProbe("Q18", listGeom,
                                /*imgX*/ 488, /*imgY*/ 8,
                                /*imgW*/ 80, /*imgH*/ 80,
                                ret, sizeof(ret));
}

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
