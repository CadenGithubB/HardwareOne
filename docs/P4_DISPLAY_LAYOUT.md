# P4 monochrome display layout

The P4-EYE keeps the OLED UI's **128-pixel logical width**, font, icons and
text wrapping. Its logical height is now **128 pixels**, presented over the
full **240×240** LCD. The SSD1306 stays **128×64**.

## Geometry and ownership

- `HAL_Display_P4EyePixels.h` owns the P4 canvas dimensions and panel mapping.
  The driver constructor, allocation and HAL dimensions use those values.
  Its page-packed canvas is 2,048 bytes, up from 1,024. DMA stripe size and
  panel transfer dimensions are unchanged.
- `OLED_Layout.h` owns compact monochrome chrome: 10-pixel header, 10-pixel
  footer and a one-pixel content gap. Content capacity remains 43 pixels on
  OLED and becomes 107 on P4. Color display chrome keeps its existing rules.
- `oledVisibleRows(rowHeight, reservedHeight, baselineRows)` in `OLED_Utils.h`
  calculates list capacity. `reservedHeight` covers a screen's status/title
  lines and padding; `baselineRows` preserves its established 64-pixel layout.
  Taller canvases use complete rows from the remaining content height.

Typical capacities:

| View | Row height | OLED | P4 |
|---|---:|---:|---:|
| Launcher / sensors / file list | 10 | 4 | 10 |
| Performance CPU / stack, with summary | 9 | 4 | 10 |
| Transcription home, with status line | 8 | 3 | 12 |
| Transcription text / saved-file list | 8 | 4 | 13 |

Lists stop at their actual item count. Drawing and navigation use the same
capacity, including transcript scroll boundaries, map menus/overlays and
ESP-NOW remote files. Existing settings/CLI/list views that already calculate
capacity inherit the taller content area. Fixed information pages retain their
content and typography; this is not a redesign of every screen.

The file browser fetches its display viewport instead of relying on the file
service's four-entry page. Its bounded render snapshot can hold 16 entries;
the service's pagination and other consumers retain their existing behavior.
Maps allocate and pack their shade and double buffers using canvas dimensions,
including all sixteen P4 pages. Icons and diagrams retain their logical size.

## Rendering tradeoff

128→240 is **1.875×** in both axes. Nearest-neighbor rendering stays strictly
black/white, fills the panel and retains the aspect ratio. Some logical pixels
occupy one physical pixel and others two; exact uniform duplication would need
a 256-pixel panel. The former 128→192 rendering also used fractional duplication.
Actual LCD legibility still needs a device check.

## Verification — 2026-10-17

Work started from `c0384687` with unrelated uncommitted work already present.
No device was flashed. A layout illustration uses the production list drawing
functions and shipping bitmap font with a graphics test double; it is not a
hardware capture. Icons and device status are omitted from that illustration.

- OLED launcher and simple-list framebuffer bytes match the saved pre-change
  renderers. These comparisons do not assert a pixel comparison for every mode.
- ASan/UBSan host coverage exercises 64- and 128-pixel layouts, visible selection,
  page navigation, text bounds, new lower-half pixels, panel edges, inversion,
  DMA stripe equivalence, map page packing, transcription drawing/scrolling,
  lifecycle retry/power behavior, list navigation and setup cancellation.
- All ten selected CTest suites passed with ASan/UBSan enabled. The separate
  pre-change OLED framebuffer comparison also passed.
- All firmware profiles below built and linked successfully. P4 maps are covered
  by the 128-pixel host packer checks; maps are disabled in `p4-openclaw`.

| Saved profile | Image bytes | Free app bytes |
|---|---:|---:|
| `p4-openclaw` | 5,910,272 | 1,515,776 |
| `qtpy-oled` | 2,909,696 | 3,074,560 |
| `qtpy-maps-test` | 3,741,056 | 2,243,200 |

The chat's accompanying verification record retains the exact commands,
profile inputs, dependency locks, build summaries and checked recovery patch.

Run the focused render check:

```sh
python3 components/hardwareone/test/host/test_oled_layout.py --sanitize \
  --preview-dir /tmp/hw1-display-preview
```

`--baseline <component-directory>` additionally compares the two OLED renders
with saved pre-change sources. `oled_layout` and `oled_transcription` are also
registered in the host CTest suite.

Existing profile build directories are reused for verification and retained
with their firmware/debug information: `build-config-p4-openclaw/` (~1.1 GB),
`build-config-qtpy-oled/` (~1.1 GB), and `build-config-qtpy-maps-test/` (~598 MB).
No new large firmware build directory
or dependency snapshot was created. Task-created host builds, source copies,
raw preview images and temporary executables are removed after recording the
results; the preview, patch and compact evidence are retained as chat artifacts.

## Performance monitor follow-up

The CPU and stack monitor had a separate `PERF_VISIBLE_ROWS = 4` limit missed
in the initial conversion. Both pages now use `oledVisibleRows(9, 9, 4)`: 9px
task rows with a 9px summary, preserving four OLED rows and showing ten on P4.
Drawing, scroll limits, arrows and sample-shrink clamping share that capacity.
The layout regression executes both shipping render/input functions at both
heights with a full task list, scrolling to its end, and shrinking samples.
