#!/usr/bin/env python3
"""Check enclosure fit by intersecting the actual OpenSCAD production modules.

Requires an OpenSCAD build with Manifold; Python uses only its standard library.
Example: python3 check_base_reinforcement.py --output /tmp/r22-fit.json \
    --openscad-command node /tmp/openscad.js

This checks nominal geometry, not print tolerances or material strength. A remote
1 mm witness cube makes empty intersections exportable without ignoring errors.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time


def stl_triangles(path):
    data = path.read_bytes()
    if len(data) >= 84:
        count = struct.unpack_from("<I", data, 80)[0]
        if len(data) == 84 + count * 50:
            return [struct.unpack_from("<12fH", data, 84 + i * 50)[3:12]
                    for i in range(count)]
    vertices = re.findall(r"vertex\s+([-+\deE.]+)\s+([-+\deE.]+)\s+([-+\deE.]+)",
                          data.decode("ascii"))
    if not vertices or len(vertices) % 3:
        raise ValueError(f"Malformed STL: {path}")
    return [tuple(float(v) for p in vertices[i:i + 3] for v in p)
            for i in range(0, len(vertices), 3)]


def volume(triangles):
    return abs(math.fsum(
        a * (e * i - f * h) + b * (f * g - d * i) + c * (d * h - e * g)
        for a, b, c, d, e, f, g, h, i in triangles) / 6)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[1] / "enclosure.scad")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--openscad-command", nargs="+", default=["openscad"])
    args = parser.parse_args()
    source = args.source.read_text()
    marker = '\nif(part=="base") base();'
    if source.count(marker) != 1:
        raise ValueError("Expected one final part dispatcher; source left untouched")
    # Keep all production parameters, assertions, and modules. Remove only the
    # final display dispatcher so each probe can call the original modules.
    model = source.split(marker)[0]
    helper = """
module fitted_lid(dz=0) { translate([0,0,mid_top+dz]) lid(); }
module fitted_parts() { base(); midframe(); fitted_lid(); }
module plug_envelopes() {
    for(y=usb_case_y)
        translate([usb_plug_front_x,y-usb_plug_body_w/2,
                   usb_center_z-usb_plug_body_h/2])
            cube([case_w-usb_plug_front_x+5,usb_plug_body_w,usb_plug_body_h]);
}
"""
    offsets = [i / 4 for i in range(37)]  # Includes seated through 9 mm lift.
    checks = {
        "base_battery": "intersection() { base(); battery_reference(); }",
        "base_lid_seated": "intersection() { base(); fitted_lid(); }",
        "base_midframe": "intersection() { base(); midframe(); }",
        "lid_midframe_seated": "intersection() { fitted_lid(); midframe(); }",
        "screws_through_assembled_parts":
            "intersection() { fitted_parts(); case_screw_reference(); }",
        "usb_plug_envelopes":
            "intersection() { fitted_parts(); plug_envelopes(); }",
        "usb_socket_references":
            "intersection() { fitted_parts(); usb_reference(); }",
        "pcb_reference":
            "intersection() { fitted_parts(); translate([0,0,pcb_z]) board_reference(); }",
        "lid_insertion_sampled": f"""
            for(dz={json.dumps(offsets)}) intersection() {{
                fitted_lid(dz); union() {{ base(); midframe(); }}
            }}
        """,
        # A prism containing the complete rear tab, swept downward relative to
        # the fixed lid, conservatively covers every continuous upward lid pose.
        # No hull of the concave shell or duplicated replacement model is used.
        "usb_end_tab_continuous_insertion_envelope": """
            intersection() {
                fitted_lid();
                translate([usb_wall_x-usb_wall_t,
                           usb_case_y[1]+usb_slot_w/2,mid_top-case_depth])
                    cube([usb_wall_t,
                          usb_rail_y[1]-(usb_case_y[1]+usb_slot_w/2),
                          usb_rail_top-mid_top+case_depth]);
            }
        """,
    }
    results = {
        "source": str(args.source.resolve()),
        "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "backend": "Manifold", "openscad_command": args.openscad_command,
        "penetration_volume_tolerance_mm3": 1e-6,
        "lid_insertion_sampled_offsets_mm": offsets,
        "limitations": [
            "Full lid insertion is sampled at 0.25 mm; the rear USB tab has an additional continuous conservative-envelope check.",
            "Reference hardware and plug envelopes use source dimensions; real tolerances, solder, finish and resin strength are not qualified.",
            "The approximately 0.5 mm nominal battery gap is not a guaranteed printed clearance.",
        ],
        "checks": {},
    }
    with tempfile.TemporaryDirectory(prefix="p4x-reinforcement-check-") as work:
        work = Path(work)
        for name, body in checks.items():
            scad, stl = work / f"{name}.scad", work / f"{name}.stl"
            scad.write_text(model + helper + "\nunion() {\n" + body +
                            "\ntranslate([200,200,200]) cube(1);\n}\n")
            started = time.monotonic()
            command = args.openscad_command + ["--enable=roof", "--backend", "Manifold", "--hardwarnings",
                                               "--export-format", "binstl", "-o", str(stl), str(scad)]
            proc = subprocess.run(command, capture_output=True, text=True)
            result = {"seconds": round(time.monotonic() - started, 3), "returncode": proc.returncode}
            if proc.returncode or re.search(r"(?:ERROR|WARNING):", proc.stderr + proc.stdout):
                result.update(passed=False, error=(proc.stderr + proc.stdout)[-8000:])
            else:
                triangles = stl_triangles(stl)
                witness = [t for t in triangles if all(199 < v < 202 for v in t)]
                collision = [t for t in triangles if not all(199 < v < 202 for v in t)]
                witness_volume = volume(witness)
                collision_volume = volume(collision)
                result.update(witness_triangles=len(witness), witness_volume_mm3=witness_volume,
                              collision_triangles=len(collision), collision_volume_mm3=collision_volume,
                              passed=abs(witness_volume - 1) < 1e-6 and collision_volume <= 1e-6)
                if collision and collision_volume <= 1e-6:
                    result["contact_only"] = True
                if collision:
                    result["collision_bounds_mm"] = [
                        [min(t[j] for t in collision for j in range(a, 9, 3)) for a in range(3)],
                        [max(t[j] for t in collision for j in range(a, 9, 3)) for a in range(3)],
                    ]
            results["checks"][name] = result
            print(f"{name}: {'PASS' if result['passed'] else 'FAIL'}", flush=True)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(results, indent=2) + "\n")
    results["passed"] = all(r["passed"] for r in results["checks"].values())
    args.output.write_text(json.dumps(results, indent=2) + "\n")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
