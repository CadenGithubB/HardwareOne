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

---

## Verified (kept here briefly for traceability)

- **G2 hijack TEXT view — JSON pagination** — verified 2026-04-28.
  Pages render with "[N/M] tap=next, 2x-tap=exit" header, single-tap
  advances, double-tap exits, wrap-on-final-page works.
- **`g2imgprobe`** — verified 2026-04-28. Cmd=3 multi-fragment send
  completes; firmware replies with `ImageRawResp` `errorCode=5
  (ImgRawFailed)` because no CREATE-image precedes — exactly the
  predicted outcome. Wire path is sound. Next step (when needed):
  reverse `ImageContainerProperty` to enable real image rendering.
- **`g2aiconfig`** — verified 2026-04-28. Empty CONFIG (no body)
  acked with `cmd=10 magic=212` and no errorCode set, meaning the
  firmware accepts an empty config. Next step (when needed): iterate
  field numbers/values to learn the schema.
- **Heartbeat tail decoding** — verified 2026-04-28. With
  `debugg2dump on`, every `HeartbeatAck` line carries `tail seq=NN
  echo=12`; `seq` increments monotonically and `echo` stays 12 across
  every capture. The tail field is therefore an uninteresting
  cmd-id echo, not hidden state.
