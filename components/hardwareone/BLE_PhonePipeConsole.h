#pragma once
// The app console as stream slot 2 of the phone pipe (phone link v2): the
// console driver between the pipe owner (BLE_PhonePipe.cpp, which owns the
// Session under its mutex) and the BLE session rows (Bluetooth.cpp, under the
// lifecycle lock). Records are BLE_PhonePipeConsoleCore.h's.
//
// Locks, outermost first: the console chain mutex (L1, this module), the pipe
// owner mutex (L2), the BLE lifecycle lock (L3). L1 -> L2 is the only nesting;
// L3 is never held with L1 or L2, in either order. The pipe task never takes
// L1 and never waits for tx space (it is the only drainer). Reliable writes
// run on cmd_exec only; every other task writes TryNow.
#include "System_BuildConfig.h"
#include <cstddef>
#include <cstdint>

namespace hw1pipe { class Session; }

enum class PipeConsoleKind : uint8_t { Text, Blob, Card, Mirror };
// Reliable: one whole chain, waiting for space within the stall bounds (cmd_exec
// only; elsewhere it is downgraded). TryNow: the whole record now, or dropped.
enum class PipeConsoleMode : uint8_t { Reliable, TryNow };
// Sent: every record pushed. Dropped: TryNow found no room (counted).
// Gone: no matching open stream or row (nothing sent). Aborted: a chain was
// cut and ABORT sent (or the stream ended mid-chain). Refused: invalid call
// (kind/size, or the caller holds a pipe or lifecycle lock).
enum class PipeConsoleWrite : uint8_t { Sent, Dropped, Gone, Aborted, Refused };
struct PipeConsoleTarget { uint32_t generation = 0; uint16_t streamId = 0; };

// What the status surfaces show for the stream console.
enum class PipeConsoleState : uint8_t {
  Off,         // no pipe session, or the pipe is not built
  NotOffered,  // bleconsole off, or the phone secret is not a device-made code
  Waiting,     // offered; the phone does not want it (CONSOLE_WANTED 0)
  Opening,     // OPEN sent, no answer yet
  Open,        // OPEN_OK and the row is bound
  Closing,     // closing after drain
  Backoff,     // waiting to reopen after an error
  Stopped,     // not reopened again this session (OPEN_ERR 17, or 9 after backoff)
};
struct PipeConsoleStatus {
  PipeConsoleState state = PipeConsoleState::Off;
  bool offered = false, wanted = false;
  uint16_t streamId = 0;
  uint16_t lastClose = 0;  // the last CLOSE reason this P4 sent on slot 2 (0: none)
  uint32_t opens = 0, cardDrops = 0, mirrorDrops = 0, unknown = 0, malformed = 0, aborts = 0;
  // Since boot: ingress credit withheld while a whole record sat in rx and a
  // gate refused it (ms in all, and the longest single wait), and how many
  // records each gate held (once per record; one held by two gates in turn
  // counts under both).
  uint32_t withheldMs = 0, withheldMaxMs = 0, poolFull = 0, executorBusy = 0, rowUnbound = 0;
};

#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE
// Writers. epoch is the session epoch the output belongs to (uint32_t is
// TransportSessionEpoch); every record re-checks it against the bound row.
PipeConsoleWrite pipeConsoleWrite(const PipeConsoleTarget& target, uint32_t epoch,
                                  PipeConsoleKind kind, const uint8_t* data, size_t len,
                                  PipeConsoleMode mode);
// Owner hooks, pipe task, owner mutex held: the CONSOLE_OFFERED bit for HELLO
// and LINK_STATE, and one pass of the driver (open/reopen, INFO, ingress to
// cmd_exec, close mailbox, drain).
uint8_t pipeConsoleLocalFlags();
void pipeConsoleTickLocked(hw1pipe::Session& session, uint32_t generation, bool ready,
                           uint32_t now, bool consoleDisplayBusy);
// Pipe task after the owner mutex is released: binds or ends the BLE row.
void pipeConsoleAfterUnlock();
// Any task, never blocks (L3 may be held): end the stream with `reason`
// (2 renewed, 5 revoked) after its tx drains.
void pipeConsoleRequestClose(uint32_t generation, uint16_t streamId, uint16_t reason);
// The calling task holds the chain mutex (output paths must not wait on it).
bool pipeConsoleHeldByCurrentTask();
void pipeConsoleStatus(PipeConsoleStatus* out);
#else
inline PipeConsoleWrite pipeConsoleWrite(const PipeConsoleTarget&, uint32_t, PipeConsoleKind,
                                         const uint8_t*, size_t, PipeConsoleMode) {
  return PipeConsoleWrite::Gone;
}
inline uint8_t pipeConsoleLocalFlags() { return 0; }
inline void pipeConsoleTickLocked(hw1pipe::Session&, uint32_t, bool, uint32_t, bool) {}
inline void pipeConsoleAfterUnlock() {}
inline void pipeConsoleRequestClose(uint32_t, uint16_t, uint16_t) {}
inline bool pipeConsoleHeldByCurrentTask() { return false; }
inline void pipeConsoleStatus(PipeConsoleStatus* out) { if (out) *out = {}; }
#endif
