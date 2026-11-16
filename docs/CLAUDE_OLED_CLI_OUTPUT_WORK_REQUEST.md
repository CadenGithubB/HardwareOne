# Work request for Claude: rewrite OLED CLI Output

## Request

Implement a bounded rewrite of HardwareOne's **OLED CLI Output** page and repair
its console-buffer interface. The result must be a reliable, readable output
browser on both small OLEDs and the connected P4's larger logical canvas.

Read `AGENTS.md` and `CONTRIBUTING.md` first, then
[`docs/OLED_CLI_OUTPUT_PLAN.md`](OLED_CLI_OUTPUT_PLAN.md), which contains the
investigation, reproduced defects, architecture and proposed milestones. Inspect
the current implementation and its callers before choosing storage details.
Carry the implementation through code, meaningful tests, affected firmware
builds, documentation and cleanup. This request is for implementation, not
another analysis-only pass.

## Repository and working state

- Repository: `~/esp/hardwareone-idf`.
- Investigation base: `9674eb49` plus working-tree changes. Verify the actual
  current state; this revision is context, not a request to reset the checkout.
- The working tree contains substantial existing work, including shared OLED
  feedback, expanding Health/graphs, and possibly concurrent board changes.
  Preserve it. Do not revert, stash, overwrite or include unrelated changes in
  a cleanup merely because they are uncommitted.
- Reuse appropriate existing saved-profile build directories. Do not create a
  full source/dependency copy or extra worktree without an actual need.

## Required outcome

Replace the timestamp-selected list and fixed detail popup with three explicit
states:

| State | Behavior and controls |
|---|---|
| Live | Follow new output automatically. Up enters Paused; A reads the selected record; X follows newest; Y goes to oldest retained; B leaves the page. |
| Paused | Anchor selection by stable record ID while new output continues arriving. Show arrivals since pausing. Up/Down browse; A reads; X resumes Live; Y selects oldest retained; B leaves. |
| Reader | Display an owned copy of one complete retained record, wrapped to available width. Up/Down scroll text; Left/Right changes records; A/B returns to the corresponding list selection; X returns to Live. |

Use context-specific hints and geometry-derived row/column counts. Reserve space
for scroll marks. Opening a record must make all its retained text reachable.
A record already open in Reader must remain readable after ring eviction; it
must never silently turn into another message. If a paused list selection is
evicted, indicate expiry and choose the nearest surviving record predictably.O

## Known problems to fix

These were reproduced using shipping functions in a controlled ASan/UBSan host
probe; re-establish them as regression tests for the desired behavior:

1. Two records stamped at the same millisecond collide. X can select the first
   record rather than the newest, and navigation can remain on the same row.
   Timestamp zero is also incorrectly used as an invalid selection sentinel.
2. New output does not establish Live following and does not itself invalidate
   the display. A static page may not refresh until unrelated input/telemetry.
3. The buffer silently retains only 63 text characters per record. It flattens
   newlines (`first\nsecond` becomes `firstsecond`). Detail cannot recover lost
   text or scroll through all text that is retained.
4. ANSI escape bytes are discarded while printable tails remain. A malformed
   UTF-8 lead byte followed by NUL can make the sanitizer read past the string;
   ASan reproduced a stack-buffer-overflow with bytes `0xE2, 0x00`.
5. Detail identifies content by another timestamp. Eviction silently substitutes
   a different message; browsing in detail does not update the list selection
   shown after closing it. The footer can describe a different record.
6. Rendering holds the producer mutex while drawing. Appenders can time out
   after 10 ms and silently lose output. Lock-failure rendering does not
   schedule its own retry, and footer reads are not a coherent snapshot.
7. With CLI debug enabled, the viewer's own input/reset traces enter its history
   and can evict the output being inspected.
8. CLI Input checks whether the record count increased to decide whether to
   append fallback output. That fails for a full ring and races asynchronous
   output delivery, potentially duplicating results.

## Implementation boundaries

Keep the existing system output queue, command executor, display/input HALs,
notification system and redaction owners. CLI Output remains read-only. This
request does not introduce a terminal emulator, shell, command editor, search,
export, global logging rewrite or new event bus.

The current stream mixes command output and background/debug messages. Preserve
that routing initially and describe it accurately. Source filters and grouping
an entire streamed command require real producer metadata; do not infer reliable
provenance from arbitrary text or claim to reconstruct dropped output.

Reuse `System_TextPager` for text handling where appropriate, and inspect the
OLED LLM reader as an existing example. Reuse list geometry/navigation where it
fits. `OLEDScrollState` has 32 items while console history supports up to 100:
do not silently discard history or enlarge every menu to force a drop-in fit.

Preserve bounded internal-memory custody of private history. Current depth is
10–100 slots, default 50, latched at initialization. Measure memory before
choosing a record/byte-arena representation. Retain complete incoming envelopes
where budget permits; explicitly mark truncation and count losses. Do not
blindly allocate 100 full command-result buffers or spill private history into
PSRAM. Document and test any intentional retention/setting behavior change.

Queued debug envelopes are currently at most 255 bytes; longer returned command
results are split upstream. Distinguish complete retained envelopes from whole
command results and from upstream-truncated output.

## Suggested implementation sequence

### 1. Console owner and snapshot boundary

Keep one history owner. Move the `OLEDConsoleBuffer` implementation out of the
broad utility file as a behavior-preserving move, then change its interface:

- Stable monotonic record IDs independent of clock timestamps.
- Generation/change tracking, retained-ID bounds, and explicit loss accounting.
- Bounded owned snapshots for the visible window and selected record.
- Checked text sanitization that preserves meaningful line breaks and cannot
  walk past malformed or truncated UTF-8/control sequences.
- Short producer-lock holds; draw only after releasing the lock.
- Generation-driven display invalidation and bounded retry after contention.

### 2. Viewer replacement

Implement Live / Paused / Reader and responsive rendering. Keep list selection,
Reader identity, footer metadata and arrival counts coherent. Remove replaced
scroll/timestamp helpers and duplicate state after migration. Prevent viewer
instrumentation from feeding back into its own OLED stream.

### 3. CLI Input and session integration

Replace the count-based fallback heuristic with record/generation identity or
an explicit result handoff that behaves correctly with a full ring and delayed
queue drain. Preserve command authorization and audit behavior.

Preserve redacted command echoes and private-STT redaction. Clear history,
selection and owned Reader/snapshot text on local-display identity transitions.
Verify queued old output cannot refill cleared private history after logout or
replacement. Current `DebugMessage` contains timestamp/category/routing/text,
without a display-session tag; clearing a drained ring alone is not proof of
safe delayed delivery. Add the narrow OLED fence needed by the actual paths,
keeping unrelated transport authorization with its existing owner.

### 4. Verification and completion

Add focused tests that execute production code, covering:

- Duplicate timestamps, timestamp zero and clock rollover.
- Empty/full rings, wrap, generation changes and loss/truncation reporting.
- Multiline, long, ANSI/control and malformed UTF-8 input.
- New arrivals in Live and Paused, burst output, and eviction while reading.
- Lock contention, retry/invalidation, and drawing outside the producer lock.
- Reader scrolling, record switching, X/Y jumps and nested Back behavior.
- CLI Input results appearing once with a full ring/delayed drain.
- Identity changes, stale queued output, private snapshot erasure and redaction.
- Render bounds at 64/88/128 pixels, including actual P4 color header/footer
  geometry, plus enabled/disabled feature compatibility.

Run relevant existing text-pager, OLED layout, command/output routing and
session/privacy regression suites. Use sanitizers for the text/buffer/viewer
cases. Build `p4-phone-pipe` and the display-disabled `xiao-default` baseline
through `tools/build_config.py`, reusing their existing directories. Record
image headroom and memory effects; do not equate a build with physical testing.

For deployment when requested, verify the currently connected device and its
layout, then use app-only flashing to preserve settings/models. The last verified
device was a P4X-EYE with USB serial `02:48:57:31:04:73`, at
`/dev/cu.usbmodem112101`; these are historical identifiers to recheck, not an
instruction to assume a current port or flash another board. Record actual
screen/input qualification separately from boot/display initialization.

Update the implementation/verification notes and host-test instructions. Clean
up task-created disposable files; preserve existing/concurrent work and matching
working firmware/ELF/manifests. Report retained large build outputs and why.

## Completion report

Provide:

1. What changed and how Live / Paused / Reader work.
2. Intentional changes to retention, routing, controls or settings, if any.
3. Tests/builds actually run, results, memory/image headroom and limitations.
4. Any remaining physical qualification or deployment work.
5. A concise list of relevant files, without claiming unrelated working-tree
   changes as part of this request.
