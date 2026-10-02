# Revision 21 - prototype enclosure parts

**Not ready for JLC3DP production or its requested insert installation.** The
current thin walls, locating features and M2 insert layout require a separate
manufacturing decision. See the enclosure folder's `JLC3DP_REVIEW.md` before
placing an order; mesh validity alone is not manufacturing approval.

Import each STL as a separate object, in millimeters, at 100% scale. The parts
have been positioned on Z=0; their import positions are not the assembled
positions. All six final revision 21 STL files passed the export checks below.

**Print the new lid, button strip and keeper together.** Revision 20 base,
midframe and power slider can be reused. The full set contains six parts.
Previous revision STL packages and quotation drawings are retained.

| Part | Supplied orientation | Notes |
|---|---|---|
| Base | Floor down | Inspect supports under internal shelves and USB openings. |
| Lid | Exterior face down | Inspect supports around keeper ledges, display opening, insert pockets, receivers and R2 edge. |
| Midframe | Flat underside down | USB-side lower support relocated to clear RESET; cable bay and locating keys retained. |
| Button strip | Actuator stems down | Old screw holes filled; includes power-slider shelf. Inspect supports under shelf, arms and bar. |
| Button keeper | Flat | 1 mm thick; preserve tabs and their bearing faces. |
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
use the screws to apply PCB preload. Physical fit was verified by the builder; support loads remain untested.

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
slider**. Its nub is 6 x 3.8 mm and projects nominally 1 mm above the lid. A 14 x 7.6 mm
flange covers the 9 x 4.4 mm running slot from underneath. An integral shelf on
the button strip captures that flange; the new keeper supports the strip on
four lid ledges.
The socket is 2 mm square with a 2.4 mm entry, sized around the nominal
1.5 mm-square switch handle. The flange overlaps the slot throughout its
3 mm nominal travel; this is not a waterproof seal.

With the lid separate, insert the cap from inside, then fit the button strip
and keeper as described below. Check free motion. When lowering the cover, align the cap socket
with the original SW7 handle. Do not force the cap onto the switch or glue the
moving slider. Check both switch positions and the cap's full travel.

There is 0.3 mm nominal Y clearance at the running guides, and 0.2 mm above
and below the flange at its nominal height. The additional 0.15 mm strip
settling makes total cap float **0.55 mm**; the lowest cap position retains
**0.25 mm nominal clearance** above the switch body and taller provisional
handle. The socket has only 0.25 mm per
side around the nominal handle, so lateral handle contact can occur before
the guide limits movement. The retainer carries intended downward loads; it
does not isolate all sideways loads. Keep the slot, guides, retainer
opening and socket free of clear-coat buildup and adhesive; clear gloss is
for exterior cosmetic surfaces. The switch drawing and BOM disagree on handle
height (2 versus 2.5 mm), so actual engagement and the shelf/bridge load path
need a fit prototype. The strip bears against solid roof datums at (63,19)
and (63,36), with no screw pilots.

## Screwless button-strip keeper

The changed lid and strip require the new separate **8.5 x 20.5 x 1 mm keeper**.
The former strip holes and lid screw pilots are filled. The strip drops in
vertically; only the keeper slides. Lid side guides locate the broad retaining
shelf with 0.2 mm nominal gaps. Its **7.8 x 5.6 mm R0.3 opening** allows for
strip translation and yaw; keep the guide faces and opening finish-free.

1. With the lid off and the power cap inserted, fit the strip vertically and
   hold its fixed bar against the two solid roof datums.
2. Lower the keeper at **+3.3 mm in Y** from its final position, then slide it
   **3.3 mm toward -Y** beneath the four ledges until it reaches the front stop.
3. Check independent button return and both power positions. Apply a **required
   small removable resin-compatible silicone bead** bridging an accessible
   rear keeper tab and the fixed ledge. Keep silicone off the arms, cap guides
   and socket; let it cure before use.

The ledges carry operating loads. The silicone prevents the keeper from
sliding back out; this is adhesive retention, not a snap fit, friction latch
or positive mechanical lock. Removal requires peeling/cutting away the bead,
sliding toward +Y and lifting out. Check silicone compatibility with the
actual printed materials.

The keeper sits directly on the ledges at Z24.65 and ends at Z25.65. The strip
underside is nominally Z25.8, allowing **0.15 mm total downward movement**.
The keys project **0.85-1.0 mm** above the case and retain **0.15-0.30 mm
nominal rest gap** before component and print tolerances. With the nominal
0.2 mm switch stroke, travel to actuation is **0.35-0.50 mm**. Do not add another gap beneath the keeper. Keep the ledge/tab
bearing surfaces free of coating buildup and check free fit without forcing.

## Hardware and assembly

- Four M2 × 8 mm nylon case screws and two M2 × 8 mm nylon midframe screws.
- Six provisional M2 inserts, reference outside diameter 3 mm and length 3.2 mm.
  The existing Ø2.7 mm pilots were designed for heat-setting. For clear resin,
  the supplier must confirm bonded insert/pocket dimensions; no thermal
  insertion into cured resin is specified.
- The independent PCB mounting pilot remains 1.2 mm for a provisional M1.6
  screw; it is not one of the six M2 insert mounts.
- No button-strip screws or metal inserts. The separate keeper and required
  removable silicone bead provide its retention.
- Screen retention, controls and PCB seating were physically fit-checked by
  the builder.

## Export checks

Revision 21 CAD changes the lid and button strip and adds the keeper. Base,
midframe and power slider hashes match revision 20 exactly. All six final
meshes are watertight with consistent winding, positive volume and one closed
component each; there are no degenerate/duplicate triangles or
boundary/nonmanifold edges. Each starts at minimum XYZ=0. Actual file and
source hashes match the manifest. The three-page PDF passed independent
file/hash, dimension and text-scope checks; all three final rendered pages
passed visual inspection. Package verification is recorded separately in
`../validation/package-validation-r21.json`. Numerical viewer checks passed with
72,216 finite faces and 64 simple perimeter rings. The Button keeper browser
view was inspected after the final refinements, seated, at its +3.3 mm Y entry
position and lowered, with no reported console errors or warnings. The package record predates the builder’s physical-fit check. The base retains its prior cleanup of five zero-area faces using
existing vertices without moving coordinates.

Revision 20's five final meshes and shortened three-page drawing passed their
recorded checks and are preserved in the repository Git history. The R21
PDF remains three pages, with the strip and keeper assembly on sheet 3;
standalone midframe and captive power-slider sections remain omitted.
