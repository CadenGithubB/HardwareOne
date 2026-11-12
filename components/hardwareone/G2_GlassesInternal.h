// =============================================================================
// G2 glasses: what the files of the glasses client share with each other
// =============================================================================
// Private to G2_Glasses.cpp and the files split from it: G2_Keyboard.cpp,
// G2_ImageProbes.cpp, G2_ImageViewers.cpp and the pages G2_Page_Launchers.cpp,
// G2_Page_Microphone.cpp, G2_Page_SystemEvents.cpp, G2_Page_Logging.cpp,
// G2_Page_FMRadio.cpp, G2_Page_LLM.cpp, G2_Page_Map.cpp and G2_Page_Health.cpp.
// Other modules use G2_Glasses.h. A name is declared here when a second file of
// the client needs it; its definition stays in the file that owns it.
#pragma once

#include "G2_Glasses.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <atomic>

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEAdvertisedDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "G2_Page_TextEntry.h"
#include "G2_Protocol.h"

// ---- Shared declarations, in the order of their definitions in G2_Glasses.cpp ----
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

// The stock 2.2.9 client sends DevCfg BASE_CONNECT_HEART_BEAT (cmd=14) on a
// separate 15 s lease from EvenCore's 5 s heartbeat.  It is best-effort native
// session maintenance: a missed send/ACK is telemetry, never a disconnect
// policy and never proof that the EvenCore rendering plugin is dead.
static constexpr uint32_t G2_BASE_HEARTBEAT_PERIOD_MS = 15000;

// Promotion transactions normally ACK in tens of milliseconds.  Keep the
// bound short enough that an explicit disconnect remains responsive, while
// allowing one full interval for a busy three-link controller.
static constexpr uint32_t G2_DEVCFG_ACK_TIMEOUT_MS = 750;
static constexpr uint32_t G2_DEVCFG_ACK_SLICE_MS = 25;

// DevCfgDataPackage nested-wrapper fields (dev_config_protocol.proto).  Keep
// these local to the session matcher: accepting the right command/magic with
// the wrong or non-empty nested body would let unsolicited status traffic
// satisfy promotion.
static constexpr uint32_t G2_DEVCFG_BODY_AUTH = 3;
static constexpr uint32_t G2_DEVCFG_BODY_ROLE = 4;
static constexpr uint32_t G2_DEVCFG_BODY_BASE_HEARTBEAT = 13;
static constexpr uint32_t G2_DEVCFG_BODY_TIME = 128;

// Mutex wait for sendEnvelope (heartbeats, single-packet commands).
// Fragmented sends take writeMutex per envelope only (not the whole
// burst). A stuck esp_ble_gattc_write_char can still exceed 500 ms
// without the peripheral being dead — too short a wait makes heartbeats
// "miss" and trips G2_TX_STUCK_DISCONNECT_BEATS prematurely.
static constexpr uint32_t G2_SEND_ENVELOPE_MUTEX_MS = 1500;

// Controller-level TX gate (bleCentralTx) waits. Fragmented sends take
// per envelope (same as single-envelope); heartbeats use a short take
// and skip cleanly when the gate is busy (glasses tolerate ≥10 s silence).
static constexpr uint32_t G2_CENTRAL_TX_ENVELOPE_MS = 1500;
static constexpr uint32_t G2_CENTRAL_TX_HEARTBEAT_MS = 50;

// After esp_ble_gattc_write_char rc=-1, delay this long before failing the
// envelope (caller may retry the whole fragment out-of-lock). Not an
// in-lock writeValue retry — that path can wedge on m_semaphoreWriteCharEvt
// (see sendEnvelopeNoMutex). Longer when R1 is up — three HIGH links drain
// the TX buffer slower.
static constexpr uint32_t G2_WRITE_RC_RETRY_COOLDOWN_MS = 100;
static constexpr uint32_t G2_WRITE_RC_RETRY_COOLDOWN_RING_MS = 200;

// Heartbeat consecutive send failures (mutex timeout or write failure) before
// forcing disconnect. At 5 s cadence, 5 misses ≈ 25 s — enough for BLE
// controller recovery without leaving a truly dead link up indefinitely.
static constexpr uint8_t G2_TX_STUCK_DISCONNECT_BEATS = 5;

// sendEnvelope result — Busy means the controller gate / peer mutex was
// contended and the caller should not treat it as a TX wedge.
enum class G2SendResult : uint8_t { Ok = 0, Busy, Fail };

enum class G2SessionPhase : uint8_t {
  Down = 0,
  Transport,
  Authenticating,
  SettingRole,
  SettingTime,
  AppLaunch,
  Ready,
};

// Primary text container name. Fixed per-session; rebuild semantics depend
// on reusing the same name. Keep ≤14 chars, case-sensitive.
static constexpr const char* CONTAINER_NAME = "app";

// Requested ATT MTU. The glasses typically negotiate down to 247/244.
// Shared with ring via G2_BLE_LOCAL_MTU_PREF — local preferred must stay high
// even when a peer (ring) negotiates a smaller per-link MTU.
static constexpr uint16_t MTU_TARGET = G2_BLE_LOCAL_MTU_PREF;

// Reassembly buffer size per temple. The largest envelope we expect is an
// image raw-data fragment (~4 KB body + header). Round up.
static constexpr size_t RX_ASSEMBLY_CAP = 8192;
// Largest protocol envelope is 8-byte header + u8-sized body. Reserve this
// much at the end of each temple's PSRAM RX buffer for ATT reassembly.
static constexpr size_t RX_FRAME_CAP = G2_ENVELOPE_HDR_LEN + 255;
static constexpr size_t RX_MESSAGE_CAP = RX_ASSEMBLY_CAP - RX_FRAME_CAP;

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
  uint8_t                      deviceAddressType;
  bool                         deviceAddressTypeKnown;
  char                         firmwareVersion[32];
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
  // Two-stage RX state. BLE notifications may split one protocol envelope,
  // and one protobuf message may itself span several protocol envelopes.
  // `rxBuf` lives in PSRAM; its tail is reserved for the current envelope
  // while the prefix accumulates the complete protobuf body.
  size_t                       rxFrameHave;
  size_t                       rxFrameExpected;
  uint32_t                     rxFrameConversateEpoch;
  uint32_t                     rxFrameNativeTerminalEpoch;
  uint32_t                     rxFrameNoteEpoch;
  G2RxReassembly               rxReassembly;
  // Kept only until the former inline parser below is retired after the
  // owner-path hardware soak; it is no longer called by the notify thunks.
  size_t                       rxHave;
  size_t                       rxExpected;
  uint8_t                      rxSeq;
  uint8_t                      rxTotalFrags;
  uint8_t                      rxNextFrag;
  uint8_t                      rxSid;
  uint8_t                      rxFlag;
  bool                         rxActive;
  uint32_t                     rxStartedMs;
  uint32_t                     rxReassemblyGeneration;
  uint32_t                     rxReassemblyLifecycleEpoch;
  uint32_t                     rxReassemblyPresentationEpoch;
  uint32_t                     rxReassemblyConversateEpoch;
  uint32_t                     rxReassemblyNativeTerminalEpoch;
  uint32_t                     rxReassemblyNoteEpoch;
  // Incremented before every subscribe attempt and on disconnect. Queued
  // notifications stamped by an older link are discarded by the owner.
  uint32_t                     connectionGeneration;

  // Native-session promotion.  A GATT transport is intentionally published
  // before AUTH/AppLaunch so its notify callback can deliver the ACKs, but no
  // ordinary sender may use it until sessionReadyGeneration matches the live
  // connection generation.
  G2SessionPhase               sessionPhase;
  uint32_t                     sessionReadyGeneration;
  uint8_t                      devCfgSessionFlag;

  // One generation-bound blocking DevCfg transaction per arm.  The binary
  // semaphore is statically backed because G2Temple itself has boot lifetime;
  // the identity fields are protected by gDevCfgTxnMux below.
  StaticSemaphore_t            devCfgAckSemStorage;
  SemaphoreHandle_t            devCfgAckSem;
  bool                         devCfgTxnArmed;
  bool                         devCfgTxnMatched;
  uint32_t                     devCfgTxnGeneration;
  uint32_t                     devCfgTxnCommand;
  uint32_t                     devCfgTxnMagic;
  uint32_t                     devCfgTxnBodyField;

  // Direct DevCfg cmd=14 maintenance telemetry.  These fields are protected
  // by gTopologyMux and reset on every generation advance.
  uint32_t                     baseHeartbeatNextDueMs;
  uint32_t                     baseHeartbeatLastTxMs;
  uint32_t                     baseHeartbeatLastAckMs;
  uint32_t                     baseHeartbeatLastMagic;

  // Write serialisation — BLE writes must not interleave mid-envelope, and
  // heartbeat must not step on a text rebuild in progress.
  SemaphoreHandle_t            writeMutex;

  // Last conn-params we successfully ASKED the controller for (1.25 ms ticks;
  // 0 = never set / invalidated by a disconnect). Not the negotiated value —
  // the peer may counter-offer, and the result only shows up in
  // ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT, which nothing latches today.
  //
  // Purpose is strictly de-duplication: setTempleConnParams() skips the GAP
  // call when the request matches what we last asked for. Without this, every
  // image push re-sent an identical update (ImgPushRadioGuard runs per push),
  // and the stack answered with "L2CA_UpdateBleConnParams connection parameter
  // already set" — one wasted L2CAP transaction per call, on a path that can
  // only have one conn-update in flight at a time.
  uint16_t                     connMinInt;
  uint16_t                     connMaxInt;
  // One conn-params request in flight per temple (all under sConnReqMux).
  // Armed just before esp_ble_gap_update_conn_params() with the range it asks
  // for latched in connReqMin/Max; answered by an event carrying that range, by
  // a refusal, or (after Bluedroid's own update timer reported status 16) by the
  // next event of any kind; dropped by templeForgetConnParams() or after
  // G2_CONN_REQ_ANSWER_MS. connMinInt/Max cannot serve here: the outOfRange
  // rule and g2connpri zero them while a request runs. Bluedroid queues a second
  // request behind a running one, but its timer lets a third reach the
  // controller mid-procedure (refused, and on 2026-10-20 the phone link died of
  // it). A request made meanwhile only sets connReapply; connParamsDeferredTick()
  // recomputes the arbiter's range once the first is answered.
  bool                         connReqInFlight;
  bool                         connReqHostTimedOut;
  bool                         connReapply;
  uint32_t                     connReqAt;
  uint16_t                     connReqMin;
  uint16_t                     connReqMax;
  // Interval the link is ACTUALLY running at, from
  // ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT (0 = not yet observed). Treat
  // connMinInt/connMaxInt as "what we asked for" and this as "what is".
  //
  // Note this is meaningful on BOTH the success and failure paths: when the
  // update is rejected the event reports the link's unchanged current value,
  // which is exactly what we want to pace against.
  uint16_t                     connAppliedInt;
  // Latency and supervision timeout the link is actually running at. Needed —
  // not just nice to have — because bluedroid SHORT-CIRCUITS a conn-params
  // request when max_int/latency/timeout all already match the live values
  // (L2CA_UpdateBleConnParams, l2c_ble.c:164-186): it logs "connection
  // parameter already set", fires our callback SYNCHRONOUSLY with status=0,
  // and never builds an HCI command. Without these two fields that no-op is
  // indistinguishable from a genuine acceptance.
  uint16_t                     connAppliedLatency;
  uint16_t                     connAppliedTimeout;
  // The range carried BY THE EVENT — not the same thing as connMinInt/Max,
  // which is what we last asked for. This is how a self-initiated update is
  // told apart from a peer-initiated one; see connEvtWho.
  uint16_t                     connEvtMinInt;
  uint16_t                     connEvtMaxInt;
  // status from the last conn-params event. esp_bt_status_t, NOT uint8_t:
  // ESP_BT_STATUS_BASE_FOR_HCI_ERR is 0x0100, so any raw-HCI passthrough
  // status truncates into an unrelated low-numbered code in a byte.
  uint16_t                     connLastStatus;
  // Who asked for the last conn-params change. See connEvtClassify().
  uint8_t                      connEvtWho;
  // Split counters. A prober must wait for OUR request's outcome specifically:
  // the temples renegotiate themselves when idle and those peer-initiated
  // updates are always accepted, so a single undifferentiated counter lets an
  // unrelated renegotiation satisfy the wait and be reported as our result.
  uint32_t                     connEvtSeqSelf;
  uint32_t                     connEvtSeqPeer;
  // Peer address captured at connect. The GAP callback uses this to attribute an
  // event to a temple WITHOUT dereferencing t.client, which templeReset() may be
  // deleting concurrently on another core.
  uint8_t                      peerBda[6];

  // Monotonic counters.
  uint32_t                     packetsSent;
  uint32_t                     packetsReceived;
  uint32_t                     heartbeatCounter;

  // Renderer-liveness watchdog. `heartbeatAwaitingEvidence` is armed only
  // after a heartbeat is physically accepted; the next successful heartbeat
  // tick converts that still-pending interval into one completed miss. Any
  // validated, current-generation EvenCore renderer message clears both the
  // pending interval and the miss count. Three completed silent intervals
  // (~15 s) mean the plugin task is unavailable — typically from a bad
  // REBUILD/UPDATE against the wrong container type — and ordinary render
  // commands should pause until current EvenCore activity resumes.
  // Caught us before: UPDATE_TEXT on a ListContainer froze every ack
  // including heartbeats; without this counter that state went silent
  // forever. (2026-04-24 incident.)
  uint32_t                     heartbeatMissed;
  bool                         heartbeatAwaitingEvidence;
  bool                         pluginDead;
  // Wear automation is deliberately stricter than renderer availability.
  // Generic typed E0 traffic can reopen rendering, but only a strict Cmd=12
  // heartbeat ACK may pair a prior NOT_WORN edge with WORN.
  bool                         heartbeatWearSilent;

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
  // ack. Cleared on a current-generation terminal boundary (E0 SYSTEM_EXIT,
  // a debounced authoritative SyncInfo ownership loss, or disconnect). Only
  // the right temple is
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


extern G2Temple gL;
extern G2Temple gR;
extern SemaphoreHandle_t gG2CompletionMutex;
SemaphoreHandle_t g2CompletionMutex();
class G2CompletionGuard {
 public:
  explicit G2CompletionGuard(TickType_t wait = portMAX_DELAY) {
    SemaphoreHandle_t mutex = g2CompletionMutex();
    locked_ = mutex && xSemaphoreTakeRecursive(mutex, wait) == pdTRUE;
  }
  ~G2CompletionGuard() {
    if (locked_) xSemaphoreGiveRecursive(gG2CompletionMutex);
  }
  explicit operator bool() const { return locked_; }
 private:
  bool locked_ = false;
};

bool g2TempleReadyAtGeneration(const G2Temple& temple,
                               uint32_t generation = 0);
extern volatile uint32_t gWidgetLifecycleEpoch;
bool g2FsmHijackActive();
bool g2FsmPageSwapping();
extern uint32_t gHijackStartedMs;
void kbdPadSysDoubleUnpublish();
uint32_t g2PresentationEpochCurrent();
bool interactiveLensLeaseAcquire();
void interactiveLensLeaseRelease();
bool pageSwapBeginWithInteractiveLease();
void pageSwapEnd();
void pageSwapFail(const char* source);
void pageSwapCancel();
bool pageSwapInFlight();
bool tapDispatcherEnqueueKeyboardService(uint32_t presentationEpoch);
uint32_t noteOurShutdownSent();
void clearOurShutdownStamp();
extern volatile bool gImgProbeAbort;
void imageProbeBegin();
void imageProbeEnd();
// When the user taps "Blocks" in the G2 side menu, the glasses emit a
// sid=0xE0 cmd=17 MenuStartUpEvent with widgetId=10509. We hijack that tap
// to push a dashboard-style status page instead of letting the built-in
// Blocks mini-app run. The hijack is unconditional (see plan §Scope) —
// when the ESP32 is connected to the glasses, the phone isn't, so tapping
// Blocks is effectively a shortcut to the ESP32's own status dashboard.
//
// Lifecycle:
//   gHijackActive = true   once g2ShowText(snapshot) returns OK
//   cleared through the centralized terminal reducer on a real E0
//   SYSTEM_EXIT, debounced authoritative SyncInfo ownership loss, BLE
//   disconnect, or the 60-second safety fallback.
static constexpr uint32_t BLOCKS_WIDGET_ID   = 10509;
bool kbdPadSysDoubleOwned();
void kbdPadSysDoubleClaim(char side);
void kbdPadSysDoubleTransition();
void kbdPadSysDoublePublish(uint32_t presentationEpoch);
bool firmwareOverlayActive();
extern volatile bool gImgProbeHoldActive;
extern volatile bool gImgProbeHoldTapPending;
extern volatile uint32_t gCamStreamPendingTap;
extern volatile bool gCamStreamActive;
#if ENABLE_MAPS
extern std::atomic<uint32_t> gMapPageActionMask;
extern std::atomic<int32_t> gMapPagePanX;
extern std::atomic<int32_t> gMapPagePanY;
extern std::atomic<uint32_t> gMapPageRequestGeneration;
extern std::atomic<uint32_t> gMapPageAcceptedPanCount;
extern std::atomic<uint32_t> gMapPagePanSaturationCount;
extern std::atomic<uint32_t> gMapPageLastInputMs;
extern std::atomic<uint8_t> gMapPageInputMenu;
extern std::atomic<bool> gMapPageActive;
void g2MapPageWake();
extern std::atomic<bool> gMapSearchCommitted;
extern std::atomic<bool> gMapSearchCancelled;
extern char gMapSearchQuery[33];
#endif

#if ENABLE_R1_HEALTH
extern volatile uint32_t gHealthPagePendingTap;
extern volatile bool gHealthPageActive;
extern volatile uint32_t gHealthPageGen;
extern volatile bool gHealthTextDead;
#endif

#if ENABLE_LLM_BACKEND
extern std::atomic<bool> gLlmPageActive;
extern std::atomic<bool> gLlmPageExit;
extern std::atomic<bool> gLlmPageAsk;
extern std::atomic<bool> gLlmPageBusyNotice;
extern std::atomic<int> gLlmPageNav;
#endif

extern volatile uint32_t gKbdAbTapDrops;
extern volatile bool gKbdAbActive;
extern volatile bool gKbdAbStarting;
extern volatile uint32_t gKbdAbGeneration;
extern volatile uint8_t gKbdAbNameBank;
static constexpr const char* kKbdAbListNames[2] = { "kbABLst0", "kbABLst1" };
extern bool (*gKbdAbRecoveryCallback)();
extern volatile uint32_t gKbdAbRecoveryEpoch;
bool kbdAbRecoveryContextLive(uint32_t epoch);
void kbdAbRecoveryDisarm();
bool kbdAbTapDequeue(uint8_t* outIdx);
void kbdAbTapQueueReset();
extern volatile uint8_t gExpectMagic;
enum class HijackListPageShape : uint8_t {
  None = 0,
  PureList,       // Cmd=0 list-only Blocks hijack / PSK_LIST page-swap
  ListWithTitle,  // PSK_LIST_TEXT compound — never use REBUILD-list in swap
};

extern size_t gLastHijackListRowCount;
extern HijackListPageShape gLastHijackListPageShape;
bool sendEnvelope(G2Temple& t, const uint8_t* data, size_t len);
const char* g2NativeTerminalRequestOpen(bool fromMenu = false, uint32_t menuStartedMs = 0, uint32_t menuOwner = 0);
// G2-side pad paths sometimes mutate gKbdPad before they reach a
// G2_Page_TextEntry callback. Cover that whole mutation window so terminal
// cleanup cannot free its image buffers concurrently with a redraw. The
// underlying lease is non-blocking and exact-session validated.
class G2TextEntryOperationGuard {
 public:
  explicit G2TextEntryOperationGuard(bool* secretOut = nullptr)
      : held_(g2TextEntryOperationBegin(secretOut)) {}
  ~G2TextEntryOperationGuard() {
    if (held_) g2TextEntryOperationEnd();
  }
  explicit operator bool() const { return held_; }
  G2TextEntryOperationGuard(const G2TextEntryOperationGuard&) = delete;
  G2TextEntryOperationGuard& operator=(
      const G2TextEntryOperationGuard&) = delete;

 private:
  bool held_;
};

bool sendCreateTextAndWait(G2Temple& arm,
                           const char* text,
                           uint32_t widgetId,
                           const G2ContainerGeom& geom,
                           bool eventCapture,
                           const char* containerName = CONTAINER_NAME);
bool tearDownActiveContainer(G2Temple& arm);
bool g2NoteCreateSuccess(G2Temple& arm, bool isList,
                         uint32_t widgetId,
                         uint32_t expectedLifecycleEpoch,
                         uint32_t* committedPresentationEpoch = nullptr);
void g2TextViewDisarm();
void g2TextViewDisarmIfOwned(void (*exitFn)(),
                             uint32_t lifecycleEpoch);
bool g2TextViewPublishForLifecycle(void (*exitFn)(), G2TapFn tapFn,
                                   uint32_t lifecycleEpoch,
                                   uint32_t presentationEpoch,
                                   bool isTextPage = false);
bool g2SessionLifecycleAllows(uint32_t lifecycleEpoch);
bool g2LensTxFenceCurrent(const G2Temple& arm,
                          uint32_t generation,
                          uint32_t lifecycleEpoch);
void g2CreateTxnRelease();
bool tearDownActiveContainerAtGeneration(G2Temple& arm,
                                         uint32_t generation,
                                         bool force,
                                         uint32_t lifecycleEpoch = 0,
                                         uint32_t createCleanupSerial = 0);
uint8_t allocSeq();
uint8_t allocCreateMagic();
extern volatile bool gMicStreamOn;
void g2StatusHandleTap(uint32_t idx);
extern const G2PageModule kStatusPage;
#if ENABLE_MAPS
bool g2ShowMapPage(void (*onDone)());   // interactive list+image Maps page, defined below
#endif

#if ENABLE_LLM_BACKEND
bool g2ShowLlmPage(void (*onDone)());   // read-only streaming LLM viewer, defined below
bool g2LlmReaderHold(uint8_t phase, uint8_t source);
void g2ShowLlmMenu();                   // LLM submenu (chat / guided / re-run / model), defined below
#endif

#if ENABLE_R1_HEALTH
bool g2ShowHealthPage(void (*onDone)()); // Apps → Health: list+graph R1 vitals, defined below
#endif

bool g2ShowSysEventsPage();             // read-only paged system-event viewer, defined below
void g2ShowLoggingMenu();               // sensor-logging control list, defined below
void invokePageFromMain(const G2PageModule& p);
extern const G2PageModule kAppsPage;
extern const G2PageModule kSystemPage;
extern const G2PageModule kConfigPage;
bool sendCreateMixedListMultiTextAndWait(
    G2Temple& arm,
    const char* const* listItems,
    size_t listItemCount,
    const G2ContainerGeom& listGeom,
    const G2TextChildSpec* textChildren,
    size_t textChildCount,
    uint32_t widgetId);
bool sendUpdateTextNamed(G2Temple& arm,
                         const char* containerName,
                         uint32_t containerId,
                         const char* content,
                         uint32_t expectedGeneration = 0);
void liveTextKickRefresh();
void liveTextExitToHijackMenu();
extern const G2PageModule kMicDetailPage;
extern const G2PageModule kSysEventsPage;
extern const G2PageModule kLoggingPage;
#if ENABLE_FM_RADIO
extern const G2PageModule kFmTunerPage;
#endif

#if ENABLE_LLM_BACKEND
extern const G2PageModule kLlmMenuPage;
#endif

void tapLabelCacheStore(const char* const* items, size_t n);
void g2UiSubmitDone();
struct G2UiSubmitClaim {
  ~G2UiSubmitClaim() { g2UiSubmitDone(); }
  G2UiSubmitClaim(const G2UiSubmitClaim&) = delete;
  G2UiSubmitClaim& operator=(const G2UiSubmitClaim&) = delete;
  G2UiSubmitClaim() = default;
};

bool g2CompensateRejectedCreate(G2Temple& arm, const char* phase);
bool g2WidgetLifecycleOpen(uint32_t lifecycleEpoch);
bool g2SendCreateTxnEnvelope(G2Temple& arm,
                             const uint8_t* data, size_t len);
bool g2SendCreateTxnPb(G2Temple& arm, const uint8_t* pb,
                       size_t pbLen);
bool armCreateSlot(const char* who, G2Temple& arm);
bool waitCreateAck(const char* who, uint32_t widgetId);
bool sendRebuildTextNamedAndWait(G2Temple& arm,
                                 const char* containerName,
                                 const char* text,
                                 const G2ContainerGeom& geom,
                                 bool eventCapture,
                                 uint32_t expectedGeneration = 0,
                                 uint32_t expectedLifecycleEpoch = 0,
                                 uint32_t expectedPresentationEpoch = 0);
bool tapRuntimeSnapshot(QueueHandle_t* queueOut);
bool g2SessionSubmit(void (*run)(void*), void* payload,
                     void (*drop)(void*), const char* tag,
                     uint32_t expectedLifecycleEpoch);
uint8_t* g2SessionScratchAcquire(size_t need);
extern volatile uint32_t gG2LiveRateMs;
extern volatile bool gG2LiveLoopKeepAlive;
G2Temple* pickEvenAiArm(const char* tag);
typedef enum {
  BMP_PAT_STRIPES,    // 0x0F / 0xF0 alternating bytes (vertical stripes)
  BMP_PAT_ALL_BLACK,  // all-zero pixels (used to test the "all-black
                      // ack faster" claim from jimrandomh)
} BmpPattern;

size_t bmp4bppFileBytesU32(uint32_t aw, uint32_t ah);
size_t buildBmp4bpp(uint8_t* out, size_t outCap,
                    int32_t width, int32_t height,
                    BmpPattern pattern);
void logPbHex(const char* tag, const uint8_t* pb, size_t pbLen);
// CREATE-image ack wait. Faceclaw uses ~2 s; we allow extra margin so a
// BLE stall or heartbeat contention right after SHUTDOWN settle does not
// false-fail the CREATE before the first Cmd=3 byte ships.
static constexpr uint32_t kImgCreateAckTimeoutMs = 3000;
// g2-kit-unofficial ImageStreamer default (ble/docs/images.md): tolerate
// up to ~3 consecutive Cmd=3 ack misses — fragment often landed anyway.
static constexpr unsigned kImgAckMissTolerance = 3;
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
// Immutable identity for cleanup and every operation that follows one
// image-shaped CREATE.  A probe must carry this token through its final Cmd=9
// instead of sampling a possibly reconnected temple at shutdown time.
struct G2ImageCreateFence {
  G2Temple* arm = nullptr;
  uint32_t generation = 0;
  uint32_t lifecycleEpoch = 0;
};

bool probeTearDownActiveContainer(
    G2Temple& arm, G2ImageCreateFence* outPostTeardownFence = nullptr);
bool probePrepImageCreateAck(uint32_t magic,
                             const G2Temple* expectedArm,
                             uint32_t expectedGeneration = 0);
bool captureImageCreateFence(G2Temple& arm,
                             G2ImageCreateFence* out);
bool imageCreateFenceCurrent(const G2ImageCreateFence& fence);
bool probeWaitImageCreateAck(uint32_t magic, uint32_t timeoutMs,
                             const G2Temple* expectedArm = nullptr,
                             uint32_t expectedGeneration = 0,
                             bool retainForPublication = false);
void probePostProbeShutdown(const G2ImageCreateFence& fence);
bool probeHoldUntilTapOrTimeout(uint32_t maxMs);
extern bool gG2ImgLz4Auto;
struct G2ImgLz4AutoOffGuard {
  bool prev;
  G2ImgLz4AutoOffGuard() : prev(gG2ImgLz4Auto) { gG2ImgLz4Auto = false; }
  ~G2ImgLz4AutoOffGuard() { gG2ImgLz4Auto = prev; }
};

bool sendImageBmpMultiFragment(G2Temple& arm,
                               const char* tag,
                               uint32_t createMagic,
                               uint32_t pushMagicBase,
                               const uint8_t* bmp,
                               size_t bmpLen,
                               unsigned* outOkFrags,
                               unsigned* outTotalFrags,
                               unsigned tolerateMissedAcks = 0,
                               int32_t imgW = 288,
                               int32_t imgH = 144,
                               uint32_t compressMode = G2_IMG_COMPRESS_RAW,
                               G2ImageCreateFence* outCreateFence = nullptr);
bool sendImageBmpFragmentsNoCreate(G2Temple& arm,
                                   const char* tag,
                                   uint32_t pushMagicBase,
                                   uint32_t cid,
                                   const char* cname,
                                   const uint8_t* bmp, size_t bmpLen,
                                   unsigned* outOkFrags,
                                   unsigned* outTotalFrags,
                                   unsigned tolerateMissedAcks = 0,
                                   unsigned inFlightCapOverride = 0,
                                   uint32_t compressMode = G2_IMG_COMPRESS_RAW,
                                   size_t* outWireBytes = nullptr,
                                   uint32_t expectedGeneration = 0,
                                   bool /*legacyMarkExpectedEcho*/ = true,
                                   uint32_t expectedLifecycleEpoch = 0);
bool readBmpFromVfs(const String& rawPath,
                    uint8_t** outData,
                    size_t* outLen,
                    int32_t* outW,
                    int32_t* outH,
                    const char** outErr);
void kbdPadDictationServiceOnTap(uint32_t presentationEpoch);
void kbdPadDictationTickMain();
bool kbdPadHandleDoubleTap();
#if ENABLE_MAPS
const char* cmd_g2map(const String& argsInput);
#endif

#if ENABLE_R1_HEALTH
const char* cmd_g2health(const String& argsInput);
#endif

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
