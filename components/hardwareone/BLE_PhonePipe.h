#pragma once
#include "System_BuildConfig.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <Arduino.h>
struct PhonePipeStream { uint32_t generation=0; uint16_t id=0; };
#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE
// standalone starts a central host without G2 presentation. Otherwise attaches
// to the already initialized G2 central host. Call from task context only.
bool phonePipeStart(bool standalone);
// Drain only this owner. Host teardown stays with the application role owner.
bool phonePipeStop();
bool phonePipeActive();
bool phonePipeStandalone();
bool phonePipeReady();
// After a positively verified terminal HAL teardown, never just a timeout.
bool phonePipeHostStopped();
const char* phonePipeStatus();
void phonePipeTick(); // no blocking work; worker owns I/O
bool phonePipeOpen(PhonePipeStream& out,const char* host,uint16_t port,
                   uint32_t timeoutMs,const std::atomic<bool>& cancelled,
                   const std::atomic<bool>* interrupted = nullptr);
// Nonblocking BIO boundary: >0 bytes, 0 would block, -1 error, -2 EOF.
int phonePipeRead(PhonePipeStream stream,uint8_t* bytes,size_t size);
int phonePipeWrite(PhonePipeStream stream,const uint8_t* bytes,size_t size);
void phonePipeClose(PhonePipeStream stream,bool cancelled);
void phonePipeRelease(PhonePipeStream stream);
const char* phonePipeCommand(const String& args);
#else
inline bool phonePipeStart(bool) {return false;}
inline bool phonePipeStop() {return true;}
inline bool phonePipeActive() {return false;}
inline bool phonePipeStandalone() {return false;}
inline bool phonePipeReady() {return false;}
inline bool phonePipeHostStopped() {return true;}
inline void phonePipeTick() {}
inline const char* phonePipeStatus() {return "not compiled";}
inline const char* phonePipeCommand(const String&) {return "Error: phone pipe not compiled";}
#endif
