# OLED CLI Output

**System → CLI Output** is a read-only browser of the local display's console
history. The history is the OLED sink of the shared output queue: every line a
command broadcasts while it runs (always), plus ambient and debug output when
those flags are enabled. It is one mixed stream, not a record of commands typed
on the display. Each record is one queue envelope of at most 255 bytes; a long
command result arrives as several records, and the queue's own `[CUT]` mark
shows where an envelope was clamped upstream. The page cannot recover output
the queue dropped and does not claim to.

The rewrite replaced the timestamp-selected list and its fixed detail popup
(see [the investigation](OLED_CLI_OUTPUT_PLAN.md)) with three explicit states.

## States and controls

| State | Shows | Controls |
|---|---|---|
| **Live** | The newest records; the status row reads `Live <count>` and adds `lost <n>` if any output was dropped by a busy ring | Up pauses on the record above the newest. A reads the newest. Y pauses on the oldest retained. X is a no-op (already following). B leaves the page. |
| **Paused** | The selection anchored by record ID while output keeps arriving; the status row reads `<index>/<count>`, then `+<n>` arrivals since the pause, then `expired` when the selected record was evicted | Up/Down browse (Down past the newest resumes Live). A reads the selected record. X resumes Live. Y selects the oldest retained. B leaves the page. |
| **Reader** | An owned copy of one complete retained record, wrapped to the screen; the status row reads `<index>/<count> T+<seconds>s`, `~` when the stored text was cut, or `expired T+…` once the record left the ring | Up/Down scroll by line (a wheel scrolls by its detents); past the last line Down continues into the newer record, and past the first line Up continues into the older record at its last page. Left/Right open the older/newer record directly. A or B return to the list on the record that was open (Live if it was opened from Live and is still the newest). X resumes Live. Y opens the oldest retained. |

The footer hint follows the state: `A:Read Y:Old B:Back`, `A:Read X:Live
B:Back`, and `↑↓:Scroll X:Live B:Back`. Entering the page always starts Live
on the newest output. The P4X-EYE's GPIO encoder produces only Up/Down (the
wheel) and A/B/X/Y, never Left/Right, which is why scrolling past either end
of a record is the record switch on that board.

Layout comes from the canvas geometry: one 8-pixel status row, then 8-pixel
record rows, with the first column reserved for the selection cursor and the
last column for scroll marks (`↑` older rows above, `↓` newer rows below). That
is 4 rows on the 128×64 OLED, 7 on the 128×88 Waveshare canvas and 12 on the
P4X-EYE's 128×128 canvas. List rows preview the first line of a record in 19
columns and end in `~` when the record holds more (a longer line or more
lines); the Reader wraps at 20 columns and keeps the record's own line breaks.

A paused selection whose record is evicted moves to the oldest surviving
record and says `expired` until the next navigation. A record open in the
Reader stays readable from its owned copy after eviction; it is never replaced
by another record. From an expired record, Left does nothing (nothing older
survives) and Right continues at the oldest survivor.

## Storage

`OLEDConsoleBuffer` (`OLED_ConsoleBuffer.h/.cpp`) is the single history owner.
Its ring arithmetic and text sanitizer live in the dependency-free
`OLED_ConsoleCore.h`, which host tests compile as it ships. Screens read owned
snapshots (a status, a window of row previews, or one record) under a 10 ms
bounded wait and draw only after the lock is released; the producer (the
output task) holds the lock for one sanitize-and-copy.

Records have monotonic IDs that are contiguous within the retained range, so
a selection is one subtraction away from its row and two records stamped in
the same millisecond are distinct. ID 0 is the only "no selection" value and
is never issued; when the counter would wrap, the history restarts at 1.
Timestamps are kept as metadata only. Every append and clear advances a
lock-free generation that the display's dirty poll compares with the last
rendered frame, so new output redraws the page without waiting for input.

Text is stored as the OLED font can draw it: `\n` is kept, `\r` dropped, tabs
become spaces, escape sequences are removed whole (parameters and final byte
included), other control bytes drop, valid UTF-8 folds to ASCII through the
shared `utf8FoldToAscii` (so `café` reads `cafe` instead of losing the word),
and malformed or truncated sequences drop byte by byte within checked bounds.

| Slots (`oledHistorySize`) | Former ring (64 B line + 4 B stamp per slot) | Now (12 B record + 64 B text budget per slot) |
|---|---|---|
| 10 (minimum) | 680 B | 760 B |
| 50 (default) | 3,400 B | 3,800 B |
| 100 (ceiling) | 6,800 B | 7,600 B |

The setting keeps its meaning and range (10–100, latched at init, reboot to
change) and the ring stays in internal DRAM. What changed is retention: a
record keeps its complete sanitized envelope (up to 255 bytes) instead of its
first 63 characters, and the text arena is shared, so a burst of long records
retains fewer than `oledHistorySize` records (two 255-byte records fill the
640-byte minimum arena). Records are evicted oldest first; the status row
counts retained records, and the Reader marks a record that was cut to fit an
envelope with `~`. Static cost of the page itself is 293 B of `.bss` (the
owned Reader copy) plus 26 B of hint text.

## Routing and privacy

The stream routing is unchanged: command output always reaches the OLED sink,
ambient and debug output reach it when enabled. Source filters and grouping a
whole command result need producer metadata the envelopes do not carry, so the
page does not infer them from text. The page keeps no instrumentation of its
own, so browsing never feeds the stream it displays (the former `DEBUG_CLIF`
traces are gone).

**CLI Input** hands its redacted command echo and the executor's reply to CLI
Output through the output queue, routed to the OLED sink only
(`broadcastOutputCore_Routed(..., MSG_ROUTE_OLED)`), instead of appending to
the ring directly and guessing from a record count whether the command had
produced output. The reply therefore appears once, after the lines the command
broadcast while running, whether the ring is full or the queue drains late.
A command that both broadcasts text and returns `OK` now shows both; the old
page suppressed the reply whenever any output had already arrived.

Every local-display identity transition resets the viewer (owned Reader copy,
selection and pause anchors) and clears the ring. `clear()` also advances the
console's delivery generation. The output queue captures that generation when
an envelope is queued (`DebugMessage::oledGeneration`, filled in
`enqueueChunk`) and the OLED sink refuses an envelope whose captured value is
stale, so envelopes the previous identity queued before logging out cannot
drain into the next identity's history. Timestamps stay on the record as
metadata and never decide ownership, so uptime and the millisecond rollover
cannot refuse valid output (the former timestamp fence refused everything
between 24.9 and 49.7 days after a boundary). A stale generation skips only
the OLED sink; serial, web, file, BLE and G2 keep their own gates. Command
echo redaction and private-STT redaction stay at their existing sources.

## Verification — 2026-11-02

Base revision `9674eb49` plus the working tree, which also carries the shared
OLED feedback and Health work; those changes are not part of this milestone.
No source copy or extra worktree was created.

Host tests (ASan/UBSan, run from the repository root):

- `python3 components/hardwareone/test/host/test_oled_console_buffer.py`
  executes the shipping core and owner: sanitizer bounds (ANSI/OSC escapes,
  controls, malformed and truncated UTF-8 including the former `0xE2 0x00`
  overflow, CRLF, trailing breaks, overflow accounting), a 6,000-append model
  check of the byte-budget ring against a reference queue, record flags,
  duplicate and zero timestamps, clock and ID wrap, clear keeping IDs,
  allocation and mutex failure, the latched 10..100 depth, lock-timeout drop
  counting, busy reporting, the delivery generation at the boundary tick,
  after 24.9 days and across the millisecond rollover, with boundaries racing
  producers,
  (and across millisecond rollover), and a 20,000-append producer thread
  against a snapshotting reader thread with no lock observed held afterwards.
- `python3 components/hardwareone/test/host/test_oled_cli_output.py` drives
  the shipping page over the shipping owner at 128×64, 128×88, 128×128 with
  10/10 chrome (the P4X-EYE canvas) and 128×128 with 12/16 chrome. It checks
  the empty page, Live following, two records in one millisecond, pause by ID,
  arrival counts, Down-past-newest, X/Y jumps, window marks and scrolling,
  expiry of a paused selection, a 104-character record read to its end, the
  Reader's status and marks, multiline records and `~` previews, Left/Right
  with the list returning on the open record, wheel detents scrolling several
  lines and Up/Down continuing into the adjacent record at either end (a spin
  stops at the end first; the newest and an expired record have nothing
  further), the pinned Reader after eviction, hostile text, a 300-byte envelope kept at 255 with its flag, busy
  ring notice plus retry frame, dropped-output reporting, session reset and
  entry. Its framebuffer double fails any draw outside the content area, any
  row that would wrap, and any draw while the ring's lock is held.
  `--preview-dir <path>` writes PGM renders.
- Both are registered as `oled_console_buffer` and `oled_cli_output` in the
  host CMake tree; see the host test README for the related suites run with
  them.

Firmware builds through `tools/build_config.py`, each in its existing
directory:

| Profile | Result | Image | Headroom |
|---|---|---|---|
| `p4-phone-pipe` (P4X-EYE, display on) | builds and links, no warnings in the changed files | 6,140,416 B | 1,285,632 B |
| `xiao-default` (display disabled) | builds and links | 4,390,480 B | 1,593,776 B |

A build is not physical qualification. On 2026-11-02 the image above was
written to the connected P4X-EYE (ESP32-P4 rev v3.2, MAC `02:48:57:31:04:73`,
`/dev/cu.usbmodem112101`, confirmed with `esptool.py read_mac` before any
write). The build directory is `HW1_OTA_LAYOUT=0`, so the app lives in the
`factory` partition at `0x10000`; only that partition was written, with the
built file rather than `idf.py app-flash`, so a concurrent rebuild could not
change the image:

```sh
esptool.py --chip esp32p4 --port /dev/cu.usbmodem112101 --baud 921600 \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x10000 build-config-p4-phone-pipe/hardwareone-idf.bin
```

NVS and LittleFS (settings, models) were untouched. The boot log captured over
the same port after a held-port reset shows boot #50 with `crashCount=0`,
`[EVENT][DISPLAY] P4-EYE ST7789 online (spi)`, the console ring allocated in
internal DRAM (`oled.console.text 3200 B` and `oled.console.records 600 B`,
both `ok:1 fail:0 fb:0 DRAM`, the 50-slot default), `[Boot] Setup complete`
at 21.2 s and no `[ERROR]` or `[WARN]` lines in 40 s. That establishes boot
and display/console initialization only.

**First on-device finding (user, 2026-11-02):** turning the wheel inside the
Reader did not move to the next or previous record. The P4X-EYE's GPIO
encoder emits only Up/Down for the wheel plus A/B/X/Y, never Left/Right, and
the first image switched records only with Left/Right. Fix in the same day:
Up/Down scroll by the wheel's detents and, at the first or last line, continue
into the adjacent record (older records open at their last page); Left/Right
remain the direct switch; the Reader hint became `↑↓:Scroll X:Live B:Back`.
The host suite covers the new behavior at all four geometries. The rebuilt
image in the table above was written with the same command; boot #51 shows
the same display and console init lines, `[Boot] Setup complete` at 21.4 s
and no `[ERROR]` or `[WARN]` lines in 40 s. The user then confirmed on the
device that scrolling in the Reader moves between records (passed). That
6,140,416-byte image contained the CLI Output tree only: the separate
list-text-bounds work (bounded labels, ellipsis, selected-row scrolling) was
built into `build-config-p4-phone-pipe` afterwards and has not been flashed,
so the directory's current binary is not the one on the device and the
device's menus still draw labels the old way.

**On-device qualification (user, 2026-11-02, boot #51):** every item below
passed on the P4X-EYE. The serial console of this board requires a login that
this run did not have, so `oledmode cli` and command bursts were driven from
the device rather than from the host.

- Live follows new output with the status row `Live <n>`: passed.
- Up enters Paused on the record above the newest; the row shows `+n` while
  output arrives and the selection does not move: passed.
- A opens the Reader on a long record; the wheel scrolls to its end with the
  `↑`/`↓` marks and then continues into the next record; scrolling up past
  the first line opens the previous record at its last page; B returns to the
  list on that record; X returns to Live; Y jumps to the oldest: passed.
- A burst (`help`) while Paused keeps the selection; a logout or login
  empties the page: passed.
- Footer hints read `A:Read Y:Old B:Back`, `A:Read X:Live B:Back` and
  `↑↓:Scroll X:Live B:Back`; the side buttons work for A/B/X/Y here: passed.
