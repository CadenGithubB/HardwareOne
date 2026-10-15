#pragma once
#include "System_BuildConfig.h"
#include <atomic>
#include <cstdint>
#include <Arduino.h>
#include <esp_http_client.h>

struct LlmHttpIo;
// The caller publishes this worker-owned handle under its request lock before
// connection setup. Cancellation only sets a flag; no foreign task frees TLS.
// ca and cancelled must remain alive until destroy, after HTTP client cleanup.
LlmHttpIo* llmHttpIoCreate(const char* ca, bool secure, const std::atomic<bool>& cancelled,
                         uint32_t startedMs, uint32_t totalMs, uint32_t revision);
bool llmHttpIoConfigure(LlmHttpIo* io, esp_http_client_config_t& config);
// Connected and Cancel run under the caller's request lock. Clear the published
// pointer under that same lock before HTTP cleanup/destroy outside the lock.
void llmHttpIoConnected(LlmHttpIo* io, esp_http_client_handle_t client);
void llmHttpIoCancel(LlmHttpIo* io);
void llmHttpIoDestroy(LlmHttpIo* io);
bool llmHttpPathAvailable(bool secure);
