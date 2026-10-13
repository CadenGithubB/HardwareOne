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
enum class AllocPref { PreferPSRAM };
void* ps_realloc(void* memory, size_t size, AllocPref) { return std::realloc(memory, size); }

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

bool mockFind(const char* name, LlmModelDesc* out, LlmBackendKind kind, const char* prefix) {
  if (!name || !name[0] || !out) return false;
  *out = {};
  out->backend = kind;
  out->available = true;
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
LLMStatus mockStatus(MockTransport& transport, const char* name) {
  LLMStatus status{};
  status.state = !transport.selected ? LLMState::UNLOADED :
                 !transport.done ? LLMState::GENERATING : LLMState::READY;
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
  return mockFind(name, out, LlmBackendKind::Http, "http");
}
size_t httpLlmEnumerate(LlmModelDesc* out, size_t cap) {
  return cap && httpLlmFindByName("openclaw", out) ? 1 : 0;
}
bool httpLlmSelectByName(const char*, char*, size_t) { return mockSelect(httpTransport); }
void httpLlmUnload() { httpTransport.selected = false; }
bool httpLlmIsReady() { return httpTransport.selected && httpTransport.done; }
LLMStatus httpLlmStatus() { return mockStatus(httpTransport, "http:openclaw"); }
int httpLlmStartAsync(const char* prompt, const LLMGenParams&) { return mockStart(httpTransport, prompt); }
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
LLMStatus cm5LlmStatus() { return mockStatus(cm5Transport, "cm5:local"); }
int cm5LlmStartAsync(const char* prompt, const LLMGenParams&) { return mockStart(cm5Transport, prompt); }
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
  const int firstSession = chatBeginTurn("Hello, agent");
  assert(firstSession > 0 && transport.prompt == "Hello, agent");
  assert(chatIsGenerating());
  assert(!chatClear());
  assert(chatRetryLast() == 0);
  assert(chatBeginTurn("Duplicate") == 0);
  assert(transport.starts == 1);
  transport.output = "Partial text";
  char text[128]{};
  assert(chatReadStream(0, text, sizeof(text)) == 12);
  assert(std::strcmp(text, "Partial text") == 0);
  chatStop();
#if ENABLE_LLM_SOURCE_HTTP
  // Stop requests cancellation; clear waits for the transport to release it.
  assert(!chatClear());
  assert(transport.resets == 0);
#endif
  finish(transport, "Partial text");
  assert(chatReadFinished(firstSession, 0, text, sizeof(text)) == 12);
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
  assert(chatFinishedLen(firstSession) == 0);
#if ENABLE_LLM_SOURCE_HTTP
  assert(transport.resets == 1);
#endif
  // Empty successful replies finish rather than leaving a perpetual spinner.
  const int emptySession = chatBeginTurn("No text");
  assert(emptySession > 0);
  finish(transport, "");
  assert(chatGetTurnCount() == 2);
  assert(chatReadFinished(emptySession, 0, text, sizeof(text)) == 0);
  ChatTurnInfo info{};
  assert(chatGetTurnInfo(-1, &info) && !info.isStreaming && info.textLen == 0);
  assert(chatClear());
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
  assert(chatClear());
  std::puts("LLM backend/chat lifecycle passed");
}
