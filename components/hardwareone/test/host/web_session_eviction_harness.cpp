// Drives the production web login slot choice from WebServer_Server.cpp:
// setSession, findFreeSessionIndex, pruneExpiredSessions, the session cookie
// writer and the session fence / slot publish helpers. The runner maps
// `unsigned long` and `long` in the extracted code to the ESP32's 32-bit
// widths, so millis() arithmetic wraps as it does on the device. httpd,
// tokens, transport epochs, events and logout reasons are recording doubles.
//
// Cases:
//   earliest_expiry   with the table full, findFreeSessionIndex names the
//                     session that expires first, at early uptime too (never
//                     -1, never a blind slot 0)
//   eviction_signout  a login into a full table signs the evicted session out
//                     like any other logout: SYSEVT_LOGOUT "evicted", a logout
//                     reason for its browser, its socket closed, its epoch
//                     closed and its slot generation bumped
//   wrap              the same choice across the 49.7-day millis() wrap
#include <Arduino.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

void webBatchTestStringAppend() {}

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "web session eviction check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// ---------------------------------------------------------------------------
// Platform doubles
// ---------------------------------------------------------------------------
static uint32_t gNow = 1000;
static uint32_t millis() { return gNow; }
// Log lanes: arguments are evaluated (as on the device) and dropped.
static void logSink(const char*, ...) {}
#define BROADCAST_PRINTF(...) logSink(__VA_ARGS__)
#define DEBUG_AUTHF(...) logSink(__VA_ARGS__)

struct StaticSemaphore_t { int unused; };
struct FenceMutex { int depth = 0; };
using SemaphoreHandle_t = FenceMutex*;
using BaseType_t = int;
using TickType_t = uint32_t;
static constexpr BaseType_t pdTRUE = 1;
static constexpr TickType_t portMAX_DELAY = 0xFFFFFFFFu;
static FenceMutex gFence;
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t*) { return &gFence; }
static BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t m, TickType_t) { ++m->depth; return pdTRUE; }
static BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t m) { --m->depth; return pdTRUE; }

enum CommandSource { SOURCE_WEB };
using TransportSessionEpoch = uint32_t;
static TransportSessionEpoch lastEpoch = 0;
static std::vector<TransportSessionEpoch> closedEpochs;
static TransportSessionEpoch transportSessionOpen(CommandSource) { return ++lastEpoch; }
static void transportSessionClose(CommandSource, TransportSessionEpoch epoch) {
  if (epoch) closedEpochs.push_back(epoch);
}
static bool sessionIdleExpired(CommandSource, uint32_t, uint32_t) { return false; }
static uint32_t sessionStampNow() { return gNow; }

typedef int esp_err_t;
#define ESP_OK 0
struct httpd_req_t {
  int sockfd;
  const char* ip;
  std::string cookie;
};
using httpd_handle_t = void*;
static int serverToken = 0;
static httpd_handle_t server = &serverToken;
static bool gServerIsHttps = false;
static void getClientIP(httpd_req_t* req, String& out) { out = req->ip; }
static int httpd_req_to_sockfd(httpd_req_t* req) { return req->sockfd; }
static std::vector<int> closedSockets;
static esp_err_t httpd_sess_trigger_close(httpd_handle_t, int fd) {
  closedSockets.push_back(fd);
  return ESP_OK;
}
static esp_err_t httpd_resp_set_hdr(httpd_req_t* req, const char*, const char* value) {
  req->cookie = value;
  return ESP_OK;
}
static unsigned tokenCounter = 0;
static String makeSessToken() {
  char token[33];
  std::snprintf(token, sizeof(token), "tok%029u", ++tokenCounter);
  return String(token);
}

enum { SYSEVT_LOGOUT = 7 };
struct Event { int kind; std::string subject, detail; };
static std::vector<Event> events;
static void systemEventPost(int kind, const char* subject, const char* detail) {
  events.push_back({kind, subject ? subject : "", detail ? detail : ""});
}
struct Reason { std::string ip, reason; };
static std::vector<Reason> reasons;
static void storeLogoutReason(const String& ip, const String& reason) {
  reasons.push_back({ip.c_str(), reason.c_str()});
}
static void updateUserLastSeen(const String&) {}
static String gBootId("boot-1");

// Production types (32-bit widths).
// INSERT_TYPES

static const uint32_t SESSION_TTL_MS = 24u * 60u * 60u * 1000u;
SessionEntry* gSessions = nullptr;

// Production session fence and slot helpers.
namespace {
// INSERT_FENCE
}  // namespace

// Production slot choice, prune and login.
// INSERT_PRODUCTION

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void resetTable() {
  delete[] gSessions;
  gSessions = new SessionEntry[MAX_SESSIONS];
  for (int i = 0; i < MAX_SESSIONS; ++i) webInvalidateSessionSlot(i);
  events.clear();
  reasons.clear();
  closedSockets.clear();
  closedEpochs.clear();
}

static httpd_req_t request(int fd, const char* ip) { return httpd_req_t{fd, ip, std::string()}; }

static int slotOf(const char* user) {
  for (int i = 0; i < MAX_SESSIONS; ++i) {
    if (gSessions[i].sid.length() > 0 && std::strcmp(gSessions[i].user.c_str(), user) == 0) return i;
  }
  return -1;
}

static void login(const char* user, int fd, const char* ip) {
  httpd_req_t req = request(fd, ip);
  const String sid = setSession(&req, String(user));
  check(sid.length() == 32);
  check(slotOf(user) >= 0);
  check(gFence.depth == 0);
}

// As clearSession does it, under the fence.
static void logout(const char* user) {
  const int idx = slotOf(user);
  check(idx >= 0);
  WebSessionFenceGuard guard;
  webInvalidateSessionSlot(idx);
  gSessions[idx] = SessionEntry();
}

static bool hasEvent(int kind, const char* subject, const char* detail) {
  for (const Event& e : events) {
    if (e.kind == kind && e.subject == subject && e.detail == detail) return true;
  }
  return false;
}

// Fill the table so that slot 1 holds the session that expires first: a logs
// in (slot 0), b logs in (slot 1), a signs out, c logs in (slot 0 again).
static void fillWithOlderSessionInSlotOne(uint32_t start) {
  resetTable();
  gNow = start;
  login("alice", 10, "10.0.0.1");
  gNow = start + 100;
  login("bob", 11, "10.0.0.2");
  gNow = start + 200;
  logout("alice");
  gNow = start + 300;
  login("carol", 12, "10.0.0.3");
  check(slotOf("carol") == 0 && slotOf("bob") == 1);
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------
static_assert(MAX_SESSIONS == 2, "the cases below assume the shipping two-slot table");

static void caseEarliestExpiry() {
  resetTable();
  gSessions[0].sid = "a";
  gSessions[0].expiresAt = 86402000u;  // about one day of uptime
  gSessions[1].sid = "b";
  gSessions[1].expiresAt = 86401000u;
  check(findFreeSessionIndex() == 1);
  gSessions[1].expiresAt = 86403000u;
  check(findFreeSessionIndex() == 0);
  gSessions[0].sid = String();
  check(findFreeSessionIndex() == 0);  // a free slot first
}

static void caseEvictionSignsOut() {
  fillWithOlderSessionInSlotOne(1000);
  const TransportSessionEpoch bobEpoch = sWebSessionEpoch[1].load();
  check(bobEpoch != 0);
#if HAVE_SLOT_GENERATION
  const uint32_t generation = sWebSessionSlotGeneration[1];
#endif
  events.clear();
  reasons.clear();
  closedSockets.clear();
  closedEpochs.clear();

  gNow = 5000;
  login("dave", 13, "10.0.0.4");
  if (slotOf("dave") != 1) {
    std::fprintf(stderr, "  dave took slot %d (carol=%d bob=%d)\n", slotOf("dave"), slotOf("carol"),
                 slotOf("bob"));
  }
  check(slotOf("dave") == 1);      // the session that expires first
  check(slotOf("carol") == 0);     // the newer session stays
  check(slotOf("bob") == -1);
  check(hasEvent(SYSEVT_LOGOUT, "bob", "evicted"));
  check(reasons.size() == 1 && reasons[0].ip == "10.0.0.2");
  check(closedSockets.size() == 1 && closedSockets[0] == 11);
  bool epochClosed = false;
  for (TransportSessionEpoch e : closedEpochs) epochClosed = epochClosed || e == bobEpoch;
  check(epochClosed);
#if HAVE_SLOT_GENERATION
  check(sWebSessionSlotGeneration[1] != generation);
#endif

  // A re-login by a signed-in user replaces its own slot and evicts nobody.
  events.clear();
  gNow = 6000;
  login("carol", 12, "10.0.0.3");
  check(slotOf("dave") >= 0);
  check(!hasEvent(SYSEVT_LOGOUT, "dave", "evicted"));

  // An evicted session on the same address as the new login gets no reason
  // stored (that browser is the one signing in) and its socket stays open
  // when it carries this login.
  fillWithOlderSessionInSlotOne(10000);
  reasons.clear();
  closedSockets.clear();
  gNow = 20000;
  login("erin", 11, "10.0.0.2");
  check(slotOf("erin") == 1);
  check(reasons.empty());
  check(closedSockets.empty());

  // A revoked session waiting out its notice grace period (as
  // enqueueTargetedRevokeForSessionIdx leaves it) already has its reason: it
  // is replaced first and its socket closed, with no second logout event.
  fillWithOlderSessionInSlotOne(30000);
  {
    WebSessionFenceGuard guard;
    webInvalidateSessionSlot(0);
    gSessions[0].revoked = true;
    gSessions[0].expiresAt = gNow + 30000u;
  }
  events.clear();
  reasons.clear();
  closedSockets.clear();
  gNow += 1000;
  login("frank", 14, "10.0.0.5");
  check(slotOf("frank") == 0);
  check(slotOf("bob") == 1);
  check(events.empty() && reasons.empty());
  check(closedSockets.size() == 1 && closedSockets[0] == 12);
}

static void caseAcrossWrap() {
  // bob logs in just before the 32-bit wrap; his session expires first.
  fillWithOlderSessionInSlotOne(0xFFFFFF00u);
  events.clear();
  gNow = 0x10u;  // after the wrap
  login("dave", 13, "10.0.0.4");
  check(slotOf("dave") == 1);
  check(slotOf("carol") == 0);
  check(hasEvent(SYSEVT_LOGOUT, "bob", "evicted"));
}

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  struct Case { const char* name; void (*run)(); };
  const Case cases[] = {
      {"earliest_expiry", caseEarliestExpiry},
      {"eviction_signout", caseEvictionSignsOut},
      {"wrap", caseAcrossWrap},
  };
  bool ran = false;
  for (const Case& c : cases) {
    if (!only.empty() && only != c.name) continue;
    c.run();
    ran = true;
    std::printf("ok %s\n", c.name);
  }
  check(ran);
  delete[] gSessions;
  std::printf("web session eviction: %u checks passed\n", checks);
  return 0;
}
