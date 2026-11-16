/**
 * OLED Console Buffer - the local display's console history owner
 *
 * One bounded ring of recent output for the CLI Output page, independent of
 * the web mirror. The output queue appends every envelope routed to the OLED
 * sink (command output always, ambient/debug output when enabled); screens
 * read owned snapshots and never borrow ring storage or hold its lock.
 *
 * Memory: the struct itself sits in .bss; init() allocates the ring in
 * INTERNAL DRAM, sized by gSettings.oledCliHistorySize (10..100 slots, default
 * 50, latched at init so the setting needs a reboot). Each slot costs 12 bytes
 * of record index plus 64 bytes of shared text arena: 3,800 B at the default,
 * 7,600 B at the ceiling. Internal memory is a requirement, not a preference:
 * the ring holds identity-private command results, so it is never spilled to
 * PSRAM, credential-bearing command echoes are redacted before they reach it,
 * and clear() erases it on every local-display identity transition.
 *
 * Delivery generation: clear() also advances the ownership generation of
 * pending OLED output. The output queue captures it when an envelope is
 * queued (enqueueChunk) and append() refuses an envelope whose captured value
 * is stale, so envelopes the old identity queued before logging out cannot
 * drain into the next identity's history. Timestamps are display metadata;
 * they never decide ownership, so uptime and clock rollover do not matter.
 *
 * Only init() needs the display: it runs after display initialization
 * succeeds, and a device without a working display keeps the ring unallocated
 * (every operation then reports "no history" instead of faulting).
 */

#ifndef OLED_CONSOLEBUFFER_H
#define OLED_CONSOLEBUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <atomic>

#include "OLED_ConsoleCore.h"

class OLEDConsoleBuffer {
 public:
  OLEDConsoleBuffer();

  // Allocates the ring and its mutex once; a later call leaves a live ring
  // alone (the output task may already be appending to it).
  void init();
  // Ring and mutex both live. Appends and snapshots report "nothing" otherwise.
  bool isReady() const;

  // Producer side (output task): sanitizes and stores one envelope. Waits a
  // bounded time for the ring; a timeout drops the envelope and counts it.
  // `deliveryGeneration` is the value deliveryGeneration() returned when the
  // envelope entered the output queue; a stale value is refused and counted.
  void append(const char* text, uint32_t timestampMs, uint32_t deliveryGeneration);

  // Identity boundary (display task): erases every record and advances the
  // delivery generation. Blocks until the ring is free.
  void clear();

  // Ownership period of pending OLED output. Producers read it at enqueue
  // (one relaxed atomic load, safe in interrupt context); clear() advances it
  // under the producer mutex, so append()'s comparison is ordered with the
  // erase. 0 until the first boundary and on an owner that never allocated.
  uint32_t deliveryGeneration() const;

  // Lock-free change counter for the display's dirty poll: differs from the
  // last rendered value whenever a frame would show something new.
  uint32_t generation() const;
  // Appends lost to a busy ring and to a stale delivery generation (lifetime counts).
  uint32_t droppedCount() const;
  uint32_t fencedCount() const;

  // Bounded-wait snapshots for screens. Each returns false when the ring is
  // busy or unallocated; callers draw from the copies after the call returns,
  // never under the producer lock.
  bool tryStatus(oled_console::Status& out);
  // `busy` (optional) separates a contended ring from a record that is gone.
  bool tryCopyRecord(uint32_t id, oled_console::RecordCopy& out, bool* busy = nullptr);
  // Previews of up to `rows` consecutive records from `firstId`; returns the
  // rows copied (0 when busy, unallocated, or `firstId` is not retained).
  size_t tryCopyWindow(uint32_t firstId, size_t rows, size_t cols,
                       oled_console::Preview* out, bool* busy = nullptr);

 private:
  bool lock(uint32_t waitMs);
  void unlock();

  oled_console::Ring ring_;
  oled_console::Record* records_;
  char* arena_;
  void* mutex_;  // SemaphoreHandle_t, kept opaque so this header stays dependency-free
  std::atomic<uint32_t> generation_;
  std::atomic<uint32_t> dropped_;
  std::atomic<uint32_t> fenced_;
  std::atomic<uint32_t> delivery_;  // advanced by clear() while mutex_ is held
};

// Global OLED console buffer instance (defined when the display is compiled in).
extern OLEDConsoleBuffer gOledConsole;

#endif  // OLED_CONSOLEBUFFER_H
