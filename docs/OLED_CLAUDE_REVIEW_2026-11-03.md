# Codex review of Claude's OLED changes — 2026-11-03

Reviewed the current working tree against the CLI and project-wide list-text
handoffs. No firmware source was changed or device flashed during this review.
The main clipping, ellipsis, marquee, console-owner and Reader mechanisms are
present. The focused suites below pass. Three uncovered defects need a bounded
correction pass before this is considered complete.

## Findings

### 1. [P2] Console fence rejects valid output after a long session

Location: `OLED_ConsoleBuffer.cpp`, `OledConsoleBuffer::append`, currently line 99.

The permanent fence tests `(int32_t)(timestampMs - fenceMs_) <= 0`. That modular
ordering is only valid within a half-range. Once a session lasts more than
`0x80000000` milliseconds (about 24.9 days), current output is interpreted as
older than the boundary and refused. The rejection continues until timestamps
enter the accepted half-range again or a new clear/session boundary occurs.

Reproduced with the production owner methods and existing clock/mutex doubles:

1. Set `millis()` to 100, initialize the buffer and call `clear()`.
2. Advance to `100u + 0x80000001u` without another identity change.
3. Append a valid current record stamped with that current time.
4. Actual: status count 0, fenced count 1. Expected: the new record is retained.

Correct the delivery fence so it remains valid across long uptime and rollover.
Prefer an owner/session generation captured when the OLED-routed envelope is
queued, or an equivalent mechanism with a demonstrated lifetime invariant.
Preserve rejection of old queued output across identity replacement; simply
removing the fence is not a fix. Add regressions for long idle/uptime as well
as the already-tested short interval crossing `UINT32_MAX`.

### 2. [P2] Different items with identical labels inherit marquee progress

Locations: `OLED_TextField.cpp`, construction of `MarqueeKey` (currently line 89),
and `OLED_TextFieldCore.h`, `MarqueeKey`/`Marquee::offset`.

The key contains only list/screen owner, sanitized text hash and width. Two
items in the same list can have the same label and width; changing selection
between them reuses the first item's clock instead of restarting the required
one-second pause. There is no item identity in the public draw call.

Reproduced with the production text-field implementation/pixel recorder:

1. Draw an overflowing label at t=10000 for one list owner.
2. Draw it again at t=11300, after it has begun moving.
3. At t=11350 select the next item, whose displayed label/width are identical;
   draw it at the new row with the same list owner.
4. Actual: pixels remain shifted. Expected: the newly selected item's text
   starts at offset zero and gets its own initial pause.

Carry a stable logical item token (and field identity where needed) through
shared/custom row APIs. Preserve continuity for the same item during routine
rebuilds or sorting; do not replace this with the per-frame refresh counter or
only pointer/index identity. Test duplicate labels and moving the same stable
item to a different index. Existing selection-reset tests change the text, so
they do not cover this case.

### 3. [P2] Nonselectable shared-list rows can animate

Location: `OLED_Utils.cpp`, `oledScrollRenderSimple`, currently lines 1464–1470;
check split-pane/two-line rendering for the same eligibility mistake.

The renderer passes `animate = showSelection && i == selectedIndex` without
checking `items[i].isSelectable`. A long nonselectable status/placeholder at
selected index zero starts a marquee and requests repeat frames. This conflicts
with the documented static behavior for nonselectable/status rows.

Reproduced with the production shared renderer and text-field implementation:

1. Create a list with one long item added with `selectable=false`.
2. Render with `showSelection=true` at t=10000, bracketed as a normal frame.
3. Advance to t=11050 and call `oledTextFieldNeedsFrame()`.
4. Actual: true. Expected: false, with a static ellipsized label.

Gate animation on actual eligibility in all shared paths and relevant custom
callers. Add an integrated renderer test rather than testing only a direct
primitive call with `animate=false`.

## Independently rerun verification

All eight suites passed on this working tree:

- `test_oled_text_field.py` (ASan/UBSan, 64/88/128 geometry).
- `test_oled_layout.py --sanitize`.
- `test_oled_cli_output.py` (ASan/UBSan is automatic; no `--sanitize` flag).
- `test_oled_console_buffer.py --sanitize` (ASan/UBSan is automatic).
- `test_oled_session_role.py --sanitize`.
- `test_oled_remote_settings.py --sanitize`.
- `test_oled_list_nav.py --sanitize`.
- `test_oled_health.py` (ASan/UBSan is automatic).

Separate temporary production-code probes reproduced all three findings with
sanitizers enabled. Their build directories and diagnostic script were removed
after recording the results. No build directory or source snapshot was created.

The existing P4 and Waveshare build manifests match the image sizes recorded
by Claude's list-text notes. This review did not independently rebuild or
repeat physical qualification; rerun affected builds after the corrections.

## Documentation reconciliation

`OLED_CLI_OUTPUT.md` still ends its earlier deployment account by saying the
list-text update has not been flashed and the device uses old menu labels.
`OLED_LIST_TEXT.md` records a later list-text deployment and user qualification.
Mark the former as a historical intermediate image and link the newer record
so the current deployed state is unambiguous. Do not invent new qualification
results or present this host review as an on-device test.

## Correction-pass handoff

Claude: fix these three defects with focused production-code regressions,
preserving the existing CLI and list migrations and all unrelated working-tree
changes. Rerun the suites above plus any directly affected callers, then rebuild
the existing `p4-phone-pipe`, display-disabled baseline and relevant Waveshare
profile if shared API changes affect it. Report the precise behavior, test and
build results, resource impact, remaining limitations and deployment state.
Return that report to the user for Codex review. Keep this pass bounded; it is
not a request to redesign logging or redo the completed UI migration.
