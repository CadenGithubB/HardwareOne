// System_CatchupLedger.h - file catch-up, device side: the ledger of offered files
//
// The ledger gives every file this device offers to the masters named on it
// (FILES_IMPL_SPEC.md section 5) a file id that survives renames by rotation
// (X -> X.1), TimeAnchors' boot-folder promotion, a capped log's head trim and
// a card taken out and put back. It lives in PSRAM and in
// /system/catchup/ledger.json ("schema":1, written staged). The rules are the
// pure ones in System_ESPNow_CatchupCore.h; this part walks the folders,
// reads heads, keeps the trim notes and stores the result. It has no ESP-NOW
// dependency: System_ESPNow_Catchup.cpp serves LIST and READ from it.
//
// Threads: every change (reconcile, a delivery claim, a size seen by a READ)
// runs on cmd_exec under the ledger mutex (in PSRAM). Readers on other tasks
// take the mutex (catchupLedgerLock). The trim and restart notes come from the
// capped-log writer on any task, under a spinlock only: no I/O, no logging,
// no allocation (logging would re-enter the writer).
#ifndef SYSTEM_CATCHUP_LEDGER_H
#define SYSTEM_CATCHUP_LEDGER_H

#include "System_BuildConfig.h"

#include <stddef.h>
#include <stdint.h>

#if ENABLE_ESPNOW_SEND

#include "System_ESPNow_CatchupCore.h"

// cmd_exec only: loads or creates the ledger, then reconciles it with the
// files when due (the first call after boot, pending trim notes, a new SD
// mount, a forced reconcile, or a full LIST 30 s or more after the last
// one). Also persists `delivered` when it changed 5 min ago or more. False
// when the ledger cannot be loaded or created (the server answers not_ready).
bool catchupLedgerEnsure(uint32_t nowMs, bool fullListRequested);
// cmd_exec only: reconcile now, whatever the pacing (a READ found its file
// replaced or missing). False as for catchupLedgerEnsure.
bool catchupLedgerReconcileNow(uint32_t nowMs);

// The ledger under its mutex (bounded wait); nullptr if not loaded or busy.
// Every non-null return must be paired with catchupLedgerUnlock().
const espnowcatchup::LedgerState* catchupLedgerLock();
void catchupLedgerUnlock();

// cmd_exec: a named master said it stored every byte of `fileId` before
// `claim` (READ_REQ flag storedBefore, owner decision Q8). Raises
// `delivered` (clamped to the file's end), never lowers it. RAM only; saved
// lazily. False for an unknown id.
bool catchupLedgerNoteDelivered(uint32_t fileId, uint32_t claim);
// cmd_exec: a READ saw the file at `fileId` grown to `physicalSize` (same
// head): the ledger's size follows, so LIST ends keep up between reconciles.
void catchupLedgerNoteSize(uint32_t fileId, uint32_t physicalSize, uint32_t nowMs);

// ledgerId, gen and the end sum over offered files, for package 1's notice
// (BACKLOG_RESP reqId 0). Any task; zeros when the ledger is not loaded.
void catchupLedgerSummary(uint32_t* ledgerId, uint32_t* gen, uint32_t* endSum);

// The capture key is usable (sealing on send). Cached by catchupLedgerEnsure;
// any task.
bool catchupLedgerKeyReady();

// From appendLineWithCap (System_Filesystem.cpp) after a successful trim
// (the first `tailStart` bytes were dropped) and from restartCappedLog after
// the log was started over. Only the six capped logs the ledger follows are
// noted (events.log, errors.log, crash-history.log on either tier); any other
// path returns at once. No I/O, no logging, no allocation.
void catchupNoteTrim(const char* destPath, size_t tailStart);
void catchupNoteRestart(const char* destPath);

// Persist `delivered` now if it changed (a reboot intent). Bounded waits; it
// gives up rather than block.
void catchupLedgerFlush();

// cmd_exec: opens the file of `fileId` (its kind's read scope, under
// FsLockGuard), reports its size now and, with checkHead, compares its first
// headLen bytes with the ledger's head CRC; then reads up to `maxBytes` from
// physical offset `physOffset` into `out` (*got = bytes read). Missing: no
// such present entry, or the path does not open. Shrunk: the file is smaller
// than the ledger knew. Replaced: the head differs. IoError: a short read.
// The caller reconciles once on Missing, Shrunk or Replaced and checks again.
enum class CatchupReadResult : uint8_t { Ok, Missing, Shrunk, Replaced, IoError };
CatchupReadResult catchupLedgerRead(uint32_t fileId, bool checkHead, uint32_t physOffset, uint8_t* out,
                                    size_t maxBytes, uint32_t* sizeNow, size_t* got);

// For espnowcatchup. Any task.
struct CatchupLedgerStats {
  bool     loaded;
  bool     keyReady;
  bool     lastWalkCut;          // the last walk stopped at its entry budget
  uint16_t lastWalkEntries;      // directory entries the last walk looked at
  uint8_t  lastHeadReads;
  uint32_t reconciles;           // this boot
  uint32_t lastReconcileMs;      // how long the last reconcile took
  uint32_t lastReconcileAtMs;    // millis() when it ran (0: not yet)
  uint32_t saveFailures;
  uint32_t loadResets;           // a missing, unreadable or invalid ledger file started a new ledger
};
void catchupLedgerStats(CatchupLedgerStats* out);

#else  // !ENABLE_ESPNOW_SEND

// System_Filesystem.cpp calls these unconditionally.
inline void catchupNoteTrim(const char*, size_t) {}
inline void catchupNoteRestart(const char*) {}
inline void catchupLedgerFlush() {}

#endif  // ENABLE_ESPNOW_SEND

#endif  // SYSTEM_CATCHUP_LEDGER_H
