# ESP-NOW File Streaming — Phase A: stream chunks to flash

> Status: **IMPLEMENTED + HW-VALIDATED 2026-06-20 — hybrid size-routing + deferred
> writer.** A 343238-byte file (1717 chunks @200B) transferred device→device intact
> (FeatherS3 pair, encrypted SESSION_FRAME): ~36 s, ~9.2 KB/s, every chunk attempt-1,
> **zero** gap/backpressure/short-write, file byte-verified by the operator. The first
> variant (inline flush on `espnow_task`) **failed deterministically at ~chunk 87** —
> exactly the §4.2 RX-stall hazard — so it was replaced with the deferred writer in §0.
> Sections 4–7 are the original fuller design; §0 records what shipped. Phases C–D
> (incl. selective retransmission — see §0 "Known limitations") remain future.

## 0. As-built (what shipped)

The implementation is **hybrid size-routing**, not "stream everything":

- **File ≤ 128 KB (`kFileSlotMaxFileSize`)** → the **original whole-file-in-PSRAM
  path, completely unchanged.** Every transfer that worked before works
  byte-for-byte. All bond config files (`_settings_out.json`, `automations.json`,
  …) are small → always here → their in-RAM `FILE_END` processing is untouched.
- **File > 128 KB** → **streamed** to `/espnow/received/.part-strm-<MAC>-<msgId>`,
  then renamed into place at `FILE_END`. Hard ceiling `kFileSlotMaxStreamSize` =
  **4 MB**.

This is **purely additive**: the streaming path only handles files that were
*rejected* before, so it cannot regress anything that works today.

**The writer is deferred to `cmd_exec`, NOT inline (this is the §4.2 fix).** The
first cut flushed the window **inline on `espnow_task`**. On HW that stalled the RX
drain during a NOR erase, the RX ring overflowed, chunks ~87–89 were dropped, and
the sequential-only stream couldn't recover → deterministic failure. The chunk-87
brick wall *is* §4.2's predicted hazard. As-built now:

- **`espnow_task` (`v4h_file_data` → `fileSlotsStreamAppend`) does ZERO flash.** It
  only `memcpy`s each 200-byte chunk into a **4 KB double-buffer** (two halves, 8 KB
  total — same as the old window, just split). When a half fills it's committed to a
  tiny FIFO and a **drain job** is handed to `cmd_exec_task` via
  `submitDeferredToCmdExec` — the *same* "defer FS off the RX task" pattern used by
  `SESSION_OPEN/CONFIRM/REKEY`. espnow flips to the other half and keeps draining the
  radio. One filled half = ~20 chunks (~430 ms) of runway to cover an erase.
- **`cmd_exec_task` does ALL flash:** lazy-opens the `.part` on first flush, appends
  each FIFO buffer in order, and at `FILE_END` runs the **finalize job** (drain
  remaining → close → rename → ACK → release).
- **All teardown is on `cmd_exec` too.** A streaming slot is only ever freed by its
  finalize/abort job. `espnow_task` and the heartbeat **timeout sweep** never free a
  streaming slot directly — they queue a job. Lifecycle: `RECEIVING → COMPLETING
  (job queued) → FREE`.
- **Fail-fast on gap.** A lost chunk (gap), writer-backpressure (both buffers full —
  `cmd_exec` fell a whole buffer behind), or a full job queue **aborts immediately**:
  `FILE_CANCEL` to the fire-and-forget sender + queued cleanup. No more 30 s zombie
  slot. **Never corruption** — sequential-chunk + byte-count gating catches any gap.

**Sender fix (independent, same change):** `sendFileToMac` used to hold the global
`FsLockGuard` for the **entire** multi-second send loop, starving every other FS
user (web, OLED, a concurrent inbound stream) for ~20–30 s. It now opens + sizes
under the lock, **releases it**, and re-takes it only briefly around each chunk
`read()`. The `File` handle stays valid across the gaps (it's a private local).

**Concurrency invariants (as-built):**
- The slot mutex guards **only the buffer/FIFO metadata** (fills, FIFO indices,
  `drainQueued`, slot state) — tiny critical sections. The actual `.part` write
  happens **outside** the mutex (the buffer being written is FIFO-`PENDING`, which
  `espnow_task` never touches), so a flash write never blocks an append.
- Lock order is always **slot-mutex → FsLockGuard**, and the two are **never nested**
  in the drain path (each is taken and released separately) → no inversion.
- `cmd_exec` is single-threaded, so a slot's drain/finalize/abort jobs run **FIFO**;
  the teardown job is always the slot's last job → frees with nothing else referring.
- Streaming requires **strictly sequential** chunks (no random-access buffer); the
  sender transmits in order, so a gap only happens on a genuinely lost frame.

**Known limitation:** sending and receiving a *big* file simultaneously on one
device is stressy — both lean on `cmd_exec`/the FS lock. Documented, not fixed.

---

_The sections below are the original fuller design, kept as reference for Phases
C–D. §4.2's "writer task" is realized here as **`cmd_exec` jobs** rather than a
dedicated thread — the user's call, to avoid a new always-on task on a DRAM-tight
board (reusing `cmd_exec` costs no extra stack)._

## 1. Why

Today an incoming file is **buffered whole in PSRAM** and written to disk in one
shot at `FILE_END`. The per-file cap (`kFileSlotMaxFileSize`, currently **128 KB**
in [`System_ESPNow_Files.h`](../components/hardwareone/System_ESPNow_Files.h)) is
just the size of that PSRAM buffer × 4 concurrent slots. The device's LittleFS
partition is ~**11 MB** free — so we're refusing files for lack of *RAM*, not
storage.

**Phase A** streams each chunk straight into a `.part` file on flash instead, so a
transfer is bounded by **free flash**, not the PSRAM buffer. This was the original
intended design — the slot header even says so:

> *"The plan called for a dedicated file_writer_task that drains 4 KB buffers to
> disk as they fill. We're shipping the simpler 'accumulate full file in PSRAM
> then write once' model for now … The writer-task pattern is a follow-up if file
> sizes ever grow past the budget."*

## 2. Current architecture (what exists)

**Sender** ([`cmd_espnow_sendfile`](../components/hardwareone/System_ESPNow.cpp) →
[`sendFileToMac`](../components/hardwareone/System_ESPNow.cpp)):
- Pre-check: `fileSize > kFileSlotMaxFileSize` → refuse up front with a clear error.
- `FILE_START` (ACK), then `FILE_DATA` × N (**200 B each**, paced, 3 *blind*
  retries, **no per-chunk ACK**), then `FILE_END` (ACK).

**Receiver** (`v4h_file_start` / `v4h_file_data` / `v4h_file_end` in
[`System_ESPNow.cpp`](../components/hardwareone/System_ESPNow.cpp)):
- `fileSlotsAllocate()` → **`ps_alloc` a PSRAM buffer of `fileSize`** + a chunk
  bitmap; slot = RECEIVING.
- `fileSlotsWriteChunk()` → `memcpy(buffer + chunkIdx*200, data)`; set bitmap bit.
- `fileSlotsIsComplete()` (bitmap) at `FILE_END`; if complete, **write the whole
  buffer** to `/espnow/received/<token>/<filename>`, then `fileSlotsRelease()`.

**Reliability:** blind retries + bitmap completeness; an incomplete transfer fails
(no selective retransmit) → `v4_send_file_cancel(INCOMPLETE)`. Stale slots are
reclaimed by `fileSlotsTimeoutSweep()` → `FILE_CANCEL(TIMEOUT)`.

**Measured rate:** ~**9 KB/s** over the air (75,853 B took ~8.2 s in a real log) ≈
46 chunks/s ≈ **~21 ms between chunks**. Hold that number — it drives the design.

## 3. What changes in Phase A (only the receiver + the cap)

The **entire sender side is untouched.** Four things change:

| # | Where | Today | Phase A |
|---|---|---|---|
| 1 | cap / pre-check | `> 128 KB` (PSRAM) | check against **free flash** |
| 2 | `fileSlotsAllocate` / FILE_START | `ps_alloc` whole-file buffer | **open a `.part` file** + small bitmap |
| 3 | `fileSlotsWriteChunk` / FILE_DATA | `memcpy` into RAM | **buffer ~4 KB → flush to `.part`** |
| 4 | `v4h_file_end` finalize | write whole buffer to flash | **rename `.part` → final** (already on disk) |

## 4. Doing it right — the critical details

These are the difference between "Phase A works" and "Phase A is slow and wears
flash." All four matter.

### 4.1 Buffer to one flash block (~4 KB) — never write per-200-byte-chunk
A naive per-chunk write makes LittleFS touch a whole **~4 KB erase block per
200 B** → **~20× write amplification** (a 200 KB file → ~4 MB of flash churn):
slow *and* wears flash. Instead, accumulate ~4 KB in the slot and flush an
**aligned block** when full (and the tail at `FILE_END`). This is the "4 KB buffer
drain" the original plan named.

### 4.2 Keep flash latency OFF the receive task
`v4h_file_data` runs inline on **`espnow_task`** (the RX drain). A NOR **block
erase is ~tens of ms**, which is *longer* than the ~21 ms inter-chunk gap — so a
synchronous flush on the RX path **stalls reception and drops incoming frames.**

Fix: a small **`file_writer_task`** (FreeRTOS) that owns the VFS writes. The RX
handler fills a 4 KB buffer and hands the *full* buffer to the writer via a queue,
then keeps receiving into a **second buffer** (double-buffer per slot). The writer
flushes A while RX fills B. Flash latency never touches the radio path.

### 4.3 Out-of-order chunks
The sender transmits **in order** (chunkIdx 0,1,2…); out-of-order only arises from
a lost-then-retried chunk (rare with blind retries). Buffer the **contiguous run**;
if a chunk lands ahead of the buffer's high-water, `seek`-write it directly into
the `.part` (accept the rare amplification) — the **bitmap still gates completeness
at `FILE_END`**. (Phase B's windowing makes out-of-order common → revisit the
buffering then.)

### 4.4 `.part` lifecycle + boot cleanup
- Write to a staging path (e.g. `/espnow/received/.part-<peer>-<msgId>`); **rename
  to the final name on complete** (atomic-ish; readers never see a partial file).
- On incomplete / cancel / timeout → **delete the `.part`**.
- Boot cleanup of stale `.part` files **already exists** (`fileSlotsBootCleanup`) —
  keep it. Phase A adds no resume, so a stale `.part` is simply discarded.

### 4.5 The cap / pre-check → and close the silent-failure gap
- `kFileSlotMaxFileSize` becomes free-flash-based, not a fixed RAM budget.
- The **sender can't know the receiver's free space**, so the authoritative check
  moves to the receiver: `fileSlotsAllocate` rejects at `FILE_START` if
  `freeFlash < fileSize + margin`.
- **Signal it back:** add `FILE_CANCEL(NO_SPACE)` so the sender reports the real
  reason instead of streaming the whole file and falsely "succeeding" — the same
  silent-failure class we already fixed for the size cap. (Keep a generous fixed
  sender-side sanity cap too, e.g. a few MB, so absurd sends die instantly.)

## 5. Wear analysis (why it's fine)

**Phase A writes the *same total* to flash as today** — the file, once — just
spread across the transfer instead of one burst at `FILE_END`. It is **not** more
flash traffic; the only risk is the §4.1 amplification, which block-buffering
removes.

Concrete, for this board (Winbond NOR, ~100 K erase cycles/block, ~11 MB LittleFS
partition with wear leveling):
- ~11 MB ÷ 4 KB ≈ **2,900 blocks** × 100 K cycles ≈ **~290 GB** of writes before
  wear-out.
- A block-buffered 200 KB transfer ≈ **~50 block writes** → ~1.5 **million**
  200 KB transfers to wear the partition out (≈70 K even at the bad 20× rate).
- The device already writes logs/settings/CSVs to this same flash constantly; a
  transfer is the **same order of magnitude**, not a new category of abuse.

→ Non-issue for occasional transfers. Continuous large streaming is the only
concern, and the answer there is **Phase D (SD card)** — wear lands on a cheap,
replaceable, huge card (also faster than ESP-NOW).

## 6. Effort & risk

- **Moderate, receiver-side, self-contained.** The bulk is the writer task +
  double-buffer + `.part` lifecycle. Sender unchanged.
- **Main risk:** the §4.2 RX-stall hazard. Get the double-buffer + writer task
  right or you drop frames → incomplete transfers. This is *the* thing to test
  (send under concurrent mesh/web load and confirm zero dropped chunks).
- Payoff: 128 KB → ~11 MB, ~80% of the practical "big file" benefit.

## 7. Concrete change list

- [`System_ESPNow_Files.h`](../components/hardwareone/System_ESPNow_Files.h):
  - `FileTransferSlotImpl`: replace `dataBuffer`/`dataBufferLen` with a **VFS file
    handle** + **two ~4 KB write buffers** + fill index. Keep the chunk bitmap.
  - `kFileSlotMaxFileSize` → free-flash-based limit (or a generous sanity cap).
- [`System_ESPNow_Files.cpp`](../components/hardwareone/System_ESPNow_Files.cpp):
  - `fileSlotsAllocate`: open `.part` (no `ps_alloc` buffer); check free flash.
  - `fileSlotsWriteChunk`: append to the active 4 KB buffer; on full/gap, enqueue
    to the writer task; update bitmap/bytes/chunks.
  - new `fileSlotsFinalize`: flush tail, close, **rename `.part` → final**.
  - `fileSlotsRelease`: close + **delete `.part`** if not finalized.
  - new **`file_writer_task`**: drains a queue of (slot, buffer, offset, len) → VFS
    writes; signals back on write failure (→ `FILE_CANCEL(WRITE_FAILED)`).
- [`System_ESPNow.cpp`](../components/hardwareone/System_ESPNow.cpp):
  - `v4h_file_end`: call `fileSlotsFinalize` instead of the whole-buffer write.
  - `fileSlotsAllocate` no-space path → `v4_send_file_cancel(NO_SPACE)` (new reason).
  - `cmd_espnow_sendfile`: relax the pre-check to the new (much higher) sanity cap.
- [`System_ESPNow_Wire.h`](../components/hardwareone/System_ESPNow_Wire.h): add
  `FILE_CANCEL_NO_SPACE` reason code.

## 8. Out of scope for Phase A (future phases)

- **Phase B** — windowed ACKs + selective retransmit, and **32-bit chunk/offset
  addressing** (today's 16-bit `chunkIdx` caps addressing at ~13 MB regardless of
  flash). Wire-protocol bump.
- **Phase C** — **resumption**: persist the `.part` + bitmap across a drop/reboot
  (a long transfer *will* hit the session desync / RF loss); a `RESUME`/`STAT`
  handshake so a transfer continues instead of restarting from zero. This is the
  make-or-break for *truly* unlimited.
- **Phase D** — stream to **SD card** when present, for GB-scale beyond the
  LittleFS partition (and to move wear off internal flash).

A dropped transfer in Phase A still **restarts from chunk 0** — acceptable at
~11 MB, not at 100 MB. Resumption (C) is what makes large sizes practical.
