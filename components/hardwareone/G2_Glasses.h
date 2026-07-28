#ifndef G2_GLASSES_H
#define G2_GLASSES_H

#include "System_BuildConfig.h"
#include "System_G2_Protocol.h"   // G2ContainerGeom + G2_GEOM_* presets
#include <Arduino.h>

// =============================================================================
// EVEN REALITIES G2 GLASSES - BLE CLIENT
// =============================================================================
// This module implements ESP32 as a BLE Central/GATT Client to connect to
// Even Realities G2 smart glasses. This mode is mutually exclusive with the
// phone BLE server mode (Bluetooth).
//
// Requires: ENABLE_BLUETOOTH=1 AND ENABLE_G2_GLASSES=1
// Protocol reference: https://github.com/i-soxi/even-g2-protocol
// =============================================================================

// G2 type definitions (always available for type-safe references)
enum G2State {
  G2_STATE_IDLE = 0,
  G2_STATE_SCANNING,
  G2_STATE_CONNECTING,
  G2_STATE_AUTHENTICATING,
  G2_STATE_CONNECTED,
  G2_STATE_DISCONNECTING,
  G2_STATE_ERROR
};

enum G2Eye {
  G2_EYE_LEFT = 0,
  G2_EYE_RIGHT = 1,
  G2_EYE_AUTO = 2
};

enum G2EventType {
  G2_EVENT_UNKNOWN = 0,
  G2_EVENT_SWIPE_UP,
  G2_EVENT_SWIPE_DOWN,
  G2_EVENT_SWIPE_LEFT,
  G2_EVENT_SWIPE_RIGHT,
  G2_EVENT_TAP,
  G2_EVENT_LONG_PRESS,
  G2_EVENT_DOUBLE_TAP,
  // Coarse-grained signals from the sid=0x0D channel. Empirical findings
  // (2026-04-24) show the firmware does NOT distinguish gesture type here —
  // head-up wake, tap, double-tap, swipe all emit the same 6-byte
  // SysEvent{EventType=1} payload. Exposed as USER_ACTIVITY so consumers
  // can treat it as a generic "user did something" signal without pretending
  // we know the specific gesture. DISPLAY_OFF fires when the display
  // transitions on→off (either inactivity timeout or user close).
  G2_EVENT_USER_ACTIVITY,
  G2_EVENT_DISPLAY_OFF,
  // Device-side system notification (NOT a user gesture). Observed
  // (2026-05-02) on the 9B SysEvent path with code=224, src=33 —
  // emitted when the on-lens "ring connected/disconnected" status banner
  // appears or dismisses. Distinct from USER_ACTIVITY so widget-management
  // consumers (TEXT-view auto-exit, hijacked menu dismissal) don't mistake
  // a transient banner for a user tap and tear down their UI.
  G2_EVENT_SYS_NOTIFY
};

typedef void (*G2EventCallback)(G2EventType event);

// G2 requires Bluetooth to be enabled first
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// -----------------------------------------------------------------------------
// G2 BLE UUIDs (from protocol docs)
// -----------------------------------------------------------------------------
// Legacy #defines — G2_Glasses.cpp owns the authoritative UUIDs now.
// The command-service value below was corrected from "...e0000" (wrong) to
// "...e5450" after real-device service discovery.
#define G2_UUID_BASE          "00002760-08c2-11e1-9073-0e8ac72e"
#define G2_SERVICE_UUID       "00002760-08c2-11e1-9073-0e8ac72e5450"
#define G2_CHAR_WRITE_UUID    "00002760-08c2-11e1-9073-0e8ac72e5401"
#define G2_CHAR_NOTIFY_UUID   "00002760-08c2-11e1-9073-0e8ac72e5402"
#define G2_CHAR_DISPLAY_UUID  "00002760-08c2-11e1-9073-0e8ac72e6402"

// -----------------------------------------------------------------------------
// G2 Protocol Constants
// -----------------------------------------------------------------------------
#define G2_PACKET_MAGIC       0xAA
#define G2_PACKET_TYPE_CMD    0x21
#define G2_PACKET_TYPE_RSP    0x12
#define G2_MTU_TARGET         512
#define G2_AUTH_PACKET_COUNT  7

#define G2_SVC_AUTH_CTRL_HI   0x80
#define G2_SVC_AUTH_CTRL_LO   0x00
#define G2_SVC_AUTH_DATA_HI   0x80
#define G2_SVC_AUTH_DATA_LO   0x20
#define G2_SVC_TELEPROMPTER_HI 0x06
#define G2_SVC_TELEPROMPTER_LO 0x20
#define G2_SVC_DISPLAY_CFG_HI 0x0E
#define G2_SVC_DISPLAY_CFG_LO 0x20
#define G2_SVC_SYNC_HI        0x80
#define G2_SVC_SYNC_LO        0x00

// -----------------------------------------------------------------------------
// G2 Client State Structure
// -----------------------------------------------------------------------------
struct G2ClientState {
  G2State state;
  G2Eye targetEye;
  bool initialized;
  
  // Connection info
  String deviceName;
  String deviceAddress;
  uint16_t mtu;
  uint32_t connectedSince;
  
  // Protocol state
  uint8_t seqNumber;      // Incrementing sequence for packets
  uint16_t msgId;         // Message ID for payloads
  
  // Statistics
  uint32_t packetsSent;
  uint32_t packetsReceived;
  uint32_t authAttempts;
  
  // Event callback
  G2EventCallback eventCallback;
  
  // Deferred event handling (ISR-safe pattern: callback sets flag, task processes)
  bool deferredGesturePending;
  G2EventType deferredGestureEvent;
};

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

// Initialization (sets up BLE client mode, tears down server if running)
bool initG2Client();
void deinitG2Client();
bool isG2ClientInitialized();

// =============================================================================
// Unified BLE connect worker (Group B, see docs/G2_REFACTOR_PROPOSAL.md)
// =============================================================================
// Single persistent task spawned in initG2Client() that drains a small queue
// of BleConnectJob entries. Replaces the 5 distinct `xTaskCreate(*TaskBody)`
// patterns (g2 connect/saved + ring connect/saved/mac) with one worker — the
// 6 KB stack is paid ONCE at G2 init time, not transiently during connect
// when internal DRAM is already tight.
//
// Public API for callers (g2Connect, g2RingConnect, etc.) stays unchanged;
// internally those functions now build a BleConnectJob and call
// g2SubmitBleConnect. The G2 family (G2_EYE/G2_SAVED) and Ring family
// (RING_*) each maintain their own in-flight flag so producers serialize
// per-family before submission — but at runtime, all kinds dispatch through
// one worker, so a G2 connect and Ring connect run sequentially rather than
// in parallel (intentional: BLE radio contention during overlapping connects
// was a real source of failures).
enum class BleConnectKind : uint8_t {
  G2_EYE     = 0,   // single eye; uses `eye` field
  G2_SAVED   = 1,   // both eyes from saved MACs
  RING_SCAN  = 2,   // scan + connect any ring
  RING_SAVED = 3,   // saved MAC reconnect (waits for glasses up first)
  RING_MAC   = 4,   // direct MAC connect; uses `mac` field
};

struct BleConnectJob {
  BleConnectKind kind;
  uint8_t        eye;        // valid for G2_EYE only (cast from G2Eye)
  char           mac[18];    // valid for RING_MAC only ("AA:BB:CC:DD:EE:FF\0")
};

// Heap-copies the job and pushes onto the BLE-connect queue. Returns false
// if initG2Client() hasn't been called (queue not yet alive) or the queue
// is full (some other connect is queued ahead). Caller should set its
// per-family in-flight flag (gConnectTaskActive / gRingConnectTaskActive)
// BEFORE calling this and clear it from the worker's dispatch only after
// the underlying *Sync function returns.
bool g2SubmitBleConnect(const BleConnectJob& job);

// Connection management
bool g2Connect(G2Eye eye = G2_EYE_LEFT);
// Saved-MAC reconnect. Uses gSettings.bleGlasses{Left,Right}MAC and matches
// scan adverts by MAC only (no name fallback). Returns false if no MACs are
// saved or a connect is already in flight. Used by the boot auto-reconnect
// hook; safe to call from any context (spawns its own background task).
bool g2ConnectSaved();
void g2Disconnect();
bool isG2Connected();
G2State getG2State();
const char* getG2StateString();

// Stricter than isG2Connected(): true iff BOTH temples have an active BLE
// link. Useful when an operation needs full glasses presence (e.g. waiting
// for boot-time reconnect to finish before kicking off a competing BLE
// activity).
bool g2BothConnected();

// True iff the LEFT temple — the audio arm — has an active BLE link AND its
// audio-notify char (6402) is subscribed. This is the microphone-availability
// predicate for HAL_Audio (the G2 mic is LEFT-temple only on FW 2.2.0.24).
// Deliberately NOT isG2Connected() (OR-of-temples → false-positive when only
// RIGHT is up) nor g2BothConnected() (false-negative when RIGHT is absent).
bool g2LeftConnected();

// Block the calling task until g2BothConnected() returns true, or until
// `timeoutMs` elapses. Returns true on success, false on timeout. Polls
// every 100 ms — coarse but adequate for boot-time sequencing where
// connects take seconds. Do not call from time-critical paths.
bool g2WaitForBothConnected(uint32_t timeoutMs);

// Update connection priority on every connected temple. Returns the count
// of temples the request was sent to (0..2). The peripheral may
// counter-offer; final values appear in ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT.
//   high=true  → ~11.25-15 ms intervals (default for normal traffic).
//   high=false → ~40-60 ms intervals (BALANCED). Frees ~3-4× more BLE
//                radio idle time for scans and concurrent connects, at the
//                cost of glasses input latency. Used during ring connect
//                attempts to prevent the BLE controller from starving an
//                existing temple link to supervision-timeout (rsn=0x8).
int g2SetAllTemplesConnPriority(bool high);

// Connection-priority arbiter. HIGH is the steady state; callers that need the
// radio to breathe REQUEST BALANCED and release when done. Refcounted, so
// overlapping requesters compose and whoever leaves first does not drag the
// links back to HIGH under someone who still needs them down.
//
// Prefer these over g2SetAllTemplesConnPriority(). `who` is a short tag for the
// log line only. Safe to call from any core (portMUX inside).
//
//   Usage:  g2ConnPriRequestBalanced("ring-connect");
//           ... work ...
//           g2ConnPriReleaseBalanced("ring-connect");
//   or wrap in an RAII guard (see GlassesPriorityGuard in G2_Ring.cpp).
void g2ConnPriRequestBalanced(const char* who);
void g2ConnPriReleaseBalanced(const char* who);
// Ask for the fast connection interval for the duration of something that
// genuinely needs the throughput (image bursts, streaming). Refcounted.
// Releasing asserts NOTHING — the glasses renegotiate to their own preference,
// and pinning them fast costs ~35x their mandatory radio wakeups because our
// requests carry latency=0 and theirs carry latency=4. Do not acquire this for
// menus, text or telemetry.
void g2ConnPriRequestFast(const char* who);
void g2ConnPriReleaseFast(const char* who);
int  g2ConnPriFastDepth();

// RAII: hold the fast interval for a scope. Nesting is FREE — the refcount
// means an inner guard inside an outer one costs no radio traffic at all, so
// it is safe to wrap both a whole probe and each individual push within it.
struct G2FastLinkGuard {
  explicit G2FastLinkGuard(const char* who) : m_who(who) { g2ConnPriRequestFast(who); }
  ~G2FastLinkGuard() { g2ConnPriReleaseFast(m_who); }
  G2FastLinkGuard(const G2FastLinkGuard&) = delete;
  G2FastLinkGuard& operator=(const G2FastLinkGuard&) = delete;
 private:
  const char* m_who;
};
int  g2ConnPriBalancedDepth();
// Re-assert the arbiter's decision on a newly connected temple, which comes up
// at HIGH regardless of what the arbiter currently wants.
void g2ConnPriReapply();

// Fill `out` with the 6-byte BLE address of the right (or left) temple in
// natural high-to-low order (matching the colon-separated string form, e.g.
// "c8:8d:65:00:97:69" → {0xC8,0x8D,0x65,0x00,0x97,0x69}). Returns true if
// the temple link is currently up; on false, `out` is zeroed. Used by the
// R1 ring's advStart payload, which wants the right-temple MAC reversed
// (LSB-first); reverse on the caller side.
bool g2GetLeftTempleMac(uint8_t out[6]);
bool g2GetRightTempleMac(uint8_t out[6]);

// Send a pre-built envelope (the byte stream returned by any g2Build*
// function) to the right-temple BLE link. Used by external modules — most
// notably the R1 ring spoof-push (G2_Ring.cpp) which synthesises sid=0x90
// RingDataPackage frames so the glasses display real ring telemetry even
// when the official bridge handshake never completes.
//
// Returns false if the right temple isn't connected, the writeChar isn't
// available, or the BLE write fails. Acquires the temple's send mutex
// internally so concurrent callers don't interleave bytes.
bool g2SendToRightTemple(const uint8_t* env, size_t envLen);

// Allocate the next outgoing-frame sequence number (shared monotonic
// counter, wraps at 0xFF). Mirrors the existing internal `allocSeq()`.
// Exposed so external builders can construct envelopes that follow the
// glasses' expected ordering (the firmware's seq counter has to be
// monotonic across all SIDs sent on a given temple link).
uint8_t g2AllocSeq();

// Scanning
bool g2StartScan(uint32_t durationMs = 10000);
void g2StopScan();

// Display output
bool g2ShowText(const char* text);

// Drive the front-pane Even-AI card via sid=0x07. The two strings are:
//   heading — rendered as the "question" panel (acts as a title/heading
//             above the body). Empty → not displayed.
//   body    — rendered as the "answer" card body text.
// The firmware paints them stacked on the same plane simultaneously.
// Verified on hardware 2026-04-26 — the full pipeline (CTRL{ENTER} →
// ASK{heading} → ANALYSE → REPLY{body}) renders both panels and ends
// with a STREAM_COMPLETE event.
bool g2ShowEvenAIReply(const char* heading, const char* body);

// Convenience: single-string variant uses "(host)" as the heading,
// matching the original prototype behaviour. Existing callers keep
// working without changes.
bool g2ShowEvenAIReply(const char* body);

// Pipeline variants for A/B testing — same surface (front-pane card),
// different state-machine paths into it. Useful for empirically pinning
// down which transitions the firmware actually requires.
bool g2ShowEvenAIReplyNoAsk(const char* heading, const char* body);   // CTRL → ANALYSE → REPLY (no ASK; heading ignored)
bool g2ShowEvenAIReplyDirect(const char* heading, const char* body);  // CTRL → REPLY (heading ignored; original failing path)
bool g2ShowEvenAIReplyNoAsk(const char* body);
bool g2ShowEvenAIReplyDirect(const char* body);

// Dismiss the front-pane EvenAI card immediately. Sends CTRL with
// status=EXIT (3) which closes the card without waiting for the ~10 s
// auto-dismiss. Returns true if the envelope was sent. Safe to call
// even if no card is showing — the firmware silently ignores EXIT
// without an active card.
//
// Use cases: pair this with g2ShowEvenAIReplyNoAsk to show a temporary
// progress message ("Scanning..." / "Connecting...") and dismiss when
// the underlying op completes.
bool g2HideEvenAICard();

// Send a fresh list to the lens via SHUTDOWN+CREATE (the path that survives
// the firmware-plugin REBUILD-list crash — see G2_PROTOCOL.md "Hijack
// page-swap lifecycle"). Used by stateful pages like the Files browser
// and Settings editor where each tap mutates the list contents.
//
// `geom` controls the on-lens rectangle. Defaults to G2_GEOM_LARGE so any
// existing caller immediately benefits from a near-full-lens widget;
// pages that want a tighter look can pass G2_GEOM_MEDIUM / _SMALL / etc.
// (see System_G2_Protocol.h for the full preset list). Geometry takes
// effect on the CREATE half of the swap, so callers can change geom
// freely between successive g2ShowListPage calls without an extra step.
//
// Returns true if the swap worker was successfully spawned.
bool g2ShowListPage(const char* const* items, size_t itemCount,
                    const G2ContainerGeom& geom = G2_GEOM_LARGE);

// Render free-flowing text on the lens as a TextContainer (no per-row
// selection borders). Same SHUTDOWN+CREATE swap pattern as
// g2ShowListPage but emits a TextObject (wrapper field 3) instead of a
// ListObject (wrapper field 2).
//
// Use cases: Settings JSON view, multi-line debug dumps, anything where
// list chrome (selection box per row) would visually fight the
// content. Newlines in `content` render as line breaks per the G2 font.
//
// `exitFn` is the fallback back-handler invoked when the firmware
// doesn't fire TextEvent CLICK on tap (the reference comment says it
// won't; we set IsEventCapture=1 anyway and hope). If firmware ignores
// the flag, a sid=0xE0 SysEvent (CLICK / DOUBLE_CLICK / SCROLL — real
// hardware gesture) always exits via exitFn so the user can always get
// back to a list view. Pass nullptr if you want only the firmware's
// tap-and-hold exit gesture.
//
// `tapFn` is the optional page-navigation handler for paginated TEXT
// views. When non-null, lens taps / ring scrolls past the post-render
// grace window call tapFn instead of exiting — the consumer reads the
// `kind` argument to advance forward or backward and rebuilds the page
// via g2ShowText (which uses REBUILD_PAGE for second-and-subsequent
// calls). The grace window is re-armed automatically. Pass nullptr for
// the legacy "any activity exits" UX.
//
// Gesture map for paginated views (tapFn != null):
//   ring scroll-down (SCROLL_BOTTOM) → tapFn(G2_TAP_PAGE_NEXT)
//   ring scroll-up   (SCROLL_TOP)    → tapFn(G2_TAP_PAGE_PREV)
//   single tap       (CLICK)         → tapFn(G2_TAP_PAGE_NEXT)
//   double tap       (DOUBLE_CLICK)  → exitFn
enum G2TapKind : uint8_t {
  G2_TAP_PAGE_NEXT = 0,
  G2_TAP_PAGE_PREV = 1,
};
typedef void (*G2TapFn)(G2TapKind kind);

// Returns true if the swap worker was successfully spawned.
bool g2ShowTextPage(const char* content,
                    const G2ContainerGeom& geom = G2_GEOM_LARGE,
                    void (*exitFn)() = nullptr,
                    G2TapFn tapFn = nullptr);

// -----------------------------------------------------------------------------
// Shared paged-text render — pairs with TextPager + the pure paging ops in
// G2_Page_Common.h. This is the one piece of that pager that touches the wire
// API (g2ShowTextPage), so it lives here rather than in the dependency-free
// common header. Used by Files, Settings JSON, and ESPNow chat.
// -----------------------------------------------------------------------------
struct TextPager;  // defined in G2_Page_Common.h

struct G2TextPageChrome {
  const char* title;       // heading text (e.g. "Pretty foo.json", "Inbox")
  const char* navHint;     // multi-page hint; null -> "tap/scroll=nav, 2x-tap=exit"
  const char* singleHint;  // single-page hint; null -> "2x-tap=exit"
  const char* separator;   // rule drawn under the header; null/"" -> none
  const char* emptyMsg;    // shown when the current page slice is empty
};

// Assemble "title [n/m] hint" + optional separator + the current page slice
// into caller-owned `pageBuf`, then (re)render via g2ShowTextPage. `navFn` is
// forwarded only when the pager has >1 page (single-page views exit on any
// gesture). A "...[truncated]" marker is appended on the final page when
// pager.truncated is set. Returns g2ShowTextPage's result.
bool g2TextPagerRender(struct TextPager& pager, char* pageBuf, size_t pageBufCap,
                       const G2TextPageChrome& chrome,
                       const G2ContainerGeom& geom,
                       void (*exitFn)(), G2TapFn navFn);

// Variant that CREATEs the TextObject with a small placeholder, then
// immediately REBUILD-text's `content` into it. The placeholder always
// fits in one fragment so the CREATE is guaranteed to ack; the
// REBUILD then carries the real test payload. Used by Transport Tests
// to compare REBUILD-text's reassembly ceiling against CREATE-text's.
bool g2ShowTextPageRebuildProbe(const char* placeholder,
                                const char* content,
                                const G2ContainerGeom& geom = G2_GEOM_LARGE,
                                void (*exitFn)() = nullptr);

// Render a CreateStartUpPage with N TextObject children at independent
// geometries (compound layout — e.g. two side-by-side buttons in the
// bottom corners). Each child carries its own geom + ContainerId +
// ContainerName + Content. `exitFn` is wired via the same
// gTextViewExitFn slot g2ShowTextPage uses, so DOUBLE_CLICK (or any
// SysEvent gesture if tapFn is null) routes through it for dismiss.
//
// Used by the Tests/Display/Selection Patterns test bench to canary
// the firmware's compound-text-container behaviour. SCHEMA RISK: this
// is the first compound shape in the codebase that emits multiple
// TextObject children under wrapper field 3 — verified working only
// at runtime.
bool g2ShowMultiTextPage(const G2TextChildSpec* children, size_t childCount,
                         void (*exitFn)() = nullptr,
                         G2TapFn tapFn = nullptr);

// Compound List + Text page — title (TextObject) + selectable list
// (ListObject). The list manages focus + scroll + CLICK natively, so
// row taps reach the dispatcher via the standard ListEvent path —
// behaves identically to a plain list page from the tap-handling
// perspective. The TextObject is a non-interactive header above (or
// alongside) the list. Use for confirmation prompts, settings
// sections, anything that wants a label above tappable rows.
//
// Both `items` and the title's `containerName` / `content` are deep-
// copied; caller buffers can be reused immediately.
bool g2ShowMixedListText(const char* const* items, size_t itemCount,
                         const G2ContainerGeom& listGeom,
                         const G2TextChildSpec& title);

// REBUILD-text child probe — given a single compound container hosting
// both a TextObject ("title") and a ListObject (the test R shape), can
// we rebuild ONLY the TextObject child via Cmd=7 REBUILD_PAGE without
// disturbing the list? Answers the dual-pane Status UX question: if
// yes, the detail pane updates on selection change without resetting
// list focus; if no, every change requires a full compound REBUILD.
//
// Sequence: tearDown → CREATE compound (title="Initial: foo" + list)
// → hold 1.5 s → REBUILD-text(name="title", content="Updated: bar") →
// wait ack → hold 4 s for visual observation → Shutdown.
//
// Returns a static result string. Synchronous; MUST be called from a
// worker task, not the BLE notify task. The G2_ASSERT_NOT_NOTIFY_TASK
// guard inside the helpers will catch a misuse.
//
// (Replaces the retired g2ProbeDualPaneCreate; see G2_PROTOCOL.md note
// on single-container-per-widget for why that experiment was closed.)
const char* g2ProbeRebuildTextChild();

// Render a multi-line text blob as a read-only list page within the active
// hijack list container. Splits on '\n', prepends "<- Back", and pushes
// the result via g2ShowListPage. Sets gHijackPage to TEXT_VIEW so the tap
// dispatcher routes idx=0 back to the main menu.
//
// Use for any "info dump" content (Status, Sensors, System) that would
// otherwise need a TEXT-widget REBUILD — REBUILDing a TEXT into a list
// container makes the firmware crash the plugin with a "lost connection"
// overlay (observed 2026-04-24). List-into-list REBUILDs work cleanly.
// `backLabel` overrides the prepended back-row text. Pass nullptr for the
// default "<- Back" — callers should pass the destination name so the
// user knows where the tap goes (e.g. "<- Main Menu", "<- Network").
bool g2ShowTextAsList(const char* text, const char* backLabel = nullptr);

// Display a BMP from VFS as a one-shot image probe — same transport as
// the `g2bmp` CLI command but async with a completion callback. Path is
// heap-copied so the caller's buffer can be reused. Spawns a worker;
// returns true on successful task creation. The worker holds the image
// until the user double-taps (or 60 s safety cap) and then fires the
// optional `onDone` callback so the caller can re-render its own page
// (e.g. the Files page re-CREATEs the file list). `onDone` is called
// from the worker task, NOT the BLE notify task, so it can call other
// page-swap APIs safely.
bool g2ShowBmpFile(const char* path, void (*onDone)() = nullptr);

// Same as g2ShowBmpFile but renders the 288×144 source full-screen by
// 2× upscaling (nearest-neighbour) and shipping as a 2×2 grid of
// 288×144 tiles via the Q12-style multi-image transport. Use when the
// caller wants the existing small image to fill the lens canvas
// without re-prepping the asset on the host. Same async + onDone
// contract as g2ShowBmpFile.
bool g2ShowBmpFileFullScreen(const char* path, void (*onDone)() = nullptr);

// Same shape as g2ShowBmpFile / g2ShowBmpFileFullScreen but the source
// is a JPEG file (e.g. snapshots saved by the camera-stream page to
// /sd/PICTURES/cam_<ms>.jpg). The worker reads the JPEG, decodes via
// img_converters.h::fmt2rgb888 to RGB888, downsamples + quantizes to a
// 288×144 4-bpp grayscale BMP (same buildBmp4bppFromRgb888 path the
// camera viewer uses), then pushes through the same wire transport as
// the BMP viewers. Returns true on worker spawn, false on heap-low /
// alloc fail. Requires ENABLE_CAMERA_SENSOR (the JPEG decoder lives
// in the esp32-camera component).
bool g2ShowJpgFile(const char* path, void (*onDone)() = nullptr);
bool g2ShowJpgFileFullScreen(const char* path, void (*onDone)() = nullptr);

// Capture one camera frame, decode JPEG → grayscale, render in the
// small 288×144 image container and hold until the user double-taps.
// One-shot: there's no live-feed refresh and no single-tap recapture
// (image-only widget state on this firmware only emits DOUBLE_CLICK,
// so single-tap is silent). Returns true if the worker spawned;
// returns false if camera is disabled / heap is too low. `onDone` is
// invoked from the worker task once the user dismisses or the 60 s
// safety cap fires — caller typically uses it to redraw its menu.
bool g2ShowCameraViewer(void (*onDone)() = nullptr);

// Continuous camera stream: capture → decode → push, looped until
// the user double-taps. Effective frame rate is bounded by the
// per-frame BLE push time (~2.7 s per 7-fragment 288×144 BMP), so
// expect ~0.4 fps. Same dismiss contract as g2ShowCameraViewer
// (firmware only emits DOUBLE_CLICK on image-only state). Returns
// true if the worker spawned. `onDone` fires on stop / error.
bool g2ShowCameraStream(void (*onDone)() = nullptr);

// When the camera stream's "Settings >>" row is tapped, the worker sets
// this flag and chains into g2ShowCameraSettingsMenu(). The settings
// page's back-row handler checks this and, if set, RELAUNCHES the stream
// instead of returning to the CAM detail page (so the user immediately
// sees their setting changes apply on the live stream). Cleared by the
// settings page after consuming, and at the top of g2CameraStreamWorker
// so a fresh entry never inherits a stale flag.
extern volatile bool g2CamStreamSettingsExitRelaunch;

// MIC detail compound page entry / tap handler. Reached from
// Sensors → MIC tap; spawns the live-text worker with a render fn
// that does the initial list+text CREATE, then per-tick UPDATE_TEXT
// (Cmd=5) on the readout child only — list child is never touched
// after CREATE so row selection persists indefinitely. Tap handler
// is registered on kMicDetailPage.handleTap and invoked by the
// hijack-tap dispatcher when the active page is MIC_DETAIL.
bool g2ShowMicDetail();
void g2MicDetailHandleTap(uint32_t idx);

// Generic sensor-detail LIVE compound (all non-camera sensors). Spawns the
// shared live-text worker with a render fn that CREATEs a selectable list
// (back / Auto Start) + a live readout child, then per-tick UPDATE_TEXTs the
// readout only (selection persists). Reuses the Sensors hijack page + tap
// handler (no new page module). Entry: showSensorDetail() in G2_Page_Sensors.cpp.
bool g2ShowSensorLive();

// One-shot inter-fragment cadence override for the next sendPbFragmented
// burst. 0 (or never calling) leaves the 20 ms default in place; any
// other value applies to all fragments of the next multi-fragment send,
// then auto-clears. Used by the Transport Tests bench to probe whether
// firmware reassembly tolerates more bytes when given more time per
// fragment. Test setting → call g2ShowTextPage / g2ShowListPage in the
// same dispatcher tick → the page-swap worker's first multi-fragment
// CREATE consumes the override.
void g2DebugSetNextBurstFragDelay(uint32_t delayMs);

// Live list page primitive — periodically rebuilds a list-shaped page in
// place via Cmd=7 REBUILD-list (no flicker). `buildFn` is called on each
// tick to repopulate the text content (same shape as the page module's
// `buildText` callback); the worker splits on newlines, prepends a
// "<- Back" row, and ships a REBUILD. Double-tap (SysEvent DOUBLE_CLICK
// src=2) kicks an immediate refresh. Single tap goes through the normal
// row-tap path → page-swap, which auto-cancels the live worker.
//
// One live page at a time; calling this while another is active stops
// the previous one first. Returns true on a successful initial render.
typedef void (*G2LivePageBuildFn)(char* out, size_t cap);
// `backLabel` overrides the auto-prepended back-row text for this session.
// Pass nullptr for the default "<- Back". Use this to surface where the
// back tap actually goes (e.g. "<- Main Menu", "<- Network", or repurpose
// it as "X Cancel" for an overlay).
bool g2StartLiveListPage(G2LivePageBuildFn buildFn, uint32_t intervalMs,
                         const char* backLabel = nullptr);
void g2StopLiveListPage();

// Wake the live-page worker so it rebuilds NOW instead of waiting for the
// next interval tick. Use after mutating state the buildFn reads (e.g.
// text-entry append/backspace) to get an immediate REBUILD-list. No-op
// when no live page is active. Same path the ring's double-tap takes.
void g2KickLivePageRefresh();

// Live TEXT page primitive — same lifecycle as g2StartLiveListPage but
// uses the TEXT widget path (Cmd=7 REBUILD_PAGE) instead of REBUILD-list.
// Text widgets have no row selection / scroll state, so REBUILDs snap
// the new content in place — no visual cycling for long content that
// refreshes periodically. NO tappable back row — exit is DOUBLE_CLICK
// on the lens. Use this for read-only info pages (Status, Sensors)
// whose content is longer than one screen. Page modules opt in via the
// `prefersTextWidget` flag on G2PageModule; the dispatcher then chooses
// this over the live-list path automatically.
//
// `renderFn` (optional, default null) replaces the default buildFn +
// g2ShowText path with a caller-owned render hook. When non-null, the
// worker calls renderFn() instead of buildFn(buf,2048)+g2ShowText(buf)
// on both the initial render and every subsequent tick. Use for
// compound layouts (multiple TextObject children at independent geoms)
// where the default single-TEXT widget is too constraining — see
// renderStatusCompound. buildFn must still be non-null (registry
// validation requires it) but is not invoked when renderFn is set.
bool g2StartLiveTextPage(G2LivePageBuildFn buildFn, uint32_t intervalMs,
                         bool (*renderFn)() = nullptr);
void g2StopLiveTextPage();

// ─── Phase 2B: G2 mic → ESP-SR AFE feed ──────────────────────────────
// When ESP-SR's mic source is switched to G2_LEFT, it drains 16 kHz mono
// int16 PCM samples from this ring buffer instead of reading I2S. The
// BLE notify task on 6402 decodes 5×40 B LC3 → 800 samples per packet
// and pushes here. Caller must arrange a StartUpPage container is
// active and `g2micon` has been issued before turning this on, or the
// firmware won't send audio.
bool g2MicSetAfeFeedActive(bool on);
bool g2MicAfeFeedIsActive();
// Turn the glasses' LC3 audio stream on/off (AudioCtrCmd{AudoFuncEn}) on the
// LEFT temple only. Idempotent; returns false if the LEFT arm is down or the
// send fails, so a caller can treat "armed but no enable" as a hard failure
// instead of a silent dead mic. HAL_Audio drives this from capture start/stop.
bool g2MicStreamEnable(bool on);
// Drains up to `capSamples` samples into `out`. Returns number of
// samples actually read (0 on timeout). Blocks up to `timeoutMs` if
// the ring is empty.
size_t g2MicReadPcmSamples(int16_t* out, size_t capSamples, uint32_t timeoutMs);
// Telemetry: number of samples currently buffered + cumulative
// overrun count (samples dropped because writer outpaced reader).
size_t g2MicAfeRingDepth();
uint32_t g2MicAfeOverrunCount();

// Page-mode tracker for the hijacked Blocks menu. Stateful pages register
// their identity here so handleHijackMenuTap() can route taps to the
// right per-page handler. See G2HijackPage enum in G2_Glasses.cpp
// for the values; reproduced here so per-page modules don't need to
// include the cpp.
enum G2HijackPage : uint8_t {
  G2_HIJACK_PAGE_MAIN      = 0,
  G2_HIJACK_PAGE_FILES     = 1,
  G2_HIJACK_PAGE_SETTINGS  = 2,
  G2_HIJACK_PAGE_NETWORK   = 3,
  // Generic read-only text views (e.g. a command result shown as text).
  // Rendered as a list with "<- Back" at idx 0 and one line per item —
  // never as a TEXT widget — because REBUILDing a TEXT widget into a
  // container that was CREATEd as a LIST widget makes the firmware bail out
  // with its generic "lost connection" overlay. No registered page owns this
  // id, so a tap at idx 0 falls back to MAIN (see handleHijackMenuTap).
  // (Status used to share this id; it now has G2_HIJACK_PAGE_STATUS so its
  // back row can route to the System launcher.)
  G2_HIJACK_PAGE_TEXT_VIEW = 4,
  // On-glasses test suite for transport / lens experiments. Stateful page
  // with size-bracket items ("Send 1 KB", etc.) so we can validate the
  // multi-fragment CREATE path against real hardware without a recompile
  // each time.
  G2_HIJACK_PAGE_TESTS     = 5,
  // Sensors landing list (one row per compiled-in sensor) and the
  // per-sensor detail sub-page reached by drilling into a row. Stateful
  // because the dispatcher needs to know whether a tap is "open this
  // sensor" or "act on the open sensor".
  G2_HIJACK_PAGE_SENSORS   = 6,
  // Power page: Restart / Power Off, with a confirmation sub-list so a
  // stray tap can't reboot the device. Stateful because the tap
  // dispatcher needs to know whether the user is on the action list or
  // the confirmation prompt.
  G2_HIJACK_PAGE_POWER     = 7,
  // Camera settings sub-page reached by drilling from Sensors → CAM.
  // Hidden from the main hijack menu (registered with hijackLabel=
  // nullptr); navigated to programmatically. Stateful — each tap
  // cycles the targeted setting's value and re-renders.
  G2_HIJACK_PAGE_CAMERA_SETTINGS = 8,
  // MIC detail compound page (list + live-readout text). Reached only
  // via Sensors → MIC; hidden from the main hijack menu so the entry
  // point is single-source. Drives a live readout via Cmd=5
  // UPDATE_TEXT (per-widget data push, no REBUILD) so list-row
  // selection persists across ticks — see g2ShowMicDetail.
  G2_HIJACK_PAGE_MIC_DETAIL      = 9,
  // ESP-NOW App — top-level page exposing actions (send / broadcast / ping /
  // peer detail / stats) over the existing ESP-NOW backend. Separate from
  // Network → ESP-NOW (which remains for settings/info: toggle, name,
  // auto-start, paired-device list). Stateful: tracks its own sub-mode
  // (peers / peer detail / broadcast / stats) and selected peer index in
  // file-static within G2_Page_ESPNow.cpp.
  G2_HIJACK_PAGE_ESPNOW_APP      = 10,
  // Apps launcher — a submenu grouping the app-like pages (ESP-NOW App,
  // Files) plus the Maps viewer under one top-level entry, keeping the
  // main hijack menu short. Stateless: each row forwards to another page's
  // show*Menu() (which flips gHijackPage) or launches the map viewer.
  G2_HIJACK_PAGE_APPS            = 11,
  // Automations App — a tap-navigated list of the device's saved
  // automations with per-item Run / Enable / Disable. Reached via the Apps
  // launcher (hidden from the main hijack menu). Impl in
  // G2_Page_Automations.cpp.
  G2_HIJACK_PAGE_AUTOMATIONS     = 12,
  // Apps → Health — Overview list+text; metric rows list+image graphs.
  // G2_HIJACK_PAGE_RING is a retired alias for the old live-text ringdash.
  G2_HIJACK_PAGE_HEALTH          = 13,
  G2_HIJACK_PAGE_RING            = 13,
  // LED control sub-page (color / effect / brightness). Reached by drilling
  // from Sensors -> LED, mirroring the Camera Settings pattern (hidden from
  // the main hijack menu). Impl in G2_Page_LED.cpp.
  G2_HIJACK_PAGE_LED             = 14,
  // System Events viewer — read-only live list of the device's typed event
  // ring (kind / subject / age). Reached via the System launcher (hidden from
  // the main hijack menu). Live-text page; impl inline in G2_Glasses.cpp.
  // NOTE: this is the ESP32-side event ring, NOT the G2's own native firmware
  // notification system (a separate, deferred effort).
  G2_HIJACK_PAGE_SYSEVENTS       = 15,
  // FM radio tuner — action rows (seek/volume/mute/power) beside a live
  // readout (freq or seek progress, RDS, volume, signal). Reached by tapping
  // the FM row on the Sensors page (hidden from the main hijack menu).
  // Live-text compound mirroring the Ring/sensor-live pattern; impl inline
  // in G2_Glasses.cpp under #if ENABLE_FM_RADIO.
  G2_HIJACK_PAGE_FMRADIO         = 16,
  // Sensor-logging control — start/stop + a per-sensor selection list.
  // Reached via the Apps launcher (hidden from the main menu). Plain list
  // page; impl inline in G2_Glasses.cpp.
  G2_HIJACK_PAGE_LOGGING         = 17,
  // User manager (admin) — list / add / delete / role change. Reached via the
  // Config launcher (row shown only for an admin pairer; hidden from the main
  // menu). Impl in G2_Page_Users.cpp.
  G2_HIJACK_PAGE_USERS           = 18,
  // System launcher — groups the status/diagnostics pages (Status, System
  // Events, Logging, Tests) under one top-level menu entry, mirroring the
  // OLED's "System" category. Stateless: each row forwards to another page's
  // show*Menu() / live-page start. Impl inline in G2_Glasses.cpp.
  G2_HIJACK_PAGE_SYSTEM          = 19,
  // Config launcher — groups the configuration pages (Settings, Users, OLED
  // Login) under one top-level menu entry, mirroring the OLED's "Config"
  // category. Stateless. Impl inline in G2_Glasses.cpp.
  G2_HIJACK_PAGE_CONFIG          = 20,
  // Status dashboard — live-text compound (body + battery corner). Given its
  // own id (rather than the shared TEXT_VIEW) by the menu reorg so its back
  // row can route to the System launcher it now lives under, without also
  // capturing generic TEXT_VIEW views (which still fall back to MAIN).
  G2_HIJACK_PAGE_STATUS          = 21,
  // LLM guided-input submenu (Apps -> LLM): Open chat / Ask (guided) /
  // Re-run last / Model select. Reached via the Apps launcher (hidden from
  // the main menu). Impl inline in G2_Glasses.cpp under
  // #if ENABLE_ONDEVICE_LLM. NOTE: this id must live here, not as a local
  // cast in the .cpp — a local `(G2HijackPage)N` silently aliases whichever
  // real member owns N once the enum grows, and the tap dispatcher resolves
  // pages by id (first match wins), so the collision hands this page's taps
  // to the other module.
  G2_HIJACK_PAGE_LLM_MENU        = 22,
};
G2HijackPage g2GetHijackPage();
void         g2SetHijackPage(G2HijackPage p);

#if ENABLE_FM_RADIO
// Open the FM tuner compound page (Sensors → FM). Defined in G2_Glasses.cpp.
bool g2ShowFmTunerPage();
#else
inline bool g2ShowFmTunerPage() { return false; }
#endif

// =============================================================================
// G2 lens-state struct
// =============================================================================
// Single source of truth for "what is currently shown on the G2 lens" plus
// the metadata around it (hijack lifecycle, transient overlays, container
// type). Read via g2LensGetState() — returns a snapshot, cheap to copy.
// Mutate only via the dedicated setters; this keeps every state change in
// one place where we can log / SSE / fire side-effects consistently.
//
// Why this exists: before consolidation, lens state lived in five separate
// places — gHijackActive (G2_Glasses.cpp), gHijackPage (same file),
// per-temple G2Temple.containerReady/containerIsList, and gOverlayDeadline
// (G2_Page_Files.cpp). New transient views (notifications, modal dialogs,
// confirmation prompts) would each have spawned their own file-static,
// invisible to other modules. The struct collects them so adding new
// overlay kinds is a one-liner in the enum below.

// Reasons a transient overlay is taking the lens. The dispatch layer uses
// this so a generic "tap=back during overlay" can do the right thing
// (return to whatever the page was before the overlay went up). Add
// values here as new overlay sources appear.
enum G2OverlayKind : uint8_t {
  G2_OVERLAY_NONE         = 0,
  G2_OVERLAY_FILE_INFO    = 1,  // Files page: file metadata flash
  G2_OVERLAY_NOTIFICATION = 2,  // g2notify / g2ShowNotification placeholder
};

struct G2LensState {
  // Hijack lifecycle (mirrors gHijackActive / gHijackStartedMs)
  bool          hijackActive;
  uint32_t      hijackStartedMs;
  G2HijackPage  hijackPage;

  // Active container — mirrors the right-temple's containerReady /
  // containerIsList. Only the right temple drives rendering, so this
  // reflects what the user actually sees.
  bool          containerReady;
  bool          containerIsList;
  uint32_t      containerWidgetId;

  // Transient overlay (auto-dismisses). overlayDeadlineMs == 0 means
  // no overlay is active.
  G2OverlayKind overlayKind;
  uint32_t      overlayDeadlineMs;
};

// Read-only snapshot of the current lens state.
G2LensState g2LensGetState();

// =============================================================================
// G2 page module registry
// =============================================================================
// Each page (Status, Sensors, System, Network, Files, Settings, ...)
// supplies one of these and registers it via g2RegisterPage(). The
// registry then drives:
//   * The hijack main-menu items (label + tap order)
//   * Tap dispatch when each page is the active hijack page
//   * The CLI command set (cmd_g2<name> templated below)
//
// Why this exists: before the registry, the hijack tap dispatcher was a
// hand-written switch with one case per page, and each page had a near-
// identical CLI command. Adding a page meant editing four places. Now
// it's one g2RegisterPage call.

struct G2PageModule {
  // CLI / JSON identifier for the page. Conventional shapes: "status",
  // "sensors", "system", "network", "files", "settings". Used as the
  // suffix for cmd_g2<name>.
  const char*   name;

  // Label shown in the hijack menu. nullptr keeps the page out of the
  // menu (CLI-only).
  const char*   hijackLabel;

  // Optional CLI help string. nullptr falls back to a generic default.
  const char*   cliHelp;

  // Build text content into `out` (CLI direct invocation path).
  // Required.
  void          (*buildText)(char* out, size_t cap);

  // Optional list-mode entry for hijack tap. If null, the hijack tap
  // calls buildText() and renders via g2ShowTextAsList. Use this when
  // your page needs a custom list layout (Files, Settings, Network).
  void          (*showMenu)(void);

  // Optional tap handler when this page is the active hijack page. If
  // null, the dispatcher uses TEXT_VIEW behaviour: idx 0 → MAIN, rest
  // no-op. Set this for stateful pages.
  void          (*handleTap)(uint32_t idx);

  // Page identity in the lens-state hijackPage tracker. Used by the
  // dispatcher to figure out whose handleTap to call.
  G2HijackPage  hijackPage;

  // Optional: when > 0 and showMenu is null, the dispatcher renders the
  // page via g2StartLiveListPage() instead of g2ShowTextAsList(). The
  // worker calls buildText every liveIntervalMs and REBUILDs the list
  // in place — no flicker. Double-tap on the lens kicks an immediate
  // refresh. 0 means "static" (one-shot render, current default).
  uint32_t      liveIntervalMs;

  // Optional back-row label shown as item 0 of the rendered text view.
  // Conventional value: "<- Main Menu" for top-level pages so the user
  // knows the back tap returns to the hijack root rather than just
  // "back". nullptr falls back to "<- Back". Only consulted when the
  // dispatcher renders via g2ShowTextAsList / g2StartLiveListPage —
  // pages with their own showMenu render the back row themselves, and
  // pages that opt into the text-widget path (prefersTextWidget below)
  // ignore this entirely (no tappable rows).
  const char*   backLabel;

  // When true (and liveIntervalMs > 0), the dispatcher renders this
  // page using g2StartLiveTextPage() — Cmd=7 REBUILD_PAGE on a single
  // TEXT widget — instead of g2StartLiveListPage(). Use this for
  // read-only info pages whose content is longer than one screen and
  // refreshes periodically: REBUILDing a list resets selection/scroll
  // and produces visible cycling, while a text widget snaps the new
  // content in place. There is no tappable back row in this mode —
  // exit is via DOUBLE_CLICK (firmware emits this on the lens). Pages
  // with their own showMenu (Network, Files, Settings, Tests) ignore
  // this flag because they need tappable rows.
  bool          prefersTextWidget;

  // Optional custom live-render hook. When non-null AND prefersTextWidget
  // is also true, the live-text worker calls this instead of the default
  // buildText+g2ShowText path on every tick (initial CREATE and
  // subsequent REBUILDs). The implementation owns its own widget
  // lifecycle — typically a compound CreateStartUpPage with multiple
  // TextObject children at independent geoms (Status uses this for the
  // body+battery-corner layout). Returns true on success; false aborts
  // the worker. Pages without compound layout needs leave this nullptr.
  bool          (*liveRender)();
};

// Register a page module. Idempotent: a re-registration with the same
// name overwrites. Returns true on success; false if the registry is
// full or the spec is invalid.
bool g2RegisterPage(const G2PageModule& spec);

// Iterate registered pages.
size_t                g2RegisteredPageCount(void);
const G2PageModule*   g2RegisteredPageAt(size_t i);
const G2PageModule*   g2FindPageByName(const char* name);
const G2PageModule*   g2FindPageByHijackPage(G2HijackPage page);

// Mutators. Each one logs the transition for visibility. Setting
// hijackActive=true also stamps hijackStartedMs.
//
// Phase 5: these are now thin event dispatchers. The actual gLens.*
// writes happen on the FSM worker task (see g2LensApply* below). Reads
// of gLens via g2LensGetState() / g2LensSnapshot() are eventually
// consistent — fine for status JSON / external displays. Code that
// needs a synchronous "is the hijack live?" answer should consult
// hijackFsmState() (single-writer-task, atomic load) instead.
void g2LensSetHijackActive(bool active);
void g2LensSetContainer(bool ready, bool isList, uint32_t widgetId);
void g2LensClearContainer();
void g2LensStartOverlay(G2OverlayKind kind, uint32_t durationMs);
void g2LensClearOverlay();

// FSM worker hooks (Phase 5). NOT for general callers — these are the
// raw mutators the FSM worker invokes when it processes the
// corresponding event. Calling them directly bypasses the dispatch /
// verify machinery and will desync the FSM from gLens.
void g2LensApplyHijackActive(bool active);
void g2LensApplyContainer(bool ready, bool isList, uint32_t widgetId);

// Tick the overlay clock. Call from g2Tick (already wired). When an
// overlay's deadline expires, this clears it AND optionally fires a
// caller-registered "overlay expired" callback. Per-page modules
// (G2_Page_Files etc.) own that callback so they can re-render their
// list. See g2LensSetOverlayExpiredCb.
typedef void (*G2OverlayExpiredCb)(G2OverlayKind kind);
void g2LensSetOverlayExpiredCb(G2OverlayExpiredCb cb);

// Convenience predicate: true while a transient overlay is showing.
// Page modules use this to make context-sensitive decisions (e.g.
// "<- Back" during overlay returns to the page's main view, not the
// hijack main menu).
inline bool g2LensInOverlay() {
  return g2LensGetState().overlayKind != G2_OVERLAY_NONE;
}

// PLACEHOLDER notification API. Not a real overlay — uses the full-screen
// text display with an auto-dismiss timer, so it WILL wipe whatever is on
// the lens for the duration. When the timer expires, the display clears
// back to blank. If another notification arrives before the timer fires,
// the newer one wins and the older timer no-ops.
//
// Real overlay notifications require the undocumented JSON-over-EFS
// protocol (see chat logs 2026-04-24 notification research). When that
// protocol is reverse-engineered, the implementation of this function
// swaps under the same API. `durationMs` is the clear-after time; 0
// means "don't auto-clear" (manual g2clear required).
bool g2ShowNotification(const char* text, uint32_t durationMs = 5000);

// Push a TRUE firmware-native notification card to the G2 (Even File Service).
// Unlike g2ShowNotification (full-screen placeholder), the firmware renders its
// own card, auto-wakes the display, and applies its own silent/DND. Enqueues
// onto the lens-applier worker — never sends BLE inline, so it is safe to call
// from any context. Returns false if no G2 is connected or the enqueue fails.
// See docs/G2_NATIVE_NOTIFICATION_PLAN.md.
bool g2SendNativeNotificationAsync(const char* appId, const char* displayName,
                                   const char* title, const char* subtitle,
                                   const char* body);
bool g2ShowMultiLine(const char* lines[], size_t lineCount);
bool g2ClearDisplay();

// Event handling
void g2SetEventCallback(G2EventCallback callback);
void g2Tick();  // Call from main loop to process events

// Status
void getG2Status(char* buffer, size_t bufferSize);

// Low-level packet functions (for advanced use)
uint16_t g2CalcCRC16(const uint8_t* data, size_t len);
bool g2SendPacket(uint8_t serviceHi, uint8_t serviceLo, const uint8_t* payload, size_t payloadLen);
size_t g2EncodeVarint(uint32_t value, uint8_t* buffer);

// -----------------------------------------------------------------------------
// Image-streaming discovery probes
// -----------------------------------------------------------------------------
// Schema for image streaming (Cmd=3 ImageRawDataUpdate body and the
// CREATE-image variant of CreateStartUpPageContainer) is NOT in our local
// reference doc. These probes ship candidate pb shapes and rely on the
// firmware's response error codes (ImgRawFailed=5, RebuildFailed=7,
// InvalidContainer=1, etc) to narrow down the real schema. Each probe
// logs verbosely via DEBUG_G2F so the operator can capture the response
// trail and report back. None of them attempt to render anything visible
// — the goal is purely wire-level discovery before we commit to a real
// image-send implementation.
//
// All five return a short summary string (caller can ignore). Side
// effect: each fires one or more BLE writes and logs to the serial /
// in-memory debug ring; nothing else changes.

// Probe Q4 — lifecycle / sequential-frames probe. Sends three Cmd=3
// frames in rapid succession (no CREATE between them) with distinct
// magics and slightly different payloads. Tells us:
//   - Whether the firmware rate-limits or accepts back-to-back Cmd=3
//   - Whether all three reject the same way (no-container) or the
//     first behaves differently from the rest ("first frame dropped"
//     hypothesis from the doc)
const char* g2ProbeImageQ4Lifecycle();

// Doc summary — logs every image-related fact we already have from
// `docs/G2_PROTOCOL.md` so the operator has a single console paste-able
// reference next to the probe responses. No BLE TX.
const char* g2ProbeImageDocSummary();

// Probe Q6 — multi-fragment BMP push. Builds a full-tile 288×144 BMP
// (~20 KB), splits into ~3 KB chunks, ships them as sequential Cmd=3
// fragments with the same MapSessionId. Tests image-layer
// fragmentation end-to-end and characterises throughput/ack-pacing
// for a real-size frame.
const char* g2ProbeImageQ6BmpMultiFragment();

// Probe Q6b — Q6 with double-tap-to-dismiss instead of fixed 3 s hold.
// Hold ends on SysEvent DOUBLE_CLICK(3) on sid=0xE0 (from ring or
// temple) or 60 s safety cap. Single CLICK and SCROLL events don't
// fire while a pure image is on-lens — confirmed 2026-04-27 with
// firmware 2.2.0.24, so double-tap is the only working dismiss
// gesture in this state.
const char* g2ProbeImageQ6bBmpTapDismiss();

// Probe Q9 — frame builder API. Builds a full-tile 288×144 BMP from
// rect primitives (white header band, stripes body, black footer band)
// to validate the bmpDrawRect4bpp helper that'll back a future
// pushTile(arm, bmp288x144) public API for feature code.
const char* g2ProbeImageQ9FrameBuilder();

// Probe QGlizzy — static-image canary at hardcoded path. Loads
// /sd/PICTURES/test.bmp via VFS and ships it through the standard
// CREATE+multi-fragment pipeline. No operator input — useful as a
// quick "is the SD-backed image-display path alive" check.
const char* g2ProbeImageQGlizzy();

// Probe Q10 — streaming swap with intermediate clear. CREATE → push
// frame A → 2 s hold → push all-black → 1 s hold → push frame B, no
// re-CREATE between frames. Tests whether an explicit black-clear
// improves transition quality between content frames; only meaningful
// if Q11 shows tearing or ghosting.
const char* g2ProbeImageQ10ClearThenPush();

// Probe Q11 — minimal streaming swap. CREATE once → push frame A →
// 3 s hold → push frame B (different content) into the same container
// without re-CREATE. The pivotal probe: if B replaces A cleanly, the
// firmware accepts back-to-back Cmd=3 sessions on a single CREATE and
// the per-frame cost drops from ~3 s (with re-CREATE) to ~2.5 s.
const char* g2ProbeImageQ11SimpleSwap();

// Probe Q12 — full-display 576×288 image as a 2×2 grid of 288×144
// tiles. Single CREATE declares all 4 ImageObject children; subsequent
// Cmd=3 streams target each tile by its CID/name pair. Validates the
// firmware accepts multi-tile CREATE geometry and that the four tiles
// align cleanly on lens (centre indicator: a 48×48 white block formed
// by each tile's inside-corner 24×24 square).
const char* g2ProbeImageQ12FullScreen();

// Probe Q13 — live image-tile pipeline. Single 288×144 container,
// pushes a fresh BMP every `g2liverate` ms (CLI-tunable). Each frame
// shifts a horizontal bar so the update is visibly different. Loops
// until double-tap or safety cap. Logs req-vs-actual cadence per frame.
const char* g2ProbeImageQ13LiveTile();

// Probe Q14 — live TEXT REBUILD pipeline. CREATE once, REBUILD_PAGE
// (Cmd=7) every `g2liverate` ms with a fresh "Live #N up=Xs" string.
// Single envelope per update vs Q13's ~7-fragment burst — useful for
// characterising the fastest text-update cadence the firmware accepts.
const char* g2ProbeImageQ14LiveText();

// Probe Q15 — LEFT-arm image push test. Same BMP/transport as Q6 but
// explicitly targets the LEFT temple instead of the default RIGHT.
// Tests the 2026-04-28 third-party claim that "image fragments are
// faster on the left arm." Compares burst time to Q6's right-arm
// baseline (~2.6 s on this firmware/stack).
const char* g2ProbeImageQ15LeftArm();

// Probe Q16 — mixed CREATE side-by-side. List in top half + 288x144
// image in bottom half, no overlap. Verifies that the firmware
// accepts a single CreateStartUpPageContainer carrying both ListObject
// and ImageObject children (schema-allowed but never tested).
const char* g2ProbeImageQ16MixedSideBySide();

// Probe Q17 — mixed CREATE with overlap. Full-screen list + 288x144
// image positioned to overlap the middle. Determines z-order: does the
// image paint on top of the list, under it, or get rejected.
const char* g2ProbeImageQ17MixedOverlap();

// Probe Q18 — mixed CREATE with icon-sized image. Full-screen list +
// 80x80 image in top-right corner. Tests whether the firmware accepts
// non-standard (non-288x144) image container dimensions, and whether
// matching small BMPs render at the requested geometry.
const char* g2ProbeImageQ18MixedIcon();

// Probe Q19 — solo small-dim image. CREATE-image at 96×96 (no list),
// push stripes BMP. Confirms whether solo image widgets render below
// the 288×144 we've used historically — gates the "fast streaming"
// mode (96×96 → ~1 fps vs 288×144 → 0.45 fps).
const char* g2ProbeImageQ19SmallSolo();

// Probe Q29 — 2-bpp solo BMP at 144×144. Same shape as Q19 but the
// payload is 2-bpp grayscale (4-entry palette) instead of 4-bpp (16
// palette). Tests whether the lens BMP parser honours biBitCount=2.
// If yes, camera streamer can cut payload ~50% with no resolution
// change. If lens stays blank but acks come back, parser is 4-bpp only.
const char* g2ProbeImageQ29Bmp2bppSolo();

// Probe Q20 — same live pipeline as Q13 but 96×96 solo tile (~2 Cmd=3
// fragments per frame). Moving horizontal bar; cadence `g2liverate`.
// Isolates small-dim sustained refresh vs full-tile BLE cost.
const char* g2ProbeImageQ20LiveTile96();

// Q31 / Q31b — can an image CONTAINER be repositioned? (Tests → Image →
// Motion.) Both sweep a 64×64 container rightwards in 64 px steps with a
// 32×32 block drawn inside it, so the operator can see whether the
// CONTAINER moved or only its pixels changed.
//
// Q31 sends Cmd=7 REBUILD with a fresh ImageObject X/Y. Untested territory:
// docs/G2_PROTOCOL.md:1665 calls REBUILD the geometry-change command, but the
// note on g2BuildRebuildList says the firmware ignores geom changes on that
// path, and REBUILD-text fails outright on this firmware. RISK: a bad image
// REBUILD may wedge the EvenCore plugin task (docs/G2_PROTOCOL.md:1813) —
// recovery is a BLE reconnect. Run Q31b first.
//
// Q31b re-CREATEs at the new X (SHUTDOWN+CREATE, known-good) and reports
// ms/step — the number that decides whether position-animation is viable at
// all, independent of how Q31 turns out.
const char* g2ProbeImageQ31RebuildMove();
const char* g2ProbeImageQ31bRecreateMove();

// Probes Q22/Q23 — same live bar pattern as Q20 at 32×32 and 64×64
// (Animated Icons test menu). Cadence `g2liverate`.
const char* g2ProbeImageQ22LiveTile32();
const char* g2ProbeImageQ23LiveTile64();
// Q26 / Q27 — same moving-bar pattern at 124×124 and 144×144 (max solo tile).
const char* g2ProbeImageQ26LiveTile124();
const char* g2ProbeImageQ27LiveTile144();
// (Q24 procedural slime probe removed — superseded by Q25 SD frame packs.)

// Q25 — loop BMPs from a VFS directory: frame_00.bmp … frame_63.bmp max (4bpp,
// |w|≤288, |h|≤144). Call g2ProbeImageQ25SetPackPath(G2_ICON_ANIMATIONS_VFS_PATH "/foo")
// before spawning the probe worker (Test Suite → Choose icon pack).
void        g2ProbeImageQ25SetPackPath(const char* dirPath);
const char* g2ProbeImageQ25SdFrameAnimation();

// Q28 — mixed image+text compound with INDEPENDENT refresh rates. CREATE
// once with text top + image bottom-centered. Loop ~24 frames pushing
// alternating procedural BMPs to the image child every ~750 ms; rebuild
// the text child via single-child REBUILD-text every other frame.
// Validates whether text REBUILD on a compound preserves an image
// sibling (different-widget-types case — should pattern-match the
// list+text verification). Gates a possible camera-stream + caption
// productionisation. Standalone — does NOT touch the existing camera
// stream worker.
const char* g2ProbeImageQ28MixedImageTextLive();

// Probe Q28L — list+image counterpart to Q28. Same image position
// (240, 168), same 96×96 dim, same 750 ms cadence, but uses
// g2BuildCreateMixedListImage (5-item list top + image bottom) instead
// of g2BuildCreateMixedImageText. If the image renders in Q28L but
// stays blank in Q28, the firmware-side rule is "image children only
// composite when paired with a List parent, not Text" — actionable as
// a workaround for caption-style use cases.
const char* g2ProbeImageQ28LMixedListImageLive();

// Q30 family — unproven 3-pane list+text+image CREATE (ContainerTotalNum=3).
// Q30: Health geometry, wire order list→text→image.
// Q30b: same geom, wire order list→image→text.
// Q30c: Health list/text + Q28L-sized 96×96 image (conservative paint path).
const char* g2ProbeImageQ30ListTextImageHealthGeom();
const char* g2ProbeImageQ30bListImageTextOrder();
const char* g2ProbeImageQ30cListTextSmallImage();

// Load a single-tile 4bpp uncompressed BMP from VFS (any size up to 288×144).
// Caller must free(*outData) with free() on success.
bool g2ReadBmp4bppFromVfs(const char* vfsPath, uint8_t** outData, size_t* outLen,
                          int32_t* outW, int32_t* outH, const char** outErr);

// Probe Q21 — Q12-style 2×2 full-screen CREATE, then two update rounds:
// round 0 paints all four tiles; round 1 re-pushes TL+BR only with
// shifted corner markers (disjoint magic bands). Logs wall time per
// round so you can compare multi-tile refresh vs Q13 single-tile rate.
const char* g2ProbeImageQ21LiveFullScreenBurst();

#else // !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)

// -----------------------------------------------------------------------------
// Stub declarations when G2 glasses support is disabled
// (G2State, G2Eye, G2EventType, G2EventCallback defined above the guard)
// -----------------------------------------------------------------------------

inline bool initG2Client() { return false; }
inline void deinitG2Client() {}
inline bool isG2ClientInitialized() { return false; }
inline bool g2Connect(G2Eye eye = G2_EYE_LEFT) { return false; }
inline bool g2ConnectSaved() { return false; }
inline void g2Disconnect() {}
inline bool isG2Connected() { return false; }
inline bool g2LeftConnected() { return false; }
inline G2State getG2State() { return G2_STATE_IDLE; }
inline const char* getG2StateString() { return "disabled"; }
inline bool g2StartScan(uint32_t durationMs = 10000) { return false; }
inline void g2StopScan() {}
inline bool g2ShowText(const char* text) { return false; }
inline bool g2ShowEvenAIReply(const char*, const char*) { return false; }
inline bool g2ShowEvenAIReply(const char*) { return false; }
inline bool g2ShowEvenAIReplyNoAsk(const char*, const char*) { return false; }
inline bool g2ShowEvenAIReplyDirect(const char*, const char*) { return false; }
inline bool g2ShowEvenAIReplyNoAsk(const char*) { return false; }
inline bool g2ShowEvenAIReplyDirect(const char*) { return false; }
inline bool g2HideEvenAICard() { return false; }
inline bool g2ShowTextAsList(const char*, const char* = nullptr) { return false; }
typedef void (*G2LivePageBuildFn)(char* out, size_t cap);
inline bool g2StartLiveListPage(G2LivePageBuildFn, uint32_t,
                                const char* = nullptr) { return false; }
inline void g2StopLiveListPage() {}
inline void g2KickLivePageRefresh() {}
inline bool g2StartLiveTextPage(G2LivePageBuildFn, uint32_t,
                                bool (*)() = nullptr) { return false; }
inline void g2StopLiveTextPage() {}
inline bool g2MicSetAfeFeedActive(bool) { return false; }
inline bool g2MicAfeFeedIsActive() { return false; }
inline bool g2MicStreamEnable(bool) { return false; }
inline size_t g2MicReadPcmSamples(int16_t*, size_t, uint32_t) { return 0; }
inline size_t g2MicAfeRingDepth() { return 0; }
inline uint32_t g2MicAfeOverrunCount() { return 0; }
// NB: when G2 is disabled, these stubs intentionally drop the
// G2ContainerGeom / G2TextChildSpec parameters (those types only exist
// inside the #if branch above). Callers are themselves gated by
// ENABLE_G2_GLASSES, so they shouldn't be reaching these stubs in
// practice — these exist only to keep "find a g2*-prototyped declaration"
// compiles working for code that takes function-pointer addresses or
// uses sizeof on the prototype set.
inline bool g2ShowListPage(const char* const*, size_t) { return false; }
enum G2TapKind : uint8_t { G2_TAP_PAGE_NEXT = 0, G2_TAP_PAGE_PREV = 1 };
typedef void (*G2TapFn)(G2TapKind kind);
inline bool g2ShowTextPage(const char*,
                           void (*)() = nullptr,
                           G2TapFn = nullptr) { return false; }
struct G2TextPageChrome {
  const char *title, *navHint, *singleHint, *separator, *emptyMsg;
};
// Geom param omitted here (like the g2ShowTextPage stub above): G2ContainerGeom
// isn't declared in the G2-disabled build. Nothing calls this stub anyway.
inline bool g2TextPagerRender(struct TextPager&, char*, size_t,
                              const G2TextPageChrome&,
                              void (*)(), G2TapFn) { return false; }
inline bool g2ShowMultiTextPage(const void*, size_t,
                                void (*)() = nullptr,
                                G2TapFn = nullptr) { return false; }
inline bool g2ShowMixedListText(const char* const*, size_t) { return false; }
inline const char* g2ProbeRebuildTextChild() { return "G2 disabled"; }
inline bool g2ShowNotification(const char* text, uint32_t durationMs = 5000) { return false; }
inline bool g2SendNativeNotificationAsync(const char*, const char*, const char*,
                                          const char*, const char*) { return false; }
inline bool g2ShowMultiLine(const char* lines[], size_t lineCount) { return false; }
inline bool g2ClearDisplay() { return false; }
inline void g2SetEventCallback(G2EventCallback callback) {}
inline void g2Tick() {}
inline void getG2Status(char* buffer, size_t bufferSize) { if (buffer) buffer[0] = '\0'; }

inline const char* g2ProbeImageQ4Lifecycle()        { return "G2 disabled"; }
inline const char* g2ProbeImageDocSummary()         { return "G2 disabled"; }
inline const char* g2ProbeImageQ6BmpMultiFragment() { return "G2 disabled"; }
inline const char* g2ProbeImageQ6bBmpTapDismiss()   { return "G2 disabled"; }
inline const char* g2ProbeImageQ9FrameBuilder()     { return "G2 disabled"; }
inline const char* g2ProbeImageQGlizzy()            { return "G2 disabled"; }
inline const char* g2ProbeImageQ10ClearThenPush()   { return "G2 disabled"; }
inline const char* g2ProbeImageQ11SimpleSwap()      { return "G2 disabled"; }
inline const char* g2ProbeImageQ12FullScreen()      { return "G2 disabled"; }
inline const char* g2ProbeImageQ13LiveTile()        { return "G2 disabled"; }
inline const char* g2ProbeImageQ14LiveText()        { return "G2 disabled"; }
inline const char* g2ProbeImageQ15LeftArm()         { return "G2 disabled"; }
inline const char* g2ProbeImageQ16MixedSideBySide() { return "G2 disabled"; }
inline const char* g2ProbeImageQ17MixedOverlap()    { return "G2 disabled"; }
inline const char* g2ProbeImageQ18MixedIcon()       { return "G2 disabled"; }
inline const char* g2ProbeImageQ19SmallSolo()       { return "G2 disabled"; }
inline const char* g2ProbeImageQ29Bmp2bppSolo()     { return "G2 disabled"; }
inline const char* g2ProbeImageQ20LiveTile96()     { return "G2 disabled"; }
inline const char* g2ProbeImageQ22LiveTile32()    { return "G2 disabled"; }
inline const char* g2ProbeImageQ23LiveTile64()    { return "G2 disabled"; }
inline const char* g2ProbeImageQ26LiveTile124()  { return "G2 disabled"; }
inline const char* g2ProbeImageQ27LiveTile144()  { return "G2 disabled"; }
inline const char* g2ProbeImageQ31RebuildMove()  { return "G2 disabled"; }
inline const char* g2ProbeImageQ31bRecreateMove() { return "G2 disabled"; }
inline void        g2ProbeImageQ25SetPackPath(const char*) {}
inline const char* g2ProbeImageQ25SdFrameAnimation() { return "G2 disabled"; }
inline const char* g2ProbeImageQ28MixedImageTextLive() { return "G2 disabled"; }
inline const char* g2ProbeImageQ28LMixedListImageLive() { return "G2 disabled"; }
inline const char* g2ProbeImageQ30ListTextImageHealthGeom() { return "G2 disabled"; }
inline const char* g2ProbeImageQ30bListImageTextOrder() { return "G2 disabled"; }
inline const char* g2ProbeImageQ30cListTextSmallImage() { return "G2 disabled"; }
inline bool g2ReadBmp4bppFromVfs(const char*, uint8_t**, size_t*, int32_t*, int32_t*,
                                 const char**) {
  return false;
}
inline const char* g2ProbeImageQ21LiveFullScreenBurst() { return "G2 disabled"; }

#endif // ENABLE_BLUETOOTH

#endif // G2_GLASSES_H
