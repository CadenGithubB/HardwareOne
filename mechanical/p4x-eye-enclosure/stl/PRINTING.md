# Revision 20 — prototype enclosure parts

**Not ready for JLC3DP production or its requested insert installation.** The
current thin walls, locating features and M2 insert layout require a separate
manufacturing decision. See the enclosure folder’s `JLC3DP_REVIEW.md` before
placing an order; mesh validity alone is not manufacturing approval.

Import each STL as a separate object, in millimeters, at 100% scale. The parts
have been positioned on Z=0; their import positions are not the assembled
positions. All five final STL files passed the export checks below.

**Print the new lid and midframe together.** Revision 19 base, button strip
and power slider can be reused. The full set still contains five parts.
Previous revision STL packages and quotation drawings are retained.

| Part | Supplied orientation | Notes |
|---|---|---|
| Base | Floor down | Inspect supports under internal shelves and USB openings. |
| Lid | Exterior face down | Insert pockets and anti-lift stop face upward. Inspect support needs around the display opening, receivers and R2 edge. |
| Midframe | Flat underside down | USB-side lower support relocated to clear RESET; cable bay and locating keys retained. |
| Button strip | Actuator stems down | Includes power-slider retainer shelf; inspect supports under shelf, arms and bar. |
| Power slider | Nub face down; socket upward | New separate moving part. Preserve flange, nub and socket dimensions; remove supports without blocking the socket. |

Use the slicer's layer preview to confirm that thin walls and button arms are
actually generated with your nozzle and extrusion width. These are generic STL
files, not printer-specific profiles or G-code. Printed fit, material strength,
button return force, and hardware clearances still need a physical trial.

## Lid alignment and screen cable

The midframe carries three 8 x 1.5 mm locating keys that rise 2.5 mm above its
plate. Chamfered tips enter blind pockets in the lid with 0.2 mm nominal
clearance per side and 0.3 mm above each key. Fit the lid and frame together
before installing electronics. The lid should lower vertically and seat freely;
the case screws secure it after the keys have located it. Do not draw a binding
key into its pocket by tightening the screws.

The LCD flex relief is now **24 x 6.35 mm**, with R0.6 corners and 0.5 mm more
upward clearance than revision 16. It retains a 1 mm skin beneath the exterior
face. Check the cable bend with the screen seated; the LCD pocket and flush
seat height are unchanged.

## PCB support and anti-lift stop

The lid's 3 x 1.2 mm rounded stop foot begins at Z21.5, **0.2 mm above the
nominal PCB top at Z21.3**. It is a travel stop, not a clamp. The display-face
bearing land at X71-74, Y41.7-42.9 is component-free solder mask, with no
drill centers or mask/paste openings found in the source review; inspect the
actual board before assembly. The stop broadens into the lid roof above it.

The old lower USB-side seat conflicted with RESET and its pads. Its replacement
is at **X69.3-70.5, Y32.75-34.75**, with the same 2.4 mm² bearing area and a
1.2 x 2 mm section. This lower solder-mask land contains tented vias. It
remains separate from the upper stop. The smallest
nominal sourced clearance is 0.275 mm to the BOOT body; the USB locating-hole
mask opening is 0.29 mm away. Verify the real solder/component envelopes.

**The lid must close freely before tightening the case screws.** Printed size
and PCB thickness can consume the stop's 0.2 mm gap. Do not force it shut or
use the screws to apply PCB preload. Physical fit and support loads are untested.

## Battery cable and power access

The midframe's former small opening and wire slot are replaced by **one open
front-right bay** with R1 corners. Its cutter spans X64.5-81, Y-1-28.65 and
opens through the front and right plate edges, removing the thin strips.
Approximately 91.3% of the battery's XY footprint remains under the plate.
The base floor and all four case-screw wells stay closed.

Orient the battery's lead corner toward case XY **(73.5,6)** and route the
lead upward through the open bay to J25. Keep slack inside the case and clear
of screws, the wheel and PCB supports. Do not trap wires in the 0.5 mm gap
beneath the remaining plate. The front PCB seat is now X60-63, with its
previous Y and Z placement retained.

Revision 19 replaces the exposed SW7 opening with a **separate captive power
slider**. Its nub is 6 x 3.8 mm and projects 1 mm above the lid. A 14 x 7.6 mm
flange covers the 9 x 4.4 mm running slot from underneath. An integral shelf on
the button strip captures that flange using the strip's existing two screws.
The socket is 2 mm square with a 2.4 mm entry, sized around the nominal
1.5 mm-square switch handle. The flange overlaps the slot throughout its
3 mm nominal travel; this is not a waterproof seal.

With the lid separate, insert the cap from inside and fit the button strip to
capture it. Check free motion. When lowering the cover, align the cap socket
with the original SW7 handle. Do not force the cap onto the switch or glue the
moving slider. Check both switch positions and the cap's full travel.

There is 0.3 mm nominal Y clearance at the running guides, and 0.2 mm above
and below the flange at its nominal height. The socket has only 0.25 mm per
side around the nominal handle, so lateral handle contact can occur before
the guide limits movement. The retainer carries intended downward loads; it
does not isolate all sideways loads. Keep the slot, guides, retainer
opening and socket free of clear-coat buildup and adhesive; clear gloss is
for exterior cosmetic surfaces. The switch drawing and BOM disagree on handle
height (2 versus 2.5 mm), so actual engagement and the shelf/bridge load path
need a fit prototype. The strip retains anchors at (63,19) and (63,36).

## Hardware and assembly

- Four M2 × 8 mm nylon case screws and two M2 × 8 mm nylon midframe screws.
- Six heat-set inserts, outside diameter 3 mm and length 3.2 mm.
- Printed insert pilots are 2.7 mm with a 3.1 mm entry. Verify the fit with your
  insert and material before heat-setting all six. Fit inserts with the battery
  and electronics removed.
- The separate PCB mounting pilot remains 1.2 mm for a provisional M1.6 screw;
  it is not one of the six M2 insert mounts.
- Screen retention, the button strip anchors, and PCB seating still need the
  physical fit check described in the parent README.

## Export checks

All five final revision 20 meshes are watertight with consistent winding,
positive volume and one closed component each. Checks found no degenerate or
duplicate triangles, boundary edges or nonmanifold edges. Each mesh begins
at minimum X, Y and Z of zero. The manifest records sizes, orientation
transforms, source/mesh hashes and per-part results.

Base, button strip and slider hashes match revision 19. The base retains its
previous cleanup of five zero-area faces using existing vertices without
moving coordinates; lid and midframe are clean revision 20 CGAL exports.
All four drawing pages passed independent text/dimension/hash checks and
rendered visual inspection. Numerical viewer checks passed with 73,814 finite
faces and exact SW1/SW2/encoder pad envelopes. The PCB travel stop browser
cutaway showed the gap cleanly with no reported console errors or warnings.
Physical fit, PCB preload clearance, operating forces, supplier acceptance
and support settings remain unverified.
