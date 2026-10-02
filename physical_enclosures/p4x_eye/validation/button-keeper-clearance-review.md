# Button keeper: read-only mechanism review

Reviewed 2026-09-29 for the proposed revision 21 mechanism. Dimensions below
are nominal CAD dimensions, not a validated resin fit or fatigue assessment.

## Assembly and retention

The original button strip cannot slide bodily into conventional rails: three
4.4 mm button caps occupy 5.2 mm lid holes, allowing only 0.4 mm nominal lateral
movement. A separate keeper can be inserted vertically 3.3 mm toward positive
Y from its seated position and then slid toward negative Y under discontinuous
lid-owned rail shelves. The strip itself stays in position.

Keeper main bar implemented X60.7-65.3, Y17.5-38, Z24.65-25.65. Its insertion
position reaches Y41.3; at this X the rear lid cavity is approximately Y42.9,
so a short drop-and-slide path fits where a full-length rail-entry path would
hit the case wall. Positive-Y removal must remain accessible with the lid off.

Keeper tabs proposed X58.7-60.9 and X65.1-67.2, at Y21-23 and Y34.4-36.4.
Guide shelves are Y20.8-23.2 and Y34.2-36.6, with top Z24.65. The first left
guide ends at Y23.2 to clear the Y25 arm's R0.3 root fillet, which can extend
to Y23.7. The first tab moves under the Y25 arm during insertion, but stays
below its resting underside at Z25.8, with 0.15 mm nominal clearance when the
strip is held against its roof datums during assembly.

The first source review found the left shelf extending to X60.6, overlapping
the bar's X60.5 edge by 0.1 mm during vertical strip insertion; the right shelf
began at X65.5, giving zero entry clearance. Confirmed source correction: left shelf
ends at X60.3 and right shelf starts at X65.7. This gives 0.2 mm entry clearance
on each side while leaving 1.6 mm / 1.5 mm nominal keeper-tab bearing overlaps.
Re-read the corrected source before export and confirmed both shelf edges.

Fixed bar side datums span X59.4-60.3 and X65.7-66.6, Y27-29,
Z25.8-27.1. They clear the arm root fillets and permit the keeper to pass below.
Rear stop Y38.2-39.2 and front power-shelf stop Y5.5-6.5 begin at Z26.0.
They leave 0.2 mm nominal end clearances. The rear stop clears the moving
keeper top by 0.35 mm throughout its slide.

The power-retainer bridge extends to Y17.1 and down to Z25.4. Keeping the new
bar front at Y17.5 avoids the bridge; a keeper starting at Y17 would collide.

Shelves carry operating loads. A small accessible silicone bead bridging the
keeper and fixed rail is proposed to prevent reverse sliding. That is
adhesive anti-backout retention, not a positive mechanical end lock. Silicone
compatibility, adhesion and removal must be checked with the actual resins.

## Vertical freedom

The upper strip datum is Z26.6 and the 0.8 mm strip underside is Z25.8. A
keeper seated on shelves at Z24.65 and 1.0 mm thick supports at Z25.65. This
permits 0.15 mm total downward strip movement, reducing the original nominal
0.3 mm switch rest gap to 0.15 mm at the lowest position. This excludes all
printing, solder, PCB and switch-height tolerances. Do not add an independent
0.2 mm gap under the keeper; that would permit enough movement to preload
the switches. Root must keep this stack explicit in CAD and assembly notes.

The power-retainer shelf moves with the strip. With 0.15 mm strip movement plus
the existing 0.2 mm power-cap bottom clearance, the cap could descend 0.35 mm
from its centered nominal position. Its lower boss would then reach
Z25.05, leaving 0.25 mm above the nominal switch-body top at Z24.8. Installed SW7
dimensions and printed motion remain unverified.

## Primary-source component check

Placement source is plain text despite its .xls extension:
`reference/ESP32-P4X-EYE/06_Placement/Placement_ESP32-P4-EYE-MB_V2.3_20250411.xls`.
Mirrored (`m`) placements are the display-facing side, matching SW3-SW5 and
SW7. Map to case XY using (74.5 - X, 6.5 + Y).

Nominal component heights below come from:
`reference/ESP32-P4X-EYE/05_BOM/BOM_ESP32-P4X-EYE_20260205.xlsx`.
They are package/BOM values, not installed measurements; the BOM is newer than
the MB V2.3 placement file. PCB display face is modeled at Z21.3.

| Ref | Case XY | BOM part / height | Nominal top Z |
|---|---|---|---|
| D18 | 63.860,20.578 | DSS24 / 1.25 mm | 22.55 |
| D12 | 62.770,30.909 | DSS24 / 1.25 mm | 22.55 |
| U20 | 59.773,25.858 | AP5056 / 1.1 mm | 22.40 |
| Q6 | 67.218,25.334 | AO3401A / 1.1 mm | 22.40 |
| Q9 | 60.206,17.252 | AO3401A / 1.1 mm | 22.40 |
| D19 | 66.758,20.983 | LESD5D5.0CT1G / 0.7 mm | 22.00 |

The proposed rail bottom at Z23.65 is 1.10 mm above the highest listed nominal
top (D12/D18), before installed-height/printing allowances. USB body envelopes
start at X68.65, giving 0.35 mm plan clearance to right guide maximum X68.3.
The existing LCD collar ends at approximately X47.46 and is outside these
guides. This is a targeted check of neighboring features, not proof of every
component or swept-volume clearance.

Final targeted source review: no remaining nominal collision identified in
the vertical strip insertion or short keeper insertion path after the shelf
correction. The front power shelf clears the front stop by 0.2 mm, its bridge
clears the keeper front by 0.4 mm, and the arm root clears the first left shelf
by at least 0.5 mm in Y. Hold the strip against the roof datums while installing
the keeper. One-millimeter keeper/ledge thicknesses are prototype dimensions;
no physical strength or durability claim is made.

No main CAD edits were made by this reviewer. Root owns implementation,
CAD/export checks and updated documentation.

## Follow-up: moving strip and power-cap slot

The original 7 x 4.6 mm power-cap clearance slot assumed a fixed strip. New
strip freedom requires a wider slot and lateral registration near the shelf.
Proposed additional shelf datums: X52.8732-53.8732 and X72.2732-73.2732,
Y10-12, Z25.8-27.1. They leave 0.2 mm beside the shelf's flat sides, clear
its vertical insertion path and stay away from the rounded shelf corners.
The left datum is more than 6.4 mm from the LCD collar. The right datum ends
0.6 mm before the front USB aperture's Y12.6 edge and remains inside the
nominal USB wall by approximately 1.5 mm.

`check_keeper_slot_poses.py` scans strip X/Y offsets at 0.005 mm increments
and yaw at 0.0001 radians, checking collisions against seven registration
stops. It found 1,231,454 permissible sampled poses, with maximum yaw about
1.1975 degrees. Transforming the entire cap guide motion (+/-1.5 mm X and
+/-0.3 mm Y) plus a conservative square 4 x 4 mm cap boss into the moving
strip's coordinates requires maximum slot half-extents 3.7202 x 2.5309 mm.
That leaves only 0.0691 mm minimum Y margin for a 7.8 x 5.2 mm slot. A
7.8 x 5.6 mm slot provides approximately 0.2691 mm margin in the same sampled
check, before the separate rounded-corner check. This is a sampled nominal
geometry result, not a tolerance analysis or mathematical proof over all poses.

The wider slot leaves 1.7 mm shelf material on its front and rear sides.
The cap flange is 14 x 7.6 mm, so the 7.8 x 5.6 mm opening still has positive
bearing overlap over the reviewed guide/strip movements. A conservative bound
using 0.5 mm relative Y movement and approximately 0.021 radians yaw gives
over 0.4 mm overlap at the limiting long-side edge. The viewer/geometry owner
will check the actual rounded profiles independently.
