#pragma once
#include "System_BuildConfig.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <Arduino.h>
#include "BLE_PhonePipeTypes.h"
#include "System_User.h"  // CommandSource, TransportSessionEpoch (PhonePairView)
// The READY session's link, for the app console on the same link
// (BLE_ConsoleAttachCore.h hw1console::PipeLink). generation is the GATT
// session's (one per link); connId is that link's, shared by GATT client and server.
struct PhonePipeLinkRecord { uint32_t generation = 0; uint16_t connId = 0; };
// A console record push into the READY session's slot 2 (phonePipeConsolePush).
// Busy: the owner mutex was not taken within the wait.
enum class PhonePipePush : uint8_t { Ok, NoSpace, Gone, Busy };
// Status snapshot of the phone link (phone link v2), copied under a leaf lock:
// counts and flags only, no addresses or content. endReason is indexed by the
// pipe error code 0..18 (0 also collects unknown codes).
struct PhonePipeLinkSummary {
  bool active = false, ready = false;
  bool relayOn = false;          // the phone's RELAY_ON (READY only)
  bool consoleWanted = false;    // the phone's CONSOLE_WANTED (READY only)
  bool consoleOffered = false;   // what this P4 announces (CONSOLE_OFFERED)
  bool versionLatched = false;   // 3 mismatch strikes: auto-reconnect held
  uint8_t strikes = 0;           // mismatch strikes in a row
  uint8_t relayStreams = 0, consoleStreams = 0;  // live streams by class
  uint32_t ended = 0;            // READY sessions ended since boot
  uint16_t lastEnd = 0;          // the last one's end reason (0..18)
  uint32_t versionMismatches = 0;
  uint32_t endReason[19] = {};
};
// `phone pair new`'s pairing window (phone link v2) as its OLED code screen
// sees it: open, or how the last one ended.
enum class PhonePairWindow : uint8_t { None, Open, Paired, Expired, Closed };
struct PhonePairView {
  PhonePairWindow state = PhonePairWindow::None;
  uint32_t id = 0;         // the open window's id (0 once it closed)
  uint32_t msLeft = 0;     // until it expires
  uint32_t ageMs = 0;      // since it opened
  bool onDisplay = false;  // its code is for the OLED code screen
  // The session that issued it: the code screen draws the code only while
  // transportSessionEpochIsLive(source, epoch).
  CommandSource source = SOURCE_SERIAL;
  TransportSessionEpoch epoch = 0;
};
#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE
// The phone link as a device on the shared Bluetooth client stack
// (BLE_CentralStack): phoneLinkOpen() brings the stack up if it is down and
// starts the pipe on it; phoneLinkClose() stops the pipe (userInitiated stamps
// the peer's intent so auto-reconnect stays quiet; false keeps it, as `closeble`
// and the recoveries do). The stack stays up, and the phone_pipe task parks
// until bleStackDown() releases it (its reset pass needs the stopped host).
// Task context only; both fail closed on a busy role transition.
bool phoneLinkOpen();
bool phoneLinkClose(bool userInitiated);
// Publishes the peer spec so the reconnect scheduler can admit a closed pipe
// (its admission opens it). Called by bleStackUp().
void phonePipeRegisterPeer();
// Drain only this owner (the pipe stops; the task and the host stay).
bool phonePipeStop();
// Invoke only after a successful stop; next start derives the new PSK off-callback.
bool phonePipeInvalidateSecret();
bool phonePipeActive();
bool phonePipeReady();
// READY and the phone announces RELAY_ON (relay forwarding on, a destination
// saved, its network usable). Relay users (LLM HTTP, Terminal) use this, not
// phonePipeReady(): the link and the console stay up with relay off.
bool phonePipeRelayAvailable();
// The console driver's writer (BLE_PhonePipeConsole.cpp): one whole record
// into slot 2 of the live READY session of `generation`, or nothing. Takes the
// owner mutex for at most lockWaitMs (0 = try once). Never call it holding the
// BLE lifecycle lock or from the pipe task.
PhonePipePush phonePipeConsolePush(uint32_t generation, uint16_t streamId,
                                   const uint8_t* head, size_t headLen,
                                   const uint8_t* payload, size_t n, size_t reserve,
                                   uint32_t lockWaitMs);
// The calling task holds the owner mutex (output paths must not wait on it).
bool phonePipeOwnerHeldByCurrentTask();
// Lock-free copy for the status surfaces; false when the pipe is not built or
// has no resources yet (out is then zeroed).
bool phonePipeLinkSummary(PhonePipeLinkSummary* out);
// Changes with every GATT session; scopes the transport's phone reachability hint.
uint32_t phonePipeGeneration();
// True with the record while the session is authenticated and READY; false
// (out zeroed) otherwise. It is set only while the pipe's client holds the
// link and may outlive a dropped link until the pipe task sees the loss.
// Copies under a portMUX, never the owner mutex: any task, BTC callbacks included.
bool phonePipeLinkRecord(PhonePipeLinkRecord* out);
// App console traffic on the pipe's link counts as stream activity (A-4), so
// replies do not wait out the idle connection interval. One atomic increment:
// any task, BTC callbacks included.
void phonePipeNoteConsoleActivity();
// After a positively verified terminal HAL teardown, never just a timeout.
bool phonePipeHostStopped();
// Lets the phone_pipe task leave (it deletes itself on its own pass) once
// the pipe is stopped and its host torn down, so an explicit off gives its
// stack back. Refused (false, nothing asked of the task) while the pipe is
// active, while the HAL is not stopped, while the host-stopped reset cannot
// run on the task (phonePipeHostStopped's 1 s), or while a phone setup job
// is claimed, queued, running or finishing (its turn sets the task's
// priority by handle). Then polls every 10 ms up to timeoutMs: true once
// the task is gone (or there was none); a timeout withdraws the request and
// keeps the task. Task context, never the pipe task itself; serialize with
// phonePipeStart (the role transition), which creates the task again.
bool phonePipeWorkerRelease(uint32_t timeoutMs);
bool phonePipeRecoveryNeeded();
// Stop worker API calls while retaining callback storage until terminal teardown.
bool phonePipeQuiesceForHostRecovery();
const char* phonePipeStatus();
// Status surfaces (BLE_LinkStatusCore.h): the setup step as hw1link::PipeStep,
// and whether `phone pair` is enrolling or `phone pair new`'s window is open.
// Lock-free.
uint8_t phonePipeStep();
bool phonePipeEnrolling();
// The OLED code screen: the window, and while it is open with its code on the
// display, the code (XXXX-XXXX-XXXX-XXXX + NUL) copied into code (if not null;
// zeroed otherwise). The caller wipes its copy. A leaf lock: any task.
PhonePairView phonePipePairView(char code[20]);
// Closes window `id` (B on the code screen, or the screen was left); the code
// is wiped at once and the pipe task ends the window. Any task; never blocks.
void phonePipePairClose(uint32_t id);
void phonePipeTick(); // no blocking work; worker owns I/O
// The caller must release a nonzero handle even when OPEN fails. budgetMs is
// what remains of the caller's deadline; phases are bounded as above.
PhonePipeOpenResult phonePipeOpen(PhonePipeStream& out,const char* host,uint16_t port,
                                  uint32_t budgetMs,const std::atomic<bool>& cancelled,
                                  const std::atomic<bool>* interrupted = nullptr);
// Nonblocking BIO boundary: >0 bytes, 0 would block, -1 error, -2 EOF.
int phonePipeRead(PhonePipeStream stream,uint8_t* bytes,size_t size);
int phonePipeWrite(PhonePipeStream stream,const uint8_t* bytes,size_t size);
void phonePipeClose(PhonePipeStream stream,bool cancelled);
void phonePipeRelease(PhonePipeStream stream);
// True only after protocol retirement and the worker release (or session loss).
bool phonePipeRetired(PhonePipeStream stream);
// The `phone` and `relay` command tables (phoneCommands, relayCommands) are
// defined in BLE_PhonePipe.cpp and registered by System_CommandRegistry.cpp.
#else
inline bool phoneLinkOpen() {return false;}
inline bool phoneLinkClose(bool) {return true;}
inline void phonePipeRegisterPeer() {}
inline bool phonePipeStop() {return true;}
inline bool phonePipeInvalidateSecret() {return true;}
inline bool phonePipeActive() {return false;}
inline bool phonePipeReady() {return false;}
inline bool phonePipeRelayAvailable() {return false;}
inline PhonePipePush phonePipeConsolePush(uint32_t,uint16_t,const uint8_t*,size_t,const uint8_t*,size_t,size_t,uint32_t) {return PhonePipePush::Gone;}
inline bool phonePipeOwnerHeldByCurrentTask() {return false;}
inline bool phonePipeLinkSummary(PhonePipeLinkSummary* out) {if(out)*out={};return false;}
inline uint32_t phonePipeGeneration() {return 0;}
inline bool phonePipeLinkRecord(PhonePipeLinkRecord* out) {if(out)*out={};return false;}
inline void phonePipeNoteConsoleActivity() {}
inline bool phonePipeHostStopped() {return true;}
inline bool phonePipeWorkerRelease(uint32_t) {return true;}
inline bool phonePipeRecoveryNeeded() {return false;}
inline bool phonePipeQuiesceForHostRecovery() {return false;}
inline void phonePipeTick() {}
inline const char* phonePipeStatus() {return "not compiled";}
inline uint8_t phonePipeStep() {return 0;}
inline bool phonePipeEnrolling() {return false;}
inline PhonePairView phonePipePairView(char code[20]) {if(code)for(int i=0;i<20;++i)code[i]=0;return {};}
inline void phonePipePairClose(uint32_t) {}
#endif
