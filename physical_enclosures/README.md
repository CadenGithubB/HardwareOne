# Physical enclosures

Printable cases for HardwareOne boards: OpenSCAD source, exported meshes,
manufacturing drawings and the scripts used to check and package them.

| Board | Folder | Current revision |
|---|---|---|
| Espressif ESP32-P4X-EYE | [p4x_eye](p4x_eye/README.md) | r21 (six parts: base, midframe, lid, button strip, button keeper, power slider) |

## Layout of a board folder

- `enclosure.scad` - the parametric source; `dimensions.json` - the measured
  board and part dimensions it is built from.
- `stl/` - the meshes of the current revision, plus `PRINTING.md` and the
  export manifest.
- `drawings/` - the technical drawing and per-part drawings sent with a
  manufacturing request.
- `tools/` - geometry, clearance and mesh checks, drawing generators and
  packaging scripts (Python and Node).

Only the current revision is kept here. Earlier revisions (r16 to r20), raw
mesh exports and supplier order records are archived outside the repository.
The design has been print-reviewed but a part is not manufacturing-approved
until its supplier review is complete; see each board's README.
