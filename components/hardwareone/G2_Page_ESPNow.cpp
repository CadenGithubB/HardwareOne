// =============================================================================
// G2 glasses — "ESPNOW App" page implementation
// =============================================================================
// See header for the contract. This page surfaces ESPNOW actions on the
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

// Forward decls — handlers reference renderers across the file.
static void showMainMenu();
static void showPeersMenu();
static void showPeerDetail();
static void showBroadcastMenu();
static void showStatsMenu();
static void showInboxMenu();
static void showPeerInboxMenu();

// -----------------------------------------------------------------------------
// Tri-state status helpers
// -----------------------------------------------------------------------------
// Three independent states the user cares about:
//   "Radio Off" — WiFi driver itself is down (esp_wifi_set_mode hasn't been
//                 called or was set to WIFI_OFF). ESPNOW cannot operate.
//   "OFF"       — WiFi up, ESPNOW subsystem not initialized. Tap to start.
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
    case EspNowAppPhase::RadioOff: return "ESPNOW: Radio Off";
    case EspNowAppPhase::Off:      return "ESPNOW: OFF";
    case EspNowAppPhase::On:       return "ESPNOW: ON";
  }
  return "ESPNOW: ?";
}

// Convenience guard for action rows: when ESPNOW isn't ON, we no-op + log.
// Returns true if the caller should bail.
static bool bailIfNotReady(const char* what) {
  if (currentPhase() != EspNowAppPhase::On) {
    DEBUG_G2F("[G2-ESPNOW-APP] %s: not ready (phase=%d)",
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

// Compact relative-time prefix ("5s" / "12m" / "3h" / "2d"). The lens row is
// ~32 chars wide so we keep this short and unit-suffixed rather than spelling
// out "ago". `nowMs` is passed in so a whole-render walk uses one consistent
// reference point (avoids the 0..tick-length jitter from re-reading millis()
// inside the loop). Writes into `out`; max 5 chars including NUL.
static void formatAgeShort(unsigned long timestampMs, unsigned long nowMs,
                           char* out, size_t cap) {
  if (cap == 0) return;
  // Defensive: protect against clock-stamped timestamps that look "in the
  // future" (clock changes, wrap-around during the ~50-day uint32 millis
  // rollover). Treat those as "now" instead of negative.
  unsigned long age = (nowMs >= timestampMs) ? (nowMs - timestampMs) : 0;
  if      (age <      1000) snprintf(out, cap, "now");
  else if (age <     60000) snprintf(out, cap, "%lus", age / 1000);
  else if (age <   3600000) snprintf(out, cap, "%lum", age / 60000);
  else if (age <  86400000) snprintf(out, cap, "%luh", age / 3600000);
  else                      snprintf(out, cap, "%lud", age / 86400000);
}

// Format one inbox row into `out`. Layout: "[<age>] <name>: <msg>" — truncated
// by snprintf to whatever fits. `name` falls back to MAC tail if empty.
static void formatInboxRow(char* out, size_t cap,
                           const ReceivedTextMessage& msg,
                           unsigned long nowMs,
                           bool showSender) {
  char age[8] = {0};
  formatAgeShort(msg.timestamp, nowMs, age, sizeof(age));
  if (showSender) {
    // MAC tail fallback when senderName is empty.
    char tail[16];
    if (msg.senderName[0]) {
      snprintf(tail, sizeof(tail), "%s", msg.senderName);
    } else {
      snprintf(tail, sizeof(tail), "%02X%02X%02X",
               msg.senderMac[3], msg.senderMac[4], msg.senderMac[5]);
    }
    snprintf(out, cap, "[%s] %s: %s", age, tail, msg.message);
  } else {
    snprintf(out, cap, "[%s] %s", age, msg.message);
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
    DEBUG_G2F("[G2-ESPNOW-APP] %s: RedrawSpec alloc failed", tag);
    return;
  }
  spec->render = renderFn;

  LensUiJob* job = new (std::nothrow) LensUiJob{};
  if (!job) {
    DEBUG_G2F("[G2-ESPNOW-APP] %s: LensUiJob alloc failed", tag);
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
    DEBUG_G2F("[G2-ESPNOW-APP] %s: lens job enqueue FAILED", tag);
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
  s += "ESPNOW App\n";
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
    stateLine,        // 1 — tap = toggle ESPNOW / jump to WiFi
    peersLine,        // 2
    inboxLine,        // 3 — merged chronological view of all peers' history
    "Broadcast >>",   // 4
    "Stats >>",       // 5
  };
  g2ShowListPage(items, sizeof(items) / sizeof(items[0]));
  DEBUG_G2F("[G2-ESPNOW-APP] main menu shown (phase=%d, peers=%d, inbox=%d)",
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
  DEBUG_G2F("[G2-ESPNOW-APP] peers menu shown (%u rows)", (unsigned)(n - 1));
#else
  static const char* na[] = { "<- Back", "(ESPNOW not compiled)" };
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
  DEBUG_G2F("[G2-ESPNOW-APP] peer detail shown (idx=%d, name='%s')",
            gSelectedPeer, nm);
#else
  static const char* na[] = { "<- Back", "(ESPNOW not compiled)" };
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
  DEBUG_G2F("[G2-ESPNOW-APP] broadcast menu shown");
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
  static const char* na[] = { "<- Back", "(ESPNOW not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// -----------------------------------------------------------------------------
// Inbox views — merged chronological + per-peer slice
// -----------------------------------------------------------------------------
// Both views read directly from gEspNow->peerMessageHistories via the public
// getAllMessages / getPeerMessages API — zero new storage, just a chronological
// rendering over data that espnow_task is already populating in its textQueue
// drain (System_ESPNow.cpp, step 11 of processMeshHeartbeats).

// Cap on rows the list widget can show without scrolling becoming awkward on
// the 4-bpp lens. Newest-first; older entries are visible by tapping each
// message's sender and using "Send..." (or just scrolling history elsewhere).
#define ESPN_APP_INBOX_DISPLAY_MAX 12

static void showInboxMenu() {
  setSub(ESPN_APP_SUB_INBOX);

#if ENABLE_ESPNOW
  // 1 back row + up to N message rows. Static buffers so we don't churn the
  // stack on every render (each ReceivedTextMessage is ~300 B; pulling 12
  // onto the stack is ~3.5 KB which is tight on BTC).
  static ReceivedTextMessage msgs[ESPN_APP_INBOX_DISPLAY_MAX];
  static char rows[1 + ESPN_APP_INBOX_DISPLAY_MAX][72];
  const char* ptrs[1 + ESPN_APP_INBOX_DISPLAY_MAX];
  strcpy(rows[0], "<- Main Menu");
  ptrs[0] = rows[0];
  size_t n = 1;

  // getAllMessages returns up to maxN, sorted newest-first.
  int got = getAllMessages(msgs, ESPN_APP_INBOX_DISPLAY_MAX, /*sinceSeq=*/0);
  if (got > 0) {
    unsigned long now = (unsigned long)millis();
    for (int i = 0; i < got && (size_t)i < ESPN_APP_INBOX_DISPLAY_MAX; i++) {
      formatInboxRow(rows[1 + i], sizeof(rows[1 + i]), msgs[i], now,
                     /*showSender=*/true);
      ptrs[n++] = rows[1 + i];
    }
  }

  if (n == 1) {
    static const char* empty[] = { "<- Main Menu", "(no messages yet)" };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2-ESPNOW-APP] inbox shown (%d msgs)", got);
#else
  static const char* na[] = { "<- Main Menu", "(ESPNOW not compiled)" };
  g2ShowListPage(na, 2);
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

  static ReceivedTextMessage msgs[ESPN_APP_INBOX_DISPLAY_MAX];
  static char rows[1 + ESPN_APP_INBOX_DISPLAY_MAX][72];
  const char* ptrs[1 + ESPN_APP_INBOX_DISPLAY_MAX];
  strcpy(rows[0], "<- Peer");
  ptrs[0] = rows[0];
  size_t n = 1;

  int got = getPeerMessages(d.mac, msgs, ESPN_APP_INBOX_DISPLAY_MAX,
                            /*sinceSeq=*/0);
  if (got > 0) {
    unsigned long now = (unsigned long)millis();
    for (int i = 0; i < got && (size_t)i < ESPN_APP_INBOX_DISPLAY_MAX; i++) {
      // showSender=false — the user knows whose inbox they're looking at;
      // dropping the prefix gives the message text more room.
      formatInboxRow(rows[1 + i], sizeof(rows[1 + i]), msgs[i], now,
                     /*showSender=*/false);
      ptrs[n++] = rows[1 + i];
    }
  }

  if (n == 1) {
    static const char* empty[] = { "<- Peer", "(no messages from this peer)" };
    g2ShowListPage(empty, 2);
  } else {
    g2ShowListPage(ptrs, n);
  }
  DEBUG_G2F("[G2-ESPNOW-APP] peer-inbox shown (peer=%d, %d msgs)",
            gSelectedPeer, got);
#else
  static const char* na[] = { "<- Peer", "(ESPNOW not compiled)" };
  g2ShowListPage(na, 2);
#endif
}

// -----------------------------------------------------------------------------
// cmd_exec completion callbacks
// -----------------------------------------------------------------------------

static void onMainRedrawDone(bool ok,
                             const char* result,
                             const G2CmdCookie& cookie,
                             void* /*userData*/) {
  DEBUG_G2F("[G2-ESPNOW-APP] main redraw cmd done: ok=%d seq=%llu menuGen=%u result='%s'",
            (int)ok, (unsigned long long)cookie.seq,
            (unsigned)cookie.menuGen, result ? result : "");
  enqueueRedrawFromCallback(cookie, &showMainMenu, "main redraw");
}

static void onPeerDetailRedrawDone(bool ok,
                                   const char* result,
                                   const G2CmdCookie& cookie,
                                   void* /*userData*/) {
  DEBUG_G2F("[G2-ESPNOW-APP] peer-detail redraw cmd done: ok=%d result='%s'",
            (int)ok, result ? result : "");
  enqueueRedrawFromCallback(cookie, &showPeerDetail, "peer-detail redraw");
}

static void onPeersRedrawDone(bool ok,
                              const char* result,
                              const G2CmdCookie& cookie,
                              void* /*userData*/) {
  DEBUG_G2F("[G2-ESPNOW-APP] peers redraw cmd done: ok=%d result='%s'",
            (int)ok, result ? result : "");
  enqueueRedrawFromCallback(cookie, &showPeersMenu, "peers redraw");
}

static void onBroadcastRedrawDone(bool ok,
                                  const char* result,
                                  const G2CmdCookie& cookie,
                                  void* /*userData*/) {
  DEBUG_G2F("[G2-ESPNOW-APP] broadcast redraw cmd done: ok=%d result='%s'",
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
    DEBUG_G2F("[G2-ESPNOW-APP] send submit FAILED — '%s'", line);
    showPeerDetail();
    return false;
  }
  BROADCAST_PRINTF("[G2-ESPNOW-APP] send → %s: '%s'",
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
    DEBUG_G2F("[G2-ESPNOW-APP] broadcast submit FAILED — '%s'", line);
    showBroadcastMenu();
    return false;
  }
  BROADCAST_PRINTF("[G2-ESPNOW-APP] broadcast: '%s'", text);
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
    DEBUG_G2F("[G2-ESPNOW-APP] forget submit FAILED — '%s'", line);
    showPeersMenu();
    return false;
  }
  BROADCAST_PRINTF("[G2-ESPNOW-APP] forget peer: %s", line + 13);
  return true;
}

// Toggle ESPNOW on/off via the existing CLI commands (same path the Network
// page uses for the ESP-NOW state line).
static void submitToggleEspNow(bool running) {
  const char* line = running ? "closeespnow" : "openespnow";
  G2CmdCookie cookie = buildCookie();
  if (!g2SubmitHijackCommand(line, cookie, onMainRedrawDone, nullptr)) {
    DEBUG_G2F("[G2-ESPNOW-APP] toggle submit FAILED — '%s'", line);
    showMainMenu();
    return;
  }
  BROADCAST_PRINTF("[G2-ESPNOW-APP] toggle: %s", line);
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
      // Wi-Fi radio itself is down — ESPNOW can't start until it's up.
      // Send the user to the Network top-level so they can navigate
      // into WiFi and bring the radio up there.
      DEBUG_G2F("[G2-ESPNOW-APP] state-tap: radio off, jumping to Network");
      g2ShowNetworkMenu();
      return;
    }
#if ENABLE_ESPNOW
    bool running = (p == EspNowAppPhase::On);
    submitToggleEspNow(running);
#endif
    return;
  }
  if (idx == 2) { showPeersMenu();     return; }
  if (idx == 3) { showInboxMenu();     return; }
  if (idx == 4) { showBroadcastMenu(); return; }
  if (idx == 5) { showStatsMenu();     return; }
  DEBUG_G2F("[G2-ESPNOW-APP] main: unknown idx=%u", (unsigned)idx);
}

static void handlePeersTap(uint32_t idx) {
  if (idx == 0) { showMainMenu(); return; }
#if ENABLE_ESPNOW
  // Rows 1..N map directly to gEspNow->devices[0..N-1].
  int peerIdx = (int)idx - 1;
  if (!gEspNow || !gEspNow->initialized ||
      peerIdx < 0 || peerIdx >= gEspNow->deviceCount) {
    DEBUG_G2F("[G2-ESPNOW-APP] peers: invalid idx=%u (count=%d)",
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
    DEBUG_G2F("[G2-ESPNOW-APP] peer-detail: info row %u", (unsigned)idx);
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
      DEBUG_G2F("[G2-ESPNOW-APP] ping start ok=%d", (int)ok);
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
      cfg.maxLen   = 80;
      cfg.onCommit = sendTypedToPeerCommit;
      cfg.onCancel = sendTypedToPeerCancel;
      if (!g2BeginTextEntry(cfg)) {
        DEBUG_G2F("[G2-ESPNOW-APP] text-entry start FAILED");
      }
      return;
    }
    case 8:    // Forget
      submitForgetSelectedPeer();
      return;
    default:
      DEBUG_G2F("[G2-ESPNOW-APP] peer-detail: unknown idx=%u", (unsigned)idx);
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
      cfg.maxLen   = 80;
      cfg.onCommit = broadcastTypedCommit;
      cfg.onCancel = broadcastTypedCancel;
      if (!g2BeginTextEntry(cfg)) {
        DEBUG_G2F("[G2-ESPNOW-APP] broadcast text-entry start FAILED");
      }
      return;
    }
    default:
      DEBUG_G2F("[G2-ESPNOW-APP] broadcast: unknown idx=%u", (unsigned)idx);
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

// Merged inbox tap dispatch.
// Row 0 returns to Main. Tapping a message row navigates to that sender's
// Peer Detail — from there the user can reply via Send… or open the per-peer
// history. Re-resolves the sender's MAC against gEspNow->devices[] at tap
// time so a stale inbox row (peer unpaired since the row was rendered)
// degrades gracefully to "no such peer" instead of crashing.
static void handleInboxTap(uint32_t idx) {
  if (idx == 0) { showMainMenu(); return; }

#if ENABLE_ESPNOW
  // Re-fetch the same window we rendered. The list-row indices line up 1:1
  // with msgs[] positions because both renderer and dispatcher source from
  // getAllMessages() with identical args.
  ReceivedTextMessage msgs[ESPN_APP_INBOX_DISPLAY_MAX];
  int got = getAllMessages(msgs, ESPN_APP_INBOX_DISPLAY_MAX, /*sinceSeq=*/0);
  int slot = (int)idx - 1;
  if (slot < 0 || slot >= got) {
    DEBUG_G2F("[G2-ESPNOW-APP] inbox: tap idx=%u out of range (got=%d)",
              (unsigned)idx, got);
    return;
  }
  // Look up the sender's slot in gEspNow->devices[] so Peer Detail has a
  // valid gSelectedPeer to render against.
  if (!gEspNow || !gEspNow->initialized) { showMainMenu(); return; }
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, msgs[slot].senderMac, 6) == 0) {
      gSelectedPeer = i;
      showPeerDetail();
      return;
    }
  }
  DEBUG_G2F("[G2-ESPNOW-APP] inbox: sender no longer paired (slot=%d)", slot);
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
  DEBUG_G2F("[G2-ESPNOW-APP] peer-inbox: tap idx=%u (info-only)",
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
    DEBUG_G2F("[G2-ESPNOW-APP] rx push-kick: enqueue FAILED");
    delete spec;
    delete job;
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
