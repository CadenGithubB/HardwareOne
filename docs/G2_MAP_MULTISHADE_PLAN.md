# G2 Map Multi-Shade Rendering — Investigation & Plan

**Status: plan-only (no code changes yet). Investigated 2026-07-06.**

Goal: the G2 glasses map should render features in multiple shades of green
(brightness = prominence), while the OLED keeps rendering exactly as today
(1-bit, byte-identical output).

---

## 1. How the pipeline works today (verified)

### Map data already carries the richness
- `.hwmap` v6: every feature has a **type byte + subtype byte**
  (`HWMapFeatureHeader`, System_Maps.h:237-242). 12 types, 22 subtypes.
- This is the same two bytes the web page uses to pick its multi-color styling
  (`SUBTYPE_COLORS[type][subtype]` fallback `COLORS[type]`, WebPage_Maps.h:446-502).

### One shared render core, three independent pixel sinks
- **Shared core**: `MapCore::renderMap()` (System_Maps.cpp:1078) walks tiles →
  filters (layer mask, subtype mask, zoom LOD, `shouldRenderFeature`) → fetches
  `MapFeatureStyle` via `renderer->getFeatureStyle(type)` (System_Maps.cpp:1284) →
  emits **polylines only** via `renderer->drawLine(...)`. No polygon fill, no labels.
- **OLED**: `mapRenderTask` renders via `OffscreenMapRenderer` into a 1-bit
  1024-byte PSRAM double buffer (128×43 content at y=11, OLED_Mode_Map.cpp:948),
  front buffer memcpy'd into the SSD1306 framebuffer each UI frame
  (OLED_Mode_Map.cpp:1141 — the **only** project-wide caller of `getBuffer()`).
- **G2**: does NOT screenshot the OLED. `g2RenderCurrentMapBmp`
  (G2_Glasses.cpp:14129) re-renders the same shared view globals into its own
  transient 1-bit 128×64 page buffer, then `buildMapBmp4bpp288x144FromPage`
  (G2_Glasses.cpp:14056) nearest-neighbour upscales 2.25× to a **288×144 4-bpp BMP
  with a 16-entry grayscale palette** — but feeds only nibbles 0x0 / 0xF
  (G2_Glasses.cpp:14113). Pushed via the existing chunked
  `sendImageBmpFragmentsNoCreate` transport.
- **Web**: fully parallel client-side JS renderer (raw `.hwmap` download +
  canvas). Shares only constants (hand-mirrored FT_*/ST_*, LOD injected at
  serve time). **Cannot be broken by any on-device renderer change.**

### The binary look is caused by the renderer, not the wire
- The single pixel primitive is a 1-bit OR-set:
  `_buffer[x + (y/8)*128] |= 1 << (y&7)` (System_Maps.cpp:1385-1389, inlined in
  the bresenham/dash/dot loops). Shade information dies here.
- The G2 wire format **already carries 16 gray levels today**. The camera
  viewer and JPG viewer push the identical 4-bpp BMP with real nibbles 0..15
  and it's HW-proven: the lens honours palette bytes (g2bmp
  brightness/contrast tuning works, G2_PROTOCOL.md "confirmed working
  2026-04-27"), community docs describe the panel as "4-bit greyscale — 16
  shades of green".
- **Nothing in the BLE transport, container CREATE, chunking, or BMP header
  needs to change.** The CREATE has no bpp/format field; encoding is implicit.

### Dormant seams that make this easy
- `MapFeatureStyle` already has a `uint16_t color` field documented "for color
  displays" — **never read anywhere** (System_Maps.h:179-186). `lineWeight` and
  `priority` are also never consumed by any drawLine.
- `getFeatureStyle` is per-renderer virtual; exactly 2 overrides exist
  (Offscreen + OLED), one C++ call site (System_Maps.cpp:1284).
- Waypoints + position marker draw through the same `MapRenderer` interface
  inside renderMap (System_Maps.cpp:1354-1357) → format-agnostic.
  Note: the G2 map page today shows waypoints + marker but **not** the GPS
  track (`renderTrack` is called only from the OLED task, OLED_Mode_Map.cpp:955).

---

## 2. Design

### Core change: shade buffer + max-write
Widen `OffscreenMapRenderer`'s output from 1-bit page-packed to **1 byte per
pixel (shade 0..15)**, and parameterize the hardcoded 128/64 dims in
drawPixel/bresenham/dash/dot while rewriting those loops.

- **Pixel write becomes `buf[i] = max(buf[i], shade)`** — NOT last-write-wins,
  NOT priority sorting. Rationale (verified): renderMap streams features in
  tile/file order with halo-duplicated geometry across tiles, so sorting can't
  fix cross-tile overpaint without a display list the streaming/LRU design
  exists to avoid. OR *is* max on 1 bit, so max-write is the exact
  generalization of today's semantics. Constraint: shade must be monotone with
  z-order (brighter = on top). Acceptable because the device draws strokes
  only; the web's one dark-over-bright case (highway casing) is a fill-beats-
  casing anyway.
- Shade comes from the style table: add `uint8_t shade` to `MapFeatureStyle`
  (or repurpose the dead `color`/`priority` fields — no-backwards-compat rule
  applies). **Type-level only** — do not thread subtype into getFeatureStyle:
  16 palette slots can't hold 26 type+subtype classes, the web's subtype
  distinctions are hue/width (collapse on a luminance ramp), and the one
  render-relevant subtype case (river vs lake) already lives in
  `shouldRenderFeature`. Blast radius if ever needed later: 7 enumerable sites.
- Overlays (position marker, waypoints, highlight, future track) pinned at
  shade 15 so they always win under max-write.

### OLED: crush at the swap, display path untouched
`mapRenderTask` renders into an 8 KB shade scratch buffer, then packs
`shade > 0 → bit set` into the existing 1-bit back buffer before the swap.
The per-frame memcpy blit, double-buffer spinlock, and every downstream OLED
byte stay **identical**. Because the crush is `>0`, shade-table tuning can
never change OLED output — parity is structural, not coincidental.

### G2: native-resolution render + direct nibble pack
Replace the 1-bit page → upscale path with a **native 288×144 render**
(41.5 KB transient PSRAM shade buffer, ps_alloc'd like today's page buffer);
`buildMapBmp...` packs `nibble = shade` directly. Two wins: real shades AND
the end of the 2.25× non-integer upscale that currently makes 1-px lines
alternately 2 and 3 px wide on the lens. (Fallback if perf disappoints:
keep 128×64 shade render + upscale — shades still work.)
Perf note: ~4.5× line-pixel work per render, but G2 renders only on tap
(dirty flag, 120 ms poll loop) — not continuous.

### Draft shade table (pending ramp test)
Start with 4–5 well-separated bands; adjacent mid-nibbles are empirically hard
to tell apart on the lens (the AutoLevels tone-mapping exists precisely
because mid bins 4..10 looked similar):

| Shade | Features |
|-------|----------|
| 15 | overlays (marker/waypoints/highlight), highways |
| 11–12 | major roads, stations |
| 7–8 | minor roads, railway (dashed), water/rivers |
| 4–5 | paths (dotted), bus/ferry (dashed) |
| 3 | buildings (dotted) |

Optional later polish: a G2-specific `shouldRenderFeature` could un-filter
parks/lakes as dim outlines (they're skipped today because 1-bit wireframe
made them noise); dash/dot patterns could then be reconsidered since shade now
carries the class information.

### Step 0 (do first, on HW): palette ramp test
No ramp probe exists today (`buildBmp4bpp` only has STRIPES/ALL_BLACK
patterns). Push a 288×144 4-bpp BMP with 16 vertical bands via the existing
`g2bmp` command (pre-made file or a trivial new pattern) and count
distinguishable shades at normal lens brightness. This decides 4 vs 8 bands
and whether the palette should be perceptually spaced (e.g. 0/96/176/255)
instead of linear. Everything else in this plan is insensitive to the answer —
only the shade-table values change.

---

## 3. Pre-existing hazard found during investigation (separate fix)

**The shared tile cache is not safe under two concurrent `renderMap` callers,
and concurrency is real today**: a G2 map-page zoom tap mutates the shared
view globals, which trips the OLED map mode's dirty check, so `mapRenderTask`
and `g2_map_page` render in true parallel against the same `LoadedMap` LRU
statics. `loadTileData` mutates LRU metadata outside any lock (FsLockGuard
covers only seek+read); `unloadMap` frees the pool under only the FS lock.
Failure modes: use-after-evict, duplicate-eviction, use-after-free. The code
itself admits a "dedicated map mutex" is needed (System_Maps.cpp:730-731).
A milder instance predates G2 entirely (`LocationContextManager::updateContext`
calls `loadTileData` from the display task concurrent with mapRenderTask).
Recommended companion fix, independent of multi-shade.

## 4. Explicit non-goals
- No BLE/transport/protocol changes (wire already 16-shade capable).
- No web page changes (fully parallel renderer).
- No map file format changes.
- No subtype→style threading, no polygon fill, no priority sorting.
- No OLED visual change of any kind.
