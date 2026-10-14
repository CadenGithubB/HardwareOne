#!/usr/bin/env python3
"""Export a matched enclosure set with CGAL and audit the actual binary meshes.

No repair is performed. A rigid translation places each oriented part at XYZ=0.
Outputs are revisioned; an existing destination is never silently overwritten.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time

from check_stl import audit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1] / "enclosure.scad")
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--revision", type=int, required=True)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--parts", nargs="+", choices=["base", "lid", "midframe", "button_strip", "button_keeper", "power_slider"])
    parser.add_argument("--openscad-command", nargs="+", default=["openscad"])
    args = parser.parse_args()
    source = args.source.read_text()
    marker = '\nif(part=="base") base();'
    if source.count(marker) != 1:
        raise ValueError("Expected exactly one part dispatcher")
    model = source.split(marker)[0]
    exports = {
        "base": ("base();", "Floor down"),
        "lid": ("translate([0,case_h,case_depth-mid_top]) rotate([180,0,0]) lid();", "Exterior face down"),
        "midframe": ("midframe();", "Flat underside down"),
        "button_strip": ("button_strip();", "Actuator stems down"),
        "button_keeper": ("button_keeper();", "Flat underside down"),
        "power_slider": ("rotate([180,0,0]) power_slider(0);", "Nub face down, socket upward"),
    }
    if args.parts:
        exports = {part: exports[part] for part in args.parts}
    args.directory.mkdir(parents=True, exist_ok=True)
    for part in exports:
        if (args.directory / f"p4x-eye-{part.replace('_', '-')}-r{args.revision}.stl").exists():
            raise FileExistsError(f"Destination already exists: {part}")

    def export(item):
        part, (call, orientation) = item
        started = time.monotonic()
        filename = f"p4x-eye-{part.replace('_', '-')}-r{args.revision}.stl"
        with tempfile.TemporaryDirectory(prefix=f"p4x-export-{part}-") as tmp:
            tmp = Path(tmp)
            scad, raw = tmp / "export.scad", tmp / "export.stl"
            scad.write_text(model + "\n" + call + "\n")
            proc = subprocess.run(args.openscad_command + ["--enable=roof", "--backend", "CGAL", "--hardwarnings",
                                  "--export-format", "binstl", "-o", str(raw), str(scad)],
                                  capture_output=True, text=True)
            log = proc.stdout + proc.stderr
            (args.directory / f"{part}-export.log").write_text(log)
            if proc.returncode or re.search(r"(?:ERROR|WARNING):", log):
                raise RuntimeError(f"{part}: export failed; see log\n{log[-2000:]}")
            data = bytearray(raw.read_bytes())
            count = struct.unpack_from("<I", data, 80)[0]
            if len(data) != 84 + 50 * count:
                raise ValueError(f"{part}: expected binary STL")
            mins = [min(struct.unpack_from("<3f", data, 84+i*50+12+j*12)[axis]
                        for i in range(count) for j in range(3)) for axis in range(3)]
            for i in range(count):
                for j in range(3):
                    offset = 84+i*50+12+j*12
                    p = struct.unpack_from("<3f", data, offset)
                    struct.pack_into("<3f", data, offset, *(p[a]-mins[a] for a in range(3)))
            destination = args.directory / filename
            destination.write_bytes(data)
            result = audit(destination)
            if not result["valid"]:
                raise RuntimeError(f"{part}: invalid export: {result}")
            result.update(part=part, file=filename, revision=args.revision,
                          export_source_sha256=hashlib.sha256(source.encode()).hexdigest(),
                          orientation=orientation, post_export_translation_mm=[-m for m in mins],
                          export_seconds=round(time.monotonic()-started, 3))
            print(f"{part}: valid, {count} triangles, {result['export_seconds']}s", flush=True)
            return result

    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        parts = list(executor.map(export, exports.items()))
    manifest = {
        "revision": args.revision, "units": "mm", "source": "../enclosure.scad",
        "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "compiler": "OpenSCAD CGAL; --hardwarnings; binary STL",
        "openscad_command": args.openscad_command,
        "mesh_cleanup": "No repair. Rigid translation only to put minimum XYZ at zero.",
        "matched_set": f"Use all six R{args.revision} parts together." if len(parts) == 6 else "Partial export; not a complete printable set.",
        "physical_fit_verified": False, "supplier_approved": False, "parts": parts,
    }
    (args.directory / f"export-manifest-r{args.revision}.json").write_text(json.dumps(manifest, indent=2)+"\n")


if __name__ == "__main__":
    main()
