# Revision 23 — nominal assembly clearance

R23 increases reviewed free assembly gaps below 0.25 mm to a **0.25 mm nominal
gap per side**, while retaining the R22 USB end-tab and lower screw-sleeve
reinforcement. Use all six R23 parts together. This is a CAD clearance revision;
it is not confirmation that coated parts will assemble or that all supplier
manufacturing concerns are resolved.

## Matched files

The editable source is `enclosure.scad`, with a dimension snapshot in
`dimensions-r23.json`. The printable set is:

| Part | STL | Export orientation |
|---|---|---|
| Base | `stl/p4x-eye-base-r23.stl` | Floor down |
| Lid | `stl/p4x-eye-lid-r23.stl` | Exterior face down |
| Midframe | `stl/p4x-eye-midframe-r23.stl` | Flat underside down |
| Button strip | `stl/p4x-eye-button-strip-r23.stl` | Actuator stems down |
| Button keeper | `stl/p4x-eye-button-keeper-r23.stl` | Flat underside down |
| Power slider | `stl/p4x-eye-power-slider-r23.stl` | Nub face down, socket upward |

Import each file in millimeters at 100% scale. Export positions are for
printing, not assembly. `stl/export-manifest-r23.json` identifies the source and
mesh hashes. Earlier STL sets, dimensions and drawings are historical; the
R21 drawings show the old right screw position and older fits and must not be
used to manufacture R23. No new R23 PDF drawing is included.

## Clearance changes

All dimensions below are nominal CAD dimensions in millimeters. A per-side
gap of 0.25 means an opening is 0.50 larger than a centered rectangular part
in that dimension, or 0.50 larger in diameter for a circular fit. PCBWay has
not clarified whether its clearance advice means this per-side interpretation
or total dimensional difference. The model uses the former.

| Interface | R23 geometry and consequence |
|---|---|
| Midframe keys into lid | 0.25 per-side clearance; the existing 0.30 top gap remains. Receiver outer walls expand locally to preserve their webs. |
| Base USB rail into lid | 0.25 at each rail end and 0.25 at the horizontal split. The lid relief remains open through its lower edge for vertical assembly. |
| Midframe at USB wall | The straight USB-side gap is 0.25 up to Y38.10; the curved return and other perimeter/post-notch gaps remain 0.30. Local plate extent preserves 0.20 nominal support overlap on the narrowed shelf. |
| Battery edge at support ledge | Relief increases from 0.20 to 0.25; the battery position and 0.50 gap to the lower right sleeve remain unchanged. |
| PCB anti-lift stop | 0.25 above the nominal PCB top; it remains a travel stop, with intended lower PCB support contacts. |
| LCD pocket | 32.02 × 34.22 pocket for the 31.52 × 33.72 module gives 0.25 per side. R0.24 pocket corners retain clearance at square module corners, including facet margin. Seat Z27.10 and depth 1.90 are unchanged. |
| Free M2 passages | Shaft and tip bores increase to Ø2.51, allowing at least 0.25 radial clearance around nominal Ø2 shafts with 64-sided cutters. The Ø4.50 head-well reference is corrected by `1/cos(180/64)` in the cutter so its flats clear nominal Ø4 heads by 0.25. |
| Right upper screw post | Diameter increases from 4.40 to 4.54 to retain at least 1 mm around the enlarged free tip passage. The nearby cable scallop ends at Y38.54, preserving its separation from the post. The lower sleeve and screw axis remain at their R22 values. |
| Button strip and keeper guides | Reviewed free side gaps and keeper vertical float become 0.25. Keeper tabs and lid guides change together; the keeper still slides 3.30 toward −Y onto its ledges. |
| Button rest position | Stem-to-switch gap is 0.50 with the strip against its upper datum and 0.25 after it settles onto the keeper. Adding nominal 0.20 switch travel gives 0.45–0.70 press-to-actuate travel. This is a geometric estimate, not a force/return test. |
| Power cap | Flange has 0.25 above and below at its nominal position. Keeper settling permits 0.50 total downward movement and 0.25 upward movement. The retainer opening becomes 8.10 × 5.80; the socket is raised to preserve 0.25 to the provisional switch body/taller handle at maximum downward travel. |

The button holes remain Ø5.20 around Ø4.40 keys (0.40 nominal radial clearance).
The slider retains its 9 × 4.40 lid slot, 6 × 3.80 nub, 3 mm cap travel and
2 mm switch travel. Its 2 mm square socket retains 0.25 per side around the
nominal 1.50 mm handle. The switch drawing's 2 mm handle height and BOM's
2.50 mm height still disagree; the taller envelope is used for the stated
clearance, but the installed switch has not been measured.

## Preserved dimensions and intended contacts

The main shell remains 80 × 56 × 29, with a 2 mm base floor. The lower sleeves
remain Ø6.52 with the right axis at (77.26, 41), extending the base locally to
X80.52. The battery remains 65 × 36 × 10 at XY(8.50, 6), Z2.50, with 0.50 to
the lower right sleeve. The rear USB end tab remains 1.10 wide. USB apertures
remain 9.80 × 4.20; the user-measured plug envelope is still 10.25 × 6, with
the plastic body assumed to start at or beyond X76.

Seating/bearing faces deliberately touch: the closed case seam, frame ledges,
PCB supports, LCD support, screw-head seats and keeper ledges are not free-fit
gaps. A control also contacts its actuator or travel stop during operation.
These interfaces are excluded from the 0.25 free-clearance target.

The actual PCB's nominal Ø2 mounting hole around the provisional M1.6 screw
has only 0.20 radial clearance; changing printed parts cannot enlarge that
PCB hole. The Ø1.20 self-tapping pilot and Ø2.70 insert pilots are intended
attachment fits, not clearance holes. Insert choice, installation method and
resin compatibility remain to be confirmed physically. The nominal Ø3 × 3.20
M2 inserts and M2 × 8 case/frame screws are unchanged.

## Finish, assembly and remaining limits

Following the user/Cici discussion, **full varnish is now the preferred
finish**, superseding the historical R21 masking/exterior-only coating advice.
There is no confirmed numerical varnish thickness allowance or approval of
the finished moving assembly. Coating on both faces, printing variation and
warpage can consume a nominal gap; this revision does not guarantee finished
fit or free motion.

The existing control assembly sequence remains: insert the power cap from
inside the lid, fit the button strip, lower the keeper at +3.30 Y, then slide
it −3.30 Y onto the ledges. The keeper still requires the specified removable,
resin-compatible silicone anti-backout bead across the rear tab/ledge seam;
it is not a snap lock. Check compatibility with the actual varnished finish
and keep the bead out of the moving interfaces. Do not force the lid closed
with the screws or preload the PCB to overcome binding.

R23 is not a general wall-thickness repair. The 0.60 wheel hardware band,
0.80 button arms and slider flange, approximately 0.90 keeper rail root and
0.50 receiver roof remain among the existing thin features. Spring force,
fatigue, shipping deformation and finished-part durability are unresolved.
Nominal component references and installed Z heights remain provisional.
R21 physical-fit experience does not verify these revised or coated parts.

## Validation status

All six final CGAL meshes pass the strict topology audit: one closed manifold
component, consistent winding, positive volume, and no degenerate/duplicate
faces or invalid edges/vertices. No geometry repairs were applied.

All **34 named actual-source probes passed**: 16 clearance gauges, eight
control assembly/motion checks and ten base/assembly checks. The latter include
37 lid insertion positions and a continuous conservative rear USB-tab envelope.
The 0.25 mm clearance gauges use a 0.0001 mm numerical allowance; control and
fit probes permit only the recorded numerical volume tolerance at intentional
contacts. Actual exported-base sections retain more than 1.006 mm sampled
radial sleeve wall and a 1.099998 mm rear USB tab. These sampled measurements
complement the source's conservative wall bounds.

Records are in `validation/assembly-clearance-r23.json`,
`validation/control-motion-r23.json`, `validation/base-fit-r23.json`,
`validation/base-sections-r23.json` and `stl/export-manifest-r23.json`.
`validation/export-source-correspondence-r23.json` records exact expanded-CSG
equality where a part was exported before an unrelated source refinement.
All final geometry corresponds to the source hash in the manifest.

Named probes and sampled poses do not establish a universal minimum clearance
or replace a physical trial. R23 physical fit, flexible-button return/durability
and supplier finished-assembly approval remain unverified.
