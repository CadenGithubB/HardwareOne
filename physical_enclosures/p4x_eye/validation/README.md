# Enclosure validation records

## Current R26 records

All six CGAL meshes pass strict audits. All 36 source-based checks and 65
named production-mesh checks pass against the same source hash, and the
whole-part thickness scan against R25 reports no regressions.

- `assembly-clearance-r26.json`: 16 nominal 0.25 mm clearance probes.
- `control-motion-r26.json`: 10 insertion/motion/support checks.
- `base-fit-r26.json`: 10 battery/hardware/assembly checks.
- `wall-sections-r26.json`: 65 sampled wall, step and bevel checks, now
  including Z1.5 screw-ligament probes on all four wells (1.006 mm).
- `thickness-scan-r25-to-r26.json`: inward-ray thickness at every face of
  every part, binned and diffed against R25; 0 regressions, 17 resolved base
  cells, five parts geometrically identical.
- `../stl/export-manifest-r26.json`: all six audited file hashes, source hash,
  print transforms and R25 correspondence per part.
- `drawing-source-correspondence-r26.json`: two-page drawing provenance and
  visual review.
- `pcbway-r26-delivery.json`: the individually uploaded file set and drawing.

Physical fit, coating tolerance, fatigue and transport resistance remain
unqualified.

## Historical R25 records

R25 is superseded: its base lost the screw-sleeve material between Z1 and Z2
(0.49-0.76 mm rims), which its Z5/Z10 ligament samples did not cover. The
records below are otherwise accurate for the R25 files.

- `assembly-clearance-r25.json`: 16 nominal 0.25 mm clearance probes.
- `control-motion-r25.json`: 10 insertion/motion/support checks, including
  the lowered shelf against the switch and two 1 mm flange-bearing patches
  over conservative translation, yaw and travel poses.
- `base-fit-r25.json`: 10 battery/hardware/assembly checks.
- `wall-sections-r25.json`: 61 sampled wall, step and bevel checks.
- `../stl/export-manifest-r25.json`: all six audited file hashes, source hash,
  print transforms and canonical-geometry correspondence for unchanged parts.
- `drawing-source-correspondence-r25.json`: mesh-derived drawing provenance.
- `package-validation-r25.json`: archive/member hashes and CRC check.

Physical fit, coating tolerance, fatigue and transport resistance remain
unqualified. Failed trial exports are not included in the release.

## Historical R24 records

Use all six R24 parts. All six final CGAL meshes pass strict topology audits;
all 34 source-based geometry checks and 54 named actual-STL section checks
pass. Source/file hashes identify the exact verified files.

- `assembly-clearance-r24.json`: 16 named nominal 0.25 mm free-fit gauges.
- `control-motion-r24.json`: eight control insertion/slide/motion checks.
- `base-fit-r24.json`: ten base/hardware/assembly checks, including 37 lid
  positions and a continuous conservative USB-tab insertion envelope.
- `wall-sections-r24.json`: 54 sampled wall/ligament/edge-removal checks.
- `../stl/export-manifest-r24.json`: all six production STL audits and hashes.
- `export-source-correspondence-r24.json`: exact expanded-CSG correspondence
  for the lid export across the unrelated battery-guide coordinate correction.
- `drawing-source-correspondence-r24.json`: unchanged production projections
  and the visually inspected full-varnish PDF hash.
- `package-validation-r24.json`: final archive/member hashes and CRC check.

No coating, strength, fatigue, shipping or physical fit qualification is
claimed. The failed preliminary base export was excluded; only the corrected,
validated base appears in the final set.

## Historical R23 records

R23 changes reviewed assembly clearances and requires all six R23 parts as a
matched set. See `../R23_ASSEMBLY_CLEARANCE.md` for scope and exceptions.

The records for this revision are:

- `assembly-clearance-r23.json`: actual-source clearance gauges for the named
  interfaces, with numerical tolerance and probe limitations recorded.
- `control-motion-r23.json`: sampled control insertion, keeper sliding and
  power-cap motion against the actual source modules.
- `base-fit-r23.json`: battery, PCB, hardware and assembled-part collision
  checks, sampled lid insertion and the continuous USB-tab envelope check.
- `../stl/export-manifest-r23.json`: source/file hashes, export orientations
  and audit results for the six final printable meshes.

All six final R23 meshes pass strict topology checks, and all 34 named geometry
probes pass (16 clearance, eight control motion, ten base fit). The final source
hash matches the three probe reports. `base-sections-r23.json` measures sections
of the final base mesh. `export-source-correspondence-r23.json` records exact
expanded-CSG equivalence for exports completed across unrelated source edits.
`package-validation-r23.json` records the final archive and member hashes.
These checks do not verify print tolerances, varnish buildup, material strength,
physical assembly or supplier approval.

## Historical R22 records

`base-reinforcement-r22.json` records R22 source fit checks, mesh topology and
measurements of the reinforced base sections. `package-validation-r22.json`
records its packaged files. Their matching hashes are in
`../stl/export-manifest-r22.json`; these records do not validate R23.

## Historical R21 records

These static release records describe the final R21 file set:

- `package-validation-r21.json` records the printable-package hash and mesh
  checks.
- `pcbway-review-package-validation-r21.json` records the supplier-review
  package hash and its included files.
- `button-keeper-clearance-review.md` records the review of the R21 button
  keeper and its lid ledges.

The package paths and results reflect the layout and source at their check
dates. The builder subsequently verified R21 physical assembly fit; that
verification does not transfer to revised R23 parts or their finished coating.
