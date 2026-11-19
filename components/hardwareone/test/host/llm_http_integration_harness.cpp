// Platform/transport mocks around the complete production registry and chat code.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <strings.h>
#include "Arduino.h"
#include <ArduinoJson.h>

size_t testStrlcpy(char* out, const char* text, size_t cap) {
  const size_t length = std::strlen(text);
  if (cap) { const size_t count = std::min(length, cap - 1);
    std::memcpy(out, text, count); out[count] = '\0'; }
  return length;
}
#define strlcpy testStrlcpy
#define EXT_RAM_BSS_ATTR
#define pdTRUE true
#define pdMS_TO_TICKS(ms) (ms)
using SemaphoreHandle_t = std::mutex*;
SemaphoreHandle_t xSemaphoreCreateMutex() {
  static std::mutex mutexes[8];
  static size_t count = 0;
  assert(count < 8);
  return &mutexes[count++];
}
bool xSemaphoreTake(SemaphoreHandle_t mutex, unsigned) { return mutex->try_lock(); }
void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
void testLog(const char*, ...) {}
#define DEBUG_UART_CONTROLF(...) testLog(__VA_ARGS__)
#define DEBUG_SYSTEMF(...) testLog(__VA_ARGS__)
#define ERROR_SYSTEMF(...) testLog(__VA_ARGS__)
void webBatchTestStringAppend() {}
[[maybe_unused]] constexpr int RF_LLM = 1;
bool ramFlushResolve(int, bool requested) { return requested; }
enum class AllocPref { PreferPsram };
void* ps_realloc(void* memory, size_t size, AllocPref) { return std::realloc(memory, size); }
static uint32_t testNow = 1000;
uint32_t millis() { return testNow; }

// INSERT_PRODUCTION_HEADERS

struct TestSettings {
  bool llmEnabled = true;
  bool llmAutoStart = false;
  String llmDefaultModel;
  int llmMaxTokens = 256;
  float llmTemperature = 0.5f;
  float llmTopP = 0.8f;
  float llmMinP = 0.0f;
  float llmRepPenalty = 1.3f;
  int llmRepWindow = 32;
  int llmSentenceLimit = 2;
  int llmHardCap = 80;
} gSettings;

struct MockTransport {
  bool selected = false;
  bool done = true;
  bool resetAllowed = true;
  int starts = 0;
  int stops = 0;
  int resets = 0;
  int session = 0;
  std::string output;
  std::string error;
  std::string prompt;
};
MockTransport httpTransport, cm5Transport;

// The HTTP source as the backend sees it: configured (available) or not, and
// whether a request path is up; resolves counts the autostart's attempts.
bool httpAvailable = true, httpPathUp = true;
int httpResolves = 0;
bool mockFind(const char* name, LlmModelDesc* out, LlmBackendKind kind, const char* prefix) {
  if (!name || !name[0] || !out) return false;
  *out = {};
  out->backend = kind;
  out->available = kind != LlmBackendKind::Http || httpAvailable;
  out->storage = LLM_STORAGE_REMOTE;
  std::snprintf(out->id, sizeof(out->id), "%s:%s", prefix, name);
  std::snprintf(out->name, sizeof(out->name), "%s", name);
  return true;
}
bool mockSelect(MockTransport& transport) {
  if (!transport.done) return false;
  transport.selected = true;
  transport.error.clear();
  return true;
}
LlmStatus mockStatus(MockTransport& transport, const char* name) {
  LlmStatus status{};
  status.state = !transport.selected ? LlmState::UNLOADED :
                 !transport.done ? LlmState::GENERATING : LlmState::READY;
  std::snprintf(status.modelPath, sizeof(status.modelPath), "%s", name);
  std::snprintf(status.errorMsg, sizeof(status.errorMsg), "%s", transport.error.c_str());
  return status;
}
int mockStart(MockTransport& transport, const char* prompt) {
  if (!transport.selected || !transport.done) return 0;
  transport.starts++;
  transport.done = false;
  transport.prompt = prompt;
  transport.output.clear();
  transport.error.clear();
  return ++transport.session;
}
int mockChunk(MockTransport& transport, int offset, char* out, int cap) {
  if (!out || cap < 1 || offset < 0 || size_t(offset) >= transport.output.size()) return 0;
  int count = std::min(cap - 1, int(transport.output.size() - size_t(offset)));
  std::memcpy(out, transport.output.data() + offset, size_t(count));
  out[count] = '\0';
  return count;
}

#if ENABLE_LLM_SOURCE_HTTP
bool httpLlmFindByName(const char* name, LlmModelDesc* out) {
  ++httpResolves;
  return mockFind(name, out, LlmBackendKind::Http, "http");
}
size_t httpLlmUnavailableReason(char* out, size_t cap) {
  if (!cap) return 0;
  out[0] = '\0';
  if (httpAvailable) return 0;
  return testStrlcpy(out, "HTTP agent disabled (llmhttpenabled)", cap);
}
bool httpLlmPathUp() { return httpPathUp; }
size_t httpLlmPathText(char* out, size_t cap) {
  return testStrlcpy(out, httpPathUp ? "Wi-Fi" : "phone link down, retrying in 4 s", cap);
}
size_t httpLlmEnumerate(LlmModelDesc* out, size_t cap) {
  return cap && httpLlmFindByName("openclaw", out) ? 1 : 0;
}
bool httpLlmSelectByName(const char*, char*, size_t) { return mockSelect(httpTransport); }
void httpLlmUnload() { httpTransport.selected = false; }
bool httpLlmIsReady() { return httpTransport.selected && httpTransport.done; }
LlmStatus httpLlmStatus() { return mockStatus(httpTransport, "http:openclaw"); }
int httpLlmStartAsync(const char* prompt, const LlmGenParams&) { return mockStart(httpTransport, prompt); }
void httpLlmStop() { httpTransport.stops++; }
int httpLlmSessionId() { return httpTransport.session; }
int httpLlmResultLen() { return int(httpTransport.output.size()); }
int httpLlmResultChunk(int offset, char* out, int cap) { return mockChunk(httpTransport, offset, out, cap); }
bool httpLlmIsDone() { return httpTransport.done; }
void httpLlmTick() {}
bool httpLlmResetConversation() {
  if (!httpTransport.done || !httpTransport.resetAllowed) return false;
  httpTransport.resets++;
  return true;
}
#endif
#if ENABLE_LLM_SOURCE_CM5
bool cm5LlmFindByName(const char* name, LlmModelDesc* out) {
  return mockFind(name, out, LlmBackendKind::Cm5, "cm5");
}
size_t cm5LlmEnumerate(LlmModelDesc* out, size_t cap) {
  return cap && cm5LlmFindByName("local", out) ? 1 : 0;
}
bool cm5LlmSelectByName(const char*, char*, size_t) { return mockSelect(cm5Transport); }
void cm5LlmUnload() { cm5Transport.selected = false; }
bool cm5LlmIsReady() { return cm5Transport.selected && cm5Transport.done; }
LlmStatus cm5LlmStatus() { return mockStatus(cm5Transport, "cm5:local"); }
int cm5LlmStartAsync(const char* prompt, const LlmGenParams&) { return mockStart(cm5Transport, prompt); }
void cm5LlmStop() { cm5Transport.stops++; cm5Transport.done = true; }
int cm5LlmSessionId() { return cm5Transport.session; }
int cm5LlmResultLen() { return int(cm5Transport.output.size()); }
int cm5LlmResultChunk(int offset, char* out, int cap) { return mockChunk(cm5Transport, offset, out, cap); }
bool cm5LlmIsDone() { return cm5Transport.done; }
void cm5LlmTick() {}
#endif

// INSERT_PRODUCTION_IMPLEMENTATIONS

void finish(MockTransport& source, const char* output = "Reply") {
  source.output = output;
  source.done = true;
  assert(!chatIsGenerating());
}

static void longReplyRetention(MockTransport& transport) {
  assert(chatClear());
  const int session = chatBeginTurn("Keep the whole long reply");
  assert(session > 0);
  const std::string notice = "\n[Reply truncated on device]";
  const std::string reply = std::string(kLlmReplyCapacityBytes - 1 - notice.size(), 'r') + notice;
  transport.output = reply.substr(0, 8192);
  assert(chatGetStreamLen() == 8192);
  finish(transport, reply.c_str());
  ChatTurnInfo info{};
  assert(chatGetTurnInfo(-1, &info) && info.textLen == reply.size());
  assert(chatFinishedLen(session) == static_cast<int>(reply.size()));
  std::string retained;
  char chunk[512];
  int n;
  while ((n = chatReadTurn(-1, retained.size(), chunk, sizeof(chunk))) > 0)
    retained.append(chunk, n);
  assert(retained == reply);  // terminal notice survives the shared chat cap
  assert(chatClear());
}

#if ENABLE_LLM_SOURCE_HTTP
// The HTTP default's autostart is a level rule with backoff, the chat selects
// it lazily, and a user unload stops both for the rest of the boot.
static void httpAutostartAndLazySelect() {
  char reason[128];
  llmBackendUnload();
  assert(chatClear());
  sUserUnloadedThisBoot = false;   // as at boot
  gSettings.llmAutoStart = true;
  gSettings.llmDefaultModel = "HTTP:openclaw";   // the prefix is case-insensitive
  httpAvailable = false; httpPathUp = false;
  // Failures back off 5, 10, 20, 40 s, then 60 s for good.
  httpResolves = 0;
  llmBackendTick(); assert(httpResolves == 1 && llmBackendActiveKind() == LlmBackendKind::None);
  testNow += 4999; llmBackendTick(); assert(httpResolves == 1);
  testNow += 1; llmBackendTick(); assert(httpResolves == 2);
  for (uint32_t delay : {10000u, 20000u, 40000u, 60000u, 60000u}) {
    testNow += delay - 1; llmBackendTick(); const int before = httpResolves;
    testNow += 1; llmBackendTick(); assert(httpResolves == before + 1);
  }
  // The path coming up retries at once.
  testNow += 1000; llmBackendTick(); const int before = httpResolves;
  httpPathUp = true; llmBackendTick(); assert(httpResolves == before + 1);
  // llmload says why it is not available, not "not available right now".
  assert(!llmBackendSelect("http:openclaw", reason, sizeof(reason)));
  assert(!std::strcmp(reason, "HTTP agent disabled (llmhttpenabled)"));
  // Not ready, with the reason the commands and the glasses show: the lazy
  // select of the default failed, so its configuration reason, not an llmload
  // that would fail the same way.
  assert(llmBackendNotReadyReason(reason, sizeof(reason)) && !std::strcmp(reason, "HTTP agent disabled (llmhttpenabled)"));
  assert(chatBeginTurn("before the agent is configured") == 0);
  assert(!std::strcmp(chatLastBeginRefusal(), "HTTP agent disabled (llmhttpenabled)"));
  // Configured again: the next due try selects it (path down does not matter).
  httpAvailable = true; httpPathUp = false;
  testNow += 60000; llmBackendTick(); assert(llmBackendActiveKind() == LlmBackendKind::Http);
  assert(llmBackendNotReadyReason(reason, sizeof(reason)) == 0);
  // A user unload: no autostart, no lazy select, for the rest of this boot.
  llmBackendUnload();
  const int afterUnload = httpResolves;
  for (int i = 0; i < 3; ++i) { testNow += 60000; llmBackendTick(); }
  assert(httpResolves == afterUnload && llmBackendActiveKind() == LlmBackendKind::None);
  assert(!llmBackendEnsureDefaultSelected());
  assert(chatBeginTurn("after unload") == 0 && llmBackendActiveKind() == LlmBackendKind::None);
  // Nothing selects the default by itself now: "no model selected" names the
  // command, even with the agent's configuration also refused.
  httpAvailable = false;
  assert(llmBackendNotReadyReason(reason, sizeof(reason)) && !std::strcmp(reason, "no model selected (llmload http:openclaw)"));
  httpAvailable = true;
  // An explicit select lifts it; after the next unload the flag is set again.
  assert(llmBackendSelect("http:openclaw", reason, sizeof(reason)));
  assert(!sUserUnloadedThisBoot);
  llmBackendUnload(); assert(sUserUnloadedThisBoot);
  // Lazy select: the chat selects the default itself and starts the turn.
  sUserUnloadedThisBoot = false;
  ChatRunToken run = 0;
  const int session = chatBeginTurn("Hey Even", nullptr, &run);
  assert(session > 0 && run && llmBackendActiveKind() == LlmBackendKind::Http);
  assert(httpTransport.prompt == "Hey Even");
  finish(httpTransport, "Hello");
  // Busy, then the source's own not-ready reason.
  httpTransport.done = false;
  assert(llmBackendNotReadyReason(reason, sizeof(reason)) && !std::strcmp(reason, "busy: a request is running"));
  httpTransport.done = true;
  // Autostart off: neither the tick nor the chat selects anything.
  llmBackendUnload(); assert(chatClear()); sUserUnloadedThisBoot = false;
  gSettings.llmAutoStart = false;
  testNow += 60000; llmBackendTick(); assert(llmBackendActiveKind() == LlmBackendKind::None);
  assert(!llmBackendEnsureDefaultSelected());
  gSettings.llmEnabled = false;
  assert(llmBackendNotReadyReason(reason, sizeof(reason)) && !std::strcmp(reason, "LLM disabled (llmenabled 0)"));
  gSettings.llmEnabled = true;
  gSettings.llmDefaultModel = "";
}
#endif

int main() {
  // A CLI/web New chat may be the first chat operation after boot. It must
  // initialize its owner without a prior generation or OLED entry point.
  assert(llmBackendActiveKind() == LlmBackendKind::None);
  assert(chatClear());
  assert(chatGetTurnCount() == 0);
  assert(httpTransport.starts == 0 && cm5Transport.starts == 0);
  LlmModelDesc models[4]{};
  const size_t count = llmEnumerateModels(models, 4);
  assert(count == ENABLE_LLM_SOURCE_HTTP + ENABLE_LLM_SOURCE_CM5);
  gSettings.llmEnabled = false;
  assert(llmEnumerateModels(models, 4) == 0);
  gSettings.llmEnabled = true;
  char error[128]{};
#if ENABLE_LLM_SOURCE_HTTP
  assert(llmBackendSelect("http:openclaw", error, sizeof(error)));
  assert(llmBackendActiveKind() == LlmBackendKind::Http);
  assert(!llmBackendSupportsRetry());
  assert(!llmBackendSupportsCommandMode());
  MockTransport& transport = httpTransport;
#else
  assert(!llmBackendSelect("http:openclaw", error, sizeof(error)));
  assert(llmBackendSelect("cm5:local", error, sizeof(error)));
  assert(llmBackendSupportsRetry());
  MockTransport& transport = cm5Transport;
#endif
  assert(chatBeginTurn("Do: toggle the LED") == 0);
  assert(chatGetRunToken() == 0);
  ChatRunToken firstRun = 0;
  const int firstSession = chatBeginTurn("Hello, agent", nullptr, &firstRun);
  assert(firstSession > 0 && transport.prompt == "Hello, agent");
  assert(firstRun && chatGetRunToken() == firstRun);
  assert(chatIsGenerating());
  assert(!chatClear());
  assert(chatRetryLast() == 0);
  assert(chatBeginTurn("Duplicate") == 0);
  assert(chatGetRunToken() == firstRun);
  assert(transport.starts == 1);
  transport.output = "Partial text";
  char text[128]{};
  assert(chatReadStream(0, text, sizeof(text)) == 12);
  assert(std::strcmp(text, "Partial text") == 0);
  bool exactDone = true;
  assert(chatReadRun(firstRun, 0, text, sizeof(text), &exactDone) == 12 && !exactDone);
  assert(chatReadRun(firstRun + 99, 0, text, sizeof(text), &exactDone) == -1 && !text[0]);
  const int stopsBeforeStale = transport.stops;
  assert(!chatStopRun(firstRun + 99) && transport.stops == stopsBeforeStale);
  assert(chatStopRun(firstRun) && transport.stops == stopsBeforeStale + 1);
  chatStop();
#if ENABLE_LLM_SOURCE_HTTP
  // Stop requests cancellation; clear waits for the transport to release it.
  assert(!chatClear());
  assert(transport.resets == 0);
#endif
  finish(transport, "Partial text");
  assert(chatGetRunToken() == firstRun);
  assert(chatReadFinished(firstSession, 0, text, sizeof(text)) == 12);
  assert(chatReadRun(firstRun, 0, text, sizeof(text), &exactDone) == 12 && exactDone);
  assert(chatReadRun(firstRun, 12, text, sizeof(text), &exactDone) == 0 && exactDone);
  assert(!chatStopRun(firstRun));
  assert(chatGetTurnCount() == 2);
#if ENABLE_LLM_SOURCE_HTTP
  const int before = transport.starts;
  assert(chatRetryLast() == 0);
  assert(transport.starts == before);
  assert(chatGetTurnCount() == 2);
  transport.resetAllowed = false;
  assert(!chatClear());
  assert(chatGetTurnCount() == 2);
  transport.resetAllowed = true;
#else
  assert(chatRetryLast() > 0);
  finish(transport, "Retry reply");
#endif
  assert(chatClear());
  assert(chatGetTurnCount() == 0);
  assert(chatGetRunToken() == 0);
  assert(chatFinishedLen(firstSession) == 0);
#if ENABLE_LLM_SOURCE_HTTP
  assert(transport.resets == 1);
#endif
  // Empty successful replies finish rather than leaving a perpetual spinner.
  // Different backends can reuse a numeric session ID. The new run token
  // must still reject the old popup's reads/cancellation after Clear.
  transport.session = firstSession - 1;
  ChatRunToken emptyRun = 0;
  const int emptySession = chatBeginTurn("No text", nullptr, &emptyRun);
  assert(emptySession == firstSession && emptyRun && emptyRun != firstRun);
  assert(chatGetRunToken() == emptyRun);
  assert(!chatStopRun(firstRun));
  assert(chatReadRun(firstRun, 0, text, sizeof(text), &exactDone) == -1);
  finish(transport, "");
  assert(chatGetRunToken() == emptyRun);
  assert(chatGetTurnCount() == 2);
  assert(chatReadFinished(emptySession, 0, text, sizeof(text)) == 0);
  assert(chatReadRun(emptyRun, 0, text, sizeof(text), &exactDone) == 0 && exactDone);
  ChatTurnInfo info{};
  assert(chatGetTurnInfo(-1, &info) && !info.isStreaming && info.textLen == 0);
  assert(chatClear());
  longReplyRetention(transport);
#if ENABLE_LLM_SOURCE_HTTP && ENABLE_LLM_SOURCE_CM5
  assert(chatBeginTurn("One route") > 0);
  finish(httpTransport);
  // A route switch with existing history is refused until an explicit clear.
  assert(!llmBackendSelect("cm5:local", error, sizeof(error)));
  assert(llmBackendActiveKind() == LlmBackendKind::Http);
  llmBackendUnload();
  llmBackendUnload();  // Repeated unload must retain the prior source boundary.
  assert(!llmBackendSelect("cm5:local", error, sizeof(error)));
  assert(!llmBackendSupportsRetry());
  assert(chatClear());
  assert(llmBackendSelect("cm5:local", error, sizeof(error)));
  assert(llmBackendSupportsRetry());
  assert(chatBeginTurn("UART request") > 0);
  finish(cm5Transport);
  assert(chatRetryLast() > 0);
  finish(cm5Transport);
  llmBackendUnload();
  assert(!llmBackendSelect("http:openclaw", error, sizeof(error)));
  assert(chatClear());
  assert(llmBackendSelect("http:openclaw", error, sizeof(error)));
#endif
  llmBackendUnload();
  assert(llmBackendActiveKind() == LlmBackendKind::None);
  assert(llmBackendIsDone());
  assert(chatBeginTurn("No source") == 0);
  assert(chatLastBeginRefusal()[0]);
  assert(chatClear());
#if ENABLE_LLM_SOURCE_HTTP
  httpAutostartAndLazySelect();
#endif
  std::puts("LLM backend/chat lifecycle passed");
}
