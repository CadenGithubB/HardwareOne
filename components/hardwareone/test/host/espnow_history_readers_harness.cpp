// Host harness for the ESP-NOW history readers: the production web handler
// (handleEspNowMessages, WebPage_ESPNow.cpp), the OLED conversation
// (oledEspNowRefreshMessages, OLED_ESPNow.cpp) and the G2 chat rows (chatWho,
// chatLogicalLine, chatEstBytes, G2_Page_ESPNow.cpp), compiled against the
// history types and declarations of System_ESPNow.h and a fake store.
// test_espnow_history_readers.py fills the INSERT_ markers. Cases:
//   web     the JSON body (printed for the Python side, which checks keys and
//           order); names come from the resolver once per peer and kind
//   oled    newest rows, refused (evicted) refs skipped, names, parts labels,
//           sent rows' msgId and sendState, maxN == ESPNOW_HISTORY_VIEW_MAX
//   g2      labels, reassembly by the record's peer MAC, size estimate
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "System_ESPNow_HistoryCore.h"
#include "System_ESPNow_HistoryNames.h"
#include "System_JsonText.h"
#include "System_MacText.h"

#define EXT_RAM_BSS_ATTR
#define ENABLE_ESPNOW 1

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++gChecks;                                                                      \
    if (!(cond)) {                                                                  \
      ++gFailures;                                                                  \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                               \
  } while (0)

template <class T> static T max(T a, T b) { return a > b ? a : b; }

// ---- production history API (System_ESPNow.h) ---------------------------------
// INSERT_API

// ---- ESP-NOW state double ------------------------------------------------------
struct EspNowStateDouble { bool initialized = true; };
static EspNowStateDouble gEspNowStorage;
static EspNowStateDouble* gEspNow = &gEspNowStorage;

// ---- fake store ------------------------------------------------------------------
// Records as the store would copy them out; a ref resolves by seq only (an
// evicted seq is refused, whatever the ref's offset says).
static std::vector<ReceivedTextMessage> gStore;
static std::vector<CollapsedMsgRef> gRefs;      // what the collapse calls return
static int gLastMaxN = -1;
static int gCopyCalls = 0, gCopyRefused = 0;
static uint8_t gLastReasmMac[6];
static uint32_t gLastReasmReqId = 0;
static bool gLastReasmSent = false;
static bool gInStore = false;                   // a store call is running

static void setText(ReceivedTextMessage& m, const char* text) {
  std::memset(m.message, 0, sizeof m.message);
  std::strncpy(m.message, text, sizeof m.message - 1);
  m.length = static_cast<uint8_t>(std::strlen(m.message));
}

static ReceivedTextMessage makeRecord(const uint8_t mac[6], uint32_t seq, const char* text, bool sent,
                                      bool unattributed = false, uint32_t reqId = 0, uint8_t piece = 1,
                                      uint8_t total = 1, uint8_t sendState = 0) {
  ReceivedTextMessage m;
  std::memset(&m, 0xA5, sizeof m);              // every field below must be set
  std::memcpy(m.peerMac, mac, 6);
  m.piece = piece;
  m.pieceTotal = total;
  m.seqNum = seq;
  m.reqId = reqId;
  m.timestamp = 1000 + seq;
  m.msgType = static_cast<LogMessageType>(0);
  m.encrypted = !unattributed;
  m.isSent = sent;
  m.unattributed = unattributed;
  m.sendState = sendState;
  setText(m, text);
  return m;
}

static CollapsedMsgRef refFor(const ReceivedTextMessage& m, uint8_t present = 1) {
  CollapsedMsgRef r;
  std::memset(&r, 0, sizeof r);
  r.seqNum = m.seqNum;
  r.reqId = m.reqId;
  r.timestamp = m.timestamp;
  r.offset = 0xFFFFFFFCu;                       // a hint the readers must never use
  r.slot = 0;
  r.headPiece = m.piece;
  r.partsPresent = present;
  r.partsTotal = m.pieceTotal;
  r.isSent = m.isSent;
  return r;
}

static void evict(uint32_t seq) {
  for (size_t i = 0; i < gStore.size(); ++i)
    if (gStore[i].seqNum == seq) { gStore.erase(gStore.begin() + static_cast<long>(i)); return; }
}

static int copyRefs(CollapsedMsgRef* out, int maxN) {
  gInStore = true;
  gLastMaxN = maxN;
  int n = static_cast<int>(gRefs.size());
  if (maxN > ESPNOW_HISTORY_VIEW_MAX) maxN = ESPNOW_HISTORY_VIEW_MAX;
  int first = n > maxN ? n - maxN : 0;          // newest maxN kept
  int k = 0;
  for (int i = first; i < n; ++i) out[k++] = gRefs[static_cast<size_t>(i)];
  gInStore = false;
  return k;
}

int espnowGetConversation(uint8_t* peerMac, CollapsedMsgRef* out, int maxN) {
  (void)peerMac;
  return copyRefs(out, maxN);
}

int espnowCollapsedAllMessages(CollapsedMsgRef* out, int maxN) { return copyRefs(out, maxN); }

bool espnowCopyCollapsedHead(const CollapsedMsgRef& ref, ReceivedTextMessage* out) {
  ++gCopyCalls;
  for (const ReceivedTextMessage& m : gStore) {
    if (m.seqNum == ref.seqNum && m.isSent == ref.isSent) { *out = m; return true; }
  }
  ++gCopyRefused;
  return false;
}

int espnowReassembleByReqId(const uint8_t* peerMac, uint32_t reqId, bool isSent, char* out, size_t cap,
                            bool* complete) {
  std::memcpy(gLastReasmMac, peerMac, 6);
  gLastReasmReqId = reqId;
  gLastReasmSent = isSent;
  if (!out || cap == 0) return 0;
  std::string joined;
  int found = 0, total = 0;
  for (int p = 1; p <= 255; ++p) {
    for (const ReceivedTextMessage& m : gStore) {
      if (m.reqId == reqId && m.isSent == isSent && std::memcmp(m.peerMac, peerMac, 6) == 0 && m.piece == p) {
        joined += m.message;
        ++found;
        total = m.pieceTotal;
        break;
      }
    }
  }
  if (complete) *complete = total > 0 && found >= total;
  return static_cast<int>(espnowhist::boundedCopy(out, cap, joined.c_str(), joined.size()));
}

static std::vector<ReceivedTextMessage> gPolled;   // what getAll/getPeer return
static bool gPeerPath = false;
static uint8_t gPeerPathMac[6];

int getPeerMessages(uint8_t* peerMac, ReceivedTextMessage* outMessages, int maxMessages, uint32_t sinceSeq) {
  gPeerPath = true;
  std::memcpy(gPeerPathMac, peerMac, 6);
  int k = 0;
  for (const ReceivedTextMessage& m : gPolled)
    if (k < maxMessages && m.seqNum > sinceSeq && std::memcmp(m.peerMac, peerMac, 6) == 0) outMessages[k++] = m;
  return k;
}

int getAllMessages(ReceivedTextMessage* outMessages, int maxMessages, uint32_t sinceSeq) {
  gPeerPath = false;
  int k = 0;
  for (const ReceivedTextMessage& m : gPolled)
    if (k < maxMessages && m.seqNum > sinceSeq) outMessages[k++] = m;
  return k;
}

// ---- resolver double: the System_ESPNow.h contract over a name table ----------------
struct PeerName { uint8_t mac[6]; char name[32]; };
static std::vector<PeerName> gNames;
static int gResolveCalls = 0;
static bool gResolvedInStore = false;

// Like the registry's fixed 32-byte field: a 32-char name has no NUL.
static void setName(const uint8_t mac[6], const char* name) {
  PeerName p;
  std::memcpy(p.mac, mac, 6);
  std::memset(p.name, 0, sizeof p.name);
  std::memcpy(p.name, name, strnlen(name, sizeof p.name));
  for (PeerName& q : gNames)
    if (std::memcmp(q.mac, mac, 6) == 0) { q = p; return; }
  gNames.push_back(p);
}

void espnowHistoryDisplayName(const ReceivedTextMessage& m, char* out, size_t cap) {
  ++gResolveCalls;
  if (gInStore) gResolvedInStore = true;
  char macText[18];
  macToDisplay(m.peerMac, macText, sizeof macText);
  const char* current = nullptr;
  for (const PeerName& p : gNames)
    if (std::memcmp(p.mac, m.peerMac, 6) == 0) current = p.name;
  espnowhist::displayName(m.isSent, m.unattributed, current, 32, macText, out, cap);
}

// ---- httpd / web doubles -------------------------------------------------------------
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_RESP_USE_STRLEN -1
struct httpd_req_t { std::string query; std::string body; bool hasQuery = false; int chunks = 0; bool ended = false; };
#define WEB_AUTH_OR_RETURN(req, ctx) do { (void)(req); } while (0)
static void httpd_resp_set_type(httpd_req_t*, const char*) {}
static esp_err_t httpd_resp_send(httpd_req_t* req, const char* s, long len) {
  req->body.assign(s, len < 0 ? std::strlen(s) : static_cast<size_t>(len));
  return ESP_OK;
}
static esp_err_t httpd_resp_send_chunk(httpd_req_t* req, const char* s, long len) {
  if (!s) { req->ended = true; return ESP_OK; }
  req->body.append(s, len < 0 ? std::strlen(s) : static_cast<size_t>(len));
  ++req->chunks;
  return ESP_OK;
}
static esp_err_t httpd_req_get_url_query_str(httpd_req_t* req, char* buf, size_t cap) {
  if (!req->hasQuery) return ESP_FAIL;
  espnowhist::boundedCopy(buf, cap, req->query.c_str(), req->query.size());
  return ESP_OK;
}
static esp_err_t httpd_query_key_value(const char* q, const char* key, char* val, size_t cap) {
  const size_t kl = std::strlen(key);
  for (const char* p = q; p && *p;) {
    const char* amp = std::strchr(p, '&');
    const char* end = amp ? amp : p + std::strlen(p);
    if (static_cast<size_t>(end - p) > kl && std::strncmp(p, key, kl) == 0 && p[kl] == '=') {
      espnowhist::boundedCopy(val, cap, p + kl + 1, static_cast<size_t>(end - (p + kl + 1)));
      return ESP_OK;
    }
    p = amp ? amp + 1 : nullptr;
  }
  return ESP_FAIL;
}
enum class AllocPref { PreferPsram, PreferInternal, RequirePsram, RequireInternal };
static void* ps_alloc(size_t n, AllocPref, const char*) { return std::calloc(1, n); }
static unsigned long millis() { return 5000; }
struct SendStatus { bool inUse; uint8_t state; uint8_t peerMac[6]; uint32_t registeredAtMs; uint32_t msgId; };
static SendStatus gSendStatus[2] = {{true, 1, {1, 2, 3, 4, 5, 6}, 4000, 42}, {false, 0, {0}, 0, 0}};
static uint8_t sendStatusSlotCount() { return 2; }
static const SendStatus* sendStatusAt(uint8_t i) { return i < 2 ? &gSendStatus[i] : nullptr; }

// ---- OLED doubles -----------------------------------------------------------------------
#define OLED_SCROLL_MAX_ITEMS 32
struct OledScrollItem { const char* line1; const char* line2; bool selectable; void* userData; };
struct OledScrollState {
  OledScrollItem items[OLED_SCROLL_MAX_ITEMS];
  int itemCount, selectedIndex, scrollOffset, visibleLines;
};
static void oledScrollClear(OledScrollState* s) { s->itemCount = 0; s->selectedIndex = 0; s->scrollOffset = 0; }
static bool oledScrollAddItem(OledScrollState* s, const char* l1, const char* l2 = nullptr, bool sel = true,
                              void* ud = nullptr) {
  if (s->itemCount >= OLED_SCROLL_MAX_ITEMS) return false;
  s->items[s->itemCount++] = {l1, l2, sel, ud};
  return true;
}
static OledScrollState sEspNowMessageListScroll = {{}, 0, 0, 0, 4};
struct { uint8_t selectedDeviceMac[6]; } gOledEspNowState = {{0x10, 0x20, 0x30, 0x40, 0x50, 0x60}};
static struct { bool isSent; uint32_t msgId; uint8_t sendState; } gOledRowMeta[OLED_SCROLL_MAX_ITEMS];

// ---- production reader code --------------------------------------------------------------
// INSERT_WEB

// INSERT_OLED

// INSERT_G2

// ---- cases ----------------------------------------------------------------------------------
static const uint8_t kA[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
static const uint8_t kB[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x11, 0x22};

static void resetWorld() {
  gStore.clear(); gRefs.clear(); gPolled.clear(); gNames.clear();
  gLastMaxN = -1; gCopyCalls = gCopyRefused = 0; gResolveCalls = 0; gResolvedInStore = false;
}

static int caseWeb() {
  resetWorld();
  setName(kA, "Alpha \"one\"");                 // needs JSON escaping
  setName(kB, "Bravo");
  gPolled.push_back(makeRecord(kA, 1, "hi from A", false));
  gPolled.push_back(makeRecord(kA, 2, "plain A", false, /*unattributed=*/true));
  gPolled.push_back(makeRecord(kA, 3, "to A", true, false, 77, 1, 1, 1));
  gPolled.push_back(makeRecord(kB, 4, "from B\nline", false, false, 9, 1, 2));
  gPolled.push_back(makeRecord(kB, 5, "part two", false, false, 9, 2, 2));
  gPolled.push_back(makeRecord(kA, 6, "again A", false));
  // Ten more peers, so the cache (8 entries) must replace entries.
  for (uint8_t i = 0; i < 10; ++i) {
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, static_cast<uint8_t>(i)};
    char name[32];
    std::snprintf(name, sizeof name, "peer%u", static_cast<unsigned>(i));
    setName(mac, name);
    gPolled.push_back(makeRecord(mac, 10u + i, "x", false));
  }
  httpd_req_t req;
  handleEspNowMessages(&req);
  CHECK(req.ended);
  CHECK(!gResolvedInStore);
  // 3 kinds for A, 1 for B, then one per extra peer shown (10, or 9 when the
  // no-PSRAM poll stops at 15 rows): each resolved once; the extra peers come
  // after A and B, so no entry is resolved twice although the cache replaces.
  CHECK(gResolveCalls == 4 + (kWebMessagesBufCount - 6 < 10 ? kWebMessagesBufCount - 6 : 10));
  std::printf("WEB_ALL %s\n", req.body.c_str());

  // ?mac= path: the filtered read, names the same way.
  httpd_req_t one;
  one.hasQuery = true;
  one.query = "since=1&mac=AA%3ABB%3ACC%3A00%3A11%3A22";
  handleEspNowMessages(&one);
  CHECK(gPeerPath && std::memcmp(gPeerPathMac, kB, 6) == 0);
  std::printf("WEB_PEER %s\n", one.body.c_str());

  // Rename between polls: the next poll shows the new name.
  setName(kB, "Bravo2");
  httpd_req_t again;
  again.hasQuery = true;
  again.query = "since=3";
  handleEspNowMessages(&again);
  std::printf("WEB_RENAMED %s\n", again.body.c_str());

  // ESP-NOW down: empty list, no store call.
  gEspNow->initialized = false;
  httpd_req_t down;
  handleEspNowMessages(&down);
  CHECK(down.body == "{\"messages\":[]}");
  gEspNow->initialized = true;
  return 0;
}

static int caseOled() {
  resetWorld();
  setName(kA, "Alpha");
  // 40 logical rows: more than the 32 the OLED shows.
  for (uint32_t s = 1; s <= 40; ++s) {
    ReceivedTextMessage m;
    if (s % 5 == 0) m = makeRecord(kA, s, "sent row", true, false, 500 + s, 1, 1, s % 4);
    else if (s == 37) m = makeRecord(kA, s, "plaintext", false, true);
    else if (s == 38) m = makeRecord(kA, s, "first piece of a long one", false, false, 900, 1, 3);
    else { char t[24]; std::snprintf(t, sizeof t, "msg %u", static_cast<unsigned>(s)); m = makeRecord(kA, s, t, false); }
    gStore.push_back(m);
    gRefs.push_back(refFor(m, s == 38 ? 2 : 1));
  }
  evict(9);   // refused on copy: the row is skipped, never read through the old offset
  evict(39);
  oledEspNowRefreshMessages();
  CHECK(gLastMaxN == ESPNOW_HISTORY_VIEW_MAX);
  CHECK(!gResolvedInStore);
  // Rows shown: refs 9..40 (the newest 32) minus the evicted 9 and 39 = 30 rows.
  CHECK(sEspNowMessageListScroll.itemCount == 30);
  CHECK(gCopyRefused == 2);
  // Resolver: once for attributed, once for unattributed (one peer).
  CHECK(gResolveCalls == 2);
  bool sawParts = false, sawMacText = false, sawSent = false;
  for (int i = 0; i < sEspNowMessageListScroll.itemCount; ++i) {
    const OledScrollItem& it = sEspNowMessageListScroll.items[i];
    if (gOledRowMeta[i].isSent) {
      sawSent = true;
      CHECK(it.line2 == nullptr);
      CHECK(gOledRowMeta[i].msgId >= 505 && gOledRowMeta[i].msgId <= 540);
      CHECK(gOledRowMeta[i].sendState == (gOledRowMeta[i].msgId - 500) % 4);
      CHECK(std::strcmp(it.line1, "sent row") == 0);
    } else if (std::strcmp(it.line1, "plaintext") == 0) {
      sawMacText = true;
      CHECK(std::strcmp(it.line2, "10:20:30:40:50:60") == 0);
    } else if (std::strncmp(it.line1, "first piece", 11) == 0) {
      sawParts = true;
      CHECK(std::strcmp(it.line2, "Alpha (2/3)") == 0);
      CHECK(std::strlen(it.line1) == 23);        // line-1 buffer is 24 bytes
    } else {
      CHECK(std::strcmp(it.line2, "Alpha") == 0);
      CHECK(gOledRowMeta[i].msgId == 0);
    }
  }
  CHECK(sawParts && sawMacText && sawSent);
  CHECK(std::strcmp(sEspNowMessageListScroll.items[0].line1, "sent row") == 0);   // seq 10
  CHECK(std::strcmp(sEspNowMessageListScroll.items[1].line1, "msg 11") == 0);

  // Rename: the next refresh shows the new name (the cache is per refresh).
  setName(kA, "Alpha renamed and quite long name");  // 32 bytes, no NUL in the table
  oledEspNowRefreshMessages();
  CHECK(std::strcmp(sEspNowMessageListScroll.items[1].line2, "Alpha renamed and quite long na") == 0);

  // Every ref refused: the empty-state rows.
  gStore.clear();
  oledEspNowRefreshMessages();
  CHECK(sEspNowMessageListScroll.itemCount == 1);
  CHECK(std::strcmp(sEspNowMessageListScroll.items[0].line1, "No messages yet") == 0);
  return 0;
}

static int caseG2() {
  resetWorld();
  setName(kA, "Alpha");
  setName(kB, "Bravo");
  ReceivedTextMessage sent = makeRecord(kA, 1, "hello", true, false, 5);
  ReceivedTextMessage rx = makeRecord(kB, 2, "yo\tthere\x7f", false);
  ReceivedTextMessage plain = makeRecord(kB, 3, "who am i", false, true);
  ReceivedTextMessage p1 = makeRecord(kB, 4, "AAAA", false, false, 70, 1, 3);
  ReceivedTextMessage p3 = makeRecord(kB, 6, "CCCC", false, false, 70, 3, 3);
  ReceivedTextMessage s1 = makeRecord(kA, 7, "first ", true, false, 71, 1, 2);
  ReceivedTextMessage s2 = makeRecord(kA, 8, "second", true, false, 71, 2, 2);
  for (const ReceivedTextMessage& m : {sent, rx, plain, p1, p3, s1, s2}) gStore.push_back(m);

  char out[1184];
  gChatNames.clear();
  chatLogicalLine(out, sizeof out, refFor(sent), sent);
  CHECK(std::strcmp(out, "me: hello") == 0);
  chatLogicalLine(out, sizeof out, refFor(rx), rx);
  CHECK(std::strcmp(out, "Bravo: yo there ") == 0);
  chatLogicalLine(out, sizeof out, refFor(plain), plain);
  CHECK(std::strcmp(out, "AA:BB:CC:00:11:22: who am i") == 0);
  chatLogicalLine(out, sizeof out, refFor(p1, 2), p1);
  CHECK(std::strcmp(out, "Bravo: AAAACCCC ...") == 0);
  CHECK(std::memcmp(gLastReasmMac, kB, 6) == 0 && gLastReasmReqId == 70 && !gLastReasmSent);
  chatLogicalLine(out, sizeof out, refFor(s1, 2), s1);
  CHECK(std::strcmp(out, "me: first second") == 0);
  CHECK(std::memcmp(gLastReasmMac, kA, 6) == 0 && gLastReasmSent);
  CHECK(!gResolvedInStore);
  // Bravo attributed + Bravo unattributed (sent rows never ask): two resolves,
  // however many rows.
  int before = gResolveCalls;
  for (int i = 0; i < 5; ++i) chatLogicalLine(out, sizeof out, refFor(rx), rx);
  CHECK(gResolveCalls == before);
  CHECK(gResolveCalls == 2);

  // The size estimate uses the shown label.
  const size_t w = kChatCols - 2;
  size_t lbl = std::strlen("Bravo") + 2, txt = std::strlen(rx.message);
  CHECK(chatEstBytes(refFor(rx), rx) == lbl + txt + ((lbl + txt) / w + 1) * 3);
  lbl = 4;
  CHECK(chatEstBytes(refFor(s1, 2), s1) == lbl + 400 + ((lbl + 400) / w + 1) * 3);

  // After clear() a rename shows.
  setName(kB, "Bravo2");
  gChatNames.clear();
  chatLogicalLine(out, sizeof out, refFor(rx), rx);
  CHECK(std::strcmp(out, "Bravo2: yo there ") == 0);
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 2) { std::fprintf(stderr, "usage: %s web|oled|g2\n", argv[0]); return 2; }
  const std::string c = argv[1];
  if (c == "web") caseWeb();
  else if (c == "oled") caseOled();
  else if (c == "g2") caseG2();
  else { std::fprintf(stderr, "unknown case %s\n", c.c_str()); return 2; }
  std::printf("CASE %s: %d/%d checks passed\n", c.c_str(), gChecks - gFailures, gChecks);
  return gFailures ? 1 : 0;
}
