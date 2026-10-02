# ESP32-P4X-EYE enclosure without camera

Revision 21, 2026-09-29. **Prototype; physical fit unverified and not ready for JLC3DP production.**

The current thin walls, locating features and M2 insert arrangement need a
manufacturing review before ordering the proposed clear finish and installed
inserts. See [JLC3DP_REVIEW.md](JLC3DP_REVIEW.md) for the process limits and
hardware decision. A closed STL mesh does not establish supplier acceptance.

PCBWay review request prepared on 2026-09-29 in
`PCBWAY_REVIEW_REQUEST_R21.txt`, with a supplier-facing package at
`p4x-eye-pcbway-review-r21.zip` (six STLs, matching PDF, manifest, assembly
notes and review request). Package CRC and member bytes passed verification.
**Not submitted: signed in; six R21 STLs uploaded and dimensions verified.**
Quantity 1 each and UTR-8100 transparent with spray varnish are confirmed.
Drawing attachment, product description and insert/review options remain.
The user rejected the lengthy extra form notes; all six special-request fields
are blank and must stay blank unless requested otherwise.
The six current models are staged in `pcbway-upload-r21/models/`; the drawing
and request are one directory above. See `PCBWAY_SUBMISSION_STATUS.json`. Factory-installed bonded
M2 inserts are the first request; customer adhesive installation is the fallback
for review. Neither route permits heat-setting into the proposed cured SLA
resin. Actual insert, adhesive and pocket specifications must be confirmed
before changing the CAD or approving manufacture. The quotation form requests
individual STL uploads, not a ZIP; extract this package for web submission.

The proposed case retains the PCB, LCD, controls, microphone, wireless antenna,
USB ports and microSD slot. The detachable camera is omitted. Revision 21
replaces the two button-strip screws with a **separate printed keeper**. Four
lid ledges support its tabs beneath the strip's fixed bar. A **required small,
removable resin-compatible silicone bead** bridging an accessible rear keeper
tab and its fixed ledge prevents it from sliding back out. The ledges carry
operating loads; the silicone provides anti-backout retention. This is not a
snap fit, friction latch or positive mechanical lock.

**Print the new lid, button strip and keeper as a matched set.** The revision 20
base, midframe and captive power slider remain reusable. There are now **six
printed parts**. The six M2 case/frame insert locations and the **80 x 56 x 29
mm body height** are unchanged. No button-strip screws or inserts are required.

The retained PCB stop has **0.2 mm nominal clearance** above the PCB and the
lower USB-side seat remains clear of RESET. The upper stop and lower seat act
at different locations; they are not a preloaded clamp. The lid must close
freely before tightening the case screws.

The power slider still has a nub nominally 1 mm above the lid and is retained by the
button strip's integral shelf. The keeper allows **0.15 mm total downward
strip movement**. This reduces the nominal button rest gap from 0.30 to **0.15
mm** and increases the cap's possible total vertical movement from 0.40 to
**0.55 mm**. At its lowest position, the cap has **0.25 mm nominal clearance**
above both the switch body and the taller provisional handle. These figures
exclude component and printing tolerances; physical fit remains unverified.

The revision 17 **24 x 6.35 mm LCD flex relief** and three midframe locating
keys are retained. The keys slide into blind lid pockets to resist lateral
movement and twisting, while the existing four case screws hold it closed.
The battery cable bay is cut by an R1 rectangle at **(64.5,-1)** with size
**16.5 x 29.65 mm**, extending through the plate's front and right edges. The
midframe still covers approximately **91.3% of the battery's XY footprint**.
Its front PCB seat remains at X60-63, clearing the open bay.

Revision 15's local rounded scallop remains beside the rear USB-C port for the user's **10.25 mm
wide x 6 mm tall plastic plug**. The case-screw axis stays at **(77,41)**, and
the rotary wheel, battery, midframe perimeter and screw hardware retain their positions.
The retained exterior wheel relief is **1 mm total**, exposing **2.1 mm of
the wheel beyond the local edge at Y=44.5**. Both halves keep the same uniform
wheel-side outline. The lower wheel wall is strengthened inward to **1.5 mm**. The upper
wall is **1.1 mm** except across the encoder/PCB clearance band at
**Z=17.7-21.6**, where it is **0.6 mm**. That thinner band preserves component
clearance and still needs a printed strength and fit check.

The battery remains at **(8.5,6.0)**, with
bounds **X=8.5-73.5, Y=6.0-42.0 mm**. The midframe follows the revised lower
cavity. The right case-screw axis remains at **(77,41)**, allowing its closed
well to join the wheel edge without a projecting corner. The wheel opening
remains **13 mm** wide. The design retains **six M2 x 8 mm nylon screws and six
3 x 3.2 mm inserts** across the four case fixings and two independent midframe
fixings.

The selected PCB screw now fastens into a **mount that belongs entirely to the
midframe**. Two separate screws fasten the midframe to the base at **(5,8.7)**
and **(44,48.5)**. The lower USB saddle now belongs to the base, so the board
can be attached to the midframe before that assembly is lowered into the case.
The plate follows the actual lower interior with 0.3 mm nominal fit clearance,
except at its open cable bay, and retains a flat battery-facing underside
around the through-cuts.

The full LCD remains nominally flush with the upper cover. Three independent
keys project **0.85-1.0 mm above the face**, depending on strip settling, and
the outer top and bottom face edges
now use **2 mm rounding** with protected screw-access rims. The solid rectangular
PCB locator is retained. Body dimensions remain **80 x 56 x 29 mm**, or
**nominally 30 mm high including keys**; the 56 mm maximum is at the PCB tab region.

Four **M2 x 8 mm nylon screws** enter deep underside wells, with head seats
**10.2 mm inside the case**, and engage blind upper-cover inserts. The wells
are closed to the battery bay. The user's inserts remain **3 mm diameter x
3.2 mm long**. The 8 mm screw length is treated as below-head length; actual
head shape, dimensions and printed fit still require checking. The top has
no case-screw holes.

## Files

- `enclosure.scad`: editable OpenSCAD shell and lid; choose `part`
  for assembly, exploded view or individual parts, including `button_strip`,
  `power_slider` and the new `button_keeper`.
  The `button_keys` option independently enables the button geometry. Default outside dimensions
  are **80 x 56 x 29 mm** for the body, or **nominally 30 mm high including keys**.
  The user's **36 x 65 x 10 mm** battery is rotated to
  **65 x 36 x 10 mm** inside the case, with its lower-left corner at case
  XY **(8.5, 6.0)**; its X span is **8.5-73.5 mm** and Y span is
  **6.0-42.0 mm**.
- `pcb-outline.svg`: exact-scale reference outline and 20 mm scale bar. Print
  at 100%, with page scaling disabled, and check the scale bar. This is a
  footprint template, not an enclosure production drawing.
- `dimensions.json`: sourced XY geometry and explicitly provisional values.
- `wheel-before-after.png`: geometry-based comparison of revision 13 and
  revision 14 around the wheel, using identical camera and scale.
- `usb-before-after.png`: revision 14-to-15 USB comparison at the same angle
  and scale, with the measured 10.25 x 6 mm plastic plug nose outlined at X=76.
- `p4x-eye-stl-r21.zip`: verified six-part prototype package, including the three-page PDF and printing notes.
- `p4x-eye-pcbway-technical-drawing-r21.pdf`: three-sheet quotation drawing.
  It retains lid/base insert details and shows the button strip and keeper
  assembly on sheet 3. Standalone midframe and power-slider sections remain
  omitted. Independent file/hash, dimension and text-scope checks passed;
  all three final rendered pages passed visual inspection.
- `p4x-eye-stl-r20.zip` and `p4x-eye-pcbway-technical-drawing-r20.pdf`: prior
  five-part package and shortened three-sheet drawing. The matching source,
  docs and PDF are preserved in `work/p4x-enclosure/revision20/`.
- `p4x-eye-stl-r19.zip` and `p4x-eye-pcbway-technical-drawing-r19.pdf`: retained
  prior five-part revision and original four-page slider drawing.
- Earlier revision 18, 17 and 16 output packages remain available as history.
- `stl/`: individual STL files, export manifest and `PRINTING.md` notes.

Revision 21 changes the lid and button strip and adds the separate keeper.
Base, midframe and power slider retain revision 20 geometry. Six M2 case/frame
fastenings retain their positions. All six final R21 meshes are watertight,
consistently wound, positive-volume and single connected components, with zero
degenerate/duplicate triangles or boundary/nonmanifold edges. Source and mesh
hashes match the manifest. Base, midframe and power-slider hashes match R20
exactly. The PDF passed independent file/hash, dimension and text-scope checks, and
all three final pages passed rendered visual inspection. The 10-file ZIP passed
CRC and exact member-byte checks; source/STL hashes match the manifest. Numerical viewer checks passed with **72,216 finite
faces and 64 simple perimeter rings**. The **Button keeper** browser view was
inspected at 0% seated, 50% at the +3.3 mm Y entry position, and 100% lowered,
after the final shelf-guide/slot refinements, with clean rendering and no
reported console errors or warnings. Screenshot:
`work/p4x-enclosure/keeper-r21-preview.png`. These checks do not establish the
keeper's printed fit or strength.

| Current STL file | Exported size X x Y x Z, mm | Export orientation |
|---|---|---|
| `stl/p4x-eye-base-r21.stl` | 80 x 56 x 22.83 | Floor down |
| `stl/p4x-eye-lid-r21.stl` | 80 x 56 x 14.5 | Exterior face down |
| `stl/p4x-eye-midframe-r21.stl` | 72.35463 x 51.4 x 8.1 | Flat underside down |
| `stl/p4x-eye-button-strip-r21.stl` | 22.77320 x 31.3 x 5.9 | Stems down; supports needed |
| `stl/p4x-eye-button-keeper-r21.stl` | 8.5 x 20.5 x 1 | Flat |
| `stl/p4x-eye-power-slider-r21.stl` | 14 x 7.6 x 4.6 | Nub down; socket up |

Final triangle counts are **30,586 base; 59,936 lid; 1,668 midframe; 2,736
button strip; 76 keeper; 972 power slider**. OpenSCAD 2026.09.23 CGAL exports
used `--hardwarnings`. The final lid unions the initial R21 render with the
two source-defined shelf guides, adding 4.8 mm³; all source assertions passed.
The base retains its prior cleanup without moved vertices.

The retained revision 20 five-part meshes passed watertightness, winding,
positive-volume and single-component checks, with no degenerate/duplicate
triangles or boundary/nonmanifold edges. Its numerical viewer checks passed
with 73,814 finite faces; its PCB travel stop cutaway was visually inspected.
The R20 drawing was shortened to three pages on 2026-09-29 and checked. The
original four-page R20 PDF remains in
`work/p4x-enclosure/drawing-r20-before-section-removal/`.

Delivery meshes sit at Z=0 and should be imported as separate
objects in millimeters at 100% scale. Review `stl/PRINTING.md`; physical fit,
insert installation, board support, slider operation and strength remain
unverified.

An inline 3D viewer was added to the Codex task on 2026-09-27. It supports
rotation, zoom, exploded layers, case transparency and lid visibility. It is a
dimension-matched visual preview, not a compiled SCAD mesh; small details and
component envelopes are simplified. Its self-contained source is
`~/.codex/visualizations/2026/09/27/01a0e470-16f8-7de2-afc9-6933b2a473e2/p4x-enclosure-unified-body.html`.
The wheel, its mounting body, midframe contact areas and upper contour are
provisional visual references. Viewer checks do not establish physical fit or
replace a compiled CAD check.
Historical revision 15 passed JavaScript syntax and browser checks without reported
warnings or errors. All **67,859 generated faces** were finite and all 64
sampled R2 inset rings were simple; prior pack, frame and closed-well checks
still passed. The measured plug envelope plus **0.2 mm per side in Y and Z**
cleared for the assumed nose at **X>=76**, sampled every 0.025 mm from
Z=19.73 to 26.13. The scallop removed no area from the full 4.4 mm post or
interior clearance channel, and the minimum sampled local cavity web was
**about 0.839 mm** in both upper wall bands. Relief was confined to Z=19.2-26.8.
The USB view and same-camera revision 14-to-15 comparison were inspected,
including the outlined plug and approximately 0.38-to-0.98 mm side gaps.
The earlier wheel comparison is unchanged. These viewer checks preceded the
STL export and do not establish physical fit. The separate CAD compilation
status is recorded above.
The fastener preview shows the installed 3 mm insert envelope; the unheated
printed pilot in CAD remains a provisional 2.7 mm. These checks do not
establish insert strength, spring return or physical fit. Revision 16 generated
**67,911 finite viewer faces**. Checks confirmed one connected frame polygon,
a connected cable cut, a clear front seat and an uncovered lead corner; prior
numerical geometry checks still passed. The midframe detail was inspected in
the browser without reported warnings or errors.
Revision 17 numerical viewer checks passed with **69,204 finite faces**.
Browser inspection passed: the **Lid alignment keys** view showed the three
keys and receivers at 12% separation, and **Display seat** showed the enlarged
flex relief with 24 x 6.35 mm dimensions. No console warnings or errors were
reported. Revision 18 also passed numerical checks with **73,125 finite faces**
and **64 simple R2 rings**. Its **Power switch** and **Midframe detail** browser
views were inspected with no reported console warnings or errors. These are
historical revision 18 results. Revision 19 passed numerical checks with
**73,560 finite faces**, **64 simple R2 rings**, and cap travel/overlap and
retainer checks. Its **Power switch** and **Button mechanism** browser views
were visually checked without reported warnings or errors. Those are historical
revision 19 results. Revision 20 passed geometry checks with **73,814 finite
faces**, including SW1/SW2/encoder pad envelopes; its **PCB travel stop** browser
cutaway showed the gap with no reported console errors or warnings. Physical fit remains unchecked. The manufacturer remains undecided; the user
ruled out Protolabs on price.

## What is established

The official P4X archive contains a P4-EYE MB V2.3 PCB layout/Gerbers dated
2025-04-11 and a P4X V2.4 schematic/BOM dated February 2026. Its Gerber coordinates
give an intact PCB envelope of **69 x 43 mm**. Most of the body is 37 mm tall;
the remaining tab carries the microphone and LED. Removing the camera does not
remove that PCB tab.

The linked LCD datasheet specifies a **31.52 x 33.72 x 1.9 mm** module with a
**27.72 x 27.72 mm** active area. Nominal thickness tolerance is **+/-0.1 mm**.
The drawing also gives **30.12 x 33.12 mm glass** and a
**29.72 x 30.12 mm polarizer**. The new seat fits the complete module outline,
not an interference fit around its glass. The earlier **28.32 mm square**
bezel viewing window is replaced by a full-module pocket. The active area
has 1.90 mm borders left and right, a 1.45 mm border at the non-flex edge and a
4.55 mm border at the flex edge. Its center is therefore 1.55 mm toward the
non-flex edge from the module center. The installed module's position, flex
orientation/routing and height above the PCB still need measurement. The
reference archive includes PCB/electrical files but no original enclosure CAD
or dimensioned LCD assembly drawing.

The board has an AP5056 charger, set to **500 mA**, with **4.2 V** charge voltage.
Select one 4.2 V-charge Li-ion/LiPo cell (normally 3.7 V nominal), preferably a
protected pack, whose datasheet permits at least 500 mA charging. The user has
supplied the pack's physical dimensions, but its model, chemistry, capacity,
charge rating and connector have not yet been identified. Those electrical
details are independent of the mechanical fit and must be checked before use.

J25 is HCTL HC-1.25-2AW, a two-pin 1.25 mm-pitch right-angle connector. Schematic
pin 2 is positive; pin 1 is ground. Check the actual mating housing and PCB
polarity, not just connector pitch. Either USB-C port supports charging. The
stock enclosure's documented battery limit is 45 x 25 x 4 mm.

## Draft limitations and next measurements

The battery thickness is user-specified. The **29 mm depth is a provisional
packaging target**, not a measured minimum. The board's thickness remains an
assumed 1.6 mm, with a 5.7 mm front component allowance. The stack is:

| Layer | Z span, mm |
|---|---:|
| Rear shell floor | 0-2 |
| Battery bottom clearance | 2-2.5 |
| Battery | 2.5-12.5 |
| Battery-to-midframe clearance | 12.5-13 |
| Flat midframe plate | 13-14.5 |
| Reserved underside component space | 14.5-19.7 |
| PCB | 19.7-21.3 |
| Reserved LCD/control space | 21.3-27 |
| Removable upper-cover walls and screw pillars, except USB split | 14.5-27 |
| Base-owned USB saddle, with lower port notches | 14.5-22.83 |
| Base USB saddle-to-cover assembly seam | 22.83-23.03 |
| Upper cover above USB rail, with upper port notches | 23.03-27 |
| LCD rear-frame support ledge | 25.9-27.1 |
| Provisional LCD module, flush with outside face | 27.1-29 |
| Nominal user-switch tops | 23.8 |
| Button stems | 24.1-25.8 |
| Removable button strip and tongues | 25.8-26.6 |
| Button key tops, including 0.15 mm strip settling | 29.85-30 |
| Upper-cover faceplate | 27-29 |

The battery-to-PCB distance is **7.2 mm**, but the new plate leaves only
**5.2 mm for components below the PCB**, before allowing for solder joints,
assembly tolerances and clearance. Physical stack measurements are critical.
The expansion header, radio module and encoder overlap the battery footprint.
J22 is a 2x10 right-angle SMT female header with 2.54 mm pitch; the official BOM
omits its manufacturer and exact part number, so its height is unverified.
U11 is ESP32-C6-MINI-1U-N4, with a datasheet envelope of 13.2 x 12.5 x 2.4 mm.
SW6 is Mitsumi SIQ-02FVS3. The draft retains these parts; their installed
heights, solder joints, antenna lead and wiring must clear the midframe.

The battery locating pocket has **67 x 38 mm** nominal internal clearance,
now beginning at **(7.5,5.0)** around the battery at **(8.5,6.0)**. Its low
1.4 mm guides locate the pouch without compressing it and move with the pack.
The front-left frame post has **0.75 mm nominal battery clearance**, the rear
post has **3.5 mm**, the closed right case-screw well has **0.5 mm**, the lower
USB wall has **0.7 mm**, and the lower rear wall has **1 mm** at the straight
wheel edge. The nearest front case-screw well has about **0.514 mm** clearance
to the battery corner. Guide geometry is trimmed around the two frame posts and case-screw
wells and merges into the case wall where their envelopes overlap; these small
clearances require a physical check. Confirm
that pack dimensions include its protection board and measure its lead exit.

**Both case halves use the same uniform outer contour.** The USB side remains
at **X=75.8**, and the wheel-side edge is **Y=44.5**. The rear-left PCB-tab region
still reaches Y=56, preserving the **80 x 56 mm** envelope. The nominal rear
curve remains the old 24-segment reference from **(59,45.5)** through controls
**(50,45.5), (48,56)** to **(38,56)**. The visible edge is formed by linearly
subdividing this reference to segments no longer than 0.25 mm and applying the
1 mm reduction, with a 2 mm cubic smoothstep transition at **X=53.65-55.65**.
Full relief continues rightward into the new corner; there is no right-hand
blend back to the old protruding edge.

The right screw axis moves to **(77,41)**. The USB flat joins its **R3 closed
well** through a tangent **R0.3 concave arc**, with center approximately
**(76.1,37.825)**. The well arc meets **(80,41)**, followed by an **R3.5 rear
arc centered at (76.5,41)** that ends at **(76.5,44.5)**, level with the wheel
edge. The 4.5 mm screw-head bore retains its **0.75 mm nominal radial rim** (about 0.749 mm in the sampled
outline check).

The original interior offsets remain the starting reference: 2 mm generally
and 1.6 mm in the USB region for the lower half; 1.6 mm generally and 1 mm in
the USB region for the upper half. The lower cavity is then clipped to a
**1.5 mm inset of the new exterior**, moving its rear inner edge from Y=43.5
to **Y=43.0**. The midframe and support ledge follow this actual cavity.
Above and below the encoder/PCB band, the rear upper cavity is additionally
clipped to a **1.1 mm inset**, giving **Y=43.4** at the inner wheel wall.
This reinforcement applies behind Y=41.5 so the USB-side geometry is retained.

Across **Z=17.7-21.6**, the upper wall uses a **0.6 mm inset** instead, leaving
its inner edge at **Y=43.9** to clear the encoder and PCB. This retains the
nominal 0.4 mm clearance to the PCB edge at Y=43.5. The exterior remains uniform
through all three height bands; no outward reinforcement bulge is added.
The 0.6 mm band is an intentional clearance compromise and needs physical
strength and fit testing. The upper walls and faceplate lift off together,
while the raised lower USB saddle belongs to the base.

The outside face edges retain **R2 rounding at Z=0-2 and Z=27-29**, modeled
with 64 angular offset slices. Protected screw-mouth footprints retain their
closed rims. The LCD seat, key projection and horizontal assembly seam remain
unchanged.

The wheel center is **(62.9,39.35)**, with rim at Y=46.6, now projecting
**2.1 mm beyond both case halves**. Its bottom-open cover notch widens to
**13 mm**, at **X=56.4-69.4, Z=14.5-18.1**. This clears the larger dial chord
at the reinforced inner wall. The battery now ends at Y=42.0; its rear wall
clearance remains 1 mm after the lower wall moves inward.

The removable **1.5 mm midframe at Z=13-14.5** has a flat underside 0.5 mm
above the battery. Its perimeter is offset **0.3 mm inward from the actual
lower interior**, including the locally thicker USB wall; it is not a uniform
rounded rectangle or a press fit. Four **R2.9 reliefs** clear the 5.2 mm upper
insert pads by 0.3 mm, with the right relief centered at **(77,41)**.
Two **2.3 mm clearance holes** accept the M2 screws fastening the frame
independently to the base. The PCB
mount is integral to the frame and has no pass-through hole to the base.
A continuous ledge at **Z=11.5-13** supports the plate, extending **1.4 mm
inward from the actual lower interior**, with the battery footprint expanded
by **0.2 mm** removed for clearance. The narrowest nominal plate overlap at
the straight USB side is **0.2 mm**, with a **0.5 mm shelf**; the rear shelf is
**0.8 mm** with **0.5 mm plate overlap**. These narrow supports need a strength
check. The case-screw support shoulders retain their larger **0.7 mm battery
relief**. The perimeter seam is not a water seal.

Revision 18 replaces the former plug opening, wire slot and entry relief with
**one R1 rounded cable bay** through the **Z13-14.5 mm** plate. Its cutter spans
**X=64.5-81, Y=-1-28.65**, extending through the plate's front and right edges.
This removes the narrow strips around the previous route, leaving a single
open space near J25. Approximately **91.3% of the battery's XY footprint**
remains beneath solid midframe; this area figure is not a measured clamping
load. The outer case, battery floor and four screw wells remain closed.

Orient the battery's lead corner toward **(73.5,6)**, bring the lead into the
open bay, and connect to J25 while accessible. Keep slack inside the case and
away from the PCB supports and screws. Do not squeeze the wire through the
**0.5 mm gap under the remaining plate** or the narrow screw-well clearance.
The reported plug width is 5 mm; its other dimensions, wire bend radius and
physical routing still require checking.

The Mitsumi SIQ-02FVS3 drawing (page 3) specifies a **14.5 mm wheel diameter,
2 mm dial thickness and 4 mm overall projection**. The nominal dial spans
**Z=15.7-17.7 mm**, 1.2 mm above the frame and 0.4 mm below the notch top.
The notch has about 0.49 mm nominal side clearance to the dial at the
inner wall, slightly reduced near its upper rounded corners. Installed mounting and
solder offsets are unverified. This initial contour,
finger access, wheel travel, edge roundover and frame stiffness need physical
checks before the shape can be treated as finished.

Both USB-C access windows are **enabled by default**. Official BOM entries
J23 (Debug) and J6 (High Speed) specify **HRO TYPE-C-31-M-12**, a right-angle
surface-mount receptacle. The manufacturer's drawing gives **8.94 mm width x
7.35 mm depth x 3.26 mm mounted height**; its webpage instead lists 3.16 mm
height. The installed height therefore remains unverified. The draft uses the
drawing's 1.63 mm center height above the mounting plane, giving provisional
case **Z=22.93 mm** above the PCB's display-facing surface at Z=21.3.

The common USB-side outer wall is **X=75.8** over **Y=11-37.825**, with
transitions into the front corner and closed right screw well. Below the
midframe its inner face is **X=74.2**. The raised USB saddle and matching
upper wall remain **1 mm** thick at **X=74.8-75.8**, leaving 0.3 mm nominal
clearance to the PCB edge at X=74.5. The socket faces at **X=76** project
**0.2 mm beyond the exterior**. The base brace widens inward below the board,
so it does not add an exterior step.

Each rounded opening is **9.8 mm along Y x 4.2 mm along Z**, with **0.5 mm
corner radii**. Debug is centered at case **Y=17.5**, spanning **Y=12.6-22.4**;
High Speed is centered at **Y=32.5**, spanning **Y=27.6-37.4**. Both span
**Z=20.83-25.03 mm**. The nominal straight-edge allowances around the 8.94 x
3.26 mm connector body are 0.43 mm per side and 0.47 mm vertically. Printed
fit, actual connector height and plug overmold clearance require checking.

The **base** carries the raised lower USB saddle at **X=74.8-75.8,
Y=11.2-37.8, Z=14.5-22.83**, with open lower port notches. The upper cover
carries their matching upper halves from **Z=23.03 to 27**. The 0.2 mm vertical
and end seams preserve vertical insertion. An **inward triangular brace**
occupies **X=74.2-74.8, Z=14.5-17.5**, supporting the rail from the lower case
wall. The midframe carries no raised USB rail. Its underside remains flat,
and the two frame screws are accessible after the board is attached to it.
The 0.4 mm end web beyond the High Speed opening requires a print-strength check.

The right case-fastener axis is **(77,41)**. At USB height its 4.4 mm neck
starts at Y=38.8, leaving **1.4 mm nominal separation** from the High Speed
opening at Y=37.4. The wider 5.2 mm insert pad tapers below the PCB underside.
The revised return keeps the 6 mm lower well fully enclosed. Rail strength,
component clearance, insertion and screw/plug access still need a prototype.

The measured **rear USB plug plastic is 10.25 mm wide x 6 mm tall**. Centered
on the High Speed port at Y=32.5, Z=22.93, its reference envelope is
**Y=27.375-37.625, Z=19.93-25.93**. This assumes its plastic nose starts at
**X=76 or farther outward**; plug depth, taper and actual inserted position
have not been measured. The board's 9.8 x 4.2 mm connector openings remain
unchanged because they clear the metal receptacle, while the larger plastic
body stays outside the USB wall.

A local cable scallop removes material at **X=75.8-81, Y=36.8-38.6,
Z=19.2-26.8**. It has **R0.2 internal plan corners** and **R0.5 top/bottom
rounds**. The full-depth span at **Z=19.7-26.3** covers the measured plug height.
At the assumed nose plane, the former nearby obstacle began around Y=38.0001;
the new scallop moves it to **Y=38.6**, raising the nominal side gap from
**0.3751 mm to 0.975 mm**, about **0.6 mm more clearance**.
The full **4.4 mm upper case post**, its insert and blind tip cavity, the closed
lower screw well and the cover roof are preserved. The remaining local cavity
web measured about **0.839 mm** in the sampled geometry checks. The screw axis and rotary
geometry do not move. The reported 0.15 print setting does not establish
strength or physical fit; the actual cable and printed scallop need checking.

The upper face now has a separately enabled **31.92 x 34.12 mm LCD module
pocket**, with **0.3 mm corner radii** and **0.2 mm nominal clearance on each
side** of the 31.52 x 33.72 mm module. The pocket bounds are
**X=14.04-45.96, Y=8.25-42.37 mm**. The display drops in from the front; its
whole module border is visible at the case face. The nominal rear is
**Z=27.1** and front is **Z=29**, level with the cover's outside surface.
The adjustable seat-depth parameter defaults to **1.9 mm**. Actual module
thickness and printed dimensions must be measured before calling the result
flush; a friction fit is not guaranteed and the glass is not an interference
fit or a screw-clamping surface.

A **1.2 mm rear support ledge at Z=25.9-27.1** joins the faceplate. Its outer
collar is **34.92 x 37.12 mm**, at **X=12.54-47.46, Y=6.75-43.87**. The
central opening is **29.52 x 31.72 mm**, at
**X=15.24-44.76, Y=9.45-41.17**, leaving **1 mm nominal overlap** beneath the
module's rear frame perimeter. The revision 17 **24 x 6.35 mm flex relief**
occupies **X=18-42, Y=5.9-12.25, Z=25.8-28.0**, with **R0.6 corners**.
Compared with revision 16, this adds 1 mm on each side, 0.75 mm at each end,
and 0.5 mm upward for the cable bend. The relieved faceplate region retains
**1 mm to the exterior surface at Z29**. The LCD pocket, flush seating depth
and remaining support ledge stay unchanged. Confirm the actual module has
suitable rear frame bearing areas and that its cable clears the relief. The collar
bottom locally leaves **4.6 mm above the PCB's display-facing surface**, so
components beneath it must clear that height. No adhesive bores or bond wells
are included.

The close pocket locates the display; the ledge sets its depth. If it needs
retention, add small accessible fillets between the **rear module frame and
the seat** using electronics-grade, noncorrosive, neutral-cure silicone
compatible with the module and printed material. Keep it off the glass,
polarizer and flex. The [Dow electronics adhesives guide](https://www.dow.com/documents/11/11-3921-01-advanced-silicone-adhesives-and-sealants.pdf?iframe=true)
describes this class of material; [DOWSIL 3145](https://www.dow.com/en-us/pdp.dowsil-3145-rtv-mil-a-46146-adhesive-sealant.01059548z.html)
is one example, not a requirement for this design. Generic household silicone
is not assumed suitable. The lid screws secure the cover and do not clamp the
display glass.

The active area remains centered provisionally at case **(30,26.86)**. The
reference display assumes the flex exits toward negative Y, placing the full
module center at **(30,25.31)** and its bounds at
**X=14.24-45.76, Y=8.45-42.17 mm**. Its XY placement, flex orientation and
reach at the raised height are unverified. J5's placement-file coordinate is
an FPC connector reference, not proof of the installed LCD active-area center.
The LCD is now carried by the upper cover and moves with it: disconnect or
manage its flex before removing the cover, and never lift by the cable.

Revision 19 uses a **captive printed slider** to operate **SW7** at case XY
**(63.0732,11.2)**. The lid has a **9 x 4.4 mm R1 running slot** and a
**17 x 8.2 mm R1 underside flange pocket**, ending at **Z27.8** and retaining
**1.2 mm of roof** beneath the Z29 exterior. The separate slider's **6 x 3.8 mm
R1 nub** reaches **nominal Z30**, with a softened upper edge. Its **14 x 7.6 mm R1
flange at Z26.8-27.6** overlaps the slot by at least **1 mm at the ends** and
**1.6 mm at the sides** over the nominal full travel. It covers the opening;
this overlapping arrangement has **no waterproof or IP-rating claim**.

A **4 x 4 mm lower boss**, with R0.3 corners and bottom at **Z25.4**, carries a
**2 x 2 mm socket** up to **Z27.9**. Its entry widens to **2.4 mm square** over
the lower 0.2 mm. The socket slips over the original **1.5 mm-square switch
handle**. The cap's nominal **3 mm total travel** allows 2 mm of switch travel,
0.5 mm of socket play and 0.5 mm reserve. The guide has **0.3 mm clearance per
side in Y** and **0.2 mm above and below the flange at its nominal position**.
R21 permits another 0.15 mm of downward movement as the strip settles onto
the keeper, giving **0.55 mm total cap float**; there is no intended preload.
The cap nub can therefore lie **0.65-1.20 mm above the lid** at these nominal
float limits.
The socket permits only 0.25 mm per side around the nominal handle, so it can
contact the handle laterally before the 0.3 mm guide gap is exhausted. The
retainer is intended to intercept downward loads; it does not isolate the
switch from every sideways load.

The button strip now includes an **18 x 9 mm R1 retaining shelf at
Z25.4-26.6**, with a **7.8 x 5.6 mm R0.3 guide opening** around the slider boss.
A **5 mm-wide bridge with R0.6 roots** joins the shelf to the strip's existing
anchor bar. The keeper now supports that bar on four lid ledges. Nominal
downward loads pass from the slider flange into this shelf, then through the
bar, keeper and ledges. The resin parts still need physical strength and
operating checks.

Nominal clearance from the shelf underside to the switch body is **0.6 mm**.
After the allowed 0.15 mm strip settling, the shelf-to-body clearance is
0.45 mm. At the slider's lowest permitted position, the boss remains **0.25 mm
above the switch body**, and the socket ceiling retains **0.25 mm above the
taller BOM handle**. The retaining shelf clears the nearby USB shell by approximately
**0.84 mm at the roof datum**, or **0.69 mm after full strip settling**.
These are CAD clearances, not measured installed fit.

The HRO **K3-1235S-F1** drawing specifies a **9 x 3.5 x 3.5 mm body** and
**2 mm slider travel along X**. It shows a **2 mm handle height**, while the
board BOM states **2.5 mm**. The provisional handle tops are Z26.8 and Z27.3,
respectively; actual solder height, socket engagement and operation need a
fit check. [HRO switch drawing](https://static.chipdip.ru/lib/844/DOC012844376.pdf)

Insert the cap from the inside of the separate lid, then install the button
strip and keeper to capture its flange. Check free motion before lowering the cover and
align the socket with the original switch handle. **Do not glue the moving
slider.** A clear gloss finish may be applied to exterior faces, but keep the
running slot, flange guide, retainer opening and handle socket free of coating
buildup and adhesive.

Three pressable top buttons are enabled independently of the LCD and
USB options. The official BOM identifies SW3, SW4 and SW5 as
**NH Technology NTC013-AA1J-A160T**. The switch datasheet gives **2.5 +/-0.2 mm
height**, **0.2 +/-0.1 mm travel** and **160 +/-50 gf operating force**.
Their case centers are **X=51.5**, at **Y=19 (SW5), 25 (SW4), and 31 (SW3)**.
With PCB top at Z=21.3, nominal switch tops are **Z=23.8**, leaving **3.2 mm**
to the cover's inside face at Z=27. The cover remains at **29 mm
body height**; the actuator stems bridge this gap, while their key tops now
reach Z=30 when the strip contacts its roof datums. With allowed settling,
they sit at Z29.85-30, or 0.85-1.0 mm above the case face.

A separate one-piece strip is held inside the cover by the keeper and also
retains the power slider. It has three independently
flexing **2 mm-wide, 0.8 mm-thick tongues**. Its mounting bar occupies
**X=60.5-65.5, Y=16.5-38, Z=25.8-26.6**, shortened to 21.5 mm along Y.
Each tongue runs from the bar at X=60.5 to its key center at X=51.5, with **0.3 mm root fillets** on both
sides. The **4.4 mm round key tops** pass through **5.2 mm cover holes**,
standing **0.85-1.0 mm above the case** and leaving **0.4 mm radial clearance** for
printing variation and tongue tilt. Each **1.8 mm-diameter stem** extends
from Z=25.8 down to **Z=24.1** at the upper datum, 1.7 mm long, with a nominal
**0.15-0.30 mm gap**
above its switch. Nominal inward travel to actuate is therefore **0.5 mm**:
0.3 mm to close the gap and 0.2 mm switch travel at the upper datum. With the
strip settled by 0.15 mm onto the keeper, the nominal rest gap is 0.15 mm and
travel to actuation is 0.35 mm. Actual key feel, return,
material fatigue/creep and tolerances need a fit prototype; the stems must
not preload the switches at rest. No hard stop or safe maximum overtravel is
claimed from the switch's actuation-stroke specification. Raising the key tops
does not change the stems or tongue shape; the R21 keeper changes the allowed
resting position as described above. The raised
keys provide nominal face-down spacing, but they flex under load; this is not
a verified screen-protection feature.

Two **solid 5 mm-diameter roof datums** remain at **(63,19)** and **(63,36)**,
from **Z26.6 to 28.99**. Their old pilots and the strip's matching holes are
filled. No button screws, heat-set inserts or bonded metal inserts are used.
The separate keeper is **8.5 x 20.5 x 1 mm** overall. Its central bar spans
**X60.7-65.3, Y17.5-38, Z24.65-25.65**, with four side tabs captured by the
lid ledges. The ledges occupy **Z23.65-24.65**, supporting the keeper directly.
The strip underside is Z25.8, leaving **0.15 mm total vertical freedom** above
the seated keeper. Do not add a second gap beneath the keeper.

With the lid off, insert the strip vertically and hold it against its roof
datums. Place the keeper vertically **3.3 mm toward +Y** from its final
position, then slide it **3.3 mm toward -Y** until it reaches the front stop.
The strip itself stays in place; its buttons cannot travel sideways with the
keeper. After checking all controls, apply a **small removable bead of
resin-compatible silicone across an accessible rear tab and fixed ledge**.
This bead is required to prevent reverse sliding; it must not enter button
arms, power-cap guides or the socket. Let it cure before use. To remove the
strip later, remove the bead, slide the keeper toward +Y and lift it out.

The bar has 0.2 mm nominal side registration clearance. Two additional lid
side guides locate the wider power-retaining shelf directly, with 0.2 mm gaps:
**X52.8732-53.8732** and **X72.2732-73.2732**, both **Y10-12, Z25.8-27.1**.
These limit strip translation and yaw; they are rigid datums, not snaps.
The keeper tabs have 1.6 mm left and 1.5 mm right nominal ledge overlap.

A nominal pose-grid check sampled approximately **1.23 million valid strip
positions**, with maximum sampled yaw about **1.20 degrees**. It found a
conservative cap-boss envelope of **3.7202 x 2.5309 mm half-widths** relative to
the moving retainer slot. The enlarged rounded slot retains approximately
**0.116 mm minimum conservative clearance**, a **1.7 mm shelf band**, and more
than **0.409 mm conservative flange overlap** at the limiting edge. This is a
sampled CAD clearance check, not a physical-fit result. The final checker
uses conservative +/-0.21 mm X/Y and +/-1.2-degree yaw limits around the valid
poses. The short insertion path
clears the modeled PCB components and USB envelope in the targeted source
review; actual installed heights, print fit and adhesive compatibility remain
unverified. The exported strip has stems and keys on opposite sides; print
orientation and supports require checking. Print the keeper flat.

**Board-support contact areas, antenna mounting/keepout, thermal clearance,
and other final access geometry remain unverified or unfinished.** Button
mechanism fit and durability require physical checks.

The user's photo identifies the selected hole on the lower projecting PCB tab
and shows the board marking MB_V2.3. It matches the outline-layer circle at
Gerber TOP XY **(53.4, 2.2)**, or display-facing case XY **(21.1, 8.7)**. The
layout circle is 2 mm in diameter; physical diameter and fastener fit remain
unmeasured. Other outline circles are retained as reference data only.

The PCB fixing at **(21.1,8.7)** now belongs entirely to the **midframe**.
Its **6 mm-diameter mount** rises from the plate at **Z=14.5 to 19.7** and
supports the PCB directly. A provisional **1.2 mm-diameter, 5 mm-deep blind
pilot** spans **Z=14.7-19.7**, leaving **1.7 mm of solid material** to the
flat plate underside at Z=13. An M1.6 screw is provisional. The old base core,
base shoulder and frame pass-through hole at this position are removed.
The board can be secured to the midframe before insertion into the case.

Two **M2 x 8 mm nylon screws** fasten the midframe to the base. The front-left post
at **(5,8.7)** is **5.5 mm diameter**; the rear post at **(44,48.5)** is
**6 mm diameter**. Both span **Z=2-13**, with matching **2.3 mm frame holes**.
Each post receives the user's **3 mm outside-diameter, 3.2 mm-long M2 insert**
from its top, nominally spanning **Z=9.8-13**. The trial printed pilot is
**2.7 mm diameter x 3.4 mm deep**, from **Z=9.6-13**, with a **3.1 mm entry,
0.25 mm deep**. Install these inserts before fitting the battery or electronics.
Their nominal battery clearances remain
**0.75 mm** and **3.5 mm**, respectively. Their positions leave the heads exposed
outside the PCB outline for fastening after the board/frame assembly is seated.
The nominal insert-to-post radial walls are 1.25 mm at the front-left and
1.5 mm at the rear; printed fit and strength require checking.

The screw heads bear on the plate at **Z=14.5**. With the provisional
**4 mm-diameter x 1.3 mm-tall head**, their tops reach **Z=15.8**. Each 8 mm
shaft reaches down to **Z=6.5**, passing 3.3 mm below the installed insert.
A **2.4 mm-diameter blind tip relief** runs down to **Z=6**, giving **0.5 mm
nominal tip clearance** and **4 mm of solid material above the base floor**,
or 6 mm to the exterior underside.
Pilot fit, head bearing, usable thread length, driver access and frame stiffness
still need physical checking. The PCB mount retains its separate M1.6 pilot;
these two M2 frame screws do not fasten the PCB directly.

The rear post's **1.6 mm-wide base rib at Z=2-6** overlaps the inner case wall
by **0.2 mm**. It is clipped to the lower interior expanded outward by 0.2 mm,
leaving **1.8 mm nominal wall to the exterior shoulder**. The rib belongs to
the base and stops inside the shell rather than ending coplanar with its outer
surface.

The four **case fasteners enter from the underside**. Their axes are
**(3.6,3.6), (76.4,3.6), (3.6,52.4) and (77,41)**. The original base-post
cores span **Z=2-14.5 mm**, with outside diameters of 5.2 mm at the first three
positions and 4.4 mm at the last. A **2.3 mm clearance shaft** runs to the joint:
these posts do not provide the case-screw threads. Each shaft opens into a
**4.5 mm-diameter, 10.2 mm-deep flat-bottom head well** on the exterior
underside. The head bearing surface moves inward by **8.4 mm**, from Z=1.8
to **Z=10.2**, leaving a **4.3 mm shaft-only column** to the joint at Z=14.5.
The illustrated 4.0 mm-diameter, 1.3 mm-tall head spans **Z=8.9-10.2**; its
exposed face is recessed 8.9 mm from the bottom. Head dimensions and the driver
needed to reach it remain unverified.

The first three wells gain **6.5 mm-diameter reinforcement from Z=2 to 11.5**,
clipped to the rounded case outline. The right well uses a **closed 6 mm-diameter
reinforcement centered at (77,41)** over the same height, with **0.75 mm
radial wall** around the 4.5 mm head-access bore. All four wells retain a
**1.3 mm reinforced roof** above the head seat and open only to the exterior
underside; there is no side opening into the battery bay. The battery remains
0.5 mm clear of the right well at its revised position. The well reinforcement ends below the midframe ledge; insert pocket and
screw length geometry are unchanged. Deep-well print quality, head bearing and
driver access need a physical check.

The upper cover has **blind M2 heat-set insert pockets opening downward**.
The insert reference is **3.0 mm outside diameter and 3.2 mm long**, as
provided by the user, seated nominally from **Z=14.5 to 17.7 mm**. The
initial trial pocket is **2.7 mm diameter and 3.4 mm deep**, from
**Z=14.5 to 17.9**, with a **3.1 mm-diameter entry chamfer, 0.25 mm deep**.
This pilot is **provisional**, not a manufacturer's recommendation: the
insert model/knurl shape and printed material must determine the final hole.
Install and test the inserts with the cover separate, before fitting the
LCD, button strip or other electronics.

All four upper pillars have **5.2 mm-diameter insert pads** at their lower
ends. At the moved right pillar, that diameter extends through
**Z=14.5-18.2**, then tapers to **4.4 mm at Z=19.2**, below the PCB underside
at Z=19.7. The narrow neck continues to Z=27, leaving **0.3 mm nominal XY
clearance to the PCB**. The other three pillars remain 5.2 mm in diameter
through to Z=27. This wider lower pad accommodates the insert while keeping
the populated board envelope clear above it; printed wall strength still
requires testing.

A **2.4 mm-diameter screw-tip relief** continues above each insert pocket to
**Z=22.5**, leaving **6.5 mm of solid material to the outer top face at
Z=29**. There is no case-screw opening through the top. The user's **M2 x 8 mm**
screw is modeled with 8 mm below its head: seated at **Z=10.2**, its tip reaches
**Z=18.2**, spanning the nominal 3.2 mm insert with **0.5 mm beyond its top**.
This leaves **4.3 mm nominal tip clearance** to the blind cavity end. The
usable thread length, tip chamfer, actual head and insert seating must still be
checked; nominal geometric overlap does not establish full usable thread
engagement. The nylon screw must not bottom in the cavity. Insert fit, head
bearing, thread engagement and case-fastener strength remain unverified. The two
midframe fixings use the same M2 x 8 mm screws and inserts. The independent
PCB mount retains its provisional M1.6 pilot; button-strip screws are removed
in R21.

Three **midframe-owned locating keys** rise **2.5 mm above the plate**, from
Z14.5 to Z17.0, and enter matching blind receivers inside the lid. Each is
8 mm long and 1.5 mm thick, at these case XY bounds:

| Key | X span, mm | Y span, mm |
|---|---|---|
| Left | 2.3-3.8 | 20-28 |
| Front | 40-48 | 2.3-3.8 |
| Rear | 24-32 | 52.2-53.7 |

The tips taper inward **0.3 mm per side over the upper 0.4 mm**. Each pocket
has **0.2 mm clearance per side**, **0.3 mm above the key**, and a lead-in that
widens by another **0.2 mm per side over its first 0.4 mm**. The lid's receiver
blocks span **Z14.5-17.8** and join its interior wall; the pockets open downward.
Their XY rectangles are **(1.5,18.8,3.5,10.4)**,
**(38.8,1.5,10.4,3.5)** and **(22.8,51,10.4,3.5)**, expressed as X, Y, width,
depth. The exterior shell and its existing wall thicknesses remain unchanged.

These three separated keys locate the lid laterally and resist twisting;
the four case screws provide closure. The cover still installs and lifts
straight vertically. The receivers stop 1.9 mm below the PCB underside and
remain outside its XY outline, with nominal gaps of 0.5 mm at the left and
1.5 mm at the front and rear. They avoid the USB saddle, wheel, cable channel
and frame screw heads. Check the printed sliding fit before installing the
display; do not use the case screws to pull a binding key into its pocket.

The upper-cover part's local Z=0 corresponds to case Z=14.5. With the cover
separate and electronics removed, install and check the four cover inserts and
the two base-post inserts. Insert the power slider from inside the lid, then
fit the strip vertically and install its keeper using the short drop-and-slide
path above. Check all controls and add the required removable silicone bead
across the rear keeper tab and ledge, then test-fit the LCD.
Adjust its seat depth to the
measured module thickness and add rear-frame retention if needed.

Attach the PCB to the midframe's blind mount first. Orient and place the battery
with its lead corner toward **(73.5,6)**. Before seating the assembly, feed the
lead into the **open front-right cable bay** and connect it to J25
while the assembly is raised and accessible, keeping slack clear of supports
and screws. Lower the PCB/midframe assembly onto the perimeter ledge while the
USB bodies enter the base saddle's open notches. Install the two exposed M2 x 8 mm
frame screws at the front-left and rear. Connect and route the LCD flex, then lower
the cover straight down over the three frame keys, aligning the power-slider
socket with SW7, and past the wheel to close
the USB openings. Confirm all three keys enter freely, the anti-lift stop does
not preload the PCB, and the seam closes without force. Insert the four M2 x 8 mm
nylon screws from the underside wells into the lid inserts. Verify engagement,
head fit, driver access and no bottoming before tightening. Check the power cap engages SW7 and allows full slider travel without binding. Check that each key rests clear of its switch and returns independently. Disconnect or manage the
LCD flex before lifting the cover away. Insertion paths, clearances, flex reach
and screw access require physical verification.

Three sparse lower seats bear at **Z19.7**, near case XY **(10,43.1)**,
**(61.5,7.0)** and **(69.9,33.75)**. The front seat remains **X60-63,
Y6.1-7.5**, with its stop at Y5.0-6.1. The left seat/guide remain unchanged.
Revision 20 removes the old **X71-74, Y42.1-42.9** seat because it conflicted
with SW2 RESET and its terminal pads. Its replacement spans **X69.3-70.5,
Y32.75-34.75, Z14.49-19.7**, overlapping the plate by 0.01 mm. It retains
**2.4 mm² nominal bearing area** in a **1.2 x 2 mm** section. This lower
contact is a solder-mask bearing land containing tented vias; it is not
unperforated bare FR4.

The placement, Gerber, mask and drill review gives these nominal clearances
around the relocated lower seat: **0.275 mm to the BOOT body**, **0.425 mm to
its mask pad** (conservative horizontal gap; **0.451 mm Euclidean**),
**0.35 mm to an encoder pad**, **0.29 mm to the USB locating
hole's mask opening**, and **0.63 mm radial clearance to the wheel**. The USB
locating hole is centered at **(69.72,35.39)**, with **0.6 mm nonplated drill**
and **0.7 mm mask opening**. These small sourced plan clearances still require
comparison with the actual populated board and printed support.

A separate **lid-owned anti-lift stop** is centered at **(72.5,42.3)**. Its
**3 x 1.2 mm R0.2 foot** spans **X71-74, Y41.7-42.9**, from **Z21.5 to
Z21.9**, leaving **0.2 mm nominal clearance** above PCB top Z21.3. The foot
widens up to a **4 x 2.4 mm R0.4 root at Z23.5**, which joins the lid through
Z27.1. The upper display-face contact is a **component-free solder-mask patch**.
The reviewed footprint has no drill centers, component bodies, mask openings
or paste openings in the placement, Gerber, mask and drill sources. The
tented vias noted above belong to the separate lower support land.

The upper stop limits upward board motion while the lower seats support it.
It is independent of the relocated lower seat and creates **no intended
preload or clamp force**. Print dimensions and PCB thickness can consume the
0.2 mm nominal gap. Fit the lid gently and confirm it closes without force
before tightening any case screws. Inspect the real board's bearing land,
components and solder mask; these geometry checks do not prove contact
pressure, tolerance fit or resistance to button/wheel loads.

A solid rectangular locator fills the former L-shaped jig at the lower-left
PCB step. It occupies **X=8-16.5, Y=8-11.7, Z=14.5-21.1 mm**, a nominal
**8.5 x 3.7 x 6.6 mm** block. Its two locating faces remain **0.4 mm clear**
of the PCB notch edges at X=16.9 and Y=12.1, and it remains **1.6 mm from the
midframe PCB standoff**. The added material sits within the empty outline
notch; component clearances and board/frame insertion paths still need physical
checks. Placement origins alone cannot establish free bearing areas.

To finish a first fit prototype, obtain:

1. Confirm actual board outline dimensions against the MB_V2.3 photo/layout match.
2. Battery model and confirmation that the supplied 36 x 65 x 10 mm dimensions
   include its protection PCB; verify lead exit, the reported 5 mm plug dimension,
   passage fit, wire bend radius and the connected route above the lead corner.
3. Populated thickness on both PCB faces, including header, wheel and LCD/flex;
   verify the new 5.2 mm underside allowance and 1.2 mm nominal wheel gap.
4. USB installed height and cable fit; switch, microSD, microphone and encoder
   opening locations/heights; and
   antenna dimensions/routing. Keep battery foil and metal fasteners away from
   the antenna's required clearance region.
5. Selected hole diameter, screw/head clearances, integral PCB mount, midframe
   vertical insertion and perimeter fit, usable jig/support contact areas,
   upper-cover removal, actual insert model and trial-pilot fit, nylon screw
   head dimensions and below-head length convention, deep-well driver access and
   recessed head bearing, LCD position, seat depth, rear-frame
   support contact, adhesive compatibility, flex reach and independent button
   travel without switch preload.

The four revision 16 STL exports passed the mesh checks recorded above.
They remain fit-prototype geometry, with physical assembly, print settings,
support placement and material strength not yet validated. A physical
fit check and operating/charging temperature check will follow the chosen
battery and completed retention/access geometry.

Camera J24 is a separate 24-pin FPC connection. Leave the module out and skip
camera initialization in the intended firmware. This task has not changed or
flashed firmware; behavior of the factory camera demo without a camera is not
verified.

## Sources and reproduction

- [Espressif P4X-EYE user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html)
- [Official reference design archive used](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32p4/_downloads/0857ce9b0e3668c3e56170a8a595db07/ESP32-P4X-EYE-EN.zip)
- [ESP32-C6-MINI-1U datasheet](https://documentation.espressif.com/esp32-c6-mini-1_mini-1u_datasheet_en.html)
- [Mitsumi SIQ-02FVS encoder mechanical drawing, page 3](https://nmbtc.com/wp-content/uploads/2019/04/switch_siq_02fvs_e.pdf)
- [HRO TYPE-C-31-M-12 manufacturer page](https://en.krhro.com/Product-Details/726.html)
- [HRO TYPE-C-31-M-12 manufacturer drawing, hosted by LCSC](https://datasheet.lcsc.com/datasheet/pdf/9e56b777c022540fcce7c7f67825f55e.pdf?productCode=C165948)
- [NH Technology NTC013 user-button switch datasheet](https://www.nh-technology.de/T-Mec/PDF/Series_TC/NTC013.pdf)
- [LCD datasheet linked by Espressif](https://dl.espressif.com/AE/esp-dev-kits/%E8%83%B6%E9%93%81%E4%B8%80%E4%BD%93ZJY154KC-IF17.pdf)
- [Dow electronics silicone adhesives guide](https://www.dow.com/documents/11/11-3921-01-advanced-silicone-adhesives-and-sealants.pdf?iframe=true)
- [DOWSIL 3145 electronics sealant example](https://www.dow.com/en-us/pdp.dowsil-3145-rtv-mil-a-46146-adhesive-sealant.01059548z.html)

Within the archive, geometry comes from `03_Gerber/OUTLINE.art` (inside the
nested Gerber ZIP), and feature positions from
`06_Placement/Placement_ESP32-P4-EYE-MB_V2.3_20250411.xls`, which is plain text.
Power facts come from the V2.4 schematic and BOM. Local downloaded working
references are under `work/p4x-enclosure/`; final project sources are here.
