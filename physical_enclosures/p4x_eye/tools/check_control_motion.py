#!/usr/bin/env python3
"""Check sampled control assembly and motion using the actual OpenSCAD modules.

These probes detect nominal rigid-body collisions, not spring deformation,
coating tolerance, surface friction or resin durability. Requires Manifold.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time

from check_base_reinforcement import stl_triangles, volume


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[1] / "enclosure.scad")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--only", nargs="+", help="Run only the named probes")
    parser.add_argument("--openscad-command", nargs="+", default=["openscad"])
    args = parser.parse_args()
    source = args.source.read_text()
    marker = '\nif(part=="base") base();'
    if source.count(marker) != 1:
        raise ValueError("Expected one final dispatcher; source left untouched")
    model = source.split(marker)[0]
    helper = """
module fitted_lid() { translate([0,0,mid_top]) lid(); }
module strip_at(dz=0) { translate([0,0,dz]) button_strip(); }
module cap_at(x=0,y=0,dz=0) { translate([0,y,dz]) power_slider(x); }
module taller_power_switch_reference() {
    power_switch_reference();
    // Extend only the existing reference handle by the documented 0.5 mm
    // BOM/drawing discrepancy; retain the actual source body and XY geometry.
    translate([0,0,0.5]) intersection() {
        power_switch_reference();
        translate([power_switch_xy[0]-5,power_switch_xy[1]-5,
                   pcb_z+pcb_t+power_switch_body[2]]) cube([10,10,4]);
    }
}
"""
    entry_offsets = [round(-4+i/4, 4) for i in range(17)]
    slide_fractions = [i/33 for i in range(34)]
    checks = {
        "strip_insertion_and_settlement_vs_lid": f"""
            for(dz=concat({json.dumps(entry_offsets)},[-button_keeper_float]))
                intersection() {{ fitted_lid(); strip_at(dz); }}
        """,
        "keeper_entry_vs_lid_and_upper_strip": f"""
            for(dz={json.dumps(entry_offsets)}) intersection() {{
                union() {{ fitted_lid(); button_strip(); }}
                translate([0,button_keeper_y_shift,dz]) button_keeper();
            }}
        """,
        "keeper_slide_vs_lid_and_upper_strip": f"""
            for(f={json.dumps(slide_fractions)}) intersection() {{
                union() {{ fitted_lid(); button_strip(); }}
                translate([0,button_keeper_y_shift*f,0]) button_keeper();
            }}
        """,
        "keeper_seated_vs_settled_strip": """
            intersection() { button_keeper(); strip_at(-button_keeper_float); }
        """,
        "cap_guide_extremes_vs_lid": """
            for(x=[-(power_slot_size[0]-power_nub_size[0])/2,0,
                     (power_slot_size[0]-power_nub_size[0])/2])
            for(y=[-(power_slot_size[1]-power_nub_size[1])/2,0,
                     (power_slot_size[1]-power_nub_size[1])/2])
            for(z=[-power_downward_float,0,power_upper_gap])
                intersection() { fitted_lid(); cap_at(x,y,z); }
        """,
        "cap_guide_extremes_vs_retaining_strip": """
            for(s=[0,-button_keeper_float])
            for(x=[-(power_slot_size[0]-power_nub_size[0])/2,0,
                     (power_slot_size[0]-power_nub_size[0])/2])
            for(y=[-(power_slot_size[1]-power_nub_size[1])/2,0,
                     (power_slot_size[1]-power_nub_size[1])/2])
            for(z=[s-power_lower_gap,0,power_upper_gap])
                intersection() { strip_at(s); cap_at(x,y,z); }
        """,
        "cap_socket_extremes_vs_taller_switch": """
            for(x=[power_switch_travel/2-(power_socket_inner-power_switch_handle[0])/2,
                   power_switch_travel/2,
                   power_switch_travel/2+(power_socket_inner-power_switch_handle[0])/2])
            for(y=[-(power_socket_inner-power_switch_handle[1])/2,0,
                    (power_socket_inner-power_switch_handle[1])/2])
            for(z=[-power_downward_float,0,power_upper_gap])
                intersection() { taller_power_switch_reference(); cap_at(x,y,z); }
        """,
        "strip_settlement_vs_switch_references": """
            for(s=[0,-button_keeper_float]) intersection() {
                strip_at(s);
                union() { button_switch_reference(); taller_power_switch_reference(); }
            }
        """,
        "lowered_shelf_switch_body_clearance": """
            for(sx=[-0.25,0.25], sy=[-0.25,0.25], angle=[-1.5,0,1.5])
            intersection() {
                translate([power_switch_xy[0]+sx,power_switch_xy[1]+sy,-button_keeper_float])
                    rotate([0,0,angle]) translate([-power_switch_xy[0],-power_switch_xy[1],0])
                        button_strip();
                translate([power_switch_xy[0]-power_switch_body[0]/2-0.2499,
                           power_switch_xy[1]-power_switch_body[1]/2-0.2499,pcb_z+pcb_t])
                    cube([power_switch_body[0]+0.4998,power_switch_body[1]+0.4998,
                          power_switch_body[2]+0.2499]);
            }
        """,
        "flange_retains_one_mm_bearing_both_sides": """
            // Two 1 x 2 mm bearing patches next to the opening must remain
            // covered by both actual parts across conservative play extremes.
            for(sx=[-0.25,0.25], sy=[-0.25,0.25], angle=[-1.5,0,1.5],
                x=[-1.5,0,1.5], y=[-0.3,0.3], side=[-1,1]) difference() {
                translate([power_switch_xy[0]+sx,power_switch_xy[1]+sy,0])
                    rotate([0,0,angle])
                        translate([side*(power_retainer_slot[0]/2+0.501)-0.5,-1,
                                   power_retainer_z[1]-0.005]) cube([1,2,0.004]);
                intersection() {
                    translate([power_switch_xy[0]+sx,power_switch_xy[1]+sy,0])
                        rotate([0,0,angle]) translate([-power_switch_xy[0],-power_switch_xy[1],0])
                            button_strip();
                    cap_at(x,y,-power_lower_gap-0.01);
                }
            }
        """,
    }
    if args.only:
        if any(name not in checks for name in args.only):
            parser.error("Unknown probe name")
        checks = {name: checks[name] for name in args.only}
    result = {
        "source": str(args.source.resolve()),
        "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "backend": "Manifold",
        "openscad_command": args.openscad_command,
        "penetration_volume_tolerance_mm3": 1e-6,
        "entry_offsets_mm": entry_offsets,
        "keeper_slide_fractions": slide_fractions,
        "limitations": [
            "Rigid-body insertion/motion is sampled; elastic key deflection and fatigue are not modeled.",
            "Cap guide extremes are checked without forcing them against the switch handle; the socket/reference probe uses its actual 0.25 mm allowed play.",
            "Controls are centered in their guide system; separate clearance probes cover adverse strip translation/yaw.",
            "Intentional bearing contact is permitted; print errors, varnish, friction and real installed component heights remain unqualified.",
        ],
        "checks": {},
    }
    with tempfile.TemporaryDirectory(prefix="p4x-control-motion-") as work:
        work = Path(work)
        for name, body in checks.items():
            scad, stl = work/f"{name}.scad", work/f"{name}.stl"
            scad.write_text(model+helper+"\nunion() {\n"+body+
                            "\ntranslate([200,200,200]) cube(1);\n}\n")
            command = args.openscad_command + ["--enable=roof", "--backend", "Manifold", "--hardwarnings",
                                               "--export-format", "binstl", "-o", str(stl), str(scad)]
            started = time.monotonic()
            proc = subprocess.run(command, capture_output=True, text=True)
            item = {"seconds": round(time.monotonic()-started, 3), "returncode": proc.returncode}
            if proc.returncode or re.search(r"(?:ERROR|WARNING):", proc.stderr+proc.stdout):
                item.update(passed=False, error=(proc.stderr+proc.stdout)[-8000:])
            else:
                triangles = stl_triangles(stl)
                witness = [t for t in triangles if all(199 < v < 202 for v in t)]
                collision = [t for t in triangles if not all(199 < v < 202 for v in t)]
                witness_volume, collision_volume = volume(witness), volume(collision)
                item.update(witness_triangles=len(witness), witness_volume_mm3=witness_volume,
                            collision_triangles=len(collision), collision_volume_mm3=collision_volume,
                            passed=abs(witness_volume-1) < 1e-6 and collision_volume <= 1e-6)
                if collision:
                    item["collision_bounds_mm"] = [
                        [min(t[j] for t in collision for j in range(a,9,3)) for a in range(3)],
                        [max(t[j] for t in collision for j in range(a,9,3)) for a in range(3)],
                    ]
            result["checks"][name] = item
            print(f"{name}: {'PASS' if item['passed'] else 'FAIL'}", flush=True)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(result, indent=2)+"\n")
    result["passed"] = all(r["passed"] for r in result["checks"].values())
    args.output.write_text(json.dumps(result, indent=2)+"\n")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
