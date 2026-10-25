#pragma once
#include "System_BuildConfig.h"
#include <atomic>
#include <cstdint>
#include <Arduino.h>
#include <esp_http_client.h>

enum class LlmHttpRequestKind : uint8_t { Chat, Terminal };
enum class LlmHttpAdmission : uint8_t { Accepted, Busy, Unavailable };
// Claim before starting a worker; release only after all its I/O is destroyed.
LlmHttpAdmission llmHttpRequestAdmit(LlmHttpRequestKind kind, bool secure);
void llmHttpRequestRelease(LlmHttpRequestKind kind);
struct LlmHttpIo;
// The caller publishes this worker-owned handle under its request lock before
// connection setup. Cancellation only sets a flag; no foreign task frees TLS.
// ca and cancelled must remain alive until destroy, after HTTP client cleanup.
LlmHttpIo* llmHttpIoCreate(const char* ca, bool secure, const std::atomic<bool>& cancelled,
                         uint32_t startedMs, uint32_t totalMs, uint32_t revision,
                         LlmHttpRequestKind kind = LlmHttpRequestKind::Chat);
bool llmHttpIoUsedPhone(const LlmHttpIo* io);
bool llmHttpIoConfigure(LlmHttpIo* io, esp_http_client_config_t& config);
// Connected and Cancel run under the caller's request lock. Clear the published
// pointer under that same lock before HTTP cleanup/destroy outside the lock.
void llmHttpIoConnected(LlmHttpIo* io, esp_http_client_handle_t client);
void llmHttpIoCancel(LlmHttpIo* io);
// Report the request's final result before destroy: completed means the full
// response arrived. Destroy then records the F3 path/outcome entry.
void llmHttpIoFinish(LlmHttpIo* io, bool completed);
void llmHttpIoDestroy(LlmHttpIo* io);
bool llmHttpPathAvailable(bool secure);
// F3: request path counters and the most recent records, one per line. No
// host, port, credential or content is stored. Returns the length written.
size_t llmHttpPathReport(char* out, size_t cap);
