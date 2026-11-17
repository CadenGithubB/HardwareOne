# OLED list text: bounds and selected-label scrolling

A list item's text never runs onto the next row, across a pane, over a fixed
value column or under the scrollbar. Every label field is confined to its own
rectangle, intersected with the content area; a label that fits is drawn as
it always was, a label that does not ends in an ASCII `...` ellipsis, and the
selected row's label reveals itself by scrolling left. The policy applies to
the shared list renderers and to the custom rows listed below, on the 128×64
OLED, the 128×88 Waveshare canvas and the P4X-EYE's 128×128 canvas alike.

## Presentation policy

| Rule | Behavior |
|---|---|
| Bounds | `oledTextFieldDraw(display, x, y, w, text, fg, bg, animate, owner)` draws one 8-pixel row of glyphs inside `[x, x+w)`, intersected with the content area. Glyphs are drawn through a stack-local clipping adapter around the vendored `Adafruit_GFX::drawChar`, so partially visible glyphs draw only their visible columns and `write()`'s wrapping never runs. |
| Fits | Same glyphs and pixels as `print()`; nothing animates. |
| Static overflow | The first `cells-3` glyphs and `...`; with three or fewer cells, that many dots; with no whole cell, nothing. |
| Selected overflow | Shows the beginning, waits 1000 ms, moves left at 20 px/s, pauses 1500 ms at the end, returns to the beginning and repeats. Position comes from elapsed time, so the display's frame throttle changes how often it is painted, not how fast it moves. |
| Who animates | Only fields drawn with `animate` (the selected row's label). `showSelection=false` lists, non-selectable rows, status rows, values, badges and fitting labels stay static. A two-line entry bounds each declared line separately; both may scroll, a third line never appears. |
| Fixed columns | Cursors, `*` markers, status dots, `[ON]`/`[OFF]`, counts, role badges, values and scrollbars keep fixed rectangles; the label field ends before them. |
| Source text | Stored names, filenames, usernames, settings and commands are never shortened for display. Line breaks and tabs become one space; escapes, controls and malformed UTF-8 are removed and valid UTF-8 folded by the shared display sanitizer. Masked secrets stay masked. Up to 127 glyphs of a label are measured and drawn. |
| Out of scope | CLI Output, the LLM reply reader, transcripts, message bodies, forms and graph axes keep their own multiline or reader behavior. |

Timing, ellipsis and identity rules live in `OLED_TextFieldCore.h` (dependency
free, host tested); drawing and the display hooks in `OLED_TextField.cpp`.

## Animation state and scheduling

The marquee table holds two slots (one item's two lines). A slot is keyed by
the owning list or screen (`owner`), a hash of the sanitized label and the
field width; the label text itself is never kept, so no secret lingers in
animation state. The renderer brackets each frame with
`oledTextFieldFrameBegin()`/`oledTextFieldFrameEnd()`; a field claims its slot
while drawing, and a slot not claimed in a frame is released, so a hidden or
deselected label stops requesting frames. Because the key is the label and
its width rather than a pointer, index or refresh counter, a list rebuilt
every frame with `oledScrollClearKeepSelection()` keeps its progress, and
changing values or status columns beside the label do not restart it; a
different selection, label text, width, owner or display mode starts a fresh
cycle, as does a field not drawn for 2.5 s (display asleep, another view).

The dirty poll asks `oledTextFieldNeedsFrame()`: true only when a slot drawn
in the last frame has reached its next pixel change (or the end of a pause),
so waiting through the initial or end pause paints nothing. `oledIsDirty()`
also passes `oledConfirmIsActive()` to `oledTextFieldSetPaused()`: a
confirmation freezes fields where they stand and they resume there. The
display-session boundary and a mode change call `oledTextFieldReset()`. There
is no task, delay, always-dirty loop or change to the update interval; the
existing 125 ms frame throttle simply skips intermediate positions.

Cost: 80 bytes of `.bss` for the table and 2,603 bytes of code
(OLED_TextField.cpp.obj in the P4 build), two 128-byte label buffers and a GFX
adapter on the display task's stack per field drawn, and no heap.

## Shared renderers

- `oledScrollRenderSimple`: the label field starts after the two-glyph cursor
  column (x = 12) and ends at the right edge, or two columns earlier when the
  scrollbar is drawn (a glyph cell ends in a blank column, so 19 glyphs still
  fit beside it).
- `oledScrollRender` split pane: the field starts at x = 2 and ends inside the
  highlight bar (`listWidth - 3` wide), so text never crosses the separator or
  the icon pane. The main menu's 68-pixel pane holds 10 glyphs, the sensor
  launcher's 78-pixel pane 12; longer launcher labels now end in `...` until
  selected.
- `oledScrollRender` full width: each of the two declared lines is a field
  from the selection bar to the scrollbar's columns; the former hard-coded
  `20 characters + ~` cut is gone.

## Caller inventory

| Screen | Rows | Status |
|---|---|---|
| Menu (main, sensors) | split-pane launcher labels | Covered centrally; right-pane badge is one glyph (static) |
| Network, LED, Transcription, R1 Health, Logging configure, LLM pickers, Power CPU/Sleep, Bluetooth G2/R1 menus, File Browser actions, ESP-NOW device list, Remote bond picker | shared lists | Covered centrally |
| Power main | status line + list | Migrated: the duplicated row/scrollbar renderer is replaced by `oledScrollRenderSimple` one row down |
| Bluetooth status, Phone | cursor + label + live `*` | Migrated: rows that can carry the live marker end their label field at x = 108 and draw the `*` at x = 114; other rows run to the arrow column; label-only rows static |
| Settings editor | categories, label/value rows, editor title, enum options, Quick Settings | Migrated: values right-aligned in a fixed column (at most 10 glyphs) ending before the arrow column; labels scroll under them; Quick Settings name field ends before the `[ON]`/`[OFF]` column |
| File Browser | main split pane, peer list, action-menu name header | Migrated: the full entry name in a 75-pixel pane field (was cut to 13 characters); peer names keep their `/`; the header ellipsizes |
| Automations | main split pane, wizard pick lists, confirm rows | Migrated: full name beside the status dot (was 12 characters); wizard rows end before the arrow column; the confirm summary shows the whole name with an ellipsis. The right pane's three-line trigger wrap is intentional and unchanged |
| ESP-NOW | main menu, rooms, room devices, pairing candidates, send/receive choice, message list (first line and sender), settings rows, device config rows, remote files, status-page device name | Migrated: running marker fixed at x = 114; room counts and setting values in fixed right columns; sent messages stay right-aligned while they fit; folder tag fixed before remote names. The device-config header (`Config: name`, drawn in the header band at y = 0) keeps its 13-character cut: the band is outside the content area |
| Remote (bond) | sensor table rows, peer-name line, bond menu | Migrated: a nine-glyph name column (the former `%-9s`) with field label and value at fixed x; the link state right-aligned beside a bounded peer name |
| Logging | `drawLoggingMenuItem` | Migrated: field ends before the arrow column; disabled rows never scroll |
| Speech | inverse control rows | Migrated (full-width field) |
| User Manager | roster, actions header, action rows, role rows | Migrated: the whole username scrolls; the role badge keeps a fixed right column; the current-role `*` keeps x = 114 |
| Setup Wizard | shared-model menu rows, sub-mode rows, setup-mode options | Migrated with static ellipsis only: these pages redraw on input outside the frame loop, so no frames would drive a scroll. Value column at most 12 glyphs, as before |
| First-Time Setup | Wi-Fi picker | Migrated with static ellipsis (same reason). Yes/No and prompt rows are fixed short literals |
| LLM | model/group/template/entity row buffers | Row buffers widened from 21 to 47 glyphs (PSRAM, about 2.1 KB); the model row leads with the active `*`, the unavailable `x` and the source tag so they stay visible, and the name is no longer cut to the screen width. Home-menu rows are built by the shared home-menu core at 19 columns and fit by construction |
| CLI Output | record previews and Reader | Excluded: owned by the CLI Output work, already bounded by its console core (`~` marks a cut preview) |
| Bluetooth status detail, Network/Web/Mesh status, ESP-NOW status | status text, not selectable rows | Excluded: fixed-width status copies already fit their rows |
| Sensor `*_oled.h` adapters | data pages | Excluded: no selectable rows |
| Setup info pages, keyboard, confirmations, toasts | own text handling | Excluded: not list rows |

Upstream truncation that remains (reported, not hidden): `LlmModelDesc::name`
is 32 bytes at its source; Wi-Fi scan labels carry the 32-byte SSID in full;
`AutomationSummary::name` is 32 bytes; the 128-glyph field buffer cuts longer
sanitized labels.

## Vertical layout findings (not changed here)

- `oledScrollRender` full-width mode draws its title and first item from
  y = 0, under the header band; the ESP-NOW device list uses it with a title.
  The field clip hides what the header covered anyway.
- The Speech menu's inverse bars start at y = 10, one pixel above the content
  area.
- The ESP-NOW device-config screen draws its own header at y = 0.

## Verification — 2026-11-03

Host (ASan/UBSan, all passing): `test_oled_text_field.py` at 64/88/128;
`test_oled_layout.py` with new overflow cases on all three shared row kinds
(its 64-pixel baseline images for fitting labels are unchanged);
`test_oled_health.py`, `test_sensorlog_ui.py`, `test_systemlog_config_ui.py`,
`test_oled_automation_wizard.py`, `test_oled_list_nav.py` and
`test_oled_bluetooth_link_watch.py`, which splice or compile the migrated
screens, now draw through the production policy (`oled_text_field_op_double.h`
for op-recording canvases); plus the remaining OLED, settings, file-action,
session and privacy suites of the host README's ctest group (35 suites).
Two of those harnesses had already stopped compiling against the working
tree's shared-feedback changes (`settingsEditorShowError`, `OledUiIcon`) and
received stand-ins here.

| Profile | Result | Image | Headroom |
|---|---|---|---|
| `p4-phone-pipe` (P4X-EYE) | builds and links, no warnings in the changed rows | 6,142,736 B (+2,496 B over the CLI-only tree) | 1,283,312 B |
| `xiao-default` (display disabled) | builds and links, unchanged | 4,390,480 B | 1,593,776 B |
| `waveshare-p4-bringup` (128×88, touch) | builds and links | 4,990,848 B | 3,397,760 B |

**Deployment (2026-11-02, P4X-EYE):** the 6,142,736-byte `p4-phone-pipe` image
was written to the board on `/dev/cu.usbmodem112101` (MAC `02:48:57:31:04:73`,
confirmed with `esptool.py read_mac` first) with the same app-only command the
CLI Output work used (`esptool.py --chip esp32p4 write_flash 0x10000 ...`, hash
verified), so settings and models were untouched. The following boot, watched
over serial without a reset: `P4-EYE ST7789 online (spi)` at 4.2 s, the
console ring allocated in DRAM, `[Boot] Setup complete` at 21.1 s, and no
`[ERROR]` or `[WARN]` line in 45 s. That establishes boot and display init
only.

**On-device qualification (user, 2026-11-03, P4X-EYE):** all checks passed on
the device: the System menu's `Notifications` row reads `Notificat...` until
selected, then holds, scrolls left, pauses and repeats without crossing the
pane divider; Settings rows keep their right-aligned value still while a long
label scrolls under it; Bluetooth rows keep the live `*` in its column with
nothing spilling onto the next row; long names in Files and Automations end in
`...` unselected and scroll inside the left pane when selected, the status dot
and icon staying put; labels in scrollbar lists stop before the bar; the touch
A/B/X/Y controls navigate these lists. The Waveshare canvas has not been
checked on hardware.
