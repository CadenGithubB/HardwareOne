# OLED CLI Output: investigation and rewrite plan

Status: implemented in the working tree on 2026-11-02; see
[OLED CLI Output](OLED_CLI_OUTPUT.md) for the shipped behavior, storage
change, host tests and build results. The sections below are the investigation
that motivated it. Source inspected: `9674eb49` plus the working tree
(including the completed OLED feedback and Health work). Existing unrelated
working-tree changes are outside this plan.

## Recommendation and scope

Rewrite the CLI Output viewer and repair its console-buffer boundary first.
Keep the existing debug/output queue, authorized command executor, display HAL,
redaction rules and session-boundary ownership. A replacement renderer alone
cannot recover text discarded by the buffer or distinguish colliding timestamps.

The stopping condition is a reliable read-only stream browser: live following,
stable paused selection, complete retained-record reading, predictable Back,
height-aware layouts, and bounded memory/access times. This is not a terminal
emulator, a command editor, a replacement event notification system, or a rewrite
of global logging. CLI Input remains a separate command surface; update only its
console integration where the buffer contract changes.

## What exists and why

| Part | Existing purpose | Current behavior |
|---|---|---|
| `System_Debug` output queue | Share producer output across serial, web, files, OLED and other sinks | Drains queued envelopes into the OLED console. Normal command broadcasts/results always add the OLED sink; ambient/debug messages also use it. This is a mixed output stream, not a history of only commands entered on the display. |
| `OledConsoleBuffer`, declared in `OLED_ConsoleBuffer.h`, implemented in `OLED_Utils.cpp` | Bound RAM and retain recent output independently of the web mirror | Default 50 slots, configurable 10–100 at reboot; each slot retains only 63 text bytes and a millisecond timestamp. Allocated in internal DRAM for private results; cleared on display identity transitions. |
| CLI Output list, `OLED_Mode_CLI.cpp` | Browse recent entries while new ones arrive | Selects by timestamp and measures the viewport backward from the newest entry. Row count already grows with content height; rows preview the first 20 characters. Selection remains on the previous entry when new output arrives. |
| Detail view | Lock a message while inspecting it | Stores another timestamp, shows index and boot-relative time, then fixed 20-character slices. Up/Down selects other messages; there is no scroll position within the message. |
| Global footer and render scheduling, `OLED_Utils.cpp` | Provide consistent controls and avoid needless frame redraws | Footer reads console count/selection separately from the page. Console append does not invalidate the display. Hints describe only A/B and do not reflect Reader state. |
| CLI Input, `OLED_Mode_CLIInput.cpp` | Enter commands and show an immediate result | Appends a redacted echo and sometimes a fallback result. Its inline result truncation points users at CLI Output for the rest, which CLI Output may already have discarded. |

Important existing boundaries to preserve:

- No command execution or settings mutation in CLI Output.
- Existing command echo redaction and private-STT output redaction.
- Internal-memory custody and identity-boundary erasure of private history.
- The incoming stream is bounded: debug envelopes are at most 255 bytes;
  longer returned results are already split into envelopes by the command
  producer. A reader must not claim to reconstruct an entire command result
  when the stream supplies only individual chunks or has already dropped data.

## Confirmed defects

A temporary ASan/UBSan host probe compiled the shipping append/accessor and
viewer functions with controlled graphics, time and semaphore boundaries.
It reproduced these behaviors without writing to the device:

1. **Timestamps are not record IDs.** Append `alpha` and `beta` at timestamp
   1000, then press X. The viewer reports row 1 rather than newest row 2.
   Lookup always returns the first timestamp match, so navigation through a
   batch emitted in one millisecond can repeatedly resolve to the same row.
   Timestamp zero is also treated as no selection, despite being valid data.
2. **There is no live-follow state.** Select the latest record, append a newer
   record, and redraw: selection remains on the old one. X is only a one-time
   jump. The UI offers no indication that it is holding an older record.
3. **Data disappears before rendering.** A 100-character message retains only
   63 characters with no truncation marker. `first\nsecond` becomes
   `firstsecond`. CR/LF structure is discarded rather than represented.
4. **Sanitization is incomplete and unsafe.** ANSI escape bytes are removed,
   but their printable tails remain (`[31mred[0m`). A truncated UTF-8 lead byte
   `0xE2, 0x00` causes an ASan stack-buffer-overflow: the parser advances three
   bytes without checking the string boundary before reading again.
5. **Detail does not expand the underlying record.** Small OLEDs show at most
   two 20-character slices and have no way to scroll to the remaining stored
   text. P4 shows more slices, but cannot recover anything beyond the storage
   cap. Slicing does not respect words or preserve original line breaks.
6. **Detail does not actually pin content.** With a three-record ring, open
   `one`, then append `four` so `one` is evicted. Rendering silently replaces
   the open detail record with `four`. A reader should never change its body
   behind the user's back.
7. **Detail and list selection diverge.** Open row 1, browse to row 2 in detail,
   then close it: the list still selects row 1. The global footer also reports
   the list selection while a different record is open in detail.
8. **The renderer owns the producer mutex while drawing.** The probe observed
   graphics/text calls under the console lock. Appenders wait at most 10 ms,
   then silently discard output if they cannot obtain it. A snapshot boundary
   would remove drawing from that critical section.

Additional source-confirmed problems:

- `append()` has no generation/change notification; `oledIsDirty()` does not
  observe console changes. New output can remain invisible until input,
  telemetry, or another unrelated dirty mark causes a frame.
- Lock-failure rendering says `Buffer locked...` but does not schedule its own
  retry. An otherwise static display can retain that temporary message.
- The footer reads count and scans timestamps outside the renderer's lock;
  its index/count need not describe the frame just drawn.
- Scroll marks occupy the last preview character's column, are drawn at the
  opposite ends from their normal reading direction, and receive no reserved
  gutter. There is no visible truncation or loss count.
- `DEBUG_CLIF` traces of this viewer's own input/reset operations enter the
  same mixed OLED stream when CLI debug output is enabled, consuming history
  and potentially evicting what the user is trying to inspect.
- CLI Input detects newly captured output by comparing record counts. A full
  ring's count never increases; asynchronous queue delivery can also occur
  after that comparison. This cannot reliably decide whether to append a
  fallback and can duplicate command results.

## Proposed interaction

| State | Display | Controls |
|---|---|---|
| Live list | Latest visible records; explicit Live indicator | Up pauses on an older record; A opens it; X follows newest; Y goes to oldest retained; B leaves the page. |
| Paused list | Selection anchored by stable ID; count of arrivals since pause | Up/Down browse; A opens the selected record; X resumes Live; Y goes to oldest retained; B leaves. |
| Reader | Owned copy of one complete retained record, wrapped to screen width | Up/Down scroll text; Left/Right changes record; A/B returns to its list selection; X returns to Live. |

If a paused list record expires, select the nearest surviving record and show
that older output expired. If the record is already open, its owned Reader copy
remains readable until the user leaves it. Erase that copy at a display identity
boundary. Detail metadata must describe the opened record, not another cursor.

Use compact, state-specific footer hints; put counters and Live/Paused status
in a layout that actually fits, rather than printing an oversized hint string.
Retain timestamps as metadata only. Use display geometry for text columns,
visible rows and a reserved scroll-indicator gutter.

## Implementation milestones

### 1. Establish a trustworthy record owner

- Keep `OledConsoleBuffer` as the single history owner and move its
  implementation out of the broad OLED utility file in a behavior-preserving
  move before changing storage semantics.
- Give records monotonic IDs independent of timestamps; distinguish valid ID
  zero/sentinels deliberately. Publish a generation and retained ID bounds.
- Expose bounded owned snapshots for the visible window and selected record.
  Accessors must not expose borrowed mutable text or require screens to lock
  and scan internals. Footer uses the same prepared state as the content.
- Preserve newlines, sanitize within checked byte boundaries, retain complete
  incoming envelopes where budget permits, and record truncation/drop causes.
- Set a measured byte budget before changing allocation. Preserve internal
  memory custody and existing configurable history behavior; do not blindly
  allocate 100 full command-result buffers. If a bounded byte arena changes
  effective retention or setting meaning, document and test that change.
- Invalidate the display on generation changes and retry transient snapshot
  contention. Rendering and input must not hold the producer mutex.

Done when duplicate timestamps, rollover, malformed text, full rings, loss
accounting, clear and concurrent append/snapshot all have meaningful tests.

### 2. Replace the viewer state machine

- Replace the timestamp-based selection and two independent detail/list cursors
  with the Live / Paused / Reader model above.
- Reuse existing geometry and the shared `System_TextPager` wrap/sanitize core,
  as the OLED LLM reader already does. Do not create another text wrapper.
- Use the shared list mechanisms where they fit. `OledScrollState` holds 32
  items while console history supports 100: do not raise every menu's capacity
  or silently discard history just to use it. Keep any needed bounded window
  and ID mapping with the console owner.
- Remove superseded viewer/window/timestamp helpers and publish coherent,
  state-aware footer information. Keep own viewer diagnostics out of the stream
  being viewed, or explicitly suppress their OLED sink.

Done when new output follows only in Live, paused selection remains stable,
Reader text scrolls completely, and navigation/eviction never changes a pinned
message silently.

### 3. Integrate CLI Input and identity transitions

- Replace the CLI Input count comparison with generation/record identity or
  an explicit command-result handoff. Verify output appears once when the ring
  is full and when the asynchronous drain is delayed.
- Keep redaction at the existing source boundaries. Clear viewer state and
  every owned private snapshot when the local display identity changes.
- Verify queued old output cannot refill cleared private history after a
  transition. Current `DebugMessage` has timestamp/category/routing/text but no
  display-session tag; clearing a drained ring alone does not establish this
  guarantee. Establish the narrow OLED delivery fence needed by the observed
  paths, without redesigning unrelated transport authorization.
- Initially preserve the existing mixed stream and describe it accurately.
  Separate command/debug filters require real producer metadata; do not infer
  provenance or severity from arbitrary message text and promise isolation.

Done when logout/re-login and delayed command/output tests establish history
custody, and input results remain correct without changing execution authority.

### 4. Verify and deploy to the existing P4

- Execute production code with duplicate timestamps, timestamp zero, wraps,
  empty/full buffers, multiline and long records, ANSI/control input, malformed
  UTF-8, queue bursts, eviction while paused/reading, and lock contention.
- Render and inspect 64-, 88- and 128-pixel layouts, including the P4's color
  header/footer geometry. Test button/ring/touch navigation through existing
  input abstractions; no device-specific navigation fork.
- Build `p4-phone-pipe` and a display-disabled baseline in their existing build
  directories; record image and memory headroom. Run relevant command-routing,
  private-output and display-session regressions.
- App-only flash the verified connected P4, preserving storage, then physically
  check Live/Paused, long-message reading, bursts, Back, and session changes.
  Record build success separately from actual screen qualification.

## Open constraints to resolve during milestone 1

The present envelope stream lacks command grouping and strong origin metadata.
A first rewrite can reliably read complete retained envelopes without claiming
to reconstruct lost output or whole command runs. Source filters, grouping an
entire streamed command result, exporting history, search and a terminal emulator
are subsequent work unless a real caller requires them for this milestone.

No new worktree, build directory or firmware artifact was created for this
investigation. The diagnostic probe used temporary compilation files, which
were removed after recording these results.
