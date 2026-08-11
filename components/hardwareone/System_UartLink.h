// System_UartLink.h — UART host link: a machine command channel for a Linux
// host (the CM5 carrier) wired to this board's UART_LINK_* pins.
//
// Design (docs/UART_HOST_LINK_PLAN.md, adversarially verified 2026-08-07):
// this is a TRANSPORT, not a sink — the MQTT-bridge shape. The drain reads
// newline-framed lines, runs them through submitAndExecuteSync like every
// other command source, and writes the result blob back on the wire itself.
// outputMask carries MSG_ROUTE_FILE only, so no broadcast/debug line can ever
// reach the channel; the reply is the one and only thing the host receives.
//
// Session model mirrors the serial console but is fully separate:
// a private synchronized auth/user snapshot (never gSerial*), brute-force key
// "uart" (never "local"), idle window sessionIdleUart. ORIGIN_UART is never
// treated as physical presence.
//
// Boards without UART_LINK_* pins (the unsupported-board fallback) compile
// no-op stubs so callers stay unconditional.
#ifndef SYSTEM_UARTLINK_H
#define SYSTEM_UARTLINK_H

#include <Arduino.h>
#include "System_BuildConfig.h"

// Nonzero only for the currently authenticated UART login. Every successful
// login gets a new monotonically generated value; logout/revoke publishes 0.
// A successful login always advances it, including a re-login by the same
// username. Long-lived producers bind their lease to this value so a
// logout/re-login cannot transfer authority merely by reusing a controller
// token. Session mutation stays centralized here because account revocation,
// idle expiry, the web auth helper, and the in-band UART login all touch the
// same globals.
void uartLinkSessionAuthenticated(const String& user);
void uartLinkSessionCleared();
uint32_t uartLinkSessionEpoch();
bool uartLinkSessionSnapshot(char* userOut, size_t userOutSize,
                             uint32_t* epochOut = nullptr);
bool uartLinkSessionClearIfUser(const String& user);

// Binary frame IDs and payload ceiling are part of the board-independent wire
// contract. Keep them visible even on boards whose UART-link methods compile
// to stubs so protocol diagnostics still build and fail cleanly at runtime.
#define UARTLINK_FRAME_AUDIO       0x01  // voicefetch payload chunk
#define UARTLINK_FRAME_META        0x02  // payload: total_size_le(4)
#define UARTLINK_FRAME_EVT         0x03  // spontaneous event push (ASCII payload)
#define UARTLINK_FRAME_LIVE_BEGIN  0x10  // live-pcm-v1 stream metadata
#define UARTLINK_FRAME_LIVE_PCM    0x11  // live-pcm-v1 offset-tagged PCM
#define UARTLINK_FRAME_LIVE_END    0x12  // live-pcm-v1 successful terminal
#define UARTLINK_FRAME_LIVE_ABORT  0x13  // live-pcm-v1 failed terminal
#define UARTLINK_FRAME_MAX_PAYLOAD 1024

#ifdef UART_LINK_PORT

// Open the link port at gSettings.uartLinkBaud (0 = UART_LINK_BAUD_DEFAULT).
// Idempotent. Buffers allocate here — zero cost while the link is disabled.
//
// THREADING: call only from the main loop task (uartLinkInitFromSettings at
// boot, or uartLinkTick applying a pending request). Commands run on
// cmd_exec_task, which ticks concurrently with the loop task, so command
// handlers must use uartLinkRequest* below instead — tearing the port down
// underneath an in-flight drain corrupts the line accumulator.
bool uartLinkStart();

// Close the port and drop the session (auth cleared, accumulator flushed).
// Same loop-task-only rule as uartLinkStart.
void uartLinkStop();

// Command-safe lifecycle control: records the intent and returns immediately;
// uartLinkTick performs the actual start/stop/restart on the loop task before
// draining. Safe from any task.
void uartLinkRequestStart();
void uartLinkRequestStop();
void uartLinkRequestRestart();

// Start iff gSettings.uartLinkEnabled — called once from hardwareone_setup,
// after first-time setup has completed.
void uartLinkInitFromSettings();

// Per-lap drain, called from loop() beside the serial CLI drain. At most one
// command per lap; parked while the setup wizard owns the CLI.
void uartLinkTick();

bool uartLinkIsRunning();

// One-line status for cmd_uartlink (static buffer).
const char* uartLinkStatusLine();

// ---------------------------------------------------------------------------
// P2 binary frame layer (docs/../cm5/CM5_AI_SERVICE_PLAN.md §Gap A1).
//
// Wire format — the CM5 client (cm5/ai-service link/protocol.py) implements
// the exact mirror of this; change BOTH or neither:
//
//   0x00  COBS(body)  0x00        one frame; 0x00 never occurs inside COBS
//   body := type(1) | seq_le(2) | len_le(2) | payload(len) | crc_le(2)
//   crc  := CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect/xorout)
//          over type..payload
//
// Text lines and frames coexist on the wire: text never contains 0x00 (the
// reply pipeline is NUL-terminated C strings), so a 0x00 unambiguously
// enters frame mode at the receiver. Frames are written as ONE port write
// under the link TX mutex — they can never split a text reply.
// Write one frame (single port write, CRC + COBS applied here). Safe from
// any task; returns false if the link is down, the session is gone, or the
// TX mutex could not be taken within 1s. Checks the active session epoch per
// call.
bool uartLinkWriteFrame(uint8_t type, uint16_t seq,
                        const uint8_t* payload, size_t len);

// Blocking frame admission fenced to one exact authenticated login
// generation. This is the bulk-transfer counterpart to the nonblocking live
// writer below: a logout/re-login while voicefetch is in progress stops the
// transfer at the next frame boundary instead of handing the remaining bytes
// to the new session.
bool uartLinkWriteFrameForSession(uint32_t sessionEpoch,
                                  uint8_t type, uint16_t seq,
                                  const uint8_t* payload, size_t len);

// Nonblocking frame admission for real-time producers. Takes the TX mutex only
// if immediately available and writes only when the complete encoded frame fits
// in the UART TX ring. A false result is retryable by a bounded producer; it
// never waits behind a command reply or a full TX ring.
bool uartLinkTryWriteFrame(uint8_t type, uint16_t seq,
                           const uint8_t* payload, size_t len);

// Nonblocking admission additionally fenced to one exact authenticated login
// generation. The generation is rechecked under the TX mutex immediately
// before the physical write. This is the live-stream authority boundary; a
// zero epoch is invalid and always rejected.
bool uartLinkTryWriteFrameForSession(uint32_t sessionEpoch,
                                     uint8_t type, uint16_t seq,
                                     const uint8_t* payload, size_t len);

// Runtime baud after the setting/default and board clamp are applied. Live PCM
// uses this to fail closed below its tested throughput floor.
int uartLinkEffectiveBaud();

// Spontaneous event push: one EVT frame carrying a short ASCII payload
// (e.g. "evenai_wake"). The host treats the payload as an event name plus
// optional space-separated args. Rides uartLinkWriteFrame, so it inherits
// the auth gate and single-write guarantee — an EVT can never split a text
// reply, and nothing is pushed to a logged-out host. Safe from any task.
bool uartLinkPushEvent(const char* text);

// Session-fenced event variants for work whose lifetime can outlast the login
// that admitted it (EvenAI wake/cancel and owned microphone completion). A
// stale producer is suppressed after logout/re-login even if the same user
// authenticates again.
bool uartLinkPushEventForSession(uint32_t sessionEpoch, const char* text);

// Best-effort EVT variant for main-loop state machines: never waits for the
// frame TX mutex. It returns false immediately when another frame owns the
// writer so the caller can retry on a later tick.
bool uartLinkTryPushEvent(const char* text);
bool uartLinkTryPushEventForSession(uint32_t sessionEpoch, const char* text);

#else  // no UART_LINK_* pins for this board — inert stubs

inline bool uartLinkStart() { return false; }
inline void uartLinkStop() {}
inline void uartLinkRequestStart() {}
inline void uartLinkRequestStop() {}
inline void uartLinkRequestRestart() {}
inline void uartLinkInitFromSettings() {}
inline void uartLinkTick() {}
inline bool uartLinkIsRunning() { return false; }
inline const char* uartLinkStatusLine() { return "UART link: unsupported on this board"; }
inline bool uartLinkWriteFrame(uint8_t, uint16_t, const uint8_t*, size_t) { return false; }
inline bool uartLinkWriteFrameForSession(uint32_t, uint8_t, uint16_t,
                                         const uint8_t*, size_t) { return false; }
inline bool uartLinkTryWriteFrame(uint8_t, uint16_t, const uint8_t*, size_t) { return false; }
inline bool uartLinkTryWriteFrameForSession(uint32_t, uint8_t, uint16_t,
                                            const uint8_t*, size_t) { return false; }
inline int uartLinkEffectiveBaud() { return 0; }
inline bool uartLinkPushEvent(const char*) { return false; }
inline bool uartLinkPushEventForSession(uint32_t, const char*) { return false; }
inline bool uartLinkTryPushEvent(const char*) { return false; }
inline bool uartLinkTryPushEventForSession(uint32_t, const char*) { return false; }

#endif  // UART_LINK_PORT

// Registry command (all boards; errors cleanly where the link is absent):
// stream a recording file to the CM5 as binary frames — the P2 replacement
// for the chunked base64 fileread loop. Registered in the microphone module.
const char* cmd_voicefetch(const String& argsInput);

#endif  // SYSTEM_UARTLINK_H
