// Template compiled by test_bond_fs_poll.py, never as a separate target.
//
// The marker INSERT_PRODUCTION is replaced by the shipping text of
// WebPage_Bond.cpp from the bridge structs to the end of handleBondFsGet: the
// three bridges, the reply callbacks and the three /api/bond/fs handlers. The
// packed FS request/reply structs come from the shipping System_ESPNow_Wire.h
// (INSERT_WIRE). WebBondFsPollCore.h, System_ArgParse.h and
// System_ESPNow_AnswerCore.h are the real headers.
//
// Doubles: esp_http_server (a request with a query string and a captured
// body), the FreeRTOS mutex, the bonded peer, the session/admin check and the
// protocol module (fs*SendRequest, fsListCancel). The protocol double hands
// out ids from one rising counter like the module does and can deliver the
// reply before the send call returns.
//
// There is no binary semaphore in this file: production code that creates or
// waits on one does not compile here.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "System_ArgParse.h"
#include "System_ESPNow_AnswerCore.h"
#include "WebBondFsPollCore.h"

static unsigned checks = 0;
static void check(bool condition, const std::string& message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}

// ── Arduino String: the three calls the handlers use ────────────────────────
class String {
  std::string text_;
 public:
  String() = default;
  String(const char* s) : text_(s ? s : "") {}
  const char* c_str() const { return text_.c_str(); }
  void replace(const char* before, const char* after) {
    const size_t n = std::strlen(before), m = std::strlen(after);
    for (size_t i = 0; (i = text_.find(before, i)) != std::string::npos; i += m) {
      text_.replace(i, n, after);
    }
  }
};

// ── wire structs ────────────────────────────────────────────────────────────
constexpr unsigned ESPNOW_V4_MAX_PLAINTEXT = 202;
// INSERT_OPCODE
// INSERT_WIRE

// ── clock ───────────────────────────────────────────────────────────────────
static uint32_t gNow = 50000;
static uint32_t millis() { return gNow; }

// ── FreeRTOS mutex double ───────────────────────────────────────────────────
using TickType_t = uint32_t;
using BaseType_t = int;
static constexpr BaseType_t pdTRUE = 1, pdFALSE = 0;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
#define EXT_RAM_BSS_ATTR
struct Mutex { bool held = false; };
using SemaphoreHandle_t = Mutex*;

struct Locks {
  unsigned created = 0, takes = 0, refused = 0;
  TickType_t longestWait = 0;
  bool refuseAll = false;     // every take fails (the other task holds it)
  bool createFails = false;
};
static Locks locks;
static Mutex theMutex;

static SemaphoreHandle_t xSemaphoreCreateMutex() {
  if (locks.createFails) return nullptr;
  ++locks.created;
  return &theMutex;
}
static BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
  check(m == &theMutex, "only the bridge mutex is ever taken");
  ++locks.takes;
  if (wait > locks.longestWait) locks.longestWait = wait;
  if (locks.refuseAll) { ++locks.refused; return pdFALSE; }
  check(!m->held, "the bridge mutex is taken while it is already held");
  m->held = true;
  return pdTRUE;
}
static BaseType_t xSemaphoreGive(SemaphoreHandle_t m) {
  check(m == &theMutex && m->held, "give without a take");
  m->held = false;
  return pdTRUE;
}
static void requireUnlocked(const char* where) {
  check(!theMutex.held, std::string("the bridge mutex is held across ") + where);
}

// ── esp_http_server double ──────────────────────────────────────────────────
using esp_err_t = int;
static constexpr esp_err_t ESP_OK = 0, ESP_ERR_NOT_FOUND = 0x105,
                           ESP_ERR_HTTPD_RESULT_TRUNC = 0xb003;
struct httpd_req_t {
  std::string query;   // without the '?'; empty = no query string
  std::string body;
  std::string contentType;
  int chunks = 0;
  bool ended = false;
};
static esp_err_t httpd_req_get_url_query_str(httpd_req_t* req, char* buf, size_t len) {
  if (req->query.empty()) return ESP_ERR_NOT_FOUND;
  if (req->query.size() + 1 > len) return ESP_ERR_HTTPD_RESULT_TRUNC;
  std::memcpy(buf, req->query.c_str(), req->query.size() + 1);
  return ESP_OK;
}
static esp_err_t httpd_query_key_value(const char* query, const char* key, char* value, size_t len) {
  const size_t keyLen = std::strlen(key);
  const char* p = query;
  while (p && *p) {
    const char* end = std::strchr(p, '&');
    const size_t partLen = end ? static_cast<size_t>(end - p) : std::strlen(p);
    if (partLen > keyLen && std::strncmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
      const size_t valueLen = partLen - keyLen - 1;
      if (valueLen + 1 > len) return ESP_ERR_HTTPD_RESULT_TRUNC;
      std::memcpy(value, p + keyLen + 1, valueLen);
      value[valueLen] = '\0';
      return ESP_OK;
    }
    p = end ? end + 1 : nullptr;
  }
  return ESP_ERR_NOT_FOUND;
}
static esp_err_t httpd_resp_set_type(httpd_req_t* req, const char* type) {
  req->contentType = type;
  return ESP_OK;
}
static esp_err_t httpd_resp_send_chunk(httpd_req_t* req, const char* data, int len) {
  requireUnlocked("a socket write");
  check(!req->ended, "a chunk after the end of the response");
  if (!data) { req->ended = true; return ESP_OK; }
  req->body.append(data, static_cast<size_t>(len));
  ++req->chunks;
  return ESP_OK;
}
static esp_err_t sendJsonResponse(httpd_req_t* req, const char* body) {
  requireUnlocked("a socket write");
  check(!req->ended && req->body.empty(), "one response per request");
  req->contentType = "application/json";
  req->body = body;
  req->ended = true;
  return ESP_OK;
}

// ── session, admin, bond ────────────────────────────────────────────────────
struct AuthContext { bool admin = false; };
struct Scenario {
  bool authenticated = true, admin = true, bonded = true;
  unsigned adminChecks = 0;
};
static Scenario scenario;
#define WEB_AUTH_JSON_OR_RETURN(req, ctx)                                        \
  AuthContext ctx;                                                               \
  if (!scenario.authenticated) return sendJsonResponse(req, "{\"error\":\"auth\"}"); \
  ctx.admin = scenario.admin
static bool hasAdminPrivilege(const AuthContext& ctx) {
  ++scenario.adminChecks;
  return ctx.admin;
}
namespace BondedPeer {
static bool peerMacBytes(uint8_t mac[6]) {
  if (!scenario.bonded) return false;
  const uint8_t peer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  std::memcpy(mac, peer, 6);
  return true;
}
}  // namespace BondedPeer
static constexpr size_t FILE_MANAGER_MAX_PATH = INSERT_MAX_PATH;

// ── protocol module double ──────────────────────────────────────────────────
typedef void (*FsListReplyCallback)(const uint8_t peerMac[6],
                                    const V4PayloadFsListRespHeader* header,
                                    const V4PayloadFsEntry* entries);
typedef void (*FsStatReplyCallback)(const uint8_t peerMac[6], const V4PayloadFsStatResp* reply);
typedef void (*FsGetAckCallback)(const uint8_t peerMac[6], const V4PayloadFsGetResp* ack);

struct Protocol {
  uint32_t nextId = 100;
  bool sendFails = false;
  // Run inside the next send, after the id is assigned and before the call
  // returns: what espnow_task can do while the httpd task is still sending.
  std::function<void(uint32_t)> duringSend;
  struct Sent { char op; uint32_t id; std::string path; uint16_t start; };
  std::vector<Sent> sent;
  std::vector<uint32_t> cancelled;
  FsListReplyCallback listCb = nullptr;
  FsStatReplyCallback statCb = nullptr;
  FsGetAckCallback getCb = nullptr;
};
static Protocol protocol;
static const uint8_t kPeer[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

static uint32_t protocolSend(char op, const char* path, uint16_t start) {
  requireUnlocked("the ESP-NOW send");
  if (protocol.sendFails) return 0;
  const uint32_t id = protocol.nextId++;
  protocol.sent.push_back({op, id, path, start});
  if (protocol.duringSend) {
    auto hook = protocol.duringSend;
    protocol.duringSend = nullptr;
    hook(id);
  }
  return id;
}
static uint32_t fsListSendRequest(const uint8_t*, const char* path, uint16_t start,
                                  FsListReplyCallback cb) {
  protocol.listCb = cb;
  return protocolSend('L', path, start);
}
static uint32_t fsStatSendRequest(const uint8_t*, const char* path, FsStatReplyCallback cb) {
  protocol.statCb = cb;
  return protocolSend('S', path, 0);
}
static uint32_t fsGetSendRequest(const uint8_t*, const char* path, FsGetAckCallback cb) {
  protocol.getCb = cb;
  return protocolSend('G', path, 0);
}
static void fsListCancel(uint32_t reqId) {
  requireUnlocked("fsListCancel");
  protocol.cancelled.push_back(reqId);
}

#define ENABLE_BONDED_MODE 1
// INSERT_PRODUCTION

// ── helpers ─────────────────────────────────────────────────────────────────
static void resetAll() {
  check(!theMutex.held, "a case left the bridge mutex held");
  sWebFsBridge = {};
  sWebFsStatBridge = {};
  sWebFsGetBridge = {};
  scenario = Scenario{};
  const uint32_t nextId = protocol.nextId;
  protocol = Protocol{};
  protocol.nextId = nextId + 10;  // ids keep rising across cases, like the module's counter
  locks.refuseAll = false;
  locks.createFails = false;
  locks.longestWait = 0;
  gNow += 100000;
}

using Handler = esp_err_t (*)(httpd_req_t*);
static std::string call(Handler handler, const std::string& query) {
  httpd_req_t req;
  req.query = query;
  check(handler(&req) == ESP_OK, "handler returns ESP_OK");
  check(req.ended, "the response is complete");
  requireUnlocked("the handler's return");
  return req.body;
}
static std::string pendingJson(uint32_t id) {
  return "{\"success\":true,\"pending\":true,\"id\":" + std::to_string(id) + "}";
}
static const char* const kBusy = "{\"success\":false,\"error\":\"bridge busy\"}";
static const char* const kExpired = "{\"success\":false,\"error\":\"expired\"}";
static const char* const kTimeout = "{\"success\":false,\"error\":\"timeout\"}";
static const char* const kAdmin = "{\"success\":false,\"error\":\"admin required\"}";

static void deliverList(uint32_t id, uint8_t status, const char* path,
                        std::initializer_list<std::pair<const char*, bool>> names,
                        bool hasMore = false) {
  V4PayloadFsListRespHeader hdr = {};
  hdr.reqId = id;
  hdr.status = status;
  hdr.hasMore = hasMore ? 1 : 0;
  hdr.totalEntries = static_cast<uint16_t>(names.size());
  hdr.nextStartIndex = hasMore ? static_cast<uint16_t>(names.size()) : 0;
  std::snprintf(hdr.path, sizeof(hdr.path), "%s", path);
  std::vector<V4PayloadFsEntry> entries;
  for (const auto& n : names) {
    V4PayloadFsEntry e = {};
    std::snprintf(e.name, sizeof(e.name), "%s", n.first);
    e.isFolder = n.second ? 1 : 0;
    e.size = n.second ? 0 : 42;
    e.perms = 3;
    entries.push_back(e);
  }
  hdr.entryCount = static_cast<uint8_t>(entries.size());
  protocol.listCb(kPeer, &hdr, entries.empty() ? nullptr : entries.data());
  requireUnlocked("the list callback's return");
}
static void deliverStat(uint32_t id, uint8_t status) {
  V4PayloadFsStatResp reply = {};
  reply.reqId = id;
  reply.status = status;
  reply.totalBytes = 1000000;
  reply.usedBytes = 250000;
  reply.freeBytes = 750000;
  reply.percentUsedX10 = 255;
  std::snprintf(reply.path, sizeof(reply.path), "%s", "/sd");
  protocol.statCb(kPeer, &reply);
  requireUnlocked("the stat callback's return");
}
static void deliverAck(uint32_t id, uint8_t status, uint32_t size) {
  V4PayloadFsGetResp ack = {};
  ack.reqId = id;
  ack.status = status;
  ack.fileSize = size;
  std::snprintf(ack.path, sizeof(ack.path), "%s", "/logs/a.txt");
  protocol.getCb(kPeer, &ack);
  requireUnlocked("the get callback's return");
}

// ── the state machine on its own ────────────────────────────────────────────
static void coreCases() {
  namespace p = bond_fs_poll;
  p::Slot s = {};
  uint32_t cancel = 99;
  check(s.state == p::State::Idle, "zero storage is an idle slot");
  check(p::poll(s, 5, 0, cancel) == p::Poll::Unknown && cancel == 0, "an idle slot knows no id");
  check(!p::accept(s, 5, 0), "an idle slot accepts no reply");

  check(p::begin(s, 1000, cancel) == p::Begin::Started && cancel == 0, "begin on idle");
  check(p::begin(s, 1001, cancel) == p::Begin::Busy, "begin while starting is busy");
  check(p::sent(s, 7) == p::Sent::Pending && s.reqId == 7 && s.lastReqId == 7, "sent");
  check(p::begin(s, 1500, cancel) == p::Begin::Busy && cancel == 0, "begin while pending is busy");
  check(p::poll(s, 7, 1500, cancel) == p::Poll::Pending, "poll while pending");
  check(p::poll(s, 8, 1500, cancel) == p::Poll::Unknown && s.state == p::State::Pending,
        "another id does not disturb the request");
  check(!p::accept(s, 6, 1600) && !p::accept(s, 0, 1600), "a reply for another id is refused");
  check(p::accept(s, 7, 1600) && s.state == p::State::Ready, "the reply makes it ready");
  check(!p::accept(s, 7, 1601), "a second reply is refused");
  check(p::begin(s, 1600 + p::kReadyHoldMs - 1, cancel) == p::Begin::Busy,
        "an answer waits for its page for the hold time");
  check(p::poll(s, 7, 99999, cancel) == p::Poll::Ready && s.state == p::State::Collecting,
        "its own poll gets the answer however late");
  check(!p::accept(s, 7, 99999) && p::begin(s, 99999, cancel) == p::Begin::Busy,
        "nothing writes while the answer is being sent");
  p::collected(s);
  check(s.state == p::State::Idle && s.reqId == 0, "collected frees the slot");
  check(p::poll(s, 7, 99999, cancel) == p::Poll::Unknown, "an answer is handed out once");

  // An uncollected answer is dropped by the next start after the hold time.
  check(p::begin(s, 200000, cancel) == p::Begin::Started, "begin");
  check(p::sent(s, 9) == p::Sent::Pending && p::accept(s, 9, 200100), "ready");
  check(p::begin(s, 200100 + p::kReadyHoldMs, cancel) == p::Begin::Started && cancel == 0,
        "the next start takes the slot of an abandoned answer");
  check(p::poll(s, 9, 205200, cancel) == p::Poll::Unknown, "the dropped answer is gone");

  // The reply arrives before the send returns.
  check(!p::accept(s, 9, 205200), "while starting, the last id this slot carried is refused");
  check(!p::accept(s, 8, 205200), "and an older one");
  check(p::accept(s, 10, 205200) && s.state == p::State::Ready && s.reqId == 10, "a newer id is accepted");
  check(p::sent(s, 10) == p::Sent::Ready && s.lastReqId == 10, "sent reports the early answer");
  check(p::poll(s, 10, 205300, cancel) == p::Poll::Ready, "and it is collectable");
  p::collected(s);

  // The send fails.
  check(p::begin(s, 300000, cancel) == p::Begin::Started, "begin");
  check(p::sent(s, 0) == p::Sent::Failed && s.state == p::State::Idle, "a failed send frees the slot");

  // A pending request nobody answers: the poll reports the timeout and hands
  // back the id to cancel; so does the next start.
  check(p::begin(s, 400000, cancel) == p::Begin::Started && p::sent(s, 11) == p::Sent::Pending, "pending");
  check(p::poll(s, 11, 400000 + p::kPendingLimitMs - 1, cancel) == p::Poll::Pending && cancel == 0,
        "pending until the limit");
  check(p::poll(s, 11, 400000 + p::kPendingLimitMs, cancel) == p::Poll::TimedOut && cancel == 11 &&
        s.state == p::State::Idle, "timed out at the limit, id handed back");
  check(p::begin(s, 500000, cancel) == p::Begin::Started && p::sent(s, 12) == p::Sent::Pending, "pending");
  check(p::begin(s, 500000 + p::kPendingLimitMs, cancel) == p::Begin::Started && cancel == 12,
        "the next start cancels an abandoned request");
  check(p::sent(s, 13) == p::Sent::Pending, "and runs");
  check(!p::accept(s, 12, 507100), "the cancelled request's reply is refused");

  // millis() wrap: ages are unsigned differences.
  p::Slot w = {};
  check(p::begin(w, 0xFFFFFF00u, cancel) == p::Begin::Started && p::sent(w, 20) == p::Sent::Pending, "pending");
  check(p::poll(w, 20, 0x00000100u, cancel) == p::Poll::Pending, "512 ms later across the wrap: pending");
  check(p::poll(w, 20, 0xFFFFFF00u + p::kPendingLimitMs, cancel) == p::Poll::TimedOut, "limit across the wrap");

  // Id wrap: "newer" is a signed difference.
  p::Slot x = {};
  x.lastReqId = 0xFFFFFFFEu;
  check(p::begin(x, 10, cancel) == p::Begin::Started, "begin");
  check(!p::accept(x, 0xFFFFFFFEu, 10) && p::accept(x, 1, 10), "the id after the wrap is newer");

  // A state left behind by a handler that lost the mutex does not block the route.
  p::Slot stuck = {};
  check(p::begin(stuck, 1000, cancel) == p::Begin::Started, "starting");
  check(p::begin(stuck, 1000 + p::kPendingLimitMs - 1, cancel) == p::Begin::Busy, "still starting");
  check(p::begin(stuck, 1000 + p::kPendingLimitMs, cancel) == p::Begin::Started && cancel == 0,
        "a stale Starting is released");
}

// ── the handlers ────────────────────────────────────────────────────────────
static void listCases() {
  // Gates: session, admin (start and poll), bond.
  resetAll();
  scenario.authenticated = false;
  check(call(handleBondFsList, "path=%2F") == "{\"error\":\"auth\"}", "no session: refused");
  check(protocol.sent.empty(), "no session: nothing sent");
  resetAll();
  scenario.admin = false;
  check(call(handleBondFsList, "path=%2F") == kAdmin, "a non-admin cannot start a listing");
  check(protocol.sent.empty() && scenario.adminChecks == 1, "non-admin: nothing sent");
  resetAll();
  scenario.bonded = false;
  check(call(handleBondFsList, "path=%2F") == "{\"success\":false,\"error\":\"not bonded\"}", "not bonded");
  check(protocol.sent.empty(), "not bonded: nothing sent");
  resetAll();
  check(call(handleBondFsList, "") == "{\"success\":false,\"error\":\"missing query\"}", "no query");
  check(call(handleBondFsList, "start=3") == "{\"success\":false,\"error\":\"missing path\"}", "no path");

  // Start returns at once; the answer comes with a poll.
  resetAll();
  const uint32_t id = protocol.nextId;
  check(call(handleBondFsList, "path=%2Flogs%20old&start=32") == pendingJson(id),
        "the start answers pending with the request id");
  check(protocol.sent.size() == 1 && protocol.sent[0].op == 'L' && protocol.sent[0].path == "/logs old" &&
        protocol.sent[0].start == 32, "one FS_LIST request with the decoded path and start index");
  check(sWebFsBridge.slot.state == bond_fs_poll::State::Pending,
        "the handler returned while the peer has not answered");
  check(locks.longestWait <= 100, "no take of the bridge mutex waits longer than 100 ms");
  check(call(handleBondFsList, "id=" + std::to_string(id)) == pendingJson(id), "poll: pending");
  check(call(handleBondFsList, "id=" + std::to_string(id)) == pendingJson(id), "poll: still pending");
  check(protocol.sent.size() == 1 && protocol.cancelled.empty(), "polls send and cancel nothing");

  // A second start while one is in flight is busy, and sends nothing.
  check(call(handleBondFsList, "path=%2Fother") == kBusy, "second start: bridge busy");
  check(protocol.sent.size() == 1 && protocol.cancelled.empty(), "busy: nothing sent, nothing cancelled");

  // A non-admin cannot collect an admin's listing.
  deliverList(id, FS_LIST_STATUS_OK, "/logs old", {{"a\"b.txt", false}, {"sub", true}}, true);
  scenario.admin = false;
  check(call(handleBondFsList, "id=" + std::to_string(id)) == kAdmin, "poll by a non-admin: refused");
  check(sWebFsBridge.slot.state == bond_fs_poll::State::Ready, "the answer is still there");
  scenario.admin = true;
  // An id that is not this route's request does not disturb it.
  check(call(handleBondFsList, "id=" + std::to_string(id + 1)) == kExpired, "another id: expired");
  for (const char* bad : {"id=0", "id=abc", "id=-1", "id=4294967296", "id=12x", "id=", "id=1234567890123456789"}) {
    check(call(handleBondFsList, bad) == kExpired, std::string("not a request id: ") + bad);
  }
  check(protocol.sent.size() == 1, "a malformed id never starts a request");

  const std::string listing = call(handleBondFsList, "id=" + std::to_string(id));
  check(listing ==
        "{\"success\":true,\"path\":\"/logs old\",\"totalEntries\":2,\"hasMore\":true,\"nextStartIndex\":2,"
        "\"entries\":[{\"name\":\"a\\\"b.txt\",\"isDir\":false,\"size\":42,\"perms\":3},"
        "{\"name\":\"sub\",\"isDir\":true,\"size\":0,\"perms\":3}]}",
        "the listing JSON is the one the page read before: " + listing);
  check(sWebFsBridge.slot.state == bond_fs_poll::State::Idle, "collected: the slot is free");
  check(call(handleBondFsList, "id=" + std::to_string(id)) == kExpired, "an answer is handed out once");
  check(locks.longestWait <= 100 && protocol.cancelled.empty(), "nothing waited, nothing cancelled");

  // The reply arrives before the send call returns.
  resetAll();
  protocol.duringSend = [](uint32_t newId) { deliverList(newId, FS_LIST_STATUS_OK, "/", {{"x", false}}); };
  const uint32_t early = protocol.nextId;
  check(call(handleBondFsList, "path=%2F") == pendingJson(early), "early reply: the start still answers pending");
  check(sWebFsBridge.slot.state == bond_fs_poll::State::Ready, "early reply: stored");
  check(call(handleBondFsList, "id=" + std::to_string(early)).find("\"entries\":[{\"name\":\"x\"") != std::string::npos,
        "early reply: collected by the first poll");

  // A late reply of the previous request, delivered while the next send runs,
  // is not taken for the new request's answer.
  resetAll();
  const uint32_t first = protocol.nextId;
  check(call(handleBondFsList, "path=%2Fold") == pendingJson(first), "first request");
  gNow += bond_fs_poll::kPendingLimitMs;  // nobody answered, nobody polled
  protocol.duringSend = [first](uint32_t) { deliverList(first, FS_LIST_STATUS_OK, "/old", {{"stale", false}}); };
  const uint32_t second = protocol.nextId;
  check(call(handleBondFsList, "path=%2Fnew") == pendingJson(second), "second request starts");
  check(protocol.cancelled == std::vector<uint32_t>{first}, "the abandoned request is cancelled once");
  check(sWebFsBridge.slot.state == bond_fs_poll::State::Pending && sWebFsBridge.slot.reqId == second,
        "the stale reply did not become the new request's answer");
  check(call(handleBondFsList, "id=" + std::to_string(first)) == kExpired, "the old id is gone");
  deliverList(second, FS_LIST_STATUS_OK, "/new", {{"fresh", false}});
  check(call(handleBondFsList, "id=" + std::to_string(second)).find("fresh") != std::string::npos,
        "the new request's own answer is delivered");

  // The send fails: the error at once, the slot free.
  resetAll();
  protocol.sendFails = true;
  check(call(handleBondFsList, "path=%2F") == "{\"success\":false,\"error\":\"send failed\"}", "send failed");
  check(sWebFsBridge.slot.state == bond_fs_poll::State::Idle, "send failed: the slot is free");
  protocol.sendFails = false;
  check(call(handleBondFsList, "path=%2F") == pendingJson(protocol.nextId - 1), "the next start runs");

  // Nobody answers and the protocol's own sweep does not run: the poll says
  // timeout after the limit and the protocol's entry is freed.
  resetAll();
  const uint32_t lost = protocol.nextId;
  check(call(handleBondFsList, "path=%2F") == pendingJson(lost), "start");
  gNow += bond_fs_poll::kPendingLimitMs - 1;
  check(call(handleBondFsList, "id=" + std::to_string(lost)) == pendingJson(lost), "pending just before the limit");
  gNow += 1;
  check(call(handleBondFsList, "id=" + std::to_string(lost)) == kTimeout, "timeout at the limit");
  check(protocol.cancelled == std::vector<uint32_t>{lost}, "the request is cancelled once");
  check(call(handleBondFsList, "id=" + std::to_string(lost)) == kExpired, "then it is unknown");
  deliverList(lost, FS_LIST_STATUS_OK, "/", {{"late", false}});
  check(sWebFsBridge.slot.state == bond_fs_poll::State::Idle, "a reply after the timeout is ignored");

  // The protocol's 5 s timeout: its synthesized error is the answer.
  resetAll();
  const uint32_t swept = protocol.nextId;
  check(call(handleBondFsList, "path=%2F") == pendingJson(swept), "start");
  gNow += 5000;
  deliverList(swept, FS_LIST_STATUS_IO_ERROR, "", {});
  check(call(handleBondFsList, "id=" + std::to_string(swept)) ==
        "{\"success\":false,\"error\":\"io_error\",\"path\":\"\"}", "the protocol's timeout reads io_error");
  check(protocol.cancelled.empty(), "nothing to cancel: the protocol already dropped it");

  // The browser went away: the answer is dropped by the next start after the
  // hold time, and that start runs.
  resetAll();
  const uint32_t orphan = protocol.nextId;
  check(call(handleBondFsList, "path=%2Fa") == pendingJson(orphan), "start");
  deliverList(orphan, FS_LIST_STATUS_OK, "/a", {{"a", false}});
  gNow += bond_fs_poll::kReadyHoldMs - 1;
  check(call(handleBondFsList, "path=%2Fb") == kBusy, "the answer still waits for its page");
  gNow += 1;
  const uint32_t next = protocol.nextId;
  check(call(handleBondFsList, "path=%2Fb") == pendingJson(next), "after the hold time the next start runs");
  check(call(handleBondFsList, "id=" + std::to_string(orphan)) == kExpired, "the dropped answer is gone");
  check(protocol.cancelled.empty(), "an answered request needs no cancel");

  // The other task holds the mutex: the handler answers and does not wait on.
  resetAll();
  check(call(handleBondFsList, "path=%2F") == pendingJson(protocol.nextId - 1), "start");
  const uint32_t held = protocol.nextId - 1;
  locks.refuseAll = true;
  check(call(handleBondFsList, "id=" + std::to_string(held)) == pendingJson(held), "poll without the mutex: ask again");
  check(call(handleBondFsStat, "path=%2F") == kBusy, "start without the mutex: busy");
  check(protocol.sent.size() == 1, "start without the mutex: nothing sent");
  deliverList(held, FS_LIST_STATUS_OK, "/", {{"dropped", false}});
  check(sWebFsBridge.slot.state == bond_fs_poll::State::Pending, "a callback without the mutex stores nothing");
  check(locks.longestWait <= 100, "bounded waits only");
  locks.refuseAll = false;

  // No mutex at all.
  resetAll();
  sWebFsMutex = nullptr;
  locks.createFails = true;
  check(call(handleBondFsList, "path=%2F") == "{\"success\":false,\"error\":\"bridge init failed\"}", "init failed");
  check(protocol.sent.empty(), "init failed: nothing sent");
  locks.createFails = false;
}

static void statCases() {
  resetAll();
  const uint32_t id = protocol.nextId;
  check(call(handleBondFsStat, "") == pendingJson(id), "stat without a query starts for /");
  check(protocol.sent.size() == 1 && protocol.sent[0].op == 'S' && protocol.sent[0].path == "/", "FS_STAT for /");
  check(call(handleBondFsStat, "id=" + std::to_string(id)) == pendingJson(id), "poll: pending");
  check(call(handleBondFsStat, "path=%2Fsd") == kBusy, "second start: busy");
  deliverStat(id, FS_LIST_STATUS_OK);
  const std::string body = call(handleBondFsStat, "id=" + std::to_string(id));
  check(body == "{\"success\":true,\"path\":\"/sd\",\"total\":1000000,\"used\":250000,\"free\":750000,"
                "\"usagePercent\":25.5}", "the stat JSON is the one the page read before: " + body);
  check(call(handleBondFsStat, "id=" + std::to_string(id)) == kExpired, "handed out once");
  check(sWebFsStatBridge.slot.state == bond_fs_poll::State::Idle, "the slot is free");
  check(call(handleBondFsStat, "path=%2Fsd") == pendingJson(protocol.nextId - 1) &&
        protocol.sent.size() == 2, "the next stat starts at once");

  resetAll();
  const uint32_t refused = protocol.nextId;
  check(call(handleBondFsStat, "path=%2Fsd") == pendingJson(refused), "start");
  check(protocol.sent[0].path == "/sd", "the decoded path");
  deliverStat(refused, FS_LIST_STATUS_NOT_READY);
  check(call(handleBondFsStat, "id=" + std::to_string(refused)) ==
        "{\"success\":false,\"error\":\"not_ready\",\"path\":\"/sd\"}", "a peer status is the error");

  resetAll();
  scenario.bonded = false;
  check(call(handleBondFsStat, "path=%2F") == "{\"success\":false,\"error\":\"not bonded\"}", "not bonded");
  resetAll();
  protocol.sendFails = true;
  check(call(handleBondFsStat, "path=%2F") == "{\"success\":false,\"error\":\"send failed\"}", "send failed");
  resetAll();
  const uint32_t lost = protocol.nextId;
  check(call(handleBondFsStat, "path=%2F") == pendingJson(lost), "start");
  gNow += bond_fs_poll::kPendingLimitMs;
  check(call(handleBondFsStat, "id=" + std::to_string(lost)) == kTimeout, "timeout");
  check(protocol.cancelled == std::vector<uint32_t>{lost}, "cancelled once");
  check(call(handleBondFsStat, "id=nope") == kExpired, "a malformed id");
}

static void getCases() {
  resetAll();
  scenario.admin = false;
  check(call(handleBondFsGet, "path=%2Flogs%2Fa.txt") == kAdmin, "a non-admin cannot start a pull");
  check(call(handleBondFsGet, "id=5") == kAdmin, "nor poll one");
  check(protocol.sent.empty(), "non-admin: nothing sent");

  resetAll();
  const uint32_t id = protocol.nextId;
  check(call(handleBondFsGet, "path=%2Flogs%2Fa.txt") == pendingJson(id), "the start answers pending");
  check(protocol.sent.size() == 1 && protocol.sent[0].op == 'G' && protocol.sent[0].path == "/logs/a.txt",
        "one FS_GET request");
  check(call(handleBondFsGet, "id=" + std::to_string(id)) == pendingJson(id), "poll: pending");
  check(call(handleBondFsGet, "path=%2Flogs%2Fa.txt") == kBusy, "a second start is busy");
  check(protocol.sent.size() == 1, "the file is requested once");
  deliverAck(id, FS_LIST_STATUS_OK, 4096);
  const std::string body = call(handleBondFsGet, "id=" + std::to_string(id));
  check(body == "{\"success\":true,\"path\":\"/logs/a.txt\",\"size\":4096,"
                "\"message\":\"transfer initiated; poll local FS for file landing\"}",
        "the ack JSON is the one the page read before: " + body);
  check(call(handleBondFsGet, "id=" + std::to_string(id)) == kExpired, "handed out once");
  check(sWebFsGetBridge.slot.state == bond_fs_poll::State::Idle, "collected: the slot is free");
  check(call(handleBondFsGet, "path=%2Flogs%2Fb.txt") == pendingJson(protocol.nextId - 1) &&
        protocol.sent.size() == 2, "the next pull starts at once");

  resetAll();
  const uint32_t refused = protocol.nextId;
  check(call(handleBondFsGet, "path=%2Fsecret") == pendingJson(refused), "start");
  deliverAck(refused, FS_LIST_STATUS_PERM_DENIED, 0);
  check(call(handleBondFsGet, "id=" + std::to_string(refused)) ==
        "{\"success\":false,\"error\":\"refused\",\"path\":\"/logs/a.txt\"}", "a refused pull");
  check(call(handleBondFsGet, "") == "{\"success\":false,\"error\":\"missing query\"}", "no query");
  check(call(handleBondFsGet, "x=1") == "{\"success\":false,\"error\":\"missing path\"}", "no path");

  // Routes do not share requests: a listing's id is unknown to /get and
  // /stat, and stays collectable on /list.
  resetAll();
  const uint32_t listId = protocol.nextId;
  check(call(handleBondFsList, "path=%2F") == pendingJson(listId), "list start");
  deliverList(listId, FS_LIST_STATUS_OK, "/", {{"only-for-list", false}});
  check(call(handleBondFsGet, "id=" + std::to_string(listId)) == kExpired, "/get does not hand out a listing");
  check(call(handleBondFsStat, "id=" + std::to_string(listId)) == kExpired, "/stat does not hand out a listing");
  const uint32_t statId = protocol.nextId;
  check(call(handleBondFsStat, "path=%2F") == pendingJson(statId), "a stat runs beside the listing");
  check(call(handleBondFsList, "id=" + std::to_string(listId)).find("only-for-list") != std::string::npos,
        "the listing is still collectable on its own route");
}

int main() {
  try {
    coreCases();
    listCases();
    statCases();
    getCases();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
  check(locks.created >= 1, "the bridge mutex was created");
  std::printf("bond fs start-and-poll: %u checks, 0 failures\n", checks);
  return 0;
}
