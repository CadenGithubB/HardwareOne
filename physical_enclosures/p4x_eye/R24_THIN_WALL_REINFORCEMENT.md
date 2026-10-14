# Revision 24 — thin-wall reinforcement

R24 reinforces the reviewed thin features while retaining the R23 nominal
assembly-clearance target. It thickens the button arms, slider flange and
socket walls, keeper rails, alignment receivers and wheel-side shell. It also
removes an acute rear-USB shell remnant. Use **all six R24 parts together**.

The requested finish is **full varnish on all six parts**. That request
supersedes the historical R21 selective-masking guidance. It does not constitute
blanket acceptance of thin-wall breakage, deformation or finished-fit risk.
Revised geometry reduces identified weaknesses; zero warping and successful
coated assembly are not established.

## Files and revision identity

`enclosure.scad` is the editable source, and `dimensions-r24.json` records the
nominal dimensions. The matched printable set is:

- `stl/p4x-eye-base-r24.stl`
- `stl/p4x-eye-lid-r24.stl`
- `stl/p4x-eye-midframe-r24.stl`
- `stl/p4x-eye-button-strip-r24.stl`
- `stl/p4x-eye-button-keeper-r24.stl`
- `stl/p4x-eye-power-slider-r24.stl`

Use millimeters and 100% scale. The export manifest identifies the source,
file hashes and supplied orientations; exported coordinates are for printing,
not assembly. R21–R23 files and records remain historical. Their drawings and
validation results do not validate the revised R24 parts.

## Geometry changes from R23

All dimensions below are nominal millimeters, in assembled case coordinates
where positions are stated.

| Feature | R24 change and preserved fit |
|---|---|
| Wheel hardware band | Material grows outward from the unchanged cavity to a 1.10 wall target. The reinforced profile applies from Y40.80 rearward over Z17.20–22.10, covering the Z17.70–21.60 hardware band plus 0.50 shoulders above and below. The addition overlaps the existing wall by 0.05. PCB/encoder cavity positions, case seam and USB openings retain their original locations. |
| Lid alignment receivers | Overall height increases from 3.30 to 3.90, leaving 1.10 above the 2.80-deep blind pocket. Receiver blocks expand inward and at their ends, giving 1.10 exposed lips at the flared opening. Key side clearance remains 0.25 and top clearance remains 0.30. |
| Alignment-key tips | Chamfer decreases from 0.30 to 0.20 per edge, leaving a 1.10-wide tip on the 1.50-wide key. Key body width and height are unchanged. |
| Battery locator ring | Exterior grows by 0.10, increasing nominal ring thickness from 1.00 to 1.10. The battery-facing boundary, lead exit and post clearances retain their locations. |
| Button arms | Thickness increases from 0.80 to 1.00; the user-selected 2.00 width remains. Root radius increases from 0.30 to 0.50. The upper arm datum, key tops and actuator-stem bottoms remain fixed. |
| Keeper and lid rails | Keeper and supporting ledges move down 0.20 to follow the thicker strip, preserving 0.25 vertical float. The previously 0.90-wide rail root and strip guides become 1.00 wide, growing away from the fitted parts. Keeper sliding direction, travel and anti-backout method remain unchanged. |
| Power-slider flange and capture | Flange increases from 0.80 to 1.00 thick at Z26.75–27.75. The shelf moves to Z25.30–26.50 and guide roof to Z28.00, preserving 0.25 nominal clearance above and below the flange. The lid skin over the guide remains 1.00 thick. |
| Power nub and socket | Nub width across Y increases from 3.80 to 4.00, leaving 1.00 beside the 2.00 square socket. Lid slot grows correspondingly from 4.40 to 4.60 across Y, retaining 0.30 per side. The socket entrance chamfer is removed so the open-end walls remain 1.00 rather than 0.80. The straight socket still has 0.25 per side around the nominal 1.50 handle. |
| Rear-USB feather | `usb_rear_feather_trim()` removes the acute shell remnant in X74.00–75.80, Y38.70–39.60, through the side wall below the lid roof. The cutter excludes the complete insert post, preserving its load-bearing annulus. This opens local clearance beside the base tab rather than adding an inward obstruction. |

The retained `wheel_hardware_wall=0.60` source parameter describes the old
cavity datum. R24 adds the wall outward; this parameter no longer describes
the completed reinforced wall thickness.

For an otherwise identical rectangular cantilever, bending stiffness scales
with thickness cubed. Increasing 0.80 to 1.00 gives an idealized ratio of
`(1.00/0.80)^3 = 1.953125`, or approximately **1.95 times the stiffness**.
The larger root fillet, printed resin, coating and actual effective arm length
also affect feel. This ratio is not a measured button-force or durability
result; firmer buttons were the selected tradeoff.

## Retained clearances and dimensions

The nominal free-fit target remains **0.25 between opposing faces, per side**:
a centered rectangular opening is 0.50 larger overall in that dimension.
PCBWay has not clarified whether its clearance advice uses that per-side
interpretation or a total dimensional difference. This is CAD clearance,
not a confirmed allowance for coating buildup on both mating surfaces.

The button rest gap remains 0.25 with the strip settled and 0.50 against its
upper datum. Adding nominal 0.20 switch travel gives 0.45–0.70 nominal
press-to-actuate travel. Power-cap float remains 0.50 downward and 0.25 upward;
its body and taller provisional handle envelope retain the stated 0.25
minimum at maximum downward movement. The switch's 2.00-versus-2.50 handle
height discrepancy remains unmeasured on the actual board.

The main shell datum remains 80 × 56 × 29; the outward wheel band changes its
local exterior profile. The base retains a 2.00 floor, Ø6.52 lower sleeves,
right screw axis (77.26, 41), local maximum X80.52 and 0.50 battery-to-sleeve
gap. The battery remains 65 × 36 × 10 at XY(8.50, 6), Z2.50. The rear base
USB tab remains 1.10 wide. LCD pocket, PCB supports, screw locations, free M2
bores and insertion-key clearance remain at R23 values.

Intentional bearing/seating contacts and attachment pilots are exceptions to
the free-fit target. The actual PCB's nominal Ø2 hole around the provisional
M1.6 screw still has 0.20 radial clearance; printed CAD cannot enlarge that
PCB hole. Insert installation, self-tapping pilots and actual component
heights remain provisional. No universal minimum across every hardware
surface or every possible control pose is claimed.

## Deformation, finishing and supplier review

PCBWay's published SLA/DLP advice recommends at least 1 mm walls and supports
for overhanging structures. These revisions address the identified thin
features using that minimum with local margin where space permits.
([PCBWay process-design guidance](https://www.pcbway.com/blog/3D_Printing/10_Ways_to_Reduce_3D_Printing_Costs_Through_Smarter_Design_Optimization_09282537.html))

Its separate quote-form deformation warning recommends walls of at least
2 mm and more heat/strength-resistant resins for susceptible structures; it
also identifies transportation loads as a deformation concern. R24 does not
make every wall 2 mm and cannot be represented as eliminating that warning.
([PCBWay manufacturing quotation guidance](https://www.pcbway.com/rapid-prototyping/manufacture/?projectattid=79587&reffercode=Project&type=2))

PCBWay lists UTR-8100 with spray varnish and describes its dimensional
stability, but that material description is not approval of these thin,
flexing, mating parts. ([UTR-8100 material page](https://www.pcbway.com/rapid-prototyping//3d-printing/plastic/resin/UTR-8100/))

The revised set still needs PCBWay review of print orientation, supports,
support removal, curing, full-varnish buildup and protective packing. Ask for
any remaining vulnerable locations to be identified on the revised geometry
and for the finished mating/moving interfaces to be reviewed. No numerical
varnish thickness allowance, cured-part flatness guarantee or finished-assembly
approval has been established. These are pending process questions, not an
instruction to accept the risks or change the ordered material or quantities.

Assembly still places the cap inside the lid, followed by the strip and
keeper; the keeper drops at +3.30 Y and slides −3.30 Y onto its ledges. The
specified removable, resin-compatible silicone anti-backout bead remains
required. Check its compatibility with the actual varnish and keep it out of
moving interfaces. Physical assembly, repeated button return and resin
durability remain unverified. Do not use the screws to pull a binding lid
into position.

## Validation status

All six final CGAL meshes pass the strict audit: one closed manifold component,
positive volume, consistent winding and no degenerate/duplicate triangles or
invalid edges/vertices. The base was regenerated after a coordinate-only CAD
correction removed two zero-area export triangles; no mesh repair was applied.

All **34 actual-source fit/clearance/motion checks pass**, including 16 nominal
0.25 mm clearance gauges, eight control checks and ten base/assembly checks.
All three reports match the final source hash. The 0.25 mm gauges use a
0.0001 mm numerical margin. Collision probes allow only the recorded
1e-6 mm3 numerical volume tolerance at intended contacts.

All **54 named actual-STL section checks pass**, covering the reinforced
features, screw ligaments, original perimeter bands and removed USB feather.
Measured examples: wheel band and receiver roof 1.100000 mm, guide/arm/flange/
socket walls 1.000000 mm, and rear USB tab 1.099998 mm. Section comparisons
allow 0.00001 mm for mesh precision. These are selected sections, not a
whole-model minimum thickness or material-strength certification. Battery
locator terminations still taper into floor-backed edges near the post cuts.

Records are in `validation/assembly-clearance-r24.json`,
`validation/control-motion-r24.json`, `validation/base-fit-r24.json`,
`validation/wall-sections-r24.json` and `stl/export-manifest-r24.json`.
`validation/export-source-correspondence-r24.json` proves exact expanded-CSG
identity for the lid exported before the unrelated base-coordinate correction.
Five parts were exported directly from the final source. The finishing PDF at
`output/pdf/p4x-eye-r24-pcbway-finish.pdf` has two visually inspected pages;
its production projections were re-exported and verified byte-identical after
the correction, as recorded in `validation/drawing-source-correspondence-r24.json`.

The supplier's shaded screenshots do not by themselves establish the cause
of its damaged-file report. The new audited files are supplied for re-review;
supplier confirmation of those marked locations remains pending.

Geometric checks do not establish coating tolerance, fatigue performance,
shipping resistance or zero warping. Physical fit and PCBWay approval of the
finished R24 assembly remain pending.
