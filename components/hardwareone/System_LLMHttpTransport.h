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
// No path is up, but the phone link is coming back by itself (connecting or
// retrying; Wi-Fi recovery is not modelled). Chat requests are admitted then
// and wait in connect for the path, nudging the phone once.
bool llmHttpPathRecoverable(bool secure);
// One line for status surfaces: "Wi-Fi" or "phone relay" when a path is up,
// otherwise the phone link's own text (BLE_PhonePipePath.h). Returns the length.
size_t llmHttpPathText(bool secure, char* out, size_t cap);
// After the request: connect gave up waiting for a path (nothing was sent),
// and whether it used its one same-path retry before any HTTP write.
bool llmHttpIoPathWaitFailed(const LlmHttpIo* io);
bool llmHttpIoRetriedSamePath(const LlmHttpIo* io);
// The failed wait was the same-path retry's: the phone relay was up but the
// phone had not released this consumer's previous stream in time.
bool llmHttpIoRetireWaitFailed(const LlmHttpIo* io);
// F3: request path counters and the most recent records, one per line. No
// host, port, credential or content is stored. Returns the length written.
size_t llmHttpPathReport(char* out, size_t cap);
