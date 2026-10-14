#!/usr/bin/env python3
"""Probe nominal assembly clearances using the actual OpenSCAD part modules.

No source parameters are replaced. Test gauges expand production geometry or
represent nominal M2 shafts; intentional support/seat contacts are excluded.
Requires OpenSCAD with Manifold. Results describe the named probes, not every
possible assembly pose or the tolerances of a physical print.
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


def probes():
    return {
        "lcd_lateral_clearance": """
            intersection() {
                fitted_lid();
                expand_xy_column(lcd_seat_z+0.001,case_depth+0.001) intersection() {
                    lcd_reference(); z_band(lcd_seat_z+0.001,case_depth+1);
                }
            }
        """,
        "alignment_keys_lateral_clearance": """
            intersection() {
                fitted_lid();
                expand_xy_column(mid_top+0.001,mid_top+lid_key_h) intersection() {
                    lid_alignment_keys(); z_band(mid_top+0.001,case_depth);
                }
            }
        """,
        "alignment_keys_top_clearance": """
            intersection() {
                fitted_lid();
                translate([0,0,probe_gap]) lid_alignment_keys();
            }
        """,
        "midframe_plate_lateral_clearance": """
            intersection() {
                base();
                expand_xy_column(mid_z+0.001,mid_top-0.001) intersection() {
                    midframe(); z_band(mid_z+0.001,mid_top-0.001);
                }
            }
        """,
        "usb_saddle_end_clearance": """
            for(side=[-1,1]) intersection() {
                fitted_lid();
                translate([0,side*probe_gap,0]) intersection() {
                    base(); usb_lower_saddle();
                    z_band(mid_top+0.001,case_depth);
                }
            }
        """,
        "usb_saddle_vertical_split_clearance": """
            intersection() {
                fitted_lid();
                translate([0,0,probe_gap]) intersection() {
                    base(); usb_lower_saddle();
                }
            }
        """,
        "pcb_upward_clearance_to_lid": """
            intersection() {
                fitted_lid();
                translate([0,0,pcb_z+probe_gap]) board_reference();
            }
        """,
        "case_m2_shaft_and_tip_free_passages": """
            intersection() {
                fitted_parts();
                for(p=boss_xy)
                    translate([p[0],p[1],case_head_recess_depth+0.001])
                        cylinder(r=(1+probe_gap)/cos(180/256),$fn=256,
                                 h=case_tip_relief_top-case_head_recess_depth-0.002);
            }
        """,
        "midframe_m2_shaft_and_tip_free_passages": """
            intersection() {
                union() { base(); midframe(); }
                for(p=[mid_third_xy,mid_right_xy])
                    translate([p[0],p[1],mid_tip_relief_bottom+0.001])
                        cylinder(r=(1+probe_gap)/cos(180/256),$fn=256,
                                 h=mid_top-mid_tip_relief_bottom-0.002);
            }
        """,
        "case_m2_head_recess_radial_clearance": """
            intersection() {
                base();
                for(p=boss_xy) translate([p[0],p[1],0.001])
                    cylinder(r=(2+probe_gap)/cos(180/512),$fn=512,
                             h=case_head_recess_depth-0.002);
            }
        """,
        "battery_lateral_clearance_to_base_and_ledge": """
            intersection() {
                base();
                expand_xy_column(battery_z,battery_z+battery_t) battery_reference();
            }
        """,
        "keeper_side_running_clearance": """
            for(side=[-1,1]) intersection() {
                fitted_lid();
                translate([side*probe_gap,0,0]) intersection() {
                    button_keeper();
                    z_band(button_keeper_z[0]+0.001,button_keeper_z[1]-0.001);
                }
            }
        """,
        "fixed_strip_and_shelf_lateral_clearance": """
            for(dz=[0,-button_keeper_float]) intersection() {
                fitted_lid();
                expand_xy_column(button_arm_z+dz+0.001,button_arm_top+dz-0.001)
                    intersection() {
                        translate([0,0,dz]) button_strip();
                        z_band(button_arm_z+dz+0.001,button_arm_top+dz-0.001);
                    }
            }
        """,
        "keeper_vertical_float": """
            intersection() {
                button_strip();
                translate([0,0,probe_gap]) button_keeper();
            }
        """,
        # Extreme poses from the independent keeper-play grid. These are
        # adverse sampled poses, not a claim covering every allowed pose.
        "retainer_socket_lateral_extreme_x": """
            retainer_socket_probe(-0.0261*180/PI,[-0.22,-0.17],[1.5,-0.3]);
        """,
        "retainer_socket_lateral_extreme_y": """
            retainer_socket_probe(-0.0251*180/PI,[-0.205,-0.195],[1.5,0.3]);
        """,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[1] / "enclosure.scad")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--clearance", type=float, default=0.25)
    parser.add_argument("--only", nargs="+", choices=list(probes()))
    parser.add_argument("--openscad-command", nargs="+", default=["openscad"])
    args = parser.parse_args()
    if args.clearance <= 0.0001:
        parser.error("Clearance must exceed the 0.0001 mm numerical allowance")
    source = args.source.read_text()
    marker = '\nif(part=="base") base();'
    if source.count(marker) != 1:
        raise ValueError("Expected one final part dispatcher; source left untouched")
    model = source.split(marker)[0]
    helpers = f"probe_gap={args.clearance-0.0001:.9f};\n" + """
module fitted_lid() { translate([0,0,mid_top]) lid(); }
module fitted_parts() { base(); midframe(); fitted_lid(); }
module z_band(z0,z1) {
    translate([-10,-10,z0]) cube([case_w+20,case_h+20,z1-z0]);
}
module expand_xy_column(z0,z1) {
    // Native 2D offset avoids thin-solid Minkowski failures in this runtime.
    // Sweeping the full production silhouette is conservative where a key
    // chamfer narrows: the wider lower outline is tested over the whole height.
    production_fn=$fn;
    translate([0,0,z0]) linear_extrude(z1-z0)
        offset(r=probe_gap/cos(180/256),$fn=256) projection(cut=false)
            let($fn=production_fn) children();
}
module retainer_socket_probe(angle,strip_xy,cap_xy) {
    strip_drop=button_keeper_float;
    cap_drop=power_flange_z[0]-power_retainer_z[1]+strip_drop;
    intersection() {
        translate([power_switch_xy[0]+strip_xy[0],
                   power_switch_xy[1]+strip_xy[1],-strip_drop])
            rotate([0,0,angle])
                translate([-power_switch_xy[0],-power_switch_xy[1],0]) button_strip();
        expand_xy_column(power_retainer_z[0]-strip_drop+0.001,
                         power_retainer_z[1]-strip_drop-0.001) intersection() {
            translate([0,cap_xy[1],-cap_drop]) power_slider(cap_xy[0]);
            // Exclude the flange's intentional bearing face on the shelf.
            z_band(power_retainer_z[0]-strip_drop+0.001,
                   power_retainer_z[1]-strip_drop-0.001);
        }
    }
}
"""
    checks = probes()
    if args.only:
        checks = {name: checks[name] for name in args.only}
    results = {
        "source": str(args.source.resolve()),
        "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "backend": "Manifold", "openscad_command": args.openscad_command,
        "requested_clearance_mm": args.clearance,
        "tested_clearance_mm": args.clearance - 0.0001,
        "penetration_volume_tolerance_mm3": 1e-6,
        "limitations": [
            "Only the named nominal interfaces/poses are checked; print tolerances and material behavior remain unqualified.",
            "LCD and frame seats, key roots, screw-head seats and blind-hole roofs are excluded by slicing 0.001 mm from their datum faces.",
            "M2 passage probes check printed bores; they exclude installed insert threads.",
            "Lateral probes offset and sweep the actual production silhouette; key chamfers are conservatively filled. USB end faces are tested at both 0.2499 mm Y offsets.",
            "Keeper side probes exclude its support ledge and preserve intentional contact with the leading Y stop. Strip probes exclude its upper roof datum.",
            "Retainer checks cover two adverse sampled strip poses at settled Z and maximum nominal cap drop, not continuous play.",
        ],
        "checks": {},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="p4x-clearance-check-") as work:
        work = Path(work)
        for name, body in checks.items():
            scad, stl = work / f"{name}.scad", work / f"{name}.stl"
            scad.write_text(model + helpers + "\nunion() {\n" + body +
                            "\ntranslate([200,200,200]) cube(1);\n}\n")
            started = time.monotonic()
            command = args.openscad_command + ["--enable=roof", "--backend", "Manifold", "--hardwarnings",
                                               "--export-format", "binstl", "-o", str(stl), str(scad)]
            proc = subprocess.run(command, capture_output=True, text=True)
            result = {"seconds": round(time.monotonic()-started, 3), "returncode": proc.returncode}
            if proc.returncode or re.search(r"(?:ERROR|WARNING):", proc.stderr + proc.stdout):
                result.update(passed=False, error=(proc.stderr + proc.stdout)[-8000:])
            else:
                triangles = stl_triangles(stl)
                witness = [t for t in triangles if all(199 < v < 202 for v in t)]
                collision = [t for t in triangles if not all(199 < v < 202 for v in t)]
                witness_volume, collision_volume = volume(witness), volume(collision)
                result.update(witness_triangles=len(witness), witness_volume_mm3=witness_volume,
                              collision_triangles=len(collision), collision_volume_mm3=collision_volume,
                              passed=abs(witness_volume-1) < 1e-6 and collision_volume <= 1e-6)
                if collision:
                    result["collision_bounds_mm"] = [
                        [min(t[j] for t in collision for j in range(a, 9, 3)) for a in range(3)],
                        [max(t[j] for t in collision for j in range(a, 9, 3)) for a in range(3)],
                    ]
            results["checks"][name] = result
            print(f"{name}: {'PASS' if result['passed'] else 'FAIL'}", flush=True)
            args.output.write_text(json.dumps(results, indent=2) + "\n")
    results["passed"] = all(r["passed"] for r in results["checks"].values())
    args.output.write_text(json.dumps(results, indent=2) + "\n")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
