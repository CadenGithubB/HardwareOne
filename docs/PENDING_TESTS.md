# Pending tests

Tracking list for things wired up but not yet verified end-to-end on
real hardware. Items move out of this file once they pass a manual
test pass and any findings are recorded in the relevant feature doc.

## OLED file browser — SD card support

**Status:** code path added (shares `FileManager` with the G2 hijack
file browser, which IS verified). OLED browser itself untested against
SD.

**Why it should work:** `System_FileManager.cpp::loadDirectory` now
calls `VFS::listVirtualEntries` to surface the `/sd` mount point at
the LittleFS root, regardless of which UI is consuming the
`FileManager`. The G2 hijack Files page renders this correctly. The
OLED browser uses the same class.

**Test steps:**
1. Boot with SD card inserted.
2. Open the OLED file-browser mode (whatever the input gesture is on
   the current build — check `OLED_Mode_FileBrowser` for the entry
   path).
3. Confirm LittleFS root listing still works as before.
4. Confirm an `sd` entry appears at the top of the LittleFS root.
5. Navigate into it; verify SD root contents render with correct
   names (no silent empty listing — the prefix-strip fix in
   `loadDirectory` handles this, but the OLED scroll/select code path
   hasn't been exercised against an SD-mode `FileManager`).
6. Drill into a subfolder; confirm entries render.
7. Use the "back" / "up" gesture to return to LittleFS root.
8. Boot WITHOUT SD card; confirm the `sd` entry is suppressed.

**What to flag if it breaks:**
- Empty SD listing despite the card being mounted → the prefix-strip
  in the OLED-side row formatter (separate from `FileManager`) is
  still using `dirPath` instead of `fsDirPath`.
- Cursor lands on `sd` but Enter doesn't navigate → the OLED
  navigateInto path may be checking something that the synthetic
  entry doesn't satisfy (e.g. `permissions & PERM_READ` — synthetic
  `/sd` entry is `PERM_READ` only via `lookupRule`).
- Crash on entering SD subfolder → likely a path-buffer overflow
  somewhere in the OLED renderer; check `FILE_MANAGER_MAX_PATH = 128`
  is respected by all consumers.

## G2 hijack TEXT view — JSON pagination

**Status:** code shipped (split into `JSON_PAGE_BODY_BUDGET = 180 B`
chunks at line boundaries, first page via CREATE-text, subsequent via
REBUILD_PAGE on tap, wrap at end, SysEvent gestures exit).

**Test steps:**
1. Hijack → Settings → flip `View: PRETTY` to `View: JSON` once
   (persists for the rest of the session).
2. Drill into a small module (e.g. `[crash] (2)`) — should render
   "[1/1] tap to back" header (single page).
3. Drill into a large module (e.g. `[espnow] (42)`) — should render
   "[1/N] tap=next, 2x-tap=exit" header.
4. Tap the lens (single tap) — should advance to page 2.
5. Cycle through all pages; final page should wrap back to page 1.
6. Double-tap to exit — should land back at the module list.
7. Watch logs for any "TEXT view: ignoring user-activity inside grace"
   spam — expected on render, but should stop after the first 600 ms.

**What to flag if it breaks:**
- TEXT view auto-dismisses on page-render → grace timer not getting
  re-stamped after `gTextViewTapFn` returns. Check
  `Optional_EvenG2.cpp` USER_ACTIVITY dispatch.
- Page never advances on tap → `gTextViewTapFn` not wired up. Verify
  the second arg passed to `g2ShowTextPage` in
  `renderCurrentJsonPage`.
- Page advances but content is identical → `g2ShowText` is going
  through the wrong path (CREATE instead of REBUILD); check
  `arm->containerReady` state.
- Single-fragment overflow on a specific module's page → `body`
  exceeds ~250 B pb. Lower `JSON_PAGE_BODY_BUDGET` to 150.

## G2 protocol probes

**Status:** wired, untested against firmware response.

### `g2aiconfig [voiceSwitch] [streamSpeed]`

Send a single Cmd=10 EvenAI CONFIG message. Watch logs for the next
sid=0x07 inbound frame; the COMM_RSP errorCode tells us:
- `errorCode=0` → firmware accepted; we guessed the schema right.
- `errorCode=1` (or any other) → field number guess is wrong; iterate.

Test calls to try (one at a time, watching the response):
- `g2aiconfig` (no body; does empty CONFIG ack?)
- `g2aiconfig 0` (voiceSwitch=0)
- `g2aiconfig 1`
- `g2aiconfig - 160` (streamSpeed only)
- `g2aiconfig 1 160` (the example from g2-kit-unofficial)

### `g2imgprobe [size_bytes]`

Send a Cmd=3 multi-fragment payload to exercise the image wire path.
Without a CREATE-image first the firmware should reject — we want to
confirm:
- Multi-fragment send completes without write-mutex timeouts.
- Firmware reassembler accepts the fragments (no `RX multi-fragment
  message — not yet handled` warnings).
- Firmware replies with `EvenCore ImageRawResp (cmd=4 magic=210)
  res=5 (ImgRawFailed)` or similar — that's the expected outcome
  proving the path works.

Test calls:
- `g2imgprobe 256` — single fragment, baseline.
- `g2imgprobe 1024` — small multi-fragment.
- `g2imgprobe 4096` — at the firmware's reported reassembly ceiling;
  watch for any warnings.

If the firmware responds with `res=5 ImgRawFailed`, the wire path is
verified and the next step is reverse-engineering
`ImageContainerProperty` (see `g2BuildCreateImage` TODO in
`System_G2_Protocol.h`).

If the firmware responds with `res=4 ImgRawSuccess`… we accidentally
hit a state where there IS a live image container. Note what was on
screen and document.

If no response at all, increase `size_bytes` and try again — firmware
might silently drop bodies smaller than some minimum.

## Heartbeat tail decoding

**Status:** decoded behind `DEBUG_G2_DUMP` flag.

**Test steps:**
1. `debugg2dump on` (or set the flag in Settings → Debug).
2. Watch the heartbeat-ack log lines. Should now include
   `HeartbeatAck tail seq=NN echo=12` alongside the ack itself.
3. Confirm `seq` increments monotonically and `echo` stays at 12.
4. If `echo` ever changes, capture the surrounding log — that means
   the tail field is more interesting than we thought.
