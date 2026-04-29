#ifndef OPTIONAL_EVEN_G2_H
#define OPTIONAL_EVEN_G2_H

#include "System_BuildConfig.h"
#include "System_G2_Protocol.h"   // G2ContainerGeom + G2_GEOM_* presets
#include <Arduino.h>

// =============================================================================
// EVEN REALITIES G2 GLASSES - BLE CLIENT
// =============================================================================
// This module implements ESP32 as a BLE Central/GATT Client to connect to
// Even Realities G2 smart glasses. This mode is mutually exclusive with the
// phone BLE server mode (Optional_Bluetooth).
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
  G2_EVENT_DISPLAY_OFF
};

typedef void (*G2EventCallback)(G2EventType event);

// G2 requires Bluetooth to be enabled first
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// -----------------------------------------------------------------------------
// G2 BLE UUIDs (from protocol docs)
// -----------------------------------------------------------------------------
// Legacy #defines — Optional_EvenG2.cpp owns the authoritative UUIDs now.
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

// ─── Phase 2B: G2 mic → ESP-SR AFE feed ──────────────────────────────
// When ESP-SR's mic source is switched to G2_LEFT, it drains 16 kHz mono
// int16 PCM samples from this ring buffer instead of reading I2S. The
// BLE notify task on 6402 decodes 5×40 B LC3 → 800 samples per packet
// and pushes here. Caller must arrange a StartUpPage container is
// active and `g2micon` has been issued before turning this on, or the
// firmware won't send audio.
bool g2MicSetAfeFeedActive(bool on);
bool g2MicAfeFeedIsActive();
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
// right per-page handler. See G2HijackPage enum in Optional_EvenG2.cpp
// for the values; reproduced here so per-page modules don't need to
// include the cpp.
enum G2HijackPage : uint8_t {
  G2_HIJACK_PAGE_MAIN      = 0,
  G2_HIJACK_PAGE_FILES     = 1,
  G2_HIJACK_PAGE_SETTINGS  = 2,
  G2_HIJACK_PAGE_NETWORK   = 3,
  // Read-only text views (Status / Sensors / System). Rendered as a list
  // with "<- Back" at idx 0 and one line per item — never as a TEXT
  // widget — because REBUILDing a TEXT widget into a container that was
  // CREATEd as a LIST widget makes the firmware bail out with its
  // generic "lost connection" overlay.
  G2_HIJACK_PAGE_TEXT_VIEW = 4,
  // On-glasses test suite for transport / lens experiments. Stateful page
  // with size-bracket items ("Send 1 KB", etc.) so we can validate the
  // multi-fragment CREATE path against real hardware without a recompile
  // each time.
  G2_HIJACK_PAGE_TESTS     = 5,
};
G2HijackPage g2GetHijackPage();
void         g2SetHijackPage(G2HijackPage p);

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
// places — gHijackActive (Optional_EvenG2.cpp), gHijackPage (same file),
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
  // pages with their own showMenu render the back row themselves.
  const char*   backLabel;
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
void g2LensSetHijackActive(bool active);
void g2LensSetContainer(bool ready, bool isList, uint32_t widgetId);
void g2LensClearContainer();
void g2LensStartOverlay(G2OverlayKind kind, uint32_t durationMs);
void g2LensClearOverlay();

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
inline bool g2MicSetAfeFeedActive(bool) { return false; }
inline bool g2MicAfeFeedIsActive() { return false; }
inline size_t g2MicReadPcmSamples(int16_t*, size_t, uint32_t) { return 0; }
inline size_t g2MicAfeRingDepth() { return 0; }
inline uint32_t g2MicAfeOverrunCount() { return 0; }
inline bool g2ShowListPage(const char* const* items, size_t itemCount,
                           const G2ContainerGeom& geom = G2_GEOM_LARGE) { return false; }
enum G2TapKind : uint8_t { G2_TAP_PAGE_NEXT = 0, G2_TAP_PAGE_PREV = 1 };
typedef void (*G2TapFn)(G2TapKind kind);
inline bool g2ShowTextPage(const char* content,
                           const G2ContainerGeom& geom = G2_GEOM_LARGE,
                           void (*exitFn)() = nullptr,
                           G2TapFn tapFn = nullptr) { return false; }
inline bool g2ShowNotification(const char* text, uint32_t durationMs = 5000) { return false; }
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

#endif // ENABLE_BLUETOOTH

#endif // OPTIONAL_EVEN_G2_H
