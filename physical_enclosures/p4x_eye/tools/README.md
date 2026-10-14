# Enclosure maintenance checks

The current R26 tools read `../enclosure.scad`; they do not maintain a second
copy of the production model. Save revisioned results under `../validation/`
and match their source hash to the export manifest.

- `check_wall_sections.py --manifest ../stl/export-manifest-r26.json --output result.json`
  measures named sections of the actual STL meshes, reversing their print
  transforms and verifying file hashes. R26 adds Z1.5 screw-ligament probes.
  These samples do not prove a global minimum wall thickness.
- `check_thickness_scan.py --old-manifest ../stl/export-manifest-r25.json
  --new-manifest ../stl/export-manifest-r26.json --output result.json` casts an
  inward ray from every face of every part, bins sub-1 mm readings into 2 mm
  cells and diffs the two revisions. Run it for every revision: it found the
  R25 sleeve regression the named sections missed. Requires NumPy. Acute-edge
  readings need inspection; it is a comparison aid, not a certification.
- `check_stl.py FILE.stl [FILE.stl ...]` audits actual printable meshes for
  one closed manifold component, consistent winding, positive volume and
  invalid faces/edges/vertices. Python standard library only; no mesh repair.
- `export_stl.py --directory /tmp/p4x-r26-export --revision 26
  --openscad-command openscad` exports all six parts with CGAL, hard warnings
  and binary STL output, then audits each final file and writes a manifest.
  It applies only a rigid translation to put minimum XYZ at zero and refuses
  to overwrite existing STL destinations. The manifest records orientations.
- `check_assembly_clearance.py --output result.json --openscad-command openscad`
  uses actual-source CSG gauges for the named nominal free-fit interfaces.
  The default target is 0.25 mm, with a 0.0001 mm numerical gauge margin.
  `--only NAME ...` can rerun particular probes. It excludes intended contact
  and attachment fits and does not establish a global minimum for every pose.
- `check_control_motion.py --output result.json --openscad-command openscad`
  checks sampled strip/keeper insertion, keeper sliding, cap-guide extremes
  and provisional switch clearance using the actual source modules. It does
  not simulate flexible arms, coating, friction or material durability.
- `check_base_reinforcement.py --output result.json --openscad-command openscad`
  runs actual-source CSG checks for nominal battery, PCB, hardware, USB and
  assembled-part fit, 37 lid insertion positions and a continuous conservative
  rear-tab insertion envelope. Despite its historical name, it reads the
  current source and can validate the named base-fit probes for R26.

R25 and later use OpenSCAD's experimental `roof` feature for the continuous face bevel.
The export and probe tools pass `--enable=roof`; enable the same feature in
the GUI when opening the source. The verified runtime is the 2026.09.23
development build. The CSG probe tools also require Manifold support. A portable Node
build can be supplied as `--openscad-command /path/to/node /path/to/openscad.js`.
Place this multi-value option last. Printable exports use **CGAL**, while
Manifold is used for collision probes; raw Manifold STL exports previously
failed the strict audit for this model. A tool description is not evidence
that its current checks passed: inspect the revisioned result records and
manifest in `../validation/` and `../stl/`.

The following older checks reproduce R21 reference geometry and do not
validate the current source or meshes:

- `unified-geometry-check.js` checks sampled enclosure geometry, control
  travel, fasteners, wheel clearance, USB-cable clearance and the R21 keeper.
  Run it with Node.js from this directory.
- `check_keeper_slot_poses.py` checks the R21 keeper aperture over a grid of
  strip positions and yaw angles. It requires Python 3 and NumPy.

`clipper-6.4.2.js` and its accompanying license files are the local dependency
used by the historical JavaScript check. Temporary export logs and intermediate
meshes belong outside the release directory; retain final revisioned source,
mesh and validation records together.

`check_control_motion.py --only NAME ...` runs selected probes. R25 adds
switch-body clearance and minimum flange-bearing checks over conservative
support translation/yaw and cap-travel poses.

`make_assembly_pdf.py` draws the two-page R26 assembly/finishing PDF from the
released STL meshes, checking the manifest and source hashes first. Fill the
`ORDERS` table at the top before a delivery. It requires NumPy, Matplotlib and
ReportLab. Intermediate images go under `../tmp/pdfs/`; visually review the
final PDF before delivery.
