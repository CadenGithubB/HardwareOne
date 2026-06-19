// =============================================================================
// G2 glasses — "ESP-NOW App" page implementation
// =============================================================================
// See header for the contract. This page surfaces ESP-NOW actions on the
// lens (send / broadcast / ping / per-peer detail / stats). State-mutating
// taps route through g2SubmitHijackCommand so they run on cmd_exec_task
// with the glasses user's auth identity (same path the Network page uses).
// Ping is the one exception — it uses the espnowAppPing* API in
// System_ESPNow.h to time the HEARTBEAT+ACK round-trip directly.

#include "G2_Page_ESPNow.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "G2_Glasses.h"
#include "G2_Page_Network.h"      // g2ShowNetworkMenu — used by "Radio Off" jump
#include "G2_Page_TextEntry.h"    // on-glasses keyboard for typed messages
#include "G2_HijackCmd.h"         // G2CmdCookie / g2SubmitHijackCommand / g2BumpMenuGen
#include "System_Debug.h"
#include <new>                    // std::nothrow — RedrawSpec / LensUiJob

#if ENABLE_WIFI
#include <WiFi.h>
#endif

#if ENABLE_ESPNOW
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"   // gRemoteSensorCache + formatRemoteSensorReadable (Bonded Device view)
#endif

// -----------------------------------------------------------------------------
// Local sub-mode state
// -----------------------------------------------------------------------------

enum ESPNowAppSub : uint8_t {
  ESPN_APP_SUB_MAIN        = 0,  // top-level chooser
  ESPN_APP_SUB_PEERS       = 1,  // peer list
  ESPN_APP_SUB_PEER_DETAIL = 2,  // per-peer actions (selected by gSelectedPeer)
  ESPN_APP_SUB_BCAST       = 3,  // broadcast menu (canned + typed)
  ESPN_APP_SUB_STATS       = 4,  // counters + radio info
  ESPN_APP_SUB_INBOX       = 5,  // merged global inbox (chronological)
  ESPN_APP_SUB_PEER_INBOX  = 6,  // per-peer history (uses gSelectedPeer)
  ESPN_APP_SUB_BOND_SENSORS = 7, // list of remote sensors across the bond/mesh
  ESPN_APP_SUB_BOND_DETAIL  = 8, // one remote sensor's live data (gSelSensor*)
};
static ESPNowAppSub gSub = ESPN_APP_SUB_MAIN;

// Mirror Network's setNetSub — every transition bumps the menu generation
// so in-flight cmd_exec callbacks compare against this value and drop
// stale redraws. The cookie's targetNetSub slot carries our ESPNowAppSub
// value for the same reason (round-tripped through g2SubmitHijackCommand).
static inline void setSub(ESPNowAppSub s) {
  if (gSub == s) return;
  gSub = s;
  g2BumpMenuGen();
}

// Peer-detail target. Index into gEspNow->devices[]. Set when the user taps
// a row in the peers list; consulted by showPeerDetail / handlers below.
// -1 means "no peer selected" (peer-detail not reachable).
static int gSelectedPeer = -1;

#if ENABLE_ESPNOW
// Bonded Device view — identity of the remote sensor the user drilled into.
// Stored as MAC+type (not a cache index) so the detail view re-finds the live
// entry even if the cache reshuffles or the entry expires between renders.
static uint8_t          gSelSensorMac[6] = {0};
static RemoteSensorType gSelSensorType   = REMOTE_SENSOR_THERMAL;
#endif

// Forward decls — handlers reference renderers across the file.
static void showMainMenu();
static void showPeersMenu();
static void showPeerDetail();
static void showBroadcastMenu();
static void showStatsMenu();
static void showInboxMenu();
static void showPeerInboxMenu();
static void showBondSensorsMenu();
static void showBondDetailMenu();

// -----------------------------------------------------------------------------
// Tri-state status helpers
// -----------------------------------------------------------------------------
// Three independent states the user cares about:
//   "Radio Off" — WiFi driver itself is down (esp_wifi_set_mode hasn't been
//                 called or was set to WIFI_OFF). ESP-NOW cannot operate.
//   "OFF"       — WiFi up, ESP-NOW subsystem not initialized. Tap to start.
//   "ON"        — gEspNow->initialized. All actions usable.

enum class EspNowAppPhase : uint8_t {
  RadioOff = 0,
  Off      = 1,
  On       = 2,
};

static EspNowAppPhase currentPhase() {
#if ENABLE_WIFI
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_OFF) return EspNowAppPhase::RadioOff;
#endif
#if ENABLE_ESPNOW
  if (gEspNow && gEspNow->initialized) return EspNowAppPhase::On;
#endif
  return EspNowAppPhase::Off;
}

static const char* phaseLabel(EspNowAppPhase p) {
  switch (p) {
    case EspNowAppPhase::RadioOff: return "ESP-NOW: Radio Off";
    case EspNowAppPhase::Off:      return "ESP-NOW: OFF";
    case EspNowAppPhase::On:       return "ESP-NOW: ON";
  }
  return "ESP-NOW: ?";
}

// Convenience guard for action rows: when ESP-NOW isn't ON, we no-op + log.
// Returns true if the caller should bail.
static bool bailIfNotReady(const char* what) {
  if (currentPhase() != EspNowAppPhase::On) {
    DEBUG_G2F("[G2-ESP-NOW-APP] %s: not ready (phase=%d)",
              what, (int)currentPhase());
    return true;
  }
  return false;
}

#if ENABLE_ESPNOW
// -----------------------------------------------------------------------------
// Inbox helpers — message-count summaries + relative-time formatting
// -----------------------------------------------------------------------------

// Total active messages across every paired peer's history. Iterates the
// dynamically-allocated peerMessageHistories[] (capacity, not deviceCount —
// PSRAM grows that array beyond deviceCount when peers come and go).
static int countInboxAll() {
  if (!gEspNow || !gEspNow->initialized ||
      !gEspNow->peerMessageHistories) return 0;
  int total = 0;
  for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
    const PeerMessageHistory& h = gEspNow->peerMessageHistories[i];
    if (h.active) total += h.count;
  }
  return total;
}

// Messages held for one peer. Returns 0 if no history slot exists yet.
static int countInboxForPeer(const uint8_t* mac) {
  if (!gEspNow || !gEspNow->initialized ||
      !gEspNow->peerMessageHistories || !mac) return 0;
  for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
    const PeerMessageHistory& h = gEspNow->peerMessageHistories[i];
    if (h.active && memcmp(h.peerMac, mac, 6) == 0) return h.count;
  }
  return 0;
}

// Column width for the chat view. The lens text grid is ~50 cols (G2_GEOM_LARGE);
// keep a small margin. Long messages are hard-wrapped to this width (never
// truncated) and the conversation is paged — see the chat helpers below.
static constexpr int kChatCols = 48;

// Reassembly scratch for chatLogicalLine — multi-fragment text is stitched by
// reqId (see espnowReassembleByReqId). PSRAM; DRAM is tight. Sized for the max
// reassembled message (ESPNOW_TEXT_MAX_LEN-ish) plus the label.
static EXT_RAM_BSS_ATTR char gChatReasm[1152];

// Build the UNWRAPPED, labelled, sanitized text for one collapsed message:
//   sent     -> "me: <text>"
//   received -> "<name>: <text>"
// Multi-fragment messages are reassembled to their FULL text by reqId (parity
// with the web/BLE UIs, which stitch the same way); an incomplete one (missing
// fragments) gets a " ..." suffix. Printable ASCII only (control/non-ASCII would
// break the layout / the protobuf UTF-8 string). No truncation here — the caller
// wraps and pages. `ref.head` is a zero-copy live-ring pointer; read immediately.
static void chatLogicalLine(char* out, size_t cap, const CollapsedMsgRef& ref) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  const ReceivedTextMessage* msg = ref.head;
  if (!msg) return;

  const char* body   = msg->message;   // single-frame: the record's own text
  const char* suffix = "";
  if (ref.partsTotal > 1) {
    bool complete = false;
    int n = espnowReassembleByReqId(msg->senderMac, ref.reqId, ref.isSent,
                                    gChatReasm, sizeof(gChatReasm), &complete);
    if (n > 0) { body = gChatReasm; if (!complete) suffix = " ..."; }
    // else: fall back to the first fragment (body unchanged)
  }
  if (ref.isSent) {
    snprintf(out, cap, "me: %s%s", body, suffix);
  } else {
    const char* who = msg->senderName[0] ? msg->senderName : "?";
    snprintf(out, cap, "%s: %s%s", who, body, suffix);
  }
  for (char* c = out; *c; c++) {
    if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7E) *c = ' ';
  }
}
#endif  // ENABLE_ESPNOW

// -----------------------------------------------------------------------------
// Lens redraw from cmd_exec_task — mirrors Network's helper
// -----------------------------------------------------------------------------
// Used by every g2SubmitHijackCommand completion callback below. Builds a
// Redraw LensUiJob and enqueues — gen-guard in the lens applier drops it
// if the user has navigated away by the time it lands.

static void enqueueRedrawFromCallback(const G2CmdCookie& cookie,
                                      void (*renderFn)(),
                                      const char* tag) {
  RedrawSpec* spec = new (std::nothrow) RedrawSpec{};
  if (!spec) {
    DEBUG_G2F("[G2-ESP-NOW-APP] %s: RedrawSpec alloc failed", tag);
    return;
  }
  spec->render = renderFn;

  LensUiJob* job = new (std::nothrow) LensUiJob{};
  if (!job) {
    DEBUG_G2F("[G2-ESP-NOW-APP] %s: LensUiJob alloc failed", tag);
    delete spec;
    return;
  }
  job->kind           = LensJobKind::Redraw;
  job->submitMenuGen  = cookie.menuGen;
  job->cmdSeq         = cookie.seq;
  job->targetPage     = cookie.targetPage;
  job->targetNetSub   = cookie.targetNetSub;
  job->payload.redraw = spec;

  if (!g2EnqueueLensJob(job)) {
    DEBUG_G2F("[G2-ESP-NOW-APP] %s: lens job enqueue FAILED", tag);
    delete spec;
    delete job;
  }
}

// Build a cookie for a hijack command originating from this page. Centralised
// so every submit site stamps the same fields and a future schema change
// only touches one place.
static G2CmdCookie buildCookie() {
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gSub;
  return cookie;
}

// -----------------------------------------------------------------------------
// CLI direct-invocation text (no hijack)
// -----------------------------------------------------------------------------
// One-screen status snapshot — same shape as other pages' buildText hooks.
// The page registry wires this as the CLI render for `g2espnowapp`.

void g2BuildESPNowAppInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';

  String s;
  s.reserve(256);
  s += "ESP-NOW App\n";
  s += phaseLabel(currentPhase());
  s += "\n";

#if ENABLE_ESPNOW
  if (gEspNow && gEspNow->initialized) {
    char line[64];
    snprintf(line, sizeof(line), "Mode: %s\n", getEspNowModeString());
    s += line;
    snprintf(line, sizeof(line), "Peers: %d\n", gEspNow->deviceCount);
    s += line;
    snprintf(line, sizeof(line), "TX %lu  RX %lu\n",
             (unsigned long)gEspNow->routerMetrics.messagesSent,
             (unsigned long)gEspNow->routerMetrics.messagesReceived);
    s += line;
    snprintf(line, sizeof(line), "Channel: %u\n", (unsigned)gEspNow->channel);
    s += line;
  }
#endif

#if ENABLE_WIFI
  s += "MAC: ";
  s += WiFi.macAddress();
#endif

  strncpy(out, s.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

// -----------------------------------------------------------------------------
// Main menu
// -----------------------------------------------------------------------------

static void showMainMenu() {
  setSub(ESPN_APP_SUB_MAIN);

  static char stateLine[40];
  static char peersLine[32];
  static char inboxLine[32];

  snprintf(stateLine, sizeof(stateLine), "%s", phaseLabel(currentPhase()));

#if ENABLE_ESPNOW
  int nPeers = (gEspNow && gEspNow->initialized) ? gEspNow->deviceCount : 0;
  int nInbox = countInboxAll();
#else
  int nPeers = 0;
  int nInbox = 0;
#endif
  snprintf(peersLine, sizeof(peersLine), "Peers (%d) >>", nPeers);
  snprintf(inboxLine, sizeof(inboxLine), "Inbox (%d) >>", nInbox);

  const char* items[] = {
    "<- Main Menu",   // 0
    stateLine,        // 1 — tap = toggle ESP-NOW / jump to WiFi
    peersLine,        // 2
    inboxLine,        // 3 — merged chronological view of all peers' history
    "Broadcast >>",   // 4
    "Stats >>",       // 5
    "Bonded Device >>", // 6 — remote sensors streamed across the bond/mesh
  };
  g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  DEBUG_G2F("[G2-ESP-NOW-APP] main menu shown (phase=%d, peers=%d, inbox=%d)",
            (int)currentPhase(), nPeers, nInbox);
}

void g2ShowESPNowAppMenu() {
  showMainMenu();
  g2SetHijackPage(G2_HIJACK_PAGE_ESPNOW_APP);
}

// -----------------------------------------------------------------------------
// Peers list
// -----------------------------------------------------------------------------

static void showPeersMenu() {
  setSub(ESPN_APP_SUB_PEERS);

#if ENABLE_ESPNOW
  // 1 back row + up to 16 peers (gEspNow->devices[] is sized 16).
  static char rows[1 + 16][40];
  const char* ptrs[1 + 16];
  strcpy(rows[0], "<- Back");
  ptrs[0] = rows[0];
  size_t n = 1;

  if (gEspNow && gEspNow->initialized) {
    int count = gEspNow->deviceCount;
    if (count > 16) count = 16;
    for (int i = 0; i < count; i++) {
      const EspNowDevice& d = gEspNow->devices[i];
      // Prefer mesh-cached friendly name if available; fall back to local
      // name; final fallback shows the MAC tail.
      const char* label =
          (d.friendlyName.length() > 0) ? d.friendlyName.c_str() :
          (d.name.length()         > 0) ? d.name.c_str()         : nullptr;
      if (label) {
        snprintf(rows[1 + i], sizeof(rows[1 + i]), "%s", label);
      } else {
        snprintf(rows[1 + i], sizeof(rows[1 + i]),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
      }
      ptrs[n++] = rows[1 + i];
    }
  }

  if (n == 1) {
    static const char* empty[] = { "<- Back", "(no paired peers)" };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2-ESP-NOW-APP] peers menu shown (%u rows)", (unsigned)(n - 1));
#else
  static const char* na[] = { "<- Back", "(ESP-NOW not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// -----------------------------------------------------------------------------
// Peer detail — drilled into by tapping a row in the peers list
// -----------------------------------------------------------------------------

#if ENABLE_ESPNOW
// Format the Ping row given the current ping slot state. Lives here so both
// the initial render and the live-tick refresh produce identical strings.
static void formatPingRow(char* out, size_t cap, const uint8_t* selectedMac) {
  uint32_t rtt = 0;
  uint8_t  pingMac[6];
  EspNowAppPingState st = espnowAppPingPoll(&rtt, pingMac);

  // Only show RTT if the slot's peer matches the one we're looking at; an
  // earlier ping to a different peer would otherwise leak its result onto
  // the row of whoever the user happens to be viewing.
  bool sameTarget = (st != EspNowAppPingState::Idle) &&
                    selectedMac != nullptr &&
                    memcmp(pingMac, selectedMac, 6) == 0;

  if (!sameTarget) { snprintf(out, cap, "Ping"); return; }
  switch (st) {
    case EspNowAppPingState::Pending:
      snprintf(out, cap, "Ping (pending...)"); break;
    case EspNowAppPingState::Ok:
      snprintf(out, cap, "Ping (%lums)", (unsigned long)rtt); break;
    case EspNowAppPingState::Timeout:
      snprintf(out, cap, "Ping (timeout)"); break;
    default:
      snprintf(out, cap, "Ping"); break;
  }
}
#endif

static void showPeerDetail() {
  setSub(ESPN_APP_SUB_PEER_DETAIL);

#if ENABLE_ESPNOW
  if (!gEspNow || !gEspNow->initialized ||
      gSelectedPeer < 0 || gSelectedPeer >= gEspNow->deviceCount) {
    // Selected peer no longer valid (subsystem stopped, peer was unpaired,
    // index went stale). Bounce to the peers list — safer than rendering
    // garbage.
    showPeersMenu();
    return;
  }
  const EspNowDevice& d = gEspNow->devices[gSelectedPeer];

  static char nameLine[40];
  static char macLine[24];
  static char pingLine[32];
  static char msgsLine[32];

  const char* nm =
      (d.friendlyName.length() > 0) ? d.friendlyName.c_str() :
      (d.name.length()         > 0) ? d.name.c_str()         : "(no name)";
  snprintf(nameLine, sizeof(nameLine), "%s", nm);
  snprintf(macLine,  sizeof(macLine),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
  formatPingRow(pingLine, sizeof(pingLine), d.mac);
  snprintf(msgsLine, sizeof(msgsLine), "Messages (%d) >>",
           countInboxForPeer(d.mac));

  const char* items[] = {
    "<- Peers",     // 0
    nameLine,       // 1 — info, no action
    macLine,        // 2 — info, no action
    msgsLine,       // 3 — drills into per-peer inbox slice
    pingLine,       // 4 — Ping (tap to start; row reads "pending..." until refresh)
    "Send: Hi",     // 5
    "Send: OK",     // 6
    "Send...",      // 7 — typed via TextEntry
    "Forget",       // 8
  };
  g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  DEBUG_G2F("[G2-ESP-NOW-APP] peer detail shown (idx=%d, name='%s')",
            gSelectedPeer, nm);
#else
  static const char* na[] = { "<- Back", "(ESP-NOW not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// -----------------------------------------------------------------------------
// Broadcast menu
// -----------------------------------------------------------------------------

static void showBroadcastMenu() {
  setSub(ESPN_APP_SUB_BCAST);
  const char* items[] = {
    "<- Back",          // 0
    "Send: Here",       // 1 — canned
    "Send: OK",         // 2 — canned
    "Send: Help",       // 3 — canned
    "Type message...",  // 4 — opens TextEntry
  };
  g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  DEBUG_G2F("[G2-ESP-NOW-APP] broadcast menu shown");
}

// -----------------------------------------------------------------------------
// Stats menu (info-only — all rows act like back)
// -----------------------------------------------------------------------------

static void showStatsMenu() {
  setSub(ESPN_APP_SUB_STATS);

#if ENABLE_ESPNOW
  static char rows[10][40];
  const char* ptrs[10];
  size_t n = 0;
  strcpy(rows[n], "<- Back"); ptrs[n] = rows[n]; n++;

  snprintf(rows[n], sizeof(rows[n]), "%s", phaseLabel(currentPhase()));
  ptrs[n] = rows[n]; n++;

  if (gEspNow && gEspNow->initialized) {
    snprintf(rows[n], sizeof(rows[n]), "Mode: %s", getEspNowModeString());
    ptrs[n] = rows[n]; n++;
    snprintf(rows[n], sizeof(rows[n]), "Peers: %d", gEspNow->deviceCount);
    ptrs[n] = rows[n]; n++;
    snprintf(rows[n], sizeof(rows[n]), "TX: %lu",
             (unsigned long)gEspNow->routerMetrics.messagesSent);
    ptrs[n] = rows[n]; n++;
    snprintf(rows[n], sizeof(rows[n]), "RX: %lu",
             (unsigned long)gEspNow->routerMetrics.messagesReceived);
    ptrs[n] = rows[n]; n++;
    snprintf(rows[n], sizeof(rows[n]), "Failed: %lu",
             (unsigned long)gEspNow->routerMetrics.messagesFailed);
    ptrs[n] = rows[n]; n++;
    snprintf(rows[n], sizeof(rows[n]), "Channel: %u",
             (unsigned)gEspNow->channel);
    ptrs[n] = rows[n]; n++;
  }

#if ENABLE_WIFI
  String mac = WiFi.macAddress();
  // Show last 6 hex chars as a compact identity anchor.
  if (mac.length() >= 17) {
    snprintf(rows[n], sizeof(rows[n]), "MAC: ..%s", mac.substring(9).c_str());
    ptrs[n] = rows[n]; n++;
  }
#endif

  g2ShowListPage(ptrs, n);
#else
  static const char* na[] = { "<- Back", "(ESP-NOW not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// -----------------------------------------------------------------------------
// Bonded Device — remote sensors streamed across the bond/mesh
// -----------------------------------------------------------------------------
// Reads gRemoteSensorCache (populated by the espnow RX path whenever a worker
// streams sensor data). The sensor list mirrors the OLED Remote-Sensors page;
// the detail view reuses the shared formatRemoteSensorReadable() so any sensor
// renders without per-type code on the lens.

#if ENABLE_ESPNOW
// Cap on rows the list widget shows without awkward scrolling on the lens.
static constexpr size_t kBondSensorDisplayMax = 15;

// Collect valid cache indices in a stable order shared by renderer + tap
// handler, so row N maps to the same entry in both. Returns count collected.
static size_t collectBondSensorIdx(int* out, size_t maxN) {
  size_t n = 0;
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE && n < maxN; i++) {
    if (gRemoteSensorCache[i].valid) out[n++] = i;
  }
  return n;
}

// Re-find a cached entry by MAC+type (the detail view's stored identity).
static RemoteSensorData* findBondSensorEntry(const uint8_t* mac, RemoteSensorType type) {
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    RemoteSensorData& e = gRemoteSensorCache[i];
    if (e.valid && e.sensorType == type && memcmp(e.deviceMac, mac, 6) == 0) return &e;
  }
  return nullptr;
}
#endif  // ENABLE_ESPNOW

static void showBondSensorsMenu() {
  setSub(ESPN_APP_SUB_BOND_SENSORS);

#if ENABLE_ESPNOW
  static char rows[1 + kBondSensorDisplayMax][40];
  const char* ptrs[1 + kBondSensorDisplayMax];
  strcpy(rows[0], "<- Back");
  ptrs[0] = rows[0];
  size_t n = 1;

  int idx[MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE];
  size_t got = collectBondSensorIdx(idx, sizeof(idx) / sizeof(idx[0]));
  for (size_t i = 0; i < got && (n - 1) < kBondSensorDisplayMax; i++) {
    const RemoteSensorData& e = gRemoteSensorCache[idx[i]];
    const char* dn = (e.deviceName[0]) ? e.deviceName : "?";
    snprintf(rows[n], sizeof(rows[n]), "%s - %s", dn, sensorTypeToString(e.sensorType));
    ptrs[n] = rows[n];
    n++;
  }

  if (n == 1) {
    static const char* empty[] = { "<- Back", "(no bonded sensors yet)" };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2-ESP-NOW-APP] bonded sensors shown (%u)", (unsigned)(n - 1));
#else
  static const char* na[] = { "<- Back", "(ESP-NOW not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

static void showBondDetailMenu() {
  setSub(ESPN_APP_SUB_BOND_DETAIL);

#if ENABLE_ESPNOW
  RemoteSensorData* e = findBondSensorEntry(gSelSensorMac, gSelSensorType);
  if (!e) {            // entry expired / peer gone — bounce back to the list
    showBondSensorsMenu();
    return;
  }

  static char hdr[40];
  static char body[200];
  const char* dn = (e->deviceName[0]) ? e->deviceName : "?";
  snprintf(hdr, sizeof(hdr), "%s - %s", dn, sensorTypeToString(e->sensorType));

  int nLines = formatRemoteSensorReadable(e->jsonData, body, sizeof(body), 8);

  // Split `body` (newline-separated "key: value" lines) into row pointers in
  // place — replace each '\n' with a NUL and point a row at each segment.
  static constexpr size_t kMaxFieldRows = 8;
  static char backRow[] = "<- Sensors";
  const char* ptrs[2 + kMaxFieldRows];
  ptrs[0] = backRow;
  ptrs[1] = hdr;
  size_t n = 2;
  char* p = body;
  while (*p && n < 2 + kMaxFieldRows) {
    ptrs[n++] = p;
    char* nl = strchr(p, '\n');
    if (!nl) break;
    *nl = '\0';
    p = nl + 1;
  }

  g2ShowListPage(ptrs, n);
  DEBUG_G2F("[G2-ESP-NOW-APP] bonded sensor detail: %s (%d field lines)", hdr, nLines);
#else
  static const char* na[] = { "<- Back", "(ESP-NOW not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// -----------------------------------------------------------------------------
// Inbox views — merged chronological + per-peer slice
// -----------------------------------------------------------------------------
// Both views read the SHARED message store via the collapsed/direction-aware API
// (espnowCollapsedAllMessages for the global inbox, espnowGetConversation for the
// per-peer inbox) — the same reads the OLED uses, so every interface shows one
// merged sent+received timeline with multi-fragment messages collapsed to a
// single logical row. Zero new storage; espnow_task populates the rings in its
// textQueue drain (System_ESPNow.cpp processMeshHeartbeats).

// Conversation rendering — PAGED TextContainer (scroll up/down through pages,
// exactly like the Files/Settings JSON viewer).
//
// The firmware's reliable single-CREATE text body is small (~180 B; see the JSON
// viewer's FILES_JSON_PAGE_BODY_BUDGET) and the List widget rejects long rows
// outright. So we render the chat as a TextContainer, hard-wrap each message
// across lines (never truncated), and split the whole conversation into
// CHAT_PAGE_BUDGET-sized pages. Scroll-up pages older, scroll-down/tap pages
// newer, double-tap exits. This is the G2-side chunking; the base ESP-NOW store
// is untouched.
#define CHAT_PAGE_BUDGET 176          // < 180 proven-safe text body; header is extra
#define CHAT_MAX_PAGES   32
#define CHAT_BUF_SIZE    4096

#if ENABLE_ESPNOW
// Shared collapse buffer for both inbox views (they never render simultaneously).
static EXT_RAM_BSS_ATTR CollapsedMsgRef gInboxRefs[2 * MESSAGES_PER_DEVICE];
// Full wrapped conversation (PSRAM — DRAM is tight); paged for display.
static EXT_RAM_BSS_ATTR char     gChatBuf[CHAT_BUF_SIZE];
static EXT_RAM_BSS_ATTR char     gChatPageBuf[CHAT_PAGE_BUDGET + 64];  // header + page slice
static EXT_RAM_BSS_ATTR char     gChatLogical[1184];                   // one labelled (reassembled) line, pre-wrap
static uint16_t gChatPageOff[CHAT_MAX_PAGES + 1];  // page start offsets + end sentinel
static int      gChatPageCount = 0;
static int      gChatPage      = 0;                 // current page (0 = oldest)
static char     gChatTitle[20] = {0};
static void   (*gChatExitFn)() = nullptr;
#endif  // ENABLE_ESPNOW

// Text-view exit handlers — the inbox is a TextContainer, so a double-tap routes
// through gTextViewExitFn (armed by g2ShowTextPage) to run the back-navigation.
static void exitInboxToMain()       { showMainMenu(); }
static void exitPeerInboxToDetail() { showPeerDetail(); }

#if ENABLE_ESPNOW
// Hard-wrap `logical` into gChatBuf at <= kChatCols columns (continuation lines
// indented 2 spaces), newline-separated. Never truncates; bounds-checked.
static void chatAppendWrapped(size_t& pos, const char* logical) {
  size_t L = strlen(logical), i = 0;
  bool first = true;
  while (i < L) {
    size_t indent = first ? 0 : 2;
    size_t width  = (size_t)kChatCols > indent ? (size_t)kChatCols - indent : 1;
    size_t take   = (L - i < width) ? (L - i) : width;
    if (pos + indent + take + 1 >= CHAT_BUF_SIZE) break;
    for (size_t k = 0; k < indent; k++) gChatBuf[pos++] = ' ';
    memcpy(gChatBuf + pos, logical + i, take); pos += take;
    gChatBuf[pos++] = '\n';
    i += take;
    first = false;
  }
  gChatBuf[pos] = '\0';
}

// Rough wrapped-byte estimate for one message — used to pick the newest tail
// that fits gChatBuf.
static size_t chatEstBytes(const CollapsedMsgRef& ref) {
  const ReceivedTextMessage* m = ref.head;
  if (!m) return 0;
  size_t lbl     = ref.isSent ? 4 : (strlen(m->senderName[0] ? m->senderName : "?") + 2);
  // Multi-fragment messages reassemble to ~partsTotal*200 B; estimate that so the
  // newest-tail fit below doesn't overflow gChatBuf mid-build.
  size_t textLen = (ref.partsTotal > 1) ? ((size_t)ref.partsTotal * 200)
                                        : strlen(m->message);
  size_t total   = lbl + textLen;
  size_t w       = (size_t)kChatCols > 2 ? (size_t)kChatCols - 2 : 1;
  return total + (total / w + 1) * 3;   // + per-line newline/indent overhead
}

// Split the [0,totalLen) wrapped buffer into <= CHAT_PAGE_BUDGET pages at line
// boundaries. Each wrapped line is <= ~kChatCols, well under the budget.
static void chatSplitPages(size_t totalLen) {
  gChatPageCount = 0;
  size_t i = 0;
  while (i < totalLen && gChatPageCount < CHAT_MAX_PAGES) {
    gChatPageOff[gChatPageCount++] = (uint16_t)i;
    size_t pe = i;
    while (pe < totalLen) {
      size_t le = pe;
      while (le < totalLen && gChatBuf[le] != '\n') le++;
      size_t lineLen = (le < totalLen) ? (le - pe + 1) : (le - pe);
      if (pe > i && (pe - i) + lineLen > CHAT_PAGE_BUDGET) break;
      pe += lineLen;
      if (le >= totalLen) break;
    }
    i = pe;
  }
  gChatPageOff[gChatPageCount] = (uint16_t)totalLen;
}

// Build gChatBuf from the newest messages that fit, then page it. Leaves the
// current page on the newest (last) page.
static void chatBuildPages(int rc) {
  size_t budget = CHAT_BUF_SIZE - 256, acc = 0;
  int start = rc;
  for (int i = rc - 1; i >= 0; i--) {
    if (!gInboxRefs[i].head) continue;
    size_t e = chatEstBytes(gInboxRefs[i]);
    if (acc + e > budget) break;
    acc += e;
    start = i;
  }
  size_t pos = 0;
  gChatBuf[0] = '\0';
  for (int i = start; i < rc; i++) {
    if (!gInboxRefs[i].head) continue;
    chatLogicalLine(gChatLogical, sizeof(gChatLogical), gInboxRefs[i]);
    chatAppendWrapped(pos, gChatLogical);
  }
  chatSplitPages(pos);
  gChatPage = (gChatPageCount > 0) ? (gChatPageCount - 1) : 0;
}

// Assemble header + current page slice and (re)render via g2ShowTextPage. With
// >1 page, scroll/tap pages (navFn); with one page, a tap exits.
static void chatRenderPage(G2TapFn navFn) {
  size_t off   = (gChatPageCount > 0) ? gChatPageOff[gChatPage]     : 0;
  size_t end   = (gChatPageCount > 0) ? gChatPageOff[gChatPage + 1] : 0;
  size_t slice = end - off;

  int hn;
  if (gChatPageCount > 1) {
    hn = snprintf(gChatPageBuf, sizeof(gChatPageBuf), "%s [%d/%d] scroll=page 2x=exit\n",
                  gChatTitle, gChatPage + 1, gChatPageCount);
  } else {
    hn = snprintf(gChatPageBuf, sizeof(gChatPageBuf), "%s  (tap=back)\n", gChatTitle);
  }
  size_t p = (hn > 0) ? (size_t)hn : 0;
  if (slice == 0) {
    snprintf(gChatPageBuf + p, sizeof(gChatPageBuf) - p, "(no messages yet)");
  } else {
    if (p + slice >= sizeof(gChatPageBuf)) slice = sizeof(gChatPageBuf) - p - 1;
    memcpy(gChatPageBuf + p, gChatBuf + off, slice);
    gChatPageBuf[p + slice] = '\0';
  }
  g2ShowTextPage(gChatPageBuf, G2_GEOM_LARGE, gChatExitFn,
                 (gChatPageCount > 1) ? navFn : nullptr);
}

// Scroll/tap page navigation: scroll-up (PREV) = older, scroll-down/tap (NEXT) =
// newer. Wraps around, matching the JSON viewer.
static void chatNavPage(G2TapKind kind) {
  if (gChatPageCount <= 1) return;
  if (kind == G2_TAP_PAGE_PREV) {
    gChatPage = (gChatPage == 0) ? (gChatPageCount - 1) : (gChatPage - 1);
  } else {
    gChatPage = (gChatPage + 1) % gChatPageCount;
  }
  chatRenderPage(chatNavPage);
}
#endif  // ENABLE_ESPNOW

// Inbox views — paged one-pane chat. received = "<name>: ...", sent = "me: ...";
// long messages wrap; scroll/tap pages through history, double-tap exits.
static void showInboxMenu() {
  setSub(ESPN_APP_SUB_INBOX);
#if ENABLE_ESPNOW
  int rc = espnowCollapsedAllMessages(gInboxRefs, 2 * MESSAGES_PER_DEVICE);
  snprintf(gChatTitle, sizeof(gChatTitle), "Inbox");
  gChatExitFn = exitInboxToMain;
  chatBuildPages(rc);
  chatRenderPage(chatNavPage);
  DEBUG_G2F("[G2-ESP-NOW-APP] inbox (text) rc=%d pages=%d", rc, gChatPageCount);
#else
  g2ShowTextPage("ESP-NOW not compiled", G2_GEOM_LARGE, exitInboxToMain, nullptr);
#endif
}

static void showPeerInboxMenu() {
  setSub(ESPN_APP_SUB_PEER_INBOX);
#if ENABLE_ESPNOW
  if (!gEspNow || !gEspNow->initialized ||
      gSelectedPeer < 0 || gSelectedPeer >= gEspNow->deviceCount) {
    showPeersMenu();
    return;
  }
  EspNowDevice& d = gEspNow->devices[gSelectedPeer];
  int rc = espnowGetConversation(d.mac, gInboxRefs, 2 * MESSAGES_PER_DEVICE);
  snprintf(gChatTitle, sizeof(gChatTitle), "%s", d.name.length() ? d.name.c_str() : "peer");
  gChatExitFn = exitPeerInboxToDetail;
  chatBuildPages(rc);
  chatRenderPage(chatNavPage);
  DEBUG_G2F("[G2-ESP-NOW-APP] peer-inbox (text) peer=%d rc=%d pages=%d",
            gSelectedPeer, rc, gChatPageCount);
#else
  g2ShowTextPage("ESP-NOW not compiled", G2_GEOM_LARGE, exitPeerInboxToDetail, nullptr);
#endif
}

// -----------------------------------------------------------------------------
// cmd_exec completion callbacks
// -----------------------------------------------------------------------------

static void onMainRedrawDone(bool ok,
                             const char* result,
                             const G2CmdCookie& cookie,
                             void* /*userData*/) {
  DEBUG_G2F("[G2-ESP-NOW-APP] main redraw cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  enqueueRedrawFromCallback(cookie, &showMainMenu, "main redraw");
}

static void onPeerDetailRedrawDone(bool ok,
                                   const char* result,
                                   const G2CmdCookie& cookie,
                                   void* /*userData*/) {
  DEBUG_G2F("[G2-ESP-NOW-APP] peer-detail redraw cmd done: ok=%d result='%s'",
            (int)ok, result ? result : "");
  enqueueRedrawFromCallback(cookie, &showPeerDetail, "peer-detail redraw");
}

static void onPeersRedrawDone(bool ok,
                              const char* result,
                              const G2CmdCookie& cookie,
                              void* /*userData*/) {
  DEBUG_G2F("[G2-ESP-NOW-APP] peers redraw cmd done: ok=%d result='%s'",
            (int)ok, result ? result : "");
  enqueueRedrawFromCallback(cookie, &showPeersMenu, "peers redraw");
}

static void onBroadcastRedrawDone(bool ok,
                                  const char* result,
                                  const G2CmdCookie& cookie,
                                  void* /*userData*/) {
  DEBUG_G2F("[G2-ESP-NOW-APP] broadcast redraw cmd done: ok=%d result='%s'",
            (int)ok, result ? result : "");
  enqueueRedrawFromCallback(cookie, &showBroadcastMenu, "broadcast redraw");
}

// -----------------------------------------------------------------------------
// Action helpers — send / broadcast / unpair via cmd_exec
// -----------------------------------------------------------------------------

#if ENABLE_ESPNOW
// Submit `espnowsend <MAC> <text>` for the currently-selected peer. On submit
// failure logs and re-renders Peer Detail inline so the user gets feedback.
static bool submitSendToSelectedPeer(const char* text) {
  if (gSelectedPeer < 0 || !gEspNow ||
      gSelectedPeer >= gEspNow->deviceCount) return false;
  if (!text || !text[0]) return false;
  const EspNowDevice& d = gEspNow->devices[gSelectedPeer];
  char line[16 + 18 + 200];   // "espnowsend " + MAC + space + text
  snprintf(line, sizeof(line),
           "espnowsend %02X:%02X:%02X:%02X:%02X:%02X %s",
           d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
           text);
  G2CmdCookie cookie = buildCookie();
  if (!g2SubmitHijackCommand(line, cookie, onPeerDetailRedrawDone, nullptr)) {
    DEBUG_G2F("[G2-ESP-NOW-APP] send submit FAILED — '%s'", line);
    showPeerDetail();
    return false;
  }
  BROADCAST_PRINTF("[G2-ESP-NOW-APP] send → %s: '%s'",
                   d.friendlyName.length() > 0 ? d.friendlyName.c_str() :
                   d.name.length()         > 0 ? d.name.c_str()         : "(peer)",
                   text);
  return true;
}

// Submit `espnowbroadcast <text>` and re-render the broadcast submenu.
static bool submitBroadcast(const char* text) {
  if (!text || !text[0]) return false;
  char line[16 + 200];   // "espnowbroadcast " + text
  snprintf(line, sizeof(line), "espnowbroadcast %s", text);
  G2CmdCookie cookie = buildCookie();
  if (!g2SubmitHijackCommand(line, cookie, onBroadcastRedrawDone, nullptr)) {
    DEBUG_G2F("[G2-ESP-NOW-APP] broadcast submit FAILED — '%s'", line);
    showBroadcastMenu();
    return false;
  }
  BROADCAST_PRINTF("[G2-ESP-NOW-APP] broadcast: '%s'", text);
  return true;
}

// Submit `espnowunpair <MAC>` for the currently-selected peer. On success the
// completion bounces back to the peers list — the detail view's target is
// gone after the unpair lands.
static bool submitForgetSelectedPeer() {
  if (gSelectedPeer < 0 || !gEspNow ||
      gSelectedPeer >= gEspNow->deviceCount) return false;
  const EspNowDevice& d = gEspNow->devices[gSelectedPeer];
  char line[16 + 18];   // "espnowunpair " + MAC
  snprintf(line, sizeof(line),
           "espnowunpair %02X:%02X:%02X:%02X:%02X:%02X",
           d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
  G2CmdCookie cookie = buildCookie();
  // After unpair the selected index will point at a different (or removed)
  // peer, so the redraw target is the peers list, not peer-detail.
  cookie.targetNetSub = (uint8_t)ESPN_APP_SUB_PEERS;
  gSelectedPeer = -1;
  if (!g2SubmitHijackCommand(line, cookie, onPeersRedrawDone, nullptr)) {
    DEBUG_G2F("[G2-ESP-NOW-APP] forget submit FAILED — '%s'", line);
    showPeersMenu();
    return false;
  }
  BROADCAST_PRINTF("[G2-ESP-NOW-APP] forget peer: %s", line + 13);
  return true;
}

// Toggle ESP-NOW on/off via the existing CLI commands (same path the Network
// page uses for the ESP-NOW state line).
static void submitToggleEspNow(bool running) {
  const char* line = running ? "closeespnow" : "openespnow";
  G2CmdCookie cookie = buildCookie();
  if (!g2SubmitHijackCommand(line, cookie, onMainRedrawDone, nullptr)) {
    DEBUG_G2F("[G2-ESP-NOW-APP] toggle submit FAILED — '%s'", line);
    showMainMenu();
    return;
  }
  BROADCAST_PRINTF("[G2-ESP-NOW-APP] toggle: %s", line);
}
#endif  // ENABLE_ESPNOW

// -----------------------------------------------------------------------------
// TextEntry commit/cancel — typed message paths
// -----------------------------------------------------------------------------

#if ENABLE_ESPNOW
static void sendTypedToPeerCommit(const char* text) {
  if (text && text[0]) {
    submitSendToSelectedPeer(text);
  } else {
    showPeerDetail();   // empty input → just bounce back
  }
}
static void sendTypedToPeerCancel() { showPeerDetail(); }

static void broadcastTypedCommit(const char* text) {
  if (text && text[0]) {
    submitBroadcast(text);
  } else {
    showBroadcastMenu();
  }
}
static void broadcastTypedCancel() { showBroadcastMenu(); }
#endif

// -----------------------------------------------------------------------------
// Tap dispatchers — one per submenu
// -----------------------------------------------------------------------------

static void handleMainTap(uint32_t idx) {
  // 0 — back to hijack root.
  // 1 — state line: tap behaviour depends on phase.
  // 2 — peers, 3 — broadcast, 4 — stats.
  if (idx == 0) {
    // Same pattern as G2_Page_Files: pages with their own showMenu render
    // the back row themselves, so we explicitly hand the user back to the
    // main hijack list. setSub resets our sub-mode for next entry.
    gSelectedPeer = -1;
    setSub(ESPN_APP_SUB_MAIN);
    g2SetHijackPage(G2_HIJACK_PAGE_MAIN);
    extern void g2RedrawHijackMainMenu();
    g2RedrawHijackMainMenu();
    return;
  }
  if (idx == 1) {
    EspNowAppPhase p = currentPhase();
    if (p == EspNowAppPhase::RadioOff) {
      // Wi-Fi radio itself is down — ESP-NOW can't start until it's up.
      // Send the user to the Network top-level so they can navigate
      // into WiFi and bring the radio up there.
      DEBUG_G2F("[G2-ESP-NOW-APP] state-tap: radio off, jumping to Network");
      g2ShowNetworkMenu();
      return;
    }
#if ENABLE_ESPNOW
    bool running = (p == EspNowAppPhase::On);
    submitToggleEspNow(running);
#endif
    return;
  }
  if (idx == 2) { showPeersMenu();      return; }
  if (idx == 3) { showInboxMenu();      return; }
  if (idx == 4) { showBroadcastMenu();  return; }
  if (idx == 5) { showStatsMenu();      return; }
  if (idx == 6) { showBondSensorsMenu(); return; }
  DEBUG_G2F("[G2-ESP-NOW-APP] main: unknown idx=%u", (unsigned)idx);
}

static void handlePeersTap(uint32_t idx) {
  if (idx == 0) { showMainMenu(); return; }
#if ENABLE_ESPNOW
  // Rows 1..N map directly to gEspNow->devices[0..N-1].
  int peerIdx = (int)idx - 1;
  if (!gEspNow || !gEspNow->initialized ||
      peerIdx < 0 || peerIdx >= gEspNow->deviceCount) {
    DEBUG_G2F("[G2-ESP-NOW-APP] peers: invalid idx=%u (count=%d)",
              (unsigned)idx,
              (gEspNow && gEspNow->initialized) ? gEspNow->deviceCount : -1);
    return;
  }
  gSelectedPeer = peerIdx;
  showPeerDetail();
#else
  (void)idx;
#endif
}

static void handlePeerDetailTap(uint32_t idx) {
  if (idx == 0) {
    gSelectedPeer = -1;
    showPeersMenu();
    return;
  }
  // 1, 2 are info rows — no action.
  if (idx == 1 || idx == 2) {
    DEBUG_G2F("[G2-ESP-NOW-APP] peer-detail: info row %u", (unsigned)idx);
    return;
  }

#if ENABLE_ESPNOW
  if (bailIfNotReady("peer-detail tap")) { showPeerDetail(); return; }
  if (gSelectedPeer < 0 || gSelectedPeer >= gEspNow->deviceCount) {
    showPeersMenu();
    return;
  }
  const EspNowDevice& d = gEspNow->devices[gSelectedPeer];

  switch (idx) {
    case 3:    // Messages (drill into per-peer inbox slice)
      showPeerInboxMenu();
      return;
    case 4: {  // Ping
      // Clear any prior result first so the row immediately reads
      // "Ping (pending...)" on the redraw below.
      espnowAppPingClear();
      bool ok = espnowAppPingStart(d.mac);
      DEBUG_G2F("[G2-ESP-NOW-APP] ping start ok=%d", (int)ok);
      showPeerDetail();
      return;
    }
    case 5:    // Send: Hi
      submitSendToSelectedPeer("Hi");
      return;
    case 6:    // Send: OK
      submitSendToSelectedPeer("OK");
      return;
    case 7: {  // Send… (typed)
      TextEntryConfig cfg = {};
      cfg.prompt   = "Send to peer";
      cfg.initial  = "";
      cfg.maxLen   = 32;  // g2BeginTextEntry rejects maxLen > 32
      cfg.onCommit = sendTypedToPeerCommit;
      cfg.onCancel = sendTypedToPeerCancel;
      if (!g2BeginTextEntry(cfg)) {
        DEBUG_G2F("[G2-ESP-NOW-APP] text-entry start FAILED");
      }
      return;
    }
    case 8:    // Forget
      submitForgetSelectedPeer();
      return;
    default:
      DEBUG_G2F("[G2-ESP-NOW-APP] peer-detail: unknown idx=%u", (unsigned)idx);
      return;
  }
#else
  (void)idx;
#endif
}

static void handleBroadcastTap(uint32_t idx) {
  if (idx == 0) { showMainMenu(); return; }
#if ENABLE_ESPNOW
  if (bailIfNotReady("broadcast tap")) { showBroadcastMenu(); return; }
  switch (idx) {
    case 1: submitBroadcast("Here"); return;
    case 2: submitBroadcast("OK");   return;
    case 3: submitBroadcast("Help"); return;
    case 4: {
      TextEntryConfig cfg = {};
      cfg.prompt   = "Broadcast";
      cfg.initial  = "";
      cfg.maxLen   = 32;  // g2BeginTextEntry rejects maxLen > 32
      cfg.onCommit = broadcastTypedCommit;
      cfg.onCancel = broadcastTypedCancel;
      if (!g2BeginTextEntry(cfg)) {
        DEBUG_G2F("[G2-ESP-NOW-APP] broadcast text-entry start FAILED");
      }
      return;
    }
    default:
      DEBUG_G2F("[G2-ESP-NOW-APP] broadcast: unknown idx=%u", (unsigned)idx);
      return;
  }
#else
  (void)idx;
#endif
}

static void handleStatsTap(uint32_t idx) {
  // Info-only — every row returns to main.
  (void)idx;
  showMainMenu();
}

// Bonded Device sensor list — row 0 back to main; a sensor row drills into its
// detail. Re-collects the same valid-entry order the renderer used so row N
// resolves to the same cache entry (degrades gracefully if it expired since).
static void handleBondSensorsTap(uint32_t idx) {
  if (idx == 0) { showMainMenu(); return; }
#if ENABLE_ESPNOW
  int sel = (int)idx - 1;
  int list[MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE];
  size_t got = collectBondSensorIdx(list, sizeof(list) / sizeof(list[0]));
  if (sel < 0 || (size_t)sel >= got) {
    DEBUG_G2F("[G2-ESP-NOW-APP] bond-sensors: idx=%u out of range (got=%u)",
              (unsigned)idx, (unsigned)got);
    return;
  }
  const RemoteSensorData& e = gRemoteSensorCache[list[sel]];
  memcpy(gSelSensorMac, e.deviceMac, 6);
  gSelSensorType = e.sensorType;
  showBondDetailMenu();
#else
  (void)idx;
#endif
}

// Bonded sensor detail — field rows are info-only; row 0 returns to the list.
static void handleBondDetailTap(uint32_t idx) {
  if (idx == 0) { showBondSensorsMenu(); return; }
  DEBUG_G2F("[G2-ESP-NOW-APP] bond-detail: info row %u", (unsigned)idx);
}

// Merged inbox tap dispatch.
// Row 0 returns to Main. Tapping a message row navigates to that sender's
// Peer Detail — from there the user can reply via Send… or open the per-peer
// history. Re-resolves the sender's MAC against gEspNow->devices[] at tap
// time so a stale inbox row (peer unpaired since the row was rendered)
// degrades gracefully to "no such peer" instead of crashing.
static void handleInboxTap(uint32_t idx) {
  if (idx == 0) { showMainMenu(); return; }

#if ENABLE_ESPNOW
  // The inbox now renders as a TextContainer chat view, so taps route through
  // the text-view exit path (gTextViewExitFn -> exitInboxToMain), not here.
  // This list-style dispatcher is only a defensive fallback (e.g. a stray tap
  // before the text page primes) — just go back to Main.
  (void)idx;
  showMainMenu();
#else
  (void)idx;
#endif
}

// Per-peer inbox tap — info-only beyond the back row. Tapping a row returns
// to the peer detail page (consistent navigation: back goes up one level).
static void handlePeerInboxTap(uint32_t idx) {
  if (idx == 0) { showPeerDetail(); return; }
  // Phase 2: rows are info-only. A future phase could open a per-message
  // detail overlay (full text + reply) — not needed for the walkie-talkie
  // use case.
  DEBUG_G2F("[G2-ESP-NOW-APP] peer-inbox: tap idx=%u (info-only)",
            (unsigned)idx);
}

// -----------------------------------------------------------------------------
// Public tap entry
// -----------------------------------------------------------------------------

void g2ESPNowAppHandleTap(uint32_t idx) {
  switch (gSub) {
    case ESPN_APP_SUB_MAIN:        handleMainTap(idx);       break;
    case ESPN_APP_SUB_PEERS:       handlePeersTap(idx);      break;
    case ESPN_APP_SUB_PEER_DETAIL: handlePeerDetailTap(idx); break;
    case ESPN_APP_SUB_BCAST:       handleBroadcastTap(idx);  break;
    case ESPN_APP_SUB_STATS:       handleStatsTap(idx);      break;
    case ESPN_APP_SUB_INBOX:       handleInboxTap(idx);      break;
    case ESPN_APP_SUB_PEER_INBOX:  handlePeerInboxTap(idx);  break;
    case ESPN_APP_SUB_BOND_SENSORS: handleBondSensorsTap(idx); break;
    case ESPN_APP_SUB_BOND_DETAIL:  handleBondDetailTap(idx);  break;
  }
}

// -----------------------------------------------------------------------------
// Push-kick — called from espnow_task RX drain on new TEXT arrival
// -----------------------------------------------------------------------------
// Drops a Redraw onto the lens applier queue when the user is currently
// looking at a view that displays this message. Other sub-modes don't show
// message bodies (Main shows only a count, Peers/Peer Detail don't show
// history bodies, Stats doesn't either), so a redraw mid-ping or mid-typing
// would only churn the lens for no visible benefit — we explicitly filter
// to the two inbox views.
//
// The Main view's "Inbox (M)" counter also moves when a message lands; we
// could repaint Main here too, but the count update is cosmetic and the
// next tap-driven render will pick it up. Keeping the kick tight to the
// pages that actually change visually keeps redraw bandwidth honest.

void g2ESPNowAppOnRxText(const uint8_t* senderMac) {
  if (g2GetHijackPage() != G2_HIJACK_PAGE_ESPNOW_APP) return;

  void (*renderFn)() = nullptr;
  if (gSub == ESPN_APP_SUB_INBOX) {
    renderFn = &showInboxMenu;
  } else if (gSub == ESPN_APP_SUB_PEER_INBOX) {
#if ENABLE_ESPNOW
    // Only repaint if the message is from the peer whose inbox we're viewing.
    if (!gEspNow || !gEspNow->initialized ||
        gSelectedPeer < 0 || gSelectedPeer >= gEspNow->deviceCount) return;
    if (!senderMac ||
        memcmp(senderMac, gEspNow->devices[gSelectedPeer].mac, 6) != 0) return;
    renderFn = &showPeerInboxMenu;
#else
    return;
#endif
  } else {
    return;  // not on a view that displays message bodies
  }

  RedrawSpec* spec = new (std::nothrow) RedrawSpec{};
  if (!spec) return;
  spec->render = renderFn;

  LensUiJob* job = new (std::nothrow) LensUiJob{};
  if (!job) { delete spec; return; }
  job->kind           = LensJobKind::Redraw;
  job->submitMenuGen  = g2CurrentMenuGen();
  job->cmdSeq         = 0;
  job->targetPage     = G2_HIJACK_PAGE_ESPNOW_APP;
  job->targetNetSub   = (uint8_t)gSub;
  job->payload.redraw = spec;

  if (!g2EnqueueLensJob(job)) {
    DEBUG_G2F("[G2-ESP-NOW-APP] rx push-kick: enqueue FAILED");
    delete spec;
    delete job;
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
