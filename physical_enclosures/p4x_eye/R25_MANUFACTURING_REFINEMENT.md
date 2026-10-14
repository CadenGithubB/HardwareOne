> **Superseded by R26.** R25 cut the lower screw sleeves back to 0.49-0.76 mm
> between Z1 and Z2 (see [R26_SLEEVE_CONTINUITY.md](R26_SLEEVE_CONTINUITY.md)).
> Do not send R25 files to production.

# Revision 25 - supplier step and assembly review

R25 responds to Cici's 2026-10-10 marked pictures. The CAD changes are made
locally; PCBWay is asked to review and manufacture the supplied files, not to
edit their geometry. Use the matching R25 base, lid, button strip and power
slider. The midframe and keeper are included in the matched six-part set.

## Changes

| Feature | R24 | R25 |
|---|---|---|
| Base and lid outer face edges | R2 approximation made from 64 stacked slices, creating sub-0.05 mm terraces | Continuous 1 x 1 mm, 45-degree chamfer; no stacked terraces |
| Upper button-bar/support step | 0.10 mm | 0.50 mm |
| Lower button-bar/support step | 0.30 mm | 0.70 mm |
| Slider support Z | 25.30-26.50 | 24.90-26.10; 1.20 mm thickness retained |
| Support bridge roots | R0.60 | R1.00 |
| Bridge overlap into bar | 0.60 mm | 0.75 mm; ends at Y17.25, before the keeper at Y17.50 |
| Support width | 18.00 mm | 20.40 mm |
| Switch-body opening in support | 8.10 x 5.80 | 10.50 x 5.80 |
| Slider flange | 14.00 x 7.60 x 1.00 | 16.40 x 7.60 x 1.40 |
| Slider flange Z | 26.75-27.75 | 26.35-27.75 |
| Lid flange guide | 17.00 x 8.20 | 19.40 x 8.20 |

The broader support moves its lid side guides outward. The larger opening
clears the switch body when the support settles; the wider flange preserves
capture on both sides of that opening. These are coordinated changes, not an
isolated deepening of the two steps. The upper and lower step dimensions are
measured between the flat support and bar surfaces, on opposite sides of the
overlapping bridge.

The continuous chamfer uses OpenSCAD's native Voronoi roof, clipped to the face
band. This avoids intersecting per-edge cutters and zero-volume sheets at the
concave USB return. Enable the experimental `roof` feature when opening the
source; the export and probe tools pass `--enable=roof` automatically. This is a
source CAD construction change, not post-export mesh repair. Local screw-access rims
remain full thickness. The 2 mm floor, 2 mm main lid skin, reinforced screw
sleeves and USB tab remain intact.

## Retained fit

- Button arms remain 1.00 mm thick x 2.00 mm wide with R0.50 roots. Key tops,
  actuator stem bottoms, keeper position and nominal button travel remain fixed.
- Slider clearance remains 0.25 mm above and below its flange. Total downward
  float remains 0.50 mm including keeper settlement; upward float remains 0.25.
- Slider nub, square socket and lid slot retain their prior dimensions; nominal
  cap travel remains 3.00 mm and switch travel remains 2.00 mm.
- Battery compartment height remains 11.00 mm, from the inside floor at Z2.00
  to the midframe underside at Z13.00: 0.50 mm pad allowance, 10.00 mm nominal
  pack, and 0.50 mm top gap. Pack wrapping, leads and actual dimensions need
  physical inspection. The 0.50 mm lateral battery-to-sleeve gap remains.
- Reviewed free interfaces retain the nominal 0.25 mm per-side target;
  intentional seats, bearing faces and attachment pilots remain exceptions.

## Supplier package and finish

The assembly PDF is generated from the released STL meshes and is deliberately
short: an exploded six-part view with the part-to-order table, then the
assembly sequence, finish and handling notes. The earlier change-log page with
mesh sections was dropped on 2026-10-12 after PCBWay asked for a streamlined
package; the section measurements remain in `validation/wall-sections-r25.json`.
Exported STLs are oriented and translated for printing; their import positions
are not assembly positions.

UTR-8100 transparent resin and full clear varnish on all internal and external
surfaces remain requested. No selective masking and no separate numeric
varnish buildup allowance are specified. Finish the individual parts before
assembly. The supplier still needs to review coating and process feasibility.

| Order | Replacement file |
|---|---|
| C-1K3W1162240A | `p4x-eye-base-r25.stl` |
| C-1K6W1162240A | `p4x-eye-lid-r25.stl` |
| C-1K5W1162240A | `p4x-eye-button-strip-r25.stl` |
| C-1K8W1162240A | `p4x-eye-power-slider-r25.stl` |

Confirm the mapping for C-1K7W1162240A, midframe and keeper. Supplying matching
files does not authorize additional paid quantities.

## Verification and limits

All six final CGAL exports pass strict mesh audits: one closed manifold
component, positive volume, consistent winding, and no invalid or duplicate
faces. All 36 source-based checks pass (16 assembly clearances, 10 control
checks, 10 base/assembly checks). All 61 named production-mesh checks pass,
including the 0.50/0.70 mm steps, continuous 1 mm bevels, zero intermediate
base terraces, and the retained wall sections. Reports and manifest match
source SHA-256 `7dc9cdda1a60c5adeab21eb7505e5dc27ed81c2e885276d88f73b7661f123d5a`.
Preliminary exports are excluded from release. The two unchanged parts have
identical canonical triangle coordinates to R24; the keeper is byte-identical.

The checks cover geometry and selected poses; they do not establish resin
fatigue life, cured-part flatness, coating thickness or successful physical
assembly. The nominal electronics envelopes remain provisional. Ask PCBWay
to review print orientation, support removal, curing, finished fit and packing,
and identify any remaining concern. No blanket deformation or fracture waiver
is accepted by this revision.
