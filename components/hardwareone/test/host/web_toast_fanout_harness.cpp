// Drives the production web toast path: the System_Notifications toast block
// (message formatter, payload renderer, predicate fan-out) and the session
// table's SSE queues from WebServer_Server.cpp / WebServer_Events.cpp (fence
// guard, slot invalidation, broadcast helpers, enqueue/dequeue). FreeRTOS's
// recursive mutex is a std::recursive_mutex double that records its owner.
//
// Cases:
//   toast_payload  a LOGIN_FAIL whose attempted name holds a quote, a
//                  backslash and a newline reaches the session queue as JSON
//                  that parses back to the same text; an all-control-character
//                  name still yields complete JSON that fits the queue slot.
//   fanout_race    a producer thread (notification tick / BLE role) fans out
//                  admin-only toasts and plain events while a consumer thread
//                  (httpd role) logs slot 0 out and in again between an admin
//                  and a guest and drains both queues. The viewer predicate must
//                  run without the fence, no admin-only toast may reach a guest
//                  session, and the stable session's queue must account for
//                  every event exactly once (delivered + dropped + left).
#include <Arduino.h>
#include <ArduinoJson.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

void webBatchTestStringAppend() {}

static std::atomic<unsigned> checks{0};
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "web toast fan-out check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

// ---------------------------------------------------------------------------
// Platform doubles
// ---------------------------------------------------------------------------
static unsigned long millis() { return 1000; }
#define DEBUG_SSEF(...) ((void)0)
#define PSRAM_JSON_DOC(name) JsonDocument name

struct StaticSemaphore_t { int unused; };
struct FenceMutex {
  std::recursive_mutex mutex;
  std::atomic<std::thread::id> owner{};
  int depth = 0;
};
using SemaphoreHandle_t = FenceMutex*;
using BaseType_t = int;
using TickType_t = uint32_t;
static constexpr BaseType_t pdTRUE = 1;
static constexpr TickType_t portMAX_DELAY = 0xFFFFFFFFu;
static FenceMutex gFenceMutex;
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t*) { return &gFenceMutex; }
static BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t m, TickType_t) {
  m->mutex.lock();
  m->owner.store(std::this_thread::get_id());
  ++m->depth;
  return pdTRUE;
}
static BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t m) {
  if (--m->depth == 0) m->owner.store(std::thread::id());
  m->mutex.unlock();
  return pdTRUE;
}
static bool fenceHeldByThisThread() { return gFenceMutex.owner.load() == std::this_thread::get_id(); }

enum CommandSource { SOURCE_WEB };
using TransportSessionEpoch = uint32_t;
static void transportSessionClose(CommandSource, TransportSessionEpoch) {}

// INSERT_SESSION_TYPES

SessionEntry* gSessions = nullptr;

// Production queue primitives (WebServer_Events.cpp / WebServer_Server.cpp).
// INSERT_QUEUE_PRIMITIVES

// Production session fence and slot invalidation (WebServer_Server.cpp).
namespace {
// INSERT_FENCE
}  // namespace

// Production broadcast helpers and fenced queue accessors.
// INSERT_BROADCAST

// ---------------------------------------------------------------------------
// Notification side doubles + production toast code
// ---------------------------------------------------------------------------
#include "System_Events.h"
const char* systemEventKindName(uint8_t) { return "kind"; }
// INSERT_NOTIF_TYPES
struct { bool notifToasts = true; } gSettings;
struct { uint32_t toastsBroadcast = 0; uint32_t toastsFiltered = 0; } gNotifPipe;
static bool notifToastAllowedFor(const char*, void*) { return true; }

// INSERT_NOTIF_PRODUCTION

static void renderToast(const SystemEvent& e, const NotifRule& r, const char* msg) {
  // INSERT_TOAST_BLOCK
}

// ---------------------------------------------------------------------------
// Harness helpers (the httpd side's session-table writes, as setSession and
// clearSession perform them under the fence)
// ---------------------------------------------------------------------------
static void httpdLogin(int idx, const char* user, const char* sid) {
  WebSessionFenceGuard guard;
  check(static_cast<bool>(guard));
  webInvalidateSessionSlot(idx);
  SessionEntry s;
  s.sid = sid;
  s.user = user;
  gSessions[idx] = s;
}

static void httpdLogout(int idx) {
  WebSessionFenceGuard guard;
  check(static_cast<bool>(guard));
  webInvalidateSessionSlot(idx);
  gSessions[idx] = SessionEntry();
}

// handleEvents' dequeue as the tree under test performs it.
static bool sseDrainOne(int idx, const String& sid, String& name, String& data) {
#if HAVE_FENCED_DEQUEUE
  return webSessionDequeueSseEvent(idx, sid, name, data);
#else
  (void)sid;
  return sseDequeueEvent(gSessions[idx], name, data);
#endif
}

static void resetTable() {
  delete[] gSessions;
  gSessions = new SessionEntry[MAX_SESSIONS];
}

// ---------------------------------------------------------------------------
// toast_payload
// ---------------------------------------------------------------------------
static void renderLoginFail(const char* attemptedName, std::string& outPayload) {
  resetTable();
  httpdLogin(0, "admin", "sid-toast-0000000000000000000000");
  SystemEvent e{};
  e.kind = SYSEVT_LOGIN_FAIL;
  std::snprintf(e.subject, sizeof(e.subject), "%s", attemptedName);
  char msg[64];
  notifFormatEvent(e, msg, sizeof(msg));
  NotifRule r{};
  r.level = 2;
  r.durMs = 4000;
  renderToast(e, r, msg);
  String name, data;
  check(sseDrainOne(0, String("sid-toast-0000000000000000000000"), name, data));
  check(std::strcmp(name.c_str(), "notification") == 0);
  outPayload = data.c_str();
}

static void caseToastPayload() {
  const char* attempted = "ev\"il\\na\nme";
  std::string payload;
  renderLoginFail(attempted, payload);
  std::printf("toast payload: %s\n", payload.c_str());
  check(payload.find('\n') == std::string::npos);  // one SSE data line
  JsonDocument doc;
  check(deserializeJson(doc, payload) == DeserializationError::Ok);
  check(doc.is<JsonObject>());
  check(doc["level"] == "warning");
  check(doc["ms"].as<unsigned>() == 4000);
  const std::string want = std::string("Login failed: ") + attempted;
  check(doc["msg"].is<const char*>());
  check(want == doc["msg"].as<const char*>());
  check(doc.as<JsonObject>().size() == 3);  // nothing injected

  // Worst case: every byte of the subject escapes to six characters. The
  // payload must stay complete JSON inside one queue slot, the message cut
  // at a character boundary.
  char controls[SYSEVT_SUBJECT_LEN];
  for (size_t i = 0; i + 1 < sizeof(controls); ++i) controls[i] = static_cast<char>(1 + (i % 30));
  controls[sizeof(controls) - 1] = '\0';
  std::string worst;
  renderLoginFail(controls, worst);
  check(worst.size() < static_cast<size_t>(SessionEntry::EVENT_DATA_MAX));
  JsonDocument worstDoc;
  check(deserializeJson(worstDoc, worst) == DeserializationError::Ok);
  const std::string got = worstDoc["msg"].as<const char*>();
  const std::string full = std::string("Login failed: ") + controls;
  check(got.size() > std::strlen("Login failed: "));
  check(full.compare(0, got.size(), got) == 0);  // a prefix of the real text
  check(worstDoc["level"] == "warning");
}

// ---------------------------------------------------------------------------
// fanout_race
// ---------------------------------------------------------------------------
static const char* const kAdmin = "administrator-account-with-a-long-name";
static const char* const kGuest = "guest-account-with-a-much-longer-name-here";
static const char* const kOperator = "operator-account-with-a-long-name";
static std::atomic<unsigned> predicateUnderFence{0};
static std::atomic<unsigned> predicateCalls{0};
static std::atomic<unsigned> loginCount{0};

static bool adminOnly(const char* username, void*) {
  const unsigned call = ++predicateCalls;
  if (fenceHeldByThisThread()) ++predicateUnderFence;
  // The device decides admin visibility from the name it reads first, then
  // resolves the rest of the viewer from flash and reads the name again.
  // Every 16th call waits (bounded) until the httpd side has signed the slot
  // out and in again meanwhile.
  std::string before(username);
  if (call % 16 == 0) {
    const unsigned seen = loginCount.load();
    for (int i = 0; i < 2000 && loginCount.load() == seen; ++i) {
      std::this_thread::sleep_for(std::chrono::microseconds(5));
    }
  } else {
    std::this_thread::sleep_for(std::chrono::microseconds(30));
  }
  std::string after(username);
  return before == kAdmin && !after.empty();
}

static void caseFanoutRace() {
  resetTable();
  const String operatorSid("sid-operator-000000000000000000000");
  httpdLogin(1, kOperator, operatorSid.c_str());
  httpdLogin(0, kAdmin, "sid-admin-0000000000000000000000000");

  const unsigned kRounds = 6000;
  std::atomic<bool> producerDone{false};
  unsigned plainProduced = 0;

  std::thread producer([&] {
    char data[64];
    for (unsigned seq = 1; seq <= kRounds; ++seq) {
      std::snprintf(data, sizeof(data), "{\"seq\":%u,\"admin\":1}", seq);
      broadcastEventToSessionsIf("notification", data, adminOnly, nullptr);
      std::snprintf(data, sizeof(data), "{\"seq\":%u,\"admin\":0}", seq);
      broadcastEventToAllSessions("ring-status", data);
      ++plainProduced;
    }
    producerDone.store(true);
  });

  unsigned misdelivered = 0, duplicates = 0, reordered = 0, operatorPlain = 0;
  unsigned logins = 0;
  bool slot0Admin = true;
  String slot0Sid("sid-admin-0000000000000000000000000");
  std::set<unsigned> slot0Seen;
  unsigned lastOperatorSeq = 0;
  unsigned spin = 0;
  auto drain = [&] {
    String name, data;
    while (sseDrainOne(0, slot0Sid, name, data)) {
      unsigned seq = 0, admin = 0;
      check(std::sscanf(data.c_str(), "{\"seq\":%u,\"admin\":%u}", &seq, &admin) == 2);
      if (admin && !slot0Admin) ++misdelivered;
      if (!slot0Seen.insert(seq * 2 + admin).second) ++duplicates;
    }
    while (sseDrainOne(1, operatorSid, name, data)) {
      unsigned seq = 0, admin = 0;
      check(std::sscanf(data.c_str(), "{\"seq\":%u,\"admin\":%u}", &seq, &admin) == 2);
      if (admin) ++misdelivered;  // the operator is never an admin viewer
      if (seq <= lastOperatorSeq) ++reordered;
      lastOperatorSeq = seq;
      ++operatorPlain;
    }
  };
  while (!producerDone.load()) {
    drain();
    if (++spin % 3 == 0) {
      // Sign slot 0 out and back in as the other account (a fresh SID each time).
      httpdLogout(0);
      slot0Admin = !slot0Admin;
      char sid[40];
      std::snprintf(sid, sizeof(sid), "sid-%08u-00000000000000000000000", ++logins);
      httpdLogin(0, slot0Admin ? kAdmin : kGuest, sid);
      ++loginCount;
      slot0Sid = sid;
      slot0Seen.clear();
    }
    std::this_thread::yield();
  }
  producer.join();
  drain();

  unsigned dropped = 0, left = 0;
  {
    WebSessionFenceGuard guard;
    dropped = gSessions[1].eqDropped;
    left = static_cast<unsigned>(gSessions[1].eqCount);
  }
  std::printf("fanout_race: predicate calls=%u under-fence=%u logins=%u misdelivered=%u "
              "duplicates=%u reordered=%u operator delivered=%u dropped=%u left=%u produced=%u\n",
              predicateCalls.load(), predicateUnderFence.load(), logins, misdelivered, duplicates,
              reordered, operatorPlain, dropped, left, plainProduced);
  check(logins > 10);  // the race was exercised
  check(predicateUnderFence.load() == 0);
  check(misdelivered == 0);
  check(duplicates == 0);
  check(reordered == 0);
  check(operatorPlain + dropped + left == plainProduced);
  check(sseEventDropsTotal() >= dropped);
}

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  if (only.empty() || only == "toast_payload") caseToastPayload();
  if (only.empty() || only == "fanout_race") caseFanoutRace();
  delete[] gSessions;
  std::printf("web toast fan-out harness: %u checks passed\n", checks.load());
  return 0;
}
