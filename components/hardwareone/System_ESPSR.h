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

void initESPSR();
bool startESPSR();
void stopESPSR();
bool isESPSRRunning();
bool isESPSRWakeActive();
void setESPSRWakeCallback(void (*callback)(const char* wakeWord));
void setESPSRCommandCallback(void (*callback)(int commandId, const char* commandPhrase));
void buildESPSRStatusJson(String& output);

// Voice state getters for OLED/Web display
const char* getESPSRVoiceState();      // Returns "idle", "category", "subcategory", "target"
const char* getESPSRCurrentCategory(); // Returns current category being processed
const char* getESPSRCurrentSubCategory(); // Returns current subcategory being processed
const char* getESPSRLastCommand();     // Returns last executed command
float getESPSRLastConfidence();        // Returns last command confidence (0.0-1.0)
uint32_t getESPSRWakeCount();          // Returns total wake word detections
uint32_t getESPSRCommandCount();       // Returns total commands executed

extern const CommandEntry espsrCommands[];
extern const size_t espsrCommandsCount;

const char* cmd_sr(const String& cmd);
const char* cmd_sr_enable(const String& cmd);
const char* cmd_sr_start(const String& cmd);
const char* cmd_sr_stop(const String& cmd);
const char* cmd_sr_status(const String& cmd);

void registerESPSRHandlers(httpd_handle_t server);

#else

inline void registerESPSRHandlers(httpd_handle_t server) { (void)server; }

#endif // ENABLE_ESP_SR

#endif // SYSTEM_ESPSR_H
