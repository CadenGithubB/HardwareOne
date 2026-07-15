// =============================================================================
// Even Realities G2 glasses — BLE client
// =============================================================================
// Rewrite of the earlier stub-level integration against the protocol described
// in https://github.com/Commute773/g2-kit-unofficial — wire primitives live in
// System_G2_Protocol.{h,cpp}; this file handles the BLE state machine,
// dual-temple connection management, session prelude, and heartbeat task.

#include "G2_Glasses.h"
#include "System_Events.h"  // systemEventPost — event register producer

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <stdarg.h>
#include <new>          // std::nothrow — for LensUiJob allocation in pageSwapEnqueue

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_gap_ble_api.h>  // esp_ble_gap_update_conn_params (HIGH priority)
#include <esp_attr.h>         // EXT_RAM_BSS_ATTR
#include <esp_heap_caps.h>    // heap_caps_get_free_size — ESP.getFreeHeap() includes PSRAM
#include <esp_timer.h>        // esp_timer_create/start_once — replaces notifyClearTaskBody

#include "System_G2_Protocol.h"
#include "Bluetooth.h"
#include "System_Debug.h"
#include "System_Filesystem.h"  // requireQuotedToken (uniform quoted-path rule)
#include "System_Command.h"
#include "System_Utils.h"
#include "System_Notifications.h"
#include "System_MemUtil.h"
#include "System_VFS.h"
#include "System_Battery.h"   // BatteryState + getBatteryPercentage etc — for ESP corner widget
#include "System_Microphone.h"  // gMicEnabled, micConnected, getAudioLevel, etc — for MIC detail page
#include "System_PollPause.h"   // PollPauseGuard — pause sensor polling during BLE connect/discovery
#include "G2_HijackCmd.h"   // g2BumpMenuGen() — called from g2SetHijackPage
#include "System_AuthIdentity.h"  // ExecIdentityGuard + currentAuthContext

extern "C" {
#include "lc3.h"  // vendored Google liblc3 — Apache-2.0 (components/hardwareone_libs/liblc3)
}
#include "WebServer_Server.h"  // broadcastEventToAllSessions() for SSE push
#include "BLE_Events.h"        // CompactJson + blePushEvent
#include "BLE_Peers.h"         // peer registry + saved-MAC reconnect
#include "G2_Ring.h"  // g2RingInit (eager registration in initG2Client)
#include "G2_Page_Sensors.h"   // g2ShowSensorList() (per-page module)
#include "G2_Page_Network.h"   // g2ShowNetworkMenu / g2NetworkHandleTap
#include "G2_Page_Settings.h"  // g2ShowSettingsMenu / g2SettingsHandleTap
#include "G2_Page_Files.h"     // g2ShowFilesMenu / g2FilesHandleTap / g2FilesTick
#include "G2_Page_Power.h"     // g2ShowPowerMenu / g2PowerHandleTap
#include "G2_Page_CameraSettings.h"  // g2ShowCameraSettingsMenu / g2CameraSettingsHandleTap
#include "G2_Page_TestSuite.h" // g2ShowTestSuiteMenu / g2TestSuiteHandleTap
#include "G2_Page_TextEntry.h" // generic on-glasses text-entry overlay
#include "G2_Page_ESPNow.h"    // g2ShowESPNowAppMenu / g2ESPNowAppHandleTap
#include "G2_HijackFsm.h"      // shadow FSM tracking page-swap / hijack lifecycle
#if ENABLE_MAPS
#include "System_Maps.h"       // MapCore/OffscreenMapRenderer — g2map renders the offline map to the lens
#include "System_TaskUtils.h"  // MAP_RENDER_STACK_WORDS — size the g2map worker like the OLED render task
#endif
#include "System_Settings.h"
#include "System_Clock.h"  // Clock::tzOffsetQuarterHours() — explicit-unit tz accessor (defeats minutes/quarter-hours swap footgun)
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

// Audio notify char on the 6450 service — receives LC3 mic frames after
// AudioCtrCmd { AudoFuncEn=1 } is sent on sid=0xE0 Cmd=15. Each notify
// is 205 bytes: [0xCC|0xCD][seq:u8][LC3:203], 16 kHz mono, 20 ms/frame.
// See g2micprobe CLI commands and the audio probe stats below.
static constexpr const char* CHAR_AUDIO_NOTIFY = "00002760-08c2-11e1-9073-0e8ac72e6402";

// Heartbeat cadence — glasses kill the plugin task after ~10 s of silence.
// 5 s gives headroom without hammering the radio.
static constexpr uint32_t HEARTBEAT_PERIOD_MS = 5000;

// Mutex wait for sendEnvelope (heartbeats, single-packet commands). Image
// bursts hold writeMutex for ~0.3–1.0 s; a stuck esp_ble_gattc_write_char
// can exceed 500 ms without the peripheral being dead. Too short a wait makes
// heartbeats "miss" and trips G2_TX_STUCK_DISCONNECT_BEATS prematurely.
static constexpr uint32_t G2_SEND_ENVELOPE_MUTEX_MS = 1500;

// Mutex wait for sendPbFragmented to *begin* a burst. Another writer may
// hold writeMutex for a full multi-envelope Cmd=3 send (many BLE chunks ×
// stepped write retries). Must exceed realistic worst-case burst duration
// so the next tile does not abort with "mutex timeout" while the link is
// merely slow — not dead. (Previously 2 s; too tight vs. G2_SEND_ENVELOPE
// tuning and rc=-1 recovery.)
static constexpr uint32_t G2_SENDPB_BURST_MUTEX_MS = 5000;

// Heartbeat consecutive send failures (mutex timeout or write failure) before
// forcing disconnect. At 5 s cadence, 5 misses ≈ 25 s — enough for BLE
// controller recovery without leaving a truly dead link up indefinitely.
static constexpr uint8_t G2_TX_STUCK_DISCONNECT_BEATS = 5;

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
  // Audio (LC3 mic) notify char on service 6450 — null if not subscribed.
  // Populated by the audio probe path; only used when g2micprobe is on.
  BLERemoteCharacteristic*     audioNotifyChar;
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

  // TX-stuck watchdog. Incremented each heartbeat tick where
  // sendEnvelope returns false (typically a writeMutex timeout — some
  // earlier writer holds the mutex, e.g. a slow BLE writeValue). Reset to
  // 0 on every successful send. G2_TX_STUCK_DISCONNECT_BEATS consecutive
  // misses means the link is wedged — we force disconnect. Without this, a
  // stuck mutex strands the worker / probe FSM (ImgProbe can hold
  // gHijackActive so safety timeouts never send Cmd=9).
  uint8_t                      txStuckBeats;

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

// Set by G2ScanCallbacks immediately before its early gScan->stop(). Arduino
// Bluedroid's BLEScan has no isScanning() (NimBLE does); we skip a redundant
// stop() at the end of the poll loop to avoid "scan not active" errors.
static volatile bool gG2ScanStoppedEarly = false;

// G2ScanCallbacks may call stop() when all required temples are found early.
// Calling stop() again after the poll loop triggers Bluedroid noise:
// "BTM_BleScan scan not active" / stop scan failed status=0x6.
static void g2ScanStopIfActive() {
  if (!gScan) return;
  if (!gG2ScanStoppedEarly) gScan->stop();
  gG2ScanStoppedEarly = false;
}

// Navigation-mode toggle — consumed by Bluetooth.cpp. Kept as an
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
// The ring's BLE layer lives in G2_Ring.cpp; here we just
// stash the advertisedDevice pointer for it to claim.
extern BLEAdvertisedDevice* gRingAdvertisedDevice;  // defined in _Ring.cpp
extern String               gRingDeviceName;
extern String               gRingDeviceAddress;
extern volatile bool        gRingScanFound;

// Per-family in-flight flag for G2 connects. Set true by the public
// g2Connect* wrappers before submitting to the unified BLE-connect worker
// (see g2SubmitBleConnect below), cleared by the worker's dispatch when
// the underlying *Sync function returns. Producers check this to reject
// duplicate submissions. gConnectCancel is signalled by g2Disconnect to
// ask an in-flight connect to bail at its next check.
//
// Group B retired the per-call task spawn (gConnectTaskBody +
// gConnectSavedTaskBody) and the gConnectTaskHandle that used to track
// each transient task — there's no per-call task to track now.
static volatile bool  gConnectTaskActive = false;
static volatile bool  gConnectCancel     = false;

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

// ─── Page-swap echo guard ────────────────────────────────────────────────
// gOurShutdownAtMs: wall-clock timestamp of our most recent intentional
// Cmd=9 ShutdownPage. Stamped via noteOurShutdownSent() at every site
// that issues a Shutdown (page-swap worker, image-probe teardown,
// mid-burst image push). The DISPLAY_OFF and SYSTEM_EXIT handlers
// consult ourShutdownEchoActive() to suppress the firmware echo of our
// own intentional shutdown — without that, those events would clear
// hijack state mid-swap and queue a second Shutdown that contends with
// the worker's pending CREATE on the write mutex.
//
// "Are we mid-swap?" / "Is hijack active?" used to live in two volatile
// bool globals (gPageSwapActive / gHijackActive) updated alongside the
// FSM events. Phase 6 of the FSM refactor retired them: the FSM is now
// the single source of truth, queried via g2FsmPageSwapping() /
// g2FsmHijackActive() inlines further down.
static volatile uint32_t gOurShutdownAtMs;

static constexpr uint32_t kOurShutdownEchoWindowMs = 2000;

// Echo-window read used by the SYSTEM_EXIT / DISPLAY_OFF handlers and
// by the shadow-FSM verify snapshot. Defined here so the inline helpers
// below can call it.
static inline bool ourShutdownEchoActive() {
  const uint32_t ours = gOurShutdownAtMs;
  return ours != 0 && (millis() - ours) < kOurShutdownEchoWindowMs;
}

// Phase 2 + 3 of the hijack-FSM refactor. Asks: "is the firmware
// currently expected to echo our intentional ShutdownPage / mid-burst
// traffic, so DISPLAY_OFF and SYSTEM_EXIT should NOT clear hijack
// state?"
//
// Two signals OR'd together:
//   * FSM state ∈ {PageSwapping, ImageProbing} — the structurally clean
//     answer. Phase 3 added ImageProbing so probe paths are now covered;
//     Phase 5 makes this state check authoritative.
//   * ourShutdownEchoActive() — the legacy 2 s wall-clock window, kept
//     as a defensive fallback for any path the Phase 3 dispatches don't
//     yet cover. With probes wired in, this should now rarely fire
//     alone — divergence logs tell us when it does.
//
// While both are running, we log when they disagree:
//   * "fsm-only" = FSM caught a window the wall-clock didn't (FSM is
//     stricter — usually means a swap/probe that ran past the 2 s
//     window, e.g. Q13's per-frame burst).
//   * "wall-only" = wall-clock caught a window the FSM didn't (FSM model
//     gap — should be near-zero now that ImageProbing is wired). When
//     this stays cold across normal use, Phase 5 can delete the
//     wall-clock entirely.
static inline bool isExpectedEchoWindow(const char* siteTag) {
  const HijackState st = hijackFsmState();
  const bool fsmSays  = (st == HijackState::PageSwapping ||
                         st == HijackState::ImageProbing);
  const bool wallSays = ourShutdownEchoActive();
  if (fsmSays && !wallSays) {
    DEBUG_G2F("[FSM] echo-window divergence @%s: fsm-only (state=%s, wall=cold)",
              siteTag ? siteTag : "?", hijackStateName(st));
  } else if (!fsmSays && wallSays) {
    DEBUG_G2F("[FSM] echo-window divergence @%s: wall-only (state=%s, wall=hot)",
              siteTag ? siteTag : "?", hijackStateName(st));
  }
  return fsmSays || wallSays;
}

// ─── FSM state predicates ──────────────────────────────────────────────
// "Is the hijack live?" — Hijacked / PageSwapping / ImageProbing all
// represent active hijack ownership of the lens. Replaces the legacy
// gHijackActive global (retired in Phase 6).
//
// Eventual consistency: dispatches are async via the FSM worker queue,
// so a g2LensSetHijackActive(true) call followed immediately by this
// predicate may briefly return false until the worker drains. In
// practice the worker drains within tens of microseconds and no read
// site is on that latency-critical path.
static inline bool g2FsmHijackActive() {
  const HijackState st = hijackFsmState();
  return st == HijackState::Hijacked ||
         st == HijackState::PageSwapping ||
         st == HijackState::ImageProbing;
}

// "Are we mid page-swap?" — only PageSwapping qualifies. Used by the
// SYSTEM_EXIT echo guard's wall-clock fallback and by the page-swap
// debounce in g2ShowText. Replaces the legacy gPageSwapActive global.
static inline bool g2FsmPageSwapping() {
  return hijackFsmState() == HijackState::PageSwapping;
}

// gHijackStartedMs is declared here (rather than near the other hijack
// state below) so pageSwapEnd can reset it. Read by the heartbeat
// safety-timer check; written at initial hijack entry, on user input
// gestures, and at the end of every page-swap.
static uint32_t gHijackStartedMs = 0;

// Page-swap-worker lifecycle. begin/end are paired with xTaskCreate; the
// xTaskCreate-failure path also calls pageSwapEnd to release the guard.
static inline void pageSwapBegin() {
  hijackFsmDispatch(HijackEvent::PageSwapBegin, "pageSwapBegin");
}
static inline void pageSwapEnd() {
  hijackFsmDispatch(HijackEvent::PageSwapEnd, "pageSwapEnd");
  // Refresh the hijack safety-timer baseline. Without this, the 60 s
  // HIJACK_SAFETY_MS window measures from the *first* MenuStartUp hijack,
  // so a user who taps a row → reads the new page for 60 s gets a
  // safety-timeout fire mid-read that tears down the foregrounded widget
  // and shows "Connection lost". Reset on every successful page-swap so
  // the timer measures time on the current page.
  gHijackStartedMs = millis();
}
static inline bool pageSwapInFlight() { return g2FsmPageSwapping(); }

// Forward decl — defined further down with the persistent page-swap worker
// infrastructure. Called from initG2Client() to spin up the queue + task.
static void pageSwapInit();

// Forward decls — tap dispatcher. Defers handleHijackMenuTap() AND the
// "Hijack tap: …" BROADCAST_PRINTF off the Bluedroid notify-task stack
// onto a persistent worker. Two reasons this matters:
//   1. BTC_TASK has a small stack (~3-4 KB). Running vsnprintf-heavy
//      BROADCAST_PRINTF + handleHijackMenuTap (which alloc + spawn page
//      swaps) inline pushes BTC over its canary — observed 2026-05-03 as
//      a "stack overflow in task BTC_TASK" panic on the first hijack tap.
//   2. handleHijackMenuTap → g2ShowTextAsList does heap allocations +
//      enqueues page-swap work; doing that inside Bluedroid spinlock
//      context risks reentrant spinlock acquires.
// Mirrors the page-swap and hijack-FSM queue-worker patterns.
static void tapDispatcherInit();
static bool tapDispatcherEnqueue(uint32_t idx, const char* iname);
static bool tapDispatcherEnqueueExit(void (*fn)());

// Echo-guard stamp for intentional Cmd=9 sends. clearOurShutdownStamp is
// called in the worker cleanup so the window doesn't bleed into unrelated
// later events. The 2 s window is now a defensive fallback; the FSM
// state predicate (PageSwapping || ImageProbing) is the primary signal —
// see isExpectedEchoWindow().
static inline void noteOurShutdownSent() {
  gOurShutdownAtMs = millis();
  hijackFsmDispatch(HijackEvent::ShutdownSent, "noteOurShutdownSent");
}
static inline void clearOurShutdownStamp() { gOurShutdownAtMs = 0; }

// Forward decl — declared near the other image-probe state at file scope
// further down, but referenced by imageProbeBegin() here.
extern volatile bool gImgProbeAbort;

// Image-probe lifecycle bookends. Call begin at the probe's pre-burst
// SHUTDOWN site (probeTearDownActiveContainer) and end at the post-probe
// SHUTDOWN site (probePostProbeShutdown). The FSM transitions
// Hijacked -> ImageProbing -> Hijacked, which makes isExpectedEchoWindow()
// suppress the firmware's DISPLAY_OFF / SYSTEM_EXIT echoes during the
// probe via state alone.
static inline void imageProbeBegin() {
  // Clear the dismiss flag so a stale "true" from a previous probe's
  // double-tap doesn't abort this one before its first fragment goes
  // out. Q13 / Q14 also reset this at their own entry (their hold loop
  // owns the flag), but other probe entry points (g2bmp, QGlizzy, etc.)
  // need this generic reset since they don't enter hold mode.
  gImgProbeAbort = false;
  hijackFsmDispatch(HijackEvent::ImageProbeBegin, "imageProbeBegin");
}
static inline void imageProbeEnd() {
  hijackFsmDispatch(HijackEvent::ImageProbeEnd, "imageProbeEnd");
}

// Firmware version string (shared across temples — they report the same
// version, so one cache suffices). Populated from sid=0x09 pushes via
// g2ParseSettingVersion. Empty until the firmware sends its first
// version-bearing settings push (not every settings push carries it).
static char gFwVersion[32] = {0};

// True when the running firmware does not emit any BLE notifications on
// the LEFT temple — heartbeat acks, gestures, state events, settings
// pushes all arrive only on RIGHT. Confirmed independently in
// g2-kit-unofficial/ble/docs/gotchas.md ("Left arm is silent on async
// events ... Don't waste time looking for a config bit"). When this is
// true, the heartbeat-miss → pluginDead heuristic is fundamentally
// inapplicable to L: every L heartbeat will go unacked because the
// firmware doesn't reply, not because the plugin task is hung. Treat
// "no L notify" as steady-state, not a fault.
//
// Empty version → assume affected (only firmware in our hardware
// coverage is 2.2.0.24). Once a version push lands and proves we're on
// a different firmware, the heuristic resumes for L.
static bool firmwareSilencesLeftNotify() {
  if (gFwVersion[0] == '\0') return true;
  if (strcmp(gFwVersion, "2.2.0.24") == 0) return true;
  return false;
}

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
// gHijackActive is defined near the top of this file alongside the other
// page-swap state flags so the shadow FSM helpers can snapshot it.
// gHijackStartedMs is declared near pageSwapBegin/pageSwapEnd at the top
// of this file (pageSwapEnd resets it on every successful page-swap).

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

// Tracks whether the firmware has its own overlay foregrounded over our
// widget — most importantly the tap-and-hold "Exit?" yes/no dialog.
// Set by SysEvent FG_ENTER, cleared by FG_EXIT. When true, USER_ACTIVITY
// arriving on sid=0x0D is the firmware's gesture-detected event (e.g.
// the long-press itself, with hint='state-9B code=224 src=34'), NOT a
// user tap on our widget — so the line-2451 auto-exit fallback must be
// suppressed or it tears down the text view the instant the dialog
// appears, leaving the lens with just the dialog and a blank background.
static volatile bool     gFirmwareOverlayUp = false;
static volatile uint32_t gFirmwareOverlayUpSinceMs = 0;
// Stamp on FG_EXIT so we keep suppressing for a short post-dismiss
// grace. Firmware emits a USER_ACTIVITY (7 B, hint='wake-word
// code=224') ~200 ms after the "Exit?" dialog dismisses — same pattern
// as the existing post-CREATE settle event. Without this grace it
// trips the line-2451 exit-handler the moment the user clicks "No",
// which sends them back to the main menu instead of leaving the
// status page they wanted to keep watching.
static volatile uint32_t gFirmwareOverlayLastExitMs = 0;
static const uint32_t kFirmwareOverlayPostExitGraceMs = 500;
// Safety: if FG_EXIT never arrives (firmware reboot mid-dialog,
// disconnect, etc.), don't keep suppressing forever. 30 s is well past
// any plausible "Exit?" prompt — way longer than the user would hold the
// dialog up.
static const uint32_t kFirmwareOverlayMaxMs = 30000;
static inline bool firmwareOverlayActive() {
  const uint32_t now = millis();
  if (gFirmwareOverlayUp &&
      (now - gFirmwareOverlayUpSinceMs) < kFirmwareOverlayMaxMs) {
    return true;
  }
  // Post-exit settle window: firmware fires one trailing wake-word
  // USER_ACTIVITY a couple hundred ms after FG_EXIT.
  if (gFirmwareOverlayLastExitMs != 0 &&
      (now - gFirmwareOverlayLastExitMs) < kFirmwareOverlayPostExitGraceMs) {
    return true;
  }
  return false;
}

// Image-probe hold state. When an image probe wants to display a BMP
// "until the user taps", it sets gImgProbeHoldActive=true and polls
// gImgProbeHoldTapPending. The SysEvent handler, when it sees a
// CLICK/SCROLL/DOUBLE_CLICK on sid=0xE0 during the hold, sets the
// pending flag — same pattern as gTextViewActive but without an
// exit-callback (the probe worker reads the flag itself).
static volatile bool     gImgProbeHoldActive     = false;
static volatile bool     gImgProbeHoldTapPending = false;
// Generic abort-now signal for probe burst senders. Set alongside
// gImgProbeHoldTapPending whenever the user dismisses (CLICK/DOUBLE_CLICK
// on sid=0xE0 during a hold). The image-burst senders
// (sendImageBmpMultiFragment, sendImageBmpFragmentsNoCreate) poll this in
// their tight inner loops — between BMP fragments and inside the
// in-flight throttle — so dismiss is responsive mid-burst, not just at
// frame boundaries. Cleared by imageProbeBegin() at the start of every
// probe (forward-declared up by the FSM helpers because that's where
// the cleanup is wired in).
volatile bool            gImgProbeAbort          = false;

// Camera-stream list-tap dispatch. The stream worker uses a list+image
// compound CREATE (container name "lstCam"); single-taps on its rows
// arrive via the BLE ListEvent dispatcher and set the corresponding bit
// in gCamStreamPendingTap. The worker drains and clears the bitfield
// between frames. Bits are mutually-exclusive in practice (firmware
// dispatches one tap event at a time) but we OR them so a quick double
// tap can't lose a request. Bit map:
//   0x1  = idx 0  ("<- Back")    → exit, run onDone() (back to CAM detail)
//   0x2  = idx 1  ("Snapshot")   → save current frame to /sd/PICTURES/
//   0x4  = idx 2  ("Settings >>") → exit, chain to g2ShowCameraSettingsMenu()
// gCamStreamActive gates the dispatcher so taps on "lstCam" outside the
// stream window are ignored (e.g. the BLE handler can't race a stale
// pre-stream listener that no worker is draining).
static volatile uint32_t gCamStreamPendingTap = 0;
static volatile bool     gCamStreamActive     = false;
#if ENABLE_MAPS
// Interactive Maps page (g2MapPageWorker) — list-tap bitfield (bit = row
// index) + active gate, mirroring the camera-stream pattern above.
static volatile uint32_t gMapPagePendingTap = 0;
static volatile bool     gMapPageActive     = false;
#endif

// Settings-page back-row → relaunch-stream coordination. See header
// comment on g2CamStreamSettingsExitRelaunch for the full contract.
volatile bool            g2CamStreamSettingsExitRelaunch = false;
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
  if (hijackActive)   *hijackActive   = g2FsmHijackActive();
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

// BLE notify task handle — captured on first handleNotify() call.
// Used by G2_ASSERT_NOT_NOTIFY_TASK to detect deadlock-prone callers
// of the sendCreate*AndWait / sendRebuild*AndWait family. Those
// helpers wait on a semaphore signaled BY this task; calling them
// from this task itself guarantees the wait times out (1.5 s) and
// the on-wire CREATE/REBUILD never gets a follow-up. We hit exactly
// this with g2StartLiveTextPage / Status; the assertion catches the
// regression class at the call site instead of at the timeout.
//
// Cleared if BLE drops + reconnects on a different task; in practice
// the notify task is bound at Bluedroid init and never moves.
static TaskHandle_t       gBleNotifyTaskHandle = nullptr;

// Forward declarations for live-list page primitive (defined further
// below near g2ShowTextAsList). The SysEvent handler in handleEnvelope
// — which sits well above the primitive's definition — checks whether
// a live page is active and kicks an immediate refresh on double-tap.
// Function indirection avoids forward-declaring file-scope statics.
static bool livePageIsActive();
static void livePageKickRefresh();
static bool liveTextIsActive();
extern void g2StopLiveTextPage();
extern void g2StopLiveListPage();

// Page-swap fast path toggle: when true, worker uses Cmd=7 REBUILD-list
// to swap items in place on an existing list container instead of the
// standard SHUTDOWN+CREATE cycle. **Default ON as of 2026-04-27.**
// REBUILD-list is only attempted when the swap is **pure list → pure list**
// with the **same row count** as we last successfully put on-lens. We
// track both count and shape because `containerIsList` is true for
// list+title compounds too — REBUILD-list wire shape must not be sent
// against a compound container. Changing counts or shape falls back to
// SHUTDOWN+CREATE (missed RebuildResp on 5→2, fw 2.2.0.24).
// Toggle remains runtime-tunable (`g2listrebuild`).
static volatile bool gG2ListRebuildEnabled = true;

enum class HijackListPageShape : uint8_t {
  None = 0,
  PureList,       // Cmd=0 list-only Blocks hijack / PSK_LIST page-swap
  ListWithTitle,  // PSK_LIST_TEXT compound — never use REBUILD-list in swap
};
static size_t              gLastHijackListRowCount  = 0;
static HijackListPageShape gLastHijackListPageShape = HijackListPageShape::None;

static void resetHijackListSwapCache() {
  gLastHijackListRowCount   = 0;
  gLastHijackListPageShape  = HijackListPageShape::None;
}

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
static esp_err_t setTempleConnParams(G2Temple& t, uint16_t min_int, uint16_t max_int);

// Connection-priority intervals (units: 1.25 ms per tick).
//   HIGH     ≈ 11.25-15 ms  → max throughput, default.
//   BALANCED ≈ 40-60 ms     → 3-4× more BLE radio idle time, used during
//                             ring connect attempts (see GlassesPriorityGuard
//                             in G2_Ring.cpp + g2SetAllTemplesConnPriority).
#define G2_CONN_INT_HIGH_MIN     9
#define G2_CONN_INT_HIGH_MAX     12
#define G2_CONN_INT_BALANCED_MIN 32
#define G2_CONN_INT_BALANCED_MAX 48
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
//
// Default containerName lives on the forward decl (not the definition)
// so callers between this decl and the definition can omit the arg.
// C++ rule: a default argument is in scope for callers that have seen
// any declaration carrying it.
static bool sendCreateListAndWait(G2Temple& arm,
                                  const char* const* items, size_t itemCount,
                                  uint32_t widgetId,
                                  const G2ContainerGeom& geom,
                                  const char* containerName = CONTAINER_NAME);
static bool sendCreateTextAndWait(G2Temple& arm,
                                  const char* text,
                                  uint32_t widgetId,
                                  const G2ContainerGeom& geom,
                                  bool eventCapture,
                                  const char* containerName = CONTAINER_NAME);
// Plain non-probe teardown — sends Cmd=9 Shutdown and waits for the
// firmware to settle. Defined far below near the image-probe section.
// Forward-declared here so the dual-pane probe can reuse it without
// reimplementing the Shutdown+settle dance.
static bool tearDownActiveContainer(G2Temple& arm);

// Container state-mutation helpers (defined alongside the sync-wait
// boilerplate further down). Forward-declared here so callers earlier
// in the file (hijackWorkerTask) can use them without reordering.
static void g2NoteCreateSuccess(G2Temple& arm, bool isList,
                                uint32_t widgetId);
static void g2NoteContainerCleared(G2Temple& arm);
static void g2TextViewArm();
static void g2TextViewDisarm();
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
// Example: "EVEN R1_XXXXXX" → last 6 hex = low 3 bytes of the ring's MAC.
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
      else                                                { side = 0; /* preserve isRing — the
                                                                 saved-MAC filter is for
                                                                 G2 temple matching only;
                                                                 ring detection should
                                                                 still work during boot
                                                                 auto-reconnect so a
                                                                 subsequent g2ringconnect
                                                                 / g2devcfg ring has a
                                                                 stashed advert ready. */ }
    } else if (!side) {
      // No explicit filter and the name didn't match. Some firmware
      // revisions advertise as `<unnamed>` for stretches at a time
      // (observed 2026-04-29: missed a temple's whole scan window because
      // its adverts had no name field even though its MAC was visible at
      // strong RSSI). Fall back to checking the BLE-Peers registry's
      // saved MACs for g2-glasses — by codebase convention mac1=LEFT,
      // mac2=RIGHT (see bleSavePeerMac call at end of g2ConnectSync).
      const String& savedL = gBlePeerData[BLE_PEER_G2_GLASSES].mac1;
      const String& savedR = gBlePeerData[BLE_PEER_G2_GLASSES].mac2;
      if (savedL.length() > 0 || savedR.length() > 0) {
        String addr = advertisedDevice.getAddress().toString().c_str();
        if      (savedL.length() > 0 && macEqualsIgnoreCase(addr, savedL)) side = 'L';
        else if (savedR.length() > 0 && macEqualsIgnoreCase(addr, savedR)) side = 'R';
      }
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
      gG2ScanStoppedEarly = true;
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
    temple->audioNotifyChar = nullptr;
    // Container is tied to the plugin task — task dies with the BLE link,
    // so force a fresh CREATE on reconnect.
    temple->containerReady = false;
    if (temple->side == 'R' && wasConnected) {
      // Display arm lost — any in-memory list swap cache is invalid.
      resetHijackListSwapCache();
    }
    // Blocks-hijack state depends on the right temple being reachable to
    // send its Cmd=9 ShutdownPage on lens-close. If R drops (or both
    // drop), drop the flag here so the next reconnect + tap redoes the
    // full CREATE rather than assuming the container still exists.
    // FSM-side: dispatch HijackExit so its state matches the now-dead
    // hijack; the worker also clears containerReady via the apply path.
    if (temple->side == 'R') g2LensSetHijackActive(false);
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
      systemEventPost(SYSEVT_G2_DISCONNECTED, temple->side == 'L' ? "LEFT" : "RIGHT",
                      (!gL.connected && !gR.connected) ? "none left" : "one side up");
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
// Audio (LC3 mic) probe — answers the "do frames flow on 6402?" question.
// =============================================================================
//
// The G2 protocol doc says the left temple's render-notify char (`6402` on
// service `6450`) carries LC3 mic frames after AudioCtrCmd{AudoFuncEn=1}.
// We've never empirically confirmed this on firmware 2.2.0.24. Worse, the
// "left arm is silent on async events" rule (g2-kit-unofficial gotchas)
// might or might not extend to `6402` — gotchas only enumerated the
// command channel (`5402`).
//
// This probe subscribes to `6402` on BOTH arms (so we learn which actually
// emits) and accumulates per-arm counters. Run `g2micon` to send the
// AudioCtrCmd; watch the lines tagged `[G2-MIC]` for live frame logs;
// run `g2micstats` for the cumulative summary. If counts stay 0 after
// `g2micon` runs and the AudioCtrRes ack arrives, the firmware just isn't
// emitting — ESP-SR + G2 mic isn't possible until firmware exposes it.
struct G2MicProbe {
  uint32_t frameCount;       // total notifications received
  uint32_t bytesTotal;       // sum of all notify lengths
  uint32_t byte0CC;          // header 0xCC (normal frame)
  uint32_t byte0CD;          // header 0xCD (session-start / resync)
  uint32_t byte0Other;       // anything else (unexpected)
  uint32_t seqGaps;          // count of non-(prev+1) seq transitions
  uint8_t  lastSeq;
  bool     hasLastSeq;
  uint32_t firstFrameMs;     // millis() at first frame this session
  uint32_t lastFrameMs;
};
static G2MicProbe gMicL{}, gMicR{};
static volatile bool gMicProbeActive = false;
static volatile bool gMicProbeVerbose = false;  // log every frame at INFO when true

// Mic-to-SD recorder state. When gMicRecFile is non-null, every audio
// notify on the chosen arm is appended to that file as raw 205 B
// packets — no LC3 decode, just bytes. The intent is to capture a
// .lc3 file we can pull off SD and decode offline (Phase 1 of the
// audio integration plan; once we know the bytes are valid LC3 we'll
// add an on-device decoder).
//
// Mutex protects the file pointer + counters across the BLE notify
// task (which writes) and the CLI task (which opens / closes). The
// notify side uses a non-blocking take so a stop-in-progress drops at
// most one packet rather than stalling the BLE pipe.
static SemaphoreHandle_t gMicRecMutex   = nullptr;
static File*             gMicRecFile    = nullptr;
static String            gMicRecPath;
static uint32_t          gMicRecBytes   = 0;
static uint32_t          gMicRecPackets = 0;
static uint32_t          gMicRecStartMs = 0;
static char              gMicRecArm     = 'L';   // L emits mic on 2.2.0.24
static constexpr uint32_t kMicRecMaxBytes = 5u * 1024u * 1024u;  // ~20 min @ 4.1 KB/s

static void ensureMicRecMutex() {
  if (!gMicRecMutex) gMicRecMutex = xSemaphoreCreateMutex();
}

// Closes the recorder file and clears state. Caller must hold the
// mutex.
static void micRecCloseLocked(const char* reason) {
  if (!gMicRecFile) return;
  gMicRecFile->close();
  delete gMicRecFile;
  gMicRecFile = nullptr;
  DEBUG_G2F("[G2-MIC-REC] closed %s — %u packets, %u B (%s)",
            gMicRecPath.c_str(), (unsigned)gMicRecPackets,
            (unsigned)gMicRecBytes, reason ? reason : "ok");
}

// ─── Phase 2A: on-device LC3 decode → WAV on SD ───────────────────────
//
// Each 205 B BLE notify is `[ 5 LC3 frames × 40 B ][ 5 B trailer ]`
// (verified in Phase 1 — see G2_PROTOCOL.md "Audio" section).
// liblc3 decodes each 40 B frame to 160 × int16_t PCM samples
// (16 kHz, 10 ms). Five frames per packet → 5 × 160 = 800 samples =
// 1600 bytes of PCM per BLE notification, written straight to a WAV
// file. WAV header written on start with placeholder sizes; on stop
// we seek back and patch the riff-size + data-size fields.
//
// Decoder memory is ~6 KB for 16 kHz / 10 ms — heap-allocated once on
// first start, freed on stop. Mutex serialises the BLE notify task
// (writer) against the CLI task (open/close).
static SemaphoreHandle_t gMicWavMutex   = nullptr;
static File*             gMicWavFile    = nullptr;
static String            gMicWavPath;
static uint32_t          gMicWavBytes   = 0;   // PCM data bytes written (NOT incl. header)
static uint32_t          gMicWavPackets = 0;
static uint32_t          gMicWavStartMs = 0;
static lc3_decoder_t     gMicWavDecoder = nullptr;
static void*             gMicWavDecMem  = nullptr;
static uint32_t          gMicWavDecodeFails = 0;
static uint32_t          gMicWavPlcFrames   = 0;
static constexpr uint32_t kMicWavMaxBytes = 16u * 1024u * 1024u;  // ~8.7 min @ 32 KB/s decoded

static constexpr int kMicLc3FrameUs   = 10000;  // 10 ms
static constexpr int kMicLc3SampleHz  = 16000;  // 16 kHz mono
static constexpr int kMicLc3FrameBytes = 40;    // bytes per LC3 frame in our 32 kbps stream
static constexpr int kMicLc3FramesPerPkt = 5;
static constexpr int kMicLc3SamplesPerFrame =
    kMicLc3SampleHz * kMicLc3FrameUs / 1000000;  // 160
static constexpr int kMicLc3TrailerBytes = 5;   // last 5 B of each 205 B BLE packet

static void ensureMicWavMutex() {
  if (!gMicWavMutex) gMicWavMutex = xSemaphoreCreateMutex();
}

// Write a placeholder 44-byte WAV header. We patch the size fields on
// close. Format: 16 kHz mono 16-bit PCM little-endian.
static void micWavWriteHeader(File& f, uint32_t pcmBytes) {
  const uint32_t riffSize  = 36 + pcmBytes;
  const uint32_t fmtSize   = 16;
  const uint16_t pcmFormat = 1;
  const uint16_t channels  = 1;
  const uint32_t rate      = (uint32_t)kMicLc3SampleHz;
  const uint16_t bits      = 16;
  const uint16_t blockAlign = channels * bits / 8;
  const uint32_t byteRate   = rate * blockAlign;
  const uint8_t header[44] = {
    'R','I','F','F',
    (uint8_t)(riffSize), (uint8_t)(riffSize>>8),
    (uint8_t)(riffSize>>16), (uint8_t)(riffSize>>24),
    'W','A','V','E',
    'f','m','t',' ',
    (uint8_t)(fmtSize), (uint8_t)(fmtSize>>8),
    (uint8_t)(fmtSize>>16), (uint8_t)(fmtSize>>24),
    (uint8_t)(pcmFormat), (uint8_t)(pcmFormat>>8),
    (uint8_t)(channels), (uint8_t)(channels>>8),
    (uint8_t)(rate), (uint8_t)(rate>>8),
    (uint8_t)(rate>>16), (uint8_t)(rate>>24),
    (uint8_t)(byteRate), (uint8_t)(byteRate>>8),
    (uint8_t)(byteRate>>16), (uint8_t)(byteRate>>24),
    (uint8_t)(blockAlign), (uint8_t)(blockAlign>>8),
    (uint8_t)(bits), (uint8_t)(bits>>8),
    'd','a','t','a',
    (uint8_t)(pcmBytes), (uint8_t)(pcmBytes>>8),
    (uint8_t)(pcmBytes>>16), (uint8_t)(pcmBytes>>24),
  };
  f.write(header, sizeof(header));
}

// Patch the riff-size and data-size fields on close so the WAV is
// playable without depending on the original placeholder values.
// Caller must hold gMicWavMutex.
static void micWavPatchSizesLocked() {
  if (!gMicWavFile) return;
  const uint32_t riffSize = 36 + gMicWavBytes;
  uint8_t riff[4] = {
    (uint8_t)(riffSize), (uint8_t)(riffSize>>8),
    (uint8_t)(riffSize>>16), (uint8_t)(riffSize>>24)
  };
  uint8_t dat[4] = {
    (uint8_t)(gMicWavBytes), (uint8_t)(gMicWavBytes>>8),
    (uint8_t)(gMicWavBytes>>16), (uint8_t)(gMicWavBytes>>24)
  };
  if (gMicWavFile->seek(4))  gMicWavFile->write(riff, 4);
  if (gMicWavFile->seek(40)) gMicWavFile->write(dat, 4);
  gMicWavFile->seek(44 + gMicWavBytes);  // restore append position (defensive)
}

static void micWavCloseLocked(const char* reason) {
  if (!gMicWavFile) return;
  micWavPatchSizesLocked();
  gMicWavFile->close();
  delete gMicWavFile;
  gMicWavFile = nullptr;
  if (gMicWavDecMem) {
    free(gMicWavDecMem);
    gMicWavDecMem = nullptr;
  }
  gMicWavDecoder = nullptr;
  DEBUG_G2F("[G2-MIC-WAV] closed %s — %u packets %u PCM bytes "
            "(%u decode-fails, %u PLC) (%s)",
            gMicWavPath.c_str(), (unsigned)gMicWavPackets,
            (unsigned)gMicWavBytes,
            (unsigned)gMicWavDecodeFails, (unsigned)gMicWavPlcFrames,
            reason ? reason : "ok");
}

// ─── Phase 2B: G2 mic → ESP-SR AFE feed ring buffer ───────────────────
//
// Decoded 16 kHz int16 PCM samples accumulate in a PSRAM ring buffer.
// The BLE notify task pushes 5 × 160 = 800 samples per 205 B packet
// (~50 ms of audio); ESP-SR's feed loop drains in 160-sample chunks
// (the AFE feed_chunksize for 16 kHz / 10 ms). 2-second buffer = 64 KB
// — comfortably absorbs WiFi-coexist scheduling jitter.
//
// Mutex protects the ring against the writer (BLE notify task) and the
// reader (SR loop on cmd_exec_task). gMicAfeDataReadySem wakes a
// blocked reader when fresh samples arrive.
static SemaphoreHandle_t  gMicAfeMutex      = nullptr;
static SemaphoreHandle_t  gMicAfeReadySem   = nullptr;
static int16_t*           gMicAfeRing       = nullptr;
static size_t             gMicAfeRingCap    = 0;     // samples
static size_t             gMicAfeRingHead   = 0;
static size_t             gMicAfeRingCount  = 0;
static lc3_decoder_t      gMicAfeDecoder    = nullptr;
static void*              gMicAfeDecMem     = nullptr;
static volatile bool      gMicAfeFeedActive = false;
static uint32_t           gMicAfeOverruns   = 0;
static constexpr size_t   kMicAfeRingCapSamples = 32u * 1024u;  // 2 s @ 16 kHz

static void micAfeRingPushLocked(const int16_t* src, size_t n) {
  if (!gMicAfeRing || gMicAfeRingCap == 0) return;
  size_t freeSpace = gMicAfeRingCap - gMicAfeRingCount;
  if (n > freeSpace) {
    // Reader has fallen behind. Drop oldest samples to make room —
    // worse than skipping the new packet because a stale gap in the
    // middle is worse for AFE than "we lost 50 ms of recent audio".
    size_t drop = n - freeSpace;
    gMicAfeRingHead = (gMicAfeRingHead + drop) % gMicAfeRingCap;
    gMicAfeRingCount -= drop;
    gMicAfeOverruns++;
  }
  size_t tail = (gMicAfeRingHead + gMicAfeRingCount) % gMicAfeRingCap;
  size_t first = n;
  if (tail + first > gMicAfeRingCap) {
    first = gMicAfeRingCap - tail;
  }
  memcpy(&gMicAfeRing[tail], src, first * sizeof(int16_t));
  if (n > first) {
    memcpy(&gMicAfeRing[0], &src[first], (n - first) * sizeof(int16_t));
  }
  gMicAfeRingCount += n;
}

bool g2MicSetAfeFeedActive(bool on) {
  if (!gMicAfeMutex)    gMicAfeMutex    = xSemaphoreCreateMutex();
  if (!gMicAfeReadySem) gMicAfeReadySem = xSemaphoreCreateBinary();
  if (!gMicAfeMutex || !gMicAfeReadySem) return false;

  xSemaphoreTake(gMicAfeMutex, portMAX_DELAY);
  if (on && !gMicAfeFeedActive) {
    if (!gMicAfeRing) {
      gMicAfeRing = (int16_t*)heap_caps_malloc(
          kMicAfeRingCapSamples * sizeof(int16_t),
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!gMicAfeRing) {
        xSemaphoreGive(gMicAfeMutex);
        DEBUG_G2F("[G2-MIC-AFE] ring alloc failed (%u B)",
                  (unsigned)(kMicAfeRingCapSamples * sizeof(int16_t)));
        return false;
      }
      gMicAfeRingCap = kMicAfeRingCapSamples;
    }
    if (!gMicAfeDecoder) {
      unsigned sz = lc3_decoder_size(kMicLc3FrameUs, kMicLc3SampleHz);
      // Decoder workspace is CPU-only — prefer PSRAM to preserve internal DRAM for stacks/BLE.
      gMicAfeDecMem = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!gMicAfeDecMem) {
        gMicAfeDecMem = malloc(sz);
      }
      if (gMicAfeDecMem) {
        gMicAfeDecoder = lc3_setup_decoder(kMicLc3FrameUs, kMicLc3SampleHz,
                                           /*sr_pcm_hz*/ 0, gMicAfeDecMem);
      }
      if (!gMicAfeDecoder) {
        free(gMicAfeDecMem);
        gMicAfeDecMem = nullptr;
        xSemaphoreGive(gMicAfeMutex);
        DEBUG_G2F("[G2-MIC-AFE] decoder setup failed");
        return false;
      }
    }
    gMicAfeRingHead = 0;
    gMicAfeRingCount = 0;
    gMicAfeOverruns = 0;
    gMicRecArm = 'L';
    gMicAfeFeedActive = true;
    DEBUG_G2F("[G2-MIC-AFE] feed ON (ring %u samples / %u B)",
              (unsigned)gMicAfeRingCap,
              (unsigned)(gMicAfeRingCap * sizeof(int16_t)));
  } else if (!on && gMicAfeFeedActive) {
    gMicAfeFeedActive = false;
    gMicAfeRingHead = 0;
    gMicAfeRingCount = 0;
    DEBUG_G2F("[G2-MIC-AFE] feed OFF (cumulative overruns=%u)",
              (unsigned)gMicAfeOverruns);
    // Keep ring + decoder allocated for fast re-arm.
  }
  xSemaphoreGive(gMicAfeMutex);
  // Wake any blocked reader so it can observe the state change.
  if (gMicAfeReadySem) xSemaphoreGive(gMicAfeReadySem);
  return true;
}

bool g2MicAfeFeedIsActive() { return gMicAfeFeedActive; }

size_t g2MicReadPcmSamples(int16_t* out, size_t capSamples, uint32_t timeoutMs) {
  if (!out || capSamples == 0) return 0;
  if (!gMicAfeMutex || !gMicAfeReadySem) return 0;
  const uint32_t deadline = millis() + timeoutMs;
  for (;;) {
    xSemaphoreTake(gMicAfeMutex, portMAX_DELAY);
    if (!gMicAfeFeedActive) {
      xSemaphoreGive(gMicAfeMutex);
      return 0;
    }
    if (gMicAfeRingCount > 0) {
      size_t toRead = (gMicAfeRingCount < capSamples)
                        ? gMicAfeRingCount : capSamples;
      size_t first = toRead;
      if (gMicAfeRingHead + first > gMicAfeRingCap) {
        first = gMicAfeRingCap - gMicAfeRingHead;
      }
      memcpy(out, &gMicAfeRing[gMicAfeRingHead], first * sizeof(int16_t));
      if (toRead > first) {
        memcpy(&out[first], &gMicAfeRing[0],
               (toRead - first) * sizeof(int16_t));
      }
      gMicAfeRingHead = (gMicAfeRingHead + toRead) % gMicAfeRingCap;
      gMicAfeRingCount -= toRead;
      xSemaphoreGive(gMicAfeMutex);
      return toRead;
    }
    xSemaphoreGive(gMicAfeMutex);
    const uint32_t now = millis();
    if (now >= deadline) return 0;
    // Block until a writer signals (or 20 ms tick — defensive).
    xSemaphoreTake(gMicAfeReadySem, pdMS_TO_TICKS(20));
  }
}

size_t   g2MicAfeRingDepth()    { return gMicAfeRingCount; }
uint32_t g2MicAfeOverrunCount() { return gMicAfeOverruns; }

// Decode one BLE packet's worth of audio (200 B = 5 LC3 frames) into
// 800 int16 samples and append to the WAV. Caller must already hold
// the WAV mutex AND have a valid decoder + open file.
static bool micWavWritePacketLocked(const uint8_t* pkt) {
  int16_t pcm[kMicLc3FramesPerPkt * kMicLc3SamplesPerFrame];
  for (int i = 0; i < kMicLc3FramesPerPkt; i++) {
    const uint8_t* in = pkt + i * kMicLc3FrameBytes;
    int16_t* out = pcm + i * kMicLc3SamplesPerFrame;
    int rc = lc3_decode(gMicWavDecoder, in, kMicLc3FrameBytes,
                        LC3_PCM_FORMAT_S16, out, /*stride*/ 1);
    if (rc < 0) {
      gMicWavDecodeFails++;
      memset(out, 0, kMicLc3SamplesPerFrame * sizeof(int16_t));
    } else if (rc == 1) {
      gMicWavPlcFrames++;
    }
  }
  size_t n = gMicWavFile->write(reinterpret_cast<const uint8_t*>(pcm),
                                sizeof(pcm));
  gMicWavBytes   += (uint32_t)n;
  gMicWavPackets += 1;
  return n == sizeof(pcm);
}

static void resetMicProbeStats(G2MicProbe& m) {
  m.frameCount = 0;
  m.bytesTotal = 0;
  m.byte0CC = m.byte0CD = m.byte0Other = 0;
  m.seqGaps = 0;
  m.hasLastSeq = false;
  m.lastSeq = 0;
  m.firstFrameMs = 0;
  m.lastFrameMs = 0;
}

static void handleAudioNotify(G2Temple& t, const uint8_t* data, size_t len) {
  G2MicProbe& m = (t.side == 'L') ? gMicL : gMicR;
  uint32_t now = millis();
  m.frameCount++;
  m.bytesTotal += (uint32_t)len;
  if (m.frameCount == 1) m.firstFrameMs = now;
  m.lastFrameMs = now;

  if (len >= 1) {
    if      (data[0] == 0xCC) m.byte0CC++;
    else if (data[0] == 0xCD) m.byte0CD++;
    else                      m.byte0Other++;
  }
  if (len >= 2) {
    uint8_t seq = data[1];
    if (m.hasLastSeq) {
      uint8_t expected = (uint8_t)(m.lastSeq + 1);
      if (seq != expected) m.seqGaps++;
    }
    m.lastSeq = seq;
    m.hasLastSeq = true;
  }

  // SD recording — append the raw 205 B packet to the open file when
  // the recorder is active for this arm. Non-blocking mutex take so a
  // CLI-side close can't stall the BLE notify task; if we miss a
  // packet during a close window, that's fine.
  if (gMicRecFile && t.side == gMicRecArm && gMicRecMutex) {
    if (xSemaphoreTake(gMicRecMutex, 0) == pdTRUE) {
      if (gMicRecFile && gMicRecBytes < kMicRecMaxBytes) {
        size_t w = gMicRecFile->write(data, len);
        gMicRecBytes   += (uint32_t)w;
        gMicRecPackets += 1;
        if (gMicRecBytes >= kMicRecMaxBytes) {
          micRecCloseLocked("cap reached");
        }
      }
      xSemaphoreGive(gMicRecMutex);
    }
  }

  // On-device LC3 → WAV decode (Phase 2A). Same packet shape as the
  // raw recorder, but here we strip the 5 B trailer, decode the 5
  // LC3 frames inline, and write 1600 B of PCM to a WAV file. Skips
  // packets that aren't the expected 205 B size (defensive).
  if (gMicWavFile && t.side == gMicRecArm && gMicWavMutex &&
      len == 205 && gMicWavDecoder) {
    if (xSemaphoreTake(gMicWavMutex, 0) == pdTRUE) {
      if (gMicWavFile && gMicWavBytes < kMicWavMaxBytes && gMicWavDecoder) {
        // The first 200 bytes are 5 × 40 B LC3 frames. Last 5 bytes
        // are the metadata trailer — discard.
        micWavWritePacketLocked(data);
        if (gMicWavBytes >= kMicWavMaxBytes) {
          micWavCloseLocked("cap reached");
        }
      }
      xSemaphoreGive(gMicWavMutex);
    }
  }

  // Phase 2B: ESP-SR AFE feed path. Decode 5 LC3 frames inline, push
  // 800 int16 samples to the ring buffer that ESP-SR's loop drains.
  // Independent of the WAV writer above — both can run together
  // (e.g. record-while-listening for ground-truth comparisons).
  if (gMicAfeFeedActive && t.side == gMicRecArm && gMicAfeMutex &&
      len == 205 && gMicAfeDecoder) {
    if (xSemaphoreTake(gMicAfeMutex, 0) == pdTRUE) {
      if (gMicAfeFeedActive && gMicAfeDecoder && gMicAfeRing) {
        int16_t pcm[kMicLc3FramesPerPkt * kMicLc3SamplesPerFrame];
        bool decOk = true;
        for (int i = 0; i < kMicLc3FramesPerPkt; i++) {
          const uint8_t* in = data + i * kMicLc3FrameBytes;
          int16_t* out = pcm + i * kMicLc3SamplesPerFrame;
          int rc = lc3_decode(gMicAfeDecoder, in, kMicLc3FrameBytes,
                              LC3_PCM_FORMAT_S16, out, /*stride*/ 1);
          if (rc < 0) {
            // Decode error — zero this frame and keep going so the AFE
            // sees continuous time rather than a missing chunk.
            memset(out, 0, kMicLc3SamplesPerFrame * sizeof(int16_t));
            decOk = false;
          }
        }
        (void)decOk;
        micAfeRingPushLocked(pcm,
                             kMicLc3FramesPerPkt * kMicLc3SamplesPerFrame);
      }
      xSemaphoreGive(gMicAfeMutex);
      // Wake any reader blocked on the ready semaphore. Outside the
      // mutex so the reader can immediately retake it.
      if (gMicAfeReadySem) xSemaphoreGive(gMicAfeReadySem);
    }
  }

  // Detail log on the first 5 frames (so we know shape/cadence at a glance),
  // then a periodic stats line every 50 frames (=1s of audio at 50fps), and
  // every frame in verbose mode.
  bool detail = m.frameCount <= 5 || gMicProbeVerbose;
  bool stats  = (m.frameCount % 50) == 0;
  if (detail) {
    DEBUG_G2F("[G2-MIC-%c] frame #%u len=%u byte0=0x%02X seq=%u "
              "(CC=%u CD=%u other=%u gaps=%u)",
              t.side, (unsigned)m.frameCount, (unsigned)len,
              (unsigned)(len ? data[0] : 0), (unsigned)(len > 1 ? data[1] : 0),
              (unsigned)m.byte0CC, (unsigned)m.byte0CD,
              (unsigned)m.byte0Other, (unsigned)m.seqGaps);
  } else if (stats) {
    uint32_t elapsedMs = m.lastFrameMs - m.firstFrameMs;
    uint32_t fps = elapsedMs ? (m.frameCount * 1000u) / elapsedMs : 0;
    DEBUG_G2F("[G2-MIC-%c] %u frames %u B avgLen=%u ~%u fps "
              "(CC=%u CD=%u other=%u gaps=%u)",
              t.side, (unsigned)m.frameCount, (unsigned)m.bytesTotal,
              (unsigned)(m.frameCount ? m.bytesTotal / m.frameCount : 0),
              (unsigned)fps,
              (unsigned)m.byte0CC, (unsigned)m.byte0CD,
              (unsigned)m.byte0Other, (unsigned)m.seqGaps);
  }
}

static void audioNotifyThunkL(BLERemoteCharacteristic* /*c*/, uint8_t* data,
                              size_t len, bool /*isNotify*/) {
  handleAudioNotify(gL, data, len);
}
static void audioNotifyThunkR(BLERemoteCharacteristic* /*c*/, uint8_t* data,
                              size_t len, bool /*isNotify*/) {
  handleAudioNotify(gR, data, len);
}

// Look up the audio notify char on this temple and subscribe. Returns true
// on success. Safe to call multiple times — re-registering the callback is
// idempotent. Called once per arm at connect time.
static bool subscribeAudioNotify(G2Temple& t) {
  if (!t.client) return false;
  BLERemoteService* svc = t.client->getService(BLEUUID(DIAG_SVC_6450));
  if (!svc) return false;
  BLERemoteCharacteristic* ch = svc->getCharacteristic(BLEUUID(CHAR_AUDIO_NOTIFY));
  if (!ch || !ch->canNotify()) {
    DEBUG_G2F("[G2-%c] audio char 6402 not available (svc=%p ch=%p notify=%d)",
              t.side, svc, ch, ch ? (ch->canNotify() ? 1 : 0) : 0);
    return false;
  }
  t.audioNotifyChar = ch;
  ch->registerForNotify((t.side == 'L') ? audioNotifyThunkL : audioNotifyThunkR);
  DEBUG_G2F("[G2-%c] audio probe subscribed on 6402", t.side);
  return true;
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
  // Latch the notify task handle on first ever call so the deadlock
  // guard in the sendCreate*AndWait helpers can compare against it.
  // Bluedroid binds this callback to a single task at init, so the
  // first capture is permanent for the life of the connection — and
  // even across reconnects the task is the same.
  if (!gBleNotifyTaskHandle) {
    gBleNotifyTaskHandle = xTaskGetCurrentTaskHandle();
  }

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
          // Counterpart of SYSEVT_G2_NOT_WORN: plugin woke = picked up/worn.
          systemEventPost(SYSEVT_G2_WORN, t.side == 'L' ? "LEFT" : "RIGHT");
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
  // 9B: SysEvent { field_3 = nested { f1=<code>, f2=<src> } }. The code
  // varint is multi-byte so we walk past its continuation bytes to find
  // the f2 (`10`) tag rather than indexing at fixed offsets.
  //
  // Observed (code, src) combinations for code=224 (widget-lifecycle):
  //   src=7   — wake-word ("Hey Even") fired inside our hijacked widget.
  //              User-activity-class signal.
  //   src=33  — ring connect/disconnect status banner appeared. NOT a
  //              user gesture; classified as G2_EVENT_SYS_NOTIFY so the
  //              widget-auto-exit code doesn't tear down our menu when
  //              the ring blips reconnect. (Found 2026-05-02 via labelled
  //              capture during ring connect/disconnect cycles.)
  //   src=34  — long-press both temples (silent-mode toggle gesture).
  //              User-activity-class signal.
  if (innerLen == 5 && len == 9 && payload[4] == 0x08) {
    const uint32_t code = peekVarint(payload, len, 5);
    size_t off = 5;
    while (off < len && (payload[off] & 0x80)) off++;
    off++;  // step past last byte of the varint
    uint32_t src = 0;
    if (off + 1 < len && payload[off] == 0x10) {
      src = peekVarint(payload, len, off + 1);
    }
    const bool isRingBanner = (code == 224 && src == 33);
    if (outHint) {
      if (isRingBanner) {
        snprintf(outHint, outHintCap,
                 "ring-banner code=%u src=%u (sys-notify, not user)",
                 (unsigned)code, (unsigned)src);
      } else {
        snprintf(outHint, outHintCap, "state-9B code=%u src=%u",
                 (unsigned)code, (unsigned)src);
      }
    }
    return isRingBanner ? G2_EVENT_SYS_NOTIFY : G2_EVENT_USER_ACTIVITY;
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
      // Camera-stream list dispatch. The stream uses a list+image
      // compound CREATE with container name "lstCam"; rows are
      //   idx 0 = "<- Back"     bit 0x1
      //   idx 1 = "Snapshot"    bit 0x2
      //   idx 2 = "Settings >>" bit 0x4
      // Set the bit; worker drains it on its next loop iteration. We
      // only respond when gCamStreamActive — outside that window any
      // late lstCam echo is ignored. Returns early so the lstCam tap
      // doesn't fall through to the gImgProbeHoldActive / "app"
      // dispatch below (different containers, different intent).
      if (etype == 0 && strcmp(cname, "lstCam") == 0) {
        if (gCamStreamActive) {
          // Refresh the 60 s hijack safety-timeout on every real stream
          // control tap — same rationale as the map page / 'app' menu. The
          // frame loop also feeds it per successful push; this is the instant
          // path so a tap landing between frames still counts.
          if (g2FsmHijackActive()) gHijackStartedMs = millis();
          uint32_t bit = (idx == 0) ? 0x1u :
                         (idx == 1) ? 0x2u :
                         (idx == 2) ? 0x4u : 0u;
          if (bit) {
            gCamStreamPendingTap |= bit;
            DEBUG_G2F("[G2] CamStream tap: idx=%u → bit 0x%X",
                      (unsigned)idx, (unsigned)bit);
          } else {
            DEBUG_G2F("[G2] CamStream tap: idx=%u out of range — ignored",
                      (unsigned)idx);
          }
        } else {
          DEBUG_G2F("[G2] CamStream tap on 'lstCam' but stream not active — ignored");
        }
        return;
      }
#if ENABLE_MAPS
      // Interactive Maps page list dispatch (container 'lstMap'). Rows:
      //   0 Back · 1 Zoom In · 2 Zoom Out · 3 Reset View · 4 Recenter.
      // Set the row's bit; the worker drains gMapPagePendingTap each loop.
      // Only while gMapPageActive so late echoes after teardown are ignored.
      if (etype == 0 && strcmp(cname, "lstMap") == 0) {
        if (gMapPageActive && idx < 32) {
          // Refresh the 60 s hijack safety-timeout on every real tap. The
          // watchdog (heartbeat task, HIJACK_SAFETY_MS) measures from the last
          // input-refreshed timestamp; without this it force-exits the map
          // page ~60 s after the Apps→Maps tap no matter how actively the user
          // is zooming/panning — surfacing as a spurious "kicked back to the
          // menu" mid-use. Mirrors the 'app'-menu and probe-hold paths below;
          // g2FsmHijackActive() is true in the ImageProbing state.
          if (g2FsmHijackActive()) gHijackStartedMs = millis();
          gMapPagePendingTap |= (1u << idx);
          DEBUG_G2F("[G2] MapPage tap: idx=%u", (unsigned)idx);
        }
        return;
      }
#endif
      // Image-probe hold dismissal — single-tap path. When a probe surfaces
      // a List widget (e.g. mixed list+image probes Q16/Q17/Q18), single
      // taps on rows arrive here as ListEvent CLICK(0) instead of the
      // SysEvent DOUBLE_CLICK channel watched above. Treat any list tap
      // during probe-hold as the dismiss gesture so the user doesn't have
      // to double-tap to escape.
      if (gImgProbeHoldActive && etype == 0) {
        if (g2FsmHijackActive()) gHijackStartedMs = millis();
        DEBUG_G2F("[G2] IMG probe hold: ListEvent CLICK on '%s' idx=%u → dismiss",
                  cname, (unsigned)idx);
        gImgProbeHoldTapPending = true;
        gImgProbeAbort          = true;
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
          // Refresh the safety-timeout window on every real user tap.
          // The 60 s cap exists to recover from stuck hijacks where
          // we've lost track of state; an actively-tapping user is, by
          // definition, not stuck. Idle hijacks still time out
          // normally because gHijackStartedMs only advances on input.
          if (g2FsmHijackActive()) gHijackStartedMs = millis();
          // Defer the actual handler AND the "Hijack tap: …" log line
          // off the Bluedroid notify-task stack. BTC_TASK's stack is
          // tight (~3-4 KB) and the inline BROADCAST_PRINTF + handler
          // path pushed it over the canary on first tap (2026-05-03).
          // The persistent tap-dispatcher worker prints + runs the
          // handler from a normal task context with full headroom.
          tapDispatcherEnqueue(idx, iname);
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
        if (g2FsmHijackActive()) gHijackStartedMs = millis();
        // Paginated views: map gestures directionally —
        //   etype==0 CLICK         → next page (lens tap, no direction)
        //   etype==1 SCROLL_TOP    → previous page (ring scroll up)
        //   etype==2 SCROLL_BOTTOM → next page (ring scroll down)
        // Single-page views (tapFn=null) preserve legacy "any tap exits".
        // No DOUBLE_CLICK on this channel — that lives in SysEvent.
        if (gTextViewTapFn) {
          // Both temples deliver the same TextEvent a few ms apart. The
          // tap handler (e.g. Settings JSON navJsonPage) mutates logical
          // page state before g2ShowTextPage — if the second copy runs
          // after the first swap started but before CREATE ack, the
          // second g2ShowTextPage fails while the index has already moved
          // twice (SCROLL_TOP especially: stuck / double-skip on next
          // scroll). Ignore duplicate nav while PageSwapping.
          if (g2FsmPageSwapping()) {
            DEBUG_G2F("[G2] TEXT view: TextEvent %s(%u) — ignoring page-%s "
                      "(swap in flight; duplicate L+R)",
                      osEventTypeName(etype), (unsigned)etype,
                      etype == 1 ? "prev" : "next");
          } else {
            const G2TapKind kind = (etype == 1) ? G2_TAP_PAGE_PREV
                                                : G2_TAP_PAGE_NEXT;
            DEBUG_G2F("[G2] TEXT view: TextEvent %s(%u) → page-%s handler",
                      osEventTypeName(etype), (unsigned)etype,
                      kind == G2_TAP_PAGE_PREV ? "prev" : "next");
            gTextViewActivatedMs = millis();
            gTextViewTapFn(kind);
          }
        } else if (gTextViewExitFn) {
          // Same race guard as the SysEvent CLICK branch below — pages
          // with their own list child + custom handleTap (MIC detail
          // and friends) own the tap; firing the exit handler here
          // would flip gHijackPage to MAIN before the ListEvent
          // dispatcher routes the tap to p->handleTap.
          const G2HijackPage cur = g2GetHijackPage();
          bool deferred = false;
          if (cur != G2_HIJACK_PAGE_MAIN) {
            const G2PageModule* p = g2FindPageByHijackPage(cur);
            if (p && p->handleTap) {
              DEBUG_G2F("[G2] TEXT view: TextEvent CLICK — deferring to "
                        "ListEvent path (page=%s has custom handleTap)",
                        p->name);
              deferred = true;
            }
          }
          if (!deferred) {
            DEBUG_G2F("[G2] TEXT view: TextEvent CLICK → deferring exit handler to g2_tap_disp");
            // Snapshot fn before clearing state. State-clear happens on
            // BTC synchronously so subsequent notify events bail out via
            // the gTextViewActive guard while the exit work runs on the
            // dispatcher worker (25 KB stack vs BTC_TASK's 4 KB).
            void (*fn)() = gTextViewExitFn;
            gTextViewActive = false;
            gTextViewExitFn = nullptr;
            gTextViewTapFn  = nullptr;
            tapDispatcherEnqueueExit(fn);
          }
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

      // FG_ENTER (4) / FG_EXIT (5) refresh the hijack safety timer.
      // These fire when the firmware overlays/dismisses its own native UI
      // on top of our widget — most importantly the tap-and-hold "Exit?"
      // yes/no dialog. The user is actively interacting at that moment,
      // so we don't want the 60 s safety to fire the moment they click
      // "no" (which is what was tearing down the menu and surfacing
      // "Connection lost"). Other event types are either user gestures
      // (0..3, refreshed downstream) or firmware teardown (7, must NOT
      // refresh).
      if (g2FsmHijackActive() && (etype == 4 || etype == 5)) {
        gHijackStartedMs = millis();
      }
      // Track firmware-overlay foreground state so the line-2451
      // USER_ACTIVITY auto-exit can ignore the firmware's gesture-
      // detection events while its dialog is up. Stamp the FG_EXIT
      // time so firmwareOverlayActive() can also gate the trailing
      // wake-word USER_ACTIVITY that fires ~200 ms after dismiss.
      if (etype == 4) {
        gFirmwareOverlayUp        = true;
        gFirmwareOverlayUpSinceMs = millis();
      } else if (etype == 5) {
        gFirmwareOverlayUp         = false;
        gFirmwareOverlayLastExitMs = millis();
      }

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
        if (g2FsmHijackActive()) gHijackStartedMs = millis();
        DEBUG_G2F("[G2] IMG probe hold: SysEvent %s(%u) src=%u → dismiss",
                  osEventTypeName(etype), (unsigned)etype, (unsigned)src);
        gImgProbeHoldTapPending = true;
        gImgProbeAbort          = true;
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
        if (g2FsmHijackActive()) gHijackStartedMs = millis();
        DEBUG_G2F("[G2] live-page: SysEvent DOUBLE_CLICK src=2 → "
                  "kicking refresh");
        livePageKickRefresh();
        return;
      }

      if (gTextViewActive &&
          (etype == 0 || etype == 1 || etype == 2 || etype == 3)) {
        // Any input gesture on a TEXT view refreshes the hijack
        // safety-timeout — same rationale as the TextEvent path above.
        if (g2FsmHijackActive()) gHijackStartedMs = millis();
        // Paginated views (tapFn != null) map gestures directionally:
        //   CLICK(0)         → next page (lens tap, no direction)
        //   SCROLL_TOP(1)    → previous page (ring scroll up)
        //   SCROLL_BOTTOM(2) → next page (ring scroll down)
        //   DOUBLE_CLICK(3)  → exit
        // Single-page views (tapFn=null) preserve legacy "any tap exits".
        const bool isExitGesture = (etype == 3);  // DOUBLE_CLICK
        if (gTextViewTapFn && !isExitGesture) {
          if (g2FsmPageSwapping()) {
            DEBUG_G2F("[G2] TEXT view: SysEvent %s(%u) src=%u — ignoring "
                      "page-%s (swap in flight; duplicate L+R)",
                      osEventTypeName(etype), (unsigned)etype,
                      (unsigned)src,
                      etype == 1 ? "prev" : "next");
            return;
          }
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
          // Compound pages with their own list child for navigation
          // (MIC detail's "<- Sensors" + toggle row, future similar
          // pages) register a custom handleTap. For those, the
          // ListEvent CLICK path owns the tap — firing the exit
          // handler here would race ahead and flip gHijackPage to
          // MAIN before the tap dispatcher gets a chance to call
          // p->handleTap, so the user's tap on (e.g.) the toggle
          // row would silently land on a main-menu item instead.
          // Defer to the list path by skipping the exit shortcut
          // when the current hijack page has a registered
          // handleTap. Single-tap dismiss for those pages comes
          // from their own back row (idx 0), not this fallback.
          // DOUBLE_CLICK(3) still routes here so a ring double-tap
          // can exit even when the page has a handleTap.
          const G2HijackPage cur = g2GetHijackPage();
          if (etype != 3 && cur != G2_HIJACK_PAGE_MAIN) {
            const G2PageModule* p = g2FindPageByHijackPage(cur);
            if (p && p->handleTap) {
              DEBUG_G2F("[G2] TEXT view: SysEvent %s(%u) src=%u — "
                        "deferring to ListEvent path (page=%s has "
                        "custom handleTap)",
                        osEventTypeName(etype), (unsigned)etype,
                        (unsigned)src, p->name);
              return;
            }
          }
          DEBUG_G2F("[G2] TEXT view: SysEvent %s(%u) src=%u → deferring exit "
                    "handler to g2_tap_disp", osEventTypeName(etype),
                    (unsigned)etype, (unsigned)src);
          // Snapshot fn and clear state synchronously on BTC; heavy work
          // (file chooser redraw, page swap, FS reads) runs on the
          // dispatcher worker. Inline fn() here used to overflow the
          // 4 KB BTC stack — see tapDispatcherEnqueueExit().
          void (*fn)() = gTextViewExitFn;
          gTextViewActive = false;
          gTextViewExitFn = nullptr;
          gTextViewTapFn  = nullptr;
          tapDispatcherEnqueueExit(fn);
          return;
        }
      }

      if (etype == 7) {  // SYSTEM_EXIT_EVENT
        // If we initiated a Shutdown ourselves within the last ~2 s
        // (page swap in flight), the firmware is just echoing our own
        // teardown. The hijack itself isn't ending — we're about to
        // CREATE a fresh widget with new content. Don't clear state.
        // Phase 2: predicate is FSM-state-aware (state==PageSwapping)
        // OR'd with the legacy 2 s wall-clock as a fallback.
        if (isExpectedEchoWindow("sysExit")) {
          DEBUG_G2F("[G2-%c] SYSTEM_EXIT during our own page-swap "
                    "(reason=%u) — ignoring, hijack stays active",
                    t.side, (unsigned)reason);
          // Don't fall through. The page-swap worker is the source of
          // truth for state across this transition.
        } else {
          // Real firmware-driven teardown (user picked "Yes" in the
          // tap-and-hold "Exit?" dialog, or some other firmware-side
          // event). Whether or not the FSM thinks hijack is active,
          // we MUST stop any live-text / live-page worker here —
          // otherwise the worker's next tick re-CREATEs the widget
          // we just got torn down, and the user's exit looks like
          // "stuck on the page that won't go away" (observed
          // 2026-05-01 on the Sensors page: tap-and-hold → Yes
          // dismissed the firmware dialog but a 2 s later the
          // live-text tick resurrected the TEXT widget; happened in a
          // loop until the user power-cycled the lenses). The buildFn
          // live-text path doesn't go through the FSM (g2ShowText is
          // legacy direct CREATE-text), so it's the "no hijack
          // active" branch below that fires for that case — but the
          // worker is still ticking and needs to be told to stop.
          if (liveTextIsActive()) {
            DEBUG_G2F("[G2-%c] SYSTEM_EXIT (reason=%u) — stopping "
                      "live-text worker so the next tick doesn't "
                      "resurrect the page", t.side, (unsigned)reason);
            g2StopLiveTextPage();
          }
          if (livePageIsActive()) {
            DEBUG_G2F("[G2-%c] SYSTEM_EXIT (reason=%u) — stopping "
                      "live-page worker so the next tick doesn't "
                      "resurrect the page", t.side, (unsigned)reason);
            g2StopLiveListPage();
          }
          // Firmware has torn down our container. Don't keep thinking
          // we have a hijack live — clear state so the next
          // MenuStartUp tap starts clean. Log the widget's lifetime
          // so timeout patterns become obvious.
          if (g2FsmHijackActive()) {
            const uint32_t lifeMs = (gHijackStartedMs > 0)
                                    ? (millis() - gHijackStartedMs) : 0;
            BROADCAST_PRINTF("[G2] Firmware tore down widget "
                             "(SYSTEM_EXIT reason=%u) after %u.%03us alive "
                             "— on-lens shows 'Connection lost'. "
                             "Clearing hijack state.",
                             (unsigned)reason,
                             (unsigned)(lifeMs / 1000),
                             (unsigned)(lifeMs % 1000));
            gHijackStartedMs = 0;
            gR.containerReady = false;
            g2LensSetHijackActive(false);
            resetHijackListSwapCache();
            g2LensClearContainer();
            g2LensClearOverlay();
            g2PushStatusEvent("fw-system-exit");
            systemEventPost(SYSEVT_G2_HIJACK_EXITED, "fw-system-exit");
            g2RingDump("firmware SYSTEM_EXIT");
          } else {
            DEBUG_G2F("[G2-%c] SYSTEM_EXIT while no hijack was active "
                      "(reason=%u)", t.side, (unsigned)reason);
          }
        }
      }
    }
  }
}

[[maybe_unused]] static const char* eventName(G2EventType e) {
  switch (e) {
    case G2_EVENT_USER_ACTIVITY: return "USER_ACTIVITY";
    case G2_EVENT_SYS_NOTIFY:    return "SYS_NOTIFY";
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
    // Firmware-overlay gate: when the firmware has its own native UI
    // foregrounded over our widget (tap-and-hold "Exit?" yes/no dialog
    // is the typical case — FG_ENTER set the flag), the firmware emits
    // its own USER_ACTIVITY events (state-9B code=224 src=34) for the
    // gesture detection and any taps inside the dialog. Those events
    // are NOT user input on our widget — they're firmware-internal.
    // Without this guard, the long-press tears down our text view the
    // instant the dialog appears, leaving the lens with the dialog and
    // a blank background, and any subsequent CREATE-list page-swap
    // races the dialog and times out.
    if (firmwareOverlayActive()) {
      DEBUG_G2F("[G2] TEXT view: ignoring user-activity (firmware overlay "
                "is foregrounded — gesture event is for the firmware UI)");
      return;
    }
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
      if (g2FsmPageSwapping()) {
        DEBUG_G2F("[G2] TEXT view: user-activity — ignoring page-next "
                  "(swap in flight; duplicate L+R)");
      } else {
        DEBUG_G2F("[G2] TEXT view: user-activity → page-next handler");
        gTextViewActivatedMs = now;
        gTextViewTapFn(G2_TAP_PAGE_NEXT);
      }
    } else if (gTextViewExitFn) {
      // Same race guard as the SysEvent CLICK / TextEvent CLICK
      // branches: pages with their own list child for navigation
      // (MIC detail's "<- Sensors" + toggle row, future similar
      // pages) own the tap via their custom handleTap. Wake-word /
      // tap USER_ACTIVITY shouldn't fire the exit shortcut and
      // bounce the user back to MAIN — observed 2026-05-09: after
      // toggling the mic on the MIC detail page, the firmware
      // emits a wake-word USER_ACTIVITY (state-7B code=224) ~370 ms
      // after the post-toggle CREATE acks, which used to invoke
      // liveTextExitToHijackMenu and dump the user back to the
      // main hijack list. Defer to the page's own back row
      // (idx 0) for dismissal when the page registers a handleTap.
      const G2HijackPage cur = g2GetHijackPage();
      bool deferred = false;
      if (cur != G2_HIJACK_PAGE_MAIN) {
        const G2PageModule* p = g2FindPageByHijackPage(cur);
        if (p && p->handleTap) {
          DEBUG_G2F("[G2] TEXT view: user-activity — deferring to "
                    "ListEvent path (page=%s has custom handleTap)",
                    p->name);
          deferred = true;
        }
      }
      if (!deferred) {
        DEBUG_G2F("[G2] TEXT view: user-activity → deferring exit handler to g2_tap_disp");
        void (*fn)() = gTextViewExitFn;
        gTextViewActive = false;
        gTextViewExitFn = nullptr;
        gTextViewTapFn  = nullptr;
        tapDispatcherEnqueueExit(fn);
      }
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
    // timeout"). Phase 2: predicate is FSM-state-aware
    // (state==PageSwapping) OR'd with the legacy 2 s wall-clock as a
    // fallback for paths the Phase 1 model doesn't yet route through
    // PageSwapping (image probes — Phase 6 promotes those to a state).
    if (isExpectedEchoWindow("displayOff")) {
      DEBUG_G2F("[G2] DISPLAY_OFF during our own page-swap — ignoring "
                "(hijack stays active, worker handles state)");
      return;
    }
    if (gL.containerReady || gR.containerReady) {
      DEBUG_G2F("[G2] DISPLAY_OFF — invalidating cached containerReady");
    }
    gL.containerReady = false;
    gR.containerReady = false;
    resetHijackListSwapCache();
    g2LensClearContainer();
    g2LensClearOverlay();
    // If a probe-hold session is active (camera viewer / stream / image
    // probe loop), the firmware just tore down the container we were
    // pushing into. Signal the worker to bail out — same flags the
    // user-tap dismiss path uses. Without this the streamer kept
    // capturing+decoding+pushing into a dead container, wasting BLE
    // bandwidth and PSRAM allocs every frame until its 5-min safety
    // cap expired.
    if (gImgProbeHoldActive) {
      DEBUG_G2F("[G2] DISPLAY_OFF — aborting active probe-hold session");
      gImgProbeHoldTapPending = true;
      gImgProbeAbort          = true;
    }
    // Blocks-hijack exit path — fire ShutdownPage from whichever arm
    // wins the dedup race so the firmware frees its plugin-task state
    // cleanly. Guarded internally by FSM hijack-active predicate so
    // normal flows don't send spurious Shutdowns.
    if (g2FsmHijackActive()) {
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
  float tempC = temperatureRead();
  {
    char line[96];
    // Use " - " as the section separator; the G2 font doesn't carry the
    // · middot and would drop it, so plain ASCII is safer. Heap and
    // PSRAM dropped from this line — they live in the bottom-right
    // meter widget now (buildG2StatusMeter) so the body content can
    // stay byte-stable for many ticks (the cache check in
    // renderStatusCompound skips REBUILD when nothing changed, which
    // is what hides the per-tick repaint flicker).
    snprintf(line, sizeof(line),
             "Up %luh%lum - %.0fC\n",
             hours, minutes, (double)tempC);
    s += line;
  }

  // Networking block — WiFi + MAC + ESPNow kept contiguous so the
  // user reads link state in one glance. Device battery moved below,
  // out of the middle of the networking lines.
#if ENABLE_WIFI
  // Body width ~302 px = ~25 chars at the firmware's near-monospace
  // font. The legacy "WiFi <SSID> <RSSI>dBm - <IP>" line was 30+
  // chars and wrapped. Show the IP only — it's the single piece of
  // info the user actually needs from the lens (everything else is
  // available via web UI / CLI `wifiinfo`). SSID + RSSI live in the
  // CLI for diagnostics; the lens stays uncluttered.
  if (WiFi.isConnected()) {
    char line[64];
    String ip = WiFi.localIP().toString();
    snprintf(line, sizeof(line), "IP: %s\n", ip.c_str());
    s += line;
  } else {
    s += "WiFi off\n";
  }
  // MAC line dropped from on-lens body 2026-04-30: not actionable from
  // the lens (no clipboard / no on-glasses copy), and removing it
  // recovers one line of vertical headroom inside the body widget,
  // which prevents the firmware from auto-rendering a scroll-bar
  // indicator when content exceeds the rendered area. MAC is still
  // available via web UI / CLI `bleinfo`.
#endif

#if ENABLE_ESPNOW
  // Same trim as WiFi above — the legacy "ESPNow <mode> - Np - Xtx/Yrx"
  // line was ~28 chars and wrapped. Show just the peer count, which
  // is the one number that actually fluctuates session-to-session.
  // Mode and tx/rx counts are available via `espnowinfo` on CLI / web
  // UI; the lens stays under the per-line width budget.
  if (gEspNow && gEspNow->initialized) {
    char line[48];
    snprintf(line, sizeof(line), "ESPNow: %dp\n",
             gEspNow->peerHistoryCount);
    s += line;
  } else {
    s += "ESPNow off\n";
  }
#endif

  // (Device battery line moved to the top-right corner widget — see
  //  buildEspStatusBattery + renderStatusCompound below. The corner
  //  shows ESP USB / ESP NN% / ESP --% via getBatteryPercentage() +
  //  isBatteryCharging(); body no longer carries it. If you need the
  //  raw voltage in the body, plumb it as a separate line — the
  //  legacy "Batt %.2fV %u%%" combined the two and folding USB
  //  reporting in cleanly required splitting the corner from the
  //  voltage display.)

#if ENABLE_G2_GLASSES
  // (G2 battery line moved to the top-right corner widget — see
  //  buildG2StatusBattery + renderStatusCompound below. Only the ring
  //  block stays in the body.)

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
  //
  // Status-page body is exactly 4 lines (any 5th line overflows the
  // bottom-aligned text box):
  //   1. device name
  //   2. Up XhYm - NC
  //   3. IP: ... (or "WiFi off")
  //   4. ESPNow: Np (or "ESPNow off")
  // Ring status reports through the corner R1 row, not the body. The
  // (rxCount, tap-seen-Ns) ring telemetry is available via the
  // ringstatus CLI command for diagnostics.
#endif

  // Trim trailing newline. Each line above ends with '\n' for clarity
  // when reading the builder, but a string ending in '\n' makes the
  // firmware's TextObject reserve a phantom empty line slot at the
  // bottom — the text fills slots 1..N-1 from the top, slot N is the
  // trailing-newline phantom, and the bottom of the box is empty
  // space. With the box sized to N slots and pinned to the panel's
  // bottom, the visible text ends one line above the box bottom,
  // making the body look misaligned with the meter on the right.
  // Stripping the trailing '\n' eliminates the phantom; text now
  // fills all N slots and the visible bottom matches the box bottom.
  // Verified 2026-05-09 against firmware 2.2.0.24.
  if (s.length() > 0 && s[s.length() - 1] == '\n') {
    s.remove(s.length() - 1);
  }

  strncpy(out, s.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

// Top-right corner battery widget content. Renders the G2 lens battery
// percentage as a short ASCII string sized to fit the corner geom (~110
// px wide, single line). Pulls from gBatteryR (right-arm sid=0x09 cache)
// — same source the body line used to read before it was extracted into
// this corner widget. Used by renderStatusCompound below as the "batt"
// child of the compound TextObject layout.
static void buildG2StatusBattery(char* out, size_t cap) {
  if (!out || cap == 0) return;
#if ENABLE_G2_GLASSES
  if (gBatteryR < 0) snprintf(out, cap, "G2: ?%%");
  else               snprintf(out, cap, "G2: %d%%", (int)gBatteryR);
#else
  snprintf(out, cap, "%s", "");
#endif
}

// Top-right corner ESP-battery widget. Same "NAME: value" shape as the G2
// ("G2: NN%") and R1 ("R1: --%") corner rows — the "ESP:" label stays; only
// the VALUE changes. The ESP row's geom (kStatusEspGeom) is widened so the
// value field fits up to 4 chars, so it is exactly one of:
//   "USB"  — no battery cell present (running on USB only).
//   "--%"  — cell present but the read came back implausible (transient).
//   "NN%"  — battery percentage 0..99. THIS is the focus; shown whenever a
//            cell is present, charging or not.
//   "100%" — battery full (now fits — the row was widened for this).
// The old combined "USB+NN%"/"USB NN%" forms overflowed the corner and got
// truncated by the lens, which is why the value looked blank/garbled.
static void buildEspStatusBattery(char* out, size_t cap) {
  if (!out || cap == 0) return;
  if (gBatteryState.status == BATTERY_NOT_PRESENT) {
    snprintf(out, cap, "ESP: USB");
    return;
  }
  if (getBatteryVoltage() <= 0.0f) {
    snprintf(out, cap, "ESP: --%%");
    return;
  }
  int pct = (int)(getBatteryPercentage() + 0.5f);
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  snprintf(out, cap, "ESP: %d%%", pct);   // value field fits 4 chars ("100%")
}

// 8-cell circle bar gauge using Unicode geometric/spinner glyphs that
// the firmware font renders cleanly (verified by the
// Tests/Character/Unicode test bench, 2026-04-30):
//   ● U+25CF  (\xe2\x97\x8f)  filled circle  — full cell
//   ◐ U+25D0  (\xe2\x97\x90)  left-half      — half cell
//   ○ U+25CB  (\xe2\x97\x8b)  empty circle   — empty cell
// `pct` is fraction USED (0.0 = empty, 1.0 = full). Each cell
// represents 12.5%; we resolve to half-cell granularity → 17 distinct
// fill levels. Glyph widths are equal in the firmware's near-monospace
// font, so the bar's pixel width stays constant as the value changes —
// which is what the user asked for ("rows stay the same size with just
// the circles changing"). UTF-8 output: 8 × 3 = 24 bytes + NUL.
// Buffer must be at least 25 bytes.
static void renderCircleBar(char* out, size_t cap, float pct) {
  if (!out || cap < 25) {
    if (out && cap > 0) out[0] = '\0';
    return;
  }
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 1.0f) pct = 1.0f;

  // Resolve to half-cell granularity: 16 half-steps across 8 cells.
  const int halfSteps = (int)(pct * 16.0f + 0.5f);  // 0..16
  const int fullCells = halfSteps / 2;              // 0..8
  const bool hasHalf  = (halfSteps & 1) != 0;       // odd → trailing half

  static const char kFull[]  = "\xe2\x97\x8f";  // ●
  static const char kHalf[]  = "\xe2\x97\x90";  // ◐
  static const char kEmpty[] = "\xe2\x97\x8b";  // ○

  size_t pos = 0;
  for (int i = 0; i < 8; i++) {
    const char* glyph;
    if (i < fullCells)                              glyph = kFull;
    else if (i == fullCells && hasHalf && i < 8)    glyph = kHalf;
    else                                            glyph = kEmpty;
    // Each glyph is 3 bytes — copy without NUL since we'll terminate
    // at the end.
    out[pos++] = glyph[0];
    out[pos++] = glyph[1];
    out[pos++] = glyph[2];
  }
  out[pos] = '\0';
}

// Bottom-right corner meter widget content. Two circle-glyph gauges for
// heap and PSRAM pressure, each followed by the FREE amount in their
// natural unit (KB for heap, MB for PSRAM). Bar = USED / TOTAL, so a
// fuller bar means less headroom. Sized for the corner geom (~250 px
// wide, two lines). Replaces the body's `Heap %uK` and `PSRAM %uK / %uK`
// lines, which dropped out when this widget landed. Buffers are 32 B
// because each circle glyph is 3 UTF-8 bytes (8 cells = 24 + NUL).
static void buildG2StatusMeter(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';

  const uint32_t heapFree  = (uint32_t)ESP.getFreeHeap();
  const uint32_t heapTotal = (uint32_t)ESP.getHeapSize();
  const uint32_t heapUsed  = heapTotal > heapFree ? (heapTotal - heapFree) : 0;
  const unsigned heapKb    = (unsigned)(heapFree / 1024);
  char barH[32];
  renderCircleBar(barH, sizeof(barH),
                  heapTotal > 0 ? (float)heapUsed / (float)heapTotal : 0.0f);

  if (psramFound()) {
    const uint32_t psFree  = (uint32_t)ESP.getFreePsram();
    const uint32_t psTotal = (uint32_t)ESP.getPsramSize();
    const uint32_t psUsed  = psTotal > psFree ? (psTotal - psFree) : 0;
    const unsigned psMb    = (unsigned)(psFree / (1024UL * 1024UL));
    char barP[32];
    renderCircleBar(barP, sizeof(barP),
                    psTotal > 0 ? (float)psUsed / (float)psTotal : 0.0f);
    // %4u / %3u right-pads the values so the bar columns line up
    // even when free heap drops below 1000 K (3 digits).
    snprintf(out, cap, "H[%s] %4uK\nP[%s] %3uMB",
             barH, heapKb, barP, psMb);
  } else {
    snprintf(out, cap, "H[%s] %4uK", barH, heapKb);
  }
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
// modules (G2_Page_Sensors, G2_Page_Network, etc.) and just supply their
// build/show functions here.

// All top-level pages return to the hijack root when their back row is
// tapped, so they share the "<- Main Menu" back label. Pages with their
// own showMenu (Network, Files, Settings, Tests) render the back row
// themselves and the backLabel here is unused — left set anyway for
// consistency in case a future code path uses g2ShowTextAsList for them
// (e.g. a CLI text-only view).

// Forward decl — Status' custom render hook is defined alongside the
// live-text worker further down. Page configs need its address here.
static bool renderStatusCompound();

static const G2PageModule kStatusPage = {
  "status", "Status",
  "Show G2 status snapshot on the lens",
  buildG2StatusSnapshot,          // already file-local in this same .cpp
  /*showMenu=*/  nullptr,         // read-only — uses live-page renderer
  /*handleTap=*/ nullptr,
  G2_HIJACK_PAGE_TEXT_VIEW,
  // 5 s auto-refresh. TEXT widgets don't surface single-tap SysEvents
  // (only DOUBLE_CLICK, which is reserved for exit), so manual-refresh
  // via tap isn't reachable from the lens — periodic ticking is the
  // only update path. REBUILD-text snaps content in place so the tick
  // doesn't lose scroll position the way REBUILD-list would.
  /*liveIntervalMs=*/ 5000,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ true,    // long content; text REBUILD avoids cycling
  /*liveRender=*/    renderStatusCompound,  // body + top-right batt corner
};

static const G2PageModule kSensorsPage = {
  "sensors", "Sensors",
  "Show device sensor list on the lens",
  g2BuildSensorList,
  g2ShowSensorsMenu,
  g2SensorsHandleTap,
  G2_HIJACK_PAGE_SENSORS,
  // Interactive list — no live tick at the page level. The detail
  // sub-page snapshots one value when entered; tap-back-tap-in to
  // refresh. Avoids competing with the live workers we already use
  // elsewhere and keeps the page resident on the lens until dismissed.
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/    nullptr,
};

static const G2PageModule kNetworkPage = {
  "network", "Network",
  "Show Network info page on the lens",
  g2BuildNetworkInfo,
  g2ShowNetworkMenu,
  g2NetworkHandleTap,
  G2_HIJACK_PAGE_NETWORK,
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/    nullptr,
};

static const G2PageModule kFilesPage = {
  "files", nullptr,   // hidden from the main menu — reached via the Apps submenu
  "Show Files browser on the lens",
  g2BuildFilesInfo,
  g2ShowFilesMenu,
  g2FilesHandleTap,
  G2_HIJACK_PAGE_FILES,
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/    nullptr,
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
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/    nullptr,
};

static const G2PageModule kPowerPage = {
  "power", "Power",
  "Show Power menu (restart / power off) on the lens",
  g2BuildPowerInfo,
  g2ShowPowerMenu,
  g2PowerHandleTap,
  G2_HIJACK_PAGE_POWER,
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/    nullptr,
};

static const G2PageModule kEspNowAppPage = {
  "espnowapp", nullptr,   // hidden from the main menu — reached via the Apps submenu
  "Show ESP-NOW App page (send/broadcast/ping/peers) on the lens",
  g2BuildESPNowAppInfo,
  g2ShowESPNowAppMenu,
  g2ESPNowAppHandleTap,
  G2_HIJACK_PAGE_ESPNOW_APP,
  // No live tick — pages with their own showMenu don't get the live
  // worker (see invokePageFromMain). The renderers re-read gEspNow / WiFi
  // mode on every show; cmd_exec completion callbacks enqueue a Redraw
  // job that re-runs the appropriate show*Menu(). For ping, the user
  // double-taps (or re-taps a row) to pick up the result — Phase 2 will
  // wire a push-kick path if we want sub-second RTT updates.
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/    nullptr,
};

// ─────────────────────────────────────────────────────────────────────
// Apps launcher — a top-level submenu that groups the "app" pages
// (ESP-NOW App, Files) plus the Maps viewer under one entry, so the main
// hijack menu stays short. Each row forwards to another page's
// show*Menu() (which flips gHijackPage itself) or launches the map
// viewer. Stateless — no sub-mode tracking needed. Files and ESP-NOW App
// stay registered (with hijackLabel=nullptr) so tap routing to their
// handleTap still works; they're just no longer top-level rows.
// ─────────────────────────────────────────────────────────────────────
#if ENABLE_MAPS
static bool g2ShowMapPage(void (*onDone)());   // interactive list+image Maps page, defined below
#endif

static void g2BuildAppsInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
#if ENABLE_MAPS
  snprintf(out, cap, "Apps\nESP-NOW App\nFiles\nMaps");
#else
  snprintf(out, cap, "Apps\nESP-NOW App\nFiles");
#endif
}

static void g2ShowAppsMenu() {
  const char* items[4];
  size_t n = 0;
  items[n++] = "<- Main Menu";
  items[n++] = "ESP-NOW App";
  items[n++] = "Files";
#if ENABLE_MAPS
  // Reflect map availability in the label so tapping "Maps" with no tiles
  // on the device isn't a silent no-op (getAvailableMaps scans /maps for
  // <name>/<name>.hwmap subdirs — the same check the worker does).
  char probe[1][96];
  const bool haveMap = MapCore::hasValidMap() || (MapCore::getAvailableMaps(probe, 1) > 0);
  items[n++] = haveMap ? "Maps" : "Maps (none)";
#endif
  if (g2ShowListPage(items, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_APPS);
    DEBUG_G2F("[G2] Apps menu shown (%u items)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Apps menu show FAILED");
  }
}

static void g2AppsHandleTap(uint32_t idx) {
  switch (idx) {
    case 0: {  // <- back to the root hijack menu
      g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
      extern void g2RedrawHijackMainMenu();
      g2RedrawHijackMainMenu();
      return;
    }
    case 1: g2ShowESPNowAppMenu(); return;  // callee sets gHijackPage = ESPNOW_APP
    case 2: g2ShowFilesMenu();     return;  // callee sets gHijackPage = FILES
#if ENABLE_MAPS
    case 3: g2ShowMapPage(&g2ShowAppsMenu); return;  // interactive map page; returns to Apps on Back
#endif
    default: return;
  }
}

static const G2PageModule kAppsPage = {
  "apps", "Apps",
  "Show the Apps launcher (ESP-NOW App, Files, Maps) on the lens",
  g2BuildAppsInfo,
  g2ShowAppsMenu,
  g2AppsHandleTap,
  G2_HIJACK_PAGE_APPS,
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/    nullptr,
};

#if ENABLE_CAMERA_SENSOR
// Hidden sub-page reached by drilling from Sensors → CAM → "Settings >".
// hijackLabel=nullptr keeps it out of the main hijack menu — the only
// way in is via g2ShowCameraSettingsMenu(). Tap dispatch still routes
// here because the registry's handleTap lookup keys on hijackPage,
// not on whether the page is menu-visible.
static const G2PageModule kCameraSettingsPage = {
  "camerasettings", nullptr,
  "Camera settings sub-page (hidden — reached via Sensors → CAM)",
  g2BuildCameraSettingsInfo,
  g2ShowCameraSettingsMenu,
  g2CameraSettingsHandleTap,
  G2_HIJACK_PAGE_CAMERA_SETTINGS,
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Camera",
  /*prefersTextWidget=*/ false,
  /*liveRender=*/    nullptr,
};
#endif

// ─────────────────────────────────────────────────────────────────────
// MIC detail compound page — list+text compound where the list is
// CREATEd once (back row, merged toggle, HW row) and the readout text
// child is updated live via Cmd=5 UPDATE_TEXT each tick. Camera-stream
// pattern applied to text: "static list child + per-tick data push to
// the active child". List-row selection stays put across ticks because
// UPDATE_TEXT doesn't touch the list child at all.
// ─────────────────────────────────────────────────────────────────────

// Forward decls — helpers used by renderMicDetailLive are defined
// further down (after the live-text worker). Declared here so the
// page module + render fn can reference them inline; same pattern
// renderStatusCompound uses.
extern "C++" bool sendCreateMixedListMultiTextAndWait(
    G2Temple& arm,
    const char* const* listItems,
    size_t listItemCount,
    const G2ContainerGeom& listGeom,
    const G2TextChildSpec* textChildren,
    size_t textChildCount,
    uint32_t widgetId);
static bool sendUpdateTextNamed(G2Temple& arm,
                                const char* containerName,
                                uint32_t containerId,
                                const char* content);
// Live-text worker refresh kick — defined alongside liveTextWorker
// way down. Forward-declared so g2MicDetailHandleTap can wake the
// worker immediately after a toggle, instead of waiting up to 1 s
// for the next scheduled tick.
static void liveTextKickRefresh();

// Layout: list left+top, readout text bottom-right corner. Disjoint x
// ranges so the two never overlap.
static constexpr G2ContainerGeom kMicDetailListGeom    = {   8,   8, 350, 270 };
static constexpr G2ContainerGeom kMicDetailReadoutGeom = { 370, 180, 200, 100 };
static constexpr uint32_t        kMicDetailReadoutCid  = 2;
static const char* const         kMicDetailReadoutName = "micLive";

// Last-rendered readout cache. Skip the UPDATE_TEXT wire send when the
// content is byte-identical to what we last shipped — UPDATE_TEXT is
// cheap but the firmware-side render is not free (~ms of plugin-task
// time), and quiet ticks where nothing changed produce no UX value
// from the repaint.
static char gMicDetailLastReadout[80] = {0};

// Build the live readout string. Three lines so a sustained look at
// the page tells the user the audio level, the I2S configuration, and
// the recording state at a glance. ~25 chars/line budget at the
// readout's 200 px width × firmware near-monospace font.
static void buildMicReadoutText(char* out, size_t cap) {
  if (!out || cap == 0) return;
#if ENABLE_MICROPHONE_SENSOR
  if (!gMicEnabled) {
    // I2S engine off: no level to read. Print the static config so the
    // page doesn't go visually empty when the user toggles off.
    snprintf(out, cap,
             "Lvl: --%%\n%dHz %db %dch\nidle",
             micSampleRate, micBitDepth, micChannels);
    return;
  }
  const int level = getAudioLevel();
  snprintf(out, cap,
           "Lvl: %d%%\n%dHz %db %dch\n%s",
           level, micSampleRate, micBitDepth, micChannels,
           micRecording ? "rec" : "idle");
#else
  snprintf(out, cap, "MIC unavailable");
#endif
}

// Live render fn driven by liveTextWorker. First call (containerReady=
// false): CREATE the compound (1 list + 1 text). Subsequent ticks:
// UPDATE_TEXT on the readout child only — list child untouched, so
// selection stays put. Toggle/back actions force a fresh CREATE by
// calling g2LensClearContainer() in the tap handler, which flips
// containerReady back to false.
static bool renderMicDetailLive() {
  G2Temple* arm = (gR.connected && !gR.pluginDead) ? &gR
                : (gL.connected && !gL.pluginDead) ? &gL
                                                   : nullptr;
  if (!arm) {
    DEBUG_G2F("[G2] mic-detail: no eligible temple");
    return false;
  }

  // Build readout text fresh every tick (cheap — getAudioLevel() reads a
  // small sample window, not a full DMA buffer).
  char readout[80];
  buildMicReadoutText(readout, sizeof(readout));

  if (!g2LensGetState().containerReady) {
    // First call (or post-toggle re-init): CREATE compound.
    // List rows mirror what the legacy generic Sensors-detail showed
    // for MIC, with the merged "MIC: <hw> | <state>" row replacing the
    // old 2-row pair. Drives the same toggle as the legacy row when
    // tapped (idx 1).
    // MIC: On/Off only — PDM has no probeable "connected" signal
    // (no chip ID, no I2C address). See G2_Page_Sensors.cpp's
    // isMic branch for the same rationale.
    char toggleRow[40];
    const bool en = gMicEnabled;
    snprintf(toggleRow, sizeof(toggleRow),
             "MIC: %s",
             en ? "On" : "Off");
    const char* listItems[3] = {
      "<- Sensors",
      toggleRow,
      "HW: PDM",
    };
    G2TextChildSpec textChild = {};
    textChild.containerName = kMicDetailReadoutName;
    textChild.containerId   = kMicDetailReadoutCid;
    textChild.content       = readout;
    textChild.geom          = kMicDetailReadoutGeom;
    textChild.eventCapture  = false;
    bool ok = sendCreateMixedListMultiTextAndWait(
        *arm,
        listItems, 3, kMicDetailListGeom,
        &textChild, 1,
        BLOCKS_WIDGET_ID);
    if (!ok) {
      DEBUG_G2F("[G2] mic-detail: CREATE-list+text failed");
      return false;
    }
    // Mirror container state into the FSM. isList=false because this is
    // a mixed-mode compound — same convention renderStatusCompound uses.
    g2NoteCreateSuccess(*arm, /*isList*/ false, BLOCKS_WIDGET_ID);
    strncpy(gMicDetailLastReadout, readout, sizeof(gMicDetailLastReadout) - 1);
    gMicDetailLastReadout[sizeof(gMicDetailLastReadout) - 1] = '\0';
    DEBUG_G2F("[G2] mic-detail: initial CREATE acked (1 list + 1 text)");
    return true;
  }

  // Subsequent ticks: UPDATE_TEXT on the readout child, gated by
  // byte-identical cache. List child untouched → selection persists.
  if (strcmp(readout, gMicDetailLastReadout) == 0) {
    return true;  // quiet tick, no wire I/O
  }
  bool ok = sendUpdateTextNamed(*arm,
                                kMicDetailReadoutName, kMicDetailReadoutCid,
                                readout);
  if (ok) {
    strncpy(gMicDetailLastReadout, readout, sizeof(gMicDetailLastReadout) - 1);
    gMicDetailLastReadout[sizeof(gMicDetailLastReadout) - 1] = '\0';
  }
  return ok;
}

// Stub buildText for the registry's required-field check. Hidden
// pages that drive the lens via liveRender don't have a CLI text
// view, but g2RegisterPage rejects entries with null buildText —
// without this stub the page never makes it into the registry,
// and handleHijackMenuTap's lookup falls through to the read-only
// TEXT_VIEW fallback (idx 0 → MAIN, rest no-op), which is exactly
// the symptom that masked itself as "tap not firing" on
// 2026-05-08. If someone runs `cmd_g2micdetail` from the CLI it
// just emits the static info line; the live data path is on the
// lens via renderMicDetailLive.
static void g2BuildMicDetailInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap,
           "MIC detail (live readout on lens; CLI view not implemented)");
}

static const G2PageModule kMicDetailPage = {
  // Hidden — hijackLabel=nullptr keeps it out of the main menu, the
  // only entry is g2ShowMicDetail() called from Sensors-detail.
  "micdetail", nullptr,
  "MIC detail page (hidden — reached via Sensors → MIC)",
  /*buildText=*/  g2BuildMicDetailInfo,   // required by g2RegisterPage; stub
  /*showMenu=*/   nullptr,
  /*handleTap=*/  g2MicDetailHandleTap,
  G2_HIJACK_PAGE_MIC_DETAIL,
  // 1 Hz tick — audio level fluctuates fast enough to feel live at
  // 1 Hz; the rate doesn't strain the BLE link (single-envelope
  // UPDATE_TEXT, no fragmenting). Bump down (200–500 ms) if the user
  // wants more responsive level updates.
  /*liveIntervalMs=*/ 1000,
  /*backLabel=*/      "<- Sensors",
  /*prefersTextWidget=*/ true,    // routes through g2StartLiveTextPage
  /*liveRender=*/     renderMicDetailLive,
};

bool g2ShowMicDetail() {
  // Mirror handleHijackMenuTap's live-text dispatch (the menu does this
  // when the user taps "Status" etc.). For MIC detail the entry isn't
  // the main menu — it's Sensors-detail tap on MIC — so we replicate
  // the dispatch inline. liveTextWorker handles SHUTDOWN of the
  // outgoing list page, then calls renderMicDetailLive (which CREATEs
  // the compound on first call).
  if (!g2StartLiveTextPage(/*buildText=*/ nullptr,
                           kMicDetailPage.liveIntervalMs,
                           kMicDetailPage.liveRender)) {
    DEBUG_G2F("[G2] mic-detail: live-text spawn failed");
    return false;
  }
  g2SetHijackPage(G2_HIJACK_PAGE_MIC_DETAIL);
  // Reset the readout cache so the first tick after entry isn't
  // suppressed by a stale value left over from the previous session.
  gMicDetailLastReadout[0] = '\0';
  BROADCAST_PRINTF("[G2] Hijack: MIC detail opened (live readout @1 Hz)");
  return true;
}

void g2MicDetailHandleTap(uint32_t idx) {
  if (idx == 0) {
    // Back to Sensors landing list. The live-text worker will see
    // gLiveTextStopFlag get tripped by the outgoing tear-down and
    // exit cleanly; g2ShowSensorsMenu re-CREATEs the sensor list.
    extern void g2ShowSensorsMenu();
    g2ShowSensorsMenu();
    return;
  }
  if (idx == 1) {
#if ENABLE_MICROPHONE_SENSOR
    // Toggle row — same start/stop helpers Sensors-detail uses, plus
    // the same persisted-flag flip via the existing CLI command path.
    const bool prev = gMicEnabled;
    const bool next = !prev;
    BROADCAST_PRINTF("[G2] MIC detail: toggle %s -> %s",
                     prev ? "ON" : "OFF",
                     next ? "ON" : "OFF");
    if (next) initMicrophone(); else stopMicrophone();
    // Persist the auto-start preference via the same hijack-command
    // submit path that Sensors-detail uses (G2_Page_Sensors.cpp:912).
    // Cookie just carries our active page so any completion logging
    // attributes correctly; we don't need a callback because the
    // forced re-CREATE below will redraw the row regardless.
    char line[32];
    snprintf(line, sizeof(line), "micautostart %s", next ? "on" : "off");
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = 0;
    (void)g2SubmitHijackCommand(line, cookie, nullptr, nullptr);
    // Force a fresh CREATE on next tick. The list row content needs to
    // change to reflect the new toggle state, and the only way to update
    // a list row is REBUILD or CREATE. REBUILD-list on a compound blanks
    // the sibling text child (verified 2026-04-24 against firmware
    // 2.2.0.24), so re-CREATE is the only safe option.
    //
    // Critical: re-CREATE requires a Shutdown on the wire FIRST, not
    // just a local-state clear. g2LensClearContainer() only flips our
    // FSM mirror — the firmware still thinks the old compound is live,
    // and the next CREATE for the same widgetId times out because the
    // firmware ignores it ("container not primed", verified
    // 2026-05-08). tearDownActiveContainer sends an actual Shutdown
    // envelope, waits 500 ms for the firmware to settle, then clears
    // both the per-temple and lens-level state — same path the worker
    // runs in its own startup prologue.
    G2Temple* arm = (gR.connected && !gR.pluginDead) ? &gR
                  : (gL.connected && !gL.pluginDead) ? &gL
                                                     : nullptr;
    if (arm) {
      tearDownActiveContainer(*arm);
    } else {
      // Fallback — at least drop the local mirror so the worker
      // tries CREATE on its next tick. Likely to fail without a real
      // wire Shutdown but better than getting stuck on the old view.
      g2LensClearContainer();
    }
    // Kick the live-text worker so the re-CREATE fires immediately
    // instead of after the remainder of the 1 s tick interval.
    // Without this, the toggle row appears unchanged for up to a
    // second after the user tapped, which feels broken.
    // liveTextKickRefresh gives the worker's wait semaphore so it
    // bails out of vTaskDelay early.
    liveTextKickRefresh();
#endif
    return;
  }
  // idx 2 (HW row) is info-only — no action.
  DEBUG_G2F("[G2] MIC detail: tap idx=%u (info row, no action)", (unsigned)idx);
}

// ---------------------------------------------------------------------------
// Generic sensor-detail LIVE compound (all non-camera sensors)
// ---------------------------------------------------------------------------
// Same shape as renderMicDetailLive: a selectable list (back / Auto Start)
// plus a live readout child that UPDATE_TEXTs each tick WITHOUT disturbing the
// list selection. Unlike MIC it registers NO page module/enum — it reuses the
// Sensors page's hijack page (G2_HIJACK_PAGE_SENSORS) + tap handler at
// gSensorsLevel == DETAIL (idx 0 back, idx 1 toggle), so taps route to the
// existing g2SensorsHandleTap. Content comes from G2_Page_Sensors.cpp (it owns
// the sensor caches): g2BuildSensorLiveList() for rows, g2BuildSensorReadout()
// for the readout. Driven by the shared g2_live_text worker (no new task).
// List narrowed (its rows are short — "Auto Start: OFF" is the widest) so the
// readout pane can be wide enough for the squared gamepad grid + button diamond.
static constexpr G2ContainerGeom kSensorLiveListGeom    = {   8,   8, 200, 270 };
static constexpr G2ContainerGeom kSensorLiveReadoutGeom = { 216,   8, 352, 270 };
static constexpr uint32_t        kSensorLiveReadoutCid  = 2;
static const char* const         kSensorLiveReadoutName = "snsLive";
static char gSensorLiveLastReadout[224] = {0};

static bool renderSensorDetailLive() {
  G2Temple* arm = (gR.connected && !gR.pluginDead) ? &gR
                : (gL.connected && !gL.pluginDead) ? &gL
                                                   : nullptr;
  if (!arm) { DEBUG_G2F("[G2] sensor-live: no eligible temple"); return false; }

  char readout[224];
  g2BuildSensorReadout(readout, sizeof(readout));

  if (!g2LensGetState().containerReady) {
    const char* listItems[4] = { nullptr, nullptr, nullptr, nullptr };
    size_t n = g2BuildSensorLiveList(listItems, 4);
    if (n == 0) { DEBUG_G2F("[G2] sensor-live: empty list"); return false; }
    G2TextChildSpec textChild = {};
    textChild.containerName = kSensorLiveReadoutName;
    textChild.containerId   = kSensorLiveReadoutCid;
    textChild.content       = readout;
    textChild.geom          = kSensorLiveReadoutGeom;
    textChild.eventCapture  = false;
    bool ok = sendCreateMixedListMultiTextAndWait(
        *arm, listItems, n, kSensorLiveListGeom, &textChild, 1, BLOCKS_WIDGET_ID);
    if (!ok) { DEBUG_G2F("[G2] sensor-live: CREATE-list+text failed"); return false; }
    g2NoteCreateSuccess(*arm, /*isList*/ false, BLOCKS_WIDGET_ID);
    strncpy(gSensorLiveLastReadout, readout, sizeof(gSensorLiveLastReadout) - 1);
    gSensorLiveLastReadout[sizeof(gSensorLiveLastReadout) - 1] = '\0';
    DEBUG_G2F("[G2] sensor-live: initial CREATE acked (1 list + 1 text)");
    return true;
  }

  // Subsequent ticks: UPDATE_TEXT the readout child only, gated by a
  // byte-identical cache. List child untouched → selection persists.
  if (strcmp(readout, gSensorLiveLastReadout) == 0) return true;  // quiet tick
  bool ok = sendUpdateTextNamed(*arm, kSensorLiveReadoutName,
                                kSensorLiveReadoutCid, readout);
  if (ok) {
    strncpy(gSensorLiveLastReadout, readout, sizeof(gSensorLiveLastReadout) - 1);
    gSensorLiveLastReadout[sizeof(gSensorLiveLastReadout) - 1] = '\0';
  }
  return ok;
}

bool g2ShowSensorLive() {
  // Caller (showSensorDetail) already set gSensorsLevel = DETAIL + hijack page;
  // reset the readout cache so the first tick isn't suppressed by a stale
  // value, then spawn the shared live-text worker in renderFn mode.
  gSensorLiveLastReadout[0] = '\0';
  if (!g2StartLiveTextPage(/*buildText=*/ nullptr, /*intervalMs=*/ 1000,
                           renderSensorDetailLive)) {
    DEBUG_G2F("[G2] sensor-live: live-text spawn failed");
    return false;
  }
  g2SetHijackPage(G2_HIJACK_PAGE_SENSORS);
  return true;
}

static const G2PageModule kTestSuitePage = {
  "tests", "Tests",
  "Show on-glasses transport test bench",
  g2BuildTestSuiteInfo,
  g2ShowTestSuiteMenu,
  g2TestSuiteHandleTap,
  G2_HIJACK_PAGE_TESTS,
  /*liveIntervalMs=*/ 0,
  /*backLabel=*/    "<- Main Menu",
  /*prefersTextWidget=*/ false,   // own showMenu — flag is irrelevant
  /*liveRender=*/    nullptr,
};

static void registerG2Pages(void) {
  // Order = menu order in the hijack list.
  // Status now covers the merged Status+System view — the unique System
  // bits (PSRAM, WiFi RSSI, device battery V/%) were folded into
  // buildG2StatusSnapshot. The standalone System page module is gone.
  g2RegisterPage(kStatusPage);
  g2RegisterPage(kSensorsPage);
  g2RegisterPage(kNetworkPage);
  g2RegisterPage(kAppsPage);       // Apps launcher (ESP-NOW App, Files, Maps)
  g2RegisterPage(kSettingsPage);
  g2RegisterPage(kPowerPage);
  g2RegisterPage(kTestSuitePage);
  // Files + ESP-NOW App are registered but hijackLabel=nullptr, so they no
  // longer appear as top-level rows — they live under Apps. Kept registered
  // so the tap dispatcher can still route to g2FilesHandleTap /
  // g2ESPNowAppHandleTap when those pages are active (lookup keys on
  // hijackPage, not on menu visibility).
  g2RegisterPage(kFilesPage);
  g2RegisterPage(kEspNowAppPage);
#if ENABLE_CAMERA_SENSOR
  g2RegisterPage(kCameraSettingsPage);   // hidden — see kCameraSettingsPage above
  g2RegisterPage(kMicDetailPage);        // hidden — see kMicDetailPage above
#endif
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
// Step 5/Group D: was previously `hijackWorkerTask` spawned via xTaskCreate
// per hijack session. Now invoked as a CustomSpec on the lens applier so
// we don't burn a 4 KB transient stack at the moment a user opens Blocks
// (when internal DRAM is already tight from dual-temple BLE state). The
// body is unchanged — same Shutdown+CREATE handshake, same FSM updates —
// just runs on the persistent applier task instead of a per-call task.
// Stack profile fits the applier's existing 4 KB (verified: identical to
// pageSwapJobBody's CREATE-list path).
static void hijackBootstrapBody() {
  if (!gR.connected) {
    DEBUG_G2F("[G2] Hijack: right temple not connected — aborting");
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
    return;
  }

  // Step 3: no container exists after cancel — we must CREATE.
  gR.containerReady = false;

  // Step 4: CREATE-list tagged with the Blocks widgetId so firmware
  // associates it with the launch it just announced.
  if (sendCreateListAndWait(gR, kHijackMenuItems, kHijackMenuCount,
                            BLOCKS_WIDGET_ID, G2_GEOM_LARGE)) {
    gHijackStartedMs = millis();
    // HijackEnter must fire before ContainerCreated so the FSM is in
    // Hijacked when the CREATE event arrives (otherwise it logs an
    // illegal ContainerCreated-in-Idle). g2NoteCreateSuccess dispatches
    // ContainerCreated via g2LensSetContainer.
    g2LensSetHijackActive(true);
    g2NoteCreateSuccess(gR, /*isList*/ true, BLOCKS_WIDGET_ID);
    gLastHijackListRowCount  = kHijackMenuCount;
    gLastHijackListPageShape = HijackListPageShape::PureList;
    g2SetHijackPage(G2_HIJACK_PAGE_MAIN);  // fresh hijack always starts at MAIN
    BROADCAST_PRINTF("[G2] Blocks hijack: %u-item menu shown",
                     (unsigned)kHijackMenuCount);
    g2PushStatusEvent("hijack-on");
    systemEventPost(SYSEVT_G2_HIJACK_ENTERED, "MAIN");
  } else {
    DEBUG_G2F("[G2] Hijack: CREATE-list failed — menu NOT displayed");
    g2PushStatusEvent("hijack-fail");
  }
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
//
// Installs the paired-user identity into this task's TLS slot for the
// duration of every page render. Any guarded VFS access from inside a page
// handler (Files, future pages) inherits it automatically — pages don't need
// per-handler boilerplate. See G2_HijackCmd.h for the rationale.
static void invokePageFromMain(const G2PageModule& p) {
  G2HijackCtxGuard ctxGuard;

  if (p.showMenu) {
    p.showMenu();
    BROADCAST_PRINTF("[G2] Hijack: %s tapped — opened sub-page",
                     p.hijackLabel ? p.hijackLabel : p.name);
    return;
  }
  // Live page: start the worker. Still renders an initial list/text
  // synchronously; subsequent refreshes REBUILD in place via Cmd=7.
  //
  // liveIntervalMs > 0 → auto-refresh that often + manual via double-tap
  // liveIntervalMs == 0 → manual refresh only (double-tap kicks the sem)
  // Both modes still spawn the worker so the double-tap kick wiring
  // and clean-exit affordances are uniform.
  if (p.buildText) {
    // Pages opt into the TEXT-widget render path via prefersTextWidget.
    // List-rendered live pages reset selection/scroll on each REBUILD,
    // which produces visible cycling for long content; text widgets
    // snap content in place, no cycling. Trade-off: text mode has no
    // tappable back row — exit is DOUBLE_CLICK only.
    const bool started = p.prefersTextWidget
                         ? g2StartLiveTextPage(p.buildText, p.liveIntervalMs,
                                                p.liveRender)
                         : g2StartLiveListPage(p.buildText, p.liveIntervalMs,
                                                p.backLabel);
    if (started) {
      g2SetHijackPage(p.hijackPage);
      if (p.liveIntervalMs > 0) {
        BROADCAST_PRINTF("[G2] Hijack: %s tapped — live %s page (every %u ms)",
                         p.hijackLabel ? p.hijackLabel : p.name,
                         p.prefersTextWidget ? "text" : "list",
                         (unsigned)p.liveIntervalMs);
      } else {
        BROADCAST_PRINTF("[G2] Hijack: %s tapped — %s page (refresh on "
                         "double-tap)",
                         p.hijackLabel ? p.hijackLabel : p.name,
                         p.prefersTextWidget ? "text" : "list");
      }
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
  if (g2ShowTextAsList(buf, p.backLabel)) {
    BROADCAST_PRINTF("[G2] Hijack: %s tapped — list shown (%u B)",
                     p.hijackLabel ? p.hijackLabel : p.name,
                     (unsigned)strlen(buf));
  } else {
    DEBUG_G2F("[G2] Hijack: %s tap — show failed", p.name);
  }
}

static void handleHijackMenuTap(uint32_t idx) {
  // Every tap dispatched from the lens runs as the user who paired the
  // glasses (gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser). Pages can
  // call FileManager / VFS::*Guarded directly inside their handleTap and
  // get the right identity without any per-page setup. See G2_HijackCmd.h
  // for the design rationale.
  G2HijackCtxGuard ctxGuard;

  // Text-entry overlay: when active, the live-page renders the keyboard
  // and taps belong to it — not the underlying page. Intercept BEFORE
  // the per-page dispatch so any caller (Network, Bluetooth, future
  // pages) gets text entry "for free" without wiring it page-by-page.
  if (g2TextEntryIsActive()) {
    g2TextEntryHandleTap(idx);
    return;
  }

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
  if (g2FsmHijackActive()) {
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
              "proceeding with fresh hijack");
    // FSM stays in Hijacked across the relaunch — the upcoming
    // hijackWorkerTask CREATE re-asserts hijack ownership without ever
    // dropping to Idle. Clear the per-temple containerReady so the
    // worker takes the CREATE path instead of REBUILD.
    gR.containerReady = false;
  }

  // Step 5/Group D: enqueue the Shutdown+CREATE handshake onto the lens
  // applier instead of spawning a per-call 4 KB stack here. Same off-the-
  // BLE-notify-task guarantee (the applier runs on a separate persistent
  // task), with no transient stack allocation. The deadlock that the
  // legacy code warned about (inline-on-BLE-notify) still applies — both
  // failure modes (alloc fail, queue full) drop the hijack rather than
  // stall the notify task.
  CustomSpec* spec = new (std::nothrow) CustomSpec{};
  if (!spec) {
    DEBUG_G2F("[G2] Hijack: CustomSpec alloc failed — dropping (would "
              "deadlock to run inline on notify task)");
    return;
  }
  spec->run = &hijackBootstrapBody;

  LensUiJob* job = new (std::nothrow) LensUiJob{};
  if (!job) {
    DEBUG_G2F("[G2] Hijack: LensUiJob alloc failed — dropping");
    delete spec;
    return;
  }
  job->kind           = LensJobKind::Custom;
  job->submitMenuGen  = g2CurrentMenuGen();   // not enforced for Custom; harmless
  job->cmdSeq         = 0;
  job->targetPage     = g2GetHijackPage();
  job->targetNetSub   = 0;
  job->payload.custom = spec;

  if (!g2EnqueueLensJob(job)) {
    DEBUG_G2F("[G2] Hijack: lens job enqueue FAILED — applier queue full?");
    delete spec;
    delete job;
  }
}

// Hijack teardown. Called when the lens swipes closed (DISPLAY_OFF on
// sid=0x0D) or by the safety fallback in the heartbeat tick. Sends a Cmd=9 ShutdownPage
// to the right temple so the plugin task releases its container slot,
// and clears the cached containerReady so the next tap re-CREATEs.
// Safe to call from any context — only the first caller per hijack does
// real work thanks to the FSM hijack-active guard.
static void sendHijackShutdown(const char* reason) {
  if (!g2FsmHijackActive()) return;
  g2LensSetHijackActive(false);
  g2LensClearContainer();
  g2LensClearOverlay();
  // Clear text-view tracking too. Without this, a silent safety-timeout
  // leaves gTextViewActive=true; the next stray USER_ACTIVITY then hits
  // the line-2451 fallback and re-launches the page via gTextViewExitFn,
  // resurrecting the hijack on a stale safety timer.
  gTextViewActive = false;
  gTextViewExitFn = nullptr;
  gTextViewTapFn  = nullptr;
  DEBUG_G2F("[G2] Hijack exit (%s) — sending ShutdownPage",
            reason ? reason : "?");
  g2PushStatusEvent(reason ? reason : "hijack-off");
  systemEventPost(SYSEVT_G2_HIJACK_EXITED, reason ? reason : "hijack-off");
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

// sid=0x80 (DevCfgDataPackage) RX decoder. The wrapper is field 1=commandId,
// field 2=magicRandom, then a per-cmd nested sub-message at fields 3..128.
// Most useful is RingInfo.connRet (field 4 of the nested RingInfo at
// wrapper field 5) — the device polls us with cmd=6 RING_CONNECT_INFO and
// the connRet status reports its bridge-attempt progress. Without this
// decoder, those polls show up as opaque `hb-rx f5 bytes(2)=[20 NN]`.
//
// Schema reference: docs/g2_proto/dev_config_protocol.proto +
// docs/g2_proto/dev_pair_manager.proto + docs/g2_proto/dev_settings.proto.
//
// Output line shape: `[G2-X] sid=0x80 RX <CMD_NAME> magic=NN <details>`
static const char* devCfgCmdName(uint64_t c) {
  switch (c) {
    case 4:   return "AUTHENTICATION";
    case 5:   return "PIPE_ROLE_CHANGE";
    case 6:   return "RING_CONNECT_INFO";
    case 7:   return "BLE_CONNECT_PARAM";
    case 8:   return "DISCONNECT_INFO";
    case 9:   return "UNPAIR_INFO";
    case 10:  return "COMMAND_EXCEPTION";
    case 13:  return "RESTORE_FACTORY";
    case 14:  return "BASE_HEARTBEAT";
    case 15:  return "QUICK_RESTART";
    case 128: return "TIME_SYNC";
    case 129: return "AUD_CONTROL";
    case 255: return "COMMAND_ERROR";
    default:  return "?";
  }
}

// connRet semantics inferred from observed values during bridge attempts.
// Numbers we've actually seen on firmware 2.2.0.24:
//   8  = persistent across attempts; almost certainly a terminal failure
//        (likely "ring rejected the connection" — ring needs pairAuth pkey)
//   13 = transient, paired with active scan/connect attempts ("trying")
//   19 = transient, less common ("scanning"?)
//   62 = rare, possibly an error class we haven't catalogued yet
// Anything else: unknown — log the number and let it tell us via use.
static const char* connRetHint(uint64_t v) {
  switch (v) {
    case 0:  return "ok";
    case 8:  return "fail-terminal?";
    case 13: return "trying?";
    case 19: return "scanning?";
    case 62: return "err?";
    default: return "?";
  }
}

static void parseSid80Rx(char side, const uint8_t* pb, size_t pbLen) {
  uint64_t cmd = 0;
  uint64_t magic = 0;
  const uint8_t* body = nullptr;
  size_t   bodyLen = 0;
  uint32_t bodyField = 0;

  size_t pos = 0;
  while (pos < pbLen) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(pb, pbLen, &pos, &field, &wire)) {
      DEBUG_G2F("[G2-%c] sid=0x80 RX (parse failed at off=%u)",
                side, (unsigned)pos);
      return;
    }
    if (field == 1 && wire == G2_PB_WIRE_VARINT) {
      if (!g2PbReadVarint(pb, pbLen, &pos, &cmd)) return;
    } else if (field == 2 && wire == G2_PB_WIRE_VARINT) {
      if (!g2PbReadVarint(pb, pbLen, &pos, &magic)) return;
    } else if (wire == G2_PB_WIRE_LEN_DELIM) {
      uint64_t sl;
      if (!g2PbReadVarint(pb, pbLen, &pos, &sl)) return;
      if (pos + sl > pbLen) return;
      // Capture the first len-delim sub-message; the wrapper has only one
      // populated nested per packet (per-cmd routing).
      if (!body) {
        body = pb + pos;
        bodyLen = (size_t)sl;
        bodyField = field;
      }
      pos += (size_t)sl;
    } else {
      if (!g2PbSkipField(pb, pbLen, &pos, wire)) return;
    }
  }

  const char* name = devCfgCmdName(cmd);

  // Cmd-specific body decoding. Only the cases we actually see / care
  // about right now — others fall through to the generic line below.
  if (cmd == 6) {  // RING_CONNECT_INFO — the bridge-status poll
    if (bodyLen == 0) {
      DEBUG_G2F("[G2-%c] sid=0x80 RX %s magic=%llu (poll, empty)",
                side, name, (unsigned long long)magic);
      return;
    }
    uint64_t connectRing = 0; bool hasConnectRing = false;
    uint64_t connRet     = 0; bool hasConnRet     = false;
    uint64_t result      = 0; bool hasResult      = false;
    size_t bp = 0;
    while (bp < bodyLen) {
      uint32_t bf; uint8_t bw;
      if (!g2PbReadTag(body, bodyLen, &bp, &bf, &bw)) break;
      if (bf == 1 && bw == G2_PB_WIRE_VARINT) {
        if (g2PbReadVarint(body, bodyLen, &bp, &connectRing)) hasConnectRing = true;
      } else if (bf == 4 && bw == G2_PB_WIRE_VARINT) {
        if (g2PbReadVarint(body, bodyLen, &bp, &connRet)) hasConnRet = true;
      } else if (bf == 5 && bw == G2_PB_WIRE_VARINT) {
        if (g2PbReadVarint(body, bodyLen, &bp, &result)) hasResult = true;
      } else {
        if (!g2PbSkipField(body, bodyLen, &bp, bw)) break;
      }
    }
    char extra[96] = {0};
    size_t off = 0;
    if (hasConnectRing) off += snprintf(extra + off, sizeof(extra) - off,
                                        " connectRing=%u", (unsigned)connectRing);
    if (hasConnRet)     off += snprintf(extra + off, sizeof(extra) - off,
                                        " connRet=%u(%s)",
                                        (unsigned)connRet, connRetHint(connRet));
    if (hasResult)      off += snprintf(extra + off, sizeof(extra) - off,
                                        " result=%u", (unsigned)result);
    DEBUG_G2F("[G2-%c] sid=0x80 RX %s magic=%llu%s",
              side, name, (unsigned long long)magic, extra);

    // Mirror the connRet / connectRing values into the ring module so
    // `ringbridge status` can display the most recent bridge-attempt state.
    g2RingNoteBridgePoll(connRet, hasConnRet, connectRing, hasConnectRing);
    return;
  }

  if (cmd == 4) {  // AUTHENTICATION
    uint64_t secAuth = 0; bool hasSecAuth = false;
    size_t bp = 0;
    while (bp < bodyLen) {
      uint32_t bf; uint8_t bw;
      if (!g2PbReadTag(body, bodyLen, &bp, &bf, &bw)) break;
      if (bf == 1 && bw == G2_PB_WIRE_VARINT) {
        if (g2PbReadVarint(body, bodyLen, &bp, &secAuth)) hasSecAuth = true;
      } else {
        if (!g2PbSkipField(body, bodyLen, &bp, bw)) break;
      }
    }
    if (hasSecAuth) {
      DEBUG_G2F("[G2-%c] sid=0x80 RX %s magic=%llu secAuth=%u",
                side, name, (unsigned long long)magic, (unsigned)secAuth);
    } else {
      DEBUG_G2F("[G2-%c] sid=0x80 RX %s magic=%llu (ack, body=%uB)",
                side, name, (unsigned long long)magic, (unsigned)bodyLen);
    }
    return;
  }

  if (cmd == 5 || cmd == 14 || cmd == 128) {
    // PIPE_ROLE_CHANGE / BASE_HEARTBEAT / TIME_SYNC — body is just an ack
    // or empty when the device is responding to our send.
    DEBUG_G2F("[G2-%c] sid=0x80 RX %s magic=%llu (ack, body=%uB)",
              side, name, (unsigned long long)magic, (unsigned)bodyLen);
    return;
  }

  // Unhandled cmd — log generically with body field tag visible.
  DEBUG_G2F("[G2-%c] sid=0x80 RX cmd=%llu(%s) magic=%llu body_field=%u body_len=%u",
            side, (unsigned long long)cmd, name,
            (unsigned long long)magic, (unsigned)bodyField, (unsigned)bodyLen);
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
          systemEventPost(SYSEVT_G2_SILENT_MODE, newSilent ? "on" : "off");
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
      // sid=0x80 carries DevCfgDataPackage — both unsolicited bridge polls
      // (cmd=6 RING_CONNECT_INFO with empty body or connRet status updates)
      // and acks of our own typed sends (g2devcfg auth/role/time/ring/hb).
      // The legacy "hb-rx" name is a misnomer kept in the SID symbol for
      // grep-history; the parser below decodes the actual cmd + relevant
      // fields per docs/g2_proto/dev_config_protocol.proto.
      parseSid80Rx(t.side, env.payload, env.payloadLen);
      break;

    case G2_SID_RING_RAW_DATA:
    case G2_SID_RING_DATA_RELAY:
      // The right temple's ring-bridge forwards RingDataPackage frames here
      // once `ringbridge on` has handed the ring off to it. sid=0x90 carries
      // RingRawData (telemetry); sid=0x91 carries RingEvent. We hand both
      // to the ring module — it parses RingRawData into the shared cache so
      // status / spoof / web UI keep working without source-of-data branches.
      DEBUG_G2F("[G2-%c] sid=0x%02X RX ring-bridge forward (%u B)",
                t.side, env.sid, (unsigned)env.payloadLen);
      g2RingNoteForwardedTelemetry(env.payload, env.payloadLen);
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
    // Re-check link state on every fragment. The disconnect callback
    // (line ~1178) nullifies writeChar; without this guard, an iteration
    // that started while connected can dereference a freed BLE
    // characteristic mid-burst when the link drops between fragments.
    if (!t.connected || !t.writeChar) {
      DEBUG_G2F("[G2-%c] sendEnvelope aborted at offset %u/%u — "
                "link dropped (connected=%d, writeChar=%p)",
                t.side, (unsigned)off, (unsigned)len,
                (int)t.connected, (void*)t.writeChar);
      ok = false;
      break;
    }
    bool wrote = t.writeChar->writeValue(const_cast<uint8_t*>(data + off),
                                         take, false);
    if (!wrote) {
      // `esp_ble_gattc_write_char rc=-1` — controller TX queue momentarily
      // refused the write at the GATT API boundary. We used to retry 3×
      // here with stepped backoff (50/100/100 ms) on the theory that the
      // queue would drain and the second attempt would land. That worked
      // for the typical brief-stall case at 96×96 (2 chunks per frame, ~34
      // envelopes), where the rc=-1 was rare and isolated.
      //
      // Wedge observed 2026-05-10 during sustained 288×144 streaming
      // (~96 envelopes per frame, ~3× the rc=-1 surface area): the FIRST
      // writeValue returned false at the rc=-1 fast path inside
      // BLERemoteCharacteristic::writeValue (esp_ble_gattc_write_char
      // rejected synchronously, no semaphore wait). The retry path then
      // called writeValue() AGAIN, but this time the write was accepted
      // by esp_ble_gattc_write_char and the call dropped into the
      // GATT-event semaphore wait (m_semaphoreWriteCharEvt) — which
      // never got given because the underlying GATT was still in a
      // degraded state from the first failure. The retry's writeValue
      // call blocked indefinitely with t.writeMutex held → heartbeats
      // started failing with "Write mutex timeout" → the 25 s TX-wedged
      // watchdog forced a disconnect to recover. Frame 21 onward of the
      // stream was lost AND the entire BLE link had to be re-established.
      //
      // New policy: on rc=-1, abort the burst immediately. Don't retry.
      // The caller (sendImageBmpFragmentsNoCreate or similar) sees ok=false,
      // breaks its frame loop, releases the mutex, and the next frame's
      // first envelope is a fresh start. We lose one frame instead of
      // the whole link. If rc=-1 transients become frequent enough to
      // notice in practice (frequent stream stutters), the right fix is
      // a controller-level cooldown / backpressure signal, not a blind
      // retry that can wedge the worker.
      DEBUG_G2F("[G2-%c] writeValue rc=-1 at offset %u/%u — aborting burst "
                "(no retry; retry path can wedge on GATT semaphore — see comment)",
                t.side, (unsigned)off, (unsigned)len);
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
  if (xSemaphoreTake(t.writeMutex, pdMS_TO_TICKS(G2_SEND_ENVELOPE_MUTEX_MS)) !=
      pdTRUE) {
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
// a multi-fragment 3800 B image push) acquired the mutex between our
// fragments, fired writeValue, and got `esp_ble_gattc_write_char rc=-1`
// because the BLE controller's pending-write queue was already saturated
// by our in-flight chunks. From there every subsequent acquire timed out
// for ~30 s ("Write mutex timeout; envelope dropped" cascade) until the
// firmware gave up on the half-complete reassembly and the controller
// drained, by which time we'd lost the hijack.
//
// Holding the mutex across the burst means heartbeats wait at the
// xSemaphoreTake site (G2_SEND_ENVELOPE_MUTEX_MS — drops the beat if a
// fragment write blocks longer than that; firmware tolerates ≥10 s of
// beat silence before tearing down).
// The 20 ms inter-fragment delay was already empirically tuned for the
// single-writer case (reference's TypeScript event-loop pacing), so
// serialising all writers behind the burst's pacing is correct without
// further tuning.
// One-shot inter-fragment delay override. Defaults to 0 (use the 20 ms
// baseline). Test bench writes via g2DebugSetNextBurstFragDelay() to
// verify whether firmware reassembly is timing-sensitive. Consumed
// (read + cleared) at the start of each sendPbFragmented burst so the
// override applies to exactly one transmit, not all subsequent ones.
static volatile uint32_t gFragNextBurstDelayMs = 0;
void g2DebugSetNextBurstFragDelay(uint32_t delayMs) {
  gFragNextBurstDelayMs = delayMs;
}

static bool sendPbFragmented(G2Temple& arm, uint8_t seq, uint8_t sid, uint8_t flag,
                             const uint8_t* pb, size_t pbLen) {
  if (!pb || pbLen == 0) return false;
  if (!arm.connected || !arm.writeChar || !arm.writeMutex) {
    DEBUG_G2F("[G2-%c] sendPbFragmented: arm not ready", arm.side);
    return false;
  }

  // Snapshot + clear the one-shot delay override before doing any work.
  // 0 == use the baseline; any non-zero value applies to all fragments
  // of this burst.
  const uint32_t override = gFragNextBurstDelayMs;
  gFragNextBurstDelayMs = 0;
  const uint32_t burstDelayMs = override ? override : 15;
  if (override) {
    DEBUG_G2F("[G2-%c] sendPbFragmented: inter-fragment delay overridden "
              "to %u ms (one-shot)", arm.side, (unsigned)burstDelayMs);
  }

  const uint16_t crc = g2CrcCcittFalse(pb, pbLen);
  const size_t   chunkSize    = G2_FRAG_CHUNK_PB;
  const size_t   totalWithCrc = pbLen + G2_ENVELOPE_CRC_LEN;
  uint8_t totFrags = (uint8_t)((totalWithCrc + chunkSize - 1) / chunkSize);
  if (totFrags == 0) totFrags = 1;

  // Take mutex ONCE for the whole burst — see G2_SENDPB_BURST_MUTEX_MS.
  if (xSemaphoreTake(arm.writeMutex, pdMS_TO_TICKS(G2_SENDPB_BURST_MUTEX_MS)) !=
      pdTRUE) {
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
    // does. 15 ms per gap gives the firmware's reassembler time to copy
    // each fragment into its slot and arm for the next one without
    // measurable user-visible latency (a 14-frag Settings push pays
    // ≤210 ms total). Keep this skip on the last frag — no point delaying
    // after the message is complete.
    //
    // The Transport Tests bench can crank the cadence up via
    // g2DebugSetNextBurstFragDelay() to test whether the firmware's
    // per-widget reassembly ceiling moves with timing. Override is a
    // one-shot — read into burstDelayMs at the top of this function and
    // cleared, so it only applies to a single sendPbFragmented call.
    if (i + 1 < totFrags) vTaskDelay(pdMS_TO_TICKS(burstDelayMs));
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
//   many nested frames, plus a G2_SEND_ENVELOPE_MUTEX_MS wait on writeMutex,
//   which is unsafe on the FreeRTOS `Tmr Svc` daemon task (shared, small stack
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

  // TX-stuck watchdog. sendEnvelope returns false when it can't acquire
  // writeMutex within G2_SEND_ENVELOPE_MUTEX_MS, or the write path fails.
  // We allow several consecutive misses (long bursts + BLE stalls) before
  // forcing disconnect. See txStuckBeats on G2Temple.
  const bool sent = (n != 0) && sendEnvelope(t, buf, n);
  if (sent) {
    t.txStuckBeats = 0;
  } else {
    if (t.txStuckBeats < 0xFF) t.txStuckBeats++;
    if (t.txStuckBeats >= G2_TX_STUCK_DISCONNECT_BEATS) {
      const unsigned approxSec =
          (unsigned)((uint32_t)t.txStuckBeats * HEARTBEAT_PERIOD_MS / 1000u);
      BROADCAST_PRINTF("[G2] %s temple TX wedged (%u consecutive heartbeat "
                       "send failures, ~%u s) — forcing disconnect to "
                       "recover BLE state",
                       t.side == 'L' ? "LEFT" : "RIGHT",
                       (unsigned)t.txStuckBeats, approxSec);
      logSystemEvent("G2", "%s temple TX wedged (%u consecutive heartbeat send failures) — forcing disconnect to recover",
                     t.side == 'L' ? "LEFT" : "RIGHT", (unsigned)t.txStuckBeats);
      g2RingDump(t.side == 'L' ? "tx-wedged (L)" : "tx-wedged (R)");
      g2PushStatusEvent(t.side == 'L' ? "tx-wedged-L" : "tx-wedged-R");
      // disconnectTemple will itself try a clean Cmd=9 SHUTDOWN via
      // sendEnvelope; that send will also time out on the same wedged
      // mutex (logged once). It then proceeds to client->disconnect()
      // which operates directly on the BT controller — no mutex needed
      // — and returns the temple to a state where the next reconnect
      // can rebuild from scratch.
      t.txStuckBeats = 0;
      disconnectTemple(t);
      return;  // skip pluginDead bookkeeping below — we're disconnected
    }
  }

  // LEFT-arm exception on firmware that silences L's notify channel:
  // heartbeat acks will never arrive on L, so counting misses there is
  // meaningless and the resulting pluginDead flip is a false alarm. We
  // still send the heartbeat (the write channel works fine and keeps
  // the link warm); we just don't escalate on missed responses.
  if (t.side == 'L' && firmwareSilencesLeftNotify()) return;

  // Saturating pre-increment. The ack handler resets to 0 on receipt. If
  // N sends go unacked, the plugin has stopped servicing sid=0xE0 — stop
  // sending further render commands from g2ShowText et al. until
  // reconnect. Cap at THRESHOLD+1 so the transition condition fires
  // exactly once.
  if (t.heartbeatMissed < HEARTBEAT_DEAD_THRESHOLD + 1) t.heartbeatMissed++;
  if (t.heartbeatMissed == HEARTBEAT_DEAD_THRESHOLD && !t.pluginDead) {
    t.pluginDead = true;
    t.containerReady = false;
    if (t.side == 'R') resetHijackListSwapCache();
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
    // Best available "glasses set down / not worn" proxy — the plugin task
    // goes dormant when the glasses aren't on a head. Debounced upstream by
    // HEARTBEAT_DEAD_THRESHOLD; fires once per silence episode.
    systemEventPost(SYSEVT_G2_NOT_WORN, t.side == 'L' ? "LEFT" : "RIGHT");
  }
}

// ─── Half-connected recovery ────────────────────────────────────────────
// When g2ConnectSync() finishes with one temple connected and the other
// missing — usually because the missing side's MAC was visible during
// scan but its adverts didn't carry a name field, or it just happened to
// be on the wrong advert phase — silently retry the missing arm in the
// background instead of forcing the user to teardown+reconnect.
//
// Backoff schedule. Seven attempts spread over ~12 minutes. The first
// three slots fire within ~45 s of detection so a flaky BLE connect
// gets 2–3 quick retries before we back off — a missing temple often
// connects fine on the second try after the firmware finishes
// resetting its advert cycle. After the last slot we give up and stop
// nagging the user; manual `g2recover` resets the count for another
// seven attempts.
static const uint32_t kRecoveryBackoffMs[] = {
  5ul * 1000,     // first retry 5 s after detection
  10ul * 1000,
  30ul * 1000,
  60ul * 1000,
  120ul * 1000,
  240ul * 1000,
  300ul * 1000,
};
static constexpr uint8_t kMaxRecoveryAttempts =
    sizeof(kRecoveryBackoffMs) / sizeof(kRecoveryBackoffMs[0]);

static uint32_t gNextRecoveryAttemptMs = 0;  // 0 = no attempt scheduled yet
static uint8_t  gRecoveryAttemptCount  = 0;
static bool     gRecoveryGiveUpLogged  = false;  // once-per-episode event latch

// Reset the backoff state. Call from anywhere a fresh recovery cycle is
// warranted — both temples are up (success), one drops (so the next
// detection cycle starts from attempt #1), or the user explicitly asks.
static void resetRecoveryBackoff() {
  gNextRecoveryAttemptMs = 0;
  gRecoveryAttemptCount  = 0;
}

// Synchronous attempt to scan + connect the missing temple. Pauses the
// connected side's heartbeat for the duration of the scan (~3 s) — that's
// well under the firmware's ~10 s beat-silence tolerance, so the live
// link rides through it. Caller is responsible for backoff scheduling.
//
// Returns true if the missing temple was found and connected.
static bool attemptMissingArmRecovery() {
  if (!gG2State || !gG2State->initialized || !gScan) return false;
  // Need exactly one connected and one missing. Both connected = nothing
  // to do; both missing = user initiated a teardown and a fresh
  // g2ConnectSync is the right path, not partial recovery.
  const bool needL = !gL.connected;
  const bool needR = !gR.connected;
  if (needL == needR) return false;

  G2Temple* missing  = needL ? &gL : &gR;
  G2Eye missingEye   = needL ? G2_EYE_LEFT : G2_EYE_RIGHT;
  const char* sideStr = needL ? "LEFT" : "RIGHT";

  // mac1=L, mac2=R per the codebase convention (see g2ConnectSync's
  // bleSavePeerMac call). The save only happens after a successful
  // connect, so a temple whose initial connect failed has no saved MAC
  // — we fall through to a name-based scan in that case rather than
  // skipping recovery entirely (the original bug: missing temple stays
  // unreconnected forever until the user manually triggers g2scan).
  String savedMac = needL ? gBlePeerData[BLE_PEER_G2_GLASSES].mac1
                          : gBlePeerData[BLE_PEER_G2_GLASSES].mac2;
  const bool haveMac = savedMac.length() > 0;

  if (haveMac) {
    BROADCAST_PRINTF("[G2] Recovery: scanning for missing %s temple "
                     "(MAC %s, attempt %u/%u)",
                     sideStr, savedMac.c_str(),
                     (unsigned)(gRecoveryAttemptCount + 1),
                     (unsigned)kMaxRecoveryAttempts);
  } else {
    BROADCAST_PRINTF("[G2] Recovery: scanning for missing %s temple "
                     "(no saved MAC — name-based match, attempt %u/%u)",
                     sideStr,
                     (unsigned)(gRecoveryAttemptCount + 1),
                     (unsigned)kMaxRecoveryAttempts);
  }

  // Tear down any stale advert object so the scan callback can claim
  // the new one. The callback's `if (t.advertisedDevice) return` guard
  // would otherwise drop the match silently.
  if (missing->advertisedDevice) {
    delete missing->advertisedDevice;
    missing->advertisedDevice = nullptr;
  }

  // When we have a saved MAC, filter the scan to just this side's MAC
  // (G2ScanCallbacks honours gG2FilterMacL/R as an authoritative override).
  // When we don't, leave both filters empty so the callback falls back
  // to its name-based classifier (classifyG2Name on "Even G2_32_L_..."
  // / "Even G2_32_R_..." prefixes) — same path the initial g2ConnectSync
  // takes on first-ever connect. gConnectTarget still pins the side, so
  // an advert for the *other* temple won't accidentally claim this slot.
  if (haveMac) {
    if (needL) { gG2FilterMacL = savedMac; gG2FilterMacR = String(); }
    else       { gG2FilterMacL = String(); gG2FilterMacR = savedMac; }
  } else {
    gG2FilterMacL = String();
    gG2FilterMacR = String();
  }
  gConnectTarget = missingEye;
  gScanFoundL = false;
  gScanFoundR = false;

  // Brief scan with the same active-scan tuning as g2ConnectSync. The
  // callback stops the scan early on first match (see G2ScanCallbacks).
  gScan->setActiveScan(true);
  gScan->setInterval(100);
  gScan->setWindow(99);
  gScan->setAdvertisedDeviceCallbacks(new G2ScanCallbacks(), true);
  const uint32_t kScanSec = 3;
  gG2ScanStoppedEarly = false;
  gScan->start(kScanSec, [](BLEScanResults) {}, false);

  // Same poll-with-yield pattern as g2ConnectSync. Yields the heartbeat
  // task while the BLE event handler dispatches advert callbacks; bails
  // as soon as the missing side is populated.
  const uint32_t deadline = millis() + (kScanSec * 1000) + 500;
  while (millis() < deadline) {
    if ((needL && gScanFoundL) || (needR && gScanFoundR)) break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  g2ScanStopIfActive();

  // Clear the filters so the next open scan (manual g2connect, etc.)
  // sees both sides again.
  gG2FilterMacL = String();
  gG2FilterMacR = String();
  gConnectTarget = G2_EYE_AUTO;

  if (!missing->advertisedDevice) {
    DEBUG_G2F("[G2] recovery: %s temple not seen during scan", sideStr);
    return false;
  }

  if (!connectTemple(*missing)) {
    DEBUG_G2F("[G2] recovery: connectTemple() failed for %s", sideStr);
    return false;
  }

  BROADCAST_PRINTF("[G2] Recovery: %s temple reconnected — both sides up",
                   sideStr);
  g2PushStatusEvent("recovery-ok");
  return true;
}

// Heartbeat-tick hook. Decides whether to actually run recovery this
// tick based on current state and the backoff schedule. Idempotent;
// safe to call every tick.
static void recoveryHeartbeatTick() {
  if (!gG2State || !gG2State->initialized) return;

  // Both up — nothing to do, and reset the backoff so the next
  // half-connect cycle starts fresh.
  if (gL.connected && gR.connected) {
    resetRecoveryBackoff();
    return;
  }
  // Both down — explicit teardown path, not our business.
  if (!gL.connected && !gR.connected) {
    resetRecoveryBackoff();
    return;
  }

  // Half-connected. First time we land here, schedule the first attempt.
  if (gNextRecoveryAttemptMs == 0) {
    gNextRecoveryAttemptMs = millis() + kRecoveryBackoffMs[0];
    gRecoveryAttemptCount  = 0;
    gRecoveryGiveUpLogged  = false;  // new episode — re-arm the give-up event
    return;
  }
  // Out of attempts — silent give-up. Manual g2recover resets.
  if (gRecoveryAttemptCount >= kMaxRecoveryAttempts) {
    if (!gRecoveryGiveUpLogged) {
      gRecoveryGiveUpLogged = true;
      logSystemEvent("G2", "half-connected recovery gave up: %s temple still missing after %u attempts",
                     gL.connected ? "RIGHT" : "LEFT", (unsigned)kMaxRecoveryAttempts);
    }
    return;
  }
  // Not yet time.
  if ((int32_t)(millis() - gNextRecoveryAttemptMs) < 0) return;

  const bool ok = attemptMissingArmRecovery();
  gRecoveryAttemptCount++;
  if (ok) {
    resetRecoveryBackoff();
  } else if (gRecoveryAttemptCount < kMaxRecoveryAttempts) {
    gNextRecoveryAttemptMs =
        millis() + kRecoveryBackoffMs[gRecoveryAttemptCount];
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
      // Hijack-safety watchdog: if the hijack has been active longer than
      // HIJACK_SAFETY_MS without a clean exit, force-shut it down so the
      // lens doesn't hang stuck on our content. The watchdog deliberately
      // runs even while a probe is active — Q13/Q14 are now capped at
      // 30 s by their own safety caps, which sits comfortably under the
      // 60 s watchdog. Earlier mitigation suppressed this during probes,
      // but that papered over a deeper issue (probes running past the
      // firmware's own ~30 s lens-idle window were fighting natural lens
      // lifecycle). The honest fix is the per-probe cap, not weakening
      // the watchdog.
      //
      // Gate on gHijackStartedMs > 0 so we don't fire on CLI-fired probes
      // from cold-Idle: those enter ImageProbing without ever transitioning
      // through HijackEnter, so no start timestamp was recorded. Phase 6
      // broadened g2FsmHijackActive() to include ImageProbing — without
      // this gate, every g2bmp / QGlizzy from cold would trip the watchdog
      // on the first heartbeat tick after device-uptime > 60 s.
      if (g2FsmHijackActive() && gHijackStartedMs > 0 &&
          (millis() - gHijackStartedMs) > HIJACK_SAFETY_MS) {
        sendHijackShutdown("safety-timeout");
      }

      // Half-connected recovery — runs after heartbeats so the live
      // arm's beat doesn't get crowded by a recovery scan on the same
      // tick. Cheap to call every tick: bails immediately when both
      // sides are connected (steady state).
      recoveryHeartbeatTick();
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
    // xTaskCreate stack lives in **internal** DRAM. NimBLE + dual GATT clients can
    // leave only a few KB internal while PSRAM still shows megabytes free —
    // ESP.getFreeHeap() is misleading here.
    const uint32_t totalFree  = (uint32_t)ESP.getFreeHeap();
    const uint32_t internalFree =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // ESP-IDF xTaskCreate: stack size is bytes. TCB + alignment ≈ 512–1k extra.
    uint32_t stackBytes = 3072;
    if (internalFree < 9000) {
      stackBytes = 2560;
    }
    if (internalFree < 7000) {
      stackBytes = 2048;
    }
    const uint32_t needApprox = stackBytes + 1200u;
    if (internalFree < needApprox) {
      DEBUG_G2F("[G2] heartbeat: worker NOT started — internal DRAM %u B "
                "(total heap %u B) < ~%u B needed for stack+TCB. "
                "Dual BLE clients exhausted internal heap; heartbeats will not run.",
                (unsigned)internalFree, (unsigned)totalFree, (unsigned)needApprox);
      gBeatTaskHandle = nullptr;
      return;
    }
    BaseType_t rc = xTaskCreate(heartbeatWorkerTask, "g2_hb_worker",
                                stackBytes, nullptr,
                                /*prio*/ 5, &gBeatTaskHandle);
    if (rc != pdPASS) {
      DEBUG_G2F("[G2] heartbeat: worker task create failed (stack=%u internal=%u)",
                (unsigned)stackBytes, (unsigned)internalFree);
      gBeatTaskHandle = nullptr;
      return;
    }
    DEBUG_G2F("[G2] heartbeat: worker started (stack=%u internal=%u total=%u)",
              (unsigned)stackBytes, (unsigned)internalFree, (unsigned)totalFree);
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

// After the right temple is up and the prelude has settled, push the
// ESP's current epoch + tz offset to the glasses' RTC. This handles the
// "G2 fully discharged and lost the time it got from the phone" case —
// once we connect and the ESP has a valid NTP-synced clock, the glasses
// get the right time without the user needing to repair to the phone.
//
// Mirrors the `g2devcfg time` CLI path (uses the same builder + arm). No
// hijack involved — straight SID 0x80 / cmd=128 to the native firmware.
//
// Footgun: G2 takes timezone as **quarter-hours** from UTC (PST = -32,
// JST = +36). Clock::tzOffsetQuarterHours() does the minutes/15 conversion
// in one place with an explicit unit name. R1 wants raw minutes — use
// Clock::tzOffsetMinutes() there. The unit-named accessors make the swap
// impossible at the type-system level.
static void g2AutoTimeSyncIfReady(G2Temple& t) {
  if (t.side != 'R') return;          // builder targets right arm only
  if (!t.connected || t.pluginDead) return;

  const time_t now = Clock::epochSeconds();
  // Sanity-check ESP has a real (post-2020) time. If NTP hasn't run yet
  // we'd push garbage — better to skip and let a later trigger retry.
  if (!Clock::isValidEpoch(now)) {
    DEBUG_G2F("[G2-R] Auto time-sync skipped: ESP RTC not yet NTP-synced (now=%ld)",
              (long)now);
    return;
  }
  const int32_t tzQ = Clock::tzOffsetQuarterHours();

  uint8_t env[96];
  const size_t n = g2BuildDevCfgTimeSync(allocSeq(), G2_MAGIC_DEVCFG_TIME_SYNC,
                                         (uint32_t)now, tzQ, env, sizeof(env));
  if (n == 0) {
    DEBUG_G2F("[G2-R] Auto time-sync: builder rejected (tzQ=%ld out of ±56?)",
              (long)tzQ);
    return;
  }
  if (!sendEnvelope(t, env, n)) {
    DEBUG_G2F("[G2-R] Auto time-sync: send failed");
    return;
  }
  DEBUG_G2F("[G2-R] Auto time-sync sent: epoch=%lu tzQ=%ld (tzMin=%d)",
            (unsigned long)now, (long)tzQ, gSettings.tzOffsetMinutes);
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
    // ESP32 Arduino BLE leaks every client we don't free ourselves —
    // BLEDevice::createClient() just overwrites a static m_pClient with
    // the latest `new BLEClient()`, never freeing the prior one
    // (BLEDevice.cpp:146). ~BLEClient() iterates m_servicesMap and
    // deletes each BLERemoteService, which chain-deletes their
    // characteristics and descriptors — so a plain `delete` reclaims the
    // full GATT cache. We own the pointer; the library doesn't.
    delete t.client;
    t.client = nullptr;
  }
  t.writeChar = nullptr;
  t.notifyChar = nullptr;
  t.audioNotifyChar = nullptr;
  t.connected = false;
  t.containerReady = false;
  // deinit wipes both temples, which invariably drops hijack state too.
  // Fire HijackExit so the FSM ends in Idle; the apply path also clears
  // the lens-mirror container fields.
  g2LensSetHijackActive(false);
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
  // Fresh session — reset the TX-stuck counter so the watchdog doesn't
  // carry forward state from a previous wedged-then-disconnected
  // session.
  t.txStuckBeats = 0;
  // Tear down any stale client from a previous unexpected drop. Arduino BLE
  // doesn't reliably reuse a client whose peer disappeared mid-session.
  if (t.client && t.clientStale) {
    DEBUG_G2F("[G2-%c] Replacing stale BLEClient from prior drop", t.side);
    // The library doesn't track our client — its static m_pClient holds
    // only whichever `new BLEClient()` was created last. So nulling our
    // pointer without delete just orphaned the object and its GATT
    // cache. ~BLEClient destroys every BLERemoteService it cached, which
    // chain-deletes characteristics and descriptors. Earlier comment
    // claimed the leak was an "acceptable trade for reliability" —
    // measured at ~10-14 KB per cycle, OOMing the device in ~10
    // reconnects. Just free it.
    delete t.client;
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
  // Pause sensor polling for the whole connect + GATT-discovery window. This is
  // the heaviest BLE-coexistence moment: BLEClient::connect() followed by
  // service/characteristic lookup and registerForNotify() walks the peer's
  // attribute table (the retrieveDescriptors traffic). With the gamepad still
  // polling its bus (Wire1 / I2C_NUM_1) every 90 ms, an RF-glitched I2C
  // transaction makes the legacy driver re-arm from inside its ISR — an
  // interrupt storm that trips the Int WDT on CPU0.
  //
  // Scoped to the gamepad's bus (gSettings.inputBus) rather than blanket: the
  // storm was always on the gamepad bus (I2C_NUM_1), and sensors on the OTHER
  // bus (e.g. the OLED) polled through every crash without issue — so they can
  // keep rendering during the connect instead of freezing. If inputBus is unset
  // (-1 → 0xFF), this falls back to a blanket pause (the safe direction).
  // RAII so every early-return below resumes; ref-counted + nest-safe, so the
  // per-arm L/R calls compose cleanly.
  PollPauseGuard pollGuard((uint8_t)gSettings.inputBus);

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
  // BluetoothGatt.CONNECTION_PRIORITY_HIGH that faceclaw uses. Default
  // GAP negotiation yields ~30–50 ms; HIGH brings it to ~11.25–15 ms,
  // tripling connection events available for TX (matters during
  // multi-fragment image bursts). Peer may counter-offer; negotiated
  // values land in ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT.
  //
  // We can also flip back to BALANCED dynamically — see
  // g2SetAllTemplesConnPriority() in this file. Used during ring
  // connect attempts to free up BLE-controller radio time.
  esp_err_t ce = setTempleConnParams(t, G2_CONN_INT_HIGH_MIN, G2_CONN_INT_HIGH_MAX);
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
  //
  // DISABLED 2026-05-04: each enumerateDiagService call invokes
  // BLERemoteService::getCharacteristics() which allocates a fresh
  // BLERemoteCharacteristic (each ~3 FreeRTOS Semaphores, all in DRAM)
  // for every char in the service. Not the root cause of the heap
  // regression we're chasing, but a real ~3-6 KB/connect tax with no
  // functional benefit beyond logging. Re-enable to inspect new
  // firmware revisions.
  // enumerateDiagService(t, DIAG_SVC_6450, "6450");
  // enumerateDiagService(t, DIAG_SVC_7450, "7450");
  // enumerateDiagService(t, DIAG_SVC_1001, "1001");

  // Subscribe to the audio notify char (6402 on service 6450) on both
  // arms regardless of `g2micon` state — the registration itself is
  // free and the handler ignores frames anyway when stats aren't being
  // examined. This lets `g2micon` flip the firmware-side stream on/off
  // without needing to re-discover the char each time.
  t.audioNotifyChar = nullptr;
  subscribeAudioNotify(t);

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

  // Auto-push our NTP-synced time to the glasses' RTC. Only the right
  // arm goes through here (g2AutoTimeSyncIfReady is a no-op on left).
  g2AutoTimeSyncIfReady(t);

  DEBUG_G2F("[G2-%c] Ready (heap=%u internal=%u)", t.side,
            (unsigned)ESP.getFreeHeap(),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
  // either way, the right-side container we rely on is gone. FSM-side:
  // dispatch HijackExit so its state matches; apply path also clears
  // the lens-mirror container fields.
  if (t.side == 'R') g2LensSetHijackActive(false);
}

// =============================================================================
// Public API
// =============================================================================

// Forward decls — defined further down alongside the unified BLE-connect
// worker. Needed here because initG2Client / deinitG2Client call them but
// are themselves above the worker definitions in the file.
static void bleConnectInit();
static void bleConnectShutdown();

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

  // gG2State lives for the program's life and is touched only by regular
  // task contexts (BLE notify, command dispatch, etc.) — safe in PSRAM.
  // Note: the embedded `String deviceName/deviceAddress` members hold
  // their own heap-backed buffers via Arduino's allocator (DRAM); only
  // the struct shell moves here.
  gG2State = (G2ClientState*)ps_calloc(1, sizeof(G2ClientState),
                                       AllocPref::PreferPSRAM, "g2.clientState");
  if (!gG2State) {
    broadcastOutput("[G2] State alloc failed");
    return false;
  }
  // ps_calloc() zeroes the struct but does NOT run constructors. G2ClientState
  // embeds String members (deviceName/deviceAddress); a zeroed String reads as
  // {isSSO=0, ptr.buff=NULL} so c_str() returns NULL → strlen(NULL) crash if
  // read before assignment. Placement-new runs the constructors (same fix as
  // gEspNow / gSessions / gWifiNetworks).
  new (gG2State) G2ClientState();

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

  // Stand up the FSM worker task + queue so subsequent
  // hijackFsmDispatch() calls run async (Phase 4). Idempotent.
  hijackFsmInit();

  // Persistent page-swap worker. One long-lived task fed by a queue
  // replaces the previous "xTaskCreate per UI action" pattern, which
  // was burning a fresh 4 KB internal-DRAM stack on every
  // navigation step and fragmenting the heap. Spawning at boot
  // means the 4 KB allocation lands while DRAM headroom is largest.
  pageSwapInit();

  // Persistent tap-dispatcher worker. handleDevEvent (on the BLE notify
  // task) enqueues the tap idx; this worker drains the queue and runs
  // handleHijackMenuTap() from a safe task context so allocations and
  // xTaskCreate inside the page handlers never run inside Bluedroid
  // spinlock contexts. Cures the spinlock_acquire(lock->count==0) assert
  // seen on the Blocks-hijack menu-tap path. Spawned alongside the
  // page-swap worker while DRAM headroom is largest. Stack is ~20 KB so
  // settings JSON writes and sensor UI fit comfortably.
  tapDispatcherInit();

  // Group B: persistent BLE-connect worker. Replaces 5 transient
  // xTaskCreate paths (g2 connect/saved + ring connect/saved/mac) with
  // one queue-fed worker. 6 KB stack paid here at G2 init time, when DRAM
  // headroom is largest, instead of transiently per connect.
  bleConnectInit();

#if ENABLE_CAMERA_SENSOR
  g2RegisterSensorsCameraPowerHook();
#endif

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
  // Group B: tear down the unified BLE-connect worker. Frees the 6 KB
  // stack so subsequent BT-mode toggles don't pile up workers. A fresh
  // initG2Client respawns it.
  bleConnectShutdown();
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
  gG2ScanStoppedEarly = false;
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
  g2ScanStopIfActive();

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
  systemEventPost(SYSEVT_G2_CONNECTED,
                  (gL.connected && gR.connected) ? "L+R" : (gL.connected ? "L" : "R"));

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

// =============================================================================
// Unified BLE-connect worker (Group B — see G2_Glasses.h for design notes)
// =============================================================================
// Replaces 5 transient *TaskBody patterns (g2 connect/saved + ring connect/
// saved/mac) with one persistent task spawned in initG2Client(). The 6 KB
// stack is allocated when DRAM headroom is largest, not during a connect
// when it's already tight. Producers (g2Connect / g2RingConnect / etc.)
// build a BleConnectJob, set their per-family in-flight flag, and submit;
// the worker dispatches by kind and clears the flag when *Sync returns.

// Forward decl — g2ConnectSavedSync is static and defined further down.
static bool g2ConnectSavedSync();

static QueueHandle_t gBleConnectQueue   = nullptr;
static TaskHandle_t  gBleConnectTaskH   = nullptr;
static const size_t  kBleConnectQueueDepth = 4;

static void bleConnectWorkerLoop(void* /*arg*/) {
  for (;;) {
    BleConnectJob* job = nullptr;
    if (xQueueReceive(gBleConnectQueue, &job, portMAX_DELAY) != pdTRUE) continue;
    if (!job) continue;

    switch (job->kind) {
      case BleConnectKind::G2_EYE:
        g2ConnectSync((G2Eye)job->eye);
        gConnectTaskActive = false;
        break;

      case BleConnectKind::G2_SAVED:
        g2ConnectSavedSync();
        gConnectTaskActive = false;
        break;

      case BleConnectKind::RING_SCAN:
        ringPerformConnect();
        g2RingConnectMarkComplete();
        break;

      case BleConnectKind::RING_SAVED: {
        // Wait for both glasses to be up before competing for BLE radio.
        // Same logic that ringConnectSavedTaskBody used to do, inlined
        // here since the worker IS the right context for it. With
        // serialization through one queue, this wait is usually trivially
        // satisfied — a preceding G2_SAVED job (if any) finishes first.
        if (g2WaitForBothConnected(20000)) {
          DEBUG_G2F("[RING] auto-reconnect: both glasses up — settling 3s "
                    "before competing for BLE radio");
          vTaskDelay(pdMS_TO_TICKS(3000));
        } else {
          DEBUG_G2F("[RING] auto-reconnect: glasses not both ready after 20s — "
                    "proceeding anyway (may degrade if they come up mid-connect)");
        }
        String mac = gBlePeerData[BLE_PEER_R1_RING].mac1;
        mac.trim();
        if (mac.length() == 0) {
          DEBUG_G2F("[RING] auto-reconnect: no saved MAC — skipping");
        } else {
          ringPerformConnect(mac);
        }
        g2RingConnectMarkComplete();
        break;
      }

      case BleConnectKind::RING_MAC:
        ringPerformConnect(String(job->mac));
        g2RingConnectMarkComplete();
        break;
    }
    delete job;
  }
}

// Idempotent — first call creates the queue + spawns the worker, subsequent
// calls are no-ops. Called from initG2Client() so the worker only exists
// when G2 client mode is active (BT off ⇒ no connect worker stack pinned).
static void bleConnectInit() {
  if (gBleConnectQueue) return;
  gBleConnectQueue = xQueueCreate(kBleConnectQueueDepth, sizeof(BleConnectJob*));
  if (!gBleConnectQueue) {
    DEBUG_G2F("[G2] ble-connect: queue create FAILED (depth=%u)",
              (unsigned)kBleConnectQueueDepth);
    return;
  }
  // 6 KB matches the largest of the retired transient stacks (g2 connect
  // was 6 KB, ring was 5 KB). Spawned here at G2-init time, when internal
  // DRAM is largest, instead of transiently per connect when it's tight.
  // Stack in WORDS (4 bytes). Historical 6144 was 24 KB. Observed peak
  // during dual-temple connect (deepest BLE callstack we ever hit) is
  // ~9.4 KB. 5120 words = 20 KB leaves ~10 KB headroom and reclaims
  // 4 KB DRAM. Don't go lower — connect-time service discovery has
  // genuinely unbounded depth on certain failure paths.
  if (xTaskCreate(bleConnectWorkerLoop, "g2_ble_connect",
                  /*stack words*/ 5120, nullptr,
                  /*prio*/  5,         &gBleConnectTaskH) != pdPASS) {
    DEBUG_G2F("[G2] ble-connect: worker xTaskCreate FAILED");
    vQueueDelete(gBleConnectQueue);
    gBleConnectQueue = nullptr;
    gBleConnectTaskH = nullptr;
  } else {
    DEBUG_G2F("[G2] ble-connect: persistent worker started "
              "(queue depth=%u, stack=6 KB)", (unsigned)kBleConnectQueueDepth);
  }
}

// Tear down the worker + drain pending jobs. Called from deinitG2Client().
// vTaskDelete on a worker mid-connect leaks BLE-stack state — but deinit
// is the user explicitly nuking G2, so that's acceptable. Subsequent
// initG2Client() respawns cleanly.
static void bleConnectShutdown() {
  if (gBleConnectTaskH) {
    vTaskDelete(gBleConnectTaskH);
    gBleConnectTaskH = nullptr;
  }
  if (gBleConnectQueue) {
    BleConnectJob* job = nullptr;
    while (xQueueReceive(gBleConnectQueue, &job, 0) == pdTRUE) {
      delete job;
    }
    vQueueDelete(gBleConnectQueue);
    gBleConnectQueue = nullptr;
  }
}

bool g2SubmitBleConnect(const BleConnectJob& job) {
  if (!gBleConnectQueue) {
    DEBUG_G2F("[G2] g2SubmitBleConnect: queue not init'd — call initG2Client first");
    return false;
  }
  BleConnectJob* heap = new (std::nothrow) BleConnectJob(job);
  if (!heap) {
    DEBUG_G2F("[G2] g2SubmitBleConnect: heap alloc failed");
    return false;
  }
  // Short timeout so a wedged worker fails the producer fast rather than
  // blocking the BLE notify task / CLI handler indefinitely.
  if (xQueueSend(gBleConnectQueue, &heap, pdMS_TO_TICKS(50)) != pdTRUE) {
    DEBUG_G2F("[G2] g2SubmitBleConnect: queue full (kind=%d)", (int)job.kind);
    delete heap;
    return false;
  }
  return true;
}

// Public API: non-blocking connect. Returns immediately after enqueueing.
// Use g2status to observe progress (scanning → connecting → connected | idle).
bool g2Connect(G2Eye eye) {
  if (gConnectTaskActive) {
    DEBUG_G2F("[G2] Connect already in progress");
    return false;
  }
  if (!gG2State && !initG2Client()) return false;
  // Reset cancel flag BEFORE submitting so a stale signal from a prior
  // disconnect doesn't immediately abort this connect. Must come before
  // setting active=true (otherwise a concurrent g2Disconnect could see
  // active without seeing the reset cancel).
  gConnectCancel = false;
  gConnectTaskActive = true;
  BleConnectJob job{};
  job.kind = BleConnectKind::G2_EYE;
  job.eye  = (uint8_t)eye;
  if (!g2SubmitBleConnect(job)) {
    DEBUG_G2F("[G2] g2SubmitBleConnect(G2_EYE) failed — queue full or worker not init'd");
    gConnectTaskActive = false;
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
  BleConnectJob job{};
  job.kind = BleConnectKind::G2_SAVED;
  if (!g2SubmitBleConnect(job)) {
    DEBUG_G2F("[G2] g2SubmitBleConnect(G2_SAVED) failed — queue full or worker not init'd");
    gConnectTaskActive = false;
    return false;
  }
  return true;
}

void g2Disconnect() {
  // If a connect is in flight, signal cancel + force-disconnect the BLE
  // links so the worker's blocking GATT calls unblock and the *Sync body
  // returns. The worker (shared across G2 + Ring) keeps running for
  // future submissions — we cannot force-delete it here without breaking
  // ring connects (Group B retired the per-call task spawn that g2Disconnect
  // used to be able to nuke). If cancel doesn't work in 2 s we just log
  // and move on; deinitG2Client() is the nuclear option for a wedged BLE
  // stack.
  if (gConnectTaskActive) {
    DEBUG_G2F("[G2] Cancelling in-flight connect");
    gConnectCancel = true;
    if (gL.client && gL.client->isConnected()) gL.client->disconnect();
    if (gR.client && gR.client->isConnected()) gR.client->disconnect();
    const uint32_t deadline = millis() + 2000;
    while (gConnectTaskActive && millis() < deadline) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (gConnectTaskActive) {
      DEBUG_G2F("[G2] Connect did not exit cleanly within 2s — leaving flag set; "
                "subsequent connects will reject until the *Sync body returns. "
                "Use deinitG2Client to fully reset.");
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

bool g2BothConnected() {
  return gL.connected && gR.connected;
}

// (G2_CONN_INT_* defined near the top of this file, alongside the forward
// decl of setTempleConnParams, so the connectTemple call path can use them.)

// Internal helper: send CONN_PARAMS update for a temple's peer. Caller is
// responsible for ensuring the BLE link exists (i.e. t.client is valid).
static esp_err_t setTempleConnParams(G2Temple& t, uint16_t min_int, uint16_t max_int) {
  if (!t.client) return ESP_FAIL;
  esp_ble_conn_update_params_t p = {};
  esp_bd_addr_t peer;
  memcpy(peer, t.client->getPeerAddress().getNative(), sizeof(esp_bd_addr_t));
  memcpy(p.bda, peer, sizeof(esp_bd_addr_t));
  p.min_int = min_int;
  p.max_int = max_int;
  p.latency = 0;
  p.timeout = 500;   // 500 × 10 ms = 5 s supervision timeout
  return esp_ble_gap_update_conn_params(&p);
}

int g2SetAllTemplesConnPriority(bool high) {
  const uint16_t min_int = high ? G2_CONN_INT_HIGH_MIN : G2_CONN_INT_BALANCED_MIN;
  const uint16_t max_int = high ? G2_CONN_INT_HIGH_MAX : G2_CONN_INT_BALANCED_MAX;
  const char* tag = high ? "HIGH" : "BALANCED";
  int count = 0;
  if (gL.connected) {
    if (setTempleConnParams(gL, min_int, max_int) == ESP_OK) {
      DEBUG_G2F("[G2-L] Conn priority → %s (req %u-%u ms)",
                tag, (unsigned)(min_int * 5 / 4), (unsigned)(max_int * 5 / 4));
      count++;
    }
  }
  if (gR.connected) {
    if (setTempleConnParams(gR, min_int, max_int) == ESP_OK) {
      DEBUG_G2F("[G2-R] Conn priority → %s (req %u-%u ms)",
                tag, (unsigned)(min_int * 5 / 4), (unsigned)(max_int * 5 / 4));
      count++;
    }
  }
  return count;
}

bool g2WaitForBothConnected(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (g2BothConnected()) return true;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

static bool fillTempleMac(const G2Temple& t, uint8_t out[6]) {
  if (!out) return false;
  memset(out, 0, 6);
  if (!t.connected || !t.client) return false;
  esp_bd_addr_t peer;
  memcpy(peer, t.client->getPeerAddress().getNative(), sizeof(esp_bd_addr_t));
  // esp_bd_addr_t is stored MSB-first (matches the colon-string form).
  memcpy(out, peer, 6);
  return true;
}

bool g2GetLeftTempleMac(uint8_t out[6])  { return fillTempleMac(gL, out); }
bool g2GetRightTempleMac(uint8_t out[6]) { return fillTempleMac(gR, out); }

bool g2SendToRightTemple(const uint8_t* env, size_t envLen) {
  if (!env || envLen == 0) return false;
  if (!gR.connected || gR.pluginDead) return false;
  return sendEnvelope(gR, env, envLen);
}

uint8_t g2AllocSeq() { return allocSeq(); }

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
void g2StopScan() { g2ScanStopIfActive(); }

// Default widgetId for CREATE_STARTUP_PAGE — matches the reference's
// default. Overridden only by the Blocks hijack path (widgetId=10509) so
// the firmware recognises our CREATE as the Blocks widget it just
// announced via cmd=17 and doesn't reject it as a same-name collision.
static constexpr uint32_t G2_DEFAULT_WIDGET_ID = 10000;

// =============================================================================
// Container state-mutation helpers
// =============================================================================
// Centralizes the small set of state mutations that follow a
// CREATE / Shutdown / page-swap. Originally duplicated across 7+
// sites (hijackWorkerTask, g2ShowText, pageSwapJobBody's 4 PSK
// branches + auto-recovery), where each site had its own inline
// 3-line "arm.containerReady = true; arm.containerIsList = ...;
// g2LensSetContainer(...);" sequence. Centralizing eliminates the
// "fix lands in one place, lags in the others" failure mode we hit
// repeatedly during the live-text / Status / Selection-S work.
//
// Per-temple flags (arm.containerReady / containerIsList) are the
// legacy fields the wire helpers read directly to decide CREATE-vs-
// REBUILD. gLens is the FSM-managed mirror that external readers
// (CLI, web, status broadcasts) consume via g2LensGetState(). Both
// must stay in sync — that's why they're updated together here.

// Mark a CREATE as successful: the firmware acked, the container is
// live on the lens, and we should treat REBUILD as the next render
// op (not CREATE). Pass isList=true for ListContainer CREATEs (where
// taps generate ListEvent CLICK natively), false for TextContainer
// or compound CREATEs that are not list-driven (where taps go through
// the gTextView* fallback channel).
static void g2NoteCreateSuccess(G2Temple& arm, bool isList,
                                uint32_t widgetId) {
  arm.containerReady  = true;
  arm.containerIsList = isList;
  g2LensSetContainer(true, isList, widgetId);
}

// Mark the container as torn down. Used between Shutdown TX and the
// follow-up CREATE so a failed CREATE leaves a known state, and so
// any concurrent reader of arm.containerReady (e.g. g2ShowText
// auto-route) sees "no container" and takes the CREATE path on its
// next call.
static void g2NoteContainerCleared(G2Temple& arm) {
  arm.containerReady  = false;
  arm.containerIsList = false;
  g2LensClearContainer();
  resetHijackListSwapCache();
}

// gTextView* exit-handler slot. Encapsulates the four file-scope
// flags (gTextViewActive / gTextViewActivatedMs / gTextViewExitFn /
// gTextViewTapFn) so the arm/disarm contract is explicit at the
// call site. Originally mutated inline in 5+ places; extracting
// helpers prevents two recurring bugs:
//   * Forgetting to stamp gTextViewActivatedMs when arming → the
//     post-CREATE settle-pulse grace window doesn't apply, and a
//     synthetic USER_ACTIVITY immediately after CREATE trips the
//     exit handler before the widget renders.
//   * Forgetting to clear gTextViewExitFn when disarming → a stale
//     exit handler from a prior page fires on the next text-mode
//     gesture and tears down the wrong page.
//
// Arm: stamps activatedMs to "now" so the firmware's auto-emitted
// USER_ACTIVITY pulse ~150 ms after CREATE is gated by the grace
// check in handleEnvelope's SysEvent dispatcher.
static void g2TextViewArm() {
  gTextViewActive      = true;
  gTextViewActivatedMs = millis();
}

// Disarm: clears active flag AND both callback slots so a stale
// handler can't fire on the next page.
static void g2TextViewDisarm() {
  gTextViewActive = false;
  gTextViewExitFn = nullptr;
  gTextViewTapFn  = nullptr;
}

// =============================================================================
// Sync ack-and-wait shared boilerplate
// =============================================================================
// All six sendCreate*AndWait / sendRebuild*AndWait helpers share the
// same pre/post pattern around their differing build+send step:
//
//   1. Verify the ack semaphore exists (init path could be broken).
//   2. Set the expected magic byte + clear the ok flag.
//   3. Drain any stale signal from a prior timed-out call.
//   4. ── build pb / envelope and ship it ── (varies per helper)
//   5. Wait up to 1500 ms for the matching ack on the semaphore.
//   6. Return ok if firmware acked success, else log and return false.
//
// The helpers below extract steps 1-3 and 5-6 so each public sync-wait
// helper just does step 4 between an arm*Slot and a wait*Ack call.
// They also embed the deadlock guard (G2_ASSERT_NOT_NOTIFY_TASK) so
// every sync-wait path is protected without needing the macro at each
// public-helper site.

// Caller log ID. The string lives at the public helper's call site so
// log lines can stay distinct ("CREATE-list" vs "CREATE-text" etc.).
//
// If invoked from the BLE notify task, the wait below would deadlock:
// the matching CreateResp/RebuildResp is delivered BY this very task,
// so we'd be sleeping on a semaphore only the sleeper can give. Log
// the violation loudly and refuse to ship — graceful degradation
// rather than a 1.5 s timeout that leaves the lens in a torn-down
// state. (g2StartLiveTextPage hit this 2026-04-30; the fix moved the
// handshake to a worker. This guard catches the regression class.)
#define G2_ASSERT_NOT_NOTIFY_TASK(who) do {                              \
  TaskHandle_t _cur = xTaskGetCurrentTaskHandle();                       \
  if (gBleNotifyTaskHandle && _cur == gBleNotifyTaskHandle) {            \
    DEBUG_G2F("[G2] %s: ABORTED — called on BLE notify task. "           \
              "The 1.5 s ack-wait would deadlock because the response "  \
              "is delivered by this task. Spawn a worker task and call " \
              "from there.", (who));                                     \
    return false;                                                        \
  }                                                                      \
} while (0)

// Arm the CREATE ack slot. Returns true if the slot is ready for a
// build+send, false (with log) if the slot isn't initialised or the
// caller is on the BLE notify task.
static bool armCreateSlot(const char* who) {
  G2_ASSERT_NOT_NOTIFY_TASK(who);
  if (!gCreateAckSem) {
    DEBUG_G2F("[G2] %s: ack sem not ready (init path broken?)", who);
    return false;
  }
  gExpectMagic = (uint8_t)G2_MAGIC_CREATE;
  gCreateOk    = false;
  xSemaphoreTake(gCreateAckSem, 0);  // drain stale signal
  return true;
}

// Wait up to 1500 ms for the CREATE ack. Returns true iff the
// firmware acked success. `widgetId` is for log context only.
static bool waitCreateAck(const char* who, uint32_t widgetId) {
  if (xSemaphoreTake(gCreateAckSem, pdMS_TO_TICKS(1500)) != pdTRUE) {
    DEBUG_G2F("[G2] %s timeout — container not primed (widgetId=%u)",
              who, (unsigned)widgetId);
    return false;
  }
  if (!gCreateOk) {
    DEBUG_G2F("[G2] %s rejected by firmware (widgetId=%u)",
              who, (unsigned)widgetId);
    return false;
  }
  return true;
}

// REBUILD-family equivalents. Separate slot (gRebuildAckSem +
// gExpectRebuildMagic + gRebuildOk) because CREATE acks are Cmd=1 and
// REBUILD acks are Cmd=8 — handleEnvelope dispatches each to its own
// sem so a CREATE/REBUILD pair can't get crossed.
static bool armRebuildSlot(const char* who) {
  G2_ASSERT_NOT_NOTIFY_TASK(who);
  if (!gRebuildAckSem) {
    DEBUG_G2F("[G2] %s: ack sem not ready", who);
    return false;
  }
  gExpectRebuildMagic = (uint8_t)G2_MAGIC_REBUILD;
  gRebuildOk          = false;
  xSemaphoreTake(gRebuildAckSem, 0);
  return true;
}

static bool waitRebuildAck(const char* who) {
  if (xSemaphoreTake(gRebuildAckSem, pdMS_TO_TICKS(1500)) != pdTRUE) {
    DEBUG_G2F("[G2] %s timeout — no RebuildResp", who);
    return false;
  }
  if (!gRebuildOk) {
    DEBUG_G2F("[G2] %s rejected by firmware", who);
    return false;
  }
  return true;
}

// (The legacy sendCreateAndWait was here. It built a CREATE_STARTUP_PAGE
// envelope into a 1 KB on-stack buffer and shipped via single-fragment
// sendEnvelope. Both limits failed once Status snapshots grew past
// ~256 B — the build returned n=0 and the live-text worker bailed.
// Removed 2026-04-30; g2ShowText now routes its CREATE branch through
// sendCreateTextAndWait, which uses an 8 KB PSRAM buffer + multi-
// fragment sendPbFragmented. See git history for the legacy code.)

// List-flavoured CREATE: ListContainerProperty with N items and native
// touchpad capture. Same ack-and-wait contract as the (now-retired)
// sendCreateAndWait.
// Firmware draws the selection box; touchpad gestures route to
// List_ItemEvent sub-messages on sid=0x0D (currently logged as UNKNOWN
// events until we extend dispatchEventPayload to decode them).
// containerName defaults to CONTAINER_NAME ("app") — pass a different
// string only for experiments that need to send a CREATE under a
// non-default container name (the dual-pane CREATE probe used this
// before the firmware proved it doesn't allow it; kept as a hook
// because compound-name patterns may emerge later). Default lives on
// the forward declaration above; this definition takes the parameter
// without a default (C++ rule: the default must appear in exactly one
// declaration that callers see).
static bool sendCreateListAndWait(G2Temple& arm,
                                  const char* const* items, size_t itemCount,
                                  uint32_t widgetId,
                                  const G2ContainerGeom& geom,
                                  const char* containerName) {
  if (!armCreateSlot("CREATE-list")) return false;

  // Heap-allocate the pb body. 8 KB headroom comfortably handles the
  // longest list page we generate today (Settings PRETTY view at 60
  // rows × ~50 chars ≈ 3 KB pb, plus pb tag overhead). Anything larger
  // implies UX-level pagination is overdue regardless of transport.
  // The send path below fragments this body into N envelopes per the
  // wire protocol — see sendPbFragmented.
  // PSRAM-preferred — buffer is filled from regular task context
  // (page-swap worker), no DMA / ISR access. Falls back to internal
  // heap if PSRAM is exhausted.
  constexpr size_t kPbCap = 8192;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPSRAM, "g2.pb.create-list");
  if (!pb) {
    DEBUG_G2F("[G2] CREATE-list: ps_alloc(%u) failed", (unsigned)kPbCap);
    return false;
  }
  size_t pbLen = g2BuildCreateListPagePb(G2_MAGIC_CREATE, containerName,
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
  return waitCreateAck("CREATE-list", widgetId);
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
  if (!armRebuildSlot("REBUILD-list")) return false;

  constexpr size_t kPbCap = 8192;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPSRAM, "g2.pb.rebuild-list");
  if (!pb) {
    DEBUG_G2F("[G2] REBUILD-list: ps_alloc(%u) failed", (unsigned)kPbCap);
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
  // Single-fragment envelope cap. The negotiated ATT MTU is 244, so a
  // single ATT_WRITE_REQ carries up to MTU-3=241 bytes of payload.
  // g2BuildRebuildList builds a one-shot envelope (no fragmentation),
  // so anything over the wire-write limit is a wire-level failure
  // mode: the firmware sees a partial frame and replies with an
  // 8-byte error envelope (seq matches ours, len=0, flag=0x02), our
  // ack wait times out after 1.5 s, and the page-swap worker falls
  // back to SHUTDOWN+CREATE anyway. Reject up-front instead of paying
  // the timeout. Verified with a 9-item / 258 B REBUILD on firmware
  // 2.2.0.24 (2026-04-30).
  //
  // 240 B leaves 1 B of safety vs the 241 B per-write limit. If a
  // future build negotiates a higher MTU we could lift this cap, or
  // teach g2BuildRebuildList to do envelope-level fragmentation
  // (mirrors what sendPbFragmented does for the CREATE family).
  constexpr size_t kSingleFragmentCap = 240;
  if (envLen > kSingleFragmentCap) {
    DEBUG_G2F("[G2] REBUILD-list: env=%u B exceeds %u B single-fragment "
              "limit (%u items) — failing fast so caller falls back to "
              "SHUTDOWN+CREATE without 1.5 s timeout",
              (unsigned)envLen, (unsigned)kSingleFragmentCap,
              (unsigned)itemCount);
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
  return waitRebuildAck("REBUILD-list");
}

// REBUILD-list against an arbitrary container name (vs.
// sendRebuildListAndWait, which hardcodes CONTAINER_NAME for the "app"
// hijacked-Blocks list). Used by compound-CREATE workers (camera stream
// list+image, captioned probes, …) where the list child has its own
// name like "lstCam" so REBUILDing must address it explicitly.
//
// Same wire-fragment cap (240 B) and same envelope+ack flow as the
// CONTAINER_NAME variant — only the name passed into g2BuildRebuildList
// differs.
static bool sendRebuildListNamedAndWait(G2Temple& arm,
                                        const char* listName,
                                        const char* const* items,
                                        size_t itemCount,
                                        const G2ContainerGeom& geom) {
  if (!listName || !items || itemCount == 0) return false;
  if (!armRebuildSlot("REBUILD-list-named")) return false;

  constexpr size_t kPbCap = 8192;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPSRAM, "g2.pb.rebuild-list-named");
  if (!pb) {
    DEBUG_G2F("[G2] REBUILD-list(name=%s): ps_alloc(%u) failed",
              listName, (unsigned)kPbCap);
    return false;
  }
  size_t envLen = g2BuildRebuildList(allocSeq(), G2_MAGIC_REBUILD,
                                     listName,
                                     items, itemCount,
                                     pb, kPbCap, geom);
  constexpr size_t kSingleFragmentCap = 240;
  if (envLen == 0 || envLen > kSingleFragmentCap) {
    DEBUG_G2F("[G2] REBUILD-list(name=%s): build fail or oversized "
              "(env=%u, %u items)",
              listName, (unsigned)envLen, (unsigned)itemCount);
    free(pb);
    return false;
  }
  bool sentOk = sendEnvelope(arm, pb, envLen);
  if (sentOk) {
    DEBUG_G2F("[G2] REBUILD-list(name=%s): %u items, env=%u B sent",
              listName, (unsigned)itemCount, (unsigned)envLen);
  }
  free(pb);
  if (!sentOk) {
    DEBUG_G2F("[G2] REBUILD-list(name=%s): send failed", listName);
    return false;
  }
  return waitRebuildAck("REBUILD-list-named");
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
// containerName defaults to CONTAINER_NAME ("app") — default lives on
// the forward declaration; see sendCreateListAndWait for rationale.
static bool sendCreateTextAndWait(G2Temple& arm,
                                  const char* text,
                                  uint32_t widgetId,
                                  const G2ContainerGeom& geom,
                                  bool eventCapture,
                                  const char* containerName) {
  if (!armCreateSlot("CREATE-text")) return false;

  // Same 8 KB cap as the list path — handles JSON dumps up to that size
  // before the multi-fragment send refuses. Per-module JSON typically
  // sits well under 1 KB, so this is mostly headroom.
  constexpr size_t kPbCap = 8192;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPSRAM, "g2.pb.create-text");
  if (!pb) {
    DEBUG_G2F("[G2] CREATE-text: ps_alloc(%u) failed", (unsigned)kPbCap);
    return false;
  }
  size_t pbLen = g2BuildCreateTextPagePb(G2_MAGIC_CREATE, containerName,
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
  return waitCreateAck("CREATE-text", widgetId);
}

// REBUILD-text equivalent of sendCreateTextAndWait — same pb-only-build +
// sendPbFragmented pattern, but emits Cmd=7 REBUILD_PAGE so the firmware
// updates an existing TEXT container in place. Required for content
// > ~240 B because:
//   * The legacy g2BuildRebuildText caps its internal payload at 256 B
//     (which fails the build for big snapshots).
//   * Even if the build succeeds, g2BuildRebuildText emits a single-
//     fragment envelope that the firmware can't reassemble past
//     ~240 B on the wire (verified 2026-04-30 with REBUILD-list).
// This helper bypasses both: 8 KB pb buffer, multi-fragment envelopes
// via sendPbFragmented (totFrags > 1 protocol-level framing).
//
// Caller is the live-text refresh path (g2ShowText REBUILD branch).
// Geom should match the CREATE's geom — we use G2_GEOM_LARGE here as
// the canonical text-widget rectangle.
static bool sendRebuildTextAndWait(G2Temple& arm,
                                   const char* text,
                                   const G2ContainerGeom& geom,
                                   bool eventCapture) {
  if (!armRebuildSlot("REBUILD-text")) return false;

  constexpr size_t kPbCap = 8192;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPSRAM, "g2.pb.rebuild-text");
  if (!pb) {
    DEBUG_G2F("[G2] REBUILD-text: ps_alloc(%u) failed", (unsigned)kPbCap);
    return false;
  }
  size_t pbLen = g2BuildRebuildTextPb(G2_MAGIC_REBUILD, CONTAINER_NAME,
                                       text ? text : "", geom, eventCapture,
                                       pb, kPbCap);
  if (pbLen == 0) {
    DEBUG_G2F("[G2] REBUILD-text: pb build failed (content_len=%u)",
              (unsigned)(text ? strlen(text) : 0));
    free(pb);
    return false;
  }
  bool sentOk = sendPbFragmented(arm, allocSeq(), G2_SID_EVEN_CORE,
                                  G2_FLAG_REQUEST, pb, pbLen);
  if (sentOk) {
    DEBUG_G2F("[G2] REBUILD-text: pb=%u B sent", (unsigned)pbLen);
  }
  free(pb);
  if (!sentOk) {
    DEBUG_G2F("[G2] REBUILD-text: send failed");
    return false;
  }
  return waitRebuildAck("REBUILD-text");
}

// REBUILD-text targeting a specific named child of a compound container.
// Same wire shape as sendRebuildTextAndWait but with caller-supplied
// containerName so multi-child layouts (e.g. body + battery corner in
// the live Status page) can update each child independently. The probe
// `g2ProbeRebuildTextChild` confirmed firmware 2.2.0.24 honors per-child
// REBUILD-text on a compound CreateStartUpPage.
static bool sendRebuildTextNamedAndWait(G2Temple& arm,
                                        const char* containerName,
                                        const char* text,
                                        const G2ContainerGeom& geom,
                                        bool eventCapture) {
  if (!containerName || !*containerName) return false;
  if (!armRebuildSlot("REBUILD-text(named)")) return false;

  constexpr size_t kPbCap = 8192;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPSRAM, "g2.pb.rebuild-text-named");
  if (!pb) {
    DEBUG_G2F("[G2] REBUILD-text(name=%s): ps_alloc(%u) failed",
              containerName, (unsigned)kPbCap);
    return false;
  }
  size_t pbLen = g2BuildRebuildTextPb(G2_MAGIC_REBUILD, containerName,
                                       text ? text : "", geom, eventCapture,
                                       pb, kPbCap);
  if (pbLen == 0) {
    DEBUG_G2F("[G2] REBUILD-text(name=%s): pb build failed (content_len=%u)",
              containerName, (unsigned)(text ? strlen(text) : 0));
    free(pb);
    return false;
  }
  bool sentOk = sendPbFragmented(arm, allocSeq(), G2_SID_EVEN_CORE,
                                  G2_FLAG_REQUEST, pb, pbLen);
  if (sentOk) {
    DEBUG_G2F("[G2] REBUILD-text(name=%s): pb=%u B sent",
              containerName, (unsigned)pbLen);
  }
  free(pb);
  if (!sentOk) {
    DEBUG_G2F("[G2] REBUILD-text(name=%s): send failed", containerName);
    return false;
  }
  return waitRebuildAck("REBUILD-text(named)");
}

// UPDATE_TEXT (Cmd=5 = APP_UPDATE_TEXT_DATA_PACKET) — per-widget text data
// push, the text equivalent of Cmd=3 image-raw push. Unlike REBUILD-text
// variants, this does NOT blank sibling children of a compound; it just
// patches the named text container's content in place. Verified empirically
// on firmware 2.2.0.24: list+text compound where this gets called against
// the text child every tick keeps the list selection persistent (mirrors
// how camera-stream Cmd=3 image push leaves the list rows alone).
//
// Historical note (G2_Glasses.cpp:7266): we previously avoided UPDATE_TEXT
// after a 2026-04-24 firmware crash. That crash was specifically against a
// LIST container (see L11512). Used against a TEXT child of a compound,
// it's the documented happy path — see System_G2_Protocol.h:467.
//
// Fire-and-forget — the firmware does not ack UPDATE_TEXT separately
// from RX delivery. Returns whether the wire write succeeded; "delivered"
// is a stronger claim than we can make without an ack semaphore.
static bool sendUpdateTextNamed(G2Temple& arm,
                                const char* containerName, uint32_t containerId,
                                const char* content) {
  if (!containerName || !*containerName) return false;
  // Single-fragment envelope is plenty — text content for a status
  // readout fits in well under the 240 B per-fragment cap. If a future
  // caller wants long text (multi-fragment), switch to sendPbFragmented.
  uint8_t env[256];
  // Magic 220 — distinct from CREATE/REBUILD slots so log output is
  // unambiguous. UPDATE_TEXT doesn't ack, so no semaphore prep.
  size_t envLen = g2BuildUpdateText(allocSeq(), 220,
                                    containerName, containerId,
                                    content ? content : "",
                                    env, sizeof(env));
  if (envLen == 0) {
    DEBUG_G2F("[G2] UPDATE_TEXT(name=%s): build failed (content_len=%u)",
              containerName, (unsigned)(content ? strlen(content) : 0));
    return false;
  }
  bool ok = sendEnvelope(arm, env, envLen);
  if (ok) {
    DEBUG_G2F("[G2] UPDATE_TEXT(name=%s, cid=%u): %u B sent",
              containerName, (unsigned)containerId, (unsigned)envLen);
  } else {
    DEBUG_G2F("[G2] UPDATE_TEXT(name=%s): send failed", containerName);
  }
  return ok;
}

// REBUILD-multitext — sends all N children of a compound CreateStartUpPage
// in a single REBUILD message. Required because per-child REBUILD-text on
// a compound (sendRebuildTextNamedAndWait) renders ONLY the named child
// on firmware 2.2.0.24; the other children blank out, leaving only the
// most-recently-rebuilt child visible. The probe g2ProbeRebuildTextChild
// only exercised the single-child case so it didn't catch this. Verified
// empirically with the body+batt Status compound: per-child REBUILD made
// the unaddressed child go dark; multi-child REBUILD keeps both visible.
//
// pb buffer 8 KB to match sendCreateMultiTextAndWait's headroom — Status
// snapshots run ~200 B today but the body field can grow.
static bool sendRebuildMultiTextAndWait(G2Temple& arm,
                                        const G2TextChildSpec* children,
                                        size_t childCount) {
  if (!children || childCount == 0) {
    DEBUG_G2F("[G2] REBUILD-multitext: no children");
    return false;
  }
  if (!armRebuildSlot("REBUILD-multitext")) return false;

  constexpr size_t kPbCap = 8192;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPSRAM,
                                   "g2.pb.rebuild-multitext");
  if (!pb) {
    DEBUG_G2F("[G2] REBUILD-multitext: ps_alloc(%u) failed", (unsigned)kPbCap);
    return false;
  }
  size_t pbLen = g2BuildRebuildMultiTextPb(G2_MAGIC_REBUILD, children, childCount,
                                            pb, kPbCap);
  if (pbLen == 0) {
    DEBUG_G2F("[G2] REBUILD-multitext: pb build failed (%u children)",
              (unsigned)childCount);
    free(pb);
    return false;
  }
  bool sentOk = sendPbFragmented(arm, allocSeq(), G2_SID_EVEN_CORE,
                                  G2_FLAG_REQUEST, pb, pbLen);
  if (sentOk) {
    DEBUG_G2F("[G2] REBUILD-multitext: %u children, pb=%u B sent",
              (unsigned)childCount, (unsigned)pbLen);
  }
  free(pb);
  if (!sentOk) {
    DEBUG_G2F("[G2] REBUILD-multitext: send failed");
    return false;
  }
  return waitRebuildAck("REBUILD-multitext");
}

// CREATE a CreateStartUpPage with N TextObject children at independent
// geometries — used by the Selection Patterns test bench. Mirrors
// sendCreateTextAndWait/sendCreateListAndWait: arms the ack sem,
// builds the multi-text pb, fragments+ships, waits for CreateResp.
//
// SCHEMA RISK: this is the first time the codebase ships a compound
// container with multiple children of the same widget type (TextObject
// repeated under wrapper field 3). The firmware has been verified to
// accept compound containers in the list+image shape (Q16-Q18); if it
// rejects this shape we'll see CreateResp res != 0 and the worker
// auto-recovers back to the hijack root menu via the existing fallback.
extern "C++" bool sendCreateMultiTextAndWait(G2Temple& arm,
                                             const G2TextChildSpec* children,
                                             size_t childCount,
                                             uint32_t widgetId) {
  if (!children || childCount == 0) {
    DEBUG_G2F("[G2] CREATE-multitext: no children");
    return false;
  }
  if (!armCreateSlot("CREATE-multitext")) return false;

  // 1 KB pb cap matches the builder's internal buffer — anything that
  // overflows is a multi-fragment-CREATE request, which the firmware
  // doesn't reassemble for compound containers (verified empirically
  // with Q12). Cap explicitly here so we error before the wire.
  constexpr size_t kPbCap = 1024;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPSRAM, "g2.pb.create-multitext");
  if (!pb) {
    DEBUG_G2F("[G2] CREATE-multitext: ps_alloc(%u) failed", (unsigned)kPbCap);
    return false;
  }
  size_t pbLen = g2BuildCreateMultiTextPb(G2_MAGIC_CREATE, children, childCount,
                                          widgetId, pb, kPbCap);
  if (pbLen == 0) {
    DEBUG_G2F("[G2] CREATE-multitext: pb build failed (%u children)",
              (unsigned)childCount);
    free(pb);
    return false;
  }
  bool sentOk = sendPbFragmented(arm, allocSeq(), G2_SID_EVEN_CORE,
                                  G2_FLAG_REQUEST, pb, pbLen);
  if (sentOk) {
    DEBUG_G2F("[G2] CREATE-multitext: %u children, pb=%u B sent",
              (unsigned)childCount, (unsigned)pbLen);
  }
  free(pb);
  if (!sentOk) {
    DEBUG_G2F("[G2] CREATE-multitext: send failed");
    return false;
  }
  return waitCreateAck("CREATE-multitext", widgetId);
}

// REBUILD_PAGE counterpart for compound with 1 ListObject + N
// TextObject children. Required because per-tick REBUILD-multitext
// (texts only) blanks the list sibling — empirically observed
// 2026-04-30 with the Status back-row compound. Including the list
// child in every REBUILD keeps it on screen.
static bool sendRebuildMixedListMultiTextAndWait(
    G2Temple& arm,
    const char* const* listItems,
    size_t listItemCount,
    const G2ContainerGeom& listGeom,
    const G2TextChildSpec* textChildren,
    size_t textChildCount) {
  if (!listItems || listItemCount == 0) return false;
  if (!textChildren || textChildCount == 0) return false;
  if (!armRebuildSlot("REBUILD-list+multitext")) return false;

  constexpr size_t kPbCap = 8192;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPSRAM,
                                    "g2.pb.rebuild-list-multitext");
  if (!pb) {
    DEBUG_G2F("[G2] REBUILD-list+multitext: ps_alloc(%u) failed",
              (unsigned)kPbCap);
    return false;
  }
  size_t pbLen = g2BuildRebuildMixedListMultiTextPb(
      G2_MAGIC_REBUILD,
      /*listName=*/   CONTAINER_NAME,
      listItems, listItemCount, listGeom,
      textChildren, textChildCount,
      pb, kPbCap);
  if (pbLen == 0) {
    DEBUG_G2F("[G2] REBUILD-list+multitext: pb build failed (1 list + %u text)",
              (unsigned)textChildCount);
    free(pb);
    return false;
  }
  bool sentOk = sendPbFragmented(arm, allocSeq(), G2_SID_EVEN_CORE,
                                  G2_FLAG_REQUEST, pb, pbLen);
  if (sentOk) {
    DEBUG_G2F("[G2] REBUILD-list+multitext: 1 list + %u text, pb=%u B sent",
              (unsigned)textChildCount, (unsigned)pbLen);
  }
  free(pb);
  if (!sentOk) {
    DEBUG_G2F("[G2] REBUILD-list+multitext: send failed");
    return false;
  }
  return waitRebuildAck("REBUILD-list+multitext");
}

// CREATE compound with 1 ListObject + N TextObject children.
// Used by Status to add a tappable "<- Main Menu" back-row at the top
// alongside the existing body/batt/meter text panes. Mirrors the
// existing list+text helper but extends to N text children.
extern "C++" bool sendCreateMixedListMultiTextAndWait(
    G2Temple& arm,
    const char* const* listItems,
    size_t listItemCount,
    const G2ContainerGeom& listGeom,
    const G2TextChildSpec* textChildren,
    size_t textChildCount,
    uint32_t widgetId) {
  if (!listItems || listItemCount == 0) {
    DEBUG_G2F("[G2] CREATE-list+multitext: no list items");
    return false;
  }
  if (!textChildren || textChildCount == 0) {
    DEBUG_G2F("[G2] CREATE-list+multitext: no text children");
    return false;
  }
  if (!armCreateSlot("CREATE-list+multitext")) return false;

  // 1 KB pb cap — same as sendCreateMultiTextAndWait. Status' compound
  // (1 list with one short item + 3 text panes ~250 B total content)
  // fits comfortably.
  constexpr size_t kPbCap = 1024;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPSRAM,
                                    "g2.pb.create-list-multitext");
  if (!pb) {
    DEBUG_G2F("[G2] CREATE-list+multitext: ps_alloc(%u) failed",
              (unsigned)kPbCap);
    return false;
  }
  size_t pbLen = g2BuildCreateMixedListMultiTextPb(
      G2_MAGIC_CREATE,
      /*listName=*/   CONTAINER_NAME,
      listItems, listItemCount, listGeom,
      textChildren, textChildCount,
      widgetId, pb, kPbCap);
  if (pbLen == 0) {
    DEBUG_G2F("[G2] CREATE-list+multitext: pb build failed (1 list + %u text)",
              (unsigned)textChildCount);
    free(pb);
    return false;
  }
  bool sentOk = sendPbFragmented(arm, allocSeq(), G2_SID_EVEN_CORE,
                                  G2_FLAG_REQUEST, pb, pbLen);
  if (sentOk) {
    DEBUG_G2F("[G2] CREATE-list+multitext: 1 list + %u text, pb=%u B sent",
              (unsigned)textChildCount, (unsigned)pbLen);
  }
  free(pb);
  if (!sentOk) {
    DEBUG_G2F("[G2] CREATE-list+multitext: send failed");
    return false;
  }
  return waitCreateAck("CREATE-list+multitext", widgetId);
}

// CREATE compound List + Text container — title + selectable list.
// Same ack/timeout pattern as sendCreateMultiTextAndWait, but builds
// the pb via g2BuildCreateMixedListText. Verified against firmware
// 2.2.0.24 — see G2_PROTOCOL.md for the wire shape.
extern "C++" bool sendCreateMixedListTextAndWait(G2Temple& arm,
                                                  const char* const* items,
                                                  size_t itemCount,
                                                  const G2ContainerGeom& listGeom,
                                                  const G2TextChildSpec& title,
                                                  uint32_t widgetId) {
  if (!items || itemCount == 0) {
    DEBUG_G2F("[G2] CREATE-list+text: no items");
    return false;
  }
  if (!armCreateSlot("CREATE-list+text")) return false;

  constexpr size_t kPbCap = 1024;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPSRAM, "g2.pb.create-list+text");
  if (!pb) {
    DEBUG_G2F("[G2] CREATE-list+text: ps_alloc(%u) failed", (unsigned)kPbCap);
    return false;
  }
  size_t pbLen = g2BuildCreateMixedListTextPb(G2_MAGIC_CREATE,
                                              CONTAINER_NAME, items, itemCount,
                                              listGeom, title, widgetId,
                                              pb, kPbCap);
  if (pbLen == 0) {
    DEBUG_G2F("[G2] CREATE-list+text: pb build failed (%u items)", (unsigned)itemCount);
    free(pb);
    return false;
  }
  bool sentOk = sendPbFragmented(arm, allocSeq(), G2_SID_EVEN_CORE,
                                  G2_FLAG_REQUEST, pb, pbLen);
  if (sentOk) {
    DEBUG_G2F("[G2] CREATE-list+text: %u items + title, pb=%u B sent",
              (unsigned)itemCount, (unsigned)pbLen);
  }
  free(pb);
  if (!sentOk) return false;
  return waitCreateAck("CREATE-list+text", widgetId);
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

  // First show of the session: CREATE_STARTUP_PAGE (Cmd=0) must ack
  // before the firmware will accept REBUILD against this container.
  //
  // We route through sendCreateTextAndWait (8 KB pb buf, multi-fragment
  // wire framing via sendPbFragmented) instead of the legacy
  // sendCreateAndWait (1 KB on-stack, single-fragment envelope) because
  // Status snapshots have grown past both limits. Verified 2026-04-30:
  // tapping Status produced "[G2] CREATE: build failed" (n=0 from
  // g2BuildCreateStartupPage's 1 KB buffer) and the live-text worker
  // bailed before rendering anything.
  //
  // G2_GEOM_LARGE is the canonical text-widget rectangle and matches
  // what the legacy CreateStartUpPage builder used implicitly.
  if (!arm->containerReady) {
    if (!sendCreateTextAndWait(*arm, text, G2_DEFAULT_WIDGET_ID,
                                G2_GEOM_LARGE, /*eventCapture*/ false)) {
      return false;
    }
    g2NoteCreateSuccess(*arm, /*isList*/ false, G2_DEFAULT_WIDGET_ID);
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

  // Subsequent shows — REBUILD_PAGE (Cmd=7) with a fresh TextContainer.
  //
  // Why not UPDATE_TEXT (Cmd=5): it's a partial-patch op
  // (`TextContainerUpgrade {ContentOffset, ContentLength, Content}`)
  // whose exact semantics we never fully nailed down, and we've watched
  // it crash the firmware's plugin task (2026-04-24). REBUILD_PAGE is
  // documented full-replace, well-exercised by the reference, and costs
  // only ~20 extra bytes on the wire.
  //
  // Routes through sendRebuildTextAndWait — pb-only build into 8 KB
  // PSRAM + sendPbFragmented for multi-fragment wire framing. Same
  // rationale as the CREATE branch above: legacy g2BuildRebuildText
  // capped its internal payload at 256 B and emitted a single-fragment
  // envelope, both of which break for Status-snapshot–sized content.
  // Geom must match the CREATE — both use G2_GEOM_LARGE.
  return sendRebuildTextAndWait(*arm, text, G2_GEOM_LARGE, /*eventCapture*/ false);
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
  g2NoteContainerCleared(*arm);
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
  PSK_LIST      = 0,
  PSK_TEXT      = 1,
  PSK_MULTITEXT = 2,   // CreateStartUpPage with N TextObject children at
                       // independent geometries — used by the Selection
                       // Patterns test bench. See g2ShowMultiTextPage.
  PSK_LIST_TEXT = 3,   // CreateStartUpPage with one ListObject + one
                       // TextObject — "title + selectable list" shape.
                       // Native list focus + scroll + click for the
                       // selection part, non-interactive header for
                       // the title. See g2ShowMixedListText.
};

struct PageSwapArgs {
  PageSwapKind     kind;
  // PSK_LIST fields:
  char**           items;        // heap-owned; freed by the worker
  size_t           itemCount;
  // PSK_TEXT fields:
  char*            text;         // heap-owned; freed by the worker
  // Optional follow-up REBUILD-text content (PSK_TEXT only). When
  // non-null, after the CREATE-text(text=placeholder) acks the worker
  // immediately fires a REBUILD-text(content=followUpText) to test
  // whether REBUILD-text scales past the CREATE-text 4-fragment
  // ceiling. Heap-owned; freed by the worker.
  char*            followUpText;
  // PSK_MULTITEXT fields:
  G2TextChildSpec* multiSpecs;   // heap-owned; strings within are heap-owned too
  size_t           multiCount;
  // PSK_LIST_TEXT fields (compound list + title text):
  G2TextChildSpec  titleSpec;    // POD inline; strings below own the
                                 // memory the spec's pointers reference.
  char*            titleName;    // heap-owned strdup
  char*            titleContent; // heap-owned strdup
  G2ContainerGeom  listGeom;     // list geom (titleSpec carries its own)
  // Shared:
  G2ContainerGeom  geom;         // on-lens rectangle for the new container
                                 // (PSK_MULTITEXT / PSK_LIST_TEXT ignore
                                 // this — children carry their own geoms)
};

// Deep-copy `src[0..n-1]` into a heap-owned char* array. Returns nullptr
// on malloc failure (partial allocations are freed). Caller frees both
// the strings and the outer array via freePageSwapItems().
static char** dupPageSwapItems(const char* const* src, size_t n) {
  if (n == 0) return nullptr;
  char** out = (char**)ps_calloc(n, sizeof(char*), AllocPref::PreferPSRAM, "g2.pageSwap.itemArr");
  if (!out) return nullptr;
  for (size_t i = 0; i < n; i++) {
    const char* s = src[i] ? src[i] : "";
    size_t len = strlen(s);
    char* dst = (char*)ps_alloc(len + 1, AllocPref::PreferPSRAM, "g2.pageSwap.itemDup");
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
//
// gPageSwapActive and gOurShutdownAtMs are declared at the top of this
// file alongside their inline access helpers (pageSwapBegin/End/
// InFlight, noteOurShutdownSent, clearOurShutdownStamp,
// ourShutdownEchoActive). All reads/writes of those flags route through
// those helpers — see Phase 0 hijack-FSM refactor.

// Per-job body. Runs the SHUTDOWN+CREATE swap for one PageSwapArgs and
// frees it. Does NOT manage task lifecycle — caller (the persistent
// worker loop) owns the task. Previously this was the body of a
// transient task spawned per UI action; now it's invoked from the
// long-lived `pageSwapWorkerLoop` via a queue so we don't churn
// internal heap by allocating a 4 KB task stack per navigation.
static void pageSwapJobBody(PageSwapArgs* args) {
  // Snapshot at worker entry: did the user have an active hijack when
  // they tapped this menu item? Used to gate auto-recovery — we only
  // recover hijacks we actually broke ourselves, not, say, a tap that
  // arrived while no hijack was active (which shouldn't happen, but be
  // defensive).
  const bool hijackWasActive = g2FsmHijackActive();
  bool didShutdown = false;   // set true once we successfully sent Shutdown
  bool createOk = false;      // declared up here so `goto cleanup` can't
                              // cross its initialization

  // Cancel any active live page (list OR text) before swapping content.
  // The live worker rebuilds the same widget on a tick; if it ran
  // concurrently with our swap, the two REBUILD streams would race.
  // Both stop helpers are cheap (no-op when nothing's live) and wait
  // briefly for the worker to drain before we proceed.
  g2StopLiveListPage();
  g2StopLiveTextPage();

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
      && args->kind == PSK_LIST
      && args->itemCount == gLastHijackListRowCount
      && gLastHijackListRowCount > 0
      && gLastHijackListPageShape == HijackListPageShape::PureList) {
    DEBUG_G2F("[G2] page-swap: attempting REBUILD-list fast path "
              "(items=%u, toggle=on)", (unsigned)args->itemCount);
    if (sendRebuildListAndWait(*arm, (const char* const*)args->items,
                               args->itemCount, args->geom)) {
      DEBUG_G2F("[G2] page-swap: REBUILD-list acked, %u items live "
                "(scroll-position preserved if firmware honours it)",
                (unsigned)args->itemCount);
      createOk = true;
      gLastHijackListRowCount  = args->itemCount;
      gLastHijackListPageShape = HijackListPageShape::PureList;
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
    noteOurShutdownSent();
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
    g2NoteContainerCleared(*arm);
  }

  // Step 2: CREATE the new widget. Branch on kind so the same worker
  // handles every container shape. Same widgetId for all kinds so the
  // firmware sees this as the Blocks app re-launching with new content
  // — same identity, fresh state. Each branch ends in:
  //   * sendCreate*AndWait → captures createOk
  //   * if createOk: g2NoteCreateSuccess + (kind-specific gTextView arm)
  // The gTextView slot is armed for kinds whose taps fall through to
  // the SysEvent exit handler (TEXT, MULTITEXT) and disarmed for
  // list-driven kinds (LIST, LIST_TEXT) where ListEvent CLICKs reach
  // the dispatcher directly.
  if (args->kind == PSK_LIST) {
    createOk = sendCreateListAndWait(*arm, (const char* const*)args->items,
                                     args->itemCount, BLOCKS_WIDGET_ID, args->geom);
    if (createOk) {
      g2NoteCreateSuccess(*arm, /*isList*/ true, BLOCKS_WIDGET_ID);
      gLastHijackListRowCount  = args->itemCount;
      gLastHijackListPageShape = HijackListPageShape::PureList;
      // Coming back from a TEXT view to a LIST view — clear the
      // text-view tracking flags so the next user-input event isn't
      // misinterpreted as a "exit text view" trigger.
      g2TextViewDisarm();
      DEBUG_G2F("[G2] page-swap: CREATE-list acked, %u items live",
                (unsigned)args->itemCount);
    }
  } else if (args->kind == PSK_TEXT) {
    // eventCapture=true is speculative; see sendCreateTextAndWait
    // comment. The text-view-active flag arms the fallback exit path
    // for the case where firmware ignores it.
    createOk = sendCreateTextAndWait(*arm, args->text,
                                     BLOCKS_WIDGET_ID, args->geom,
                                     /*eventCapture=*/ true);
    if (createOk) {
      g2NoteCreateSuccess(*arm, /*isList*/ false, BLOCKS_WIDGET_ID);
      g2TextViewArm();
      DEBUG_G2F("[G2] page-swap: CREATE-text acked (%u B content)",
                (unsigned)(args->text ? strlen(args->text) : 0));
      // Optional follow-up REBUILD-text. Used by the Transport Tests
      // bench to test whether REBUILD-text scales past the CREATE-text
      // ~4-fragment ceiling. The CREATE above seeds the container with
      // a small placeholder (which always fits in 1 fragment), and
      // this REBUILD ships the real test payload via sendPbFragmented.
      // Failure leaves the page showing the placeholder so the user
      // can dismiss normally.
      if (args->followUpText) {
        const size_t fuLen = strlen(args->followUpText);
        DEBUG_G2F("[G2] page-swap: follow-up REBUILD-text (%u B) starting",
                  (unsigned)fuLen);
        const bool rebuildOk = sendRebuildTextAndWait(*arm,
                                                      args->followUpText,
                                                      args->geom,
                                                      /*eventCapture=*/ true);
        if (rebuildOk) {
          DEBUG_G2F("[G2] page-swap: follow-up REBUILD-text acked (%u B)",
                    (unsigned)fuLen);
        } else {
          DEBUG_G2F("[G2] page-swap: follow-up REBUILD-text FAILED (%u B) "
                    "— same firmware ceiling as CREATE-text",
                    (unsigned)fuLen);
        }
      }
    }
  } else if (args->kind == PSK_MULTITEXT) {
    // CreateStartUpPage with N TextObject children at distinct
    // geometries — Selection Patterns test bench. Same gTextView*
    // arming as PSK_TEXT so DOUBLE_CLICK / SysEvent gestures route
    // through the existing exit handler path. The container is not a
    // list (containerIsList=false) — gestures don't generate ListEvent
    // CLICKs unless eventCapture=1 on the child.
    extern bool sendCreateMultiTextAndWait(G2Temple& arm,
                                           const G2TextChildSpec* children,
                                           size_t childCount,
                                           uint32_t widgetId);
    createOk = sendCreateMultiTextAndWait(*arm, args->multiSpecs,
                                          args->multiCount, BLOCKS_WIDGET_ID);
    if (createOk) {
      g2NoteCreateSuccess(*arm, /*isList*/ false, BLOCKS_WIDGET_ID);
      g2TextViewArm();
      DEBUG_G2F("[G2] page-swap: CREATE-multitext acked (%u children)",
                (unsigned)args->multiCount);
    }
  } else /* PSK_LIST_TEXT */ {
    // CreateStartUpPage with one ListObject + one TextObject — title
    // + selectable list. The list manages focus / scroll / CLICK
    // natively, so containerIsList=true is the right state (ListEvent
    // CLICKs on row taps reach the dispatcher). Header is text-only,
    // not tappable. gTextView is left disarmed because the page is
    // list-driven; tap events come through the normal ListEvent path,
    // not the gTextViewExitFn fallback.
    extern bool sendCreateMixedListTextAndWait(G2Temple& arm,
                                               const char* const* items,
                                               size_t itemCount,
                                               const G2ContainerGeom& listGeom,
                                               const G2TextChildSpec& title,
                                               uint32_t widgetId);
    createOk = sendCreateMixedListTextAndWait(*arm,
                                              (const char* const*)args->items,
                                              args->itemCount,
                                              args->listGeom,
                                              args->titleSpec,
                                              BLOCKS_WIDGET_ID);
    if (createOk) {
      g2NoteCreateSuccess(*arm, /*isList*/ true, BLOCKS_WIDGET_ID);
      gLastHijackListRowCount  = args->itemCount;
      gLastHijackListPageShape = HijackListPageShape::ListWithTitle;
      g2TextViewDisarm();
      DEBUG_G2F("[G2] page-swap: CREATE-list+text acked (%u list items + 1 title)",
                (unsigned)args->itemCount);
    }
  }

  if (!createOk) {
    const char* kindStr = args->kind == PSK_LIST      ? "list" :
                          args->kind == PSK_TEXT      ? "text" :
                          args->kind == PSK_MULTITEXT ? "multitext" :
                                                        "list+text";
    DEBUG_G2F("[G2] page-swap: CREATE-%s failed — hijack state "
              "may be inconsistent",
              kindStr);

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
        g2NoteCreateSuccess(*arm, /*isList*/ true, BLOCKS_WIDGET_ID);
        gLastHijackListRowCount  = fallN;
        gLastHijackListPageShape = HijackListPageShape::PureList;
        g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
        g2TextViewDisarm();
        DEBUG_G2F("[G2] page-swap: auto-recovery succeeded — back at root menu");
      } else {
        DEBUG_G2F("[G2] page-swap: auto-recovery FAILED — user must hit "
                  "Re-open hijack or reconnect");
      }
    }
  }

cleanup:
  clearOurShutdownStamp();
  if (args->kind == PSK_LIST) {
    freePageSwapItems(args->items, args->itemCount);
  } else if (args->kind == PSK_TEXT) {
    free(args->text);
    if (args->followUpText) free(args->followUpText);
  } else if (args->kind == PSK_MULTITEXT) {
    if (args->multiSpecs) {
      for (size_t i = 0; i < args->multiCount; i++) {
        // containerName + content were strdup'd in g2ShowMultiTextPage.
        free((void*)args->multiSpecs[i].containerName);
        free((void*)args->multiSpecs[i].content);
      }
      free(args->multiSpecs);
    }
  } else /* PSK_LIST_TEXT */ {
    freePageSwapItems(args->items, args->itemCount);
    free(args->titleName);
    free(args->titleContent);
  }
  delete args;
  pageSwapEnd();
  // No vTaskDelete — caller (pageSwapWorkerLoop) owns the task and
  // will return to xQueueReceive() to pick up the next job.
}

// ─────────────────────────────────────────────────────────────────────
// Persistent page-swap worker
// ─────────────────────────────────────────────────────────────────────
// One long-lived task at boot, fed by a queue. Each entry point
// (g2ShowListPage / g2ShowTextPage / etc.) deep-copies its inputs into
// a heap-allocated PageSwapArgs, then enqueues the pointer. The
// worker dequeues, runs pageSwapJobBody, frees, repeats. This
// replaces the previous "xTaskCreate per UI action" pattern that
// allocated a fresh 4 KB stack from internal DRAM on every
// navigation step — which was the root cause of the contiguous-block
// exhaustion that took down the post-camera-viewer page-swap.
//
// Concurrency is still serialised by gPageSwapActive (set by
// pageSwapBegin / cleared by pageSwapEnd). The queue depth is small
// (2) because pageSwapInFlight() rejects new requests while one is
// in flight; any backlog past depth=2 means something has wedged and
// the right thing is to log + drop, not to keep memory growing.

// Notification generation counter — each g2ShowNotification call bumps
// this; the lens applier's Notify case (and the legacy notifyClearTaskBody
// path it replaces in step 5) only clears if its captured gen matches.
// Lets a newer notification overwrite an older one without the older one's
// timer prematurely wiping the new content. Hoisted up here from its
// natural home below g2ShowNotification so the lens applier (next func)
// can read it; the original site near g2ShowNotification just uses it.
static volatile uint32_t gNotifyGen = 0;

// Step 3: queue now carries LensUiJob* instead of PageSwapArgs*. The legacy
// names ("gPageSwapQueue", "pageSwapWorkerLoop", task name "g2_page_swap_w")
// are retained to minimise churn against in-flight branch work; functionally
// this is the lens applier from G2_REFACTOR_PROPOSAL.md §5.2. Depth raised
// from 2 to 8 because future LensJobKind variants (Redraw/Toast/Notify/
// Custom — steps 4+) will share this queue alongside PageSwap.
static QueueHandle_t gPageSwapQueue   = nullptr;   // holds LensUiJob*
static TaskHandle_t  gPageSwapTaskH   = nullptr;
static const size_t  kPageSwapQueueDepth = 8;

static void pageSwapWorkerLoop(void* /*arg*/) {
  for (;;) {
    LensUiJob* job = nullptr;
    if (xQueueReceive(gPageSwapQueue, &job, portMAX_DELAY) != pdTRUE) {
      continue;  // spurious wake — try again
    }
    if (!job) continue;

    // Staleness check: a bumped menuGen between enqueue and dispatch means
    // the user navigated away. Step 3 logs only and still dispatches; step 4
    // will flip G2_LENS_GEN_GUARD once cmd-completion callbacks depend on
    // the drop semantics (and will add per-kind payload-free for the drop
    // path so we don't leak when the guard fires).
    const uint32_t liveGen = g2CurrentMenuGen();
    if (job->submitMenuGen != liveGen) {
      DEBUG_G2F("[lens.applier] gen mismatch: job=%u live=%u kind=%d (logging only)",
                (unsigned)job->submitMenuGen, (unsigned)liveGen, (int)job->kind);
    }

    switch (job->kind) {
      case LensJobKind::PageSwap:
        if (job->payload.swap) pageSwapJobBody(job->payload.swap);
        // pageSwapJobBody owns + frees the PageSwapArgs.
        break;

      case LensJobKind::Redraw: {
        // Redraw kind ALWAYS enforces the staleness guard — these are
        // command-completion redraws and must not snap the user back to a
        // stale view if they navigated away. (PageSwap kind only logs the
        // mismatch above for now — see G2_LENS_GEN_GUARD note in
        // G2_HijackCmd.h.)
        RedrawSpec* spec = job->payload.redraw;
        if (job->submitMenuGen != liveGen) {
          DEBUG_G2F("[lens.applier] Redraw dropped: stale gen "
                    "(job=%u live=%u, page=%u→%u, seq=%llu)",
                    (unsigned)job->submitMenuGen, (unsigned)liveGen,
                    (unsigned)job->targetPage, (unsigned)g2GetHijackPage(),
                    (unsigned long long)job->cmdSeq);
        } else if (spec && spec->render) {
          spec->render();
        }
        delete spec;
        break;
      }

      case LensJobKind::Notify: {
        // Notification auto-clear: fired by the esp_timer that replaced the
        // per-notification notifyClearTaskBody xTaskCreate. The spec carries
        // the gNotifyGen captured when the notification was shown — if a
        // newer notification has bumped gNotifyGen since then, drop the
        // clear (the new notification's content stays on screen).
        NotifySpec* spec = job->payload.notify;
        if (spec) {
          if (gNotifyGen == spec->gen) {
            DEBUG_G2F("[G2] Notification timer expired (gen=%u) — clearing display",
                      (unsigned)spec->gen);
            g2ClearDisplay();
          } else {
            DEBUG_G2F("[G2] Notification timer skipped (gen=%u, current=%u) — "
                      "newer notification replaced this one",
                      (unsigned)spec->gen, (unsigned)gNotifyGen);
          }
        }
        delete spec;
        break;
      }

      case LensJobKind::Custom: {
        // Run an arbitrary function on the lens applier context. Used for
        // hijack bootstrap (Group D) and any future one-shot work that
        // belongs off the BLE notify task. NOT gen-guarded — the run fn
        // must do its own state checks. spec is freed after the call.
        CustomSpec* spec = job->payload.custom;
        if (spec && spec->run) {
          spec->run();
        }
        delete spec;
        break;
      }

      // Step 6+ will populate this.
      case LensJobKind::Toast:
        DEBUG_G2F("[lens.applier] kind=%d not implemented — dropping job",
                  (int)job->kind);
        break;
    }
    delete job;
  }
}

// Idempotent — safe to call multiple times. The first call creates
// the queue + spawns the worker (the only `xTaskCreate` for the
// page-swap path that runs at boot, when DRAM headroom is largest);
// subsequent calls are no-ops.
static void pageSwapInit() {
  if (gPageSwapQueue) return;
  gPageSwapQueue = xQueueCreate(kPageSwapQueueDepth, sizeof(LensUiJob*));
  if (!gPageSwapQueue) {
    DEBUG_G2F("[G2] page-swap: queue create FAILED (depth=%u)",
              (unsigned)kPageSwapQueueDepth);
    return;
  }
  // Stack: 3584 words (~14 KB), trimmed from 4096 (~16 KB) 2026-06-07.
  // Measured HWM ~6.2 KB with glasses connected → ~7.8 KB headroom. This is
  // the ONLY xTaskCreate on the page-swap path now — all
  // navigation re-uses this worker via the queue.
  BaseType_t rc = xTaskCreate(pageSwapWorkerLoop, "g2_page_swap_w",
                              /*stack words*/ 3584, nullptr,
                              /*prio*/  tskIDLE_PRIORITY + 2,
                              &gPageSwapTaskH);
  if (rc != pdPASS) {
    DEBUG_G2F("[G2] page-swap: worker xTaskCreate FAILED (rc=%d)", (int)rc);
    vQueueDelete(gPageSwapQueue);
    gPageSwapQueue = nullptr;
    gPageSwapTaskH = nullptr;
  } else {
    DEBUG_G2F("[G2] page-swap: persistent worker started (queue depth=%u, msg=LensUiJob*)",
              (unsigned)kPageSwapQueueDepth);
  }
}

// Hand a job to the persistent worker. Caller must have already done
// pageSwapBegin() and prepared a fully-populated heap-allocated
// PageSwapArgs (with all string ownership transferred). On success,
// the worker takes ownership and frees the args after running. On
// failure, ownership stays with the caller — they must run the same
// cleanup the spawn-failure path used to do (free strings, delete
// args, pageSwapEnd, clear gTextView callbacks).
//
// Returns false if the queue is uninitialised (init wasn't called)
// or full (something has wedged the worker — the existing
// pageSwapInFlight guard normally prevents this from happening).
//
// Step 3: producers still pass PageSwapArgs*; this function wraps it in a
// LensUiJob and stamps the menuGen / targetPage cookie before pushing. The
// 5 producer sites in this file therefore stay unchanged.
static bool pageSwapEnqueue(PageSwapArgs* args) {
  if (!gPageSwapQueue) {
    DEBUG_G2F("[G2] page-swap enqueue: queue not initialised — call pageSwapInit() first");
    return false;
  }
  LensUiJob* job = new (std::nothrow) LensUiJob{};
  if (!job) {
    DEBUG_G2F("[G2] page-swap enqueue: LensUiJob alloc failed");
    return false;
  }
  job->kind          = LensJobKind::PageSwap;
  job->submitMenuGen = g2CurrentMenuGen();
  job->cmdSeq        = 0;                       // navigation-origin
  job->targetPage    = g2GetHijackPage();
  job->targetNetSub  = 0;                       // not captured for navigation-origin
  job->payload.swap  = args;

  // Short timeout: if the worker is wedged, fail fast and let the
  // caller log + clean up rather than blocking the BLE notify task.
  if (xQueueSend(gPageSwapQueue, &job, pdMS_TO_TICKS(50)) != pdTRUE) {
    DEBUG_G2F("[G2] page-swap enqueue: queue full (depth=%u, worker stuck?)",
              (unsigned)kPageSwapQueueDepth);
    delete job;   // give back to caller; caller still owns `args` per the contract above
    return false;
  }
  return true;
}

// Generic LensUiJob enqueue — for cmd-completion callbacks (step 4+) and
// non-PageSwap kinds. Caller-allocated, applier-freed on success.
bool g2EnqueueLensJob(LensUiJob* job) {
  if (!job) return false;
  if (!gPageSwapQueue) {
    DEBUG_G2F("[lens.applier] g2EnqueueLensJob: queue not initialised");
    return false;
  }
  if (xQueueSend(gPageSwapQueue, &job, pdMS_TO_TICKS(50)) != pdTRUE) {
    DEBUG_G2F("[lens.applier] g2EnqueueLensJob: queue full (kind=%d)", (int)job->kind);
    return false;   // caller still owns the job + its payload
  }
  return true;
}

// =============================================================================
// Tap dispatcher — runs handleHijackMenuTap() on its own worker task
// =============================================================================
// Why this exists:
//   handleDevEvent() is invoked from the Bluedroid notify-task stack —
//   that context already holds GATT/heap/scheduler spinlocks owned by
//   the BLE stack. Calling handleHijackMenuTap() inline from there
//   chains into invokePageFromMain → page handlers → g2ShowTextAsList,
//   which performs heap allocations (operator new, ps_alloc) and
//   xTaskCreate (the page-swap worker spin-up path). Any of those
//   internally take spinlocks that may already be held by this CPU,
//   triggering the spinlock_acquire (lock->count == 0) assert observed
//   on 2026-05-03 ~3 s after a Blocks-hijack menu tap.
//
// The fix:
//   handleDevEvent now enqueues the tap idx and returns immediately —
//   no allocations, no task spawns on the BLE callback stack. The
//   persistent worker below drains the queue from a normal FreeRTOS
//   task context where the allocator can take its locks safely.
//
// Lifecycle:
//   * Spawned at boot from initG2Client() alongside pageSwapInit() and
//     hijackFsmInit() — DRAM headroom is largest there.
//   * Persistent: never torn down. With G2 off / disconnected the queue
//     is empty and the worker just sleeps on the receive — costs only
//     its 4 KB stack + queue (8 × uint32_t = 32 B). Cheap insurance.
//   * Survives BT/G2 enable/disable cycles without re-init churn.

// Queue payload carries the tap idx plus the item name so the
// "Hijack tap: …" BROADCAST_PRINTF runs on the worker (off the small
// BTC stack), not on the BLE notify task.
//
// The TAP_IDX variant is the original use — hijack list-tap dispatch.
// The EXIT_FN variant carries a TEXT-view exit callback (function pointer
// only; no args). It exists for the same reason: the exit-handler call
// chain (file chooser redraw, page swap, FS access) overflows BTC_TASK's
// 4 KB stack when invoked synchronously from the BLE notify path.
enum TapDispatchKind : uint8_t {
  TAP_DISPATCH_IDX     = 0,  // hijack list-tap by index + iname
  TAP_DISPATCH_EXIT_FN = 1,  // TEXT-view exit handler (function pointer)
};

struct TapDispatchEntry {
  TapDispatchKind kind;
  uint32_t        idx;       // valid when kind == TAP_DISPATCH_IDX
  char            iname[32]; // valid when kind == TAP_DISPATCH_IDX
  void          (*exitFn)(); // valid when kind == TAP_DISPATCH_EXIT_FN
};

static QueueHandle_t gTapQueue       = nullptr;
static TaskHandle_t  gTapTaskHandle  = nullptr;
static const size_t  kTapQueueDepth  = 8;
static volatile uint32_t gTapDropped = 0;

static void tapDispatcherWorkerLoop(void* /*arg*/) {
  TapDispatchEntry e;
  uint32_t lastDroppedSeen = 0;
  for (;;) {
    if (xQueueReceive(gTapQueue, &e, portMAX_DELAY) != pdTRUE) {
      // Spurious wake — try again.
      continue;
    }
    switch (e.kind) {
      case TAP_DISPATCH_IDX: {
        // Emit the user-facing log here, off the BLE notify task. Uses
        // vsnprintf internally (~300-500 B stack). The worker stack is
        // sized for writeSettingsJson() + sensor toggles (not 4 KB).
        BROADCAST_PRINTF("[G2] Hijack tap: item %u (%s)",
                         (unsigned)e.idx, e.iname);
        // Run the heavy handler from this safe task context. handleHijackMenuTap
        // may allocate, enqueue page swaps, send BLE frames, etc. — all legal
        // here because we're not on the Bluedroid notify-task stack.
        handleHijackMenuTap(e.idx);
        break;
      }
      case TAP_DISPATCH_EXIT_FN: {
        // TEXT-view exit handler deferred off BTC_TASK. The exit fn
        // typically redraws the previous page (file chooser, menu, …) which
        // is a heavy call chain: G2HijackCtxGuard → FS access → page-swap
        // enqueue → BLE frame send. Producer cleared gTextViewActive
        // synchronously on BTC so further notify events skip the dead view.
        if (e.exitFn) e.exitFn();
        break;
      }
    }

    // Surface any drop counter increments since the last loop, so a stuck
    // worker / overflow burst is visible in the log instead of silent.
    const uint32_t dropped = __atomic_load_n(&gTapDropped, __ATOMIC_RELAXED);
    if (dropped != lastDroppedSeen) {
      DEBUG_G2F("[G2] tap-dispatch: queue full — %u tap(s) dropped (cumulative)",
                (unsigned)dropped);
      lastDroppedSeen = dropped;
    }
  }
}

// Idempotent. Creates the queue + spawns the persistent worker on first
// call; subsequent calls are no-ops. Same priority + stack as the
  // page-swap worker so taps and page swaps drain at parity (neither
  // preempts the other; both sit above idle and below the BLE stack).
  //
  // Stack budget: hijack handlers call setSetting → writeSettingsJson
  // (large JSON merge/serialize) and may start sensors — use ~20 KB.
  static void tapDispatcherInit() {
  if (gTapQueue) return;
  gTapQueue = xQueueCreate(kTapQueueDepth, sizeof(TapDispatchEntry));
  if (!gTapQueue) {
    DEBUG_G2F("[G2] tap-dispatch: queue create FAILED (depth=%u)",
              (unsigned)kTapQueueDepth);
    return;
  }
  // NOTE: xTaskCreate's third parameter is in WORDS (4 bytes), not bytes,
  // despite what older comments throughout this file claim. Historical
  // value here was 20480 — that was 80 KB, not 20 KB. The dispatcher
  // used to run deep-stack inline calls (writeSettingsJson, BLE init,
  // WiFi connect, etc.) so the oversize was defensible at the time.
  // Now that all those paths are migrated to cmd_exec_task, observed
  // peak usage is ~11.5 KB. 6400 words = 25 KB leaves ~13 KB headroom
  // and reclaims 55 KB of DRAM.
  BaseType_t rc = xTaskCreate(tapDispatcherWorkerLoop, "g2_tap_disp",
                              /*stack words*/ 6400, nullptr,
                              /*prio*/  tskIDLE_PRIORITY + 2,
                              &gTapTaskHandle);
  if (rc != pdPASS) {
    DEBUG_G2F("[G2] tap-dispatch: worker xTaskCreate FAILED (rc=%d)", (int)rc);
    vQueueDelete(gTapQueue);
    gTapQueue      = nullptr;
    gTapTaskHandle = nullptr;
  } else {
    DEBUG_G2F("[G2] tap-dispatch: persistent worker started (queue depth=%u)",
              (unsigned)kTapQueueDepth);
  }
}

// Producer. Called from handleDevEvent on the BLE notify-task stack.
// Non-blocking: zero-tick send, drop on full. Falling back to inline
// handleHijackMenuTap() would re-introduce the BTC stack overflow /
// reentrancy hazards the dispatcher exists to prevent, so the failure
// path drops + counts instead.
//
// Stack-discipline note: this function is intentionally tiny (one
// struct on stack + one xQueueSend). Anything that snprintfs or
// allocates belongs on the worker side, not here.
static bool tapDispatcherEnqueue(uint32_t idx, const char* iname) {
  if (!gTapQueue) {
    DEBUG_G2F("[G2] tap-dispatch: queue not initialised — tap idx=%u dropped",
              (unsigned)idx);
    return false;
  }
  TapDispatchEntry e{};
  e.kind = TAP_DISPATCH_IDX;
  e.idx  = idx;
  // Bounded copy — iname source is already decoded into a 32-byte
  // buffer in handleDevEvent, but use strncpy + null-term defensively
  // for any future caller. No vsnprintf here (would defeat the point).
  if (iname) {
    size_t n = 0;
    while (iname[n] && n + 1 < sizeof(e.iname)) { e.iname[n] = iname[n]; ++n; }
    e.iname[n] = '\0';
  } else {
    e.iname[0] = '\0';
  }
  if (xQueueSend(gTapQueue, &e, 0) != pdPASS) {
    __atomic_add_fetch(&gTapDropped, 1, __ATOMIC_RELAXED);
    return false;
  }
  return true;
}

// Producer for the TEXT-view exit handler. Same rationale as
// tapDispatcherEnqueue: the exit fn does heavy work (file chooser redraw,
// page swap, FS access) that overflows BTC_TASK's small stack when invoked
// inline from the BLE notify path. The BTC-side caller must clear
// gTextViewActive / gTextViewExitFn / gTextViewTapFn *before* this returns
// so further notify events ignore the now-closing view in the window
// between enqueue and worker execution.
//
// On queue-full or pre-init we drop (don't fall back to inline) — calling
// fn() from BTC_TASK is exactly the hazard this dispatcher exists to
// prevent. User can re-tap to retry. The drop counter surfaces in the
// worker log so a stuck dispatcher is visible.
static bool tapDispatcherEnqueueExit(void (*fn)()) {
  if (!fn) return false;
  if (!gTapQueue) {
    DEBUG_G2F("[G2] tap-dispatch: queue not initialised — exit-fn dropped");
    return false;
  }
  TapDispatchEntry e{};
  e.kind   = TAP_DISPATCH_EXIT_FN;
  e.exitFn = fn;
  if (xQueueSend(gTapQueue, &e, 0) != pdPASS) {
    __atomic_add_fetch(&gTapDropped, 1, __ATOMIC_RELAXED);
    return false;
  }
  return true;
}

// Public entry. Enqueues the swap and returns immediately. `items` must
// stay valid until the swap completes — page modules satisfy this by
// using file-static row buffers, with pageSwapInFlight() blocking
// concurrent taps that would rewrite the buffer mid-swap.
bool g2ShowListPage(const char* const* items, size_t itemCount,
                    const G2ContainerGeom& geom) {
  if (!items || itemCount == 0) return false;
  if (pageSwapInFlight()) {
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
  pageSwapBegin();
  if (!pageSwapEnqueue(args)) {
    DEBUG_G2F("[G2] g2ShowListPage: enqueue failed");
    freePageSwapItems(itemsCopy, itemCount);
    delete args;
    pageSwapEnd();
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
  if (pageSwapInFlight()) {
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
  char* copy = (char*)ps_alloc(len + 1, AllocPref::PreferPSRAM, "g2.showTextPage.copy");
  if (!copy) {
    DEBUG_G2F("[G2] g2ShowTextPage: copy alloc failed (%u B)", (unsigned)len);
    return false;
  }
  memcpy(copy, content, len + 1);

  PageSwapArgs* args = new PageSwapArgs;
  args->kind         = PSK_TEXT;
  args->items        = nullptr;
  args->itemCount    = 0;
  args->text         = copy;
  args->followUpText = nullptr;  // no follow-up REBUILD by default
  args->geom         = geom;

  // Arm the fallback exit + optional tap handler before flagging the
  // swap active so the dispatcher can't see a stale state even briefly.
  // The worker sets gTextViewActive after CREATE acks; we just stash
  // the function pointers up front.
  gTextViewExitFn = exitFn;
  gTextViewTapFn  = tapFn;

  pageSwapBegin();
  if (!pageSwapEnqueue(args)) {
    DEBUG_G2F("[G2] g2ShowTextPage: enqueue failed");
    free(copy);
    delete args;
    pageSwapEnd();
    gTextViewExitFn = nullptr;
    gTextViewTapFn  = nullptr;
    return false;
  }
  return true;
}

// Same shape as g2ShowTextPage but immediately follows the CREATE-text
// with a REBUILD-text carrying `content`. The CREATE seeds a tiny
// placeholder so it always fits in one fragment and the firmware
// happily acks; the REBUILD-text is the actual test of whether that
// transport scales past the CREATE-text 4-fragment ceiling. Used by
// Transport Tests' (R) brackets — answers "does in-place patch into
// an existing TextObject have a different reassembly ceiling than the
// initial CREATE?". Both strings are heap-copied. exitFn fires on
// dismiss the same way as g2ShowTextPage.
bool g2ShowTextPageRebuildProbe(const char* placeholder,
                                const char* content,
                                const G2ContainerGeom& geom,
                                void (*exitFn)()) {
  if (!placeholder || !content) return false;
  if (pageSwapInFlight()) {
    DEBUG_G2F("[G2] g2ShowTextPageRebuildProbe: swap in flight, dropping");
    return false;
  }
  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) {
    DEBUG_G2F("[G2] g2ShowTextPageRebuildProbe: no eligible temple");
    return false;
  }

  const size_t plen = strlen(placeholder);
  const size_t clen = strlen(content);
  char* placeholderCopy = (char*)ps_alloc(plen + 1, AllocPref::PreferPSRAM,
                                          "g2.txtRebuild.placeholder");
  char* contentCopy     = (char*)ps_alloc(clen + 1, AllocPref::PreferPSRAM,
                                          "g2.txtRebuild.content");
  if (!placeholderCopy || !contentCopy) {
    if (placeholderCopy) free(placeholderCopy);
    if (contentCopy)     free(contentCopy);
    DEBUG_G2F("[G2] g2ShowTextPageRebuildProbe: alloc failed (p=%u c=%u)",
              (unsigned)plen, (unsigned)clen);
    return false;
  }
  memcpy(placeholderCopy, placeholder, plen + 1);
  memcpy(contentCopy,     content,     clen + 1);

  PageSwapArgs* args = new PageSwapArgs;
  args->kind         = PSK_TEXT;
  args->items        = nullptr;
  args->itemCount    = 0;
  args->text         = placeholderCopy;
  args->followUpText = contentCopy;     // worker will REBUILD this in
  args->geom         = geom;

  gTextViewExitFn = exitFn;
  gTextViewTapFn  = nullptr;

  pageSwapBegin();
  if (!pageSwapEnqueue(args)) {
    DEBUG_G2F("[G2] g2ShowTextPageRebuildProbe: enqueue failed");
    free(placeholderCopy);
    free(contentCopy);
    delete args;
    pageSwapEnd();
    gTextViewExitFn = nullptr;
    return false;
  }
  return true;
}

// CreateStartUpPage with N TextObject children at independent geoms.
// Used by the Selection Patterns test bench to render side-by-side
// button affordances and similar compound layouts. `exitFn` is wired
// the same way as g2ShowTextPage — fires on DOUBLE_CLICK (or any
// SysEvent gesture if tapFn is null) so the user can dismiss back to
// whatever invoked them.
//
// Children are deep-copied (containerName + content) so the caller's
// buffers can be reused immediately. The page-swap worker frees the
// copies when done.
bool g2ShowMultiTextPage(const G2TextChildSpec* children, size_t childCount,
                         void (*exitFn)(), G2TapFn tapFn) {
  if (!children || childCount == 0) {
    DEBUG_G2F("[G2] g2ShowMultiTextPage: no children");
    return false;
  }
  if (pageSwapInFlight()) {
    DEBUG_G2F("[G2] g2ShowMultiTextPage: swap already in flight, dropping tap");
    return false;
  }
  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) {
    DEBUG_G2F("[G2] g2ShowMultiTextPage: no eligible temple");
    return false;
  }

  // Deep-copy the spec array + each spec's strings so the worker can
  // outlive the caller's buffers. Roll back partial allocations on
  // any single failure to avoid leaks. Names + contents go into
  // PSRAM via ps_alloc; the spec array itself is small enough that
  // either pool is fine (ps_alloc falls back to internal heap if
  // PSRAM is exhausted, which is also fine here).
  G2TextChildSpec* specCopy =
      (G2TextChildSpec*)ps_alloc(sizeof(G2TextChildSpec) * childCount,
                                  AllocPref::PreferPSRAM,
                                  "g2.showMultiText.specs");
  if (!specCopy) {
    DEBUG_G2F("[G2] g2ShowMultiTextPage: spec-array alloc failed (%u)",
              (unsigned)childCount);
    return false;
  }
  for (size_t i = 0; i < childCount; i++) {
    specCopy[i] = children[i];   // copy POD fields (geom, cid)
    specCopy[i].containerName = nullptr;
    specCopy[i].content       = nullptr;
    if (children[i].containerName) {
      const size_t n = strlen(children[i].containerName);
      char* dup = (char*)ps_alloc(n + 1, AllocPref::PreferPSRAM, "g2.showMultiText.name");
      if (!dup) goto fail_partial;
      memcpy(dup, children[i].containerName, n + 1);
      specCopy[i].containerName = dup;
    }
    if (children[i].content) {
      const size_t n = strlen(children[i].content);
      char* dup = (char*)ps_alloc(n + 1, AllocPref::PreferPSRAM, "g2.showMultiText.content");
      if (!dup) goto fail_partial;
      memcpy(dup, children[i].content, n + 1);
      specCopy[i].content = dup;
    }
  }

  {
    PageSwapArgs* args = new PageSwapArgs;
    args->kind       = PSK_MULTITEXT;
    args->items      = nullptr;
    args->itemCount  = 0;
    args->text       = nullptr;
    args->multiSpecs = specCopy;
    args->multiCount = childCount;
    args->geom       = G2_GEOM_LARGE;   // unused for PSK_MULTITEXT

    gTextViewExitFn = exitFn;
    gTextViewTapFn  = tapFn;

    pageSwapBegin();
    if (!pageSwapEnqueue(args)) {
      DEBUG_G2F("[G2] g2ShowMultiTextPage: enqueue failed");
      // Worker would have freed these — do it ourselves on the failure path.
      for (size_t i = 0; i < childCount; i++) {
        free((void*)specCopy[i].containerName);
        free((void*)specCopy[i].content);
      }
      free(specCopy);
      delete args;
      pageSwapEnd();
      gTextViewExitFn = nullptr;
      gTextViewTapFn  = nullptr;
      return false;
    }
    return true;
  }

fail_partial:
  // Roll back any successful allocations from the loop above.
  for (size_t j = 0; j < childCount; j++) {
    free((void*)specCopy[j].containerName);
    free((void*)specCopy[j].content);
  }
  free(specCopy);
  DEBUG_G2F("[G2] g2ShowMultiTextPage: child string copy failed");
  return false;
}

// Render a compound page: ListObject (selectable rows, native focus +
// scroll + click) + TextObject (title / header, non-interactive).
// Tap dispatch flows through the normal ListEvent CLICK path on row
// taps — exactly like a vanilla list page, but with a header label
// painted above. Use this for confirmation prompts ("Save changes?"
// + Yes/No), settings-style toggles with section labels, etc.
//
// `title` is the header TextObject; its containerName + content are
// deep-copied so caller buffers can be reused. `items` are the list
// rows (also deep-copied). `listGeom` is the on-lens rect for the
// list itself; the title carries its own geom via `title.geom`.
bool g2ShowMixedListText(const char* const* items, size_t itemCount,
                         const G2ContainerGeom& listGeom,
                         const G2TextChildSpec& title) {
  if (!items || itemCount == 0) {
    DEBUG_G2F("[G2] g2ShowMixedListText: no items");
    return false;
  }
  if (pageSwapInFlight()) {
    DEBUG_G2F("[G2] g2ShowMixedListText: swap already in flight");
    return false;
  }
  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) {
    DEBUG_G2F("[G2] g2ShowMixedListText: no eligible temple");
    return false;
  }

  char** itemsCopy = dupPageSwapItems(items, itemCount);
  if (!itemsCopy) {
    DEBUG_G2F("[G2] g2ShowMixedListText: item-copy alloc failed");
    return false;
  }
  // Deep-copy title strings — same lifetime model as items[].
  const char* nameSrc = title.containerName ? title.containerName : "";
  const char* contSrc = title.content       ? title.content       : "";
  size_t nameLen = strlen(nameSrc);
  size_t contLen = strlen(contSrc);
  char* nameDup = (char*)ps_alloc(nameLen + 1, AllocPref::PreferPSRAM, "g2.listText.titleName");
  char* contDup = (char*)ps_alloc(contLen + 1, AllocPref::PreferPSRAM, "g2.listText.titleContent");
  if (!nameDup || !contDup) {
    DEBUG_G2F("[G2] g2ShowMixedListText: title-string alloc failed");
    free(nameDup);
    free(contDup);
    freePageSwapItems(itemsCopy, itemCount);
    return false;
  }
  memcpy(nameDup, nameSrc, nameLen + 1);
  memcpy(contDup, contSrc, contLen + 1);

  PageSwapArgs* args = new PageSwapArgs;
  args->kind         = PSK_LIST_TEXT;
  args->items        = itemsCopy;
  args->itemCount    = itemCount;
  args->text         = nullptr;
  args->multiSpecs   = nullptr;
  args->multiCount   = 0;
  args->titleSpec    = title;
  args->titleSpec.containerName = nameDup;
  args->titleSpec.content       = contDup;
  args->titleName    = nameDup;
  args->titleContent = contDup;
  args->listGeom     = listGeom;
  args->geom         = listGeom;   // unused but populated for safety

  pageSwapBegin();
  if (!pageSwapEnqueue(args)) {
    DEBUG_G2F("[G2] g2ShowMixedListText: enqueue failed");
    free(nameDup);
    free(contDup);
    freePageSwapItems(itemsCopy, itemCount);
    delete args;
    pageSwapEnd();
    return false;
  }
  return true;
}

// =============================================================================
// Dual-pane REBUILD-text probe (replaces the retired dual-pane CREATE probe)
// =============================================================================
// The dual-pane CREATE experiment (test S, run 2026-04-30) proved that
// the firmware enforces single-container-per-widget: a second CREATE
// with a different ContainerName is silently dropped, and the firmware
// fires SYSTEM_EXIT ~2 s later to tear the widget down. ContainerName
// is widget-internal scope, not a peer-container multiplexer. See
// docs/G2_PROTOCOL.md for the wire trace.
//
// Open question this probe answers: in a single compound container
// hosting BOTH a TextObject and a ListObject (the test R shape, built
// by g2BuildCreateMixedListText), can we rebuild ONLY the TextObject
// child via Cmd=7 REBUILD_PAGE without touching the ListObject? If
// yes, we have the dual-pane Status UX: list of options on one side,
// detail pane that updates on selection change without resetting list
// focus. If no, the only path forward is full-compound REBUILD on
// every change and hope the firmware preserves list focus.
//
// Method:
//   1. Tear down any current container.
//   2. CREATE compound list+text via sendCreateMixedListTextAndWait
//      with outer name "app", title TextObject named "title" with
//      content "Initial: foo" at STATUS_BAR, list at the area below.
//   3. Hold 1.5 s so the user sees the initial render.
//   4. Build a REBUILD-text envelope with ContainerName="title" and
//      content "Updated: bar". Targets the TextObject CHILD by name.
//   5. Wait for RebuildResp.
//   6. Hold 4 s so the user can answer:
//        (a) did the title text change to "Updated: bar"?
//        (b) was the list focus / scroll position preserved?
//        (c) did anything else change (list went blank, etc.)?
//   7. Send Shutdown to clear the lens.
//
// Result string outcomes (caller logs):
//   * "OK rebuild acked — observe lens"   — RebuildResp res=6, look at glasses
//   * "FAIL pre-tearDown"                  — initial Shutdown failed
//   * "FAIL CREATE compound"               — compound CREATE rejected/timeout
//   * "FAIL REBUILD-text rejected/timeout" — child name not routed, or rejected
//
// Synchronous ack-waits: spawn from imgProbeWorker (separate task).
// G2_ASSERT_NOT_NOTIFY_TASK guard catches a misuse from the BLE notify
// task.
const char* g2ProbeRebuildTextChild() {
  static char result[96];
  result[0] = '\0';

  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) {
    snprintf(result, sizeof(result), "FAIL no temple");
    return result;
  }

  // Step 1: clean slate.
  if (!tearDownActiveContainer(*arm)) {
    snprintf(result, sizeof(result), "FAIL pre-tearDown");
    return result;
  }

  // Step 2: CREATE compound list+text. Outer container is the canonical
  // "app". The TextObject child is named "title" — the name we'll
  // target with REBUILD-text below. The ListObject child is the
  // implicit list inside g2BuildCreateMixedListText (its name is the
  // outer "app" by convention; only the title gets a distinct child
  // name in this builder). Geom layout: title at STATUS_BAR (top
  // 40 px), list below at {8, 56, 560, 224} (rest of canvas).
  static const char* kListItems[] = { "<- Back", "Yes", "No" };
  static const G2ContainerGeom kTitleGeom = G2_GEOM_STATUS_BAR;
  static const G2ContainerGeom kListGeom  = { 8, 56, 560, 224 };
  G2TextChildSpec title = {
    /*containerName*/ "title",
    /*content*/       "Initial: foo",
    /*containerId*/   99,
    /*geom*/          kTitleGeom,
    /*eventCapture*/  false
  };
  if (!sendCreateMixedListTextAndWait(*arm, kListItems, 3,
                                       kListGeom, title,
                                       BLOCKS_WIDGET_ID)) {
    snprintf(result, sizeof(result), "FAIL CREATE compound");
    return result;
  }
  DEBUG_G2F("[G2] rebuild-text-child: compound CREATE acked");

  // Step 3: hold so the user sees the initial state.
  vTaskDelay(pdMS_TO_TICKS(1500));

  // Step 4 + 5: REBUILD-text targeting child name="title". g2BuildRebuildText
  // writes the complete envelope; we ship via sendEnvelope. The arm/wait
  // pair uses gRebuildAckSem (not gCreateAckSem) — it's the Cmd=8
  // RebuildResp slot.
  if (!armRebuildSlot("REBUILD-text(child)")) {
    snprintf(result, sizeof(result), "FAIL arm rebuild slot");
    // Still try to clean the lens.
    uint8_t buf[32];
    size_t n = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN, 0,
                               buf, sizeof(buf));
    if (n > 0) sendEnvelope(*arm, buf, n);
    vTaskDelay(pdMS_TO_TICKS(200));
    return result;
  }
  uint8_t envBuf[256];
  size_t envLen = g2BuildRebuildText(allocSeq(), G2_MAGIC_REBUILD,
                                     /*containerName*/ "title",
                                     /*content*/       "Updated: bar",
                                     envBuf, sizeof(envBuf));
  if (envLen == 0) {
    snprintf(result, sizeof(result), "FAIL REBUILD-text build");
    uint8_t buf[32];
    size_t n = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN, 0,
                               buf, sizeof(buf));
    if (n > 0) sendEnvelope(*arm, buf, n);
    vTaskDelay(pdMS_TO_TICKS(200));
    return result;
  }
  if (!sendEnvelope(*arm, envBuf, envLen)) {
    snprintf(result, sizeof(result), "FAIL REBUILD-text send");
    uint8_t buf[32];
    size_t n = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN, 0,
                               buf, sizeof(buf));
    if (n > 0) sendEnvelope(*arm, buf, n);
    vTaskDelay(pdMS_TO_TICKS(200));
    return result;
  }
  DEBUG_G2F("[G2] rebuild-text-child: REBUILD-text(name=title) sent (env=%u B)",
            (unsigned)envLen);
  bool acked = waitRebuildAck("REBUILD-text(child)");
  if (!acked) {
    snprintf(result, sizeof(result), "FAIL REBUILD-text rejected/timeout");
    // Lens may or may not still be live — Shutdown to clean up
    // before the imgProbeWorker rebuilds the picker.
    uint8_t buf[32];
    size_t n = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN, 0,
                               buf, sizeof(buf));
    if (n > 0) sendEnvelope(*arm, buf, n);
    vTaskDelay(pdMS_TO_TICKS(200));
    return result;
  }
  DEBUG_G2F("[G2] rebuild-text-child: REBUILD acked — observe lens for 4 s");

  // Step 6: hold for the user to observe the lens. The result of THIS
  // probe is what they see, not anything we can read on the wire.
  vTaskDelay(pdMS_TO_TICKS(4000));

  // Step 7: clean up.
  uint8_t buf[32];
  size_t n = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN, 0,
                             buf, sizeof(buf));
  if (n > 0) sendEnvelope(*arm, buf, n);
  vTaskDelay(pdMS_TO_TICKS(200));

  snprintf(result, sizeof(result), "OK rebuild acked — observe lens");
  return result;
}

// Render a newline-separated text blob as a list page. First item is always
// the back affordance (returns to MAIN); subsequent items are the text
// lines. `backLabel` overrides the default "<- Back" — pass the destination
// name so the user knows where the tap goes. Mutates a private buffer in
// place to NUL-terminate each line. Returns true if the REBUILD frame went
// out.
bool g2ShowTextAsList(const char* text, const char* backLabel) {
  if (!text) return false;

  // Cap: list widget renders ~6-8 lines comfortably. Beyond that the user
  // would need to scroll — which works, but very long lists waste the row
  // buffer. 32 is plenty for the current Status/Sensors/System payloads.
  // Buffers in PSRAM (~2.2 KB) — only touched from g2ShowTextAsList +
  // BLE notify task, no DMA / ISR.
  static constexpr size_t kMaxRows = 32;
  EXT_RAM_BSS_ATTR static char        gRows[kMaxRows][64];
  EXT_RAM_BSS_ATTR static const char* gPtrs[kMaxRows];

  // Row 0: back affordance.
  const char* lbl = (backLabel && backLabel[0]) ? backLabel : "<- Back";
  strncpy(gRows[0], lbl, sizeof(gRows[0]));
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
// updates (row count fixed for the life of that live page).
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
// Per-session back-row label (null/empty → default "<- Back"). Captured at
// g2StartLiveListPage so the worker uses the same label across every tick
// without the caller needing to thread it down.
static const char*         gLivePageBackLabel = nullptr;

// AuthContext captured at g2StartLiveListPage time. The worker installs this
// into its own TLS slot via ExecIdentityGuard for each buildFn call so any
// guarded VFS access inside the page render runs as the user who paired
// the glasses (the worker task's default identity is ANON).
//
// No live page today reads the FS, so this is purely defensive — but a
// future "Storage" / "File-stats" live page would need it, and forgetting
// to wire it post-hoc is exactly the kind of regression the captured
// pattern prevents.
//
// Same lifecycle shape as gAutoLogOwnerCtx (System_Automation.cpp): set on
// open, used per tick, replaced on the next open. Never explicitly
// cleared — once the worker exits there's no consumer.
static AuthContext         gLivePageOwnerCtx;

// Render `text` into the same row-shape g2ShowTextAsList uses (back row
// prepended, newline-split). Caller owns the row storage. `backLabel`
// overrides the default "<- Back".
static size_t splitTextIntoRows(const char* text,
                                char rows[][64], const char** ptrs,
                                size_t maxRows,
                                const char* backLabel) {
  if (!text || maxRows == 0) return 0;
  const char* lbl = (backLabel && backLabel[0]) ? backLabel : "<- Back";
  strncpy(rows[0], lbl, sizeof(rows[0]));
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
  // stack than we want to chew, and PSRAM is plentiful. Worker runs in
  // regular task context (no DMA / ISR), so PSRAM-backed buffers are
  // safe — and ps_alloc falls back to internal heap if PSRAM is full.
  constexpr size_t kMaxRows = 32;
  char (*rows)[64]  = (char(*)[64])ps_alloc(sizeof(char[kMaxRows][64]),
                                            AllocPref::PreferPSRAM, "g2.livePage.rows");
  const char** ptrs = (const char**)ps_alloc(sizeof(const char*) * kMaxRows,
                                             AllocPref::PreferPSRAM, "g2.livePage.ptrs");
  char* textBuf     = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "g2.livePage.text");
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
    //
    // gLivePageIntervalMs == 0 means "manual refresh only" — same
    // semantics as the live-text worker. UINT32_MAX as the effective
    // interval keeps the inner stop-flag poll alive without ticking.
    const uint32_t waitMs = gLivePageIntervalMs > 0
                            ? gLivePageIntervalMs : UINT32_MAX;
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
    // Install the captured lens identity + G2 notification source for the
    // buildFn call so any guarded VFS access inside it reads as the paired
    // user, and any notify*() attributes to "G2 / <user>". Uses the LATCHED
    // owner ctx (captured at g2StartLivePage time, may differ from current
    // pairedByUser if a re-stamp happened mid-page); CommandIdentityScope
    // auto-derives NOTIF_SOURCE_G2 from the ctx's transport field.
    {
      CommandIdentityScope scope(gLivePageOwnerCtx);
      gLivePageBuildFn(textBuf, 2048);
    }
    size_t n = splitTextIntoRows(textBuf, rows, ptrs, kMaxRows,
                                 gLivePageBackLabel);
    if (n == 0) continue;

    G2Temple* arm = nullptr;
    if (gR.connected && !gR.pluginDead)      arm = &gR;
    else if (gL.connected && !gL.pluginDead) arm = &gL;
    if (!arm) {
      DEBUG_G2F("[G2] live-page: no eligible temple, ending worker");
      break;
    }

    // Skip the tick when the firmware has its own overlay foregrounded
    // (tap-and-hold "Exit?" yes/no dialog). Same rationale as the
    // live-text path: our list is in background, REBUILD wastes BLE.
    if (firmwareOverlayActive()) {
      DEBUG_G2F("[G2] live-page: tick skipped (firmware overlay foregrounded)");
      continue;
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
bool g2StartLiveListPage(G2LivePageBuildFn buildFn, uint32_t intervalMs,
                         const char* backLabel) {
  if (!buildFn) return false;
  // intervalMs == 0 is the "manual refresh only" sentinel — pass
  // through unchanged. Any nonzero value gets a 500 ms floor so a
  // misconfigured page can't hammer BLE with sub-frame refreshes.
  if (intervalMs > 0 && intervalMs < 500) intervalMs = 500;

  // If a live page is already running, stop it first.
  if (gLivePageActive) g2StopLiveListPage();

  if (!gLivePageRefreshSem) {
    gLivePageRefreshSem = xSemaphoreCreateBinary();
    if (!gLivePageRefreshSem) return false;
  }
  // Drain any stale signal from a prior session.
  xSemaphoreTake(gLivePageRefreshSem, 0);

  // Capture the back label for the worker. Caller is expected to keep
  // the pointer alive for the session — string literals from the page
  // module satisfy this trivially; dynamic labels would need a copy.
  gLivePageBackLabel = (backLabel && backLabel[0]) ? backLabel : nullptr;

  // Initial render via the standard path so the widget gets CREATEd
  // cleanly; this also seeds containerReady so subsequent REBUILDs
  // succeed. Build the text once here and ship via g2ShowTextAsList.
  char seed[2048];
  seed[0] = '\0';
  buildFn(seed, sizeof(seed));
  if (!g2ShowTextAsList(seed, gLivePageBackLabel)) {
    DEBUG_G2F("[G2] live-page: initial CREATE-list failed");
    return false;
  }

  gLivePageBuildFn    = buildFn;
  gLivePageIntervalMs = intervalMs;
  // Capture the lens identity NOW (at page-open time). The worker installs
  // this around every buildFn call so guarded VFS reads see the paired
  // user. See gLivePageOwnerCtx's declaration for rationale.
  gLivePageOwnerCtx   = g2HijackAuthContext();
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
// the SysEvent handler when DOUBLE_CLICK(3) src=2 fires, and exposed via
// g2KickLivePageRefresh() so other modules (text-entry) can request an
// immediate REBUILD after mutating buildFn-visible state.
static void livePageKickRefresh() {
  if (gLivePageActive && gLivePageRefreshSem) {
    xSemaphoreGive(gLivePageRefreshSem);
  }
}

void g2KickLivePageRefresh() { livePageKickRefresh(); }

// Forward-declared above handleEnvelope so the SysEvent handler can
// peek at the live-page state without a variable forward-declaration
// (which C++ doesn't permit cleanly for file-scope statics).
static bool livePageIsActive() {
  return gLivePageActive;
}

// Forward decl — defined alongside the image probes way below. The
// plain (non-probe) variant is what we actually want for live-text:
// drops the active list container without firing ImageProbeBegin into
// the FSM. The probe variant adds imageProbeBegin() on top.
static bool tearDownActiveContainer(G2Temple& arm);

// =============================================================================
// Live TEXT page worker — mirror of livePageWorker but for the TEXT
// container path (Cmd=7 REBUILD_PAGE on a single text widget) instead of
// REBUILD-list. Text widgets have no row selection / scroll state, so
// each REBUILD just snaps the new content in place — no visual cycling
// when the page content is longer than one screen and refreshes
// periodically. Pattern proven by the Q14 image probe.
//
// Lifecycle differs from live-list in two important ways:
//   1. There is no tappable back row — the text widget isn't a list and
//      can't render row affordances. Exit is via DOUBLE_CLICK (or any
//      tap, since gTextViewTapFn=nullptr) and routes through the
//      gTextViewExitFn slot, which we arm with liveTextExitToHijackMenu.
//   2. Returning to the hijack menu is a TEXT→LIST page-swap (not a
//      simple REBUILD), so the exit handler calls g2ShowListPage to
//      drive the standard SHUTDOWN+CREATE-list path.
// =============================================================================

static volatile bool       gLiveTextActive    = false;
static volatile bool       gLiveTextStopFlag  = false;
static G2LivePageBuildFn   gLiveTextBuildFn   = nullptr;
static bool              (*gLiveTextRenderFn)() = nullptr;
static volatile uint32_t   gLiveTextIntervalMs = 5000;
static SemaphoreHandle_t   gLiveTextRefreshSem = nullptr;
// AuthContext captured at g2StartLiveTextPage time. See gLivePageOwnerCtx
// for the full rationale — same lifecycle, same purpose, separate global
// because the live-text and live-list workers are independent and a user
// can't have both up at once but they can interleave via different page
// types.
static AuthContext         gLiveTextOwnerCtx;

static bool liveTextIsActive() {
  return gLiveTextActive;
}

static void liveTextKickRefresh() {
  if (gLiveTextActive && gLiveTextRefreshSem) {
    xSemaphoreGive(gLiveTextRefreshSem);
  }
}

void g2StopLiveTextPage();  // forward decl for the exit handler

// SysEvent exit handler — armed in gTextViewExitFn before the worker
// starts. The dispatcher calls this on DOUBLE_CLICK (or any tap, since
// we leave gTextViewTapFn null). Stops the worker and triggers a swap
// back to the main hijack menu.
static void liveTextExitToHijackMenu() {
  // Stop the worker first so its next tick can't fight the swap.
  g2StopLiveTextPage();
  // TEXT → LIST swap. g2ShowListPage spawns a page-swap worker that
  // does the SHUTDOWN+CREATE handshake required to switch widget
  // types, then sets containerIsList=false→true via the lens.
  g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
  const char* items[G2_PAGE_REGISTRY_MAX];
  size_t n = populateHijackMenuItems(items, G2_PAGE_REGISTRY_MAX);
  if (n == 0) {
    DEBUG_G2F("[G2] live-text: exit — no menu items to restore");
    return;
  }
  if (!g2ShowListPage(items, n)) {
    DEBUG_G2F("[G2] live-text: exit — restore-list swap dropped");
  }
}

// Compound-render geoms used by renderStatusCompound. Five text
// children + one list arranged so no two rectangles overlap:
//   back  — top-left strip, single-row tappable list with
//           "<- Main Menu". Tap fires ListEvent CLICK on
//           container='app' idx=0, routed through the existing
//           TEXT_VIEW dispatch (idx 0 → MAIN). Lives at the very top
//           so the back affordance is consistently positioned across
//           pages (Files / Settings / Tests follow the same convention
//           when rendered as plain lists). 40 px tall (matches the
//           STATUS_BAR preset) — the firmware's list widget renders
//           a selection-highlight strip slightly taller than its
//           bounding box, so a tighter 32 px back row painted into
//           body's territory on first render.
//   body  — bottom-left, sized to fit exactly 4 lines of text and
//           pinned so the box's bottom-left = the panel's
//           bottom-left at (8, 280). Firmware's TextObject aligns
//           text to the TOP of its bounding box, so the box must
//           equal the content size for the visible bottom to land
//           at the box's bottom edge — same trick the meter
//           (kStatusMeterGeom) uses (h=56 = exactly 2 lines × 28).
//           Per-line height in this widget is ~28 px (verified by
//           the meter), so 4 lines = 112 px. h=120 gives an 8-px
//           safety margin so the firmware's per-line padding can
//           round up without triggering a scroll bar. Pinned y=160
//           so y_bottom = 160 + 120 = 280, matching the meter's
//           bottom edge and the panel's 8 px bottom margin
//           convention. Critical companion: the body builder
//           (buildG2StatusSnapshot) MUST trim the trailing '\n'
//           on its content — without that, firmware reserves a
//           phantom 5th line slot and the visible text floats to
//           the top of the box with the bottom slot empty. Width
//           302 (vs. the meter's left edge at x=318) leaves an
//           8 px gap so they never overlap on the shared y-range
//           y=224..280. If body content ever changes, recompute h
//           and adjust y to keep y_bottom=280.
//   batt  — top-right corner row 1. Single-line G2% indicator.
//   r1    — top-right corner row 2. R1% indicator (placeholder until
//           ring telemetry is plumbed).
//   esp   — top-right corner row 3. ESP %/USB indicator. Reads
//           getBatteryPercentage()/isBatteryCharging() from
//           System_Battery.h. Replaces the legacy "Batt %.2fV %u%%"
//           line that used to live in the body block.
//   meter — bottom-right corner, hugging the canvas edge. Two-line
//           ASCII bar gauge for heap + PSRAM.
//
// y-axis layout (no overlap, gap-separated where the firmware needs it):
//   back  : y=[8,  48]
//   body  : y=[160, 280]   (pinned bottom-left; h=120 sized for
//                           exactly 4 lines × 28 px firmware
//                           line-height + small safety margin;
//                           builder strips trailing '\n' to kill
//                           the phantom 5th-line slot; visible
//                           text bottom = box bottom = panel
//                           bottom-left corner)
//   meter : y=[224, 280]   (shares y with body but disjoint x range)
//   batt  : y=[8,   40]    (separate x range from back, no overlap)
//   r1    : y=[42,  74]
//   esp   : y=[76, 108]
//
// x-axis layout:
//   back                    : x=[8,  454]
//   body                    : x=[8,  310]   (narrower than back so it
//                                            clears the meter at x=318
//                                            on the shared y range)
//   batt  / r1   / esp      : x=[458, 568]
//   meter                   : x=[318, 568]   (8 px gap from body's right edge)
//
// Meter width 250 px (was 168) — the firmware font is wider than ~10
// px per char, so the 17-char "H[####....]  39K" line was wrapping at
// the original 168-px width and pushing the PSRAM line off-screen.
static constexpr G2ContainerGeom kStatusBackGeom  = {   8,   8, 446,  40 };
static constexpr G2ContainerGeom kStatusBodyGeom  = {   8, 160, 302, 120 };
static constexpr G2ContainerGeom kStatusBattGeom  = { 458,   8, 110,  32 };
// R1 row directly under the G2 battery so the right column reads
// G2-then-R1 top-down. Content is just "R1" for now — ring telemetry
// (battery, link state) isn't wired up yet, but the slot exists so
// the visual layout matches the eventual two-row corner.
static constexpr G2ContainerGeom kStatusR1Geom    = { 458,  42, 110,  32 };
// ESP row directly below R1, third row of the right column. Reads the
// local SoC battery state via buildEspStatusBattery. WIDER than the G2/R1
// rows: left edge shifted left (x 458→408, w 110→160) while the right edge
// stays pinned at the canvas edge (568). Sized for the WIDEST value, which
// is "USB" — uppercase U/S/B are much wider than the "NN%" digits, so
// "ESP: USB" overflowed the narrower box, wrapped to a 2nd line, and the
// lens drew a scroll bar (the % values fit on one line, hence no bar there).
// Only this row's left edge moves; its y-band (76..108) is otherwise empty,
// so nothing collides. (Tighten x/w here if you want it less wide.)
static constexpr G2ContainerGeom kStatusEspGeom   = { 408,  76, 160,  32 };
static constexpr G2ContainerGeom kStatusMeterGeom = { 318, 224, 250,  56 };

// Custom live-render hook for kStatusPage. Keeps a fresh CREATE on the
// first call (when the worker has just torn the prior container down)
// and switches to per-child REBUILD-text on every subsequent tick. Two
// children: name="body" (full-canvas TextObject with the reordered
// snapshot) and name="batt" (top-right corner TextObject with the G2
// lens battery percent). Per-child REBUILD shape verified once by
// g2ProbeRebuildTextChild on firmware 2.2.0.24; this is its first
// recurring use, so flake under repeated REBUILDs surfaces here.
//
// "Skip REBUILD when content is byte-identical" cache: the firmware
// briefly repaints the widget on every REBUILD-text, so issuing a
// REBUILD with the same content the lens already shows produces a
// visible flicker for no UX gain. Caching the last-rendered strings
// and gating the REBUILD on a strcmp eliminates that — most ticks
// where none of body/batt/meter changed will short-circuit. Cleared
// implicitly on container tear-down via the !containerReady branch
// below (which does CREATE and resets the cache).
static char gStatusLastBattStr[64]   = {0};
static EXT_RAM_BSS_ATTR char gStatusLastBodyStr[2048];
static char gStatusLastMeterStr[128] = {0};
static char gStatusLastR1Str[32]     = {0};
static char gStatusLastEspStr[32]    = {0};

static bool renderStatusCompound() {
  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) {
    DEBUG_G2F("[G2] status-compound: no eligible temple");
    return false;
  }

  char battStr[32];
  buildG2StatusBattery(battStr, sizeof(battStr));

  char meterStr[128];
  buildG2StatusMeter(meterStr, sizeof(meterStr));

  // R1 row content. Mirrors the G2 battery format ("G2 NN%") so the
  // two corner rows read consistently. "---" fills the percentage slot
  // when we don't have a value, keeping column width stable so the
  // layout doesn't shift when telemetry comes online. Ring battery
  // isn't plumbed through yet — the placeholder is permanent until the
  // R1 protocol exposes a battery reading; swap to an int read at that
  // point. Disconnected state is communicated by this corner row alone
  // (no separate body line needed).
  char r1Str[32];
  snprintf(r1Str, sizeof(r1Str), "R1: --%%");

  // ESP/USB indicator — see buildEspStatusBattery for the three-state
  // logic. Same shape as battStr/r1Str so it threads through the
  // multi-text CREATE/REBUILD path identically.
  char espStr[32];
  buildEspStatusBattery(espStr, sizeof(espStr));

  // Body buffer — heap'd to keep the worker stack small. 2 KB matches
  // the single-text path's textBuf; Status currently produces well
  // under 1 KB so headroom is generous.
  char* bodyBuf = (char*)ps_alloc(2048, AllocPref::PreferPSRAM,
                                  "g2.statusCompound.body");
  if (!bodyBuf) {
    DEBUG_G2F("[G2] status-compound: body alloc failed");
    return false;
  }
  bodyBuf[0] = '\0';
  buildG2StatusSnapshot(bodyBuf, 2048);

  // Initial CREATE: container is freshly torn down (worker prologue or
  // prior tick's failure path). Build a 4-child compound (1 list +
  // 3 text) and ship it via the new mixed-list+multitext helper.
  //
  // Tap routing: the list child is named CONTAINER_NAME ("app") with
  // eventCapture=1, so a tap on its single row emits ListEvent CLICK
  // container='app' idx=0 — caught by the existing dispatcher
  // (handleHijackMenuTap) which, for a TEXT_VIEW page with idx=0,
  // routes back to MAIN. No new tap-handling code needed.
  //
  // Per-child REBUILD asymmetry experiment: we only REBUILD the 3
  // text children on subsequent ticks (see REBUILD branch below). The
  // hypothesis is that the firmware preserves the list child because
  // it's a different widget type than the rebuilt children — same
  // pattern that works for `g2BuildCreateMixedListText` + per-child
  // REBUILD-text. If this turns out wrong, the back row will
  // disappear after the first tick and we'll need to either rebuild
  // the list every tick too or fall back to a different layout.
  if (!g2LensGetState().containerReady) {
    static const char* kBackItems[] = { "<- Main Menu" };
    G2TextChildSpec textChildren[5] = {
      { /*containerName=*/ "body",  /*content=*/ bodyBuf,
        /*containerId=*/   2,       /*geom=*/    kStatusBodyGeom,
        /*eventCapture=*/  false },
      { /*containerName=*/ "batt",  /*content=*/ battStr,
        /*containerId=*/   3,       /*geom=*/    kStatusBattGeom,
        /*eventCapture=*/  false },
      { /*containerName=*/ "r1",    /*content=*/ r1Str,
        /*containerId=*/   5,       /*geom=*/    kStatusR1Geom,
        /*eventCapture=*/  false },
      { /*containerName=*/ "esp",   /*content=*/ espStr,
        /*containerId=*/   6,       /*geom=*/    kStatusEspGeom,
        /*eventCapture=*/  false },
      { /*containerName=*/ "meter", /*content=*/ meterStr,
        /*containerId=*/   4,       /*geom=*/    kStatusMeterGeom,
        /*eventCapture=*/  false },
    };
    bool ok = sendCreateMixedListMultiTextAndWait(
        *arm,
        kBackItems, 1, kStatusBackGeom,
        textChildren, 5,
        BLOCKS_WIDGET_ID);
    if (!ok) {
      free(bodyBuf);
      DEBUG_G2F("[G2] status-compound: CREATE-list+multitext failed "
                "(firmware may not honor 1-list + 3-text shape)");
      return false;
    }
    // Mirror container state into the lens FSM. isList=false because
    // the live-text worker treats "container became list" as a signal
    // to bail (see liveTextWorker), and we DO want it to keep ticking
    // REBUILDs on the text children. ListEvent dispatch from the back-
    // row tap doesn't gate on containerIsList — it routes purely on
    // the event's container name (CONTAINER_NAME = "app"), so the back
    // tap still reaches handleHijackMenuTap regardless of this flag.
    // Net: containerIsList tracks "is the active widget purely a list"
    // for the worker's benefit; the compound is mixed-mode and we pick
    // the value that keeps the worker alive.
    g2NoteCreateSuccess(*arm, /*isList*/ false, BLOCKS_WIDGET_ID);
    // Seed the "last rendered" cache with what we just sent so the
    // first REBUILD tick can short-circuit if nothing changed.
    strncpy(gStatusLastBattStr,  battStr,  sizeof(gStatusLastBattStr)  - 1);
    gStatusLastBattStr[sizeof(gStatusLastBattStr) - 1]   = '\0';
    strncpy(gStatusLastBodyStr,  bodyBuf,  sizeof(gStatusLastBodyStr)  - 1);
    gStatusLastBodyStr[sizeof(gStatusLastBodyStr) - 1]   = '\0';
    strncpy(gStatusLastMeterStr, meterStr, sizeof(gStatusLastMeterStr) - 1);
    gStatusLastMeterStr[sizeof(gStatusLastMeterStr) - 1] = '\0';
    strncpy(gStatusLastR1Str,    r1Str,    sizeof(gStatusLastR1Str)    - 1);
    gStatusLastR1Str[sizeof(gStatusLastR1Str) - 1]       = '\0';
    strncpy(gStatusLastEspStr,   espStr,   sizeof(gStatusLastEspStr)   - 1);
    gStatusLastEspStr[sizeof(gStatusLastEspStr) - 1]     = '\0';
    free(bodyBuf);
    DEBUG_G2F("[G2] status-compound: initial CREATE acked "
              "(1 list + 5 text children)");
    return true;
  }

  // Subsequent ticks: send a single multi-child REBUILD with ALL
  // four children (1 list + 3 text), gated by content diff so quiet
  // ticks don't trigger any repaint at all.
  //
  // The list MUST be re-sent every tick. Without it, multi-child
  // REBUILD blanks the unmentioned siblings — verified empirically
  // 2026-04-30: REBUILD-multitext on just the 3 text children made
  // the back-row list disappear after the first tick. The firmware's
  // multi-child REBUILD semantic appears to be "render exactly this
  // child set, blank everything else", not "patch these and leave
  // others alone." (Single-child REBUILD-text is the exception that
  // does leave siblings alone — see G2_PROTOCOL.md.)
  const bool bodyChanged  = strcmp(bodyBuf,  gStatusLastBodyStr)  != 0;
  const bool battChanged  = strcmp(battStr,  gStatusLastBattStr)  != 0;
  const bool meterChanged = strcmp(meterStr, gStatusLastMeterStr) != 0;
  const bool r1Changed    = strcmp(r1Str,    gStatusLastR1Str)    != 0;
  const bool espChanged   = strcmp(espStr,   gStatusLastEspStr)   != 0;

  if (!bodyChanged && !battChanged && !meterChanged && !r1Changed && !espChanged) {
    free(bodyBuf);
    DEBUG_G2F("[G2] status-compound: tick (no changes — skipped REBUILD)");
    return true;
  }

  static const char* kBackItems[] = { "<- Main Menu" };
  G2TextChildSpec textChildren[5] = {
    { /*containerName=*/ "body",  /*content=*/ bodyBuf,
      /*containerId=*/   2,       /*geom=*/    kStatusBodyGeom,
      /*eventCapture=*/  false },
    { /*containerName=*/ "batt",  /*content=*/ battStr,
      /*containerId=*/   3,       /*geom=*/    kStatusBattGeom,
      /*eventCapture=*/  false },
    { /*containerName=*/ "r1",    /*content=*/ r1Str,
      /*containerId=*/   5,       /*geom=*/    kStatusR1Geom,
      /*eventCapture=*/  false },
    { /*containerName=*/ "esp",   /*content=*/ espStr,
      /*containerId=*/   6,       /*geom=*/    kStatusEspGeom,
      /*eventCapture=*/  false },
    { /*containerName=*/ "meter", /*content=*/ meterStr,
      /*containerId=*/   4,       /*geom=*/    kStatusMeterGeom,
      /*eventCapture=*/  false },
  };
  bool ok = sendRebuildMixedListMultiTextAndWait(
      *arm,
      kBackItems, 1, kStatusBackGeom,
      textChildren, 5);
  if (!ok) {
    free(bodyBuf);
    DEBUG_G2F("[G2] status-compound: REBUILD-list+multitext failed — "
              "firmware may not honor mixed-list REBUILD shape");
    return false;
  }
  // Update all cache entries on success — even if only one of the
  // five children changed, the wire-shipped strings now match what's
  // on the lens, so caching the current values is still correct.
  strncpy(gStatusLastBodyStr,  bodyBuf,  sizeof(gStatusLastBodyStr)  - 1);
  gStatusLastBodyStr[sizeof(gStatusLastBodyStr) - 1]   = '\0';
  strncpy(gStatusLastBattStr,  battStr,  sizeof(gStatusLastBattStr)  - 1);
  gStatusLastBattStr[sizeof(gStatusLastBattStr) - 1]   = '\0';
  strncpy(gStatusLastMeterStr, meterStr, sizeof(gStatusLastMeterStr) - 1);
  gStatusLastMeterStr[sizeof(gStatusLastMeterStr) - 1] = '\0';
  strncpy(gStatusLastR1Str,    r1Str,    sizeof(gStatusLastR1Str)    - 1);
  gStatusLastR1Str[sizeof(gStatusLastR1Str) - 1]       = '\0';
  strncpy(gStatusLastEspStr,   espStr,   sizeof(gStatusLastEspStr)   - 1);
  gStatusLastEspStr[sizeof(gStatusLastEspStr) - 1]     = '\0';
  free(bodyBuf);

  DEBUG_G2F("[G2] status-compound: tick (multi-rebuilt: "
            "body=%s batt=%s r1=%s esp=%s meter=%s)",
            bodyChanged  ? "changed" : "same",
            battChanged  ? "changed" : "same",
            r1Changed    ? "changed" : "same",
            espChanged   ? "changed" : "same",
            meterChanged ? "changed" : "same");
  return true;
}

static void liveTextWorker(void* /*arg*/) {
  // textBuf only used when the page goes through buildFn+g2ShowText.
  // renderFn-mode pages own their buffers internally (see
  // renderStatusCompound) so we skip the alloc.
  const bool useRenderFn = (gLiveTextRenderFn != nullptr);
  char* textBuf = nullptr;
  if (!useRenderFn) {
    textBuf = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "g2.liveText.buf");
    if (!textBuf) {
      DEBUG_G2F("[G2] live-text: worker heap alloc failed");
      gLiveTextActive   = false;
      gLiveTextBuildFn  = nullptr;
      gLiveTextRenderFn = nullptr;
      vTaskDelete(nullptr);
      return;
    }
  }

  // ── Initial tearDown + CREATE — runs HERE, on the worker task,
  // not on the BLE notify task that called g2StartLiveTextPage.
  // tearDownActiveContainer's vTaskDelay and sendCreateAndWait's
  // 1.5 s semaphore wait would otherwise stall the notify task,
  // and since the CreateResp is delivered BY that same task,
  // the wait would always time out — Shutdown gets sent, no CREATE
  // follows, lens shows "Connection lost". Doing the handshake on
  // this worker keeps the notify task free to deliver the response.
  G2Temple* initArm = nullptr;
  if (gR.connected && !gR.pluginDead)      initArm = &gR;
  else if (gL.connected && !gL.pluginDead) initArm = &gL;
  if (!initArm) {
    DEBUG_G2F("[G2] live-text: worker — no eligible temple at startup");
    if (textBuf) free(textBuf);
    gLiveTextActive   = false;
    gLiveTextBuildFn  = nullptr;
    gLiveTextRenderFn = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  // Drop the existing list container so the upcoming render call
  // sees containerReady=false and CREATEs a fresh widget rather than
  // auto-routing to g2ShowTextAsList (which would perpetuate the list
  // and we'd cycle on every REBUILD). renderFn-mode pages also rely on
  // this clean slate to take the CREATE branch on first call.
  if (!tearDownActiveContainer(*initArm)) {
    DEBUG_G2F("[G2] live-text: worker — pre-loop SHUTDOWN failed");
    if (textBuf) free(textBuf);
    gLiveTextActive   = false;
    gLiveTextBuildFn  = nullptr;
    gLiveTextRenderFn = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  // Initial render. renderFn-mode pages own the entire CREATE+REBUILD
  // protocol internally (see renderStatusCompound for the canonical
  // example); buildFn-mode pages emit a single-TEXT CREATE via the
  // existing g2ShowText auto-routing.
  // Install the captured lens identity + G2 notification source for the
  // render call. Both buildFn and renderFn paths get them — either may
  // touch guarded VFS or fire notifications, and we want them to see the
  // paired user instead of this worker task's default ANON identity, with
  // notifications attributed to "G2 / <user>" via the auto-derive in
  // CommandIdentityScope. See gLiveTextOwnerCtx for design.
  bool initOk;
  {
    CommandIdentityScope scope(gLiveTextOwnerCtx);
    if (useRenderFn) {
      initOk = gLiveTextRenderFn();
    } else {
      textBuf[0] = '\0';
      if (gLiveTextBuildFn) gLiveTextBuildFn(textBuf, 2048);
      initOk = g2ShowText(textBuf);
    }
  }
  if (!initOk) {
    DEBUG_G2F("[G2] live-text: worker — initial render failed");
    if (textBuf) free(textBuf);
    gLiveTextActive   = false;
    gLiveTextBuildFn  = nullptr;
    gLiveTextRenderFn = nullptr;
    // Don't try to re-show the hijack menu from here — the BLE write
    // path is in an unknown state and we'd just compound the
    // failure. Next user tap on Blocks will start a fresh hijack.
    vTaskDelete(nullptr);
    return;
  }

  // Arm the SysEvent text-view slot AFTER the CREATE acks so a
  // synthetic USER_ACTIVITY between Shutdown and CREATE can't trip
  // our exit handler before the widget exists. Activated timestamp
  // gates the post-CREATE settle pulse the firmware emits ~150 ms
  // after CREATE — same logic as g2ShowTextPage.
  gTextViewActivatedMs  = millis();
  gTextViewExitFn       = liveTextExitToHijackMenu;
  gTextViewTapFn        = nullptr;
  gTextViewActive       = true;

  while (!gLiveTextStopFlag) {
    // Same wait-with-poll-on-stop pattern as livePageWorker so stop
    // signals don't have to wait out the full interval.
    //
    // gLiveTextIntervalMs == 0 means "manual refresh only" — wait
    // until the user double-taps (which gives the refresh sem) or
    // the page is stopped. Use UINT32_MAX as the effective interval
    // so the millis() comparison never trips; the inner loop still
    // polls the stop flag every 100 ms so g2StopLiveTextPage stays
    // responsive.
    const uint32_t waitMs  = gLiveTextIntervalMs > 0
                             ? gLiveTextIntervalMs : UINT32_MAX;
    const uint32_t startMs = millis();
    while ((millis() - startMs) < waitMs && !gLiveTextStopFlag) {
      if (xSemaphoreTake(gLiveTextRefreshSem, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!gLiveTextStopFlag) {
          DEBUG_G2F("[G2] live-text: refresh kicked");
        }
        break;
      }
    }
    if (gLiveTextStopFlag) break;

    if (!useRenderFn && !gLiveTextBuildFn) continue;

    G2Temple* arm = nullptr;
    if (gR.connected && !gR.pluginDead)      arm = &gR;
    else if (gL.connected && !gL.pluginDead) arm = &gL;
    if (!arm) {
      DEBUG_G2F("[G2] live-text: no eligible temple, ending worker");
      break;
    }
    // If something else swapped us back to a list (e.g. user tapped a
    // hijack-menu row that opens a sub-page while we're still spinning
    // up), bail before the next tick. g2ShowText would otherwise
    // auto-route to g2ShowTextAsList and start cycling, defeating the
    // whole point of this worker. renderFn-mode pages bail here too —
    // their CREATE branch wouldn't run again without a fresh tearDown.
    if (g2LensGetState().containerIsList) {
      DEBUG_G2F("[G2] live-text: container became list — ending worker");
      break;
    }

    // Skip the tick when the firmware has its own overlay foregrounded
    // (tap-and-hold "Exit?" yes/no dialog). Our widget is in background
    // — pushing REBUILD while it's hidden wastes BLE bandwidth and
    // contests the radio against the dialog's own latency. We loop back
    // to the wait so the next tick fires once the user dismisses the
    // dialog and the overlay flag clears.
    if (firmwareOverlayActive()) {
      DEBUG_G2F("[G2] live-text: tick skipped (firmware overlay foregrounded)");
      continue;
    }

    // Same composed identity+notif install as the initial render path
    // (CommandIdentityScope auto-derives NOTIF_SOURCE_G2 from the latched
    // ctx). Per-tick wrapping is unconditional even though no current page
    // reads the FS or fires notifications — keeps the property "future page
    // that does FS work or fires notifications just works without
    // remembering this plumbing."
    bool tickOk;
    {
      CommandIdentityScope scope(gLiveTextOwnerCtx);
      if (useRenderFn) {
        tickOk = gLiveTextRenderFn();
      } else {
        textBuf[0] = '\0';
        gLiveTextBuildFn(textBuf, 2048);
        tickOk = g2ShowText(textBuf);
      }
    }
    if (tickOk) {
      DEBUG_G2F("[G2] live-text: tick rendered");
    } else {
      DEBUG_G2F("[G2] live-text: tick failed — aborting worker");
      break;
    }
  }

  if (textBuf) free(textBuf);
  gLiveTextActive   = false;
  gLiveTextStopFlag = false;
  gLiveTextBuildFn  = nullptr;
  gLiveTextRenderFn = nullptr;
  // Release the SysEvent text-view slot so the next page hijack starts
  // with a clean state. (If the exit handler fired, this is a no-op
  // because the SysEvent dispatch already cleared these.)
  if (gTextViewExitFn == liveTextExitToHijackMenu) {
    gTextViewActive = false;
    gTextViewExitFn = nullptr;
    gTextViewTapFn  = nullptr;
  }
  DEBUG_G2F("[G2] live-text: worker exited");
  vTaskDelete(nullptr);
}

bool g2StartLiveTextPage(G2LivePageBuildFn buildFn, uint32_t intervalMs,
                         bool (*renderFn)()) {
  // Need at least one render path. buildFn is the registry's required
  // field, so usually present even when renderFn is set; renderFn is
  // optional and overrides buildFn when supplied.
  if (!buildFn && !renderFn) return false;
  // intervalMs == 0 is the "manual refresh only" sentinel — pass
  // through unchanged. See g2StartLiveListPage for the rationale.
  if (intervalMs > 0 && intervalMs < 500) intervalMs = 500;

  // Stop any active live page (text OR list) before starting a new one
  // so we don't have two workers fighting over the active container.
  if (gLivePageActive) g2StopLiveListPage();
  if (gLiveTextActive) g2StopLiveTextPage();

  if (!gLiveTextRefreshSem) {
    gLiveTextRefreshSem = xSemaphoreCreateBinary();
    if (!gLiveTextRefreshSem) return false;
  }
  xSemaphoreTake(gLiveTextRefreshSem, 0);

  // Sanity: at least one temple must be live, otherwise no point
  // spawning the worker. Final temple selection happens inside the
  // worker right before the initial CREATE.
  if (!((gR.connected && !gR.pluginDead) ||
        (gL.connected && !gL.pluginDead))) {
    DEBUG_G2F("[G2] live-text: no eligible temple");
    return false;
  }

  // Stash worker config BEFORE xTaskCreate so the worker can read it
  // immediately. The synchronous tearDown + initial CREATE handshake
  // runs INSIDE liveTextWorker — see the prologue there for why we
  // can't do it here. Common caller is handleHijackMenuTap, which
  // runs on the BLE notify task; doing sendCreateAndWait there would
  // deadlock because the CreateResp is delivered by that same task.
  gLiveTextBuildFn    = buildFn;
  gLiveTextRenderFn   = renderFn;
  gLiveTextIntervalMs = intervalMs;
  // Capture the lens identity at page-open time. The worker installs this
  // around every render call so guarded VFS reads see the paired user.
  // Mirrors gLivePageOwnerCtx — see its declaration for design rationale.
  gLiveTextOwnerCtx   = g2HijackAuthContext();
  gLiveTextStopFlag   = false;
  gLiveTextActive     = true;
  // gTextView* slot is armed by the worker AFTER the initial CREATE
  // acks — leaving it disarmed here means a stray SysEvent during
  // the Shutdown→CREATE window can't trip our exit handler before
  // the widget exists.

  // 4 KB stack — same as livePageWorker. Heap-allocated text buffer
  // keeps the on-stack pressure to local frames + g2ShowText's pb
  // builder (also heap-backed via ps_alloc).
  if (xTaskCreate(liveTextWorker, "g2_live_text", 4096, nullptr,
                  /*prio*/ 5, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] live-text: xTaskCreate failed");
    gLiveTextActive   = false;
    gLiveTextBuildFn  = nullptr;
    gLiveTextRenderFn = nullptr;
    return false;
  }
  DEBUG_G2F("[G2] live-text: started (interval=%u ms, mode=%s)",
            (unsigned)intervalMs, renderFn ? "renderFn" : "buildFn");
  return true;
}

void g2StopLiveTextPage() {
  if (!gLiveTextActive) return;
  gLiveTextStopFlag = true;
  if (gLiveTextRefreshSem) xSemaphoreGive(gLiveTextRefreshSem);
  // Worker self-deletes; spin briefly so cleanup ordering is observable
  // to the next caller. Mirrors g2StopLiveListPage.
  for (int i = 0; i < 30 && gLiveTextActive; i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
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
// G2 lens state — single source of truth (see G2_Glasses.h G2LensState)
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

// ─── Phase 5: FSM is the authoritative writer of lens-mirror state ──
// The public g2LensSet*/g2LensClearContainer functions just dispatch to
// the FSM. The worker, when it processes the event, calls the
// g2LensApply* mutators below to actually update gLens. This makes the
// FSM the single writer; legacy direct mutations of gLens.container* /
// gLens.hijack* fields no longer exist anywhere outside the apply
// helpers.

void g2LensApplyHijackActive(bool active) {
  gLens.hijackActive = active;
  if (active) gLens.hijackStartedMs = millis();
  else        gLens.hijackStartedMs = 0;
  DEBUG_G2F("[G2] lens.hijackActive → %d", active ? 1 : 0);

  // When the hijack ends from ANY cause (safety-timeout, BLE
  // disconnect, ShutdownPage from a peer-side dismiss, an internal
  // FSM error transition, …), signal in-flight image probes /
  // camera stream to abort. Without this, only the user-tap paths
  // (CLICK/DOUBLE_CLICK on sid=0xE0) flip gImgProbeAbort, so the
  // camera-stream worker happily keeps capturing → encoding →
  // pushing for ~10 s after the glasses have already left lens-page
  // mode — wasting BLE TX and advancing magic/sequence numbers into
  // the next hijack attempt's window. Probe entry points reset
  // gImgProbeAbort = false, so leaving it set here is safe.
  if (!active && !gImgProbeAbort) {
    gImgProbeAbort = true;
    DEBUG_G2F("[G2] gImgProbeAbort=1 (hijack ended; aborting in-flight probe)");
  }
}

void g2LensApplyContainer(bool ready, bool isList, uint32_t widgetId) {
  gLens.containerReady    = ready;
  gLens.containerIsList   = isList;
  gLens.containerWidgetId = widgetId;
  if (ready) {
    DEBUG_G2F("[G2] lens.container ready=1 isList=%d wid=%u",
              isList ? 1 : 0, (unsigned)widgetId);
  } else {
    DEBUG_G2F("[G2] lens.container cleared");
  }
}

void g2LensSetHijackActive(bool active) {
  // Optimistic read of gLens — purely a debouncer to avoid spamming the
  // FSM with redundant transitions. A torn read just means we'd post a
  // duplicate event, which the FSM logs and drops (HijackEnter from
  // Hijacked is illegal/no-op).
  if (gLens.hijackActive == active) return;
  hijackFsmDispatch(active ? HijackEvent::HijackEnter
                           : HijackEvent::HijackExit,
                    "g2LensSetHijackActive");
}

void g2LensSetContainer(bool ready, bool isList, uint32_t widgetId) {
  // ready=false used to be a "silent setter" with no FSM dispatch; the
  // one remaining caller now uses g2LensClearContainer() directly. Treat
  // any stray ready=false here as a clear, for safety.
  if (!ready) {
    g2LensClearContainer();
    return;
  }
  HijackEventPayload payload;
  payload.isList   = isList;
  payload.widgetId = widgetId;
  hijackFsmDispatch(HijackEvent::ContainerCreated, "g2LensSetContainer",
                    payload);
}

void g2LensClearContainer() {
  // Don't clear hijackPage — even if the container went away, the page
  // tracker reflects "where the user was" and the next CREATE will reset
  // it to MAIN explicitly. (gLens.hijackPage is not part of the
  // container-mirror state and is left to its own setter.)
  hijackFsmDispatch(HijackEvent::ContainerCleared, "g2LensClearContainer");
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
  // Page transition invalidates any in-flight hijack command's view snapshot.
  // See G2_HijackCmd.h staleness contract; safe even when no cmd is in flight.
  g2BumpMenuGen();
  DEBUG_G2F("[G2] lens.hijackPage → %u", (unsigned)p);
}

// gNotifyGen is hoisted to the lens applier section above so the Notify-job
// dispatch can read it without a forward-declared accessor. The original
// declaration site is intentionally empty here — see the comment above
// gNotifyGen near pageSwapWorkerLoop.

// Step 5: replaces the per-notification `xTaskCreate(notifyClearTaskBody …)`
// pattern with a single persistent esp_timer that gets restarted on each
// new notification. Timer fires on esp_timer's task (cannot safely call
// g2ClearDisplay there — BLE + page-swap state), so the callback enqueues
// a LensJobKind::Notify job carrying the captured gen. The lens applier's
// Notify case (above) compares gen against gNotifyGen and clears the
// display if it still matches. Same staleness semantics as the legacy
// task; eliminates one of the 17 one-shot xTaskCreate patterns called out
// in G2_REFACTOR_PROPOSAL.md §6.6.
static esp_timer_handle_t gNotifyClearTimer    = nullptr;
static volatile uint32_t  gNotifyClearTimerGen = 0;   // gen baked into the running timer

static void notifyClearTimerCb(void* /*arg*/) {
  const uint32_t myGen = gNotifyClearTimerGen;
  // Build + enqueue a Notify job. Lens applier owns the gen-vs-gNotifyGen
  // check + the actual g2ClearDisplay call so we don't touch BLE state
  // from the esp_timer task (which has a small stack and is shared with
  // every other esp_timer in the firmware).
  NotifySpec* spec = new (std::nothrow) NotifySpec{};
  if (!spec) return;
  spec->gen = myGen;

  LensUiJob* job = new (std::nothrow) LensUiJob{};
  if (!job) {
    delete spec;
    return;
  }
  job->kind           = LensJobKind::Notify;
  job->submitMenuGen  = g2CurrentMenuGen();   // not used by Notify path; harmless
  job->cmdSeq         = 0;
  job->targetPage     = g2GetHijackPage();
  job->targetNetSub   = 0;
  job->payload.notify = spec;

  if (!g2EnqueueLensJob(job)) {
    DEBUG_G2F("[G2] notify-clear: lens job enqueue FAILED — display won't auto-clear");
    delete spec;
    delete job;
  }
}

static bool ensureNotifyClearTimer() {
  if (gNotifyClearTimer) return true;
  esp_timer_create_args_t args = {};
  args.callback = &notifyClearTimerCb;
  args.arg      = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name     = "g2_notify_clear";
  args.skip_unhandled_events = false;
  esp_err_t err = esp_timer_create(&args, &gNotifyClearTimer);
  if (err != ESP_OK) {
    DEBUG_G2F("[G2] notify-clear: esp_timer_create failed err=%d", (int)err);
    gNotifyClearTimer = nullptr;
    return false;
  }
  return true;
}

bool g2ShowNotification(const char* text, uint32_t durationMs) {
  if (!text) return false;
  // Bump the generation BEFORE showing so any stale Notify job already in
  // the lens applier queue sees gNotifyGen != its own and no-ops.
  const uint32_t myGen = ++gNotifyGen;
  if (!g2ShowText(text)) {
    DEBUG_G2F("[G2] Notification show failed (text=%u B)",
              (unsigned)strlen(text));
    return false;
  }
  if (durationMs == 0) {
    DEBUG_G2F("[G2] Notification shown (gen=%u, no auto-clear)",
              (unsigned)myGen);
    // Make sure no leftover timer fires and clears this no-auto-clear
    // notification (the prior notification's timer might still be armed).
    if (gNotifyClearTimer) esp_timer_stop(gNotifyClearTimer);
    return true;
  }
  if (!ensureNotifyClearTimer()) {
    DEBUG_G2F("[G2] Notification: timer init failed — display won't auto-clear");
    return true;  // show succeeded; clear won't
  }
  // Stop any prior pending fire (idempotent if not running). Safe even if
  // a fire is mid-callback; esp_timer_stop blocks until the cb returns.
  esp_timer_stop(gNotifyClearTimer);
  gNotifyClearTimerGen = myGen;
  esp_err_t err = esp_timer_start_once(gNotifyClearTimer,
                                       (uint64_t)durationMs * 1000ULL);
  if (err != ESP_OK) {
    DEBUG_G2F("[G2] Notification: esp_timer_start_once failed err=%d — won't auto-clear",
              (int)err);
    return true;
  }
  DEBUG_G2F("[G2] Notification shown (gen=%u, clear-in=%u ms, esp_timer)",
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
   .kv("h",  g2FsmHijackActive())
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

  // Pair-intent stamp from the CALLING task's identity. `openg2` from a
  // logged-in CLI session IS the natural pairing gesture — the user is
  // saying "these are my glasses." Stamp pairedByUser NOW, before the
  // connect is dispatched to the g2_ble_connect worker. The worker runs
  // as ANON; if we relied on `bleSavePeerMac`'s end-of-connect stamp,
  // the [WARN][BT] SKIPPED path would fire and pairedByUser would stay
  // blank — exactly the trap we hit before this change.
  //
  // Stamping eagerly means even if the actual BLE connect fails, we've
  // captured intent. Idempotent: helper no-ops if already owned.
  bleStampPairedByIfBlank(BLE_PEER_G2_GLASSES);

  if (!g2Connect(eye)) {
    return gConnectTaskActive
           ? "G2: connect already in progress — wait or use closeg2"
           : "Error: G2: failed to start connect task";
  }
  return "G2: scan/connect started in background — use g2status to watch";
}

static const char* cmd_g2disconnect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // `closeg2`        — disconnect, keep BLEClient + GATT cache. Reconnect
  //                    skips the 1.5–3 s service discovery and is near-
  //                    instant. Cost: ~15 KB per temple (~30 KB total)
  //                    stays allocated until openg2 / closeg2 full /
  //                    deinitG2Client / reboot.
  // `closeg2 full`   — disconnect AND free per-temple cache (templeReset
  //                    deletes BLEClient — chain-frees services, chars,
  //                    descriptors, plus rxBuf and writeMutex). Next
  //                    reconnect re-runs service discovery (slower) but
  //                    the heap recovers ~30 KB. Use when you're done
  //                    with the glasses for a while, or under DRAM
  //                    pressure.
  String arg = argsInput; arg.trim(); arg.toLowerCase();
  const bool fullReset = (arg == "full");
  g2Disconnect();
  if (fullReset) {
    templeReset(gL);
    templeReset(gR);
    return "G2: disconnected (full reset — GATT cache freed, ~30 KB recovered)";
  }
  return "G2: disconnected (cache retained for fast reconnect; use 'closeg2 full' to free)";
}

static const char* cmd_g2status(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char buf[256];
  getG2Status(buf, sizeof(buf));
  return buf;
}

// Dump everything we've learned about the connected glasses. Populated
// from the settings-push decoder (firmware version) + temple connection
// state (MAC, MTU, battery). Expand as we identify more fields.
static const char* cmd_g2info(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char buf[512];
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
           g2FsmHijackActive() ? "active" : "off",
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
  return "Error: invalid arguments — Usage: g2settings verbose [on|off]";
}

static const char* cmd_g2show(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return g2ShowText(argsInput.c_str()) ? "G2: text sent" : "Error: G2: send failed";
}

// Live-update probe cadence — read by Q13 (image-tile streaming) and Q14
// (text REBUILD streaming). Runtime-only; not persisted across boots.
// Sane lower bound 100 ms — below that the firmware ack queue saturates
// and the probe in-flight cap kicks in immediately. No upper clamp.
// Default 600 ms for snappier on-lens benches; tune with CLI `g2liverate`.
static constexpr uint32_t kG2LiveRateDefaultMs = 600;
static volatile uint32_t gG2LiveRateMs = kG2LiveRateDefaultMs;

// Q13/Q14 keep-alive toggle. When the firmware idle-times out the lens
// (~30 s with no ring/temple input), DISPLAY_OFF fires and clears
// `containerReady` on both arms. Default behavior (false): the loop
// breaks with reason "lens timeout" — clean shutdown, no flicker.
// When true: the loop re-CREATEs the container on the next frame
// (Q13 by clearing `createdOnce`, Q14 by g2ShowText auto-recreate)
// and continues. Each re-CREATE costs one ~70 ms flicker cycle but
// keeps long animations playing indefinitely. Tap-dismiss still wins
// over keep-alive — if the user double-taps, we break regardless.
static volatile bool gG2LiveLoopKeepAlive = false;

// Toggle the REBUILD-list fast path in the page-swap worker. When ON,
// REBUILD runs only if the incoming list has the same **row count** as
// the list on-lens (see gLastHijackListRowCount + gLastHijackListPageShape);
// otherwise we use
// SHUTDOWN+CREATE. Same-count swaps stay ~70 ms; changing counts need
// the slower path (avoids missing RebuildResp on some navigations).
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
    return "Error: invalid arguments — Usage: g2listrebuild [on|off]";
  }
  snprintf(out, sizeof(out),
           "G2 list-rebuild fast path: %s (%s)",
           gG2ListRebuildEnabled ? "ON" : "OFF",
           gG2ListRebuildEnabled
               ? "pure list, same row-count → REBUILD; else SHUTDOWN+CREATE"
               : "always SHUTDOWN+CREATE; brief flicker on nav");
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
    return "Error: invalid arguments — Usage: g2liverate [ms>=100]";
  }
  gG2LiveRateMs = (uint32_t)n;
  snprintf(out, sizeof(out), "G2 live-update rate set to %u ms",
           (unsigned)gG2LiveRateMs);
  return out;
}

// Toggle Q13/Q14 lens-idle keep-alive. Default off (clean break on
// DISPLAY_OFF — animation stops the moment the firmware idle-times
// out the lens, ~30 s with no user input). On (`g2liveloop keep on`)
// re-CREATEs the container each time the firmware times out, so long
// animations play indefinitely. Cost is one ~70 ms flicker cycle per
// idle timeout; user double-tap still wins over keep-alive.
static const char* cmd_g2liveloop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char out[120];
  CommandArgs ca(argsInput);
  String sub = ca.arg(0); sub.toLowerCase();
  String val = ca.arg(1); val.toLowerCase();
  if (sub == "keep") {
    if (val == "on" || val == "1" || val == "true") {
      gG2LiveLoopKeepAlive = true;
    } else if (val == "off" || val == "0" || val == "false") {
      gG2LiveLoopKeepAlive = false;
    } else if (val.length() != 0) {
      return "Error: invalid arguments — Usage: g2liveloop keep [on|off]";
    }
    snprintf(out, sizeof(out),
             "G2 live-loop keep-alive: %s (re-CREATEs on lens idle-timeout)",
             gG2LiveLoopKeepAlive ? "ON" : "OFF");
    return out;
  }
  // Status (no args, or 'status').
  snprintf(out, sizeof(out),
           "G2 live-loop: rate=%u ms, keep-alive=%s",
           (unsigned)gG2LiveRateMs, gG2LiveLoopKeepAlive ? "ON" : "OFF");
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
         : "Error: G2: AI reply failed";
}

static const char* cmd_g2ai_noask(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return g2ShowEvenAIReplyNoAsk(argsInput.c_str())
         ? "G2: AI reply sent (CTRL+ANALYSE+REPLY — skip ASK)"
         : "Error: G2: AI reply failed";
}

static const char* cmd_g2ai_direct(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return g2ShowEvenAIReplyDirect(argsInput.c_str())
         ? "G2: AI reply sent (CTRL+REPLY — original failing path)"
         : "Error: G2: AI reply failed";
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
    return "Error: invalid arguments — Usage: g2probe <sid_hex> <cmd_dec> [body_hex]\n"
           "  Fires `08 <cmd> 10 <magic> [body]` on the given sid (flag=0x20).\n"
           "  Example: g2probe 07 9              -- EvenAI HEARTBEAT\n"
           "           g2probe 07 10 080110A001  -- EvenAI CONFIG voiceSwitch=0,streamSpeed=160\n"
           "  sid=0x80 (dev_config) is blocked — known to brick.";
  }
  const uint8_t sid = (uint8_t)strtoul(ca.arg(0).c_str(), nullptr, 16);
  const uint32_t cmd = (uint32_t)ca.argInt(1, 0);

  if (sid == 0x80) {
    return "Error: G2: probe denied — sid=0x80 (dev_config) is in the brick blocklist.";
  }

  uint8_t body[200];
  size_t bodyLen = 0;
  if (ca.count() >= 3) {
    String hex = ca.arg(2);
    bodyLen = parseHexBytes(hex.c_str(), body, sizeof(body));
    if (bodyLen == SIZE_MAX) return "Error: G2: probe — bad hex body (need pairs of nibbles)";
  }

  // Build pb: f1 cmd, f2 magic=250, then raw body.
  uint8_t pb[256];
  size_t pos = 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, /*field*/ 1, cmd)) return "Error: G2: probe build failed (cmd)";
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, /*field*/ 2, 250)) return "Error: G2: probe build failed (magic)";
  if (bodyLen) {
    if (pos + bodyLen > sizeof(pb)) return "Error: G2: probe — body too large for single fragment";
    memcpy(pb + pos, body, bodyLen);
    pos += bodyLen;
  }

  uint8_t env[256];
  size_t n = g2BuildEnvelope(allocSeq(), sid, G2_FLAG_REQUEST, pb, pos, env, sizeof(env));
  if (n == 0) return "Error: G2: probe — envelope build failed";

  G2Temple* arm = pickEvenAIArm("g2probe");
  if (!arm) return "Error: G2: probe — no reachable temple";
  if (!sendEnvelope(*arm, env, n)) return "Error: G2: probe — send failed (mutex timeout?)";

  snprintf(ret, sizeof(ret),
           "G2: probe sid=0x%02X (%s) cmd=%u body=%u B sent — watch logs for response",
           sid, g2sidName(sid), (unsigned)cmd, (unsigned)bodyLen);
  return ret;
}

// g2devcfg <subcommand> [args]
// Typed senders for sid=0x80 (DevCfgDataPackage). Each subcommand calls a
// hardcoded-commandId builder in System_G2_Protocol.cpp — no way to
// reach a destructive opcode (cmd=11 SET_DEVICE_INFO, cmd=13
// RESTORE_TO_FACTORY_SETTINGS) without writing new C code. The g2probe
// blocklist for sid=0x80 stays in place; this is a separate code path
// covering only the safe-tier opcodes.
//
// Subcommands:
//   heartbeat                  — empty body (cmd=14). Safest possible.
//   auth                       — secAuth=true, phoneType=PHONE_ANDROID (cmd=4).
//   role <both|right|left>     — PIPE_ROLE_CHANGE (cmd=5). Sent to right arm.
//   time [tzQuarterHours]      — TIME_SYNC (cmd=128). Default tz=0 (UTC).
//                                 NB: quarter-hours = minutes/15. PST = -32.
//   ring <mac> <name>          — RING_CONNECT_INFO (cmd=6). Bridge trigger.
//                                 mac in BLE address order ("aa:bb:cc:dd:ee:ff"
//                                 or "aabbccddeeff"); builder reverses it
//                                 internally per FlutterApp behaviour.
//
// All subcommands send to the right arm only (matching FlutterApp's
// _runStandardSetup pattern). Returns a status string with sid + cmd +
// resulting envelope length so the user can confirm the wire shape.
static const char* cmd_g2devcfg(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char ret[200];
  CommandArgs ca(argsInput);
  if (ca.count() < 1) {
    return "Error: invalid arguments — Usage: g2devcfg <heartbeat|auth|role|time|ring> [args]\n"
           "  heartbeat                  — empty cmd=14, safest validator\n"
           "  auth                       — fixed AuthMgr (cmd=4)\n"
           "  role <both|right|left>     — PipeRoleChange (cmd=5)\n"
           "  time [tzQuarterHours]      — TimeSync (cmd=128); tz in mins/15\n"
           "  ring <mac> <name>          — RING_CONNECT_INFO (cmd=6)\n"
           "All sent to right arm. sid=0x80 = DevCfgDataPackage.";
  }

  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead) arm = &gR;
  if (!arm) return "Error: G2 devcfg: right arm not connected";

  String sub = ca.arg(0); sub.toLowerCase();
  uint8_t env[96];
  size_t n = 0;
  uint32_t cmd = 0;

  if (sub == "heartbeat" || sub == "hb") {
    n = g2BuildDevCfgHeartbeat(allocSeq(), G2_MAGIC_DEVCFG_HEARTBEAT,
                               env, sizeof(env));
    cmd = G2_DEVCFG_CMD_BASE_HEART_BEAT;
  } else if (sub == "auth") {
    n = g2BuildDevCfgAuth(allocSeq(), G2_MAGIC_DEVCFG_AUTH,
                          env, sizeof(env));
    cmd = G2_DEVCFG_CMD_AUTHENTICATION;
  } else if (sub == "role") {
    if (ca.count() < 2) return "G2 devcfg: usage — g2devcfg role <both|right|left>";
    String r = ca.arg(1); r.toLowerCase();
    uint8_t roleVal;
    if (r == "both")       roleVal = G2_DEVCFG_ROLE_BOTH;
    else if (r == "right") roleVal = G2_DEVCFG_ROLE_RIGHT;
    else if (r == "left")  roleVal = G2_DEVCFG_ROLE_LEFT;
    else return "Error: G2 devcfg: role must be both|right|left";
    n = g2BuildDevCfgPipeRoleChange(allocSeq(), G2_MAGIC_DEVCFG_PIPE_ROLE,
                                    roleVal, env, sizeof(env));
    cmd = G2_DEVCFG_CMD_PIPE_ROLE_CHANGE;
  } else if (sub == "time") {
    int32_t tzQ = (ca.count() >= 2) ? (int32_t)ca.argInt(1, 0) : 0;
    uint32_t now = (uint32_t)time(nullptr);
    n = g2BuildDevCfgTimeSync(allocSeq(), G2_MAGIC_DEVCFG_TIME_SYNC,
                              now, tzQ, env, sizeof(env));
    cmd = G2_DEVCFG_CMD_TIME_SYNC;
    if (n == 0) {
      snprintf(ret, sizeof(ret),
               "G2 devcfg: time rejected (ts=%u, tzQ=%d) — RTC unsynced or tz out of ±56 quarter-hours",
               (unsigned)now, (int)tzQ);
      return ret;
    }
  } else if (sub == "ring") {
    if (ca.count() < 3) return "G2 devcfg: usage — g2devcfg ring <mac> <name>";
    uint8_t macBle[6];
    String macStr = ca.arg(1);
    size_t macLen = parseHexBytes(macStr.c_str(), macBle, sizeof(macBle));
    if (macLen != 6) return "Error: G2 devcfg: ring mac must be 6 bytes (aa:bb:cc:dd:ee:ff)";
    String name = ca.arg(2);
    n = g2BuildDevCfgRingConnect(allocSeq(), G2_MAGIC_DEVCFG_RING_CONNECT,
                                 /*connect=*/true,
                                 macBle, name.c_str(), env, sizeof(env));
    cmd = G2_DEVCFG_CMD_RING_CONNECT_INFO;
    if (n == 0) {
      return "Error: G2 devcfg: ring build failed (name empty/>32 chars or null mac)";
    }
  } else {
    return "Error: G2 devcfg: unknown subcommand — try heartbeat|auth|role|time|ring";
  }

  if (n == 0) return "Error: G2 devcfg: envelope build failed";
  if (!sendEnvelope(*arm, env, n)) return "Error: G2 devcfg: send failed (mutex timeout?)";

  snprintf(ret, sizeof(ret),
           "G2 devcfg: sid=0x80 cmd=%u envLen=%u sent to right — watch logs for response",
           (unsigned)cmd, (unsigned)n);
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
    else return "Error: G2: imgprobe — size must be 1..4096 bytes";
  }

  // Build the test pattern on the heap; 4 KB on stack would crowd the
  // BTC task's budget. Free at the end of this function.
  uint8_t* pattern = (uint8_t*)ps_alloc(sizeBytes, AllocPref::PreferPSRAM, "g2.imgprobe.pattern");
  if (!pattern) return "Error: G2: imgprobe — pattern alloc failed";
  for (size_t i = 0; i < sizeBytes; i++) {
    pattern[i] = (i & 1) ? 0x0F : 0xF0;
  }

  // Image bodies can be much larger than a single fragment, so we
  // build the pb body into a separate buffer and let sendPbFragmented
  // chunk it. Size budget: pb wrapper overhead (~10 B) + nested
  // ImgRawMsg overhead (~6 B) + dataLen.
  const size_t bodyCap = sizeBytes + 64;
  uint8_t* body = (uint8_t*)ps_alloc(bodyCap, AllocPref::PreferPSRAM, "g2.imgprobe.body");
  if (!body) {
    free(pattern);
    return "Error: G2: imgprobe — body alloc failed";
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
    return "Error: G2: imgprobe — body build failed";
  }

  G2Temple* arm = pickEvenAIArm("g2imgprobe");
  if (!arm) {
    free(body);
    return "Error: G2: imgprobe — no reachable temple";
  }

  const bool ok = sendPbFragmented(*arm, allocSeq(), G2_SID_EVEN_CORE,
                                   G2_FLAG_REQUEST, body, bodyLen);
  free(body);
  if (!ok) return "Error: G2: imgprobe — fragmented send failed";

  snprintf(ret, sizeof(ret),
           "G2: imgprobe sent %u B body via Cmd=3 multi-frag — "
           "expect ImgRawFailed(5) on Cmd=4 (no CREATE-image yet); "
           "watch logs for OS_RESPONSE_IMAGE_RAW_DATA",
           (unsigned)bodyLen);
  return ret;
}

// G2 mic probe — sends AudioCtrCmd to start/stop the LC3 mic stream
// and dumps frame stats. The audio-notify subscription is wired at
// connect time on both arms (see subscribeAudioNotify); these CLIs
// just toggle the firmware-side stream and read the counters.
//
// Usage:
//   g2micon          → AudioCtrCmd{AudoFuncEn=1} on LEFT (default mic arm)
//   g2micon r        → same, but target RIGHT temple (for comparison)
//   g2micoff         → AudioCtrCmd{AudoFuncEn=0} on whichever arm is up
//   g2micstats       → print accumulated frame counters per arm
//   g2micreset       → zero counters without changing stream state
//   g2micverbose [on|off] → toggle per-frame log line (default off)
static G2Temple* pickMicArm(const char* tag, bool preferLeft) {
  if (preferLeft && gL.connected) return &gL;
  if (gR.connected) return &gR;
  if (gL.connected) return &gL;
  DEBUG_G2F("[%s] no connected temple", tag);
  return nullptr;
}

static const char* cmd_g2micon(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char ret[200];
  CommandArgs ca(argsInput);
  String armArg = ca.count() > 0 ? ca.arg(0) : String("");
  char first = armArg.length() > 0 ? armArg.charAt(0) : '\0';
  bool preferLeft = !(first == 'r' || first == 'R');
  G2Temple* arm = pickMicArm("g2micon", preferLeft);
  if (!arm) return "Error: G2 mic: no reachable temple";

  uint8_t buf[64];
  size_t n = g2BuildAudioCtrl(allocSeq(), G2_MAGIC_AUDIO_CTRL,
                              /*enable*/ true, buf, sizeof(buf));
  if (!n) return "Error: G2 mic: AudioCtrl build failed";
  if (!sendEnvelope(*arm, buf, n)) return "Error: G2 mic: TX failed";
  gMicProbeActive = true;
  snprintf(ret, sizeof(ret),
           "G2 mic: AudioCtrCmd{en=1} sent on %c — watch [G2-MIC-%c] logs "
           "for frames on 6402; if none arrive within ~2s, firmware "
           "isn't streaming on this char",
           arm->side, arm->side);
  return ret;
}

static const char* cmd_g2micoff(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char ret[160];
  G2Temple* arm = pickMicArm("g2micoff", true);
  if (!arm) return "Error: G2 mic: no reachable temple";
  uint8_t buf[64];
  size_t n = g2BuildAudioCtrl(allocSeq(), G2_MAGIC_AUDIO_CTRL,
                              /*enable*/ false, buf, sizeof(buf));
  if (!n) return "Error: G2 mic: AudioCtrl build failed";
  if (!sendEnvelope(*arm, buf, n)) return "Error: G2 mic: TX failed";
  gMicProbeActive = false;
  snprintf(ret, sizeof(ret),
           "G2 mic: AudioCtrCmd{en=0} sent on %c — frame counters "
           "left intact (use g2micreset to zero)", arm->side);
  return ret;
}

static const char* cmd_g2micstats(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char ret[320];
  auto fmtArm = [](char side, const G2MicProbe& m, char* out, size_t cap) {
    uint32_t span = (m.frameCount > 1) ? (m.lastFrameMs - m.firstFrameMs) : 0;
    uint32_t fps = span ? (m.frameCount * 1000u) / span : 0;
    snprintf(out, cap,
             "%c=%u frames %u B (CC=%u CD=%u other=%u gaps=%u ~%u fps)",
             side, (unsigned)m.frameCount, (unsigned)m.bytesTotal,
             (unsigned)m.byte0CC, (unsigned)m.byte0CD,
             (unsigned)m.byte0Other, (unsigned)m.seqGaps, (unsigned)fps);
  };
  char l[160], r[160];
  fmtArm('L', gMicL, l, sizeof(l));
  fmtArm('R', gMicR, r, sizeof(r));
  snprintf(ret, sizeof(ret), "G2 mic: %s | %s%s", l, r,
           gMicProbeActive ? " (stream ON)" : " (stream OFF)");
  return ret;
}

static const char* cmd_g2micreset(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  resetMicProbeStats(gMicL);
  resetMicProbeStats(gMicR);
  return "G2 mic: counters reset";
}

static const char* cmd_g2micverbose(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char ret[80];
  CommandArgs ca(argsInput);
  if (ca.count() > 0) {
    String v = ca.arg(0);
    char c = v.length() > 0 ? v.charAt(0) : '\0';
    gMicProbeVerbose = (c == '1' || c == 'o' || c == 'O' ||
                        c == 'y' || c == 'Y' || c == 't' || c == 'T');
  } else {
    gMicProbeVerbose = !gMicProbeVerbose;
  }
  snprintf(ret, sizeof(ret), "G2 mic verbose: %s",
           gMicProbeVerbose ? "ON (every frame)" : "OFF (first 5 + 1/sec)");
  return ret;
}

// Phase-1 audio integration: dump raw mic packets to SD as a .lc3 file
// so we can pull it off the device and decode offline with liblc3.
// Confirms (a) the bytes are valid LC3 and (b) whether there's a
// 5-byte packet header to strip before LC3 decode. Independent of
// g2micon — the user can flip recording on/off without restarting the
// stream, useful for capturing a clean 5–10 sec sample.
//
// Usage:
//   g2micrec start [path]   default path: /sd/g2_mic-<unixTs>.lc3
//   g2micrec stop           close the file
//   g2micrec status         print state (or `g2micrec` with no args)
//
// File is capped at 5 MB (~20 min at 4.1 KB/s) to avoid eating the SD
// card if the user forgets to stop. Recorder watches LEFT only —
// 2.2.0.24 emits zero frames on RIGHT's 6402 (verified empirically).
static const char* cmd_g2micrec(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char ret[240];
  CommandArgs ca(argsInput);
  String sub = ca.count() > 0 ? ca.arg(0) : String("");
  sub.toLowerCase();

  ensureMicRecMutex();
  if (!gMicRecMutex) return "Error: G2 mic rec: mutex alloc failed";

  if (sub == "start") {
    if (!VFS::isSDAvailable()) return "Error: G2 mic rec: SD card not available";
    xSemaphoreTake(gMicRecMutex, portMAX_DELAY);
    if (gMicRecFile) {
      xSemaphoreGive(gMicRecMutex);
      return "Error: G2 mic rec: already recording — run g2micrec stop first";
    }
    String path;
    if (ca.count() > 1) {
      const char* qerr = requireQuotedToken(ca, 1, path);
      if (qerr) { xSemaphoreGive(gMicRecMutex); return qerr; }
    }
    if (path.length() == 0) {
      uint32_t ts = (uint32_t)time(nullptr);
      if (ts == 0) ts = millis() / 1000;
      char buf[64];
      snprintf(buf, sizeof(buf), "/sd/g2_mic-%lu.lc3", (unsigned long)ts);
      path = buf;
    } else if (!path.startsWith("/")) {
      path = String("/sd/") + path;
    }
    File* f = new File(VFS::openGuarded(path, "w", currentAuthContext(), true));
    if (!f || !*f) {
      delete f;
      xSemaphoreGive(gMicRecMutex);
      snprintf(ret, sizeof(ret), "G2 mic rec: failed to open %s", path.c_str());
      return ret;
    }
    gMicRecFile    = f;
    gMicRecPath    = path;
    gMicRecBytes   = 0;
    gMicRecPackets = 0;
    gMicRecStartMs = millis();
    gMicRecArm     = 'L';
    xSemaphoreGive(gMicRecMutex);
    snprintf(ret, sizeof(ret),
             "G2 mic rec: writing to %s (cap %u B). Run g2micon if stream "
             "isn't active; g2micrec stop to close.",
             path.c_str(), (unsigned)kMicRecMaxBytes);
    return ret;
  }

  if (sub == "stop") {
    xSemaphoreTake(gMicRecMutex, portMAX_DELAY);
    if (!gMicRecFile) {
      xSemaphoreGive(gMicRecMutex);
      return "Error: G2 mic rec: not recording";
    }
    String path     = gMicRecPath;
    uint32_t bytes  = gMicRecBytes;
    uint32_t pkts   = gMicRecPackets;
    uint32_t durMs  = millis() - gMicRecStartMs;
    micRecCloseLocked("user stop");
    xSemaphoreGive(gMicRecMutex);
    snprintf(ret, sizeof(ret),
             "G2 mic rec: closed %s (%u packets, %u B, %u ms)",
             path.c_str(), (unsigned)pkts, (unsigned)bytes, (unsigned)durMs);
    return ret;
  }

  // status (default)
  xSemaphoreTake(gMicRecMutex, portMAX_DELAY);
  if (!gMicRecFile) {
    xSemaphoreGive(gMicRecMutex);
    return "G2 mic rec: idle (use g2micrec start [\"path\"])";
  }
  String path    = gMicRecPath;
  uint32_t bytes = gMicRecBytes;
  uint32_t pkts  = gMicRecPackets;
  uint32_t durMs = millis() - gMicRecStartMs;
  xSemaphoreGive(gMicRecMutex);
  snprintf(ret, sizeof(ret),
           "G2 mic rec: %s — %u packets, %u B, %u ms (%u%% of cap)",
           path.c_str(), (unsigned)pkts, (unsigned)bytes, (unsigned)durMs,
           (unsigned)((bytes * 100u) / kMicRecMaxBytes));
  return ret;
}

// Phase-2A audio integration: on-device LC3 decode → WAV on SD.
// Strips the 5 B trailer per BLE packet, decodes 5 × 40 B LC3 frames
// to 5 × 160 int16 samples (16 kHz / 10 ms / 32 kbps), appends the
// 1600 B of PCM to a WAV file. Independent of g2micrec — both can
// run simultaneously to compare raw vs decoded.
//
// Usage:
//   g2micwav start [path]   default: /sd/g2_mic-<unixTs>.wav
//   g2micwav stop           patches WAV header sizes and closes
//   g2micwav status         (or `g2micwav` with no args)
//
// File capped at 16 MB (~8.7 min @ 32 KB/s decoded).
static const char* cmd_g2micwav(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  EXT_RAM_BSS_ATTR static char ret[260];
  CommandArgs ca(argsInput);
  String sub = ca.count() > 0 ? ca.arg(0) : String("");
  sub.toLowerCase();

  ensureMicWavMutex();
  if (!gMicWavMutex) return "Error: G2 mic wav: mutex alloc failed";

  if (sub == "start") {
    if (!VFS::isSDAvailable()) return "Error: G2 mic wav: SD card not available";
    xSemaphoreTake(gMicWavMutex, portMAX_DELAY);
    if (gMicWavFile) {
      xSemaphoreGive(gMicWavMutex);
      return "Error: G2 mic wav: already recording — run g2micwav stop first";
    }

    // Allocate decoder memory + set up the decoder. liblc3 needs ~6 KB
    // for 16 kHz / 10 ms; sized exactly via lc3_decoder_size.
    unsigned decSize = lc3_decoder_size(kMicLc3FrameUs, kMicLc3SampleHz);
    if (decSize == 0) {
      xSemaphoreGive(gMicWavMutex);
      return "Error: G2 mic wav: lc3_decoder_size returned 0 (bad params?)";
    }
    void* mem = malloc(decSize);
    if (!mem) {
      xSemaphoreGive(gMicWavMutex);
      snprintf(ret, sizeof(ret), "G2 mic wav: malloc %u B failed", decSize);
      return ret;
    }
    lc3_decoder_t dec = lc3_setup_decoder(kMicLc3FrameUs, kMicLc3SampleHz,
                                          /*sr_pcm_hz*/ 0, mem);
    if (!dec) {
      free(mem);
      xSemaphoreGive(gMicWavMutex);
      return "Error: G2 mic wav: lc3_setup_decoder failed";
    }

    String path;
    if (ca.count() > 1) {
      const char* qerr = requireQuotedToken(ca, 1, path);
      if (qerr) { free(mem); xSemaphoreGive(gMicWavMutex); return qerr; }
    }
    if (path.length() == 0) {
      uint32_t ts = (uint32_t)time(nullptr);
      if (ts == 0) ts = millis() / 1000;
      char buf[64];
      snprintf(buf, sizeof(buf), "/sd/g2_mic-%lu.wav", (unsigned long)ts);
      path = buf;
    } else if (!path.startsWith("/")) {
      path = String("/sd/") + path;
    }
    File* f = new File(VFS::openGuarded(path, "w", currentAuthContext(), true));
    if (!f || !*f) {
      delete f;
      free(mem);
      xSemaphoreGive(gMicWavMutex);
      snprintf(ret, sizeof(ret), "G2 mic wav: failed to open %s", path.c_str());
      return ret;
    }
    micWavWriteHeader(*f, /*pcmBytes*/ 0);
    gMicWavFile        = f;
    gMicWavPath        = path;
    gMicWavBytes       = 0;
    gMicWavPackets     = 0;
    gMicWavStartMs     = millis();
    gMicWavDecoder     = dec;
    gMicWavDecMem      = mem;
    gMicWavDecodeFails = 0;
    gMicWavPlcFrames   = 0;
    gMicRecArm         = 'L';
    xSemaphoreGive(gMicWavMutex);
    snprintf(ret, sizeof(ret),
             "G2 mic wav: writing to %s (16k mono 16-bit, decoder=%u B). "
             "Need an active hijack page for the firmware to send audio.",
             path.c_str(), decSize);
    return ret;
  }

  if (sub == "stop") {
    xSemaphoreTake(gMicWavMutex, portMAX_DELAY);
    if (!gMicWavFile) {
      xSemaphoreGive(gMicWavMutex);
      return "Error: G2 mic wav: not recording";
    }
    String path     = gMicWavPath;
    uint32_t bytes  = gMicWavBytes;
    uint32_t pkts   = gMicWavPackets;
    uint32_t durMs  = millis() - gMicWavStartMs;
    uint32_t fails  = gMicWavDecodeFails;
    uint32_t plc    = gMicWavPlcFrames;
    micWavCloseLocked("user stop");
    xSemaphoreGive(gMicWavMutex);
    float secs = bytes ? (float)bytes / (float)(kMicLc3SampleHz * 2) : 0.f;
    snprintf(ret, sizeof(ret),
             "G2 mic wav: closed %s — %u packets, %u B PCM (%.2f s audio), "
             "%u ms wall, %u decode-fails, %u PLC",
             path.c_str(), (unsigned)pkts, (unsigned)bytes, (double)secs,
             (unsigned)durMs, (unsigned)fails, (unsigned)plc);
    return ret;
  }

  // status (default)
  xSemaphoreTake(gMicWavMutex, portMAX_DELAY);
  if (!gMicWavFile) {
    xSemaphoreGive(gMicWavMutex);
    return "G2 mic wav: idle (use g2micwav start [\"path\"])";
  }
  String path    = gMicWavPath;
  uint32_t bytes = gMicWavBytes;
  uint32_t pkts  = gMicWavPackets;
  uint32_t durMs = millis() - gMicWavStartMs;
  uint32_t fails = gMicWavDecodeFails;
  xSemaphoreGive(gMicWavMutex);
  float secs = bytes ? (float)bytes / (float)(kMicLc3SampleHz * 2) : 0.f;
  snprintf(ret, sizeof(ret),
           "G2 mic wav: %s — %u packets, %u B PCM (%.2f s), %u ms, "
           "%u decode-fails (%u%% of cap)",
           path.c_str(), (unsigned)pkts, (unsigned)bytes, (double)secs,
           (unsigned)durMs, (unsigned)fails,
           (unsigned)((bytes * 100u) / kMicWavMaxBytes));
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
  if (n == 0) return "Error: G2: aiconfig — envelope build failed";

  G2Temple* arm = pickEvenAIArm("g2aiconfig");
  if (!arm) return "Error: G2: aiconfig — no reachable temple";
  if (!sendEnvelope(*arm, env, n)) return "Error: G2: aiconfig — send failed (mutex timeout?)";

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
           : "Error: G2: AI reply failed";
  }
  String heading = argsInput.substring(0, sep);
  String body    = argsInput.substring(sep + 1);
  heading.trim();
  body.trim();
  return g2ShowEvenAIReply(heading.c_str(), body.c_str())
         ? "G2: AI reply sent (custom heading)"
         : "Error: G2: AI reply failed";
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
  if (args.length() == 0) return "Error: invalid arguments — Usage: g2notify [<seconds>] <text>";

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
  if (args.length() == 0) return "Error: invalid arguments — Usage: g2notify [<seconds>] <text>";
  return g2ShowNotification(args.c_str(), duration)
         ? "G2 notify: shown (placeholder — not a real overlay, full-screen)"
         : "Error: G2 notify: show failed";
}

static const char* cmd_g2clear(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return g2ClearDisplay() ? "G2: cleared" : "Error: G2: clear failed";
}

static const char* cmd_g2scan(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return g2Connect(G2_EYE_AUTO) ? "G2: found" : "Error: G2: not found";
}

static const char* cmd_g2init(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return initG2Client() ? "G2: init ok" : "Error: G2: init failed";
}

static const char* cmd_g2deinit(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  deinitG2Client();
  return "G2: deinit ok";
}

static const char* cmd_g2battery(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!isG2Connected()) return "Error: G2: not connected";
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
  if (!isG2Connected()) return "Error: G2: not connected";
  CommandArgs ca(argsInput);
  String a = ca.arg(0); a.toLowerCase();
  bool enable = (a == "on" || a == "start" || a == "1");
  uint8_t buf[64];
  uint8_t seq = allocSeq();
  size_t n = g2BuildAudioCtrl(seq, G2_MAGIC_AUDIO_CTRL, enable,
                              buf, sizeof(buf));
  if (n == 0 || !sendToBoth(buf, n)) return "Error: G2 mic: send failed";
  // NOTE: audio frames are delivered on the left temple's render-notify
  // (…e6402), not on the command-notify (…e5402) we currently subscribe
  // to. Enabling the mic here is harmless but no audio will arrive until
  // a future phase subscribes to the render-notify characteristic and
  // ports an LC3 decoder.
  return enable ? "G2 mic: requested ON (LC3 decode not yet wired)"
                : "G2 mic: requested OFF";
}

// Stream dims live in gSettings.g2StreamWidth/Height (persisted across
// reboots, set via this CLI command or via the Camera Settings lens page).
// The stream worker reads them at session start; mid-stream changes apply
// on the next stream restart.

static const char* cmd_g2streamres(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char ret[160];
  String s = argsInput; s.trim();
  if (s.length() == 0) {
    snprintf(ret, sizeof(ret),
             "G2 stream resolution: %dx%d (allowed 16..288 x 16..144). "
             "Set with: g2streamres <W>x<H>",
             gSettings.g2StreamWidth, gSettings.g2StreamHeight);
    return ret;
  }
  int xi = s.indexOf('x');
  if (xi < 0) xi = s.indexOf('X');
  if (xi < 0) xi = s.indexOf(' ');
  if (xi <= 0) {
    return "Error: invalid arguments — Usage: g2streamres <W>x<H>  (examples: 96x96, 160x120, 288x144)";
  }
  int w = s.substring(0, xi).toInt();
  int h = s.substring(xi + 1).toInt();
  if (w < 16 || w > 288 || h < 16 || h > 144) {
    snprintf(ret, sizeof(ret),
             "Bad dims %dx%d (allowed 16..288 x 16..144)", w, h);
    return ret;
  }
  const int wEff = w & ~1;  // 4-bpp packs 2 px/byte
  setSetting(gSettings.g2StreamWidth,  wEff);
  setSetting(gSettings.g2StreamHeight, h);
  snprintf(ret, sizeof(ret),
           "G2 stream resolution set to %dx%d%s. Restart the stream to apply.",
           gSettings.g2StreamWidth, gSettings.g2StreamHeight,
           (wEff != w) ? " (W rounded down to even)" : "");
  return ret;
}

// Q25 (SD-pack animation) playback cadence in ms per frame. Independent
// of g2liverate (which paces the live-tile test probes) so animation
// playback speed doesn't interfere with cinematic test cadences.
// Takes effect on the next pack run. For large frames the BLE push
// time dominates and the cap is effectively ignored.
static const char* cmd_g2packrate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char ret[160];
  String s = argsInput; s.trim();
  if (s.length() == 0) {
    snprintf(ret, sizeof(ret),
             "G2 pack playback cadence: %d ms/frame (~%d fps). Set: g2packrate <ms>",
             gSettings.g2PackRateMs,
             gSettings.g2PackRateMs > 0 ? (1000 / gSettings.g2PackRateMs) : 0);
    return ret;
  }
  int v = s.toInt();
  if (v < 20 || v > 2000) {
    return "Error: invalid arguments — Usage: g2packrate <ms>  (range 20..2000)";
  }
  setSetting(gSettings.g2PackRateMs, v);
  snprintf(ret, sizeof(ret),
           "G2 pack playback cadence set to %d ms/frame (~%d fps). Takes effect on next pack run.",
           v, 1000 / v);
  return ret;
}

// G2 stream tone mapping (auto-levels). When enabled, each frame's BMP
// build does a per-frame luma min/max scan and remaps to full 0..255
// range before quantizing to 4-bpp. Recovers dynamic range on washed-out
// OV3660 frames that would otherwise quantize to similar shades on the
// green-tinted G2 panel. Applies on the next frame after the change —
// no stream restart needed.
static const char* cmd_g2streamtonemap(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static char ret[140];
  String s = argsInput; s.trim();
  if (s.length() == 0) {
    snprintf(ret, sizeof(ret),
             "G2 stream tone-map (auto-levels): %s. Toggle: g2streamtonemap <on|off>",
             gSettings.g2StreamToneMap ? "ON" : "OFF");
    return ret;
  }
  int p = parseBoolArg(s);
  if (p < 0) {
    return "Error: invalid arguments — Usage: g2streamtonemap <on|off>";
  }
  setSetting(gSettings.g2StreamToneMap, p ? true : false);
  snprintf(ret, sizeof(ret),
           "G2 stream tone-map (auto-levels): %s%s",
           p ? "ON" : "OFF",
           p ? " — washed-out frames will get stretched to full 0..255 range"
             : " — frames render with raw luma quantization");
  return ret;
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
  if (!gR.connected) return "Error: G2: right temple not connected";
  handleMenuStartUp(gR, BLOCKS_WIDGET_ID);
  return g2FsmHijackActive() ? "G2 hijack: fired (status page shown)"
                             : "G2 hijack: fire attempted — check logs";
}

// Manual missing-arm recovery — same path as the heartbeat-tick auto-
// retry, but bypasses the backoff gate so the user can force a try
// immediately. Resets the attempt counter so subsequent ticks have a
// fresh allowance if this manual try doesn't succeed.
static const char* cmd_g2recover(const String& /*argsInput*/) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gG2State || !gG2State->initialized) return "Error: G2: client not initialized";
  if (gL.connected && gR.connected) return "G2: both temples already connected";
  if (!gL.connected && !gR.connected) {
    return "Error: G2: both temples down — use 'openg2 auto' for a full reconnect";
  }
  resetRecoveryBackoff();
  const bool ok = attemptMissingArmRecovery();
  // Schedule the next auto-retry from now if this manual one missed,
  // so the heartbeat tick takes over without waiting on a stale deadline.
  if (!ok) gNextRecoveryAttemptMs = millis() + kRecoveryBackoffMs[0];
  return ok ? "G2 recovery: missing temple reconnected"
            : "G2 recovery: missing temple not seen — auto-retries scheduled";
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
  if (!gR.connected) return "Error: G2: right temple not connected — reconnect first";
  if (gR.pluginDead) return "Error: G2: plugin task silent — reconnect to recover";
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
#if ENABLE_MAPS
static const char* cmd_g2map(const String& argsInput);
#endif

// `extern` on both this table and its count below because System_Utils.cpp
// refers to them as `extern const ...` — without it, C++ gives file-scope
// const objects internal linkage and the link fails.
extern const CommandEntry g2Commands[] = {
  { "openg2",       "Connect to G2 glasses: openg2 [left|right|auto]", false, cmd_g2connect, "Usage: openg2 [left|right|auto]  (default auto)" },
  { "closeg2",      "Disconnect G2 glasses [full=also free ~30KB GATT cache].", false, cmd_g2disconnect, "Usage: closeg2 [full]" },
  { "g2status",     "Show G2 connection status",                       false, cmd_g2status },
  { "g2info",       "Dump device info (firmware, MAC, battery, etc.)",  false, cmd_g2info },
  { "g2settings",   "Settings debug: g2settings verbose [on|off]",     false, cmd_g2settings, "Usage: g2settings verbose [<on|off>]  (bare verbose = toggle)" },
  { "g2liverate",   "Get/set live-update probe cadence (ms), default 600: g2liverate [N]", false, cmd_g2liverate, "Usage: g2liverate [ms>=100]  (bare = report)" },
  { "g2liveloop",   "Q13/Q14 lens-idle keep-alive: g2liveloop keep [on|off] (default off → break on lens timeout)", false, cmd_g2liveloop, "Usage: g2liveloop keep [<on|off>]  (bare = report state)" },
  { "g2listrebuild","REBUILD-list on swap when pure list + same row count [on|off] (default ON)", false, cmd_g2listrebuild, "Usage: g2listrebuild [<on|off>]  (bare = report state)" },
  { "g2show",       "Display text: g2show <text>",                     false, cmd_g2show, "Usage: g2show <text>" },
  { "g2ai",         "Front-pane AI card (full pipeline): g2ai <text>", false, cmd_g2ai, "Usage: g2ai <text>" },
  { "g2ai-noask",   "Variant: skip ASK step: g2ai-noask <text>",       false, cmd_g2ai_noask, "Usage: g2ai-noask <text>" },
  { "g2ai-direct",  "Variant: CTRL+REPLY only: g2ai-direct <text>",    false, cmd_g2ai_direct, "Usage: g2ai-direct <text>" },
  { "g2aih",        "Front-pane card with custom heading: g2aih <heading>|<body>", false, cmd_g2aih, "Usage: g2aih <heading>|<body>  (no | = whole text as body)" },
  { "g2aiconfig",   "Probe EvenAI CONFIG (cmd=10): g2aiconfig [voiceSwitch] [streamSpeed], use - to omit",  false, cmd_g2aiconfig, "Usage: g2aiconfig [voiceSwitch] [streamSpeed]  (use - to omit a field; bare = empty body)" },
  { "g2imgprobe",   "Probe Cmd=3 multi-frag wire path: g2imgprobe [size_bytes]",                            false, cmd_g2imgprobe, "Usage: g2imgprobe [size_bytes]  (1..4096, default 1024)" },
  { "g2micon",      "G2 mic probe: AudioCtrCmd{en=1} on LEFT (or 'r' for RIGHT)",                           false, cmd_g2micon, "Usage: g2micon [r]  (default LEFT; arg starting r = RIGHT)" },
  { "g2micoff",     "G2 mic probe: AudioCtrCmd{en=0} (stop stream)",                                        false, cmd_g2micoff },
  { "g2micstats",   "G2 mic probe: dump per-arm frame counters",                                            false, cmd_g2micstats },
  { "g2micreset",   "G2 mic probe: zero per-arm counters",                                                  false, cmd_g2micreset },
  { "g2micverbose", "G2 mic probe: per-frame log [on|off]",                                                 false, cmd_g2micverbose, "Usage: g2micverbose [<on|off>]  (bare = toggle)" },
  { "g2micrec",     "G2 mic dump: g2micrec start [\"path\"] | stop | status — writes raw 205B LC3 packets to SD", false, cmd_g2micrec, "Usage: g2micrec start [\"path\"] | stop | status  (bare = status)" },
  { "g2micwav",     "G2 mic decode: g2micwav start [\"path\"] | stop | status — decodes LC3 → 16k mono WAV on SD", false, cmd_g2micwav, "Usage: g2micwav start [\"path\"] | stop | status  (bare = status)" },
  { "g2protostats", "Show G2 protocol stats per sid: g2protostats [verbose]",      false, cmd_g2protostats, "Usage: g2protostats [verbose]" },
  { "g2probe",      "Fire arbitrary pb cmd: g2probe <sid_hex> <cmd_dec> [body_hex]", false, cmd_g2probe, "Usage: g2probe <sid_hex> <cmd_dec> [body_hex]  (sid=0x80 blocked)" },
  { "g2devcfg",     "Typed sid=0x80 sender: g2devcfg <heartbeat|auth|role|time|ring> [args]", false, cmd_g2devcfg, "Usage: g2devcfg <heartbeat|auth|role <both|right|left>|time [tzQuarterHours]|ring <mac> <name>>" },
  { "g2notify",     "Transient text (placeholder): g2notify [secs] <text>", false, cmd_g2notify, "Usage: g2notify [<seconds>] <text>  (seconds 1..599, default 5)" },
  { "g2bmp",        "Display BMP: g2bmp </path.bmp> [brightness -100..100] [contrast -100..100] [holdSeconds 0..120]", false, cmd_g2bmp, "Usage: g2bmp </path/to/file.bmp> [brightness -100..100] [contrast -100..100] [holdSeconds 0..120]" },
#if ENABLE_MAPS
  { "g2map",        "Render the offline map on the G2 lens (288x144)",  false, cmd_g2map, "Usage: g2map   (renders the current map view; double-tap the lens to dismiss)" },
#endif
  { "g2sensors",    "Show device's sensor list on the G2 lens",        false, cmd_g2sensors },
  { "g2network",    "Show Network info page on the G2 lens",           false, cmd_g2network },
  { "g2settingspage","Show Settings inspector page on the G2 lens",    false, cmd_g2settingspage },
  { "g2files",      "Show Files browser page on the G2 lens",          false, cmd_g2files },
  { "g2clear",      "Clear G2 display",                                false, cmd_g2clear },
  { "g2scan",       "Scan for G2 glasses",                             false, cmd_g2scan },
  { "g2init",       "Initialize G2 client mode",                       false, cmd_g2init },
  { "g2deinit",     "Deinitialize G2 client mode",                     false, cmd_g2deinit },
  { "g2nav",        "Menu navigation mode: g2nav [on|off|toggle] (bare = report state)", false, cmd_g2nav, "Usage: g2nav [on|off|toggle]  (bare = report state)" },
  { "g2streamres",  "Lens stream resolution: g2streamres [<W>x<H>] (bare = report; e.g., 96x96, 160x120, 288x144)", false, cmd_g2streamres, "Usage: g2streamres [<W>x<H>]  (W 16..288, H 16..144; bare = report)" },
  { "g2streamtonemap", "Lens stream auto-levels: g2streamtonemap [on|off] (bare = report state)", false, cmd_g2streamtonemap, "Usage: g2streamtonemap [<on|off>]  (bare = report state)" },
  { "g2packrate",   "SD-pack animation cadence: g2packrate [<ms>] (range 20..2000, default 80)", false, cmd_g2packrate, "Usage: g2packrate [<ms>]  (20..2000; bare = report)" },
  { "g2battery",    "Query G2 battery % on connected temples",         false, cmd_g2battery },
  { "g2mic",        "Enable/disable G2 mic capture: g2mic <on|off>",   false, cmd_g2mic, "Usage: g2mic <on|off>" },
  { "g2verbose",    "Scan-verbose logging: g2verbose [on|off|toggle] (bare = report state)", false, cmd_g2verbose, "Usage: g2verbose [on|off|toggle]  (bare = report state)" },
  { "g2hijacktest", "Simulate a Blocks tap (status-page hijack)",      false, cmd_g2hijacktest },
  { "g2reopen",     "Re-open the hijacked Blocks app after an abnormal exit", false, cmd_g2reopen },
  { "g2dumpframes", "Print the recent G2 envelope ring buffer",        false, cmd_g2dumpframes },
  { "g2recover",    "Try to reconnect a missing G2 temple without tearing down the connected one", false, cmd_g2recover },
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

// Exact on-wire size for BI_RGB 4bpp BMP from buildBmp4bpp (rows are
// 4-byte aligned). Do not use width*height/2 alone — e.g. 124×124 needs
// 8054 B while w*h/2 is only 7688 B of pixel payload before padding.
static size_t bmp4bppFileBytesU32(uint32_t aw, uint32_t ah) {
  if (aw == 0 || ah == 0) return 0;
  const uint32_t rowStride = ((aw * 4 + 31) / 32) * 4;
  return (size_t)(14u + 40u + 64u) + (size_t)rowStride * ah;
}

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

// 2-bpp BMP builder — same shape as buildBmp4bpp but with biBitCount=2,
// a 4-entry grayscale palette (0/85/170/255), and pixel data packed at
// 4 px/byte. Exists only to test whether the G2 lens firmware decodes
// lower bit depths; if it does, the camera streamer can cut payload
// size by ~50% with no resolution change. If it doesn't, lens stays
// blank but acks still come back (same diagnostic shape as Q19).
static size_t buildBmp2bpp(uint8_t* out, size_t outCap,
                           int32_t width, int32_t height,
                           BmpPattern pattern) {
  if (!out || outCap == 0) return 0;
  const uint32_t aw = (uint32_t)(width  < 0 ? -width  : width);
  const uint32_t ah = (uint32_t)(height < 0 ? -height : height);
  if (aw == 0 || ah == 0) return 0;
  // 2-bpp = 4 px/byte. Row stride is 4-byte aligned per BMP spec.
  const uint32_t rowStride  = ((aw * 2 + 31) / 32) * 4;
  const uint32_t pixelSize  = rowStride * ah;
  const uint32_t paletteSize = 4 * 4;          // 4 entries × BGRA
  const uint32_t headerSize  = 14 + 40 + paletteSize;
  const uint32_t total       = headerSize + pixelSize;
  if (total > outCap) return 0;

  auto wr16 = [](uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
  };
  auto wr32 = [](uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);          p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);  p[3] = (uint8_t)((v >> 24) & 0xff);
  };

  // BITMAPFILEHEADER (14 B)
  out[0] = 'B'; out[1] = 'M';
  wr32(out + 2,  total);
  wr16(out + 6,  0);
  wr16(out + 8,  0);
  wr32(out + 10, headerSize);

  // BITMAPINFOHEADER (40 B) — biBitCount = 2, biClrUsed = 4
  wr32(out + 14, 40);
  wr32(out + 18, (uint32_t)width);
  wr32(out + 22, (uint32_t)height);
  wr16(out + 26, 1);
  wr16(out + 28, 2);                 // 2-bpp
  wr32(out + 30, 0);                 // BI_RGB
  wr32(out + 34, pixelSize);
  wr32(out + 38, 2835);
  wr32(out + 42, 2835);
  wr32(out + 46, 4);                 // biClrUsed = 4
  wr32(out + 50, 0);

  // 4-shade grayscale palette (BGRA): 0, 85, 170, 255.
  for (int i = 0; i < 4; i++) {
    const uint8_t v = (uint8_t)((i * 255) / 3);
    out[54 + i*4 + 0] = v;
    out[54 + i*4 + 1] = v;
    out[54 + i*4 + 2] = v;
    out[54 + i*4 + 3] = 0;
  }

  // Pixel data. 2-bpp packing: high bits = leftmost pixel.
  //   0xCC = 11 00 11 00 → indices 3,0,3,0 (W,B,W,B)
  //   0x33 = 00 11 00 11 → indices 0,3,0,3 (B,W,B,W)
  // Alternating bytes give 1-px vertical stripes — equivalent diagnostic
  // to Q19's 4-bpp 0xF0/0x0F pattern.
  uint8_t* pixels = out + headerSize;
  if (pattern == BMP_PAT_ALL_BLACK) {
    memset(pixels, 0, pixelSize);
  } else {
    for (uint32_t i = 0; i < pixelSize; i++) {
      pixels[i] = (i & 1) ? 0x33 : 0xCC;
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

// CREATE-image ack wait. Faceclaw uses ~2 s; we allow extra margin so a
// BLE stall or heartbeat contention right after SHUTDOWN settle does not
// false-fail the CREATE before the first Cmd=3 byte ships.
static constexpr uint32_t kImgCreateAckTimeoutMs = 3000;

// Per-image push-ack completion wait after the last Cmd=3 fragment is sent.
// Faceclaw uses 3.5 s; central link recovery + controller backlog after
// write retries can defer acks longer. 7 s keeps full-viewer / probe tiles
// from timing out while the stack is still catching up (see kImgFragGapMs).
static constexpr uint32_t kImgPushAckTimeoutMs = 7000;

// Inter-fragment gap during a Cmd=3 burst (between sendPbFragmented calls).
// kImgInFlightCap=3: 10 ms keeps ~2 frags in flight on a healthy link
// (lowered from 15 ms 2026-05-10 to reclaim per-frame ms in the camera
// stream — small win, but cheap to revert if rc=-1 spikes return).
// Tune up (e.g. 30) if rc=-1 spikes return under transient queue pressure
// (pairs with stepped write retries in sendEnvelopeNoMutex).
static constexpr uint32_t kImgFragGapMs = 10;

// In-burst envelope gap (within a single sendPbFragmented call) for
// image push only. The default sendPbFragmented gap is 15 ms — fine for
// short bursts (CREATE-list, REBUILD-text, …) and for image-only Cmd=3
// pushes. But COMPOUND CREATEs (list+image, text+image) make the
// firmware's plugin task slower to drain ATT WRITE_CMDs, and at 15 ms
// gap the BLE controller's L2CAP-CMD buffer (≈8–16 packets on ESP32)
// historically filled around envelope 15/17 of a 3800 B image fragment —
// observed 2026-05-07 in Q28/Q28L logs as `esp_ble_gattc_write_char
// rc=-1` at envelope 15/17, never recovering.
//
// Tuning history:
//   25 ms — original safe value, ~120 ms/frame overhead
//   20 ms (2026-05-08) — reclaimed ~110 ms/frame, envelope 15 at ~280 ms
//   15 ms (2026-05-10) — pushed lower after sendEnvelopeNoMutex got the
//                        rc=-1 50ms-retry path; the rare transient now
//                        recovers in-line instead of stranding the mutex.
//                        Cleared 57 frames at 96×96 with 0 phantom-acks
//                        and 0 retry log lines, so the steady-state push
//                        stream tolerates 15 ms even though compound
//                        CREATE was the original failure case (only 1
//                        compound CREATE per stream session, vs N×17
//                        envelopes per frame).
//   tiered (2026-05-12) — 15 ms still fails on larger frames (spinning_earth
//                        animation pack at 3800 B/fragment → envelope 17/17
//                        rc=-1, ImageRawResp timeout). Root cause is shared
//                        controller TX buffer under 3-connection load:
//                        sustained 15ms cadence drains slower than submitted
//                        when G2-L + G2-R + R1 all hold connections. Picking
//                        the gap per-fragment from `pbLen` keeps slime-sized
//                        pushes at full speed and only slows down on
//                        fragments large enough to stress the buffer.
//
// Selection: pickImgEnvelopeGapMs(pbLen) — see below. Image-push paths set
// this via gFragNextBurstDelayMs before each sendPbFragmented call so
// non-image bursts keep their faster cadence.
static constexpr uint32_t kImgEnvGapMs_Small  = 15;   // ≤ ~6 envelopes
static constexpr uint32_t kImgEnvGapMs_Medium = 20;   // 7-12 envelopes
static constexpr uint32_t kImgEnvGapMs_Large  = 25;   // 13+ envelopes
static constexpr size_t   kImgFragSizeSmallMaxB  = 1500;  // bytes ≈ 6 env @ MTU 244
static constexpr size_t   kImgFragSizeMediumMaxB = 3000;  // bytes ≈ 12 env

// Backward-compat alias — the small-tier default. Tier-aware call sites
// should use pickImgEnvelopeGapMs(pbLen) instead so they auto-scale.
static constexpr uint32_t kImgInBurstEnvelopeGapMs = kImgEnvGapMs_Small;

// Pick the inter-envelope gap for one upcoming image-push fragment based
// on its serialized protobuf size. Small fragments fit inside the
// controller's TX buffer window and never stress it — keep the fast 15 ms
// gap. Larger fragments sustain pressure long enough across many
// connection events that the buffer drain rate (shared with the other two
// connections) can't keep up at 15 ms; bump to 20/25 ms to widen the
// window. Failure mode at undersize gap: rc=-1 from writeValue mid-burst,
// ImageRawResp never arrives, the next burst inherits the wedge.
static uint32_t pickImgEnvelopeGapMs(size_t pbLen) {
  if (pbLen <= kImgFragSizeSmallMaxB)  return kImgEnvGapMs_Small;
  if (pbLen <= kImgFragSizeMediumMaxB) return kImgEnvGapMs_Medium;
  return kImgEnvGapMs_Large;
}

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
// kImgFragGapMs=15 the in-flight count actually pipelines (frag N+1
// goes out before frag N's ack arrives), so the cap now bounds real
// work rather than just being a safety net.
static constexpr unsigned kImgInFlightCap = 3;

// Plain SHUTDOWN+settle of the active container — no FSM events fired.
// Use when the caller is NOT a probe (e.g. live-text page setup needs
// to drop the hijack list before CREATEing a TEXT widget without
// pretending to be an image probe). Probe callers should use
// probeTearDownActiveContainer instead, which wraps this in
// imageProbeBegin() so the echo-suppression predicate ignores the
// firmware's DISPLAY_OFF / SYSTEM_EXIT events that follow our SHUTDOWN.
static bool tearDownActiveContainer(G2Temple& arm) {
  if (!arm.containerReady) {
    DEBUG_G2F("[G2] teardown: no live container, skipping shutdown");
    return true;
  }
  noteOurShutdownSent();
  uint8_t buf[32];
  size_t n = g2BuildShutdown(allocSeq(), G2_MAGIC_SHUTDOWN,
                             /*exitMode*/ 0, buf, sizeof(buf));
  if (n == 0 || !sendEnvelope(arm, buf, n)) {
    DEBUG_G2F("[G2] teardown: SHUTDOWN send failed");
    return false;
  }
  DEBUG_G2F("[G2] teardown: SHUTDOWN sent, settling %u ms",
            (unsigned)kImgShutdownSettleMs);
  vTaskDelay(pdMS_TO_TICKS(kImgShutdownSettleMs));
  arm.containerReady  = false;
  arm.containerIsList = false;
  g2LensClearContainer();
  return true;
}

static bool probeTearDownActiveContainer(G2Temple& arm) {
  // Phase 3: this is the canonical "a probe is starting" chokepoint.
  // Every probe calls it as its first step; the FSM transitions
  // Hijacked -> ImageProbing here, so the echo-suppression predicate
  // ignores the firmware's DISPLAY_OFF / SYSTEM_EXIT during teardown.
  imageProbeBegin();
  return tearDownActiveContainer(arm);
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
  noteOurShutdownSent();
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
  // Phase 3: probe lifecycle complete. FSM transitions
  // ImageProbing -> Hijacked here. Place this AFTER the legacy state
  // mutations so a state-aware verify (Phase 5) sees the post-write
  // reality.
  imageProbeEnd();
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

  noteOurShutdownSent();
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
// imgW/imgH: container dimensions declared in CREATE-image. Default 288×144
// matches the historical solo-image shape every probe used before Q19.
// Smaller dims (e.g. 96×96 for fast streaming) are caller's choice — the
// BMP they hand in must match. Mixed-widget probes (Q16-Q18) bypass this
// helper entirely via runMixedListImageProbe.
//
// tolerateMissedAcks: if > 0, the throttle watchdog tightens to ~500 ms
// and "phantom-acks" up to N missing acks per burst, letting the stream
// keep flowing through transient ack drops. Default 0 = strict (today's
// behavior: if the in-flight cap blocks for >7 s, abort the whole burst).
// Streaming callers set this to ~3 (g2-kit-unofficial's tolerance value);
// single-shot callers leave it at 0 because the user expects this exact
// frame to render. See docs/G2_PROTOCOL.md "Windowed Cmd=3" for context.
static bool sendImageBmpMultiFragment(G2Temple& arm,
                                      const char* tag,
                                      uint32_t createMagic,
                                      uint32_t pushMagicBase,
                                      const uint8_t* bmp,
                                      size_t bmpLen,
                                      unsigned* outOkFrags,
                                      unsigned* outTotalFrags,
                                      unsigned tolerateMissedAcks = 0,
                                      int32_t imgW = 288,
                                      int32_t imgH = 144) {
  if (outOkFrags) *outOkFrags = 0;
  if (outTotalFrags) *outTotalFrags = 0;
  if (!bmp || bmpLen == 0) return false;

  const uint32_t kCID        = 2;
  const char*    kCName      = "imgQ4";
  const int32_t  kImgW       = imgW;
  const int32_t  kImgH       = imgH;
  const size_t   kChunkBytes = G2_IMG_MAPRAW_CHUNK_BYTES;

  const unsigned kFrags = (unsigned)((bmpLen + kChunkBytes - 1) / kChunkBytes);
  if (outTotalFrags) *outTotalFrags = kFrags;

  uint8_t createBuf[128];
  size_t createLen = g2BuildCreateImage(
      allocSeq(), createMagic, kCName, kCID, 0, 0, (uint32_t)kImgW, (uint32_t)kImgH,
      BLOCKS_WIDGET_ID, createBuf, sizeof(createBuf));
  if (createLen == 0) return false;

  noteOurShutdownSent();
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

  uint8_t* pbBuf = (uint8_t*)ps_alloc(kChunkBytes + 96, AllocPref::PreferPSRAM, "g2.imgpush.pbBuf");
  if (!pbBuf) return false;

  unsigned okFrags = 0;
  unsigned acksMissed = 0;  // bumped each time we phantom-ack past a stuck throttle
  size_t off = 0;
  const uint32_t burstStartMs = millis();
  bool aborted = false;
  // Stuck-throttle threshold. Strict mode (tolerateMissedAcks=0): wait
  // kImgPushAckTimeoutMs*2 (see constant) then abort. Tolerant mode
  // (tolerateMissedAcks>0): 500 ms then phantom-ack and continue, so a
  // streaming caller doesn't stall a frame waiting for one stray ack.
  const uint32_t stuckTimeoutMs = (tolerateMissedAcks > 0) ? 500 : (kImgPushAckTimeoutMs * 2);
  for (unsigned i = 0; i < kFrags && !aborted; i++) {
    // Honour user dismiss mid-burst. Without this, a Q13 frame in
    // progress finishes its full BMP push (and waits for acks) before
    // the worker's between-frame poll catches the dismiss — visibly
    // unresponsive.
    if (gImgProbeAbort) {
      DEBUG_G2F("[ImgProbe] %s aborted by user dismiss at frag %u/%u",
                tag, i + 1, kFrags);
      aborted = true;
      break;
    }
    // Sliding-window throttle. Hold off if we'd exceed the firmware's
    // reassembly window of kImgInFlightCap unacked fragments.
    // In-flight count = (fragments sent so far) - (acks received + phantom-acks).
    uint32_t throttleStartMs = millis();
    while (i > (gImgPushAcked + acksMissed + kImgInFlightCap - 1u)) {
      if (gImgProbeAbort) {
        DEBUG_G2F("[ImgProbe] %s aborted in throttle at %u acked / %u sent",
                  tag, (unsigned)gImgPushAcked, i);
        aborted = true;
        break;
      }
      if ((millis() - throttleStartMs) > stuckTimeoutMs) {
        if (acksMissed < tolerateMissedAcks) {
          acksMissed++;
          DEBUG_G2F("[ImgProbe] %s phantom-ack %u/%u (acked=%u, sent=%u) — keep stream flowing",
                    tag, acksMissed, tolerateMissedAcks, (unsigned)gImgPushAcked, i);
          throttleStartMs = millis();  // reset for next phantom window
          continue;
        }
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

    noteOurShutdownSent();
    DEBUG_G2F("[ImgProbe] %s frag %u/%u magic=%u fragIdx=%u packet=%u (offset=%u/%u, in-flight=%u)",
              tag, i + 1, kFrags, (unsigned)magic, i,
              (unsigned)chunk, (unsigned)off, (unsigned)bmpLen,
              (unsigned)(i + 1u - gImgPushAcked));
    // Pace this burst's envelopes based on fragment size — see the
    // tier picker comment above kImgEnvGapMs_*. Small fragments keep
    // the fast 15 ms cadence; large fragments (animation packs etc.)
    // get 20-25 ms so the controller TX buffer has time to drain
    // between submissions.
    g2DebugSetNextBurstFragDelay(pickImgEnvelopeGapMs(pbLen));
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

  // Block for the full ack set (kImgPushAckTimeoutMs). Returns false if any
  // expected ack didn't arrive — canonical "image probably didn't render".
  unsigned ackedCount = 0;
  unsigned ackTarget = 0;
  bool allAcked = false;
  if (aborted) {
    // User dismissed mid-burst: disarm push-ack tracking immediately.
    // Waiting out probeWaitImagePushAcks (multi-second) made the ring feel
    // like it needed a second double-tap and could overlap teardown with
    // follow-up BLE work.
    ackedCount = gImgPushAcked;
    ackTarget  = gImgPushTarget;
    gImgPushTarget = 0;
    if (gImgPushAckSem) {
      (void)xSemaphoreTake(gImgPushAckSem, 0);
    }
    DEBUG_G2F("[ImgProbe] %s push ack wait skipped (user dismiss) — had %u/%u",
              tag, ackedCount, ackTarget);
  } else {
    allAcked = probeWaitImagePushAcks(kImgPushAckTimeoutMs,
                                       &ackedCount, &ackTarget);
  }
  // Tolerant mode: phantom-acks count toward the success threshold. If
  // (real acks + phantom acks) covers the target, we treat it as OK and
  // let the streaming caller keep going. Strict mode (tolerateMissedAcks=0)
  // collapses to the original `allAcked` check since acksMissed stays 0.
  const bool sufficientlyAcked = (ackedCount + acksMissed) >= ackTarget;
  const uint32_t burstMs = millis() - burstStartMs;
  const char* status =
      aborted ? "aborted"
              : (allAcked ? "OK"
                          : (sufficientlyAcked ? "OK (with phantom-acks)"
                                               : "ACK TIMEOUT"));
  DEBUG_G2F("[ImgProbe] %s push complete: %u/%u sent, %u/%u acked (+%u phantom) in %u ms (%s)",
            tag, okFrags, kFrags, ackedCount, ackTarget,
            acksMissed, (unsigned)burstMs, status);
  return okFrags == kFrags && sufficientlyAcked;
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
// the ~4 KB per-Cmd=3 reassembly cap. Has to be split into MapRawData
// chunks (G2_IMG_MAPRAW_CHUNK_BYTES) and shipped as a sequential image-layer session: same
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
// Each Cmd=3 has ~50 B of pb overhead on top of MapRawData; G2_IMG_MAPRAW_CHUNK_BYTES
// keeps the full Cmd=3 pb body under the ~4 KB firmware reassembly cap.
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
  const size_t   kChunkBytes     = G2_IMG_MAPRAW_CHUNK_BYTES;

  probeBanner("Q6: multi-fragment full-tile BMP (288×144)",
              kCreateMagic, kPushMagicBase + 7,
              "expect Cmd=1 res=0 then a stream of Cmd=4 acks (one per "
              "fragment) all ErrorCode=4. Total ~20.8 KB BMP shipped as "
              "~6 chunks. Throughput ceiling per Discord is ~2.5 s for "
              "a full-tile render.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q6: pre-burst SHUTDOWN failed";

  // Step 1: build the full-tile BMP into the heap. ~21 KB so we can't
  // stack-allocate it.
  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmp");
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
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmp");
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
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmp");
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
// tolerateMissedAcks: see sendImageBmpMultiFragment for semantics.
static bool sendImageBmpFragmentsNoCreate(G2Temple& arm,
                                          const char* tag,
                                          uint32_t pushMagicBase,
                                          uint32_t cid,
                                          const char* cname,
                                          const uint8_t* bmp, size_t bmpLen,
                                          unsigned* outOkFrags,
                                          unsigned* outTotalFrags,
                                          unsigned tolerateMissedAcks = 0) {
  if (outOkFrags) *outOkFrags = 0;
  if (outTotalFrags) *outTotalFrags = 0;
  if (!bmp || bmpLen == 0 || !cname) return false;

  const uint32_t kCID    = cid;
  const char*    kCName  = cname;
  const size_t   kChunkBytes = G2_IMG_MAPRAW_CHUNK_BYTES;
  const unsigned kFrags = (unsigned)((bmpLen + kChunkBytes - 1) / kChunkBytes);
  if (outTotalFrags) *outTotalFrags = kFrags;

  // Same ack-tracking pattern as sendImageBmpMultiFragment, minus the
  // CREATE step. Each push in the new session gets its own magic in
  // [pushMagicBase, pushMagicBase + kFrags - 1]; we count Cmd=4 acks
  // in that range and block for completion at the end.
  probePrepImagePushAcks(pushMagicBase, pushMagicBase + kFrags - 1, kFrags);

  uint8_t* pbBuf = (uint8_t*)ps_alloc(kChunkBytes + 96, AllocPref::PreferPSRAM, "g2.imgpush.pbBuf");
  if (!pbBuf) return false;

  unsigned okFrags = 0;
  unsigned acksMissed = 0;
  size_t off = 0;
  const uint32_t burstStartMs = millis();
  bool aborted = false;
  const uint32_t stuckTimeoutMs = (tolerateMissedAcks > 0) ? 500 : (kImgPushAckTimeoutMs * 2);
  for (unsigned i = 0; i < kFrags && !aborted; i++) {
    // Mid-burst dismiss check; see sendImageBmpMultiFragment for rationale.
    if (gImgProbeAbort) {
      DEBUG_G2F("[ImgProbe] %s aborted by user dismiss at frag %u/%u",
                tag, i + 1, kFrags);
      aborted = true;
      break;
    }
    // Same sliding-window throttle + phantom-ack mechanism as
    // sendImageBmpMultiFragment — see there for rationale.
    uint32_t throttleStartMs = millis();
    while (i > (gImgPushAcked + acksMissed + kImgInFlightCap - 1u)) {
      if (gImgProbeAbort) {
        DEBUG_G2F("[ImgProbe] %s aborted in throttle at %u acked / %u sent",
                  tag, (unsigned)gImgPushAcked, i);
        aborted = true;
        break;
      }
      if ((millis() - throttleStartMs) > stuckTimeoutMs) {
        if (acksMissed < tolerateMissedAcks) {
          acksMissed++;
          DEBUG_G2F("[ImgProbe] %s phantom-ack %u/%u (acked=%u, sent=%u) — keep stream flowing",
                    tag, acksMissed, tolerateMissedAcks, (unsigned)gImgPushAcked, i);
          throttleStartMs = millis();
          continue;
        }
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
    noteOurShutdownSent();
    DEBUG_G2F("[ImgProbe] %s frag %u/%u magic=%u fragIdx=%u packet=%u (offset=%u/%u, in-flight=%u)",
              tag, i + 1, kFrags, (unsigned)magic, i,
              (unsigned)chunk, (unsigned)off, (unsigned)bmpLen,
              (unsigned)(i + 1u - gImgPushAcked));
    // Pace this burst's envelopes based on fragment size — see the
    // tier picker comment above kImgEnvGapMs_*. Small fragments keep
    // the fast 15 ms cadence; large fragments (animation packs etc.)
    // get 20-25 ms so the controller TX buffer has time to drain
    // between submissions.
    g2DebugSetNextBurstFragDelay(pickImgEnvelopeGapMs(pbLen));
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
  bool allAcked = false;
  if (aborted) {
    ackedCount = gImgPushAcked;
    ackTarget  = gImgPushTarget;
    gImgPushTarget = 0;
    if (gImgPushAckSem) {
      (void)xSemaphoreTake(gImgPushAckSem, 0);
    }
    DEBUG_G2F("[ImgProbe] %s push ack wait skipped (user dismiss) — had %u/%u",
              tag, ackedCount, ackTarget);
  } else {
    allAcked = probeWaitImagePushAcks(kImgPushAckTimeoutMs,
                                       &ackedCount, &ackTarget);
  }
  const bool sufficientlyAcked = (ackedCount + acksMissed) >= ackTarget;
  const uint32_t burstMs = millis() - burstStartMs;
  const char* status =
      aborted ? "aborted"
              : (allAcked ? "OK"
                          : (sufficientlyAcked ? "OK (with phantom-acks)"
                                               : "ACK TIMEOUT"));
  DEBUG_G2F("[ImgProbe] %s push complete: %u/%u sent, %u/%u acked (+%u phantom) in %u ms (%s)",
            tag, okFrags, kFrags, ackedCount, ackTarget,
            acksMissed, (unsigned)burstMs, status);
  return okFrags == kFrags && sufficientlyAcked;
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
  uint8_t* bmpA = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmpA");
  uint8_t* bmpB = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmpB");
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
  EXT_RAM_BSS_ATTR static char ret[260];

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
  uint8_t* bmpA = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmpA");
  uint8_t* bmpBlack = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmpBlack");
  uint8_t* bmpB = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmpB");
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
  if (!VFS::existsGuarded(path, currentAuthContext())) { if (outErr) *outErr = "file not found"; return false; }

  File f = VFS::openGuarded(path, FILE_READ, currentAuthContext());
  if (!f || !f.available()) { if (outErr) *outErr = "failed to open file"; return false; }
  const size_t len = (size_t)f.size();
  const size_t kMaxBmpBytes = 65536;
  if (len < 70) { f.close(); if (outErr) *outErr = "file too small for BMP"; return false; }
  if (len > kMaxBmpBytes) { f.close(); if (outErr) *outErr = "BMP too large (>64KB)"; return false; }

  uint8_t* buf = (uint8_t*)ps_alloc(len, AllocPref::PreferPSRAM, "g2.bmp.fileLoad");
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

// Q25: directory path for frame_XX.bmp sequence (set before worker spawn).
static char s_q25PackDir[128];

bool g2ReadBmp4bppFromVfs(const char* vfsPath, uint8_t** outData, size_t* outLen,
                          int32_t* outW, int32_t* outH, const char** outErr) {
  if (outData) *outData = nullptr;
  if (outLen) *outLen = 0;
  if (outW) *outW = 0;
  if (outH) *outH = 0;
  if (outErr) *outErr = "unknown error";
  if (!vfsPath || !vfsPath[0]) {
    if (outErr) *outErr = "empty path";
    return false;
  }
  String path(vfsPath);
  path.trim();
  if (path.length() == 0) {
    if (outErr) *outErr = "empty path";
    return false;
  }
  if (!path.startsWith("/")) path = "/" + path;
  if (!VFS::existsGuarded(path, currentAuthContext())) {
    if (outErr) *outErr = "file not found";
    return false;
  }

  File f = VFS::openGuarded(path, FILE_READ, currentAuthContext());
  if (!f || !f.available()) {
    if (outErr) *outErr = "failed to open file";
    return false;
  }
  const size_t len = (size_t)f.size();
  const size_t kMaxBmpBytes = 65536;
  if (len < 70) {
    f.close();
    if (outErr) *outErr = "file too small for BMP";
    return false;
  }
  if (len > kMaxBmpBytes) {
    f.close();
    if (outErr) *outErr = "BMP too large (>64KB)";
    return false;
  }

  uint8_t* buf = (uint8_t*)ps_alloc(len, AllocPref::PreferPSRAM, "g2.bmp.anyTile");
  if (!buf) {
    f.close();
    if (outErr) *outErr = "out of memory reading BMP";
    return false;
  }
  const size_t rd = f.read(buf, len);
  f.close();
  if (rd != len) {
    free(buf);
    if (outErr) *outErr = "short read from BMP file";
    return false;
  }

  if (!(buf[0] == 'B' && buf[1] == 'M')) {
    free(buf);
    if (outErr) *outErr = "not a BMP (missing BM header)";
    return false;
  }
  const uint32_t dibSize = (uint32_t)buf[14] | ((uint32_t)buf[15] << 8) |
                           ((uint32_t)buf[16] << 16) | ((uint32_t)buf[17] << 24);
  if (dibSize < 40) {
    free(buf);
    if (outErr) *outErr = "unsupported BMP DIB header";
    return false;
  }

  const int32_t w = (int32_t)((uint32_t)buf[18] | ((uint32_t)buf[19] << 8) |
                              ((uint32_t)buf[20] << 16) | ((uint32_t)buf[21] << 24));
  const int32_t h = (int32_t)((uint32_t)buf[22] | ((uint32_t)buf[23] << 8) |
                              ((uint32_t)buf[24] << 16) | ((uint32_t)buf[25] << 24));
  const uint16_t planes = (uint16_t)buf[26] | ((uint16_t)buf[27] << 8);
  const uint16_t bpp = (uint16_t)buf[28] | ((uint16_t)buf[29] << 8);
  const uint32_t compression = (uint32_t)buf[30] | ((uint32_t)buf[31] << 8) |
                                 ((uint32_t)buf[32] << 16) | ((uint32_t)buf[33] << 24);
  if (planes != 1) {
    free(buf);
    if (outErr) *outErr = "invalid BMP planes";
    return false;
  }
  if (bpp != 4) {
    free(buf);
    if (outErr) *outErr = "only 4bpp BMP supported";
    return false;
  }
  if (compression != 0) {
    free(buf);
    if (outErr) *outErr = "compressed BMP not supported";
    return false;
  }
  const int32_t aw = (w < 0) ? -w : w;
  const int32_t ah = (h < 0) ? -h : h;
  if (aw < 2 || ah < 2 || aw > 288 || ah > 144) {
    free(buf);
    if (outErr) *outErr = "BMP size out of range (2..288 x 2..144)";
    return false;
  }

  if (outData) *outData = buf;
  if (outLen) *outLen = len;
  if (outW) *outW = w;
  if (outH) *outH = h;
  if (outErr) *outErr = "";
  return true;
}

void g2ProbeImageQ25SetPackPath(const char* dirPath) {
  s_q25PackDir[0] = '\0';
  if (!dirPath || !dirPath[0]) return;
  // Packs must live under G2_ICON_ANIMATIONS_VFS_PATH (no ".." traversal).
  if (strstr(dirPath, "..")) {
    DEBUG_G2F("[G2] Q25 pack path rejected (path traversal)");
    return;
  }
  const char* const root = G2_ICON_ANIMATIONS_VFS_PATH;
  const size_t rootLen = strlen(root);
  if (strncmp(dirPath, root, rootLen) != 0 || dirPath[rootLen] != '/') {
    DEBUG_G2F("[G2] Q25 pack path rejected (must be subdirectory of %s/)", root);
    return;
  }
  strncpy(s_q25PackDir, dirPath, sizeof(s_q25PackDir) - 1);
  s_q25PackDir[sizeof(s_q25PackDir) - 1] = '\0';
  size_t n = strlen(s_q25PackDir);
  while (n > 0 && (s_q25PackDir[n - 1] == '/' || s_q25PackDir[n - 1] == '\\')) {
    s_q25PackDir[--n] = '\0';
  }
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
  if (path.length() == 0) return "Error: invalid arguments — Usage: g2bmp </path/to/file.bmp> [brightness -100..100] [contrast -100..100] [holdSeconds 0..120]";
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
  if (!arm) return "Error: G2 BMP: no reachable temple";

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

  const unsigned estimatedFrags =
      (unsigned)((bmpLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) / G2_IMG_MAPRAW_CHUNK_BYTES);
  const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x20;  // 242
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x21;  // 243+
  if (kPushMagicBase + estimatedFrags >= 256) {
    free(bmp);
    return "Error: G2 BMP: too many fragments for magic window";
  }

  DEBUG_G2F("[G2] g2bmp: path='%s' bytes=%u dims=%dx%d frags=%u bright=%d contrast=%d hold=%ds",
            path.c_str(), (unsigned)bmpLen, (int)bmpW, (int)bmpH, estimatedFrags,
            brightness, contrast, holdSeconds);
  if (!probeTearDownActiveContainer(*arm)) {
    free(bmp);
    return "Error: G2 BMP: pre-push SHUTDOWN failed";
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
// Async BMP viewer — public API for UI consumers (Files page, future
// gallery, etc.). Mirrors cmd_g2bmp's transport but runs in a worker
// task with a completion callback so a tap dispatcher running on the
// BLE notify thread can launch it without blocking. Holds the image
// until the user double-taps (probe-hold pattern, 60 s safety cap)
// then teardowns and fires onDone so the caller can redraw its page.
// ─────────────────────────────────────────────────────────────────────
struct BmpViewerArgs {
  char* path;
  void (*onDone)();
};

static void g2BmpViewerWorker(void* arg) {
  // Install the paired-user identity + G2 notification source so
  // readBmpFromVfs's guarded reads succeed from this worker task's default
  // ANON TLS slot, and any notify*() fired during the read attributes to
  // "G2 / <user>" rather than "Unknown".
  G2HijackCtxGuard ctxGuard;
  auto* a = (BmpViewerArgs*)arg;

  do {
    G2Temple* arm = pickEvenAIArm("bmpView");
    if (!arm) {
      DEBUG_G2F("[G2] BMP viewer: no eligible arm");
      break;
    }

    const char* loadErr = "";
    uint8_t* bmp = nullptr;
    size_t bmpLen = 0;
    int32_t bmpW = 0, bmpH = 0;
    if (!readBmpFromVfs(String(a->path), &bmp, &bmpLen, &bmpW, &bmpH, &loadErr)) {
      DEBUG_G2F("[G2] BMP viewer: load failed for '%s': %s",
                a->path, loadErr ? loadErr : "?");
      break;
    }

    // Same magic range as cmd_g2bmp. The probe FSM gates concurrent
    // probes (only one can hold ImageProbing at a time), so reusing the
    // range with the CLI command is safe.
    const unsigned estFrags =
        (unsigned)((bmpLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) / G2_IMG_MAPRAW_CHUNK_BYTES);
    const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x20;  // 242
    const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x21;  // 243+
    if (kPushMagicBase + estFrags >= 256) {
      DEBUG_G2F("[G2] BMP viewer: too many fragments (%u) for magic window",
                estFrags);
      free(bmp);
      break;
    }

    DEBUG_G2F("[G2] BMP viewer: '%s' bytes=%u dims=%dx%d frags=%u",
              a->path, (unsigned)bmpLen, (int)bmpW, (int)bmpH, estFrags);

    if (!probeTearDownActiveContainer(*arm)) {
      DEBUG_G2F("[G2] BMP viewer: pre-push SHUTDOWN failed");
      free(bmp);
      break;
    }

    unsigned okFrags = 0, totalFrags = 0;
    (void)sendImageBmpMultiFragment(*arm, "bmpView",
                                    kCreateMagic, kPushMagicBase,
                                    bmp, bmpLen, &okFrags, &totalFrags);
    free(bmp);

    DEBUG_G2F("[G2] BMP viewer: image up — double-tap to dismiss (60 s cap)");
    const bool tapped = probeHoldUntilTapOrTimeout(60000);
    DEBUG_G2F("[G2] BMP viewer: hold ended via %s (frags %u/%u)",
              tapped ? "user tap" : "60 s timeout", okFrags, totalFrags);

    probePostProbeShutdown(*arm);
  } while (0);

  if (a->onDone) a->onDone();
  free(a->path);
  delete a;
  vTaskDelete(nullptr);
}

bool g2ShowBmpFile(const char* path, void (*onDone)()) {
  if (!path || !*path) return false;
  // Heap-low guard — the worker stack and the BMP buffer together can
  // run a couple of tens of KB. Decline if internal heap is already
  // tight rather than reboot mid-push.
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] BMP viewer: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new BmpViewerArgs;
  if (!a) return false;
  a->path   = strdup(path);
  a->onDone = onDone;
  if (!a->path) {
    delete a;
    return false;
  }
  // 6 KB stack — readBmpFromVfs heap-allocates the BMP, so the worker
  // mainly carries small probe-state buffers on the stack.
  if (xTaskCreate(g2BmpViewerWorker, "g2_bmp_view", 6144, a,
                  tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] BMP viewer: xTaskCreate failed");
    free(a->path);
    delete a;
    return false;
  }
  return true;
}

#if ENABLE_MAPS
// ─────────────────────────────────────────────────────────────────────
// Map viewer — render the loaded offline map HEADLESSLY at the lens's
// native 288×144 into a byte-per-pixel shade buffer (the shared
// OffscreenMapRenderer draws 0-15 per pixel: brightness = feature
// class), pack shades 1:1 into a 288×144 4-bpp grayscale BMP, and push
// it to one lens over the same image transport as g2bmp/camera. Pure
// downstream consumer. Native-res rendering replaced the old 128×64
// 1-bit page + 2.25× nearest-neighbour upscale, so lines are crisp
// (uniform 1 px) instead of alternately 2-and-3 px wide.
// ─────────────────────────────────────────────────────────────────────

// Pack a 288×144 shade buffer (one byte per pixel, values 0..15,
// row-major) into a 288×144 4-bpp top-down BMP. 288×144 is the
// HW-proven native lens-tile size, and the container bytes are
// IDENTICAL to what the proven camera path (buildBmp4bppFromRgb888)
// emits: same 118-byte header, same 16-shade gray palette, same
// 4-byte-aligned 144-byte row stride, same 4-bpp packing (high nibble
// = even column, low = odd). Only the source of the nibbles changed:
// shade values map straight to palette indices instead of 0x0/0xF bits.
static size_t buildMapBmp4bpp288x144FromShades(uint8_t* out, size_t outCap,
                                               const uint8_t* shades) {
  const int32_t  dstW       = 288;
  const int32_t  dstH       = 144;
  const uint32_t rowStride  = ((uint32_t)dstW * 4 + 31) / 32 * 4;  // = 144
  const uint32_t pixelSize  = rowStride * (uint32_t)dstH;
  const uint32_t headerSize = 14 + 40 + 64;
  const uint32_t total      = headerSize + pixelSize;
  if (!out || !shades || total > outCap) return 0;

  auto wr16 = [](uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
  };
  auto wr32 = [](uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);          p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);  p[3] = (uint8_t)((v >> 24) & 0xff);
  };

  // BITMAPFILEHEADER (14 B)
  out[0] = 'B'; out[1] = 'M';
  wr32(out + 2,  total);
  wr16(out + 6,  0);
  wr16(out + 8,  0);
  wr32(out + 10, headerSize);
  // BITMAPINFOHEADER (40 B) — negative biHeight => top-down
  wr32(out + 14, 40);
  wr32(out + 18, (uint32_t)dstW);
  wr32(out + 22, (uint32_t)(-dstH));
  wr16(out + 26, 1);
  wr16(out + 28, 4);              // biBitCount = 4-bpp
  wr32(out + 30, 0);              // BI_RGB
  wr32(out + 34, pixelSize);
  wr32(out + 38, 2835);
  wr32(out + 42, 2835);
  wr32(out + 46, 16);
  wr32(out + 50, 0);
  // 16-shade grayscale palette (BGRA), matching buildBmp4bpp.
  for (int i = 0; i < 16; i++) {
    const uint8_t v = (uint8_t)((i * 255) / 15);
    out[54 + i*4 + 0] = v;
    out[54 + i*4 + 1] = v;
    out[54 + i*4 + 2] = v;
    out[54 + i*4 + 3] = 0;
  }

  // Pixels: shade bytes map 1:1 to 4-bpp palette indices (no scaling).
  // memset clears each row first so the (already 4-byte-aligned) stride
  // has no stale bytes.
  uint8_t* px = out + headerSize;
  for (int32_t dy = 0; dy < dstH; dy++) {
    const uint8_t* srcRow = shades + (size_t)dy * dstW;
    uint8_t* dstRow = px + (size_t)dy * rowStride;
    memset(dstRow, 0, rowStride);
    for (int32_t dx = 0; dx < dstW; dx++) {
      const uint8_t nib = srcRow[dx] & 0x0F;
      if ((dx & 1) == 0) dstRow[dx >> 1] |= (uint8_t)(nib << 4);  // even → high
      else               dstRow[dx >> 1] |= nib;                  // odd  → low
    }
  }
  return total;
}

struct MapPageArgs {
  void (*onDone)();
};

// Render the CURRENT shared map state (gMapCenter*, gMapZoom, gMapRotation,
// gVisibleLayers) into a 288×144 4-bpp BMP for the lens. Initializes the
// center to the loaded map's midpoint the first time (mirrors the OLED map's
// first-open behaviour). Returns the BMP length, or 0 on failure.
static size_t g2RenderCurrentMapBmp(uint8_t* bmp, size_t cap) {
  if (!MapCore::hasValidMap()) return 0;
  const LoadedMap& m = MapCore::getCurrentMap();
  extern float gMapCenterLat;
  extern float gMapCenterLon;
  extern bool  gMapCenterSet;
  if (!gMapCenterSet) {
    gMapCenterLat = (m.header.minLat + m.header.maxLat) / 2000000.0f;
    gMapCenterLon = (m.header.minLon + m.header.maxLon) / 2000000.0f;
    gMapCenterSet = true;
  }
  constexpr int kLensW = 288, kLensH = 144;
  uint8_t* shades = (uint8_t*)ps_alloc((size_t)kLensW * kLensH,
                                       AllocPref::PreferPSRAM, "g2.map.shade");
  if (!shades) return 0;
  OffscreenMapRenderer r(shades, kLensW, kLensH, kLensW, kLensH, 0);
  r.setSurfaceAreas(true);   // G2 lens: also surface water bodies + coastlines
  r.clear();
  // zoom/rotation/layers come from the shared globals via the params snapshot.
  // The viewport is 2.25× the OLED's 128×64, and renderMap's px scale is
  // zoom-only, so multiply zoom by 2.25 to keep the SAME geographic framing
  // as the OLED at the same gMapZoom — just rendered at native lens density.
  // LOD also sees the scaled zoom, so the denser display legitimately shows
  // more detail (e.g. buildings appear at gMapZoom ~0.9 instead of 2.0).
  MapRenderParams params = mapRenderParamsFromGlobals();
  params.zoom *= (float)kLensW / 128.0f;
  MapCore::renderMap(&r, gMapCenterLat, gMapCenterLon, params);
  const size_t len = buildMapBmp4bpp288x144FromShades(bmp, cap, shades);
  free(shades);
  return len;
}

// ─────────────────────────────────────────────────────────────────────
// Interactive Maps page — a list+image compound (control list on the LEFT,
// 288×144 map image on the RIGHT), mirroring the camera-stream page. Each
// control tap mutates the shared map state and re-pushes just the image
// (Cmd=3) into the existing container — the list stays put. Lives until the
// user taps "<- Back". NOTE: a REBUILD-list against a live list+image
// compound silently drops the image on this firmware, so the list labels are
// static (no on/off state shown) — the feedback is the map itself changing.
// ─────────────────────────────────────────────────────────────────────
static void g2MapPageWorker(void* arg) {
  // Paired-user identity + G2 notify source so tile-loading FS reads succeed.
  G2HijackCtxGuard ctxGuard;
  auto* a = (MapPageArgs*)arg;

  // Control rows. Bit index in gMapPagePendingTap == row index (see the
  // 'lstMap' dispatch in the BLE notify handler).
  static const char* const kRows[] = {
    "<- Back",     // 0
    "Zoom In",     // 1
    "Zoom Out",    // 2
    "Reset View",  // 3
    "Recenter",    // 4
  };
  static constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

  do {
    G2Temple* arm = pickEvenAIArm("mapPage");
    if (!arm) { DEBUG_G2F("[G2] map page: no eligible arm"); break; }

    // Ensure a map is loaded; auto-load the first available.
    if (!MapCore::hasValidMap()) {
      char maps[8][96];
      const int n = MapCore::getAvailableMaps(maps, 8);
      if (n <= 0) { DEBUG_G2F("[G2] map page: no maps on device (/maps/*.hwmap)"); break; }
      char p[128];
      snprintf(p, sizeof(p), "/maps/%s", maps[0]);
      if (!MapCore::loadMapFile(p)) { DEBUG_G2F("[G2] map page: load failed for '%s'", p); break; }
    }

    // Tear down the Apps list, then CREATE the list+image compound once.
    if (!probeTearDownActiveContainer(*arm)) {
      DEBUG_G2F("[G2] map page: pre-page SHUTDOWN failed");
      break;
    }
    // Clear stale image-probe state so a leftover flag can't terminate us
    // (matches the camera-stream setup).
    gImgProbeHoldTapPending = false;
    gImgProbeAbort          = false;
    gImgProbeHoldActive     = false;
    gMapPagePendingTap      = 0;

    // Layout: control list on the LEFT, 288×144 map image on the RIGHT.
    const G2ContainerGeom kListGeom = { 8, 8, 264, 272 };
    G2ImageTile imgTile = {};
    imgTile.x = 280;
    imgTile.y = 72;      // vertically center the 144-tall image in the lens
    imgTile.w = 288;
    imgTile.h = 144;
    imgTile.containerId   = 2;
    imgTile.containerName = "imgMap";

    const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x20;  // 242
    const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x21;  // 243+ (6 frags → 243..248)
    {
      uint8_t createBuf[1024];
      const size_t createLen = g2BuildCreateMixedListImage(
          allocSeq(), kCreateMagic,
          /*listName*/ "lstMap",
          kRows, kRowCount, kListGeom,
          imgTile, BLOCKS_WIDGET_ID,
          createBuf, sizeof(createBuf));
      if (createLen == 0) { DEBUG_G2F("[G2] map page: CREATE-mixed build failed"); break; }
      probePrepImageCreateAck(kCreateMagic);
      if (!sendEnvelope(*arm, createBuf, createLen)) {
        DEBUG_G2F("[G2] map page: CREATE-mixed TX failed");
        break;
      }
      if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs)) {
        DEBUG_G2F("[G2] map page: CREATE-mixed ack timeout");
        probePostProbeShutdown(*arm);
        break;
      }
      DEBUG_G2F("[G2] map page: CREATE-mixed acked (list@'lstMap' + image@'imgMap' 288x144)");
    }

    gMapPageActive = true;   // BLE handler now routes lstMap taps to us
    bool dirty = true;       // render the initial frame

    while (true) {
      if (!gLens.hijackActive) { DEBUG_G2F("[G2] map page: hijack ended"); break; }
      if (gImgProbeAbort)      { DEBUG_G2F("[G2] map page: abort flag set"); break; }

      // Drain control taps (bit == row index). Read-and-clear.
      const uint32_t taps = gMapPagePendingTap;
      gMapPagePendingTap = 0;
      if (taps) {
        extern float gMapZoom;
        extern float gMapRotation;
        extern float gMapCenterLat;
        extern float gMapCenterLon;
        extern bool  gMapCenterSet;
        if (taps & (1u << 0)) { DEBUG_G2F("[G2] map page: Back → exit"); break; }
        if (taps & (1u << 1)) { gMapZoom *= 1.5f; if (gMapZoom > 30.0f) gMapZoom = 30.0f; dirty = true; }
        if (taps & (1u << 2)) { gMapZoom /= 1.5f; if (gMapZoom < 0.20f) gMapZoom = 0.20f; dirty = true; }
        if (taps & (1u << 3)) { gMapZoom = 1.0f; gMapRotation = 0.0f; dirty = true; }
        if (taps & (1u << 4)) {  // Recenter to the loaded map's midpoint
          const LoadedMap& cm = MapCore::getCurrentMap();
          gMapCenterLat = (cm.header.minLat + cm.header.maxLat) / 2000000.0f;
          gMapCenterLon = (cm.header.minLon + cm.header.maxLon) / 2000000.0f;
          gMapCenterSet = true;
          dirty = true;
        }
      }

      if (dirty) {
        const size_t bmpCap = 14 + 40 + 64 + (288 / 2) * 144 + 64;
        uint8_t* bmp = (uint8_t*)ps_alloc(bmpCap, AllocPref::PreferPSRAM, "g2.map.bmp");
        if (bmp) {
          const size_t bmpLen = g2RenderCurrentMapBmp(bmp, bmpCap);
          if (bmpLen > 0) {
            // Push into the existing image child (no re-CREATE). Blocks for
            // full ack (tolerate=0) so pushes never overlap → reusing the
            // same magic range each frame is safe.
            unsigned okFrags = 0, totalFrags = 0;
            (void)sendImageBmpFragmentsNoCreate(*arm, "mapPage",
                                                kPushMagicBase,
                                                imgTile.containerId, imgTile.containerName,
                                                bmp, bmpLen, &okFrags, &totalFrags,
                                                /*tolerateMissedAcks*/ 0);
            DEBUG_G2F("[G2] map page: image pushed (%u/%u frags)", okFrags, totalFrags);
          } else {
            DEBUG_G2F("[G2] map page: render failed (no valid map?)");
          }
          free(bmp);
        }
        dirty = false;
      }

      vTaskDelay(pdMS_TO_TICKS(120));   // poll for taps between renders
    }

    gMapPageActive     = false;
    gMapPagePendingTap = 0;
    probePostProbeShutdown(*arm);
  } while (0);

  if (a->onDone) a->onDone();
  delete a;
  vTaskDelete(nullptr);
}

// Public entry — spawn the interactive map-page worker. Stack matches the
// OLED map render task (renderMap is the stack-heavy step; the image push
// runs after it returns each frame, so max-not-sum).
static bool g2ShowMapPage(void (*onDone)()) {
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] map page: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new MapPageArgs;
  if (!a) return false;
  a->onDone = onDone;
  if (xTaskCreate(g2MapPageWorker, "g2_map_page", MAP_RENDER_STACK_WORDS, a,
                  tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] map page: xTaskCreate failed");
    delete a;
    return false;
  }
  return true;
}

static const char* cmd_g2map(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (!isG2Connected()) return "Error: G2 map: not connected";
  if (!g2ShowMapPage(nullptr)) return "Error: G2 map: busy or heap low — try again";
  return "G2 map: opening interactive map page — tap a control on the lens; Back to exit.";
}
#endif // ENABLE_MAPS

// ─────────────────────────────────────────────────────────────────────
// Camera viewer — capture one JPEG, decode to RGB888 via
// img_converters.h::fmt2rgb888, downsample-and-quantize into a 288×144
// 4-bpp grayscale BMP, push using the same single-tile transport as
// g2ShowBmpFile, then hold until the user double-taps. One-shot: no
// auto-refresh and no single-tap recapture (the firmware doesn't emit
// CLICK on image-only state — only DOUBLE_CLICK fires).
//
// Memory profile (peak, all in PSRAM):
//   JPEG copy        ~10–30 KB   (camera dependent, freed after decode)
//   RGB888 buffer    sw·sh·3     (e.g. QVGA 320×240 → 230 KB; freed
//                                 after BMP build)
//   4-bpp BMP        ~21 KB      (held until push completes)
// ─────────────────────────────────────────────────────────────────────

// img_converters.h (fmt2rgb888) and the buildBmp4bppFromRgb888 helper
// below are NOT camera-sensor-specific. The esp32-camera component — and
// its software conversions library — is linked unconditionally (see the
// component CMakeLists REQUIRES list), so JPEG software-decode works on
// every board whether or not a physical sensor is present. Only the
// live-sensor code (capture/stream, further down) is gated on
// ENABLE_CAMERA_SENSOR.
#include "img_converters.h"

// Build a 288×144 4-bpp top-down grayscale BMP at `out` from a
// `srcW`×`srcH` RGB888 buffer at `src`. Sampling is nearest-neighbour
// (one source pixel per destination pixel) — fast and good enough for
// the 288×144 lens window. Returns total bytes written, or 0 on
// failure (capacity / dim).
//
// `toneMap`:
//   None        — quantize raw luma directly (gray >> 4). Preserves source
//                 levels but washed-out frames map to a narrow nibble range
//                 (e.g. mid-grays only), which on a green-tinted G2 panel
//                 means everything shows similar brightness.
//   AutoLevels  — pre-pass over the fit region to find actual luma min/max,
//                 then linearly remap [min..max] → [0..255] before
//                 quantizing. Costs one extra fit-region scan (~1 ms at
//                 192×144 dst). Skipped when range is below kAutoLevelsMinRange
//                 to avoid amplifying noise in near-uniform frames.
enum class BmpToneMap : uint8_t {
  None       = 0,
  AutoLevels = 1,
};

static size_t buildBmp4bppFromRgb888(uint8_t* out, size_t outCap,
                                     const uint8_t* src,
                                     int32_t srcW, int32_t srcH,
                                     int32_t dstW = 288,
                                     int32_t dstH = 144,
                                     BmpToneMap toneMap = BmpToneMap::None) {
  if (!out || !src) return 0;
  if (srcW <= 0 || srcH <= 0) return 0;
  if (dstW <= 0 || dstH <= 0) return 0;
  const uint32_t rowStride  = ((uint32_t)dstW * 4 + 31) / 32 * 4;  // 4-byte align
  const uint32_t pixelSize  = rowStride * (uint32_t)dstH;
  const uint32_t headerSize = 14 + 40 + 64;
  const uint32_t total      = headerSize + pixelSize;
  if (total > outCap) return 0;

  auto wr16 = [](uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
  };
  auto wr32 = [](uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);          p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);  p[3] = (uint8_t)((v >> 24) & 0xff);
  };

  // BITMAPFILEHEADER (14 B)
  out[0] = 'B'; out[1] = 'M';
  wr32(out + 2,  total);
  wr16(out + 6,  0);
  wr16(out + 8,  0);
  wr32(out + 10, headerSize);
  // BITMAPINFOHEADER (40 B) — biHeight negative => top-down
  wr32(out + 14, 40);
  wr32(out + 18, (uint32_t)dstW);
  wr32(out + 22, (uint32_t)(-dstH));
  wr16(out + 26, 1);
  wr16(out + 28, 4);
  wr32(out + 30, 0);
  wr32(out + 34, pixelSize);
  wr32(out + 38, 2835);
  wr32(out + 42, 2835);
  wr32(out + 46, 16);
  wr32(out + 50, 0);
  // 16-shade grayscale palette (BGRA)
  for (int i = 0; i < 16; i++) {
    const uint8_t v = (uint8_t)((i * 255) / 15);
    out[54 + i*4 + 0] = v;
    out[54 + i*4 + 1] = v;
    out[54 + i*4 + 2] = v;
    out[54 + i*4 + 3] = 0;
  }

  // Aspect-preserving fit (letterbox). The lens widget is 2:1 (288×144)
  // but every camera framesize is 4:3 or 1:1, so without correction the
  // image gets stretched ~1.5–2× horizontally. Compute the largest
  // fitW×fitH that preserves srcW:srcH and fits inside dstW×dstH, then
  // centre it. Pixels outside the fit region stay zero from the row
  // memset below — palette index 0 is black, so the gaps render as
  // black bars.
  //
  // Aspect comparison via integer cross-multiply to avoid floats:
  //   srcW/srcH  vs  dstW/dstH
  //   →  srcW*dstH  vs  srcH*dstW
  int32_t fitW, fitH;
  if ((int64_t)srcW * dstH > (int64_t)srcH * dstW) {
    // Source aspect wider than destination — width is the bound,
    // letterbox top + bottom.
    fitW = dstW;
    fitH = (int32_t)(((int64_t)srcH * dstW) / srcW);
  } else {
    // Source aspect taller-or-equal — height is the bound, letterbox
    // left + right. (This is the case for every standard camera
    // framesize against the 2:1 lens widget.)
    fitH = dstH;
    fitW = (int32_t)(((int64_t)srcW * dstH) / srcH);
  }
  const int32_t offX = (dstW - fitW) / 2;
  const int32_t offY = (dstH - fitH) / 2;

  // Tone-map pre-pass. When AutoLevels is requested, scan the fit region
  // to find true luma min/max, then in the writing loop below remap
  // [min..max] → [0..255] before quantizing to a 4-bpp nibble. This
  // recovers dynamic range from washed-out frames where the source luma
  // sits in (e.g.) 70..170 — without it those map to nibble bins 4..10,
  // i.e. only 7 distinct shades on the lens.
  //
  // tmMin/tmMax are the remap endpoints. When toneMap == None or the
  // range is too small to be useful, they stay at 0/255 so the remap
  // becomes a no-op (gray * 255 / 255 == gray).
  //
  // kAutoLevelsMinRange: floor on (max-min) below which auto-levels is
  // skipped. Below this you'd be amplifying sensor noise in a
  // near-uniform frame — looks worse than letting it stay flat.
  uint8_t tmMin = 0;
  uint8_t tmMax = 255;
  if (toneMap == BmpToneMap::AutoLevels) {
    constexpr int kAutoLevelsMinRange = 24;
    uint8_t lmin = 255, lmax = 0;
    for (int32_t dy = 0; dy < fitH; dy++) {
      const int32_t sy = (dy * srcH) / fitH;
      const uint8_t* srcRow = src + (size_t)sy * (size_t)srcW * 3;
      for (int32_t dx = 0; dx < fitW; dx++) {
        const int32_t sx = (dx * srcW) / fitW;
        const uint8_t* p = srcRow + (size_t)sx * 3;
        const uint32_t gray = ((uint32_t)p[0] + p[1] + p[2]) / 3;
        const uint8_t g8 = (uint8_t)(gray > 255 ? 255 : gray);
        if (g8 < lmin) lmin = g8;
        if (g8 > lmax) lmax = g8;
      }
    }
    if ((int)lmax - (int)lmin >= kAutoLevelsMinRange) {
      tmMin = lmin;
      tmMax = lmax;
    }
  }
  const int tmRange = (int)tmMax - (int)tmMin;

  // Pixel rows. esp32-camera fmt2rgb888 emits BGR order in RGB888 (the
  // function name is historical) but for grayscale conversion the
  // channel order doesn't matter — we average all three.
  //
  // We track gray range + a coarse sum across the fit region so the
  // caller can log whether the BMP actually has content. If
  // min==max==0 across the whole fit region, fmt2rgb888 produced a
  // zero buffer (silent decode failure or corrupt input JPEG) and the
  // image will render as solid black on the lens — that's the
  // diagnostic signal we need to distinguish "BMP fine, lens didn't
  // render" from "BMP all-black, fix the capture path."
  uint8_t  diagMin  = 255;
  uint8_t  diagMax  = 0;
  uint64_t diagSum  = 0;
  uint32_t diagSamp = 0;
  uint8_t* dstRowBase = out + headerSize;
  for (int32_t dy = 0; dy < dstH; dy++) {
    uint8_t* dstRow = dstRowBase + (size_t)dy * rowStride;
    memset(dstRow, 0, rowStride);

    // Top + bottom letterbox rows: leave entirely black.
    if (dy < offY || dy >= offY + fitH) continue;

    const int32_t dyFit = dy - offY;
    const int32_t sy = (dyFit * srcH) / fitH;
    const uint8_t* srcRow = src + (size_t)sy * (size_t)srcW * 3;

    // Sample only the fit-region columns; everything outside stays
    // black from the memset above.
    const int32_t dxStart = offX;
    const int32_t dxEnd   = offX + fitW;
    for (int32_t dx = dxStart; dx < dxEnd; dx++) {
      const int32_t dxFit = dx - offX;
      const int32_t sx = (dxFit * srcW) / fitW;
      const uint8_t* p = srcRow + (size_t)sx * 3;
      // Luma approximation — straight average is fine for monochrome
      // lens; weighted ITU-R BT.601 (0.299 R + 0.587 G + 0.114 B) is
      // marginally better but adds a 16-bit multiply per pixel.
      const uint32_t gray = ((uint32_t)p[0] + p[1] + p[2]) / 3;
      const uint8_t  gray8 = (uint8_t)(gray > 255 ? 255 : gray);
      if (gray8 < diagMin) diagMin = gray8;
      if (gray8 > diagMax) diagMax = gray8;
      diagSum  += gray8;
      diagSamp += 1;
      // Remap [tmMin..tmMax] -> [0..255]. When auto-levels is off or the
      // pre-pass found the range too small to stretch safely, tmMin=0
      // and tmMax=255 so this is a no-op.
      uint32_t mapped;
      if (tmRange > 0) {
        int v = (int)gray8 - (int)tmMin;
        if (v < 0) v = 0;
        mapped = ((uint32_t)v * 255U) / (uint32_t)tmRange;
        if (mapped > 255) mapped = 255;
      } else {
        mapped = gray8;
      }
      const uint8_t nibble = (uint8_t)(mapped >> 4);  // 0..15
      // 4-bpp packing: high nibble = even column, low nibble = odd
      const uint32_t byteIdx = (uint32_t)dx >> 1;
      if ((dx & 1) == 0) {
        dstRow[byteIdx] |= (uint8_t)(nibble << 4);
      } else {
        dstRow[byteIdx] |= nibble;
      }
    }
  }
  // Coarse content stat — printed by the worker so the user can tell
  // an "all-black" BMP (decode failure) from a successfully-built one.
  // Empty fit region (shouldn't happen, defensive) → leave defaults.
  const uint32_t mean = diagSamp ? (uint32_t)(diagSum / diagSamp) : 0;
  DEBUG_G2F("[G2] BMP build: src=%dx%d → fit=%dx%d offX=%d offY=%d "
            "gray min=%u max=%u mean=%u samples=%u",
            srcW, srcH, fitW, fitH, offX, offY,
            (unsigned)diagMin, (unsigned)diagMax, (unsigned)mean,
            (unsigned)diagSamp);
  return total;
}

#if ENABLE_CAMERA_SENSOR
#include "esp_camera.h"
#include "System_Camera_DVP.h"   // captureFrame, cameraWidth, cameraHeight, gCameraEnabled

struct CameraViewerArgs {
  void (*onDone)();
};

static void g2CameraViewerWorker(void* arg) {
  auto* a = (CameraViewerArgs*)arg;
  uint8_t* jpegBuf = nullptr;
  uint8_t* rgbBuf  = nullptr;
  uint8_t* bmpBuf  = nullptr;

  do {
    if (!gCameraEnabled) {
      DEBUG_G2F("[G2] Camera viewer: camera not enabled — abort");
      break;
    }
    G2Temple* arm = pickEvenAIArm("camView");
    if (!arm) {
      DEBUG_G2F("[G2] Camera viewer: no eligible arm");
      break;
    }

    // Capture. captureFrame() copies into PSRAM and returns ownership.
    size_t jpegLen = 0;
    jpegBuf = captureFrame(&jpegLen);
    if (!jpegBuf || jpegLen == 0) {
      DEBUG_G2F("[G2] Camera viewer: capture failed");
      break;
    }
    // Snapshot dimensions while still consistent — captureFrame() doesn't
    // expose them, but cameraWidth/Height are written by initCamera() and
    // persist across captures at the same framesize.
    const int srcW = cameraWidth;
    const int srcH = cameraHeight;
    if (srcW <= 0 || srcH <= 0) {
      DEBUG_G2F("[G2] Camera viewer: bad dims %dx%d", srcW, srcH);
      break;
    }
    DEBUG_G2F("[G2] Camera viewer: captured %u B JPEG at %dx%d",
              (unsigned)jpegLen, srcW, srcH);

    // Decode JPEG → RGB888.
    const size_t rgbLen = (size_t)srcW * (size_t)srcH * 3;
    rgbBuf = (uint8_t*)ps_alloc(rgbLen, AllocPref::PreferPSRAM, "g2.cam.rgb");
    if (!rgbBuf) {
      DEBUG_G2F("[G2] Camera viewer: rgb alloc failed (%u B)", (unsigned)rgbLen);
      break;
    }
    if (!fmt2rgb888(jpegBuf, jpegLen, PIXFORMAT_JPEG, rgbBuf)) {
      DEBUG_G2F("[G2] Camera viewer: fmt2rgb888 failed");
      break;
    }
    free(jpegBuf); jpegBuf = nullptr;

    // Build 288×144 4-bpp BMP. Header (118 B) + 144*144 px-bytes ≈ 21 KB.
    const size_t kBmpCap = 14 + 40 + 64 + (288 / 2) * 144 + 64;
    bmpBuf = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.cam.bmp");
    if (!bmpBuf) {
      DEBUG_G2F("[G2] Camera viewer: bmp alloc failed");
      break;
    }
    const size_t bmpLen = buildBmp4bppFromRgb888(bmpBuf, kBmpCap, rgbBuf, srcW, srcH);
    free(rgbBuf); rgbBuf = nullptr;
    if (bmpLen == 0) {
      DEBUG_G2F("[G2] Camera viewer: bmp build failed");
      break;
    }

    // Use a magic range that doesn't collide with the file BMP viewer
    // (0x20..0x2D) or the test-suite probes. Cmd=0 CREATE at +0x10,
    // Cmd=3 push fragments at +0x11..+0x1F. Frame is single-fragment
    // small but we keep room for safety.
    const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x10;  // 226
    const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x11;  // 227+
    if (!probeTearDownActiveContainer(*arm)) {
      DEBUG_G2F("[G2] Camera viewer: pre-push SHUTDOWN failed");
      break;
    }

    unsigned okFrags = 0, totalFrags = 0;
    (void)sendImageBmpMultiFragment(*arm, "camView",
                                    kCreateMagic, kPushMagicBase,
                                    bmpBuf, bmpLen,
                                    &okFrags, &totalFrags);
    DEBUG_G2F("[G2] Camera viewer: image up (%u/%u frags) — double-tap to dismiss",
              okFrags, totalFrags);
    const bool tapped = probeHoldUntilTapOrTimeout(60000);
    DEBUG_G2F("[G2] Camera viewer: hold ended via %s",
              tapped ? "user tap" : "60 s timeout");

    probePostProbeShutdown(*arm);
  } while (0);

  if (jpegBuf) free(jpegBuf);
  if (rgbBuf)  free(rgbBuf);
  if (bmpBuf)  free(bmpBuf);

  // Camera stays running across the dismiss — user explicitly chose
  // ON via the toggle and expects subsequent captures to work without
  // re-toggling. (Earlier auto-stop here was too aggressive: it fixed
  // the QVGA+ post-viewer DRAM exhaustion but broke the common-case
  // flow at smaller framesizes where there's plenty of DRAM. If
  // QVGA+ hits xTaskCreate failure on the post-viewer page-swap
  // again, fix the underlying issue (persistent worker task /
  // smaller stack / lower FB count for ImageProbing path) rather
  // than yanking the user's camera out from under them.)

  if (a && a->onDone) a->onDone();
  delete a;
  vTaskDelete(nullptr);
}

bool g2ShowCameraViewer(void (*onDone)()) {
  if (!gCameraEnabled) {
    DEBUG_G2F("[G2] Camera viewer: declining — camera not enabled");
    return false;
  }
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] Camera viewer: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new CameraViewerArgs;
  if (!a) return false;
  a->onDone = onDone;
  // 6 KB stack — heavy buffers all live in PSRAM, the worker carries
  // probe state + small locals only.
  if (xTaskCreate(g2CameraViewerWorker, "g2_cam_view", 6144, a,
                  tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] Camera viewer: xTaskCreate failed");
    delete a;
    return false;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────
// Camera stream — same capture+push pipeline as g2ShowCameraViewer
// but in a loop. We tear the active container down once at the
// start (Hijacked → ImageProbing transition), then for each frame
// capture, decode, build the BMP, and push via
// sendImageBmpMultiFragment with the same magic range each time —
// the firmware accepts a fresh CREATE that replaces the previous
// image session. Loop stops when the user double-taps (firmware
// emits SysEvent DOUBLE_CLICK; the existing probe-hold hook flips
// gImgProbeHoldTapPending which we poll between frames).
//
// Effective frame rate is bounded by the BLE push time —
// ~2.7 s/frame at 288×144 4-bpp = 7 frags. With the ~100 ms
// breathing delay between frames the cadence is ~0.35 fps.
// ─────────────────────────────────────────────────────────────────────

struct CameraStreamArgs {
  void (*onDone)();
};

// User-configurable lens-stream dimensions. The worker snapshots
// these at session start (so changing mid-stream takes effect on the
// next stream) and does its own clamp. Bounds match the lens panel
// (288×144) and a 16 px floor below which the firmware reassembler
// gets unhappy. Width is forced even because 4-bpp packs 2 px/byte.
//
// Source of truth is gSettings.g2StreamWidth/Height (persisted, default
// 96×96). The stream worker reads gSettings directly each session.
// Reference cadence: 96×96 → ~1.4 fps; 160×120 → ~0.7 fps;
// 288×144 → ~0.37 fps.

static void g2CameraStreamWorker(void* arg) {
  auto* a = (CameraStreamArgs*)arg;
  // Set when the user taps the "Settings >>" row — controls whether the
  // post-loop cleanup invokes onDone() (Back path → CAM detail) or
  // chains into g2ShowCameraSettingsMenu() (Settings path → settings
  // page → relaunch on back via g2CamStreamSettingsExitRelaunch).
  bool exitToSettings = false;

  do {
    if (!gCameraEnabled) {
      DEBUG_G2F("[G2] Camera stream: camera not enabled — abort");
      break;
    }
    G2Temple* arm = pickEvenAIArm("camStream");
    if (!arm) {
      DEBUG_G2F("[G2] Camera stream: no eligible arm");
      break;
    }

    // One-time SHUTDOWN of the underlying container (the Sensors
    // detail list). Subsequent frames just CREATE/push within the
    // ImageProbing FSM state.
    if (!probeTearDownActiveContainer(*arm)) {
      DEBUG_G2F("[G2] Camera stream: pre-stream SHUTDOWN failed");
      break;
    }

    // Stream is now list+image compound (Q28L's verified shape) — the
    // image lives in the bottom half, a 3-row list controls input in
    // the top half. The list-tap dispatcher in the BLE notify handler
    // sets bits in gCamStreamPendingTap which we drain each loop.
    // Double-tap-to-dismiss is intentionally NOT armed here — exit is
    // via the "<- Back" list row instead. The image-probe abort signal
    // is still cleared so a stale flag from a prior probe doesn't
    // immediately terminate this stream (firmware safety-timeout still
    // sets it via g2LensApplyHijackActive — see fix from 2026-05-07).
    gImgProbeHoldTapPending = false;
    gImgProbeAbort          = false;
    gImgProbeHoldActive     = false;
    gCamStreamPendingTap    = 0;
    // Don't inherit a stale relaunch flag from a prior session — fresh
    // entry should always honour onDone() unless THIS run sets it.
    g2CamStreamSettingsExitRelaunch = false;

    DEBUG_G2F("[G2] Camera stream: started (list+image; tap row to act)");

    // CREATE once on frame 0; subsequent frames push into the same
    // container with sendImageBmpFragmentsNoCreate. Re-issuing CREATE
    // every frame with the same magic/CID is silently dropped by the
    // firmware, which is what was killing the stream after 1 frame.
    //
    // Stream dims are user-configurable (see gSettings.g2StreamWidth/Height).
    // Smaller dims = fewer Cmd=3 fragments per push = higher fps. Reference
    // points (4-bpp, G2_IMG_MAPRAW_CHUNK_BYTES):
    //   ·  96× 96 →  ~4.8 KB →  2 frags →  ~700 ms /frame  (~1.4 fps)
    //   · 160×120 →  ~9.7 KB →  3 frags → ~1100 ms /frame  (~0.9 fps)
    //   · 288×144 → ~20.9 KB →  6 frags → ~2300 ms /frame  (~0.43 fps)
    // Confirmed by Q19 probe that solo small-dim BMPs render on the
    // lens; 288×144 is the lens-panel native size.
    int32_t streamW = gSettings.g2StreamWidth;
    int32_t streamH = gSettings.g2StreamHeight;
    if (streamW < 16)  streamW = 16;
    if (streamH < 16)  streamH = 16;
    if (streamW > 288) streamW = 288;
    if (streamH > 144) streamH = 144;
    streamW &= ~1;  // even — 4-bpp packs 2 px/byte

    // Estimate fragsPerFrame from the BMP layout we'll build. The
    // 4-bpp row stride is 4-byte aligned (matches buildBmp4bppFromRgb888).
    const size_t   kChunkBytes      = G2_IMG_MAPRAW_CHUNK_BYTES;
    const size_t   kBmpHdrBytes     = 14 + 40 + 64;
    const size_t   bmpRowStride     = (((size_t)streamW * 4 + 31) / 32) * 4;
    const size_t   bmpEstLen        = kBmpHdrBytes + bmpRowStride * (size_t)streamH;
    const unsigned fragsPerFrame    = (unsigned)((bmpEstLen + kChunkBytes - 1) / kChunkBytes);

    // Push magics must all fit in uint8 ≤ 255 (firmware constraint —
    // see Q10 comment). After CREATE we have 227..254 = 28 magics for
    // push fragments. Each frame's burst occupies fragsPerFrame
    // contiguous magics, so we get floor(28 / fragsPerFrame) cycling
    // slots — e.g. 14 @2 frags, 9 @3, 7 @4, 4 @6–7 frags. Cycling
    // avoids reusing the same magic range on consecutive frames,
    // which would confuse the firmware reassembler.
    const uint32_t kCreateMagic     = G2_MAGIC_IMAGE_BASE + 0x10;  // 226
    const uint32_t kPushSlotStart   = G2_MAGIC_IMAGE_BASE + 0x11;  // 227
    const uint32_t kMaxPushMagic    = 254;
    const unsigned pushAvailable    = (unsigned)(kMaxPushMagic - kPushSlotStart + 1);
    const unsigned numSlots         = (fragsPerFrame > 0 && fragsPerFrame <= pushAvailable)
                                        ? (pushAvailable / fragsPerFrame)
                                        : 1;
    // BMP cap sized for the lens-panel max so the per-frame ps_alloc
    // is a no-op regardless of the configured stream dims.
    const size_t   kBmpCap              = 14 + 40 + 64 + (288 / 2) * 144 + 64;
    const uint32_t kStreamSafetyCapMs   = 5UL * 60UL * 1000UL;  // 5 minute cap
    // Defensive frame-rate ceiling. The natural cadence at any sane
    // dim is well above this (700 ms minimum at 96×96), so this cap
    // basically never engages — it's here as a backstop in case the
    // BLE link ever speeds up enough that we'd hammer the OV3660 or
    // collide with the web's MJPEG stream.
    const uint32_t kMinFramePeriodMs    = 250;  // ~4 Hz ceiling

    // Image lives in the bottom half, list lives in the top half.
    // List geom matches the verified-rendering Q28L shape so we know
    // the firmware composites both children. Image position centers
    // streamW × streamH inside the bottom 144 px band; W is forced
    // even (4-bpp packing constraint).
    const G2ContainerGeom kListGeom = { 8, 0, 560, 130 };
    const int32_t imgX = ((576 - streamW) / 2) & ~1;
    const int32_t imgY = 144 + (144 - streamH) / 2;
    G2ImageTile imgTile = {};
    imgTile.x = (uint32_t)imgX;
    imgTile.y = (uint32_t)imgY;
    imgTile.w = (uint32_t)streamW;
    imgTile.h = (uint32_t)streamH;
    imgTile.containerId   = 2;
    imgTile.containerName = "imgCam";

    // Root row labels — kept short so the CREATE-list pb stays well
    // under the 240 B single-fragment cap. Snapshot label flips to
    // "Saved!" briefly via REBUILD-list when the user taps it (see
    // confirmFlash plumbing below).
    static const char* const kRootRows[] = {
      "<- Back",
      "Snapshot",
      "Settings >>",
    };
    static constexpr size_t kRootRowCount = 3;
    // Snapshot confirm-flash via REBUILD-list was tried first (row 1
    // → "Saved!" for ~1.5 s, then REBUILD back). Verified on-device
    // 2026-05-08: REBUILD-list against a list+image compound silently
    // drops the image child on this firmware. Subsequent Cmd=3 image
    // pushes still ack with res=0 but the lens stops compositing
    // them, and a follow-up REBUILD back to the original rows does
    // NOT recover (image is gone until SHUTDOWN+CREATE-mixed). So
    // any REBUILD-list during a live compound stream is a one-way
    // ticket to "list-only" mode. Snapshot now saves silently — the
    // log line + the file on SD are the feedback path.

    // Build + send CREATE-mixed-list-image. We do this by hand (rather
    // than via sendImageBmpMultiFragment) because the helper hardcodes
    // the image-only CREATE path. The image-push helpers below
    // (sendImageBmpFragmentsNoCreate) target this CREATE's image child.
    {
      uint8_t createBuf[1024];
      size_t createLen = g2BuildCreateMixedListImage(
          allocSeq(), kCreateMagic,
          /*listName*/ "lstCam",
          kRootRows, kRootRowCount, kListGeom,
          imgTile, BLOCKS_WIDGET_ID,
          createBuf, sizeof(createBuf));
      if (createLen == 0) {
        DEBUG_G2F("[G2] Camera stream: CREATE-mixed build failed");
        break;
      }
      probePrepImageCreateAck(kCreateMagic);
      if (!sendEnvelope(*arm, createBuf, createLen)) {
        DEBUG_G2F("[G2] Camera stream: CREATE-mixed TX failed");
        break;
      }
      if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs)) {
        DEBUG_G2F("[G2] Camera stream: CREATE-mixed ack timeout");
        probePostProbeShutdown(*arm);
        break;
      }
      DEBUG_G2F("[G2] Camera stream: CREATE-mixed acked "
                "(list@'lstCam' + image@'imgCam' %dx%d @(%d,%d))",
                (int)streamW, (int)streamH, (int)imgX, (int)imgY);
    }

    // Mark active so the BLE ListEvent dispatcher routes lstCam taps
    // into our pending-tap bitfield. Cleared at exit so late echoes
    // don't pollute a future stream session.
    gCamStreamActive = true;

    DEBUG_G2F("[G2] Camera stream: dims=%dx%d frags/frame=%u slots=%u cap=%uB",
              (int)streamW, (int)streamH, fragsPerFrame, numSlots,
              (unsigned)kBmpCap);

    int      frame = 0;
    uint32_t startMs = millis();
    // Set when the worker should save the next captured JPEG. The
    // user's tap can land at any moment; defer the save until we have
    // a fresh JPEG in hand on the next iteration so we never race a
    // half-built buffer.
    bool snapshotPending = false;

    while (true) {
      // Safety: never let the stream run indefinitely.
      if ((millis() - startMs) > kStreamSafetyCapMs) {
        DEBUG_G2F("[G2] Camera stream: 5-min safety cap reached, stopping");
        break;
      }
      // Firmware-side hijack timeout (or peer-side dismiss) tears down
      // lens-page mode. Catch that here so we don't spend a frame's
      // worth of capture+decode+BMP-build on a panel no one's looking at.
      if (!gLens.hijackActive) {
        DEBUG_G2F("[G2] Camera stream: hijack ended mid-stream, stopping");
        break;
      }
      // Firmware-driven abort (safety-timeout sets gImgProbeAbort via
      // g2LensApplyHijackActive). User-side dismiss now flows through
      // gCamStreamPendingTap instead, but the abort flag is still our
      // emergency stop for non-user causes.
      if (gImgProbeAbort) {
        DEBUG_G2F("[G2] Camera stream: gImgProbeAbort set, stopping");
        break;
      }

      // Drain pending list-row taps. Read-and-clear so a re-tap during
      // this iteration sets a fresh pending bit for the next pass.
      const uint32_t taps = gCamStreamPendingTap;
      gCamStreamPendingTap = 0;
      if (taps & 0x1u) {
        DEBUG_G2F("[G2] Camera stream: Back tap → exit");
        break;
      }
      if (taps & 0x4u) {
        DEBUG_G2F("[G2] Camera stream: Settings tap → exit, chain to settings");
        exitToSettings = true;
        break;
      }
      if (taps & 0x2u) {
        // Defer: the actual save happens after the next captureFrame()
        // succeeds, while we still own the JPEG buffer.
        snapshotPending = true;
      }

      const uint32_t frameStartMs = millis();

      // Capture
      size_t   jpegLen = 0;
      uint8_t* jpegBuf = captureFrame(&jpegLen);
      if (!jpegBuf || jpegLen == 0) {
        DEBUG_G2F("[G2] Camera stream: capture failed (frame %d)", frame);
        if (jpegBuf) free(jpegBuf);
        break;
      }
      const int srcW = cameraWidth;
      const int srcH = cameraHeight;
      if (srcW <= 0 || srcH <= 0) {
        DEBUG_G2F("[G2] Camera stream: bad dims %dx%d", srcW, srcH);
        free(jpegBuf);
        break;
      }

      // Snapshot save — has to land BEFORE we free(jpegBuf). We save
      // the full-resolution JPEG (not the downsampled BMP we push to
      // lens) so the user gets a usable photo, not a 96×96 4-bpp
      // postage stamp. Filename: cam_<millis>.jpg under /sd/PICTURES/.
      // Save is silent on the lens (no row swap) — see the firmware-
      // killed-image note up by kRootRows. The DEBUG log line + the
      // file appearing under /sd/PICTURES/ are the feedback path.
      if (snapshotPending) {
        snapshotPending = false;
        char path[64];
        snprintf(path, sizeof(path), "/sd/PICTURES/cam_%lu.jpg",
                 (unsigned long)millis());
        // Route through VFS (shared FsLockGuard + permission guard) instead of
        // raw POSIX fopen — fopen also requires /sd to be registered as a POSIX
        // mount, whereas VFS dispatches /sd to the Arduino SD backend the rest
        // of the camera code already uses. Behavior is otherwise unchanged: the
        // parent dir must still pre-exist (open does not create it).
        File f = VFS::openGuarded(String(path), "w",
                                  VFS::systemAuth("g2.cam_snapshot"), true);
        if (f) {
          const size_t wrote = f.write((const uint8_t*)jpegBuf, jpegLen);
          f.close();
          if (wrote == jpegLen) {
            DEBUG_G2F("[G2] Camera stream: snapshot saved %s (%u B)",
                      path, (unsigned)jpegLen);
          } else {
            DEBUG_G2F("[G2] Camera stream: snapshot write short %u/%u to %s",
                      (unsigned)wrote, (unsigned)jpegLen, path);
          }
        } else {
          DEBUG_G2F("[G2] Camera stream: snapshot open failed: %s "
                    "(SD mounted? /sd/PICTURES/ exists?)",
                    path);
        }
      }

      // Decode
      const size_t rgbLen = (size_t)srcW * (size_t)srcH * 3;
      uint8_t* rgbBuf = (uint8_t*)ps_alloc(rgbLen, AllocPref::PreferPSRAM, "g2.camstr.rgb");
      if (!rgbBuf) {
        DEBUG_G2F("[G2] Camera stream: rgb alloc failed");
        free(jpegBuf);
        break;
      }
      if (!fmt2rgb888(jpegBuf, jpegLen, PIXFORMAT_JPEG, rgbBuf)) {
        DEBUG_G2F("[G2] Camera stream: fmt2rgb888 failed (frame %d)", frame);
        free(jpegBuf); free(rgbBuf);
        break;
      }
      free(jpegBuf);

      // Build BMP
      uint8_t* bmpBuf = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.camstr.bmp");
      if (!bmpBuf) {
        DEBUG_G2F("[G2] Camera stream: bmp alloc failed");
        free(rgbBuf);
        break;
      }
      const BmpToneMap toneMap = gSettings.g2StreamToneMap
                                   ? BmpToneMap::AutoLevels
                                   : BmpToneMap::None;
      const size_t bmpLen = buildBmp4bppFromRgb888(bmpBuf, kBmpCap,
                                                   rgbBuf, srcW, srcH,
                                                   streamW, streamH,
                                                   toneMap);
      free(rgbBuf);
      if (bmpLen == 0) {
        DEBUG_G2F("[G2] Camera stream: bmp build failed (frame %d)", frame);
        free(bmpBuf);
        break;
      }

      // Push fragments only (CREATE was done once above). Magic
      // cycling avoids re-using the same range on consecutive frames
      // — see numSlots derivation above. tolerateMissedAcks=3 keeps
      // the stream flowing through occasional ack drops; streaming is
      // forgiving since frame N+1 overwrites N quickly.
      const unsigned kStreamAckTolerance = 3;
      unsigned okFrags = 0, totalFrags = 0;
      const uint32_t pushBase =
          kPushSlotStart + (uint32_t)(frame % numSlots) * (uint32_t)fragsPerFrame;
      const bool pushed = sendImageBmpFragmentsNoCreate(
          *arm, "camStream",
          pushBase, /*cid*/ imgTile.containerId,
          /*cname*/ imgTile.containerName,
          bmpBuf, bmpLen, &okFrags, &totalFrags,
          kStreamAckTolerance);
      free(bmpBuf);

      if (!pushed) {
        DEBUG_G2F("[G2] Camera stream: push aborted (frame %d, %u/%u frags)",
                  frame, okFrags, totalFrags);
        break;
      }

      DEBUG_G2F("[G2] Camera stream: frame %d pushed (%u frags)",
                frame, totalFrags);
      frame++;

      // Per-frame floor (defensive cap; natural cadence already
      // exceeds this). When natural cadence is the binding constraint
      // (always true at any sane stream dim), the small post-burst
      // breather lets FreeRTOS run idle/heartbeat tasks without
      // appreciably hurting fps.
      const uint32_t elapsedMs = millis() - frameStartMs;
      const uint32_t toSleepMs = (elapsedMs < kMinFramePeriodMs)
                                   ? (kMinFramePeriodMs - elapsedMs)
                                   : 25;
      vTaskDelay(pdMS_TO_TICKS(toSleepMs));
    }

    gCamStreamActive     = false;
    gCamStreamPendingTap = 0;
    DEBUG_G2F("[G2] Camera stream: stopped after %d frame(s) (%s)",
              frame,
              exitToSettings           ? "Settings tap" :
              !gLens.hijackActive      ? "hijack ended (safety-timeout/peer)" :
              gImgProbeAbort           ? "abort flag"
                                       : "Back tap or loop exit");
    probePostProbeShutdown(*arm);
  } while (0);

  // Camera stays running across the stream-stop. See the equivalent
  // comment in g2CameraViewerWorker: auto-stopping the camera after
  // a lens action is bad UX — the user toggled CAM ON and expects
  // it to stay on until they toggle OFF.

  if (exitToSettings) {
    // Settings path — chain into the camera-settings page. The page's
    // back-row handler checks g2CamStreamSettingsExitRelaunch and
    // relaunches THIS worker on back, returning the user to the live
    // stream with their settings already applied. onDone is NOT
    // invoked here — that's only the Back-row exit.
    g2CamStreamSettingsExitRelaunch = true;
    g2ShowCameraSettingsMenu();
  } else if (a && a->onDone) {
    // Back-row exit (or any non-Settings stop) → invoke onDone, which
    // returns the user to the CAM detail page (per the Sensors-detail
    // wire-up at G2_Page_Sensors.cpp:930).
    a->onDone();
  }
  delete a;
  vTaskDelete(nullptr);
}

bool g2ShowCameraStream(void (*onDone)()) {
  if (!gCameraEnabled) {
    DEBUG_G2F("[G2] Camera stream: declining — camera not enabled");
    return false;
  }
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] Camera stream: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new CameraStreamArgs;
  if (!a) return false;
  a->onDone = onDone;
  if (xTaskCreate(g2CameraStreamWorker, "g2_cam_stream", 6144, a,
                  tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] Camera stream: xTaskCreate failed");
    delete a;
    return false;
  }
  return true;
}

#else  // !ENABLE_CAMERA_SENSOR
bool g2ShowCameraViewer(void (*onDone)()) {
  (void)onDone;
  return false;
}
bool g2ShowCameraStream(void (*onDone)()) {
  (void)onDone;
  return false;
}
#endif

// ─────────────────────────────────────────────────────────────────────
// Full-screen BMP viewer — 288×144 source upscaled 2× via nearest-
// neighbour to a 576×288 image and shipped as a 2×2 grid of 288×144
// tiles. The single-large-image transport (one ImageObject with
// 576×288 geom) has never been verified on this firmware; Q12 is the
// known-good path for full-canvas content, so we reuse its tile shape.
//
// Memory: 1× source BMP (~21 KB PSRAM) + 1× tile-build buffer (~21 KB
// PSRAM, reused across the four pushes). The upscale is done per-tile
// directly into the build buffer so we never materialise the full
// 576×288 intermediate.
// ─────────────────────────────────────────────────────────────────────

// Read 288×144 4bpp BMP `src` (raw file as returned by readBmpFromVfs)
// and emit a fresh 288×144 4bpp BMP into `out` whose pixel data is the
// `quadrant`-th quarter of `src` upscaled 2× by pixel duplication.
//   quadrant 0 = top-left  (src cols 0..143,  rows 0..71)
//            1 = top-right (src cols 144..287, rows 0..71)
//            2 = bottom-left
//            3 = bottom-right
// Output BMP is always top-down (negative biHeight) so caller-side
// row math doesn't have to flip. Returns total bytes written, or 0 on
// failure (capacity / malformed source).
static size_t buildTileBmpFromQuadrant(uint8_t* out, size_t outCap,
                                       const uint8_t* src, size_t srcLen,
                                       uint8_t quadrant) {
  if (!out || !src || quadrant > 3) return 0;
  if (outCap < 70 || srcLen < 118) return 0;
  if (!(src[0] == 'B' && src[1] == 'M')) return 0;

  // Source pixel-data offset from the file header (bfOffBits).
  const uint32_t srcPxOff = (uint32_t)src[10] | ((uint32_t)src[11] << 8) |
                            ((uint32_t)src[12] << 16) | ((uint32_t)src[13] << 24);
  if (srcPxOff < 14 + 40 + 64 || srcPxOff > srcLen) return 0;

  // Source dimensions (must be 288×144 — readBmpFromVfs already
  // enforced this, but we re-check so this helper is self-defensive).
  const int32_t srcW = (int32_t)((uint32_t)src[18] | ((uint32_t)src[19] << 8) |
                                 ((uint32_t)src[20] << 16) | ((uint32_t)src[21] << 24));
  const int32_t srcHsigned = (int32_t)((uint32_t)src[22] | ((uint32_t)src[23] << 8) |
                                       ((uint32_t)src[24] << 16) | ((uint32_t)src[25] << 24));
  if (srcW != 288) return 0;
  const int32_t srcH = (srcHsigned < 0) ? -srcHsigned : srcHsigned;
  if (srcH != 144) return 0;
  const bool srcTopDown = (srcHsigned < 0);

  // Output BMP: 288×144 top-down 4bpp. Header layout matches
  // buildBmp4bpp() so the wire transport doesn't need to know the
  // difference between a synthetic test-pattern tile and a
  // user-content tile. headerSize = 14 (file) + 40 (DIB) + 64 (16-entry
  // BGRA palette) = 118 bytes. Pixel data row stride = 144 bytes
  // (288 px / 2, already 4-byte aligned).
  const uint32_t kHeaderSize = 14 + 40 + 64;
  const uint32_t kRowStride  = 144;
  const uint32_t kPixelSize  = kRowStride * 144;
  const uint32_t kTotal      = kHeaderSize + kPixelSize;
  if (outCap < kTotal) return 0;

  // Copy the full source header (file + DIB + palette) so palette
  // entries are preserved. We override the dimensions / sizes / offset
  // fields for our output shape.
  // Source header may be larger than 118 (extended DIB headers, gaps
  // before pixel data); we only copy up to kHeaderSize because that's
  // all the receiver expects in the buildBmp4bpp wire shape.
  const size_t hdrCopy = (srcPxOff < kHeaderSize) ? srcPxOff : kHeaderSize;
  memcpy(out, src, hdrCopy);
  if (hdrCopy < kHeaderSize) {
    // Shouldn't happen given the >=118 check above, but zero the rest
    // of the palette region defensively.
    memset(out + hdrCopy, 0, kHeaderSize - hdrCopy);
  }

  auto wr32 = [](uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);          p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);  p[3] = (uint8_t)((v >> 24) & 0xff);
  };
  out[0] = 'B'; out[1] = 'M';
  wr32(out + 2,  kTotal);                          // bfSize
  wr32(out + 10, kHeaderSize);                     // bfOffBits
  wr32(out + 14, 40);                              // biSize
  wr32(out + 18, (uint32_t)288);                   // biWidth
  wr32(out + 22, (uint32_t)(int32_t)(-144));       // biHeight (top-down)
  out[26] = 1; out[27] = 0;                        // biPlanes
  out[28] = 4; out[29] = 0;                        // biBitCount = 4
  wr32(out + 30, 0);                               // biCompression
  wr32(out + 34, kPixelSize);                      // biSizeImage
  wr32(out + 46, 16);                              // biClrUsed
  wr32(out + 50, 0);                               // biClrImportant

  // ── Pixel-data upscale ──
  // Source byte layout: 144 bytes per row × 144 rows = 20736 B.
  // Quadrant origin in source-image coordinates:
  const size_t srcByteCol0 = (quadrant & 1) ? 72 : 0;   // right vs left half
  const size_t srcRowImg0  = (quadrant & 2) ? 72 : 0;   // bottom vs top half
  const uint8_t* srcPixels = src + srcPxOff;
  uint8_t*       dstPixels = out + kHeaderSize;

  for (size_t outRow = 0; outRow < 144; outRow++) {
    // Each output row pulls from one source row (every two output
    // rows share the same source row — vertical 2× duplication).
    const size_t srcRowImg = srcRowImg0 + (outRow >> 1);
    // If source is bottom-up, row 0 in image-space is at the BOTTOM
    // of the file's pixel data; flip the row index accordingly.
    const size_t srcRowFile = srcTopDown ? srcRowImg : (143 - srcRowImg);
    const uint8_t* srcRow   = srcPixels + srcRowFile * 144 + srcByteCol0;
    uint8_t*       dstRow   = dstPixels + outRow * kRowStride;

    // 72 source bytes (144 source pixels) → 144 output bytes (288
    // output pixels). Each source nibble appears twice in the output.
    for (size_t i = 0; i < 72; i++) {
      const uint8_t b  = srcRow[i];
      const uint8_t hi = (uint8_t)((b >> 4) & 0x0F);
      const uint8_t lo = (uint8_t)( b       & 0x0F);
      dstRow[i * 2]     = (uint8_t)((hi << 4) | hi);
      dstRow[i * 2 + 1] = (uint8_t)((lo << 4) | lo);
    }
  }

  return kTotal;
}

struct BmpFullViewerArgs {
  char* path;
  void (*onDone)();
};

static void g2BmpFullViewerWorker(void* arg) {
  G2HijackCtxGuard ctxGuard;  // identity + NOTIF_SOURCE_G2 for VFS reads
  auto* a = (BmpFullViewerArgs*)arg;

  do {
    G2Temple* arm = pickEvenAIArm("bmpFullView");
    if (!arm) {
      DEBUG_G2F("[G2] BMP full-viewer: no eligible arm");
      break;
    }

    const char* loadErr = "";
    uint8_t* src = nullptr;
    size_t srcLen = 0;
    int32_t srcW = 0, srcH = 0;
    if (!readBmpFromVfs(String(a->path), &src, &srcLen, &srcW, &srcH, &loadErr)) {
      DEBUG_G2F("[G2] BMP full-viewer: load failed for '%s': %s",
                a->path, loadErr ? loadErr : "?");
      break;
    }
    DEBUG_G2F("[G2] BMP full-viewer: '%s' loaded (%u B, %dx%d)",
              a->path, (unsigned)srcLen, (int)srcW, (int)srcH);

    // Tile geometry — full canvas as 2×2 grid of 288×144 quadrants.
    const int32_t kTileW = 288;
    const int32_t kTileH = 144;
    const G2ImageTile kTiles[4] = {
      { /*x*/ 0,                  /*y*/ 0,
        /*w*/ (uint32_t)kTileW,   /*h*/ (uint32_t)kTileH,
        /*cid*/ 2, /*name*/ "tileTL" },
      { /*x*/ (uint32_t)kTileW,   /*y*/ 0,
        /*w*/ (uint32_t)kTileW,   /*h*/ (uint32_t)kTileH,
        /*cid*/ 3, /*name*/ "tileTR" },
      { /*x*/ 0,                  /*y*/ (uint32_t)kTileH,
        /*w*/ (uint32_t)kTileW,   /*h*/ (uint32_t)kTileH,
        /*cid*/ 4, /*name*/ "tileBL" },
      { /*x*/ (uint32_t)kTileW,   /*y*/ (uint32_t)kTileH,
        /*w*/ (uint32_t)kTileW,   /*h*/ (uint32_t)kTileH,
        /*cid*/ 5, /*name*/ "tileBR" },
    };

    // Magic ranges. CREATE = 0x10 (226). Each tile's 7 push frags get
    // its own block of 7 to keep the firmware's per-tile ack
    // bookkeeping clear. Same shape as Q12.
    const uint32_t kCreateMagic = G2_MAGIC_IMAGE_BASE + 0x10;          // 226
    const uint32_t kPushBase[4] = {
      G2_MAGIC_IMAGE_BASE + 0x11,  // 227..233 TL
      G2_MAGIC_IMAGE_BASE + 0x18,  // 234..240 TR
      G2_MAGIC_IMAGE_BASE + 0x1F,  // 241..247 BL
      G2_MAGIC_IMAGE_BASE + 0x26,  // 248..254 BR
    };

    if (!probeTearDownActiveContainer(*arm)) {
      DEBUG_G2F("[G2] BMP full-viewer: pre-burst SHUTDOWN failed");
      free(src);
      break;
    }

    // ── Multi-tile CREATE ──
    uint8_t createBuf[256];
    size_t createLen = g2BuildCreateImageMulti(
        allocSeq(), kCreateMagic, kTiles, /*tileCount*/ 4,
        BLOCKS_WIDGET_ID, createBuf, sizeof(createBuf));
    if (createLen == 0) {
      DEBUG_G2F("[G2] BMP full-viewer: CREATE-multi build failed");
      free(src);
      probePostProbeShutdown(*arm);
      break;
    }
    noteOurShutdownSent();
    probePrepImageCreateAck(kCreateMagic);
    if (!sendEnvelope(*arm, createBuf, createLen)) {
      DEBUG_G2F("[G2] BMP full-viewer: CREATE-multi TX failed");
      free(src);
      probePostProbeShutdown(*arm);
      break;
    }
    if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs)) {
      DEBUG_G2F("[G2] BMP full-viewer: CREATE-multi ack timeout — "
                "firmware may not honour the 4-tile geometry");
      free(src);
      probePostProbeShutdown(*arm);
      break;
    }
    DEBUG_G2F("[G2] BMP full-viewer: CREATE-multi acked, pushing 4 tiles");

    // ── Per-tile build + push ──
    const size_t kTileBmpCap = 24 * 1024;
    uint8_t* tileBmp = (uint8_t*)ps_alloc(kTileBmpCap, AllocPref::PreferPSRAM,
                                          "g2.bmpFull.tile");
    if (!tileBmp) {
      DEBUG_G2F("[G2] BMP full-viewer: tile-buf alloc failed");
      free(src);
      probePostProbeShutdown(*arm);
      break;
    }

    const char* kTileTag[4] = { "Full/TL", "Full/TR", "Full/BL", "Full/BR" };
    unsigned okTotal = 0, fragTotal = 0;
    bool anyTileFailed = false;
    for (size_t i = 0; i < 4; i++) {
      const size_t tileLen = buildTileBmpFromQuadrant(tileBmp, kTileBmpCap,
                                                       src, srcLen, (uint8_t)i);
      if (tileLen == 0) {
        DEBUG_G2F("[G2] BMP full-viewer: tile %u/4 build failed", (unsigned)(i + 1));
        anyTileFailed = true;
        break;
      }
      unsigned ok = 0, total = 0;
      (void)sendImageBmpFragmentsNoCreate(*arm, kTileTag[i], kPushBase[i],
                                           kTiles[i].containerId,
                                           kTiles[i].containerName,
                                           tileBmp, tileLen, &ok, &total);
      okTotal   += ok;
      fragTotal += total;
      DEBUG_G2F("[G2] BMP full-viewer: tile %u/4 (%s) shipped %u/%u",
                (unsigned)(i + 1), kTileTag[i], ok, total);
      // Inter-tile gap so the firmware's per-tile reassembly window
      // drains before the next push begins (same pattern as Q12).
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    free(tileBmp);
    free(src);

    if (anyTileFailed) {
      probePostProbeShutdown(*arm);
      break;
    }

    DEBUG_G2F("[G2] BMP full-viewer: image up — double-tap to dismiss "
              "(60 s cap, %u/%u total frags)", okTotal, fragTotal);
    const bool tapped = probeHoldUntilTapOrTimeout(60000);
    DEBUG_G2F("[G2] BMP full-viewer: hold ended via %s",
              tapped ? "user tap" : "60 s timeout");

    probePostProbeShutdown(*arm);
  } while (0);

  if (a->onDone) a->onDone();
  free(a->path);
  delete a;
  vTaskDelete(nullptr);
}

bool g2ShowBmpFileFullScreen(const char* path, void (*onDone)()) {
  if (!path || !*path) return false;
  // Heap-low guard. Both the source BMP (~21 KB) and the per-tile
  // staging buffer (~24 KB) are PSRAM allocations (ps_alloc /
  // PreferPSRAM), so they don't draw on internal DRAM. The only
  // DRAM cost is the 8 KB worker stack + small local variables
  // (~1 KB). Gate at 16 KB — same as the single-tile viewer which
  // uses a 6 KB stack and passes this check fine with ~23 KB free.
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] BMP full-viewer: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new BmpFullViewerArgs;
  if (!a) return false;
  a->path   = strdup(path);
  a->onDone = onDone;
  if (!a->path) {
    delete a;
    return false;
  }
  // 8 KB stack — 4-tile burst nests several helper calls (probe ack,
  // mutex take, fragment build) so we run a slightly larger stack
  // than the single-image viewer's 6 KB.
  if (xTaskCreate(g2BmpFullViewerWorker, "g2_bmp_full", 8192, a,
                  tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] BMP full-viewer: xTaskCreate failed");
    free(a->path);
    delete a;
    return false;
  }
  return true;
}

// ═════════════════════════════════════════════════════════════════════
// JPG viewer — same wire transport as the BMP viewers above; the only
// difference is a JPEG → 4-bpp BMP conversion step at the front. The
// camera viewer (g2CameraViewerWorker) and camera stream worker both
// take exactly this conversion path on every frame; the JPG file
// viewer reuses the same decoder so any JPEG that the camera could
// produce can also be displayed back to the lens. Required to view
// the camera-stream snapshot files at /sd/PICTURES/cam_<ms>.jpg.
//
// Memory profile (peak):
//   JPEG file copy   ≤ 128 KB    (gated; freed after decode)
//   RGB888 buffer    srcW·srcH·3 (e.g. 240×240 = 173 KB; freed after BMP build)
//   4-bpp BMP        ~21 KB      (held until push completes)
// All allocations prefer PSRAM via ps_alloc.
// ═════════════════════════════════════════════════════════════════════

// JPG file viewer — unconditional. fmt2rgb888 comes from the esp32-camera
// conversions library, which is linked on every board (component
// CMakeLists REQUIRES), so file-based JPEG decode does not depend on a
// physical sensor / ENABLE_CAMERA_SENSOR — only live capture/stream does.
#include "img_converters.h"  // already included unconditionally above (header-guarded)

// Walk the JPEG marker stream looking for SOF0/SOF1/SOF2/etc to
// extract the source image's pixel dimensions. We need this before
// allocating the RGB888 decode buffer — fmt2rgb888 wants a buffer
// pre-sized to W·H·3 but doesn't return dims itself. Stops on first
// SOFn. Returns false on malformed input or if no SOFn is found.
//
// JPEG marker structure: every marker starts with 0xFF; the byte
// after is the marker ID. SOFn markers are 0xC0..0xCF excluding
// 0xC4 (DHT), 0xC8 (reserved), and 0xCC (DAC). Each SOFn payload
// begins with: 2 B segment length (big-endian) + 1 B sample
// precision + 2 B image height + 2 B image width.
static bool parseJpegDimensions(const uint8_t* buf, size_t len,
                                int32_t* outW, int32_t* outH) {
  if (!buf || len < 4 || !outW || !outH) return false;
  if (buf[0] != 0xFF || buf[1] != 0xD8) return false;  // SOI
  size_t p = 2;
  while (p + 3 < len) {
    // Skip fill bytes (0xFF padding before next marker).
    while (p < len && buf[p] == 0xFF) p++;
    if (p >= len) return false;
    const uint8_t marker = buf[p++];
    // Standalone markers (no payload): SOI, EOI, RSTn, TEM. None of
    // these carry segment length, so just continue. (We've already
    // skipped past SOI; EOI here = malformed since we hit it before
    // a SOFn.)
    if (marker == 0xD9 || marker == 0x01 ||
        (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }
    if (p + 1 >= len) return false;
    const uint16_t segLen = ((uint16_t)buf[p] << 8) | (uint16_t)buf[p + 1];
    if (segLen < 2 || p + segLen > len) return false;
    // SOFn marker check.
    if (marker >= 0xC0 && marker <= 0xCF &&
        marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
      // Payload layout: [segLen:2][precision:1][height:2][width:2]
      if (segLen < 7) return false;
      *outH = ((int32_t)buf[p + 3] << 8) | (int32_t)buf[p + 4];
      *outW = ((int32_t)buf[p + 5] << 8) | (int32_t)buf[p + 6];
      return (*outW > 0 && *outH > 0);
    }
    // Other marker — skip its segment payload.
    p += segLen;
  }
  return false;
}

// Read JPEG from VFS, decode, and produce a 288×144 4-bpp BMP buffer
// suitable for either single-tile push (g2ShowJpgFile) or 4-tile full-
// screen push (g2ShowJpgFileFullScreen, which then upscales each
// quadrant 2× via buildTileBmpFromQuadrant). On success the caller
// owns *outBmp and must free() it. Errors are returned via outErr
// (static strings — caller doesn't free).
static bool loadJpgAsBmp288x144(const String& rawPath,
                                uint8_t** outBmp, size_t* outBmpLen,
                                const char** outErr) {
  if (outBmp) *outBmp = nullptr;
  if (outBmpLen) *outBmpLen = 0;
  if (outErr) *outErr = "unknown error";

  String path = rawPath;
  path.trim();
  if (path.length() == 0) { if (outErr) *outErr = "empty path"; return false; }
  if (!path.startsWith("/")) path = "/" + path;
  if (!VFS::existsGuarded(path, currentAuthContext())) {
    if (outErr) *outErr = "file not found";
    return false;
  }

  File f = VFS::openGuarded(path, FILE_READ, currentAuthContext());
  if (!f || !f.available()) {
    if (outErr) *outErr = "failed to open file";
    return false;
  }
  const size_t len = (size_t)f.size();
  // 128 KB cap — covers typical phone snapshots and anything the
  // camera-stream worker writes. Larger files almost certainly mean
  // the user dropped a multi-MP JPG on the SD card; the decoded
  // RGB888 would be tens of MB and won't fit even in PSRAM.
  const size_t kMaxJpgBytes = 128 * 1024;
  if (len < 16) {
    f.close();
    if (outErr) *outErr = "file too small for JPEG";
    return false;
  }
  if (len > kMaxJpgBytes) {
    f.close();
    if (outErr) *outErr = "JPEG too large (>128 KB)";
    return false;
  }

  uint8_t* jpg = (uint8_t*)ps_alloc(len, AllocPref::PreferPSRAM, "g2.jpg.fileLoad");
  if (!jpg) {
    f.close();
    if (outErr) *outErr = "out of memory reading JPG";
    return false;
  }
  const size_t rd = f.read(jpg, len);
  f.close();
  if (rd != len) {
    free(jpg);
    if (outErr) *outErr = "short read from JPG file";
    return false;
  }

  if (!(jpg[0] == 0xFF && jpg[1] == 0xD8)) {
    free(jpg);
    if (outErr) *outErr = "not a JPEG (missing SOI marker)";
    return false;
  }

  int32_t srcW = 0, srcH = 0;
  if (!parseJpegDimensions(jpg, len, &srcW, &srcH)) {
    free(jpg);
    if (outErr) *outErr = "JPEG has no SOFn marker (dims unknown)";
    return false;
  }
  // Reject pathological dims that would blow PSRAM on RGB888 decode.
  // 1600×1200 → 5.5 MB, comfortably under PSRAM headroom; 2048×1536 →
  // 9.4 MB, too tight. Cap at the lower bound for safety.
  if (srcW > 1600 || srcH > 1600) {
    DEBUG_G2F("[G2] JPG viewer: source too large %dx%d (max 1600 per dim)",
              (int)srcW, (int)srcH);
    free(jpg);
    if (outErr) *outErr = "JPEG dims exceed 1600 px";
    return false;
  }

  const size_t rgbLen = (size_t)srcW * (size_t)srcH * 3;
  uint8_t* rgb = (uint8_t*)ps_alloc(rgbLen, AllocPref::PreferPSRAM, "g2.jpg.rgb");
  if (!rgb) {
    free(jpg);
    if (outErr) *outErr = "out of memory for RGB888 decode";
    return false;
  }
  if (!fmt2rgb888(jpg, len, PIXFORMAT_JPEG, rgb)) {
    free(jpg);
    free(rgb);
    if (outErr) *outErr = "fmt2rgb888 decode failed";
    return false;
  }
  free(jpg);
  jpg = nullptr;

  // Build 288×144 4-bpp BMP — same target shape readBmpFromVfs
  // requires for the BMP viewer / full-viewer transports. Header
  // (118 B) + 144 × 144 px-bytes ≈ 21 KB.
  const size_t kBmpCap = 14 + 40 + 64 + (288 / 2) * 144 + 64;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.jpg.bmp");
  if (!bmp) {
    free(rgb);
    if (outErr) *outErr = "out of memory for BMP build";
    return false;
  }
  const size_t bmpLen = buildBmp4bppFromRgb888(bmp, kBmpCap, rgb, srcW, srcH);
  free(rgb);
  if (bmpLen == 0) {
    free(bmp);
    if (outErr) *outErr = "BMP build from RGB888 failed";
    return false;
  }

  if (outBmp) *outBmp = bmp;
  if (outBmpLen) *outBmpLen = bmpLen;
  if (outErr) *outErr = "";
  DEBUG_G2F("[G2] JPG viewer: loaded '%s' src=%dx%d → BMP=%u B",
            path.c_str(), (int)srcW, (int)srcH, (unsigned)bmpLen);
  return true;
}

// Single-tile JPG viewer — mirror of g2BmpViewerWorker. Decodes JPG
// into a 288×144 BMP (top-left of the lens canvas) and pushes via
// sendImageBmpMultiFragment. Magic range borrowed from the BMP
// viewer (0x20/0x21+) — the FSM probe gate prevents both viewers
// from running concurrently, so the magic overlap is benign.
struct JpgViewerArgs {
  char* path;
  void (*onDone)();
};

static void g2JpgViewerWorker(void* arg) {
  G2HijackCtxGuard ctxGuard;  // identity + NOTIF_SOURCE_G2 for VFS reads
  auto* a = (JpgViewerArgs*)arg;

  do {
    G2Temple* arm = pickEvenAIArm("jpgView");
    if (!arm) {
      DEBUG_G2F("[G2] JPG viewer: no eligible arm");
      break;
    }

    uint8_t* bmp = nullptr;
    size_t bmpLen = 0;
    const char* loadErr = "";
    if (!loadJpgAsBmp288x144(String(a->path), &bmp, &bmpLen, &loadErr)) {
      DEBUG_G2F("[G2] JPG viewer: load failed for '%s': %s",
                a->path, loadErr ? loadErr : "?");
      break;
    }

    const unsigned estFrags =
        (unsigned)((bmpLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) / G2_IMG_MAPRAW_CHUNK_BYTES);
    const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x20;  // 242
    const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x21;  // 243+
    if (kPushMagicBase + estFrags >= 256) {
      DEBUG_G2F("[G2] JPG viewer: too many fragments (%u) for magic window",
                estFrags);
      free(bmp);
      break;
    }

    if (!probeTearDownActiveContainer(*arm)) {
      DEBUG_G2F("[G2] JPG viewer: pre-push SHUTDOWN failed");
      free(bmp);
      break;
    }

    unsigned okFrags = 0, totalFrags = 0;
    (void)sendImageBmpMultiFragment(*arm, "jpgView",
                                    kCreateMagic, kPushMagicBase,
                                    bmp, bmpLen, &okFrags, &totalFrags);
    free(bmp);

    DEBUG_G2F("[G2] JPG viewer: image up — double-tap to dismiss (60 s cap)");
    const bool tapped = probeHoldUntilTapOrTimeout(60000);
    DEBUG_G2F("[G2] JPG viewer: hold ended via %s (frags %u/%u)",
              tapped ? "user tap" : "60 s timeout", okFrags, totalFrags);

    probePostProbeShutdown(*arm);
  } while (0);

  if (a->onDone) a->onDone();
  free(a->path);
  delete a;
  vTaskDelete(nullptr);
}

bool g2ShowJpgFile(const char* path, void (*onDone)()) {
  if (!path || !*path) return false;
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] JPG viewer: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new JpgViewerArgs;
  if (!a) return false;
  a->path   = strdup(path);
  a->onDone = onDone;
  if (!a->path) {
    delete a;
    return false;
  }
  if (xTaskCreate(g2JpgViewerWorker, "g2_jpg_view", 6144, a,
                  tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] JPG viewer: xTaskCreate failed");
    free(a->path);
    delete a;
    return false;
  }
  return true;
}

// Full-screen JPG viewer — mirror of g2BmpFullViewerWorker. Reuses the
// 288×144 BMP load above, then tiles to 4 quadrants via the existing
// buildTileBmpFromQuadrant helper. Same wire shape as the BMP
// full-viewer; only the load path differs.
struct JpgFullViewerArgs {
  char* path;
  void (*onDone)();
};

static void g2JpgFullViewerWorker(void* arg) {
  G2HijackCtxGuard ctxGuard;  // identity + NOTIF_SOURCE_G2 for VFS reads
  auto* a = (JpgFullViewerArgs*)arg;

  do {
    G2Temple* arm = pickEvenAIArm("jpgFullView");
    if (!arm) {
      DEBUG_G2F("[G2] JPG full-viewer: no eligible arm");
      break;
    }

    uint8_t* src = nullptr;
    size_t srcLen = 0;
    const char* loadErr = "";
    if (!loadJpgAsBmp288x144(String(a->path), &src, &srcLen, &loadErr)) {
      DEBUG_G2F("[G2] JPG full-viewer: load failed for '%s': %s",
                a->path, loadErr ? loadErr : "?");
      break;
    }

    // Per-tile staging buffer reused across the 4 pushes. Same size
    // budget as the BMP full-viewer's (header + 144*144 px-bytes).
    const size_t kTileCap = 14 + 40 + 64 + 144 * 144 + 64;
    uint8_t* tile = (uint8_t*)ps_alloc(kTileCap, AllocPref::PreferPSRAM, "g2.jpg.tile");
    if (!tile) {
      DEBUG_G2F("[G2] JPG full-viewer: tile alloc failed (%u B)",
                (unsigned)kTileCap);
      free(src);
      break;
    }

    if (!probeTearDownActiveContainer(*arm)) {
      DEBUG_G2F("[G2] JPG full-viewer: pre-push SHUTDOWN failed");
      free(src);
      free(tile);
      break;
    }

    // Push each quadrant via the same multi-fragment helper the
    // single-tile viewer uses. Magic ranges per quadrant mirror the
    // BMP full-viewer's allocation: 0x30/0x31+ (TL), 0x40/0x41+ (TR),
    // 0x50/0x51+ (BL), 0x60/0x61+ (BR). Probe FSM gates concurrent
    // viewers so overlapping with the BMP variant is fine.
    static const uint32_t kCreateMagics[4] = {
      G2_MAGIC_IMAGE_BASE + 0x30, G2_MAGIC_IMAGE_BASE + 0x40,
      G2_MAGIC_IMAGE_BASE + 0x50, G2_MAGIC_IMAGE_BASE + 0x60,
    };
    static const uint32_t kPushMagicBases[4] = {
      G2_MAGIC_IMAGE_BASE + 0x31, G2_MAGIC_IMAGE_BASE + 0x41,
      G2_MAGIC_IMAGE_BASE + 0x51, G2_MAGIC_IMAGE_BASE + 0x61,
    };
    unsigned totalOk = 0, totalSent = 0;
    for (uint8_t q = 0; q < 4; q++) {
      const size_t tileLen = buildTileBmpFromQuadrant(tile, kTileCap,
                                                       src, srcLen, q);
      if (tileLen == 0) {
        DEBUG_G2F("[G2] JPG full-viewer: tile %u build failed", (unsigned)q);
        continue;
      }
      unsigned okFrags = 0, totalFrags = 0;
      (void)sendImageBmpMultiFragment(*arm, "jpgFullView",
                                      kCreateMagics[q], kPushMagicBases[q],
                                      tile, tileLen,
                                      &okFrags, &totalFrags);
      totalOk   += okFrags;
      totalSent += totalFrags;
      // Brief inter-tile breather so the firmware can settle the
      // CREATE before we Shutdown+CREATE the next one. Same cadence
      // as the BMP full-viewer.
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    free(src);
    free(tile);

    DEBUG_G2F("[G2] JPG full-viewer: image up (%u/%u frags total) — "
              "double-tap to dismiss (60 s cap)", totalOk, totalSent);
    const bool tapped = probeHoldUntilTapOrTimeout(60000);
    DEBUG_G2F("[G2] JPG full-viewer: hold ended via %s",
              tapped ? "user tap" : "60 s timeout");

    probePostProbeShutdown(*arm);
  } while (0);

  if (a->onDone) a->onDone();
  free(a->path);
  delete a;
  vTaskDelete(nullptr);
}

bool g2ShowJpgFileFullScreen(const char* path, void (*onDone)()) {
  if (!path || !*path) return false;
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] JPG full-viewer: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new JpgFullViewerArgs;
  if (!a) return false;
  a->path   = strdup(path);
  a->onDone = onDone;
  if (!a->path) {
    delete a;
    return false;
  }
  if (xTaskCreate(g2JpgFullViewerWorker, "g2_jpg_full", 8192, a,
                  tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
    DEBUG_G2F("[G2] JPG full-viewer: xTaskCreate failed");
    free(a->path);
    delete a;
    return false;
  }
  return true;
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
  EXT_RAM_BSS_ATTR static char ret[260];

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

  noteOurShutdownSent();
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
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmp");
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
    // drains before tile N+1 starts piling up. 50 ms after each tile at the
    // current per-tile burst baseline (~2.2 s); increase if multi-tile probes
    // show cross-tile reassembly issues.
    vTaskDelay(pdMS_TO_TICKS(50));
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
  EXT_RAM_BSS_ATTR static char ret[260];

  G2Temple* arm = pickEvenAIArm("imgQ13");
  if (!arm) return "Img Q13: no reachable temple";

  const uint32_t kCreateMagic     = G2_MAGIC_IMAGE_BASE + 0x00;  // 210
  const uint32_t kFirstPushBase   = G2_MAGIC_IMAGE_BASE + 0x01;  // 211
  const int32_t  kImgW            = 288;
  const int32_t  kImgH            = 144;
  // Capped at 30 s / 12 frames. Each frame's BMP push hits the BLE
  // throughput limit at ~2.7 s (21 KB BMP + acks); the firmware's own
  // lens-idle timeout is ~30 s. Q13 was previously capped at 2 min /
  // 240 frames, which (a) deceived operators about achievable cadence
  // and (b) ran the hijack past the firmware's natural idle window,
  // forcing us to fight the 60 s hijack-safety watchdog. 30 s is the
  // honest envelope for "sustained streaming" on this BLE link;
  // double-tap dismiss is still available for early exit.
  const uint32_t kSafetyCapMs     = 30000;
  const uint32_t kSafetyCapFrames = 12;

  probeBanner("Q13: live image tile (push @ rate, no re-CREATE)",
              kCreateMagic, kFirstPushBase + 6,
              "single 288x144 container, push a fresh BMP every "
              "g2liverate ms (default 600). Each frame shifts a "
              "horizontal bar to make the update visible. Double-tap "
              "to dismiss; auto-stops at 30 s or 12 frames.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q13: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmp");
  if (!bmp) {
    probePostProbeShutdown(*arm);
    return "Img Q13: BMP heap alloc failed";
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;

  // Arm the tap-hold sentinel so the SysEvent handler will set
  // gImgProbeHoldTapPending when the user double-taps. We poll it
  // between frames rather than blocking on probeHoldUntilTapOrTimeout.
  // gImgProbeAbort is the same dismiss signal observed by the inner
  // burst senders — clear it here so a stale value from a prior probe
  // doesn't terminate this run on iter 1.
  gImgProbeHoldTapPending = false;
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = true;

  // Wrap push-magic base across frames so we never exceed uint8.
  // 7 magics per push; wrap to 211 if next 7 would cross 255.
  uint32_t pushMagicBase = kFirstPushBase;
  bool createdOnce = false;
  unsigned framesOk = 0;
  unsigned framesAttempted = 0;
  unsigned recreateCount = 0;
  const uint32_t startMs = millis();
  bool dismissed = false;
  bool lensTimedOut = false;

  while ((millis() - startMs) < kSafetyCapMs &&
         framesAttempted < kSafetyCapFrames) {
    if (gImgProbeHoldTapPending) { dismissed = true; break; }

    // Lens-idle detection. The firmware fires DISPLAY_OFF after ~30 s
    // of no ring/temple input and our event handler clears
    // containerReady. Without this check the loop happily blasts
    // image fragments into a dead container until the safety cap.
    if (createdOnce && !arm->containerReady) {
      if (gG2LiveLoopKeepAlive) {
        recreateCount++;
        DEBUG_G2F("[ImgProbe] Q13 lens idle-timeout (recreate #%u)",
                  recreateCount);
        createdOnce = false;  // next push goes through CREATE path
      } else {
        lensTimedOut = true;
        break;
      }
    }

    // Build BMP for this frame. Bar is 12 px tall and walks down by
    // 12 px per frame, wrapping at the bottom.
    size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
    if (bmpLen == 0) break;
    const int32_t barY = (int32_t)((framesAttempted * 12) % (uint32_t)(kImgH - 12));
    bmpDrawRect4bpp(bmp + kPixOffset, kImgW, -kImgH,
                    /*x*/ 0, /*y*/ barY, /*w*/ kImgW, /*h*/ 12, /*idx*/ 15);

    const unsigned fragsThisFrame =
        (unsigned)((bmpLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) / G2_IMG_MAPRAW_CHUNK_BYTES);
    // Wrap push-magic window if the next burst would cross uint8.
    if (pushMagicBase + fragsThisFrame > 256) pushMagicBase = kFirstPushBase;

    const uint32_t frameStartMs = millis();
    bool ok = false;
    if (!createdOnce) {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpMultiFragment(*arm, "Q13",
                                     kCreateMagic, pushMagicBase,
                                     bmp, bmpLen, &okFrags, &totalFrags);
      createdOnce = ok;
      // Stamp lens/arm container state on first successful CREATE-image so the
      // lens-idle gate above doesn't trip on iter 2. sendImageBmpMultiFragment
      // doesn't do this itself — by contract (sendCreateAndWait note) the
      // caller is responsible. Without this, Q13 bails as "lens timeout"
      // after a single frame even though the container is alive.
      if (ok) {
        arm->containerReady  = true;
        arm->containerIsList = false;
        g2LensSetContainer(true, false, BLOCKS_WIDGET_ID);
      }
    } else {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpFragmentsNoCreate(*arm, "Q13",
                                          pushMagicBase,
                                          /*cid*/ 2, /*cname*/ "imgQ4",
                                          bmp, bmpLen, &okFrags, &totalFrags);
    }
    pushMagicBase += fragsThisFrame;
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

  const char* reason = dismissed     ? "user tap"
                     : lensTimedOut  ? "lens timeout"
                                     : "safety cap";
  const uint32_t totalMs = millis() - startMs;
  DEBUG_G2F("[ImgProbe] Q13 ended (%s): %u/%u frames %u recreates in %u ms "
            "(avg %u ms/frame)",
            reason, framesOk, framesAttempted, recreateCount, (unsigned)totalMs,
            framesAttempted ? (unsigned)(totalMs / framesAttempted) : 0u);

  probePostProbeShutdown(*arm);
  probeFooter("Q13", framesOk, framesAttempted);
  snprintf(ret, sizeof(ret),
           "Img Q13: %u/%u frames in %u ms, %u re-creates, end via %s",
           framesOk, framesAttempted, (unsigned)totalMs,
           recreateCount, reason);
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
  EXT_RAM_BSS_ATTR static char ret[260];

  if (!gR.connected && !gL.connected) {
    return "Img Q14: no reachable temple";
  }

  // Capped at 30 s / 60 frames — see Q13 rationale. Q14 is text-only
  // (REBUILD per tick), so each "frame" is much cheaper than Q13's full
  // BMP push, but the firmware's lens-idle window is the same ~30 s, so
  // running longer is still fighting natural lens lifecycle. Operators
  // wanting sustained text-on-lens benches can re-arm via the test menu.
  const uint32_t kSafetyCapMs     = 30000;
  const uint32_t kSafetyCapFrames = 60;

  probeBanner("Q14: live TEXT (REBUILD @ rate)",
              /*magicLo*/ G2_MAGIC_CREATE, /*magicHi*/ G2_MAGIC_REBUILD,
              "g2ShowText() loop. First call CREATEs the TEXT widget, "
              "subsequent calls REBUILD_PAGE — single envelope per "
              "update. Cadence via g2liverate (default 600 ms). "
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
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = true;

  unsigned framesOk = 0;
  unsigned framesAttempted = 0;
  unsigned recreateCount = 0;
  const uint32_t startMs = millis();
  bool dismissed = false;
  bool lensTimedOut = false;

  while ((millis() - startMs) < kSafetyCapMs &&
         framesAttempted < kSafetyCapFrames) {
    if (gImgProbeHoldTapPending) { dismissed = true; break; }

    // Lens-idle detection — same gate as Q13. g2ShowText's first call
    // CREATEs the TEXT widget, subsequent calls REBUILD. When DISPLAY_OFF
    // fires the firmware tears the widget down and our event handler
    // clears containerReady; the next g2ShowText auto-CREATEs a fresh
    // widget. With keep-alive off, we break here so the loop stops cleanly
    // instead of letting g2ShowText churn through the recreate cycle.
    G2Temple* armCheck = pickEvenAIArm("imgQ14");
    if (framesAttempted > 0 && armCheck && !armCheck->containerReady) {
      if (gG2LiveLoopKeepAlive) {
        recreateCount++;
        DEBUG_G2F("[ImgProbe] Q14 lens idle-timeout (auto-recreate #%u)",
                  recreateCount);
        // No state to clear — g2ShowText handles re-CREATE itself on the
        // next call when containerReady is false.
      } else {
        lensTimedOut = true;
        break;
      }
    }

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

  const char* reason = dismissed     ? "user tap"
                     : lensTimedOut  ? "lens timeout"
                                     : "safety cap";
  const uint32_t totalMs = millis() - startMs;
  DEBUG_G2F("[ImgProbe] Q14 ended (%s): %u/%u rebuilds %u recreates in %u ms "
            "(avg %u ms/frame)",
            reason, framesOk, framesAttempted, recreateCount, (unsigned)totalMs,
            framesAttempted ? (unsigned)(totalMs / framesAttempted) : 0u);

  // Q14 uses a TEXT widget — clean it up via the standard hijack path
  // so the next probe / picker rebuild starts from a known state.
  G2Temple* arm = pickEvenAIArm("imgQ14");
  if (arm) probePostProbeShutdown(*arm);
  probeFooter("Q14", framesOk, framesAttempted);
  snprintf(ret, sizeof(ret),
           "Img Q14: %u/%u rebuilds in %u ms, %u re-creates, end via %s",
           framesOk, framesAttempted, (unsigned)totalMs,
           recreateCount, reason);
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
//   * On firmware 2.2.0.24, ALL notify traffic (incl. cmd=4 ImageRawResp
//     acks) arrives on the RIGHT pipe — see docs/G2_PROTOCOL.md "Notify
//     channel topology on firmware 2.2.0.24 — right-only" and
//     g2-kit-unofficial/ble/docs/gotchas.md "Arms" section. So even
//     when we TX to LEFT, acks come back via RIGHT and the existing
//     ack-tracking path still works.
//   * Because L never acks heartbeats on this firmware, beatOne()
//     skips the miss-counter for L (firmwareSilencesLeftNotify gate),
//     keeping gL.pluginDead=false in steady state. The pluginDead
//     check below remains as a safety net for the case where some
//     other code path (e.g. an explicit teardown) sets it.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ15LeftArm() {
  EXT_RAM_BSS_ATTR static char ret[260];

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
  const size_t   kChunkBytes     = G2_IMG_MAPRAW_CHUNK_BYTES;

  probeBanner("Q15: LEFT-arm image push (288x144, baseline-vs-LEFT test)",
              kCreateMagic, kPushMagicBase + 7,
              "same BMP / transport / fragmentation as Q6 but TXed via "
              "LEFT temple. Compare burst time to Q6's ~2.6 s baseline. "
              "Per Discord 2026-04-28 left may be faster — verify.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q15: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmp");
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
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.bmp");
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
  EXT_RAM_BSS_ATTR static char ret[260];
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
  EXT_RAM_BSS_ATTR static char ret[260];
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
  EXT_RAM_BSS_ATTR static char ret[260];
  const G2ContainerGeom listGeom = G2_GEOM_LARGE;
  // 80×80 icon in the top-right corner. 80 px @ 4 bpp = 40 bytes/row,
  // already 4-byte aligned for BMP stride; total pixel data = 3200 B,
  // header + palette = 118 B → 3318 B BMP = single Cmd=3 fragment.
  return runMixedListImageProbe("Q18", listGeom,
                                /*imgX*/ 488, /*imgY*/ 8,
                                /*imgW*/ 80, /*imgH*/ 80,
                                ret, sizeof(ret));
}

// ─────────────────────────────────────────────────────────────────────
// Q19 — solo small-dim image. CREATE-image at 96×96 (not mixed with a
// list), push a stripes BMP, hold for tap. Goal: confirm whether the
// firmware renders solo image widgets below the 288×144 we've used
// historically. If yes, the camera streamer can use 96×96 as a
// "small-but-fast" mode (~1 fps vs 0.45 fps at 288×144).
//
// Q18 already pushes 80×80 in a *mixed* list+image widget, but mixed
// CREATE uses a different protobuf path (g2BuildCreateMixedListImage)
// that may render images differently from solo CREATE-image. Q19
// isolates the solo path.
//
// Math: 96×96 4bpp BMP = 14+40+64+(96/2)*96 = 4726 B → 2 fragments.
// At 15 ms inter-frag gap with cap=3 in-flight, push wall-clock should
// be ~550 ms (order-of-magnitude; BLE pacing dominates).
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ19SmallSolo() {
  static char ret[220];

  G2Temple* arm = pickEvenAIArm("imgQ19");
  if (!arm) return "Img Q19: no reachable temple";

  const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x16;  // 230
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x17;  // 231..232 (2 frags)
  const int32_t  kImgW          = 96;
  const int32_t  kImgH          = 96;

  probeBanner("Q19: solo 96×96 image",
              kCreateMagic, kPushMagicBase + 1,
              "tests whether solo CREATE-image renders below 288×144. "
              "If the lens shows a stripes pattern, small-dim solo "
              "rendering works — green-light for fast streaming. "
              "If lens stays blank but acks come back, firmware "
              "accepts the data but won't render at this size.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q19: pre-burst SHUTDOWN failed";

  // 96×96 4bpp BMP: ~4.7 KB. Allocate 8 KB for headroom.
  const size_t kBmpCap = 8 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.q19.bmp");
  if (!bmp) {
    probePostProbeShutdown(*arm);
    return "Img Q19: BMP heap alloc failed";
  }
  // Stripes pattern, top-down (negative height) for intuitive on-lens
  // orientation — same convention Q18 uses.
  const size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(*arm);
    return "Img Q19: BMP build failed";
  }
  DEBUG_G2F("[ImgProbe] Q19 BMP %u B (%dx%d 4bpp stripes)",
            (unsigned)bmpLen, (int)kImgW, (int)kImgH);

  unsigned okFrags = 0, totalFrags = 0;
  (void)sendImageBmpMultiFragment(*arm, "Q19", kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, &totalFrags,
                                  /*tolerateMissedAcks*/ 0,
                                  kImgW, kImgH);
  free(bmp);

  DEBUG_G2F("[ImgProbe] Q19 — image up, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q19 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(*arm);
  probeFooter("Q19", okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img Q19: solo 96×96, %u/%u frags acked — %s — "
           "did the lens render the stripes?",
           okFrags, totalFrags, tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q29 — 2-bpp BMP solo. Same shape as Q19 (CREATE-image, multi-fragment
// push, hold for tap) but the BMP is 2-bpp grayscale instead of 4-bpp.
// Tests whether the lens firmware's BMP parser honours biBitCount=2 and
// a 4-entry palette. If it renders the stripes pattern, the camera
// streamer can drop payload by ~50% with no resolution change. If lens
// stays blank but acks come back, parser is hardcoded to 4-bpp.
//
// Math at 144×144 2-bpp: 14 + 40 + 16 + (144*2/8)*144 = 70 + 5184 =
// 5254 B → ~2 fragments (vs 3 frags / 10486 B at 4-bpp). If decode
// works, expected wall-clock ≈ 500 ms vs ~770 ms at 4-bpp.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ29Bmp2bppSolo() {
  static char ret[240];

  G2Temple* arm = pickEvenAIArm("imgQ29");
  if (!arm) return "Img Q29: no reachable temple";

  const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x28;  // 248
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x29;  // 249..250 (~2 frags)
  const int32_t  kImgW          = 144;
  const int32_t  kImgH          = 144;

  probeBanner("Q29: 2-bpp solo 144×144 image",
              kCreateMagic, kPushMagicBase + 1,
              "tests whether lens firmware decodes biBitCount=2 BMPs. "
              "Pixel data is half the size of 4-bpp at the same dims. "
              "If lens shows stripes, 2-bpp is supported and camera "
              "streaming can cut payload ~50%. If lens is blank but "
              "acks come back, parser is 4-bpp only.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q29: pre-burst SHUTDOWN failed";

  // 2-bpp 144×144 BMP: ~5.3 KB. Allocate 8 KB for headroom.
  const size_t kBmpCap = 8 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.q29.bmp");
  if (!bmp) {
    probePostProbeShutdown(*arm);
    return "Img Q29: BMP heap alloc failed";
  }
  const size_t bmpLen = buildBmp2bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(*arm);
    return "Img Q29: BMP build failed";
  }
  DEBUG_G2F("[ImgProbe] Q29 BMP %u B (%dx%d 2bpp stripes)",
            (unsigned)bmpLen, (int)kImgW, (int)kImgH);

  unsigned okFrags = 0, totalFrags = 0;
  (void)sendImageBmpMultiFragment(*arm, "Q29", kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, &totalFrags,
                                  /*tolerateMissedAcks*/ 0,
                                  kImgW, kImgH);
  free(bmp);

  DEBUG_G2F("[ImgProbe] Q29 — image up, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q29 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(*arm);
  probeFooter("Q29", okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img Q29: 2-bpp 144×144, %u/%u frags acked — %s — "
           "did the lens render the stripes? (blank=parser is 4-bpp only)",
           okFrags, totalFrags, tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q20 / Q22 / Q23 / Q26 / Q27 — shared solo live tile: stripes + moving bar,
// double-tap / 30 s safety window. Each probe uses disjoint magic bands so
// logs stay grep-clean.
//
// `framePaceMs`: minimum interval between frame starts. Pass 0 to use
// the global `gG2LiveRateMs` (CLI `g2liverate`). Smaller values push
// the lens harder when BLE + firmware keep up (see log line timings).
// ─────────────────────────────────────────────────────────────────────
static const char* probeLiveScrollingBarSolo(
    char* ret, size_t retCap,
    const char* probeId,
    const char* pickTag,
    uint32_t createMagic,
    uint32_t firstPushBase,
    int32_t imgW, int32_t imgH, int32_t barH,
    uint32_t safetyCapFrames,
    uint32_t framePaceMs,
    const char* bannerTitle,
    const char* bannerSub) {
  G2Temple* arm = pickEvenAIArm(pickTag);
  if (!arm) {
    snprintf(ret, retCap, "Img %s: no reachable temple", probeId);
    return ret;
  }

  const uint32_t kSafetyCapMs = 30000;
  const uint32_t bannerMagicHi = firstPushBase + 7;

  probeBanner(bannerTitle, createMagic, bannerMagicHi, bannerSub);
  if (!probeTearDownActiveContainer(*arm)) {
    snprintf(ret, retCap, "Img %s: pre-burst SHUTDOWN failed", probeId);
    return ret;
  }

  size_t kBmpCap = bmp4bppFileBytesU32((uint32_t)imgW, (uint32_t)imgH) + 256;
  if (kBmpCap < 4096) kBmpCap = 4096;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.liveBar");
  if (!bmp) {
    probePostProbeShutdown(*arm);
    snprintf(ret, retCap, "Img %s: BMP heap alloc failed", probeId);
    return ret;
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;

  gImgProbeHoldTapPending = false;
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = true;

  const uint32_t paceBudgetMs =
      (framePaceMs == 0) ? gG2LiveRateMs : framePaceMs;

  uint32_t pushMagicBase = firstPushBase;
  bool     createdOnce   = false;
  unsigned framesOk = 0;
  unsigned framesAttempted = 0;
  unsigned recreateCount = 0;
  const uint32_t startMs = millis();
  bool dismissed = false;
  bool lensTimedOut = false;

  while ((millis() - startMs) < kSafetyCapMs &&
         framesAttempted < safetyCapFrames) {
    if (gImgProbeHoldTapPending) {
      dismissed = true;
      break;
    }

    if (createdOnce && !arm->containerReady) {
      if (gG2LiveLoopKeepAlive) {
        recreateCount++;
        DEBUG_G2F("[ImgProbe] %s lens idle-timeout (recreate #%u)",
                  probeId, recreateCount);
        createdOnce = false;
      } else {
        lensTimedOut = true;
        break;
      }
    }

    size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, imgW, -imgH, BMP_PAT_STRIPES);
    if (bmpLen == 0) break;
    const int32_t denom = imgH - barH;
    const int32_t barY = (denom > 0)
        ? (int32_t)((framesAttempted * (uint32_t)barH) % (uint32_t)denom)
        : 0;
    bmpDrawRect4bpp(bmp + kPixOffset, imgW, -imgH,
                    /*x*/ 0, /*y*/ barY, /*w*/ imgW, /*h*/ barH, /*idx*/ 15);

    const unsigned fragsThisFrame =
        (unsigned)((bmpLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) / G2_IMG_MAPRAW_CHUNK_BYTES);
    if (pushMagicBase + fragsThisFrame > 256) pushMagicBase = firstPushBase;

    const uint32_t frameStartMs = millis();
    bool ok = false;
    if (!createdOnce) {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpMultiFragment(*arm, probeId,
                                     createMagic, pushMagicBase,
                                     bmp, bmpLen, &okFrags, &totalFrags,
                                     /*tolerateMissedAcks*/ 0,
                                     imgW, imgH);
      createdOnce = ok;
      if (ok) {
        arm->containerReady  = true;
        arm->containerIsList = false;
        g2LensSetContainer(true, false, BLOCKS_WIDGET_ID);
      }
    } else {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpFragmentsNoCreate(*arm, probeId,
                                         pushMagicBase,
                                         /*cid*/ 2, /*cname*/ "imgQ4",
                                         bmp, bmpLen, &okFrags, &totalFrags);
    }
    pushMagicBase += fragsThisFrame;
    framesAttempted++;
    if (ok) framesOk++;

    const uint32_t frameMs = millis() - frameStartMs;
    DEBUG_G2F("[ImgProbe] %s frame %u: %s in %u ms (pace=%u ms)",
              probeId, framesAttempted, ok ? "OK" : "FAIL", (unsigned)frameMs,
              (unsigned)paceBudgetMs);

    if (frameMs < paceBudgetMs) {
      const uint32_t sleepMs = paceBudgetMs - frameMs;
      const uint32_t sleepStart = millis();
      while ((millis() - sleepStart) < sleepMs) {
        if (gImgProbeHoldTapPending) {
          dismissed = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      if (dismissed) break;
    }
  }
  gImgProbeHoldActive = false;
  free(bmp);

  const char* reason = dismissed    ? "user tap"
                     : lensTimedOut ? "lens timeout"
                                    : "safety cap";
  const uint32_t totalMs = millis() - startMs;
  DEBUG_G2F("[ImgProbe] %s ended (%s): %u/%u frames %u recreates in %u ms "
            "(avg %u ms/frame)",
            probeId, reason, framesOk, framesAttempted, recreateCount, (unsigned)totalMs,
            framesAttempted ? (unsigned)(totalMs / framesAttempted) : 0u);

  probePostProbeShutdown(*arm);
  probeFooter(probeId, framesOk, framesAttempted);
  snprintf(ret, retCap,
           "Img %s: %u/%u frames (%dx%d) in %u ms, %u re-creates, end via %s",
           probeId, framesOk, framesAttempted, (int)imgW, (int)imgH,
           (unsigned)totalMs, recreateCount, reason);
  return ret;
}

// Q20 — live 96×96 (Animated Icons menu). Same transport as Q13 bar
// pattern; fewer Cmd=3 fragments per frame than 288×144.
const char* g2ProbeImageQ20LiveTile96() {
  EXT_RAM_BSS_ATTR static char ret[260];
  return probeLiveScrollingBarSolo(
      ret, sizeof(ret),
      "Q20", "imgQ20",
      G2_MAGIC_IMAGE_BASE + 0x24, G2_MAGIC_IMAGE_BASE + 0x25,
      96, 96, 8,
      /*safetyCapFrames*/ 45,
      /*framePaceMs*/ 0,
      "Q20: live 96x96 tile @ rate (Animated Icons)",
      "stripes + bar in 96x96 solo container; g2liverate ms between frames "
      "(default 600). Logs measured ms/frame vs requested cadence.");
}

// Q22 — live 32×32 scrolling bar (icon-sized cadence bench).
const char* g2ProbeImageQ22LiveTile32() {
  EXT_RAM_BSS_ATTR static char ret[260];
  return probeLiveScrollingBarSolo(
      ret, sizeof(ret),
      "Q22", "imgQ22",
      G2_MAGIC_IMAGE_BASE + 39, G2_MAGIC_IMAGE_BASE + 40,  // 249, 250
      32, 32, 4,
      /*safetyCapFrames*/ 200,
      /*framePaceMs*/ 280,
      "Q22: live 32x32 bar @ rate (Animated Icons)",
      "same bar pattern as Q20 at 32x32; fixed ~280 ms pace (override "
      "g2liverate for this probe). Compare vs Q23/Q20.");
}

// Q23 — live 64×64 scrolling bar.
const char* g2ProbeImageQ23LiveTile64() {
  EXT_RAM_BSS_ATTR static char ret[260];
  return probeLiveScrollingBarSolo(
      ret, sizeof(ret),
      "Q23", "imgQ23",
      G2_MAGIC_IMAGE_BASE + 43, G2_MAGIC_IMAGE_BASE + 44,  // 253, 254
      64, 64, 6,
      /*safetyCapFrames*/ 120,
      /*framePaceMs*/ 380,
      "Q23: live 64x64 bar @ rate (Animated Icons)",
      "same bar pattern as Q20 at 64x64; fixed ~380 ms pace (more payload "
      "than Q22). Compare ms/frame vs 32/96.");
}

// Q26 — live 124×124 scrolling bar (between 96 and max solo tile).
const char* g2ProbeImageQ26LiveTile124() {
  EXT_RAM_BSS_ATTR static char ret[260];
  return probeLiveScrollingBarSolo(
      ret, sizeof(ret),
      "Q26", "imgQ26",
      G2_MAGIC_IMAGE_BASE + 34, G2_MAGIC_IMAGE_BASE + 35,  // 244, 245
      124, 124, 10,
      /*safetyCapFrames*/ 32,
      /*framePaceMs*/ 0,
      "Q26: live 124x124 bar @ rate (Animated Icons)",
      "same stripe+bar pattern as Q20; cadence g2liverate. Heavier BLE load "
      "than 96x96 — watch fragment timings.");
}

// Q27 — live 144×144 scrolling bar (max single-image container).
const char* g2ProbeImageQ27LiveTile144() {
  EXT_RAM_BSS_ATTR static char ret[260];
  return probeLiveScrollingBarSolo(
      ret, sizeof(ret),
      "Q27", "imgQ27",
      G2_MAGIC_IMAGE_BASE + 26, G2_MAGIC_IMAGE_BASE + 27,  // 236, 237
      144, 144, 12,
      /*safetyCapFrames*/ 24,
      /*framePaceMs*/ 0,
      "Q27: live 144x144 bar @ rate (Animated Icons)",
      "largest solo-tile bar probe; cadence g2liverate. May need higher "
      "g2liverate ms/frame than Q20 if lens drops frames.");
}

// (Q24 procedural slime probe removed — replaced operationally by Q25
// SD-frame BMP packs, which serve the same "loop animated frames"
// purpose with arbitrary user content.)


// ─────────────────────────────────────────────────────────────────────
// Q25 — loop 4bpp BMPs from a VFS directory (frame_00.bmp …). Path is set
// via g2ProbeImageQ25SetPackPath before the test worker runs. Only subdirs of
// G2_ICON_ANIMATIONS_VFS_PATH subdirs are accepted (see gif_to_g2_bmps.py).
// All frames are read from SD/VFS once into PSRAM (g2ReadBmp4bppFromVfs); the
// animation loop only reuses those buffers — no per-frame card reads.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ25SdFrameAnimation() {
  EXT_RAM_BSS_ATTR static char ret[320];
  static char pathBuf[192];

  G2Temple* arm = pickEvenAIArm("imgQ25");
  if (!arm) {
    snprintf(ret, sizeof(ret), "Img Q25: no reachable temple");
    g2ProbeImageQ25SetPackPath(nullptr);
    return ret;
  }
  if (!s_q25PackDir[0]) {
    snprintf(ret, sizeof(ret), "Img Q25: no pack directory set");
    g2ProbeImageQ25SetPackPath(nullptr);
    return ret;
  }

  // Cap on consecutive frame_XX.bmp files scanned; PSRAM holds only frameCount
  // loaded buffers (see loop below), not kQ25PackMaxFrames worth of BMPs.
  static constexpr unsigned kQ25PackMaxFrames = 64;
  unsigned frameCount = 0;
  for (unsigned i = 0; i < kQ25PackMaxFrames; i++) {
    snprintf(pathBuf, sizeof(pathBuf), "%s/frame_%02u.bmp", s_q25PackDir, i);
    if (!VFS::existsGuarded(String(pathBuf), currentAuthContext())) break;
    frameCount++;
  }
  if (frameCount == 0) {
    snprintf(ret, sizeof(ret), "Img Q25: no frame_00.bmp in %s", s_q25PackDir);
    g2ProbeImageQ25SetPackPath(nullptr);
    return ret;
  }

  uint8_t* frameCache[kQ25PackMaxFrames] = {};
  size_t   frameLen[kQ25PackMaxFrames]   = {};
  int32_t  imgW = 0;
  int32_t  imgH = 0;
  const char* loadErr = "";

  for (unsigned i = 0; i < frameCount; i++) {
    snprintf(pathBuf, sizeof(pathBuf), "%s/frame_%02u.bmp", s_q25PackDir, i);
    uint8_t* bmp = nullptr;
    size_t   blen = 0;
    int32_t  fw = 0, fh = 0;
    loadErr = "";
    if (!g2ReadBmp4bppFromVfs(pathBuf, &bmp, &blen, &fw, &fh, &loadErr)) {
      for (unsigned j = 0; j < i; j++) {
        if (frameCache[j]) {
          free(frameCache[j]);
          frameCache[j] = nullptr;
        }
      }
      snprintf(ret, sizeof(ret), "Img Q25: frame_%02u: %s", i,
               loadErr ? loadErr : "?");
      g2ProbeImageQ25SetPackPath(nullptr);
      return ret;
    }
    if (i == 0) {
      imgW = fw;
      imgH = fh;
    } else if (fw != imgW || fh != imgH) {
      free(bmp);
      for (unsigned j = 0; j < i; j++) {
        if (frameCache[j]) {
          free(frameCache[j]);
          frameCache[j] = nullptr;
        }
      }
      snprintf(ret, sizeof(ret), "Img Q25: frame_%02u size mismatch", i);
      g2ProbeImageQ25SetPackPath(nullptr);
      return ret;
    }
    frameCache[i] = bmp;
    frameLen[i]   = blen;
    // Yield between frame reads so the IDLE task on this core can reset
    // the task watchdog. Without it, loading a multi-frame pack
    // (especially large ones) trips `task_wdt: IDLE1 (CPU 1) ...
    // g2_img_probe` because the SD reads stay on-CPU long enough to
    // starve the idle task. 1 tick (≈1 ms) is enough — load time
    // overhead is negligible compared to BLE push time downstream.
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  const int32_t cw = (imgW < 0) ? -imgW : imgW;
  const int32_t ch = (imgH < 0) ? -imgH : imgH;

  const uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 10;  // 220
  const uint32_t kFirstPushBase = G2_MAGIC_IMAGE_BASE + 11;  // 221
  const uint32_t kSafetyCapMs     = 30000;
  const uint32_t kSafetyCapFrames = 500;
  // Q25 cadence is user-tunable via gSettings.g2PackRateMs (CLI: g2packrate).
  // Independent of g2liverate so animation playback speed is decoupled from
  // the live-tile test cadence. Floor 20 ms keeps the worker from busy-looping;
  // upper bound 2000 ms is the setting range. For large frames the BLE push
  // dominates and this value is effectively ignored.
  const uint32_t kPaceMs          = (uint32_t)gSettings.g2PackRateMs;

  probeBanner("Q25: PSRAM-cached BMP loop", kCreateMagic, kFirstPushBase + 10,
              s_q25PackDir);
  if (!probeTearDownActiveContainer(*arm)) {
    for (unsigned i = 0; i < frameCount; i++) {
      if (frameCache[i]) {
        free(frameCache[i]);
        frameCache[i] = nullptr;
      }
    }
    snprintf(ret, sizeof(ret), "Img Q25: pre-burst SHUTDOWN failed");
    g2ProbeImageQ25SetPackPath(nullptr);
    return ret;
  }

  gImgProbeHoldTapPending = false;
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = true;

  const uint32_t paceBudgetMs  = kPaceMs;
  uint32_t       pushMagicBase = kFirstPushBase;
  bool           createdOnce   = false;
  unsigned       framesOk      = 0;
  unsigned       framesAttempted = 0;
  unsigned       recreateCount = 0;
  const uint32_t startMs       = millis();
  bool           dismissed     = false;
  bool           lensTimedOut  = false;

  while ((millis() - startMs) < kSafetyCapMs &&
         framesAttempted < kSafetyCapFrames) {
    if (gImgProbeHoldTapPending) {
      dismissed = true;
      break;
    }
    if (createdOnce && !arm->containerReady) {
      if (gG2LiveLoopKeepAlive) {
        recreateCount++;
        DEBUG_G2F("[ImgProbe] Q25 lens idle-timeout (recreate #%u)",
                  recreateCount);
        createdOnce = false;
      } else {
        lensTimedOut = true;
        break;
      }
    }

    const unsigned fi = (unsigned)(framesAttempted % frameCount);
    uint8_t* const bmp    = frameCache[fi];
    const size_t   bmpLen = frameLen[fi];

    const unsigned fragsThisFrame =
        (unsigned)((bmpLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) /
                   G2_IMG_MAPRAW_CHUNK_BYTES);
    if (pushMagicBase + fragsThisFrame > 256) pushMagicBase = kFirstPushBase;

    const uint32_t frameStartMs = millis();
    bool           ok           = false;
    if (!createdOnce) {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpMultiFragment(*arm, "Q25", kCreateMagic, pushMagicBase,
                                      bmp, bmpLen, &okFrags, &totalFrags,
                                      /*tolerateMissedAcks*/ 0, cw, ch);
      createdOnce = ok;
      if (ok) {
        arm->containerReady  = true;
        arm->containerIsList = false;
        g2LensSetContainer(true, false, BLOCKS_WIDGET_ID);
      }
    } else {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpFragmentsNoCreate(*arm, "Q25", pushMagicBase,
                                          /*cid*/ 2, /*cname*/ "imgAnim",
                                          bmp, bmpLen, &okFrags, &totalFrags);
    }
    pushMagicBase += fragsThisFrame;
    framesAttempted++;
    if (ok) framesOk++;

    const uint32_t frameMs = millis() - frameStartMs;
    DEBUG_G2F("[ImgProbe] Q25 frame %u: %s in %u ms (pace=%u ms)",
              framesAttempted, ok ? "OK" : "FAIL", (unsigned)frameMs,
              (unsigned)paceBudgetMs);

    if (frameMs < paceBudgetMs) {
      const uint32_t sleepMs = paceBudgetMs - frameMs;
      const uint32_t sleepStart = millis();
      while ((millis() - sleepStart) < sleepMs) {
        if (gImgProbeHoldTapPending) {
          dismissed = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      if (dismissed) break;
    }
  }
  gImgProbeHoldActive = false;

  const char* reason = dismissed    ? "user tap"
                     : lensTimedOut ? "lens timeout"
                                    : "safety cap";
  const uint32_t totalMs = millis() - startMs;
  DEBUG_G2F("[ImgProbe] Q25 ended (%s): %u/%u frames %u recreates in %u ms",
            reason, framesOk, framesAttempted, recreateCount, (unsigned)totalMs);

  probePostProbeShutdown(*arm);
  probeFooter("Q25", framesOk, framesAttempted);

  for (unsigned i = 0; i < frameCount; i++) {
    if (frameCache[i]) {
      free(frameCache[i]);
      frameCache[i] = nullptr;
    }
  }

  snprintf(ret, sizeof(ret),
           "Img Q25: %u/%u frames (%u cached PSRAM, %dx%d) in %u ms, %u re-creates, "
           "end via %s",
           framesOk, framesAttempted, frameCount, (int)cw, (int)ch,
           (unsigned)totalMs, recreateCount, reason);
  g2ProbeImageQ25SetPackPath(nullptr);
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q28 — Mixed Image+Text compound with INDEPENDENT refresh rates.
//
// Hypothesis: a CREATE carrying both a TextObject and an ImageObject
// (one container, two children) accepts BOTH:
//   - Cmd=3 ImageMapRaw fragments addressed to the image child by name
//     (the "camera-stream" pattern, applied here to procedural BMPs)
//   - Cmd=7 single-child REBUILD-text addressed to the text child by name
// without either disturbing the other. docs/G2_PROTOCOL.md verifies that
// single-child REBUILD-text on a list+text compound preserves the list
// (Q-series probe g2ProbeRebuildTextChild). image+text is structurally
// the same shape (different-widget-types case), so the preservation should
// extend. Same-type compounds (text+text) have a known-broken multi-child
// blanking bug — different case, doesn't apply here.
//
// Pace: alternating BMP A/B every 750 ms (fast-cycling 96×96 patterns,
// visibly different so dropped frames are obvious), text rebuild every
// other frame (~1.5 s) showing a frame counter + elapsed-ms. 24-frame
// cap (~18 s), double-tap exits early.
//
// If this passes: the camera-stream worker can be refactored to optionally
// CREATE a compound and emit per-frame text alongside live frames (e.g.
// fps/dims/sensor readings). If it fails: existing solo-image stream
// stays as-is, no production code touched — that's the whole point of
// keeping this as a separate probe.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ28MixedImageTextLive() {
  EXT_RAM_BSS_ATTR static char ret[300];
  G2Temple* arm = pickEvenAIArm("imgQ28");
  if (!arm) {
    snprintf(ret, sizeof(ret), "Img Q28: no reachable temple");
    return ret;
  }

  static constexpr uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x10;  // 226
  static constexpr uint32_t kPushSlotStart = G2_MAGIC_IMAGE_BASE + 0x11;  // 227
  static constexpr uint32_t kMaxPushMagic  = 254;                          // uint8 ceiling
  static constexpr int32_t  kImgW   = 96;
  static constexpr int32_t  kImgH   = 96;
  static constexpr unsigned kFrames = 24;
  static constexpr uint32_t kPaceMs = 750;     // per-frame target
  static const char* const kImgChild = "imgQ28";
  static const char* const kTxtChild = "txtQ28";

  // Magic cycling — firmware constrains magic to uint8 (≤255). After
  // CREATE we have 28 push magics available (227..254), and each frame's
  // burst occupies fragsPerFrame consecutive magics. We cycle through
  // floor(28 / fragsPerFrame) slots so consecutive frames don't reuse
  // the same magic range. 96×96 4-bpp BMP is 2 fragments → 14 slots.
  // Without this cycling, the 15th frame wraps past 255 and the firmware
  // reassembler stops acking. Same pattern as the camera stream worker.
  static constexpr unsigned kFragsPerFrame = 2;  // 96×96 4-bpp = ~4.7 KB
  static constexpr unsigned kPushAvailable =
      (unsigned)(kMaxPushMagic - kPushSlotStart + 1);
  static constexpr unsigned kNumSlots = kPushAvailable / kFragsPerFrame;

  probeBanner("Q28: mixed image+text live (independent refresh)",
              kCreateMagic, kMaxPushMagic,
              "compound CREATE: text top half + image 96x96 centered "
              "in bottom half. Image push every ~750ms, text rebuild "
              "every other frame. Double-tap to exit.");

  if (!probeTearDownActiveContainer(*arm)) {
    snprintf(ret, sizeof(ret), "Img Q28: pre-burst SHUTDOWN failed");
    return ret;
  }

  // Geometry: text top (8,0,560,130 — same as Q16's list slot), image
  // 96x96 centered horizontally in the bottom half (240,168). No overlap.
  G2TextChildSpec textChild = {};
  textChild.containerId   = 1;
  textChild.containerName = kTxtChild;
  textChild.eventCapture  = false;
  textChild.content       = "Q28 starting...";
  textChild.geom          = { 8, 0, 560, 130 };

  G2ImageTile imgTile = {};
  imgTile.x = 240; imgTile.y = 168;
  imgTile.w = (uint32_t)kImgW;
  imgTile.h = (uint32_t)kImgH;
  imgTile.containerId   = 2;
  imgTile.containerName = kImgChild;

  uint8_t createBuf[1024];
  size_t createLen = g2BuildCreateMixedImageText(
      allocSeq(), kCreateMagic,
      textChild, imgTile,
      BLOCKS_WIDGET_ID,
      createBuf, sizeof(createBuf));
  if (createLen == 0) {
    snprintf(ret, sizeof(ret), "Img Q28: CREATE-mixed build failed");
    return ret;
  }

  probePrepImageCreateAck(kCreateMagic);
  if (!sendEnvelope(*arm, createBuf, createLen)) {
    snprintf(ret, sizeof(ret), "Img Q28: CREATE-mixed TX failed");
    return ret;
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs)) {
    probePostProbeShutdown(*arm);
    snprintf(ret, sizeof(ret),
             "Img Q28: CREATE-mixed ack timeout — firmware may not accept "
             "image+text composition (image+list verified, image+text new)");
    return ret;
  }
  DEBUG_G2F("[ImgProbe] Q28 CREATE-mixed acked (text@'%s' + image@'%s' %dx%d)",
            kTxtChild, kImgChild, (int)kImgW, (int)kImgH);

  // Two procedural BMPs (stripes vs all-black) — alternation makes
  // dropped frames eye-visible. 96x96 4-bpp BMP is ~4.7 KB → 2 fragments
  // per push at G2_IMG_MAPRAW_CHUNK_BYTES.
  const size_t kBmpCap = 8 * 1024;
  uint8_t* bmpA = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.q28.bmpA");
  uint8_t* bmpB = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.q28.bmpB");
  if (!bmpA || !bmpB) {
    if (bmpA) free(bmpA);
    if (bmpB) free(bmpB);
    probePostProbeShutdown(*arm);
    snprintf(ret, sizeof(ret), "Img Q28: BMP heap alloc failed");
    return ret;
  }
  size_t bmpALen = buildBmp4bpp(bmpA, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  size_t bmpBLen = buildBmp4bpp(bmpB, kBmpCap, kImgW, -kImgH, BMP_PAT_ALL_BLACK);
  if (bmpALen == 0 || bmpBLen == 0) {
    free(bmpA); free(bmpB);
    probePostProbeShutdown(*arm);
    snprintf(ret, sizeof(ret), "Img Q28: BMP build failed");
    return ret;
  }

  unsigned framesPushed = 0;
  unsigned framesFailed = 0;
  unsigned textRebuilds = 0;
  unsigned textFails    = 0;
  const uint32_t startMs = millis();

  for (unsigned i = 0; i < kFrames && !gImgProbeAbort; i++) {
    const uint32_t frameStartMs = millis();
    const uint8_t* bmp    = (i & 1) ? bmpB    : bmpA;
    const size_t   bmpLen = (i & 1) ? bmpBLen : bmpALen;
    // Cycle through kNumSlots magic-bands so we never overflow uint8.
    // Slot N occupies [kPushSlotStart + N*kFragsPerFrame .. + 1]; with
    // kFragsPerFrame=2 and 14 slots that's 227..253 across 14 frames,
    // then wraps. Same pattern the camera stream worker uses.
    const uint32_t pushMagic =
        kPushSlotStart + ((uint32_t)(i % kNumSlots)) * (uint32_t)kFragsPerFrame;

    // tolerateMissedAcks=3 — streaming is forgiving; if the firmware
    // drops one ack we don't want to abort the whole 24-frame test.
    // Same value the camera stream worker uses.
    unsigned okFrags = 0, totalFrags = 0;
    bool pushed = sendImageBmpFragmentsNoCreate(
        *arm, "Q28", pushMagic,
        /*cid*/ imgTile.containerId, /*cname*/ kImgChild,
        bmp, bmpLen, &okFrags, &totalFrags,
        /*tolerateMissedAcks*/ 3);
    if (pushed) {
      framesPushed++;
    } else {
      framesFailed++;
      DEBUG_G2F("[ImgProbe] Q28 frame %u image push FAILED (%u/%u frags)",
                i, okFrags, totalFrags);
    }

    // Text rebuild every OTHER frame — different cadence from image
    // refresh is the whole point. If single-child REBUILD-text disturbs
    // the image child, we'll see image flicker on text rebuild ticks.
    if ((i & 1) == 0) {
      char txt[64];
      const uint32_t elapsed = millis() - startMs;
      snprintf(txt, sizeof(txt),
               "Q28 frame %u/%u\nimg pushed: %u\nelapsed: %u ms",
               i + 1, kFrames, framesPushed, (unsigned)elapsed);
      const G2ContainerGeom txtGeom = { 8, 0, 560, 130 };
      if (sendRebuildTextNamedAndWait(*arm, kTxtChild, txt, txtGeom,
                                      /*eventCapture*/ false)) {
        textRebuilds++;
      } else {
        textFails++;
        DEBUG_G2F("[ImgProbe] Q28 frame %u text REBUILD FAILED", i);
      }
    }

    // Pace per frame — sleep what's left of the budget in 50 ms slices,
    // checking gImgProbeAbort between each slice. Without this, a
    // double-tap during the pacing wait would take up to kPaceMs to
    // notice; with 50 ms slices the worst-case exit latency is bounded
    // to ~50 ms even when nothing else is happening. Same shape the live
    // page workers use for their inter-tick wait.
    const uint32_t consumedMs = millis() - frameStartMs;
    if (consumedMs < kPaceMs) {
      const uint32_t remainingMs = kPaceMs - consumedMs;
      const uint32_t kSliceMs = 50;
      const uint32_t sliceEndMs = millis() + remainingMs;
      while (millis() < sliceEndMs && !gImgProbeAbort) {
        const uint32_t leftMs = sliceEndMs - millis();
        const uint32_t thisMs = leftMs < kSliceMs ? leftMs : kSliceMs;
        vTaskDelay(pdMS_TO_TICKS(thisMs));
      }
    }
  }

  const bool aborted = gImgProbeAbort;
  free(bmpA); free(bmpB);
  probePostProbeShutdown(*arm);
  probeFooter("Q28", framesPushed, kFrames);

  const uint32_t totalMs = millis() - startMs;
  snprintf(ret, sizeof(ret),
           "Img Q28: %u/%u img frames (%u fail), %u/%u text rebuilds (%u fail) "
           "in %u ms (end: %s)",
           framesPushed, kFrames, framesFailed,
           textRebuilds, kFrames / 2, textFails,
           (unsigned)totalMs,
           aborted ? "user dismiss" : "all frames");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q28L — list+image counterpart to Q28. Identical image cadence (24
// frames × 750 ms × 96×96 stripes/black alternation) and identical
// image position (240, 168), but the compound CREATE pairs the image
// with a 5-item ListObject (verified-rendering shape from Q16/Q17/Q18)
// instead of a TextObject. No per-frame list rebuild — the question
// this probe answers is "does the image half of a list+image compound
// paint during live image-only push?", which is precisely the
// situation Q28 fails. If Q28L paints the image and Q28 doesn't, the
// firmware-side rule is "image children only composite under a List
// parent, not under Text".
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ28LMixedListImageLive() {
  EXT_RAM_BSS_ATTR static char ret[300];
  G2Temple* arm = pickEvenAIArm("imgQ28L");
  if (!arm) {
    snprintf(ret, sizeof(ret), "Img Q28L: no reachable temple");
    return ret;
  }

  static constexpr uint32_t kCreateMagic   = G2_MAGIC_IMAGE_BASE + 0x10;  // 226
  static constexpr uint32_t kPushSlotStart = G2_MAGIC_IMAGE_BASE + 0x11;  // 227
  static constexpr uint32_t kMaxPushMagic  = 254;
  static constexpr int32_t  kImgW          = 96;
  static constexpr int32_t  kImgH          = 96;
  static constexpr unsigned kFrames        = 24;
  static constexpr uint32_t kPaceMs        = 750;
  static const char* const  kImgChild      = "imgQ28L";
  static const char* const  kListChild     = "lstQ28L";

  // Same magic-cycling math as Q28: 96×96 4-bpp = ~4.7 KB → 2 fragments
  // per push; 28 push magics ÷ 2 = 14 cycling slots before we'd wrap
  // past 255.
  static constexpr unsigned kFragsPerFrame = 2;
  static constexpr unsigned kPushAvailable =
      (unsigned)(kMaxPushMagic - kPushSlotStart + 1);
  static constexpr unsigned kNumSlots = kPushAvailable / kFragsPerFrame;

  probeBanner("Q28L: list+image live (Q28 list-parent counterpart)",
              kCreateMagic, kMaxPushMagic,
              "compound CREATE: 5-item list top + image 96x96 @(240,168). "
              "Same image cadence/position as Q28 but list parent instead "
              "of text. If image renders here but not in Q28, list+image "
              "is the firmware-supported compound. Double-tap to exit.");

  if (!probeTearDownActiveContainer(*arm)) {
    snprintf(ret, sizeof(ret), "Img Q28L: pre-burst SHUTDOWN failed");
    return ret;
  }

  // 5-item list — matches the verified-rendering count from Q16/Q17/Q18
  // (runMixedListImageProbe). First row labels the test; rest are
  // filler so we stay safely above any "minimum items" the firmware
  // might enforce (1-item lists are untested).
  const char* listItems[] = {
    "Q28L: list+image live",
    "img: stripes/black alt",
    "row 3",
    "row 4",
    "row 5",
  };
  const size_t listItemCount = sizeof(listItems) / sizeof(listItems[0]);
  // List occupies the same top-half slot Q28 gave its TextObject so the
  // image-position variable stays clean across the comparison.
  const G2ContainerGeom listGeom = { 8, 0, 560, 130 };

  G2ImageTile imgTile = {};
  imgTile.x = 240; imgTile.y = 168;
  imgTile.w = (uint32_t)kImgW;
  imgTile.h = (uint32_t)kImgH;
  imgTile.containerId   = 2;
  imgTile.containerName = kImgChild;

  uint8_t createBuf[1024];
  size_t createLen = g2BuildCreateMixedListImage(
      allocSeq(), kCreateMagic,
      kListChild, listItems, listItemCount, listGeom,
      imgTile, BLOCKS_WIDGET_ID,
      createBuf, sizeof(createBuf));
  if (createLen == 0) {
    snprintf(ret, sizeof(ret), "Img Q28L: CREATE-mixed build failed");
    return ret;
  }

  probePrepImageCreateAck(kCreateMagic);
  if (!sendEnvelope(*arm, createBuf, createLen)) {
    snprintf(ret, sizeof(ret), "Img Q28L: CREATE-mixed TX failed");
    return ret;
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs)) {
    probePostProbeShutdown(*arm);
    snprintf(ret, sizeof(ret),
             "Img Q28L: CREATE-mixed ack timeout (list+image rejected? "
             "shouldn't happen — Q16/Q17/Q18 use the same builder)");
    return ret;
  }
  DEBUG_G2F("[ImgProbe] Q28L CREATE-mixed acked (list@'%s' + image@'%s' %dx%d)",
            kListChild, kImgChild, (int)kImgW, (int)kImgH);

  // Same alternating stripes/black pair as Q28 — alternation makes a
  // dropped frame eye-visible and matches the Q28 baseline so visual
  // comparison is direct.
  const size_t kBmpCap = 8 * 1024;
  uint8_t* bmpA = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.q28l.bmpA");
  uint8_t* bmpB = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.q28l.bmpB");
  if (!bmpA || !bmpB) {
    if (bmpA) free(bmpA);
    if (bmpB) free(bmpB);
    probePostProbeShutdown(*arm);
    snprintf(ret, sizeof(ret), "Img Q28L: BMP heap alloc failed");
    return ret;
  }
  size_t bmpALen = buildBmp4bpp(bmpA, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  size_t bmpBLen = buildBmp4bpp(bmpB, kBmpCap, kImgW, -kImgH, BMP_PAT_ALL_BLACK);
  if (bmpALen == 0 || bmpBLen == 0) {
    free(bmpA); free(bmpB);
    probePostProbeShutdown(*arm);
    snprintf(ret, sizeof(ret), "Img Q28L: BMP build failed");
    return ret;
  }

  unsigned framesPushed = 0;
  unsigned framesFailed = 0;
  const uint32_t startMs = millis();

  for (unsigned i = 0; i < kFrames && !gImgProbeAbort; i++) {
    const uint32_t frameStartMs = millis();
    const uint8_t* bmp    = (i & 1) ? bmpB    : bmpA;
    const size_t   bmpLen = (i & 1) ? bmpBLen : bmpALen;
    const uint32_t pushMagic =
        kPushSlotStart + ((uint32_t)(i % kNumSlots)) * (uint32_t)kFragsPerFrame;

    unsigned okFrags = 0, totalFrags = 0;
    bool pushed = sendImageBmpFragmentsNoCreate(
        *arm, "Q28L", pushMagic,
        /*cid*/ imgTile.containerId, /*cname*/ kImgChild,
        bmp, bmpLen, &okFrags, &totalFrags,
        /*tolerateMissedAcks*/ 3);
    if (pushed) {
      framesPushed++;
    } else {
      framesFailed++;
      DEBUG_G2F("[ImgProbe] Q28L frame %u image push FAILED (%u/%u frags)",
                i, okFrags, totalFrags);
    }

    // Deliberately NO list rebuild — Q28's text-rebuild on alternate
    // frames muddied the diagnostic ("which frame numbers are visible?")
    // because rebuild incidentally repaints the compound. Skipping
    // rebuild here means: if the image paints, it does so on every
    // push, independent of any rebuild trigger.

    const uint32_t consumedMs = millis() - frameStartMs;
    if (consumedMs < kPaceMs) {
      const uint32_t remainingMs = kPaceMs - consumedMs;
      const uint32_t kSliceMs = 50;
      const uint32_t sliceEndMs = millis() + remainingMs;
      while (millis() < sliceEndMs && !gImgProbeAbort) {
        const uint32_t leftMs = sliceEndMs - millis();
        const uint32_t thisMs = leftMs < kSliceMs ? leftMs : kSliceMs;
        vTaskDelay(pdMS_TO_TICKS(thisMs));
      }
    }
  }

  const bool aborted = gImgProbeAbort;
  free(bmpA); free(bmpB);
  probePostProbeShutdown(*arm);
  probeFooter("Q28L", framesPushed, kFrames);

  const uint32_t totalMs = millis() - startMs;
  snprintf(ret, sizeof(ret),
           "Img Q28L: %u/%u img frames (%u fail) in %u ms (end: %s) — "
           "did the image render?",
           framesPushed, kFrames, framesFailed,
           (unsigned)totalMs,
           aborted ? "user dismiss" : "all frames");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q21 — full-screen 4-tile (Q12 geometry) with two update rounds logged
// separately: round 0 pushes all four tiles; round 1 re-pushes TL and BR
// with different corner indices so the centre pattern shifts (magic bands
// 211–238 then 239–252, disjoint). Hold afterward like Q12. Does not touch
// production streaming paths — probe-only.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ21LiveFullScreenBurst() {
  EXT_RAM_BSS_ATTR static char ret[280];

  G2Temple* arm = pickEvenAIArm("imgQ21");
  if (!arm) return "Img Q21: no reachable temple";

  const uint32_t kCreateMagic = G2_MAGIC_IMAGE_BASE + 0x00;  // 210
  const uint32_t kPushBaseR0[4] = {
      G2_MAGIC_IMAGE_BASE + 0x01,  // 211..217  TL
      G2_MAGIC_IMAGE_BASE + 0x08,  // 218..224  TR
      G2_MAGIC_IMAGE_BASE + 0x0F,  // 225..231  BL
      G2_MAGIC_IMAGE_BASE + 0x16,  // 232..238  BR
  };
  const uint32_t kPushBaseR1[2] = {
      G2_MAGIC_IMAGE_BASE + 0x1F,  // 239..245  TL (round 1)
      G2_MAGIC_IMAGE_BASE + 0x26,  // 246..252  BR (round 1)
  };
  const int32_t kTileW = 288;
  const int32_t kTileH = 144;

  const G2ImageTile kTiles[4] = {
    { 0,                0, (uint32_t)kTileW, (uint32_t)kTileH, 2, "tileTL" },
    { (uint32_t)kTileW, 0, (uint32_t)kTileW, (uint32_t)kTileH, 3, "tileTR" },
    { 0,                (uint32_t)kTileH, (uint32_t)kTileW, (uint32_t)kTileH, 4, "tileBL" },
    { (uint32_t)kTileW, (uint32_t)kTileH, (uint32_t)kTileW, (uint32_t)kTileH, 5, "tileBR" },
  };

  probeBanner("Q21: full-screen 4-tile (2 refresh rounds)",
              kCreateMagic, kPushBaseR1[1] + 6,
              "Round 0: paint all 4 tiles like Q12. Round 1: re-push TL+BR "
              "only with shifted markers (239–252 magics). Serial logs show "
              "ms per round — multi-tile sustained refresh vs Q13.");
  if (!probeTearDownActiveContainer(*arm)) return "Img Q21: pre-burst SHUTDOWN failed";

  uint8_t createBuf[256];
  size_t createLen = g2BuildCreateImageMulti(
      allocSeq(), kCreateMagic, kTiles, 4, BLOCKS_WIDGET_ID,
      createBuf, sizeof(createBuf));
  if (createLen == 0) return "Img Q21: CREATE-multi build failed";

  noteOurShutdownSent();
  probePrepImageCreateAck(kCreateMagic);
  if (!sendEnvelope(*arm, createBuf, createLen)) {
    probePostProbeShutdown(*arm);
    return "Img Q21: CREATE-multi TX failed";
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs)) {
    probePostProbeShutdown(*arm);
    return "Img Q21: CREATE-multi ack timeout";
  }

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPSRAM, "g2.img.q21.bmp");
  if (!bmp) {
    probePostProbeShutdown(*arm);
    return "Img Q21: BMP heap alloc failed";
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;
  struct CornerXY {
    int32_t x, y;
  };
  const CornerXY kCorner[4] = {
    { kTileW - 24, kTileH - 24 },
    { 0,           kTileH - 24 },
    { kTileW - 24, 0           },
    { 0,           0           },
  };
  const char* kTileTag[4] = { "Q21/TL", "Q21/TR", "Q21/BL", "Q21/BR" };

  unsigned okProbe = 0, fragProbe = 0;
  const uint32_t r0Start = millis();
  for (size_t i = 0; i < 4; i++) {
    size_t bmpLen =
        buildBmp4bpp(bmp, kBmpCap, kTileW, -kTileH, BMP_PAT_STRIPES);
    if (bmpLen == 0) {
      free(bmp);
      probePostProbeShutdown(*arm);
      return "Img Q21: round-0 BMP build failed";
    }
    bmpDrawRect4bpp(bmp + kPixOffset, kTileW, -kTileH,
                    kCorner[i].x, kCorner[i].y, 24, 24, /*idx*/ 15);
    unsigned ok = 0, total = 0;
    (void)sendImageBmpFragmentsNoCreate(
        *arm, kTileTag[i], kPushBaseR0[i], kTiles[i].containerId,
        kTiles[i].containerName, bmp, bmpLen, &ok, &total);
    okProbe += ok;
    fragProbe += total;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  const uint32_t r0Ms = millis() - r0Start;
  DEBUG_G2F("[ImgProbe] Q21 round0 (4 tiles) wall-clock %u ms, %u/%u frags",
            (unsigned)r0Ms, okProbe, fragProbe);

  const uint32_t r1Start = millis();
  for (size_t pass = 0; pass < 2; pass++) {
    const size_t ti = (pass == 0) ? 0u : 3u;
    size_t bmpLen =
        buildBmp4bpp(bmp, kBmpCap, kTileW, -kTileH, BMP_PAT_STRIPES);
    if (bmpLen == 0) {
      free(bmp);
      probePostProbeShutdown(*arm);
      return "Img Q21: round-1 BMP build failed";
    }
    bmpDrawRect4bpp(bmp + kPixOffset, kTileW, -kTileH,
                    kCorner[ti].x, kCorner[ti].y, 24, 24,
                    /*idx*/ 14);  // different palette index vs round 0
    unsigned ok = 0, total = 0;
    (void)sendImageBmpFragmentsNoCreate(
        *arm, kTileTag[ti], kPushBaseR1[pass], kTiles[ti].containerId,
        kTiles[ti].containerName, bmp, bmpLen, &ok, &total);
    okProbe += ok;
    fragProbe += total;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  const uint32_t r1Ms = millis() - r1Start;
  DEBUG_G2F("[ImgProbe] Q21 round1 (TL+BR only) wall-clock %u ms (cumulative %u/%u frags)",
            (unsigned)r1Ms, okProbe, fragProbe);

  free(bmp);

  DEBUG_G2F("[ImgProbe] Q21 — pattern up (r0=%u ms r1=%u ms), double-tap to dismiss",
            (unsigned)r0Ms, (unsigned)r1Ms);
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  probePostProbeShutdown(*arm);
  probeFooter("Q21", okProbe, fragProbe);
  snprintf(ret, sizeof(ret),
           "Img Q21: 4-tile 2 rounds — round0=%u ms round1(TL+BR)=%u ms — %s",
           (unsigned)r0Ms, (unsigned)r1Ms,
           tapped ? "user tap" : "60s timeout");
  return ret;
}

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
