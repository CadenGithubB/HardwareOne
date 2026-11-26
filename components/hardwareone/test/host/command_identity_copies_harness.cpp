// Runs the command-result fan-out (broadcastOutput and deliverCommandResult
// from HardwareOne.cpp, with the production redactOutputForLog) and the task
// identity slot (TaskIdentity, getOrCreateSlot and ExecIdentityGuard from
// System_AuthIdentity.cpp) as shipped, on the production command types and
// the real Arduino String with its heap calls counted. The line queue, the
// BLE sender, the serial port, the session registry and FreeRTOS's task-local
// storage are doubles that record what they are handed.
//
//   transcript <file>   every delivery case: what reached each sink, with the
//                       route of the queued copy and the traces
//   counts              String blocks of one delivery and of identity installs
#include <Arduino.h>
#include "string_heap_count.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using SemaphoreHandle_t = void*;
using QueueHandle_t = void*;
using TaskHandle_t = void*;
using BaseType_t = int;
static inline uint32_t millis() { return 0; }
#include "System_CommandTypes.h"

#define MSG_ROUTE_SERIAL 0x01
#define MSG_ROUTE_WEB 0x02
#define MSG_ROUTE_FILE 0x04
#define MSG_ROUTE_OLED 0x08
#define MSG_ROUTE_BLE 0x10
#define MSG_ROUTE_G2 0x20

static void fail(const char* what) {
  std::fprintf(stderr, "command identity copies: %s\n", what);
  std::exit(1);
}

// ---------------------------------------------------------------------------
// Sinks. Each records into the case log.
// ---------------------------------------------------------------------------
static std::string caseLog;
static bool traceOn = false;
static std::string format(const char* fmt, ...) {
  char out[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(out, sizeof(out), fmt, args);
  va_end(args);
  return out;
}
static std::string quoted(const char* text, size_t length) {
  std::string out = "\"";
  for (size_t i = 0; i < length; ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += static_cast<char>(ch);
    } else if (ch < 0x20 || ch >= 0x7f) {
      out += format("\\x%02x", ch);
    } else {
      out += static_cast<char>(ch);
    }
  }
  return out + "\"";
}
#define DEBUG_CMD_FLOWF(...) \
  do { if (traceOn) caseLog += "  trace " + format(__VA_ARGS__) + "\n"; } while (0)
#define BROADCAST_PRINTF(...) (caseLog += "  broadcast " + format(__VA_ARGS__) + "\n")

uint32_t gOutputFlags = MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE;

// The session registry: one live epoch per run; a write may end it.
static uint32_t liveEpoch = 7;
static bool revokeDuringWrite = false;
static bool modeOwned = false;
static unsigned short shortWriteBy = 0;
bool transportSessionEpochIsLive(CommandSource, TransportSessionEpoch epoch) {
  return epoch != 0 && epoch == liveEpoch;
}
bool cliModeOwnedBySession(CommandSource, TransportSessionEpoch) { return modeOwned; }
void debugWaitOutputDrained(uint32_t ms) { caseLog += format("  drain %u\n", (unsigned)ms); }
struct SerialDouble {
  size_t write(const uint8_t* data, size_t length) {
    caseLog += "  serial " + quoted(reinterpret_cast<const char*>(data), length) + "\n";
    if (revokeDuringWrite) liveEpoch = 8;
    return length > shortWriteBy ? length - shortWriteBy : 0;
  }
  size_t write(uint8_t ch) {
    const char c = static_cast<char>(ch);
    caseLog += "  serial-byte " + quoted(&c, 1) + "\n";
    return 1;
  }
} Serial;

#define DEBUG_MSG_SIZE 256
#define EXT_RAM_BSS_ATTR
static size_t formatOriginPrefix(char* buf, size_t capacity, const char* source, const String& user,
                                 const String& ip) {
  const int n = std::snprintf(buf, capacity, "[%s] %s@%s ", source, user.c_str(), ip.c_str());
  return n < 0 ? 0 : (size_t)n < capacity ? (size_t)n : capacity - 1;
}
static void broadcastCommandResultQueued(const char* prefix, size_t prefixLen, const String& body,
                                         uint8_t route) {
  caseLog += "  queued route=" + format("0x%02X", route) + " prefix=" +
             quoted(prefix, prefixLen) + " body=" + quoted(body.c_str(), body.length()) + "\n";
}
static bool sendBleResponseToSession(uint16_t conn, TransportSessionEpoch epoch, const char* text,
                                     size_t length, bool = true) {
  caseLog += format("  ble conn=%u epoch=%u ", (unsigned)conn, (unsigned)epoch) + quoted(text, length) + "\n";
  return true;
}

// ---------------------------------------------------------------------------
// FreeRTOS task-local storage double: one task, four slots.
// ---------------------------------------------------------------------------
static int taskToken = 0;
static void* tlsSlots[4] = {};
static TaskHandle_t xTaskGetCurrentTaskHandle() { return &taskToken; }
static void* pvTaskGetThreadLocalStoragePointer(TaskHandle_t, BaseType_t index) { return tlsSlots[index]; }
typedef void (*TlsDeleteCallbackFunction_t)(int, void*);
static void vTaskSetThreadLocalStoragePointerAndDelCallback(TaskHandle_t, BaseType_t index, void* value,
                                                           TlsDeleteCallbackFunction_t) {
  tlsSlots[index] = value;
}

// INSERT_IDENTITY_TYPES

// INSERT_PRODUCTION

// ---------------------------------------------------------------------------
// Delivery cases.
// ---------------------------------------------------------------------------
static const CommandOrigin kOrigins[] = {ORIGIN_SERIAL, ORIGIN_WEB, ORIGIN_BLUETOOTH, ORIGIN_MQTT,
                                         ORIGIN_SYSTEM};
static const char* const kSids[] = {"", "3", "x7"};
static const char* const kResults[] = {
    "", "ordinary result",
    "{\"state\":\"done\",\"sttText\":\"a private sentence spoken to the device\"}"};
enum Liveness { kStateless, kLive, kStale, kRevokedDuringWrite, kNoEpoch };

static int transcript(const char* path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) fail("cannot write the transcript");
  unsigned long cases = 0;
  for (CommandOrigin origin : kOrigins)
    for (uint32_t mask = 0; mask < 64; ++mask)
      for (const char* sid : kSids)
        for (const char* result : kResults)
          for (int validate = 0; validate < 2; ++validate)
            for (int serialOn = 0; serialOn < 2; ++serialOn)
              for (int liveness = kStateless; liveness <= kNoEpoch; ++liveness)
                for (int owned = 0; owned < 2; ++owned)
                  for (int trace = 0; trace < 2; ++trace) {
                    CommandContext ctx;
                    ctx.origin = origin;
                    ctx.auth.transport = origin == ORIGIN_SERIAL ? SOURCE_SERIAL : SOURCE_WEB;
                    ctx.auth.user = "operator";
                    ctx.auth.ip = "local";
                    ctx.auth.path = "serial";
                    ctx.auth.sid = sid;
                    ctx.outputMask = mask;
                    ctx.validateOnly = validate != 0;
                    ctx.transportSessionEpoch = liveness == kNoEpoch ? 0 : 7;
                    if (liveness != kStateless) ctx.behaviorFlags |= COMMAND_CONTEXT_REQUIRE_LIVE_SESSION;
                    liveEpoch = liveness == kStale ? 9 : 7;
                    revokeDuringWrite = liveness == kRevokedDuringWrite;
                    gOutputFlags = MSG_ROUTE_WEB | MSG_ROUTE_FILE | (serialOn ? MSG_ROUTE_SERIAL : 0);
                    modeOwned = owned != 0;
                    traceOn = trace != 0;
                    shortWriteBy = 0;
                    caseLog = format("case origin=%d mask=0x%02X sid=%s result=%zu validate=%d serial=%d "
                                     "live=%d owned=%d trace=%d\n",
                                     (int)origin, (unsigned)mask, sid, std::strlen(result), validate, serialOn,
                                     liveness, owned, trace);
                    deliverCommandResult(String(result), ctx);
                    out << caseLog;
                    caseLog = format("case broadcast origin=%d mask=0x%02X sid=%s result=%zu trace=%d\n",
                                     (int)origin, (unsigned)mask, sid, std::strlen(result), trace);
                    if (validate == 0 && serialOn == 0 && owned == 0 && liveness == kLive) {
                      broadcastOutput(String(result), ctx);
                      out << caseLog;
                    }
                    ++cases;
                  }
  // A short serial write is reported, and the mirror still follows.
  CommandContext ctx;
  ctx.origin = ORIGIN_SERIAL;
  ctx.auth.transport = SOURCE_SERIAL;
  ctx.auth.user = "operator";
  ctx.outputMask = MSG_ROUTE_SERIAL | MSG_ROUTE_FILE;
  ctx.transportSessionEpoch = 7;
  liveEpoch = 7;
  revokeDuringWrite = false;
  modeOwned = false;
  traceOn = false;
  gOutputFlags = MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE;
  shortWriteBy = 3;
  caseLog = "case short write\n";
  deliverCommandResult(String("ordinary result"), ctx);
  out << caseLog;
  shortWriteBy = 0;
  std::printf("command identity copies: %lu delivery cases\n", cases);
  return 0;
}

// ---------------------------------------------------------------------------
// Counts.
// ---------------------------------------------------------------------------
static TaskIdentity* slot() {
  return static_cast<TaskIdentity*>(pvTaskGetThreadLocalStoragePointer(&taskToken, kAuthTlsSlot));
}

static int counts() {
  // A serial command's result: the console write and the mirror. The user
  // and the address are longer than the inline capacity, as an account name
  // and a peer tag can be.
  CommandContext ctx;
  ctx.origin = ORIGIN_SERIAL;
  ctx.auth.transport = SOURCE_SERIAL;
  ctx.auth.user = "a-long-account-name";
  ctx.auth.ip = "a-long-peer-address";
  ctx.auth.path = "serial";
  ctx.outputMask = MSG_ROUTE_SERIAL | MSG_ROUTE_FILE;
  ctx.behaviorFlags |= COMMAND_CONTEXT_REQUIRE_LIVE_SESSION;
  ctx.transportSessionEpoch = 7;
  liveEpoch = 7;
  revokeDuringWrite = false;
  modeOwned = false;
  traceOn = false;
  gOutputFlags = MSG_ROUTE_SERIAL | MSG_ROUTE_WEB | MSG_ROUTE_FILE;
  const String result("ok");
  gStringHeap = StringHeapCount{};
  deliverCommandResult(result, ctx);
  std::printf("%lu serial delivery\n", gStringHeap.made);

  // A result past the inline capacity: the console write adds no copy of it.
  const String longResult("a result longer than the inline capacity");
  gStringHeap = StringHeapCount{};
  deliverCommandResult(longResult, ctx);
  std::printf("%lu serial delivery, long result\n", gStringHeap.made);

  // The same context through broadcastOutput: the mirror's own blocks.
  gStringHeap = StringHeapCount{};
  broadcastOutput(result, ctx);
  std::printf("%lu broadcast\n", gStringHeap.made);

  // The identity slot: a web command's identity (a 32-character session id
  // and a 16-character path) installed and restored on one task, five times.
  AuthContext web;
  web.transport = SOURCE_WEB;
  web.user = "alice";
  web.ip = "web-peer";
  web.path = "/api/cli/execute";
  web.sid = "0123456789abcdef0123456789abcdef";
  for (int command = 1; command <= 5; ++command) {
    gStringHeap = StringHeapCount{};
    {
      ExecIdentityGuard guard(web);
      if (!slot() || slot()->ctx.sid != web.sid || slot()->user != web.user) fail("the identity is installed");
    }
    if (slot()->ctx.sid.length() || slot()->ctx.path.length() || slot()->user.length()) {
      fail("the identity is restored");
    }
    std::printf("%lu identity install %d (freed %lu)\n", gStringHeap.made, command, gStringHeap.freed);
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "transcript") == 0) return transcript(argv[2]);
  if (argc >= 2 && std::strcmp(argv[1], "counts") == 0) return counts();
  fail("usage: transcript <file> | counts");
  return 1;
}
