# ESP-NOW File Transfer — Integrity, Abort, and Staging Keys

Status: **IMPLEMENTED, builds green on FeatherS3, NOT committed, HARDWARE TEST
PENDING.** Three review rounds: a 5-auditor plan audit, a 4-lens review of the
implementation, and a 3-lens verification of the review fixes themselves.
Written 2026-07-25 against `main` @ 889f1b8.

---

## ROUND 3 — verifying the review fixes (2026-07-26)

The second-round fixes were themselves adversarially verified. Two of them were
wrong, one dangerously so. Corrections applied:

- **The teardown-parking fix (F2) opened a data-corruption hole.** Parking a
  streaming slot in COMPLETING keeps its `.part` handle open, and the staging
  path was keyed on `(peer, filenameHash)` only — so a retry of the same file
  legitimately allocated a *different* slot with the *identical* path. The
  parked slot's eventual abort (`releaseLocked` → `close()` +
  `removeGuarded(partPath)`) would then delete, or truncate under, the file the
  live transfer was streaming into. The receive CRC cannot catch this: it is
  accumulated over bytes **as they arrive**, never re-read from flash, so a
  clobbered staging file still renames into the inbox as "verified good."
  **Fix:** the staging path now carries the slot index —
  `.part-strm-<mac>-<hash>-<slot>`. Unique among live slots by construction,
  which also removes the previous reliance on cmd_exec FIFO ordering. The
  `(mac, hash)` prefix is retained so a future resume can still glob for a
  candidate partial. The RAM temp gained the msgId for the same reason.
- **The idempotent-FILE_START fix (F1) matched on msgId alone.** msgId is
  per-boot (counter restarts at 1), and the codebase already treats post-reboot
  msgId reuse as expected — it flushes the dedup ring on `bootChanged`. A peer
  that resets mid-transfer could therefore re-mint an id whose 30 s slot we
  still hold, and the new file would be poured into the old slot's buffer at
  the old slot's offsets, failing with the *wrong filename* in every operator
  message. **Fix:** a duplicate must match filenameHash + fileSize +
  chunkCount + chunkSize; anything else supersedes.
- **Supersede could destroy the incumbent and then fail.** Superseding a
  streaming slot with the table full killed the old transfer *and* rejected the
  new one (the sender is fire-and-forget — it does not try again). **Fix:**
  `fileSlotsAllocate` is now two-pass — survey first, act only once a free slot
  is known to exist; otherwise reject and leave the incumbent running.
- **A late chunk on a successfully-finalizing slot sent a spurious cancel.**
  `fileSlotsStreamAppend` treated "not RECEIVING" as "failed", so a MAC-layer
  duplicate arriving after FILE_END produced a `FILE_CANCEL` that the new abort
  matcher would honour — turning a transfer that *succeeded* into a reported
  failure. **Fix:** new `STREAM_APPEND_FINALIZING` result; the caller does not
  cancel or log on it.
- **Two unsynchronised reads of the abort slot** in `sendFileToMac` could yield
  `aborted=false` + `receiverKilled=true` — no ABORTED log, no FILE_END, returns
  false with no reason recorded. **Fix:** one snapshot feeds both.
- **The web ESP-NOW fetch poller was entirely dead** (pre-existing): its success
  branch tested `'File sent successfully'`, a string the requester never sees,
  and its failure branch tested lowercase `'failed'` against `"Failed to
  receive:"`. Both branches now match what `logFileTransferEvent` actually
  writes. My round-2 comment claiming this poller consumed the CLI error string
  was simply wrong and has been corrected — an `espnowfetch`'s sending side is
  the remote peer's FS_GET handler, which never runs that command.
- Also: a bounded-visibility WARN for a teardown that stays unqueueable,
  `v4h_file_start`'s header comment rewritten to the new conflict contract
  (it still described the removed PATH_BUSY), the FsList comment's reference to
  a non-existent `pull()` corrected, and USERGUIDE's `imagesend` synopsis fixed
  (the path is required, and the command is no longer fire-and-forget).

---

## POST-AUDIT REVISIONS (2026-07-26)

A 5-agent parallel audit verified every claim against the tree. All call-site
safety claims held (all 8 `sendFileToMac` consumers tolerate an honest `false`;
all run on cmd_exec_task, so the single global send-slot is sound). These
corrections changed the design:

1. **Drop `r1Crc32`; use `esp_crc32_le`.** `r1Crc32` is doubly compile-gated —
   the whole of System_R1_Protocol.cpp/.h body sits inside
   `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES` and CMake drops the .cpp when
   either flag is off — so Task 1/3 as written would couple ESP-NOW to BT+G2
   and break BT-off builds (invisible on feathers3/xiao, where both are 1).
   System_ESPNow.cpp already includes `<esp_crc.h>` (line 17) and uses
   `esp_crc32_le` twice (bond settings hashes, :6365/:7029). It is natively
   seeded (`crc = esp_crc32_le(crc, buf, len)`), ROM-resident, and
   `esp_crc32_le(0, buf, 0) == 0` preserves the empty-file invariant. The whole
   `r1Crc32Seed` refactor is deleted from Task 1. Both Task 1 and Task 3 use
   `esp_crc32_le`.
2. **FILE_START rejection sends nothing — Task 2's biggest dead-transfer class
   had no trigger.** `v4h_file_start` on allocation failure (BUSY/PATH_BUSY/
   ALLOC/TOO_BIG) only logs (System_ESPNow.cpp:4034-4044), and no-slot
   FILE_DATA is silently ignored (:4054-4057) — so a rejected-at-start transfer
   never generates the FILE_CANCEL the abort flag consumes, and the sender
   pumps the whole file into the void. Fix folded into Task 2: send
   `FILE_CANCEL(REJECTED)` from the FILE_START rejection branch and from the
   no-slot FILE_DATA branch (mirroring the existing per-chunk resend precedent
   at :4069-4074). New reason `FILE_CANCEL_REJECTED = 5`.
3. **Streaming CRC check must precede `fileSlotsStreamBeginFinalize` in the
   guard chain.** BeginFinalize flips the slot to COMPLETING; the else-branch
   teardown (`fileSlotsStreamFail`) no-ops unless RECEIVING and the sweep skips
   non-RECEIVING slots — so a CRC check placed after it would leak the slot,
   both PSRAM buffers, and the open handle permanently on mismatch.
4. **Third sender abort cause: short read.** `if (bytesRead <= 0) break;`
   exits the loop and still sent FILE_END success=1. The restructured loop
   derives `aborted` from `chunkIdx < totalChunks` on exit, covering all three
   causes (read failure, 3× send failure, receiver cancel).
5. **Abort flag is a msgId, not a bool.** `volatile uint32_t fileSendAbortMsgId`
   compared against the loop's own `transferId` makes the one theoretical
   stale-store race (handler preempted between check and store while transfer A
   ends and B begins) self-neutralizing: a late store of A's msgId can never
   equal B's transferId. Handler additionally gates on `fileSendInProgress`
   before touching the (stale-on-idle) peer/msgId fields.
6. **Suppress the cancel echo on sender-initiated aborts.** The receiver replies
   to FILE_END success=0 with a FILE_CANCEL of its own (streaming :4228, RAM
   incomplete :4243) — after Task 2 every abort would double-log. Cancels are
   now only sent when `fe->success != 0` (the sender still believes the
   transfer is alive). This also fixes the WRITE_FAILED mislabel on
   sender-aborted streaming transfers.
7. **The (peer,msgId) reset branch in `fileSlotsAllocate` is a latent
   use-after-free for streaming slots** — it calls `releaseLocked` on
   espnow_task, closing `gStreamFile[idx]` while cmd_exec may be mid-write; the
   module's own ownership comment (Files.cpp:86-94) and the timeout sweep
   (:673-677) both route streaming teardown through cmd_exec for exactly this
   reason. Task 3's rewritten conflict handling defers streaming teardown via
   `fileSlotsStreamFailLocked` and only inline-releases RAM slots.
8. **PATH_BUSY was doubly wrong**: it compared filename only across ALL peers
   (blocking two peers sending same-named files, though destinations are
   per-peer dirs) AND it self-blocked genuine retries for up to 30 s (a retry
   has a fresh msgId, so it missed the reset branch and hit PATH_BUSY against
   its own stale slot — silently, per #2). The rewritten check keys on
   (peer, filenameHash), treats a match as "old transfer dead or superseded"
   (fail it, allocate fresh), and drops the cross-peer rejection entirely.
   `kFileSlotErrPathBusy` is removed. This also makes the hash-collision check
   and the conflict check the same comparison — the plan's earlier claim that
   "the slot table's active-transfer checks catch the concurrent case" was
   fiction (nothing compared partPath).
9. **Hash the STORED filename, not the wire buffer.** `V4PayloadFileStart.filename`
   is char[64] with no guaranteed NUL from a hostile peer; hash with an
   explicit `strnlen(_, 63)` bound. Also bound the `%s` logs of `fs->filename`
   in the rejection path (`%.64s`).
10. **RAM temp cannot take the strm-style name**: `tmpPath[48]` is 2 bytes too
    small for `.part-strm-<12mac>-<8hex>`. RAM path uses the short form
    `.part-<12mac>-<8hex>` (45 B incl NUL) — and gains the MAC it lacked.
11. **Duplicate FILE_END re-entry guard**: `fileSlotsFindByMsg` matches
    COMPLETING slots, so a duplicated FILE_END could race the cmd_exec finalize
    job and emit a spurious cancel for a succeeding transfer. v4h_file_end now
    early-outs for streaming slots not in RECEIVING (new `fileSlotsIsReceiving`
    accessor).
12. **Timeout sweep count honesty**: `expired++` was unconditional while the
    `out[]` write was capacity-guarded — benign today (cap == kFileSlots == 4)
    but the return value could claim entries it never wrote. Count only what is
    written; teardown still happens regardless.
13. **`sendBondSettings` pumps automations.json even when the settings send
    failed** (ENABLE_AUTOMATION block not gated on `sent`) — contradicts the
    airtime rationale; now gated.
14. Small scope additions: `imagesend` help/cliHint text is stale the same way
    `espnowsendfile`'s is (claims async/no-result); the FsList comment at
    System_ESPNow_FsList.cpp:881-882 misdescribes sendFileToMac as having
    "per-fragment ACK waits"; the `cmd_espnow_sendfile` too-big comment claims
    FILE_START rejection "is NOT signaled back" (true before #2, false after).
    All updated in the same pass.

Bond-sync note from the audit worth keeping: the worker-side
`bondSettingsSent` / "*** SYNC COMPLETE ***" log is set at SUBMIT time
(System_ESPNow.cpp:8888), deliberately, and is NOT made honest by Task 2 —
verify bond sync on the MASTER's log, which gates on transfer-verified
`bondSettingsReceived`. Also pre-existing and accepted: FS_GET requesters learn
of a failed reply-transfer only via their own timeout sweep; and the responder's
3 s SETTINGS_DEBOUNCE equals the master's 3 s retry period, so a retried
SETTINGS_REQ landing inside the debounce window is eaten and recovery waits for
the next retry (converges; just slower).

Three independent fixes to the existing ESP-NOW file transfer path. Each is
separately landable and separately valuable. Together they are the prerequisite
for any resumable / automatic sync work — see "What this unblocks" at the end.

All file:line citations below were read directly from the tree while writing
this doc. Re-verify before implementing; this is a plan, not a record.

---

## Why these three, and why now

The trigger was scoping a "car device syncs its logs when it gets home" feature
(outbox manifest + resumable transfer). Investigating that surfaced three
defects in the *existing* transfer path that are worth fixing on their own
merits, independent of whether the sync feature is ever built:

1. The file transfer has **no integrity check whatsoever**.
2. The sender **cannot be stopped**, and doesn't stop itself, once a transfer
   is doomed — it burns the full remaining airtime either way.
3. The on-flash staging file is **named after the attempt, not the file**, so
   a retry can never find the partial work its predecessor left behind, and
   repeated failures leak orphan files.

None of these are regressions; all three are original behaviour. (1) and (2)
also make today's `espnowsendfile` / `espnowfetch` / bond sync more honest, so
they pay for themselves before any sync feature exists.

---

## Verified current state

Transfer shape (unchanged by this plan):

- Sender: `sendFileToMac()` (System_ESPNow.cpp:12949) — opens the file, emits
  `FILE_START`, then a `FILE_DATA` chunk loop (200 B data + 2 B chunkIndex =
  202 B = `ESPNOW_V4_MAX_PLAINTEXT` exactly), then `FILE_END`. Paced 15 ms per
  chunk plus 50 ms every 10.
- Receiver: `fileSlotsAllocate()` (System_ESPNow_Files.cpp:215) picks one of
  4 slots and routes by size against `kFileSlotMaxFileSize` (131072):
  - **≤128 KB — RAM path.** Whole file in a PSRAM buffer plus a `chunkMap`
    bitmap. `fileSlotsWriteChunk()` (:370) writes at `chunkIdx * chunkSize`,
    so it tolerates out-of-order and duplicate chunks. Written to flash once
    at `FILE_END`.
  - **>128 KB — streaming path.** Two 4 KB PSRAM half-buffers; espnow_task only
    memcpy's, cmd_exec_task does all flash. `fileSlotsStreamAppend()` (:493) is
    **strictly sequential** — it never uses `chunkIdx` for positioning, only as
    a sequence check (`< nextChunk` = duplicate, `> nextChunk` = gap → abort).
  - Both finalize by renaming the staging file to
    `/espnow/received/<senderToken>/<filename>` (:4124-4132).

Auth: `FILE_*` opcodes are gated `REQ_PAIRED | REQ_SESSION_ENC`
(System_ESPNow.cpp:4816-4819). Unchanged by this plan.

---

## TASK 1 — Compute and verify the file CRC

### Problem

`V4PayloadFileEnd` has carried a `crc32` field since it was defined
(System_ESPNow_Wire.h:585). The sender hardcodes it:

```c
endPayload.crc32 = 0;  // CRC not implemented yet
```
— System_ESPNow.cpp:13128

And the receiver never reads it. `v4h_file_end` (:4182) branches only on
`fe->success` and on chunk/byte accounting (`fileSlotsIsComplete`). So today the
*only* integrity guarantee is "the expected number of chunks and bytes arrived."
That catches loss but not corruption, and it is a counting argument, not a
content check.

This is survivable today because a transfer is one-shot: the streaming receiver
aborts on any gap, so a completed transfer is a correct in-order concatenation
by construction. It stops being survivable the moment bytes from two different
sessions are spliced (resume), which is exactly what the sync work needs.

### Design

Use the existing `r1Crc32` (System_R1_Protocol.cpp:82). It is table-driven,
MSB-first, init=0, no final XOR. It is **not** standard zlib CRC-32 — that is
fine, both ends call the same function; note it in a comment so nobody
"fixes" it later expecting interoperability.

`r1Crc32` is one-shot and cannot accumulate. Refactor to expose a seeded form:

```c
// System_R1_Protocol.cpp
uint32_t r1Crc32Seed(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint8_t idx = (uint8_t)(data[i] ^ ((crc >> 24) & 0xFF));
    crc = (crc << 8) ^ kR1Crc32Table[idx];
  }
  return crc;
}
uint32_t r1Crc32(const uint8_t* data, size_t len) { return r1Crc32Seed(0, data, len); }
```

This is exactly equivalent to today's implementation (which starts `crc = 0`
and applies no final XOR), so every existing R1 caller is bit-identical.
Declare `r1Crc32Seed` in System_R1_Protocol.h next to `r1Crc32` (:176).

**Sender** (`sendFileToMac`, System_ESPNow.cpp:12949):
- `uint32_t runningCrc = 0;` before the chunk loop.
- After each successful `file.read()`: `runningCrc = r1Crc32Seed(runningCrc, fd->data, bytesRead);`
  Accumulate over what was *read*, in read order — which equals send order.
- `endPayload.crc32 = runningCrc;`

**Receiver, streaming path:** accumulate incrementally, because the whole file
is never resident. Add `uint32_t runningCrc;` to `FileTransferSlotImpl`
(System_ESPNow_Files.cpp:38-77). It is zero-initialised for free — the struct is
`memset` on allocate (:279) and 0 is the correct CRC seed. Update it inside
`fileSlotsStreamAppend` alongside the existing memcpy (:548), *after* the
duplicate/gap checks so a duplicate chunk cannot corrupt the accumulator:

```c
s.runningCrc = r1Crc32Seed(s.runningCrc, data, dataLen);
```

Cost is ~200 table lookups per chunk against a 21 ms/chunk budget — negligible.
Expose `uint32_t fileSlotsGetRunningCrc(const FileTransferSlot*)` alongside the
existing getters (:619-642).

**Receiver, RAM path:** the whole file is in `dataBuffer` at `FILE_END`, so
one-shot `r1Crc32(dataBuf, recvBytes)` is fine. Worst case 128 KB of table
lookups on espnow_task — small next to the synchronous flash write that branch
already performs inline.

**On mismatch:** treat exactly like an incomplete transfer — do not rename the
staging file into place, send `FILE_CANCEL`, log `MSG_FILE_RECV_FAILED`, release
the slot. Add a reason code:

```c
FILE_CANCEL_CRC_MISMATCH = 4,   // System_ESPNow_Wire.h:594-598
```

and extend the `why` decode in `v4h_file_cancel` (:4397-4399), which currently
maps only 1/2/3 and would otherwise print "unknown".

Placement in `v4h_file_end`:
- Streaming branch (:4207): the success condition is
  `complete && fe->success && fileSlotsStreamBeginFinalize(slot)`. Add the CRC
  comparison to the guard, and extend the existing `else` branch's reason
  selection (currently `complete ? WRITE_FAILED : INCOMPLETE`) to report
  `CRC_MISMATCH` when that is the actual cause.
- RAM branch (:4235 onward): compare before the
  `if (fe->success && dataBuf)` block that dispatches bond magic filenames, so a
  corrupt `_settings_out.json` can never reach `processBondSettings`. This is a
  real hardening win beyond the sync use case — bond config currently rides the
  RAM path with no content check at all.

### Edge cases

- **Empty file.** `fileSize == 0` → zero chunks → CRC 0 on both sides. Matches.
- **CRC value of 0 is legal.** No sentinel semantics; both sides always compute.
  Do not special-case 0.
- **Flag day.** A pre-fix sender sends `crc32 = 0`; a post-fix receiver rejects
  every non-empty file. Both devices must be flashed together. Acceptable per
  the project's no-backwards-compat policy (the user erases before flashing),
  but it must be called out in the changelog because it is a *silent* symptom —
  transfers just start failing.

### Test

1. Unit-equivalence: assert `r1Crc32Seed(0, d, n) == r1Crc32(d, n)` for a known
   vector, and that a split accumulation
   (`r1Crc32Seed(r1Crc32Seed(0, d, k), d+k, n-k)`) equals the one-shot value.
   Guards the refactor.
2. Log the computed CRC at INFO on both ends. Send a small file (RAM path) and
   a >128 KB file (streaming path); confirm the values match and both land.
3. Forced-mismatch: temporarily XOR the sender's CRC behind a debug flag, prove
   the receiver rejects, emits `FILE_CANCEL_CRC_MISMATCH`, leaves no file in
   `/espnow/received/<token>/`, and leaves no orphan staging file.
4. Bond sync still completes (manifest + settings both ride the RAM path).

---

## TASK 2 — Stop pumping a dead transfer

### Problem

Two distinct failures, same symptom.

**2a — the sender ignores its own send failures.** System_ESPNow.cpp:13093-13110:

```c
bool sent = false;
for (int attempt = 0; attempt < 3 && !sent; attempt++) { ... }
if (!sent) {
  ERROR_ESPNOWF("[V4_FILE_TX] Chunk %u failed after 3 retries", chunkIdx);
}
chunkIdx++;   // <-- falls straight through and keeps going
```

After three failed attempts it logs an error and continues to the next chunk.
The receiver's sequential check will reject the resulting gap and kill the
transfer, but the sender transmits the entire remainder of the file first. And
`sendFileToMac` then `return true;` unconditionally (:13143).

Note what `sent == false` actually means: `v4_send_payload_smart` (:2245)
returns whether the frame was sealed and queued, not whether it reached the
peer. `onEspNowDataSent` (:5901) only increments aggregate counters and never
correlates status to a frame. So on-air loss is invisible to the sender —
`sent == false` is the *queue-level* failure only (no session, clerk full).
It is still worth acting on, but it is not the common case.

**2b — the sender ignores the receiver telling it to stop.** This is the bigger
one. When the receiver detects a gap it calls `fileSlotsStreamFailLocked` and
sends `FILE_CANCEL`. The sender's handler:

```c
static void v4h_file_cancel(const V4RxCtx& ctx) {
  ...
  BROADCAST_PRINTF("[V4_FILE_TX] file to %s FAILED on receiver ...");
  logFileTransferEvent(...);
}
```
— System_ESPNow.cpp:4392-4404

It logs. That is all. The chunk loop keeps running to completion. On a 343 KB
transfer that fails early, this is minutes of airtime spent on a transfer that
is already dead — airtime that is shared with heartbeats, ACKs and every other
peer on the channel.

### Design

**2a** — break instead of continue:

```c
if (!sent) {
  ERROR_ESPNOWF("[V4_FILE_TX] Chunk %u failed after 3 retries — aborting transfer", chunkIdx);
  aborted = true;
  break;
}
```

**2b** — give the cancel handler a way to reach the send loop. The state already
exists: `FileSendActiveGuard` (:12936) sets `gEspNow->fileSendInProgress` and
`fileSendPeer` for the whole `FILE_START`→`FILE_END` window. Add two fields
next to them (System_ESPNow.h:705-707, plus ctor entries at :892-894):

```c
uint32_t      fileSendMsgId;           // transferId of the in-flight send
volatile bool fileSendAbortRequested;
```

`FileSendActiveGuard` takes the transferId and clears the abort flag on
construction; `v4h_file_cancel` sets it only when **both** the peer MAC and the
msgId match the in-flight send. Matching on msgId as well as MAC matters — a
late `FILE_CANCEL` from a previous, already-abandoned transfer must not kill the
current one. The wire contract supports this: the cancel's header msgId
"correlates to the original FILE_START" (System_ESPNow_Wire.h:590-593).

The chunk loop polls the flag each iteration and bails. Cross-task communication
is a plain `volatile bool` written on espnow_task and read on cmd_exec_task —
the same pattern `fileSendInProgress` already uses, and the same reason it is
sound: single writer, single reader, no compound state.

**On abort (either cause):** close the file, send `FILE_END` with `success = 0`,
return `false`. The `success = 0` matters — it lets the receiver tear down
immediately instead of waiting out `kFileSlotTimeoutMs`. Verify the receiver's
handling of `success == 0` on **both** paths during implementation; the
streaming branch clearly falls into the abort `else` (:4207), the RAM branch
needs reading past :4249 to confirm it releases the slot rather than falling
through.

**Return-value honesty.** `sendFileToMac` must return `false` on abort. There
are 8 call sites, and they currently receive `true` in cases where the transfer
failed:

| Site | Caller |
|---|---|
| System_ESPNow.cpp:6569 | bond settings send |
| System_ESPNow.cpp:6597 | bond automations send |
| System_ESPNow.cpp:6693 | bond schema send |
| System_ESPNow.cpp:6788 | bond manifest send |
| System_ESPNow.cpp:13332 | `cmd_espnow_sendfile` (CLI) |
| System_ESPNow_FsList.cpp:890 | FS_GET reply transfer |
| System_ImageManager.cpp:648 | image send |
| OLED_Mode_FileBrowser.cpp:564 | OLED "send to peer" |

**Required audit step:** confirm none of these treat `false` destructively
(deleting a source file, clearing a pending flag that should retry, marking a
bond stage complete). Most only log, and the CLI path already branches on it —
this change makes those branches finally reachable. But the bond senders set
pending/retry state around these calls and must be re-read, not assumed.

Help text for `espnowsendfile` (System_ESPNow.cpp:15530) currently says
"synchronous local send; does not confirm peer accepted". Still true, but the
failure semantics change — update it to say a failure result now means chunks
could not be queued or the receiver cancelled.

### Test

1. Send a large file, pull the receiver's power mid-transfer. Sender should stop
   within one chunk of the receiver's `FILE_CANCEL` (or, with the receiver gone,
   ride out its own path) rather than running to completion. Time it against the
   current behaviour to confirm the airtime saving.
2. Force a gap: with debug logging on, confirm receiver gap → `FILE_CANCEL` →
   sender stops. Check `espnowsaturation` before/after to quantify.
3. `espnowsendfile` to a powered-off peer now reports failure rather than
   "success".
4. Regression: normal transfers of both sizes still complete; bond sync still
   reaches SYNC COMPLETE; a stale `FILE_CANCEL` from an old msgId does not kill
   a fresh transfer (drive by sending two files back to back).

---

## TASK 3 — Key the staging file by content, not by attempt

### Problem

The streaming staging path is built from the message id
(System_ESPNow_Files.cpp:305):

```
/espnow/received/.part-strm-<mac>-<msgId>
```

`msgId` comes from `generateMessageId()` (System_ESPNow.cpp:7298), an atomic
increment over `gEspNow->nextMessageId`, which is initialised to **1 on every
boot** (System_ESPNow.h:886). So the name identifies *the attempt*, not *the
file*. Consequences:

- **A retry can never find the previous partial.** Attempt 1 leaves
  `.part-strm-<mac>-00000191`; attempt 2 gets a different id and looks for a
  different name. Prerequisite blocker for any resume work.
- **Orphans accumulate under distinct names.** Every failed attempt at the same
  file leaves a differently-named staging file. `fileSlotsBootCleanup()` sweeps
  them, but only **8 per boot** — `char victims[8][96]` with `n < 8`
  (System_ESPNow_Files.cpp:193-195). More than 8 orphans means the surplus
  survives until a later boot, in a directory the user browses.
- **Reboot-reset ids will collide once partials are preserved.** The counter
  restarts at 1, so ids are reused across boots. Today that is nearly harmless
  (partials are deleted on failure and swept at boot, so there is rarely
  anything to collide with) and the open is `"w"` which truncates
  (:470). It becomes a live corruption path the moment resume preserves them.

There is also an over-strict check worth fixing in the same pass. The
`PATH_BUSY` guard compares filename only (:264):

```c
if (strncmp(s.filename, filename, sizeof(s.filename)) == 0) { ... PATH_BUSY ... }
```

with the comment "prevents two peers racing the same destination". But the
destination is per-peer — `/espnow/received/<senderToken>/<filename>` (:4124).
Two peers each sending `log.csv` do not collide on the final path, yet the
second is rejected. For a mesh where several devices ship same-named logs, this
is exactly the case that will occur.

The RAM path has a milder version of the same defect: its temp is
`/espnow/received/.part-%08lx` keyed on msgId with **no MAC at all** (:4291).
The exposure window is small (written only at `FILE_END`) but the naming should
be consistent.

### Design

Key the staging file on (peer, filename) instead of (peer, msgId):

```
/espnow/received/.part-strm-<mac>-<crc32(filename) as 8 hex>
```

Reuse `r1Crc32` over the null-terminated filename. The MAC stays in the name, so
the hash only needs to disambiguate files from one peer. Apply the same scheme
to the RAM path temp, adding the MAC it currently lacks.

Widen the `PATH_BUSY` comparison to `(peerMac, filename)` so it rejects only
genuine same-destination races, and keep the existing same-(peer,msgId)
re-`FILE_START` reset branch (:256-262) as-is.

Hash collision: two different filenames from the same peer sharing a CRC-32 is
~1 in 4 billion, and the slot table's active-transfer checks catch the
concurrent case. Accept it and note it in a comment.

### Explicitly NOT in this task

**Do not start preserving the staging file on failure.** `releaseLocked()`
(:114-128) keeps deleting it, and the boot sweep keeps running. Task 3 is a
naming change only.

This split is deliberate and load-bearing: re-key-without-preserve is safe and
immediately useful (retries stop leaking orphans), whereas preserve-without-
re-key is *worse than doing nothing* — it is the combination that turns reboot
id reuse into silent file corruption. If the resume work later adds
preservation, it must land on top of this task, never independently of it.

### Migration

None needed. The boot sweep matches on the `.part-` prefix regardless of suffix
(:200), so staging files left by pre-change firmware are still cleaned up.

### Test

1. Interrupt the same large file transfer 3× in a row. Confirm exactly one
   staging file exists between attempts (not three), and it is reused/truncated
   rather than accumulating.
2. Two peers send identically-named files concurrently. Both should now succeed
   into their own `/espnow/received/<token>/` directories — this is the
   `PATH_BUSY` fix; verify it was genuinely broken first so the test means
   something.
3. Confirm `files /espnow/received` is clean after a series of failures, and
   that boot cleanup still removes a pre-existing old-format orphan.

---

## Cross-cutting

**Build.** Must be built for **FeatherS3** — `OLED_Mode_FileBrowser.cpp:564` is
a `sendFileToMac` caller and is compiled out on XIAO configs, so an XIAO-only
green build proves nothing about Task 2's return-value change. See the
board-gating footgun. If the cert bundle races, `ninja -j1 x509_crt_bundle.S`.

**Landing order.** Task 3 is independent. Tasks 1 and 2 both touch the
`sendFileToMac` chunk loop and `v4h_file_end`, so land 2 before 1 to keep the
diffs readable — 2 restructures the loop's control flow, 1 then adds an
accumulator to the settled shape.

**Suggested commits** (per the project's plain-English, version-prefixed style,
and only with explicit approval after hardware testing):
- `vX.Y.Z: stop ESP-NOW file sends that the receiver has already given up on`
- `vX.Y.Z: verify a CRC on every ESP-NOW file transfer`
- `vX.Y.Z: name ESP-NOW staging files after the file, not the attempt`

**Changelog must call out the flag day** from Task 1 — both devices need
flashing together or transfers fail with a non-obvious symptom.

**Review process.** Per the workflow that worked on the pairing-mode change:
write the plan (this doc) → fan out parallel read-only auditors to verify every
claim here against the actual tree → fold corrections back in → implement →
one more adversarial pass on the real diff. Both checkpoints caught real bugs
last time. This doc has **not** been through step 2 yet.

---

## What this unblocks

With all three landed, resumable transfer becomes a small increment rather than
a redesign:

- Task 1 gives the integrity proof that makes splicing bytes from two sessions
  safe. Without it, resume silently produces corrupt files.
- Task 2 gives a transfer that stops promptly, which is what makes retrying
  cheap enough to be worth doing.
- Task 3 gives the stable name a retry needs to find its predecessor's work.

The remaining resume work is then: preserve the staging file on failure (with a
cap and age-out), seed `nextChunk`/`receivedBytes`/`receivedCrc` from the
recovered partial, round the resume point down to a chunk boundary and seek past
the ragged tail, and allocate `FILE_RESUME_OK = 117` — already reserved in the
opcode charter (System_ESPNow_Wire.h:67). Selective retransmit for the ≤128 KB
RAM path (`FILE_NACK = 116`, also reserved) is a separate, independent
increment that exploits the `chunkMap` bitmap that path already maintains.

Note that resuming needs **no new field in `FILE_DATA`** — that struct is at
exactly zero headroom (202 B, static_assert-pinned,
System_ESPNow_Wire.h:578-583), but it does not need one: `chunkIndex` already
encodes absolute position in both receive paths. Resume re-sends the original
chunk indices.

The outbox/manifest layer on top of that needs almost nothing new either —
`FS_LIST`/`FS_STAT`/`FS_GET` already exist, already return structured paginated
entries, and are gated `REQ_PAIRED | REQ_SESSION_ENC` rather than bond-gated
(System_ESPNow.cpp:4830-4834), so they work between any paired mesh peers. The
header comment in System_ESPNow_FsList.h claiming `REQ_BOND_MODE` is stale and
should be corrected whenever that file is next touched.
