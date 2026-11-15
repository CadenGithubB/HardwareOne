// Runs the command submit functions (submitAndExecuteSync, submitCommandAsync,
// releaseSyncExecReqOwner from System_Utils.cpp) and the command-result fan-out
// (broadcastOutput from HardwareOne.cpp, with the production
// redactOutputForLog) as shipped, on the production command types and the
// real Arduino String with its heap calls counted. The trace macros record a
// line only while the trace is switched on and evaluate their arguments only
// then, like DEBUGF_QUEUE_DEBUG. The command redactor is a counting double;
// the queue, semaphores, executor, line queue and BLE sender are doubles.
#include <Arduino.h>
#include "string_heap_count.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

static unsigned checks = 0;
static void checked(bool ok, const char* text, int line) {
  ++checks;
  if (ok) return;
  std::fprintf(stderr, "command trace copies check failed at line %d: %s\n", line, text);
  std::exit(1);
}
#define check(condition) checked((condition), #condition, __LINE__)

using SemaphoreHandle_t = void*;
using QueueHandle_t = void*;
using TaskHandle_t = void*;
using BaseType_t = int;
static constexpr BaseType_t pdTRUE = 1;
static constexpr BaseType_t pdFALSE = 0;
#define pdMS_TO_TICKS(ms) (ms)
#define configASSERT(x) check(x)
static inline uint32_t millis() { return 0; }
#include "System_CommandTypes.h"

#define ENABLE_HTTP_SERVER 1
#define MSG_ROUTE_SERIAL 0x01
#define MSG_ROUTE_WEB 0x02
#define MSG_ROUTE_FILE 0x04
#define MSG_ROUTE_OLED 0x08
#define MSG_ROUTE_BLE 0x10
#define MSG_ROUTE_G2 0x20

// ---------------------------------------------------------------------------
// Trace and error lines.
// ---------------------------------------------------------------------------
static bool traceOn = false;
static std::vector<std::string> traces, errors, broadcasts;
static std::string format(const char* fmt, ...) {
  char out[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(out, sizeof(out), fmt, args);
  va_end(args);
  return out;
}
#define DEBUG_CMD_FLOWF(...) do { if (traceOn) traces.push_back(format(__VA_ARGS__)); } while (0)
#define ERROR_COMMANDF(...) errors.push_back(format(__VA_ARGS__))
#define ERROR_MEMORYF(...) errors.push_back(format(__VA_ARGS__))

// ---------------------------------------------------------------------------
// Doubles.
// ---------------------------------------------------------------------------
static unsigned redactions = 0;
String redactCmdForAudit(const String& line) {
  ++redactions;
  return String("<redacted:") + line + ">";
}
void broadcastOutput(const char* s) { broadcasts.push_back(s); }
struct EspDouble {
  uint32_t getFreeHeap() const { return 1000; }
  uint32_t getFreePsram() const { return 2000; }
} ESP;
enum class AllocPref { PreferPsram };
static void* ps_alloc(size_t size, AllocPref, const char*) { return std::malloc(size); }
bool gCliValidateOnly = false;
static void setCurrentCommandContext(void*) {}
static void clearCurrentCommandContext() {}
static bool executeCommand(AuthContext&, const char*, char* out, size_t size) {
  std::snprintf(out, size, "%s", "direct");
  return true;
}
static int semaphoreToken = 0;
static SemaphoreHandle_t xSemaphoreCreateBinary() { return &semaphoreToken; }
static void vSemaphoreDelete(SemaphoreHandle_t done) { check(done == &semaphoreToken); }
static const char* pcTaskGetName(void*) { return "loopTask"; }
static std::atomic<int> gWebCmdWaiters{0};

static int queueToken = 0;
QueueHandle_t gCmdExecQ = &queueToken;
// The executor double: queueAccepts false is a full queue; otherwise a sync
// request completes at once (result "done", ok) unless the caller is to time
// out, and an async request is kept for the test to finish.
static struct {
  bool queueAccepts = true;
  bool timeout = false;
  ExecReq* asyncRequest = nullptr;
} exec;
static BaseType_t xQueueSend(QueueHandle_t, const CmdExecItem* item, int) {
  if (!exec.queueAccepts) return pdFALSE;
  ExecReq* r = item->req;
  if (r->done) {
    std::snprintf(r->out, sizeof(r->out), "%s", "done");
    r->ok = true;
    if (!exec.timeout) releaseSyncExecReqOwner(r);  // cmd_exec's release
    else exec.asyncRequest = r;                      // released after the wait
  } else {
    exec.asyncRequest = r;
  }
  return pdTRUE;
}
static BaseType_t taskWdtAwareSemaphoreTake(SemaphoreHandle_t, int) {
  return exec.timeout ? pdFALSE : pdTRUE;
}

// broadcastOutput's boundaries.
static String originPrefix(const char* source, const String&, const String&) {
  return String("[") + source + "] ";
}
static std::vector<std::string> queued;
static void broadcastCommandResultQueued(const String&, const String& body, uint8_t) {
  queued.push_back(body.c_str());
}
static unsigned bleSends = 0;
static bool sendBleResponseToSession(uint16_t, TransportSessionEpoch, const char*, size_t, bool = true) {
  ++bleSends;
  return true;
}

// INSERT_PRODUCTION

static void reset() {
  traces.clear(); errors.clear(); broadcasts.clear(); queued.clear();
  redactions = 0; bleSends = 0;
  exec = {};
  gStringHeap = StringHeapCount{};
}

static Command command(const char* line) {
  Command cmd;
  cmd.line = line;
  cmd.ctx.origin = ORIGIN_SERIAL;
  cmd.ctx.auth.user = "alice";
  return cmd;
}

static void submitCases() {
  static const char kLine[] = "wifiadd example-network example-secret";
  const std::string redacted = std::string("<redacted:") + kLine + ">";
  const Command cmd = command(kLine);

  // Trace off, the command completes: no redaction at all (before: one per
  // submit, made and dropped).
  reset();
  String out;
  bool completed = false;
  check(submitAndExecuteSync(cmd, out, &completed));
  check(completed && std::strcmp(out.c_str(), "done") == 0);
  check(redactions == 0 && traces.empty() && errors.empty());

  // Trace on: the same three lines as before, with one redaction.
  reset();
  traceOn = true;
  check(submitAndExecuteSync(cmd, out, &completed));
  traceOn = false;
  check(redactions == 1);
  check(traces.size() == 3);
  check(traces[0] == format("[submitSync] cmd='%.80s' origin=%d user='%s'", redacted.c_str(),
                            (int)ORIGIN_SERIAL, "alice"));
  check(traces[1] == format("[submitSync] queued '%.40s' waiting...", redacted.c_str()));
  check(traces[2] == "[submitSync] done ok=1 len=4");

  // A full queue, trace on: its line carries the redacted command.
  reset();
  traceOn = true;
  exec.queueAccepts = false;
  check(!submitAndExecuteSync(cmd, out, &completed));
  traceOn = false;
  check(redactions == 1 && traces.size() == 2);
  check(traces[1] == format("[submitSync] queue full for '%.40s'", redacted.c_str()));
  check(broadcasts.size() == 1 && broadcasts[0] == "[ERROR] Command queue full - try again");
  // The same, trace off: no redaction.
  reset();
  exec.queueAccepts = false;
  check(!submitAndExecuteSync(cmd, out, &completed));
  check(redactions == 0 && broadcasts.size() == 1);

  // A timeout, trace off: the error line names the redacted command, made
  // for it alone.
  reset();
  exec.timeout = true;
  check(!submitAndExecuteSync(cmd, out, &completed));
  check(std::strcmp(out.c_str(), "[ERROR] Command timed out") == 0);
  check(redactions == 1 && traces.empty() && errors.size() == 1);
  check(errors[0] == format("sync command timed out after 60000 ms; command execution may continue; "
                            "line='%.60s'", redacted.c_str()));
  releaseSyncExecReqOwner(exec.asyncRequest);  // cmd_exec's late release
  // A timeout, trace on: the traces and the error share one redaction.
  reset();
  traceOn = true;
  exec.timeout = true;
  check(!submitAndExecuteSync(cmd, out, &completed));
  traceOn = false;
  check(redactions == 1 && traces.size() == 2 && errors.size() == 1);
  check(errors[0] == format("sync command timed out after 60000 ms; command execution may continue; "
                            "line='%.60s'", redacted.c_str()));
  releaseSyncExecReqOwner(exec.asyncRequest);

  // Async: no redaction with the trace off; the same entry line with it on.
  for (bool on : {false, true}) {
    reset();
    traceOn = on;
    check(submitCommandAsync(cmd, nullptr, nullptr));
    traceOn = false;
    check(exec.asyncRequest && std::strcmp(exec.asyncRequest->line, kLine) == 0);
    exec.asyncRequest->~ExecReq();
    std::free(exec.asyncRequest);
    check(redactions == (on ? 1u : 0u));
    if (on) {
      check(traces.size() == 2);
      check(traces[0] == format("[submitAsync] enter: cmd.line='%s'", redacted.c_str()));
      check(traces[1] == "[submitAsync] Command queued successfully");
    }
  }
}

static void broadcastCases() {
  // A result past the inline String capacity, with a session id and a
  // password value the trace redaction rewrites.
  const String result(
      "{\"user\":\"alice\",\"sid\":\"0123456789abcdef0123\",\"password\":\"hash-value\","
      "\"detail\":\"a result long enough to need its own heap block\"}");
  CommandContext ctx;
  ctx.origin = ORIGIN_WEB;
  ctx.auth.user = "alice";
  ctx.outputMask = MSG_ROUTE_WEB | MSG_ROUTE_FILE;

  // Trace off: the result is queued as it is, and no copy of it is made
  // (before: one full copy, plus the redaction's own blocks).
  reset();
  broadcastOutput(result, ctx);
  check(queued.size() == 1 && queued[0] == result.c_str());
  std::printf("broadcast, trace off: %lu String blocks\n", gStringHeap.made);
  check(gStringHeap.made == 0);

  // Trace on: the trace shows the first 50 bytes of the redacted copy, as before.
  reset();
  traceOn = true;
  broadcastOutput(result, ctx);
  traceOn = false;
  check(queued.size() == 1 && queued[0] == result.c_str());
  check(traces.size() == 2);
  const String safe = redactOutputForLog(result);
  check(std::strstr(safe.c_str(), "hash-value") == nullptr);
  check(traces[0] == format("[BROADCAST_CTX] origin=%s user=%s mask=0x%02lX msg='%.50s'", "web", "alice",
                            (unsigned long)ctx.outputMask, safe.c_str()));
  check(traces[1] == format("[broadcast] route=0x%02X len=%d",
                            MSG_ROUTE_WEB | MSG_ROUTE_FILE | MSG_ROUTE_OLED | MSG_ROUTE_G2,
                            (int)result.length()));

  // A private STT result: the shared copy is the placeholder, with the trace
  // off and on; the trace shows the placeholder too.
  const String stt("{\"state\":\"done\",\"sttText\":\"a private sentence spoken to the device\"}");
  for (bool on : {false, true}) {
    reset();
    traceOn = on;
    broadcastOutput(stt, ctx);
    traceOn = false;
    check(queued.size() == 1 && queued[0] == "[private STT result]");
    if (on) {
      check(traces[0] == format("[BROADCAST_CTX] origin=%s user=%s mask=0x%02lX msg='%.50s'", "web",
                                "alice", (unsigned long)ctx.outputMask, "[private STT result]"));
    }
  }

  // A Bluetooth session still gets the original result directly.
  reset();
  ctx.origin = ORIGIN_BLUETOOTH;
  ctx.auth.sid = "3";
  ctx.transportSessionEpoch = 7;
  ctx.outputMask = MSG_ROUTE_BLE;
  broadcastOutput(stt, ctx);
  check(bleSends == 1 && queued.size() == 1 && queued[0] == "[private STT result]");
}

int main() {
  submitCases();
  broadcastCases();
  std::printf("command trace copies passed (%u checks)\n", checks);
  return 0;
}
