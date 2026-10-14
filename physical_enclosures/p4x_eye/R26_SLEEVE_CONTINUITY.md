# Revision 26 - screw-well sleeve continuity

R26 corrects a regression that R25 introduced while replacing the stepped face
edges with a continuous chamfer. No other geometry changes. Use the matching
R26 base with the R26 lid, midframe, button strip, keeper and power slider;
those five are geometrically identical to their R25 files and are re-exported
only so the set carries one source hash.

## What was wrong in R25

`base_floor()` extruded the four lower screw sleeves only through the 1 mm face
bevel, and `screw_bosses()` starts them again at the 2 mm floor top. Between
Z1 and Z2 the sleeves did not exist, so the wall between each screw-head
counterbore and the outside dropped to the plain shell profile. PCBWay's first
review had marked exactly this feature (a 0.749 mm sleeve reading) and R22/R24
had fixed it; R25 undid the fix in a 1 mm tall band without anyone noticing.

Minimum material between the counterbore contour and the outer contour,
measured on the actual meshes with the project's section-contour method:

| Well | Z | R24 | R25 | R26 |
|---|---|---|---|---|
| Three corner wells | 1.5 | 1.006 | 0.762 | 1.006 |
| USB-end well (77.26, 41) | 1.5 | 1.006 | 0.487 | 1.006 |
| All four wells | 0.5, 2.5, 5, 10 | 1.006 | 1.006 | 1.006 |

The band also formed two new sub-millimetre steps at Z1 and Z2 around every
well, the same "step effect" PCBWay had asked to have removed elsewhere.

## Why the R25 checks passed

The 61 named section checks sampled the screw ligaments at Z5 and Z10 only.
The regression was found on 2026-10-12 by a whole-part ray-cast thickness scan
diffed against R24 (`tools/check_thickness_scan.py`, now part of the toolset).

## Change

One line in `base_floor()`: the sleeve outline is extruded through `floor_t`
instead of `face_edge_chamfer`, so the sleeves run continuously from Z0 to the
pad top at Z11.5. The bevel band itself is unchanged.

## Verification

- All six CGAL exports pass the strict mesh audit (one closed component,
  positive volume, consistent winding, no exact-degenerate or duplicate faces).
- All 36 source-based probes pass: 10 base/assembly fit, 16 assembly
  clearances at 0.25 mm, 10 control-motion checks.
- All 65 named production-mesh section checks pass. Four new Z1.5 screw
  ligament probes read 1.006 mm; they would have failed on R25.
- `validation/thickness-scan-r25-to-r26.json`: 0 regressions, 17 base thin
  cells resolved, no new or thinner cells in any part. Base area reading under
  0.8 mm falls from 18.90 mm2 to 7.06 mm2, matching R24.
- Lid, midframe, button strip, keeper and power slider have identical canonical
  triangle coordinates to R25; the keeper is byte-identical.

Records: `validation/base-fit-r26.json`, `assembly-clearance-r26.json`,
`control-motion-r26.json`, `wall-sections-r26.json`,
`thickness-scan-r25-to-r26.json`, `drawing-source-correspondence-r26.json`,
`pcbway-r26-delivery.json` and `stl/export-manifest-r26.json`, all against
source SHA-256 `137e3eaabe9ac5556293db0ccc01434ffde566227c85d4e718f45397d0c06afc`.

## Known nano-slivers

The Voronoi roof leaves a few dozen triangles with areas below 1e-6 mm2 where
the bevel meets the concave USB return (lid at Z28, base at Z1). They pass the
audit because it rejects only exactly-zero area, and R24 carried thousands of
smaller terrace faces that PCBWay accepted. They are listed in the thickness
scan record under `degenerate_faces` in case a supplier tool paints them.

## Supplier package

The two-page assembly PDF (`output/pdf/p4x-eye-r26-pcbway-assembly.pdf`) shows
the exploded six-part view with the part-to-order table, then the assembly
sequence, finish and handling notes. The STLs are uploaded individually to
their orders rather than zipped with source and validation files.

| Order | File |
|---|---|
| C-1K3W1162240A | `p4x-eye-base-r26.stl` |
| C-1K6W1162240A | `p4x-eye-lid-r26.stl` |
| C-1K5W1162240A | `p4x-eye-button-strip-r26.stl` |
| C-1K8W1162240A | `p4x-eye-power-slider-r26.stl` |
| C-1K7W1162240A | `p4x-eye-midframe-r26.stl` |
| C-1K4W1162240A | `p4x-eye-button-keeper-r26.stl` |

UTR-8100 transparent resin with clear varnish on all surfaces of every part,
no masking, parts shipped unassembled. The customer assembles.

## Toolchain

Exports and probes used OpenSCAD 2026.09.23 (WebAssembly, `--enable=roof`,
CGAL for exports and Manifold for probes) run as
`node /tmp/p4x-openscad-node/openscad.js`. A durable copy of that build is at
`~/.cache/p4x-openscad-node/openscad.js`; the node binary used was Codex's
bundled v24 under `~/.cache/codex-runtimes/`. The PDF generator
and thickness scan need NumPy (plus Matplotlib and ReportLab for the PDF),
which no system Python on the build machine provides; use a virtualenv.

## Limits

Geometry checks do not establish resin fatigue life, cured flatness, coating
thickness or physical assembly. No deformation or fracture waiver is implied by
this revision; those are answered in the order correspondence.
