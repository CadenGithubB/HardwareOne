#pragma once

#include "System_BuildConfig.h"
#if ENABLE_LLM_BACKEND && ENABLE_LLM_SOURCE_HTTP
#include "System_LLMBackend.h"

// Validate before persisting/displaying a URL. Transport opt-in is enforced
// separately by configure, so URL and plaintext settings can be entered in any order.
bool httpLlmValidateBaseUrl(const char* baseUrl);

// Publish after settings load/update, from the settings owner. The worker only
// uses immutable bounded copies; it never dereferences gSettings Strings.
// Invalid configuration is retained as unavailable with a non-secret reason.
bool httpLlmConfigure(bool enabled, const char* baseUrl, const char* model,
                      const char* token, bool allowPlaintext, const char* caPem);
size_t httpLlmEnumerate(LlmModelDesc* out, size_t cap);
bool httpLlmFindByName(const char* name, LlmModelDesc* out);
bool httpLlmSelectByName(const char* name, char* error, size_t errorCap);
void httpLlmUnload();
bool httpLlmIsReady();
LLMStatus httpLlmStatus();
int httpLlmStartAsync(const char* prompt, const LLMGenParams& params);
void httpLlmStop();
int httpLlmSessionId();
int httpLlmResultLen();
int httpLlmResultChunk(int offset, char* out, int cap);
bool httpLlmIsDone();
void httpLlmTick();
// Refused until the worker has released its transport. A new ID starts a new
// host conversation; it does not delete host transcripts or agent memory.
bool httpLlmResetConversation();

#if ENABLE_G2_GLASSES && ENABLE_LOCAL_STT
// Native Terminal uses the same configured relay/TLS credentials, but has its
// own bounded asynchronous JSON exchange. It never replaces the chat result.
constexpr size_t HTTP_TERMINAL_JSON_CAP = 4096;
using HttpTerminalToken = uint32_t;
struct HttpTerminalResult {
  bool done = false;
  int httpStatus = 0;
  size_t length = 0;
  uint32_t configRevision = 0;
  char error[96] = {};
};
bool httpTerminalStart(const char* json, size_t length, HttpTerminalToken* token);
// False means a stale token or insufficient output capacity. Pending reads
// return true with done=false and empty output; completed reads are repeatable.
bool httpTerminalRead(HttpTerminalToken token, char* out, size_t cap,
                      HttpTerminalResult* result);
// Stops local I/O only; a sent host mutation may already have been accepted.
void httpTerminalCancel(HttpTerminalToken token);
// Zero means configuration is unavailable. A change invalidates a native view.
uint32_t httpTerminalConfigRevision();
#endif
#endif
