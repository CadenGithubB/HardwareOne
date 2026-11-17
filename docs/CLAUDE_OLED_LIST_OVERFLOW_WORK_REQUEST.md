# Work request for Claude: project-wide OLED list text bounds and scrolling

## Request and coordination

Investigate the current working tree, then implement a consistent overflow
policy for list items across HardwareOne's OLED-style UI, including the P4.
The user's requirement is that a list item's text must never run onto the next
row. Long selected labels should reveal their text by scrolling left.

This is a separate shared-UI task from the CLI Output rewrite you are already
working on. Preserve that work. Shared files currently being edited include
`OLED_Utils.cpp`, `OLED_Display.h`, `System_Debug.cpp`, component/test CMake files
and test documentation. New console owner/core/test files are already present.
Re-read current files and integrate after a coherent CLI checkpoint; do not
replace them with earlier versions or reapply whole-file patches from the
previous investigation. The user will bring your implementation report back to
Codex for review.

Repository: `~/esp/hardwareone-idf`. Read `AGENTS.md` and
`CONTRIBUTING.md`. Investigation base was `9674eb49` plus the evolving working
tree. Preserve all existing and concurrent edits, including CLI, feedback,
Health/graphs, I2C and Waveshare speaker work. No firmware source or device was
changed during this investigation; this handoff is the implementation request.

## Investigation findings

### Shared paths

`OLED_Utils.cpp` owns two list renderers:

- `oledScrollRenderSimple`: compact eight-pixel rows. Prints a two-character
  selection prefix and then the entire label using `print`, without a text
  width limit. The display's automatic wrapping remains enabled.
- `oledScrollRender`: split-pane and intentional two-line entries. Split-pane
  labels are printed without bounds and can cross the pane separator. The
  full-width two-line path instead truncates to a hard-coded 20 characters and
  `~`, regardless of actual field width.

A source inventory found **28 direct shared-renderer calls across 12 files**:
Menu, Network, Bluetooth, Power, Logging, LED, LLM, Remote, File Browser,
R1 Health, Transcription and OLED ESP-NOW. This is a snapshot of the inspected
working tree, not a substitute for checking current callers.

A temporary ASan/UBSan probe compiled the shipping renderers with the existing
framebuffer/font test double at 64/88/128-pixel canvas heights:

- A 19-character compact label fit; adding one character wrote 12 lit pixels
  outside its row. The two-character prefix means a nominal 20-character label
  does not fit on the 128-pixel canvas.
- A 14-character split-pane label wrote 16 lit pixels outside a 78-pixel pane.
  Text can corrupt the icon/details pane even before reaching the screen edge.

`print()` versus `println()` does not prevent automatic wrapping. Explicit LF
also advances the cursor independently of the automatic-wrap setting.

### Graphics constraints

The bundled Adafruit_GFX has screen-edge clipping and `setTextWrap`, but no
public general-purpose clip rectangle for a list cell. Disabling wrapping alone
still permits text to cross a pane, selection marker, status column or scrollbar.
It also risks changing subsequent readers if global text state is not restored.

P4X-EYE and Waveshare render through an Adafruit_SSD1306-compatible monochrome
software canvas. Logical dimensions are P4X-EYE **128x128**, Waveshare **128x88**,
and ordinary SSD1306 **128x64**; their physical panel sizes are not text widths.
P4X-EYE currently declares `DISPLAY_IS_COLOR=0`, so derive chrome from the actual
HAL macros rather than assuming the color-panel header/footer sizes. Other
color display backends also exist and share GFX drawing conventions.

### Custom paths and early truncation

Fixing only the shared functions leaves custom rows untouched. Some screens
already shorten the source label before drawing. A marquee cannot reveal text
that has already been cut. Restore full *display* text through safe owner APIs
or bounded owned buffers, without changing stored names or action identity.

| Source | Custom paths / integration work to inspect |
|---|---|
| `OLED_Mode_Menu.cpp` | Shared split-pane main/sensor launchers; labels and right-pane decoration. |
| `OLED_Mode_Power.cpp` | `renderPowerMainList` manually duplicates compact-row/scrollbar drawing; CPU/Sleep use shared lists. |
| `OLED_Mode_Bluetooth.cpp` | `displayBluetoothStatus` and `displayPhone` manually print rows and append live `*` markers; G2/R1 submenus use shared lists. |
| `OLED_SettingsEditor.cpp` | Category rows, setting label/value rows, enum choices and Quick Settings; existing 15-character label truncation, numeric/string values and fixed ON/OFF columns. Remote Settings delegates here. |
| `OLED_Mode_FileBrowser.cpp` | Main filename/icon split pane truncates filenames to 13 characters; action submenu is shared. Preserve filesystem identity and file operations. |
| `OLED_Mode_Automations.cpp` | Main names/status-dot split pane clips names to 12 characters; wizard choice rows are custom. Source `AutomationSummary::name` is already bounded to 32 bytes. |
| `OLED_ESPNow.cpp` | Main menu, rooms/room-device rows, pairing candidates, mode/config/settings rows and remote file list; some lists use shared rendering. Audit shortened display buffers and fixed suffixes. Message-body reading is a separate case. |
| `OLED_Mode_Remote.cpp` | Bond sensor/stream rows and formatted label/value strings; bond picker uses shared rendering. |
| `OLED_Mode_Logging.cpp` | Custom `drawLoggingMenuItem`, as well as shared system-log/configuration rows. |
| `OLED_Mode_Speech.cpp` | Inverse-highlight control rows use scroll state but still print labels manually. |
| `OLED_Mode_UserManager.cpp` | User, action and role lists; usernames are shortened for display. Keep authorization, founder indicators and roster identity. |
| `OLED_SetupWizard.cpp` | Formatted shared-model menu labels/values and selectable startup/feature choices. Keep draft ownership and secrets masking. |
| `OLED_FirstTimeSetup.cpp` | Basic/Advanced/Import and other selectable setup choices. Preserve setup behavior. |
| `OLED_Mode_LLM.cpp` | Shared pickers have upstream row buffers capped at `LLM_CHARS` (~21 characters), including model/group/template/entity labels. Use full available display text safely; do not enlarge every row indiscriminately. |
| `OLED_Mode_LED.cpp`, `OLED_Mode_Network.cpp`, `OLED_Mode_R1_Health.cpp`, `OLED_Mode_Transcription.cpp` | Shared lists, including frequently rebuilt labels, reserved status rows, saved/scan Wi-Fi names, graph picker, and state-dependent actions. |

Also search sensor-specific `*oled.h` adapters and remaining selectable loops.
Return a completed coverage inventory: covered centrally, migrated custom row,
static safe choice, or excluded with a concrete reason. Do not claim global
coverage from changing just the two shared functions.

## Required presentation policy

1. **Hard bounds first.** Every label field is restricted to its own rectangle,
   intersected with the actual content area. No text or background writes may
   escape into another row, a pane, a fixed value, the header/footer or scrollbar.
2. Fitting labels render normally and do not animate.
3. Overflowing unselected labels use a bounded ASCII `...` ellipsis. Narrow
   fields must degrade safely, even when fewer than three dots fit.
4. Overflowing selected labels initially show their beginning, wait **1000 ms**,
   then move left at **20 logical pixels/second**. Pause **1500 ms** at the end,
   return to the beginning and repeat. These are central defaults, not settings
   to duplicate throughout screens. No bouncing or simultaneous moving list.
5. Only selected item's eligible label fields animate. `showSelection=false`,
   nonselectable/status rows and fitting fields remain static. For intentional
   two-line entries, bound each declared line separately: overflowing selected
   fields may use the same timing policy, but never create a third line.
6. Keep cursors, checkboxes, status dots, badges, values and scrollbars stationary.
   Label/value rows reserve separate fixed rectangles; scrolling the label must
   not move or cover ON/OFF, numeric values, connection markers or current values.
7. Preserve original source text and underlying IDs. In list labels, line breaks
   and tabs become safe single-line spacing; malformed/unsupported text must not
   break measurement or drawing. Preserve secret masking/redaction. Display-only
   shortening is not allowed to mutate a setting, username, filename or command.
8. Do not marquee full CLI/LLM replies, transcripts, message bodies, forms or
   graph axes. Those keep their existing multiline/reader behavior. CLI previews
   may adopt the bounded primitive if it fits the ongoing CLI design; its Live /
   Paused / Reader model remains owned by that work.

## Suggested implementation shape

### Milestone 1: bounded text primitive

Put reusable row/text-field rendering with the existing UI owner (`OLED_UI` or
an appropriately small sibling). `OLED_Utils` list functions and custom row
renderers call it. Keep calculation/state logic host-testable.

A small stack-local GFX clipping adapter that forwards `drawPixel` only inside
a rectangle is one feasible route: use direct glyph drawing, bypassing `write()`
wrapping, without modifying the vendor library or global display state. Use
another approach if simpler, provided it guarantees pixel bounds on every
supported backend, including partially visible glyphs during scrolling.

Use the existing list font contract (currently the classic six-by-eight cell at
size one), or explicitly carry actual font metrics when needed. Measurement,
sanitization, ellipsis and rendering must agree. Do not change the project's
font system to implement a marquee. Avoid per-row heap allocations, full-screen
scratch buffers, driver-specific forks and full-string drawing at offscreen
coordinates without local clipping. Draw only the visible glyph portion and
clear only the owned text field as needed to prevent trails.

### Milestone 2: stable animation and scheduling

Maintain bounded UI-owned state for the selected item/fields. Identify selection
by stable caller data, not only pointer address, selected index, or list refresh
counter. Screens reuse buffers and rebuild lists every frame;
`oledScrollClearKeepSelection()` increments `refreshCounter`, and item
`validationKey` follows that counter. Neither is a semantic identity for marquee
reset purposes. Do not let a routine list rebuild restart the one-second delay.

Reset on actual selection/label/width/layout changes and explicit view reset.
Preserve progress when the same item moves index after sorting if its stable ID
is still known. Keep changing numeric/status columns outside the scrolled label
so telemetry does not continually restart the animation.

Use elapsed-time motion, not pixels-per-render. Handle `millis()` rollover.
Pause/stop animation when hidden, in another view, under a modal, or asleep;
clear stale deadlines on mode/session changes and avoid retaining secret text
in global animation state. Keep raw text ownership with the existing caller or
an owned bounded snapshot, not a borrowed temporary `String::c_str()`.

Integrate a due-time check with the OLED dirty-render policy. Mark/repaint only
when a visible animated label's pixels need to change or a reset needs drawing.
Waiting for the initial/end pause must not force continuous redraw. No blocking
`sleep`/`delay`, new task, global always-dirty loop or forced change to the user's
display update interval. The existing frame throttle may skip intermediate pixel
steps; elapsed time should preserve the intended speed.

Do not copy old `oledIsDirty()` code over the ongoing CLI generation/invalidation
changes. Integrate the scheduling hook narrowly into the current implementation.

### Milestone 3: shared paths and custom migration

Apply the primitive to compact, split-pane and intentional two-line shared lists.
Calculate widths from geometry and reserve the marker/scrollbar gutters. The
compact prefix consumes 12 pixels; icon panes and fixed suffixes reduce the text
viewport further. Do not use another universal 20-character constant.

Then migrate the custom rows in the inventory, retaining existing screen look,
actions, navigation, list capacities, ownership and permissions. Remove replaced
local truncation/rendering helpers once callers use the shared policy. When a
source snapshot itself is bounded, expose all available text and mark any real
source truncation; do not promise to recover missing bytes.

Avoid increasing all list backing buffers to a large fixed length. Prefer
existing full-name sources or a bounded selected-label snapshot where appropriate.
Measure allocation/state costs and preserve private-memory requirements.

This work fixes horizontal list overflow. Report separate existing vertical
capacity/layout problems discovered during the audit rather than silently
changing unrelated navigation or claiming they were all fixed.

### Milestone 4: tests, builds and return report

Tests must exercise production code and assert actual pixel writes, not only
cursor positions. At minimum cover:

- Empty, fitting, exact-fit and one-glyph-too-long text; tiny/zero viewports.
- Compact cursors, narrow split panes, scrollbars, deliberate two-line entries,
  inverted highlights, fixed ON/OFF/numeric columns and status icons.
- No writes outside a field at initial position, fractional glyph offsets,
  middle, final position and reset; no residual pixels/trails.
- Unselected/static rows never animate; selection/label/width changes reset;
  frequent keep-selection rebuilds and unrelated value updates do not restart.
- Initial pause, elapsed-time speed, end pause, repeat, clock rollover, view
  changes, modal pause, wake/session reset and no idle repaint loop.
- Embedded newline/tab/control input, malformed/unsupported UTF-8 and long
  filenames/user names/template labels without overruns or unsafe source use.
- 128x64, 128x88 and 128x128 logical canvases using actual chrome definitions;
  wider backend geometry where the existing supported build permits it.
- Representative custom screens, including Settings value columns, File Browser
  icons, Bluetooth badges, Automations dots and LLM full selected-label access.

Extend the current OLED layout tests and add focused clipping/animation tests
with ASan/UBSan. Reinspect their harness after API changes: existing tests splice
shipping functions and currently do not model every GFX state/API. Keep fits-
without-overflow output unchanged where practical; record intentional ellipsis
changes. Inspect rendered previews for readability as well as bounds.

Run current CLI buffer/viewer, list navigation/layout, Health, menu availability,
settings/remote settings, file-action and session/privacy regressions as affected.
Build the existing `p4-phone-pipe` profile and display-disabled `xiao-default`
baseline through `tools/build_config.py`; consider `waveshare-p4-bringup` for the
88-pixel/touch combination. Reuse compatible build directories and serialize
shared dependency reconfiguration. No bulk builds/worktrees/dependency copies.

Update the shared UI contract and verification record. Remove task-created
preview files, temporary executables and logs after preserving useful evidence.
Report retained large output directories and their purpose. Device deployment
is a separate step when requested; re-verify the connected device/partition
layout before app-only flashing. Do not assume an old USB port identifies the
current board or write settings/models merely to demonstrate scrolling.

## Response to return to Codex

Provide a self-contained report the user can paste back, including:

1. Implemented behavior and any deliberate deviation from the requested policy.
2. API/state/scheduler design, lifecycle handling, and how CLI changes were
   preserved/integrated. Name every shared file changed by both workstreams.
3. Completed caller inventory, including remaining bypasses or upstream truncation.
4. Tests and profile builds actually run, results, memory/image impact and
   physical qualification limits. Include commands/reproduction steps for gaps.
5. Relevant files and a reviewable patch/commit reference if available. Do not
   commit or attribute unrelated work without authorization.
6. Cleanup performed and remaining deployment work.

Codex will review the resulting diff and verification evidence. Do not describe
this as complete project-wide coverage while known selectable rows still bypass
the bounding policy without an explained exception.
