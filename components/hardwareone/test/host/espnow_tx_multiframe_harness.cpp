// Host harness for test_espnow_tx_multiframe.py. Two compiled units share this
// file, selected by -DHW1_UNIT_DISPATCHER or -DHW1_UNIT_STREAM:
//
//  * DISPATCHER: the real espnowtx namespace (sliced from System_ESPNow_Tx.cpp)
//    against single-threaded FreeRTOS doubles and a stubbed radio whose
//    multi-frame path models the per-fragment ACK wait. While a sender waits
//    for an ACK, other tasks run: producers submit fire-and-forget jobs, and
//    the dispatcher drains the queue unless it is the task that is waiting.
//    CALLER plays cmd_exec_task (gCmdExecTaskHandle); HTTPD plays any other
//    synchronous sender (httpd, the OLED loop).
//  * STREAM: the real STREAM line sender (sendEspNowStreamMessage, its session
//    helper and session lookup, plus v4_single_frame_cap, the per-task session
//    pick streamCmdIdForCurrentTask and the broadcast gate
//    espnowStreamOutputWanted when the source has them) against a recording
//    espnowtx::sendAead and a per-task command-context double.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);        \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

using TaskHandle_t = void*;
static int gCallerToken;
static TaskHandle_t const CALLER = &gCallerToken;
static TaskHandle_t gCurrentTask = CALLER;
TaskHandle_t xTaskGetCurrentTaskHandle() { return gCurrentTask; }
static uint32_t gClock = 1000;
unsigned long millis() { return gClock; }
// Consumes log-only arguments so production variables stay "used" under -Werror.
void debugSink(int, const char*, ...) {}

static const uint8_t kDirect[6] = {0x5a, 0x10, 0x20, 0x30, 0x40, 0x50};
static const uint8_t kRouted[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x51};
// Test convention: a MAC whose first byte is 0x02 is two or more hops away.
bool meshRouterIsRouted(const uint8_t* dst) { return dst && dst[0] == 0x02; }

#if defined(HW1_UNIT_DISPATCHER)
// ============================================================================
// Unit 1: the espnow_tx dispatcher
// ============================================================================
static int gDispatcherToken;
static TaskHandle_t const DISPATCHER = &gDispatcherToken;
static int gHttpdToken;
static TaskHandle_t const HTTPD = &gHttpdToken;
TaskHandle_t gCmdExecTaskHandle = CALLER;
using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = uint32_t;
using EventBits_t = uint32_t;
using TaskFunction_t = void (*)(void*);
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) (ms)
struct portMUX_TYPE { int depth; };
#define portMUX_INITIALIZER_UNLOCKED {0}
static int gCritical = 0;
void taskENTER_CRITICAL(portMUX_TYPE* m) { ++m->depth; ++gCritical; }
void taskEXIT_CRITICAL(portMUX_TYPE* m) { --m->depth; --gCritical; }

struct FakeQueue {
  std::deque<std::vector<uint8_t>> items;
  UBaseType_t depth;
  size_t itemSize;
};
using QueueHandle_t = FakeQueue*;
QueueHandle_t xQueueCreate(UBaseType_t depth, size_t itemSize) {
  return new FakeQueue{{}, depth, itemSize};
}
// A full queue refuses both the 0-tick and the timed send: in this model the
// timed enqueue "blocks until its timeout" without anyone draining.
BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t) {
  if (q->items.size() >= q->depth) return pdFALSE;
  const uint8_t* p = static_cast<const uint8_t*>(item);
  q->items.emplace_back(p, p + q->itemSize);
  return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void* out, TickType_t) {
  if (q->items.empty()) return pdFALSE;
  std::memcpy(out, q->items.front().data(), q->itemSize);
  q->items.pop_front();
  return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { return (UBaseType_t)q->items.size(); }
void vQueueDelete(QueueHandle_t q) { delete q; }

struct FakeSemaphore { int count; };
using SemaphoreHandle_t = FakeSemaphore*;
struct StaticSemaphore_t { FakeSemaphore sem; };
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s) { s->sem.count = 1; return &s->sem; }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* s) { s->sem.count = 0; return &s->sem; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t) {
  if (s->count <= 0) return pdFALSE;
  --s->count;
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { ++s->count; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t) {}

struct FakeEventGroup { EventBits_t bits; };
using EventGroupHandle_t = FakeEventGroup*;
struct StaticEventGroup_t { FakeEventGroup group; };
static void (*gOnBlock)() = nullptr;  // what runs while a caller is blocked
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t* s) { s->group.bits = 0; return &s->group; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b) {
  EventBits_t old = g->bits;
  g->bits &= ~b;
  return old;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b) { g->bits |= b; return g->bits; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t b, BaseType_t, BaseType_t, TickType_t) {
  if (!(g->bits & b) && gOnBlock) gOnBlock();
  return g->bits;
}
void vEventGroupDelete(EventGroupHandle_t) {}

// The dispatcher task is never started on its own; stop() runs its body once
// (xTaskAbortDelay) so the real CLOSING drain and join execute.
static TaskFunction_t gTaskFn = nullptr;
static void* gTaskParam = nullptr;
BaseType_t xTaskCreateLogged(TaskFunction_t fn, const char*, uint32_t, void* param, UBaseType_t,
                             TaskHandle_t* out, const char*, BaseType_t) {
  gTaskFn = fn;
  gTaskParam = param;
  *out = DISPATCHER;
  return pdPASS;
}
void xTaskAbortDelay(TaskHandle_t) {
  if (!gTaskFn) return;
  TaskHandle_t prev = gCurrentTask;
  gCurrentTask = DISPATCHER;
  gTaskFn(gTaskParam);
  gCurrentTask = prev;
}
void vTaskDelete(TaskHandle_t) { gTaskFn = nullptr; gTaskParam = nullptr; }
static unsigned gDelayCalls = 0;
void vTaskDelay(TickType_t) {
  // A dispatcher that never quiesces would spin forever here; fail instead.
  if (++gDelayCalls > 100000) {
    std::fprintf(stderr, "FAIL: dispatcher never quiesced\n");
    std::exit(2);
  }
}
void vTaskSuspend(TaskHandle_t) {}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
int xPortGetCoreID() { return 0; }

enum class AllocPref { PreferPSRAM, RequireInternal };
void* ps_alloc(size_t n, AllocPref, const char*) { return std::malloc(n); }
void* ps_calloc(size_t c, size_t n, AllocPref, const char*) { return std::calloc(c, n); }
void heap_caps_free(void* p) { std::free(p); }
#define DEBUG_ESPNOW_CORE 1
#define DEBUGF(level, ...) debugSink(level, __VA_ARGS__)
static constexpr uint32_t ESPNOW_TX_STACK_WORDS = 5120;
static constexpr UBaseType_t TASK_PRIORITY_HIGH = 5;
static constexpr BaseType_t PRO_CORE = 0;

#include "System_ESPNow_Tx.h"

// ---- stubbed radio ---------------------------------------------------------
static constexpr uint16_t kDirectCap = 202, kRoutedCap = 130, kFragment = 200;
uint16_t v4_single_frame_cap(const uint8_t* dst) {
  return meshRouterIsRouted(dst) ? kRoutedCap : kDirectCap;
}
struct RadioCall { uint16_t len; TaskHandle_t task; };
static std::vector<RadioCall> gRadio;
static uint32_t gAckDelayMs = 200;
static bool gRadioOk = true;
static void (*gProducerHook)() = nullptr;  // other tasks get CPU during an ACK wait
bool v4_send_payload_smart(const uint8_t* dst, uint8_t, uint16_t, uint32_t,
                           const uint8_t*, uint16_t payloadLen, uint8_t) {
  gRadio.push_back({payloadLen, gCurrentTask});
  if (payloadLen > v4_single_frame_cap(dst)) {
    const unsigned frags = (payloadLen + kFragment - 1) / kFragment;
    for (unsigned i = 0; i < frags; ++i) {
      gClock += gAckDelayMs;
      if (gProducerHook) gProducerHook();
    }
  }
  return gRadioOk;
}
bool v4_send_frame(const uint8_t*, uint8_t, uint16_t, uint32_t, const uint8_t*, uint16_t, uint8_t) {
  return true;
}

// INSERT_DISPATCHER

namespace espnowtx {
// The dispatcher's turn: drain whatever is queued, as txTask would.
void harnessDrain() {
  Job job;
  while (sJobQueue && xQueueReceive(sJobQueue, &job, 0) == pdTRUE) {
    TaskHandle_t prev = gCurrentTask;
    gCurrentTask = DISPATCHER;
    runJob(job);
    gCurrentTask = prev;
  }
}
}  // namespace espnowtx

static unsigned gProducerSubmits = 0, gProducerRefused = 0;
// Two fire-and-forget 100 B jobs per ACK wait (sensor envelopes, heartbeats).
// The dispatcher drains only when it is not the task sitting in the wait.
static void producers() {
  uint8_t line[100] = {1};
  for (int i = 0; i < 2; ++i) {
    ++gProducerSubmits;
    if (!espnowtx::sendAead(kDirect, 7, 0, 1000 + gProducerSubmits, line, sizeof line, 1)) {
      ++gProducerRefused;
    }
  }
  if (gCurrentTask != DISPATCHER) espnowtx::harnessDrain();
}

static const char* taskName(TaskHandle_t t) {
  return t == CALLER ? "CALLER(cmd_exec)" : t == HTTPD ? "HTTPD" : t == DISPATCHER ? "DISPATCHER" : "none";
}

int main() {
  gOnBlock = espnowtx::harnessDrain;
  std::vector<uint8_t> big(4096, 'x');
  espnowtx::Stats s = {};

  // F: before init() (lifecycle STOPPED) a multi-frame sync send is refused
  //    without touching the radio; the payload copy is still released (ASan).
  gRadio.clear();
  CHECK(!espnowtx::sendAeadSync(kDirect, 9, 0, 7, big.data(), (uint16_t)big.size(), 1, 1000));
  CHECK(gRadio.empty());

  CHECK(espnowtx::init());

  // A: a 4 KB synchronous send (21 fragments) from cmd_exec runs its fragment
  //    loop on cmd_exec, and the 42 fire-and-forget jobs submitted during its
  //    ACK waits all get through.
  CHECK(gCurrentTask == gCmdExecTaskHandle);
  gProducerHook = producers;
  gRadio.clear();
  const bool okA = espnowtx::sendAeadSync(kDirect, 9, 0, 1, big.data(), (uint16_t)big.size(), 1, 3000);
  gProducerHook = nullptr;
  espnowtx::getStats(&s);
  const TaskHandle_t bigTask = gRadio.empty() ? nullptr : gRadio.front().task;
  std::printf("A: 4096 B sendAeadSync ran on %s; producers submitted=%u refused=%u; "
              "stats dropped=%u sent=%u\n",
              taskName(bigTask), gProducerSubmits, gProducerRefused,
              (unsigned)s.dropped, (unsigned)s.sent);
  CHECK(okA);
  CHECK(!gRadio.empty() && gRadio.front().len == 4096);
  if (bigTask != CALLER) {
    std::fprintf(stderr, "FAIL A: cmd_exec's multi-frame send ran on the espnow_tx dispatcher, not on cmd_exec\n");
    ++failures;
  }
  if (gProducerRefused != 0 || s.dropped != 0) {
    std::fprintf(stderr, "FAIL A: %u fire-and-forget jobs were dropped (queue full) during the fragment ACK waits\n",
                 gProducerRefused);
    ++failures;
  }
  CHECK(gRadio.size() == 1 + gProducerSubmits);
  CHECK(s.submitted == s.sent + s.failed + s.dropped);

  // B: a single-frame synchronous send still goes through the dispatcher.
  gRadio.clear();
  uint8_t small[100] = {2};
  CHECK(espnowtx::sendAeadSync(kDirect, 9, 0, 2, small, sizeof small, 1, 1000));
  CHECK(gRadio.size() == 1 && gRadio.front().task == DISPATCHER);

  // C: the destination's cap decides: 150 B from cmd_exec to a routed peer
  //    (cap 130) runs on cmd_exec, the same 150 B to a direct peer (cap 202) is
  //    dispatcher work.
  gRadio.clear();
  uint8_t mid[150] = {3};
  CHECK(espnowtx::sendAeadSync(kRouted, 9, 0, 3, mid, sizeof mid, 1, 1000));
  std::printf("C: 150 B from cmd_exec to a routed peer ran on %s\n",
              gRadio.empty() ? "nothing" : taskName(gRadio.front().task));
  if (!(gRadio.size() == 1 && gRadio.front().task == CALLER)) {
    std::fprintf(stderr, "FAIL C: cmd_exec's payload over the routed single-frame cap did not run on cmd_exec\n");
    ++failures;
  }
  gRadio.clear();
  CHECK(espnowtx::sendAeadSync(kDirect, 9, 0, 4, mid, sizeof mid, 1, 1000));
  CHECK(gRadio.size() == 1 && gRadio.front().task == DISPATCHER);
  // Exactly at the cap is still one frame.
  gRadio.clear();
  uint8_t atCap[202] = {4};
  CHECK(espnowtx::sendAeadSync(kDirect, 9, 0, 5, atCap, sizeof atCap, 1, 1000));
  CHECK(gRadio.size() == 1 && gRadio.front().task == DISPATCHER);

  // I: another task's synchronous multi-frame job stays dispatcher work. The FS
  //    RPC requests (LIST 144 B) are sent from httpd and the OLED loop, whose
  //    stacks are not sized for the send path; to a routed peer (cap 130) that
  //    is 2 fragments, queued and sent by the dispatcher with the real outcome.
  gCurrentTask = HTTPD;
  gRadio.clear();
  espnowtx::getStats(&s);
  const uint32_t sentBeforeI = s.sent;
  uint8_t fsListReq[144] = {6};
  CHECK(espnowtx::sendAeadSync(kRouted, 9, 0, 12, fsListReq, sizeof fsListReq, 1, 2000));
  std::printf("I: 144 B from httpd to a routed peer ran on %s\n",
              gRadio.empty() ? "nothing" : taskName(gRadio.front().task));
  if (!(gRadio.size() == 1 && gRadio.front().task == DISPATCHER && gRadio.front().len == 144)) {
    std::fprintf(stderr, "FAIL I: a non-cmd_exec task's multi-frame sync send ran on that task's stack, "
                         "not on the espnow_tx dispatcher\n");
    ++failures;
  }
  espnowtx::getStats(&s);
  CHECK(s.sent == sentBeforeI + 1);
  gCurrentTask = CALLER;

  // D: a failed multi-frame send reports failure and is counted exactly once.
  gRadio.clear();
  gRadioOk = false;
  espnowtx::getStats(&s);
  const uint32_t failedBefore = s.failed, submittedBefore = s.submitted, sentBefore = s.sent;
  CHECK(!espnowtx::sendAeadSync(kDirect, 9, 0, 6, big.data(), (uint16_t)big.size(), 1, 1000));
  espnowtx::getStats(&s);
  CHECK(s.failed == failedBefore + 1);
  CHECK(s.submitted == submittedBefore + 1);
  CHECK(s.sent == sentBefore);
  CHECK(gRadio.size() == 1);
  gRadioOk = true;

  // E: the dispatcher itself may not call submitSync, whatever the size (the
  //    self-deadlock guard precedes the multi-frame branch).
  gCurrentTask = DISPATCHER;
  gRadio.clear();
  CHECK(!espnowtx::sendAeadSync(kDirect, 9, 0, 7, big.data(), (uint16_t)big.size(), 1, 1000));
  CHECK(!espnowtx::sendAeadSync(kDirect, 9, 0, 8, small, sizeof small, 1, 1000));
  CHECK(gRadio.empty());
  gCurrentTask = CALLER;

  // G: fire-and-forget submit() is unchanged: an over-cap sendAead is still
  //    queued and sent by the dispatcher (producers stay single-frame by their
  //    own caps; this module does not refuse them).
  gRadio.clear();
  uint8_t twoFrag[210] = {5};
  CHECK(espnowtx::sendAead(kDirect, 7, 0, 9, twoFrag, sizeof twoFrag, 1));
  CHECK(gRadio.empty());
  espnowtx::harnessDrain();
  CHECK(gRadio.size() == 1 && gRadio.front().task == DISPATCHER && gRadio.front().len == 210);

  // H: stop() with a queued job fails and frees it, then a multi-frame sync
  //    send is refused like any other job (lifecycle STOPPED).
  gRadio.clear();
  espnowtx::getStats(&s);
  const uint32_t failedBeforeStop = s.failed;
  CHECK(espnowtx::sendAead(kDirect, 7, 0, 10, small, sizeof small, 1));
  CHECK(espnowtx::stop(1000));
  espnowtx::getStats(&s);
  CHECK(s.failed == failedBeforeStop + 1);
  CHECK(!espnowtx::sendAeadSync(kDirect, 9, 0, 11, big.data(), (uint16_t)big.size(), 1, 1000));
  CHECK(gRadio.empty());

  CHECK(gCritical == 0);
  std::printf("dispatcher unit: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}

#elif defined(HW1_UNIT_STREAM)
// ============================================================================
// Unit 2: STREAM line sender
// ============================================================================
static int gOtherToken;
static TaskHandle_t const OTHER_TASK = &gOtherToken;
class String {
 public:
  String(const char* s = "") : value_(s ? s : "") {}
  explicit String(const std::string& s) : value_(s) {}
  size_t length() const { return value_.size(); }
  const char* c_str() const { return value_.c_str(); }

 private:
  std::string value_;
};
String redactOutputForLog(const String& message) { return message; }

#define DEBUG_ESPNOW_STREAM 1
#define DEBUGF(level, ...) debugSink(level, __VA_ARGS__)

// Mirrors System_ESPNow_Wire.h; the Python side checks the header still says so.
#define ESPNOW_V4_MAX_PAYLOAD (250 - 32)
#define ESPNOW_V4_AEAD_TAG_LEN 16
#define ESPNOW_V4_MAX_PLAINTEXT (ESPNOW_V4_MAX_PAYLOAD - ESPNOW_V4_AEAD_TAG_LEN)
#define ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT 130
static constexpr uint8_t ESPNOW_V4_TYPE_STREAM = 90;

struct EspNowState {
  bool initialized;
  bool streamingSuspended;
  bool streamActive;
  uint8_t streamTarget[6];
  unsigned long lastStreamSendTime;
  uint32_t streamDroppedCount;
  uint32_t streamSentCount;
};
static EspNowState gState = {};
EspNowState* gEspNow = &gState;
TaskHandle_t gCmdExecTaskHandle = CALLER;

// Per-task command context (System_AuthIdentity's TLS slot): CALLER plays
// cmd_exec_task, OTHER_TASK any other task. The two fields the stream owner
// reads mirror System_CommandTypes.h (the driver checks that they still exist).
enum CommandOrigin { ORIGIN_SERIAL, ORIGIN_WEB, ORIGIN_SYSTEM, ORIGIN_ESPNOW };
struct CommandContext {
  CommandOrigin origin;
  uint32_t id;
};
static CommandContext gExecCtx = {};
static bool gExecHasCtx = false;
void* currentCommandContext() {
  return (gCurrentTask == CALLER && gExecHasCtx) ? &gExecCtx : nullptr;
}

struct SendCall {
  uint8_t mac[6];
  uint8_t type;
  uint16_t flags;
  uint32_t msgId;
  uint16_t len;
};
static std::vector<SendCall> gSends;
namespace espnowtx {
bool sendAead(const uint8_t mac[6], uint8_t type, uint16_t flags, uint32_t msgId,
              const uint8_t*, uint16_t payloadLen, uint8_t) {
  SendCall c = {};
  std::memcpy(c.mac, mac, 6);
  c.type = type;
  c.flags = flags;
  c.msgId = msgId;
  c.len = payloadLen;
  gSends.push_back(c);
  return true;
}
}  // namespace espnowtx

// INSERT_STREAM

static String line(size_t n) { return String(std::string(n, 'L')); }

static void resetStream() {
  gSends.clear();
  gState = {};
  gState.initialized = true;
#ifdef HW1_STREAM_GLOBAL_ID
  gCurrentStreamCmdId = 0;
#endif
  gCurrentTask = CALLER;
  gExecHasCtx = false;
  for (StreamSession& s : gStreamSessions) s = {};
  gClock += 1000;  // clear the legacy rate limit
}

// What v4_handle_cmd does on espnow_task when it accepts a request: open the
// request's stream session keyed by its msgId and queue the command with
// origin ORIGIN_ESPNOW, ctx.id = msgId. A pre-fix source also published the
// msgId in one global the sender read, set at queue time.
static void queueEspNowRequest(int slot, uint32_t msgId, const uint8_t* mac) {
  gStreamSessions[slot].active = true;
  gStreamSessions[slot].cmdMsgId = msgId;
  std::memcpy(gStreamSessions[slot].targetMac, mac, 6);
#ifdef HW1_STREAM_GLOBAL_ID
  gCurrentStreamCmdId = msgId;
#endif
}

// What commandExecTask does around one command: install its context on
// cmd_exec for the run (execIdle: between commands, or a deferred job).
static void runOnExec(CommandOrigin origin, uint32_t id) {
  gCurrentTask = CALLER;
  gExecCtx = {origin, id};
  gExecHasCtx = true;
}
static void execIdle() { gExecHasCtx = false; }

// broadcastOutputCore's question before it copies a line for the sender.
static bool streamGate() {
#ifdef HW1_STREAM_GATE
  return espnowStreamOutputWanted();
#else
  return gCurrentStreamCmdId != 0;  // the pre-fix System_Debug gate
#endif
}

static void expectOneSend(const char* label, const uint8_t* mac, uint32_t msgId, uint16_t expectLen) {
  std::printf("%s: sent %zu frame(s), len=%u (expect %u)\n", label, gSends.size(),
              gSends.empty() ? 0u : (unsigned)gSends.front().len, (unsigned)expectLen);
  CHECK(gSends.size() == 1);
  if (gSends.empty()) return;
  const SendCall& c = gSends.front();
  CHECK(std::memcmp(c.mac, mac, 6) == 0);
  CHECK(c.type == ESPNOW_V4_TYPE_STREAM);
  CHECK(c.msgId == msgId);
  if (c.len != expectLen) {
    std::fprintf(stderr,
                 "FAIL %s: STREAM line sent with %u B, over the %u B single-frame budget for its target "
                 "(a fire-and-forget line must never fragment on the dispatcher)\n",
                 label, (unsigned)c.len, (unsigned)expectLen);
    ++failures;
  }
}

int main() {
  // S1: legacy stream (startstream) to a direct target: a 400-char line is
  //     clamped to one frame (202 B); to a routed target, 130 B.
  resetStream();
  gState.streamActive = true;
  std::memcpy(gState.streamTarget, kDirect, 6);
  sendEspNowStreamMessage(line(400));
  expectOneSend("S1 legacy direct", kDirect, 0, 202);
  CHECK(gState.streamSentCount == 1);

  resetStream();
  gState.streamActive = true;
  std::memcpy(gState.streamTarget, kRouted, 6);
  sendEspNowStreamMessage(line(400));
  expectOneSend("S1 legacy routed", kRouted, 0, 130);

  // S2: command-session stream from cmd_exec: direct 202 B, routed 130 B.
  for (int routed = 0; routed < 2; ++routed) {
    resetStream();
    queueEspNowRequest(1, 42, routed ? kRouted : kDirect);
    runOnExec(ORIGIN_ESPNOW, 42);
    sendEspNowStreamMessage(line(400));
    expectOneSend(routed ? "S2 session routed" : "S2 session direct",
                  routed ? kRouted : kDirect, 42, routed ? 130 : 202);
  }

  // S3: short lines pass whole on both paths.
  resetStream();
  gState.streamActive = true;
  std::memcpy(gState.streamTarget, kRouted, 6);
  sendEspNowStreamMessage(line(50));
  expectOneSend("S3 legacy short", kRouted, 0, 50);
  resetStream();
  queueEspNowRequest(0, 7, kDirect);
  runOnExec(ORIGIN_ESPNOW, 7);
  sendEspNowStreamMessage(line(50));
  expectOneSend("S3 session short", kDirect, 7, 50);

  // S4: the legacy rate limit still drops a second line within 100 ms.
  resetStream();
  gState.streamActive = true;
  std::memcpy(gState.streamTarget, kDirect, 6);
  sendEspNowStreamMessage(line(20));
  sendEspNowStreamMessage(line(20));
  CHECK(gSends.size() == 1);
  CHECK(gState.streamDroppedCount == 1);

  // S5: session output only from cmd_exec: another task's line falls through
  //     to the legacy stream (inactive here, so nothing is sent).
  resetStream();
  queueEspNowRequest(0, 42, kDirect);
  runOnExec(ORIGIN_ESPNOW, 42);
  gCurrentTask = OTHER_TASK;
  sendEspNowStreamMessage(line(40));
  CHECK(gSends.empty());
  gCurrentTask = CALLER;

  // S6: suspended streaming sends nothing.
  resetStream();
  gState.streamActive = true;
  gState.streamingSuspended = true;
  std::memcpy(gState.streamTarget, kDirect, 6);
  sendEspNowStreamMessage(line(40));
  CHECK(gSends.empty());

  // A9: the session is the one of the command cmd_exec is running, from its
  // context, not the request that was queued last.
  // S7: peer A's request 42 (direct) and then peer B's 43 (routed) are queued;
  //     A's output goes to A, then B's output goes to B.
  resetStream();
  queueEspNowRequest(0, 42, kDirect);
  queueEspNowRequest(1, 43, kRouted);
  runOnExec(ORIGIN_ESPNOW, 42);
  sendEspNowStreamMessage(line(40));
  expectOneSend("S7 first requester's output", kDirect, 42, 40);
  gSends.clear();
  runOnExec(ORIGIN_ESPNOW, 43);
  sendEspNowStreamMessage(line(40));
  expectOneSend("S7 second requester's output", kRouted, 43, 40);

  // S8: a local (web) command runs on cmd_exec while request 42 waits in the
  //     queue, even with a colliding millis() id: nothing streams to the peer.
  resetStream();
  queueEspNowRequest(0, 42, kDirect);
  runOnExec(ORIGIN_WEB, 42);
  sendEspNowStreamMessage(line(40));
  if (!gSends.empty()) std::fprintf(stderr, "FAIL S8: a web command's output streamed to a queued ESP-NOW requester\n");
  CHECK(gSends.empty());
  CHECK(!streamGate());

  // S9: a deferred job on cmd_exec (no command context) while request 42 waits.
  resetStream();
  queueEspNowRequest(0, 42, kDirect);
  execIdle();
  sendEspNowStreamMessage(line(40));
  if (!gSends.empty()) std::fprintf(stderr, "FAIL S9: a deferred job's output streamed to a queued ESP-NOW requester\n");
  CHECK(gSends.empty());
  CHECK(!streamGate());

  // S10: broadcastOutputCore's gate. Open for the ESP-NOW command cmd_exec
  //      runs; closed for other tasks and other origins while a request is only
  //      queued; the legacy startstream sink still hears every task while a
  //      remote command is in flight, and nothing when none is.
  resetStream();
  queueEspNowRequest(0, 42, kDirect);
  runOnExec(ORIGIN_ESPNOW, 42);
  CHECK(streamGate());
  gCurrentTask = OTHER_TASK;
  if (streamGate()) std::fprintf(stderr, "FAIL S10: another task's line passes the stream gate while a request is queued\n");
  CHECK(!streamGate());
  gState.streamActive = true;
  std::memcpy(gState.streamTarget, kRouted, 6);
  CHECK(streamGate());
  sendEspNowStreamMessage(line(40));
  expectOneSend("S10 legacy sink while a remote command is in flight", kRouted, 0, 40);
  resetStream();
  gState.streamActive = true;
  std::memcpy(gState.streamTarget, kRouted, 6);
  gCurrentTask = OTHER_TASK;
  CHECK(!streamGate());
  gCurrentTask = CALLER;

  std::printf("stream unit: %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}

#else
#error "define HW1_UNIT_DISPATCHER or HW1_UNIT_STREAM"
#endif
