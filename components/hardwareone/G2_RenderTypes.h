#ifndef G2_RENDER_TYPES_H
#define G2_RENDER_TYPES_H

#include <stdint.h>

// Shared presentation data for page callers and wire builders. These types
// remain available when G2 support is disabled; they do not enable a backend
// or depend on Arduino, BLE or the wire-protocol implementation.

// ── Container geometry presets ───────────────────────────────────────────────
// The list/text container's on-lens rectangle is a free parameter in the
// pb schema (X/Y/W/H, lens is 576×288). The reference's
// `buildCreateStartUpPageContainer` hardcodes 280×130 at origin (0,0), which
// is roughly half-lens and only fits ~3 rows of a list with the firmware's
// default font. We expose a small named-preset table so per-page callers
// can pick a size by intent ("LARGE" for menus, "SMALL" for compact info
// chips) without hand-tuning four magic numbers each time.
//
// Geometry is applied at CREATE time. Changing the geom requires
// SHUTDOWN+CREATE — REBUILD is in-place and reuses the existing rectangle.
// (g2ShowListPage already does SHUTDOWN+CREATE for every swap, so changing
// geom between pages is free.)
struct G2ContainerGeom {
  uint32_t x;
  uint32_t y;
  uint32_t w;
  uint32_t h;
};

// Full lens, no margin. Rarely needed — use LARGE in practice; this exists
// for future image work or any place where edge-to-edge is desired.
static constexpr G2ContainerGeom G2_GEOM_FULL   = {   0,   0, 576, 288 };

// Default for hijack menus and any "give me as many rows as possible" case.
// 8 px margin on every side keeps content clear of the curved lens edges.
static constexpr G2ContainerGeom G2_GEOM_LARGE  = {   8,   8, 560, 272 };

// Centered rectangle — about 80 % of the lens. Comfortable for medium-
// length lists where you want some surrounding dead space.
static constexpr G2ContainerGeom G2_GEOM_MEDIUM = {  48,  24, 480, 240 };

// Reference's original 280×130, re-centered. Matches the look of the
// firmware's own one-shot prompts. ~3 rows visible.
static constexpr G2ContainerGeom G2_GEOM_SMALL  = { 148,  79, 280, 130 };

// Half-height presets for split layouts (e.g. status header + scroll body).
// Currently unused by any page; provided for future composition.
static constexpr G2ContainerGeom G2_GEOM_TOP_HALF    = {   8,   8, 560, 130 };
static constexpr G2ContainerGeom G2_GEOM_BOTTOM_HALF = {   8, 150, 560, 130 };

// Half-width presets — useful when pairing two surfaces side-by-side
// (e.g. text on the left, icon/image on the right). 8 px margin on the
// outer edges; inner edge butts the lens midline so two LEFT/RIGHT
// containers tile cleanly.
static constexpr G2ContainerGeom G2_GEOM_LEFT_HALF   = {   8,   8, 280, 272 };
static constexpr G2ContainerGeom G2_GEOM_RIGHT_HALF  = { 288,   8, 280, 272 };

// Production list+text(+image) split — Files, Health, Maps, keyboard pad.
// 264px nav list, 8px gutter, 288px right column ending at x=568 (same
// right edge as G2_GEOM_LARGE). Distinct from LEFT_HALF/RIGHT_HALF, which
// tile the midline with no gutter and a narrower 280px right pane.
static constexpr G2ContainerGeom G2_GEOM_SPLIT_LIST      = {   8,   8, 264, 272 };
static constexpr G2ContainerGeom G2_GEOM_SPLIT_RIGHT     = { 280,   8, 288, 272 };
static constexpr G2ContainerGeom G2_GEOM_SPLIT_RIGHT_TOP = { 280,   8, 288, 112 };

// Quadrant presets — quarter-lens rectangles. Together they tile the
// full lens with consistent 8 px edge margins and 0 px interior gutter.
// Useful for 2x2 layouts and combo (text + image) tests.
static constexpr G2ContainerGeom G2_GEOM_QUAD_TL = {   8,   8, 280, 136 };
static constexpr G2ContainerGeom G2_GEOM_QUAD_TR = { 288,   8, 280, 136 };
static constexpr G2ContainerGeom G2_GEOM_QUAD_BL = {   8, 144, 280, 136 };
static constexpr G2ContainerGeom G2_GEOM_QUAD_BR = { 288, 144, 280, 136 };

// Thin horizontal strips — status-bar (top) and footer (bottom). About
// one row tall on the firmware's default font. Pair with a TOP_HALF /
// BOTTOM_HALF body container for a header-plus-content layout.
static constexpr G2ContainerGeom G2_GEOM_STATUS_BAR = {   8,   8, 560,  40 };
static constexpr G2ContainerGeom G2_GEOM_FOOTER     = {   8, 240, 560,  40 };

// Edge-anchored geoms. Verified 2026-04-30 on firmware 2.2.0.24 via
// the Tests / Display / Selection Patterns / Edges + canaries bench
// (tests N / O / L). Because TextObject content anchors top-left of
// its bounding box, "bottom strip" geoms must hug y≈248 to actually
// paint at the bottom of vision — a geom anchored at y=144 paints
// in the vertical middle. STATUS_BAR above is the matching top
// preset (verified by test P).
//   BOTTOM_BAR    — 32 px strip across the full width
//   BOTTOM_BAR_L  — left half of BOTTOM_BAR (paired Yes button)
//   BOTTOM_BAR_R  — right half of BOTTOM_BAR (paired No button)
//   RIGHT_COL     — narrow column hugging the right edge, full height
static constexpr G2ContainerGeom G2_GEOM_BOTTOM_BAR    = {   8, 248, 560,  32 };
static constexpr G2ContainerGeom G2_GEOM_BOTTOM_BAR_L  = {   8, 248, 280,  32 };
static constexpr G2ContainerGeom G2_GEOM_BOTTOM_BAR_R  = { 288, 248, 280,  32 };
static constexpr G2ContainerGeom G2_GEOM_RIGHT_COL     = { 438,   8, 130, 272 };

// Stress shapes for geometry-edge testing — extreme aspect ratios that
// production pages don't use, but that exercise the firmware's container
// layout against unusual rectangles.
static constexpr G2ContainerGeom G2_GEOM_TALL_NARROW = {  16,   8,  96, 272 };
static constexpr G2ContainerGeom G2_GEOM_CENTER_DOT  = { 224, 104, 128,  80 };

// One text child in a compound layout. containerId must be distinct per
// child so the renderer can identify its TextEvent reports.
struct G2TextChildSpec {
  const char*       containerName;  // e.g. "btnYes" — short ASCII
  const char*       content;        // displayed text, may include \n
  uint32_t          containerId;    // 1..N, must be distinct per child
  G2ContainerGeom   geom;           // on-lens rectangle for THIS child
  bool              eventCapture;   // set TRUE to ask firmware to fire tap
                                    // events for this widget. The reference
                                    // says it's unused for text widgets but
                                    // the firmware empirically may emit
                                    // TextEvent CLICK with this CID — see
                                    // the Selection Patterns test bench's
                                    // canary for verification on 2.2.0.24.
};

#endif // G2_RENDER_TYPES_H
