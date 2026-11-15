#ifndef SYSTEM_ESPSR_H
#define SYSTEM_ESPSR_H

#include "System_BuildConfig.h"

#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

#if ENABLE_ESP_SR

#include <Arduino.h>
#include "System_Command.h"
#include "System_Settings.h"

void initEspSr();
bool startEspSr();
// False means a worker is still stopping; its resources remain owned for retry.
bool stopEspSr();
bool isEspSrRunning();
bool isEspSrWakeActive();
void setEspSrWakeCallback(void (*callback)(const char* wakeWord));
void setEspSrCommandCallback(void (*callback)(int commandId, const char* commandPhrase));
void buildEspSrStatusJson(String& output);

// Voice state getters for OLED/Web display
const char* getEspSrVoiceState();      // Returns "idle", "category", "subcategory", "target"
// The recognizer task rewrites the category, the subcategory and the last
// command while other tasks show them, so a caller gets a copy, never a
// pointer: each call fills `out` (NUL-terminated, cut to outSize - 1
// characters) under the recognizer's text lock. The sizes hold a whole
// MultiNet phrase and the longest last command built from three of them.
constexpr size_t kEspSrPhraseTextSize = 64;  // a MultiNet phrase (at most 63 characters) and its NUL
constexpr size_t kEspSrLastCommandTextSize = 3 * kEspSrPhraseTextSize;  // "category subcategory target"
void getEspSrCurrentCategory(char* out, size_t outSize);     // category being processed, "" when none
void getEspSrCurrentSubCategory(char* out, size_t outSize);  // subcategory being processed, "" when none
void getEspSrLastCommand(char* out, size_t outSize);         // last recognised command, "" when none
float getEspSrLastConfidence();        // Returns last command confidence (0.0-1.0)
uint32_t getEspSrWakeCount();          // Returns total wake word detections
uint32_t getEspSrCommandCount();       // Returns total commands executed

// Recompute the legacy gSrDebugLevel from the new bool-flag system
// (gSettings.debugSr / debugSrWake / debugSrCommand / debugSrAfe /
// debugSrLifecycle / debugSrTuning). Existing SR_DBG_L call sites use the
// integer level, so this lets the granular flags drive them without
// rewriting every caller. Safe to call unconditionally — the #else branch
// below supplies an inline no-op when SR is compiled out.
void srSyncDebugLevel();

const char* cmd_sr(const String& argsInput);
const char* cmd_sr_enable(const String& argsInput);
const char* cmd_sr_start(const String& argsInput);
const char* cmd_sr_stop(const String& argsInput);
const char* cmd_sr_status(const String& argsInput);

void registerEspSrHandlers(httpd_handle_t server);

#else

inline void registerEspSrHandlers(httpd_handle_t server) { (void)server; }
inline void srSyncDebugLevel() {}

#endif // ENABLE_ESP_SR

#endif // SYSTEM_ESPSR_H
