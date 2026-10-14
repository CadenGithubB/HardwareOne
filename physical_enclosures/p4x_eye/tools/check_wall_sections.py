#!/usr/bin/env python3
"""Measure named enclosure walls in exported STL meshes, without repairing them.

The manifest supplies file hashes and translations from export_stl.py. Rays and
triangle-plane sections are evaluated in assembly coordinates. These samples
are not a global minimum-thickness proof, a strength test, or a warpage model.
The named coordinates cover the R26 design; moving features requires reviewing
the probe positions. Only Python's standard library is needed.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path

from check_stl import read_stl


def load_meshes(manifest_path, case_height, case_depth):
    manifest = json.loads(manifest_path.read_text())
    meshes, hashes = {}, {}
    for part in manifest["parts"]:
        name = part["part"]
        path = manifest_path.parent / part["file"]
        raw = path.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if digest != part["sha256"]:
            raise ValueError(f"Manifest hash mismatch: {path}")
        shift = part["post_export_translation_mm"]

        def assembly_point(point):
            p = [point[i] - shift[i] for i in range(3)]
            if name == "lid":
                p = [p[0], case_height - p[1], case_depth - p[2]]
            elif name == "power_slider":
                p = [p[0], -p[1], -p[2]]
            return p

        meshes[name] = [[assembly_point(p) for p in t] for t in read_stl(raw)]
        hashes[name] = digest
    return manifest, meshes, hashes


def ray_intervals(triangles, axis, fixed):
    """Intersect a complete axis-aligned line; pair unique boundary crossings."""
    dims = [d for d in range(3) if d != axis]
    hits = []
    for a, b, c in triangles:
        u = [b[d] - a[d] for d in dims]
        v = [c[d] - a[d] for d in dims]
        p = [fixed[i] - a[d] for i, d in enumerate(dims)]
        det = u[0] * v[1] - u[1] * v[0]
        if abs(det) < 1e-16:
            continue
        f = (p[0] * v[1] - p[1] * v[0]) / det
        g = (u[0] * p[1] - u[1] * p[0]) / det
        if f >= -1e-9 and g >= -1e-9 and f + g <= 1 + 1e-9:
            hits.append(a[axis] + f * (b[axis] - a[axis]) + g * (c[axis] - a[axis]))
    unique = []
    for value in sorted(hits):
        if not unique or value - unique[-1] > 1e-6:
            unique.append(value)
    if len(unique) % 2:
        raise ValueError(f"Unpaired ray crossings at axis {axis}, {fixed}: {unique}")
    return list(zip(unique[::2], unique[1::2]))


def section(triangles, axis, value):
    dims = [d for d in range(3) if d != axis]
    segments = []
    for triangle in triangles:
        hits = set()
        for i in range(3):
            p, q = triangle[i], triangle[(i + 1) % 3]
            v, w = p[axis] - value, q[axis] - value
            if v == 0:
                hits.add(tuple(p[d] for d in dims))
            if v * w < 0:
                f = -v / (w - v)
                hits.add(tuple(p[d] + f * (q[d] - p[d]) for d in dims))
        if len(hits) == 2:
            segments.append(tuple(hits))
        elif len(hits) > 2:
            raise ValueError("Section lies on a triangle; choose a different plane")
    return segments


def point_segment(point, segment):
    a, b = segment
    delta = [b[i] - a[i] for i in range(2)]
    length2 = sum(d * d for d in delta)
    t = max(0, min(1, sum((point[i] - a[i]) * delta[i] for i in range(2)) / length2)) if length2 else 0
    closest = tuple(a[i] + t * delta[i] for i in range(2))
    return math.dist(point, closest)


def segment_distance(a, b):
    # The classified section contours are disjoint. Their nearest points must
    # include an endpoint; no distance between adjacent facets is mistaken for
    # the thickness of a wall.
    return min(*(point_segment(p, b) for p in a), *(point_segment(p, a) for p in b))


def measurements(meshes, case_depth):
    results = []

    def wall(label, part, axis, fixed, window, minimum=1.0):
        intervals = [p for p in ray_intervals(meshes[part], axis, fixed)
                     if window[0] < sum(p) / 2 < window[1]]
        thickness = min((b - a for a, b in intervals), default=None)
        results.append({"feature": label, "part": part, "method": "axis ray",
                        "ray_axis": "XYZ"[axis], "fixed_axes": "".join("XYZ"[d] for d in range(3) if d != axis),
                        "fixed_coordinates_mm": fixed, "selection_window_mm": window,
                        "solid_intervals_mm": intervals, "sampled_thickness_mm": thickness,
                        "required_minimum_mm": minimum,
                        "pass": thickness is not None and thickness >= minimum - 1e-5})

    wall("base floor", "base", 2, [40.123, 20.123], [-1, 3], 2)
    wall("lid main face", "lid", 2, [70.123, 30.123], [27, 30], 2)
    wall("lid front wall", "lid", 1, [40.123, 24.123], [-1, 3])
    wall("encoder hardware band", "lid", 1, [64.123, 19.123], [42, 46])
    wall("encoder upper wall", "lid", 1, [64.123, 24.123], [42, 46])
    wall("USB side wall", "lid", 0, [22.123, 25.123], [74, 77])
    wall("left key receiver roof", "lid", 2, [3.123, 24.123], [17, 19])
    wall("left key receiver mouth", "lid", 0, [24.123, 14.501], [4, 6])
    wall("front key receiver mouth", "lid", 1, [44.123, 14.501], [4, 6])
    wall("rear key receiver mouth", "lid", 1, [28.123, 14.501], [50, 52])
    wall("left keeper rail root", "lid", 0, [22.123, 25.123], [57, 59])
    wall("right keeper rail root", "lid", 0, [22.123, 25.123], [67, 69])
    wall("left strip guide", "lid", 0, [28.123, 26.123], [59, 61])
    wall("right strip guide", "lid", 0, [28.123, 26.123], [65, 67])
    for y in [19.123, 25.123, 31.123]:
        wall(f"button arm at Y{y}", "button_strip", 2, [56.123, y], [24, 28])
    wall("slider support bridge", "button_strip", 2, [63.123, 16.123], [24, 28], 1.2)
    wall("support/bar overlap", "button_strip", 2, [63.123, 16.923], [24, 28], 1.0)
    bridge = ray_intervals(meshes["button_strip"], 2, [63.123, 16.123])
    bar = ray_intervals(meshes["button_strip"], 2, [63.123, 17.373])
    for i, label in [(0, "lower"), (1, "upper")]:
        step = bar[0][i] - bridge[0][i] if len(bar) == len(bridge) == 1 else None
        results.append({"feature": f"button/support {label} step", "part": "button_strip",
                        "method": "difference of actual mesh section datums",
                        "bridge_ray_xy_mm": [63.123, 16.123], "bar_ray_xy_mm": [63.123, 17.373],
                        "bridge_intervals_mm": bridge, "bar_intervals_mm": bar,
                        "measured_step_mm": step, "required_minimum_mm": 0.5,
                        "pass": step is not None and step >= 0.5-1e-5})
    wall("keeper plate", "button_keeper", 2, [62.123, 30.123], [23, 27])
    wall("slider flange", "power_slider", 2, [68.123, 11.323], [26, 28])
    wall("slider socket mouth walls", "power_slider", 0, [11.323, 25.551], [60, 66])
    wall("slider socket upper walls", "power_slider", 0, [11.323, 25.951], [60, 66])
    wall("slider nub beside socket", "power_slider", 1, [63.123, 28.031], [8, 14])
    wall("left alignment key tip", "midframe", 0, [24.123, 16.999], [2, 4])
    wall("front alignment key tip", "midframe", 1, [44.123, 16.999], [2, 4])
    wall("rear alignment key tip", "midframe", 1, [28.123, 16.999], [52, 54])
    wall("battery front guide", "base", 1, [40.123, 2.7], [3, 6])
    wall("battery rear guide", "base", 1, [40.123, 2.7], [42, 45])
    wall("rear USB saddle tab", "base", 1, [75.3, 22.0], [37, 39])

    centers = [[3.6, 3.6], [76.4, 3.6], [3.6, 52.4], [77.26, 41.0]]
    # Z1.5 samples the floor band between the face bevel and the floor top,
    # where R25 lost the sleeve material around every screw-head well.
    for part, z_values, bore_radius in [("base", [1.5, 5.0, 10.0], 2.252),
                                       ("lid", [14.501], 1.54153846),
                                       ("lid", [15.1], 1.35), ("lid", [20.1], 1.255)]:
        for z in z_values:
            segments = section(meshes[part], 2, z)
            for center in centers:
                inside, other = [], []
                for seg in segments:
                    destination = inside if all(abs(math.dist(p, center) - bore_radius) < .01 for p in seg) else other
                    destination.append(seg)
                # A second, nearly coincident outer contour must not be
                # silently classified as part of the hole and excluded from
                # the ligament measurement. Exactly one bore turns through
                # 2*pi radians; additional/missing contour arcs fail this guard.
                coverage = 0.0
                for a, b in inside:
                    u = [a[i] - center[i] for i in range(2)]
                    v = [b[i] - center[i] for i in range(2)]
                    coverage += abs(math.atan2(u[0] * v[1] - u[1] * v[0],
                                               u[0] * v[0] + u[1] * v[1]))
                bore_identified = len(inside) >= 3 and abs(coverage - 2 * math.pi) < 1e-6
                minimum = min((segment_distance(a, b) for a in inside for b in other), default=None)
                results.append({"feature": "screw bore ligament", "part": part,
                                "method": "minimum distance between section contours",
                                "section_z_mm": z, "bore_center_xy_mm": center,
                                "bore_boundary_segments": len(inside), "bore_angular_coverage_radians": coverage,
                                "bore_identification_passed": bore_identified, "sampled_thickness_mm": minimum,
                                "required_minimum_mm": 1.0,
                                "pass": bore_identified and minimum is not None and minimum >= 1 - 1e-5})

    for part, cavity_corner in [("base", [2, 2]), ("lid", [1.6, 2])]:
        segments = section(meshes[part], 0, 40.123)
        if part == "lid":
            segments = [[(y, case_depth - z) for y, z in seg] for seg in segments]
        outer = [seg for seg in segments if all(0 <= y <= 2.0001 and 0 <= z <= 2.0001 for y, z in seg)]
        minimum = min((point_segment(cavity_corner, seg) for seg in outer), default=None)
        results.append({"feature": "front chamfered perimeter band", "part": part,
                        "method": "section contour to unchanged cavity corner",
                        "section_x_mm": 40.123, "cavity_corner_y_face_depth_mm": cavity_corner,
                        "outer_segments": len(outer), "sampled_thickness_mm": minimum,
                        "required_minimum_mm": 1.0,
                        "pass": minimum is not None and minimum >= 1 - 1e-5})
        bevel = [seg for seg in outer if all(-1e-5 <= y <= 1.00001 and
                                             -1e-5 <= z <= 1.00001 for y, z in seg)]
        residual = max((abs(y+z-1) for seg in bevel for y, z in seg), default=None)
        results.append({"feature": "continuous 1 mm face chamfer", "part": part,
                        "method": "actual section follows y + face_depth = 1",
                        "section_x_mm": 40.123, "bevel_segments": len(bevel),
                        "maximum_line_residual_mm": residual,
                        "pass": residual is not None and residual < 1e-5})

    terraces = [t for t in meshes["base"]
                if max(p[2] for p in t)-min(p[2] for p in t) < 1e-7
                and 1e-5 < sum(p[2] for p in t)/3 < 1-1e-5]
    results.append({"feature": "base micro-terraces eliminated", "part": "base",
                    "method": "no intermediate horizontal faces inside 0 < Z < 1 bevel band",
                    "horizontal_triangles_in_open_band": len(terraces), "pass": not terraces})

    for z in [15.1, 18.7, 20.1, 24.1]:
        intervals = ray_intervals(meshes["lid"], 1, [75.037, z])
        overlaps = [p for p in intervals if min(p[1], 39.3) - max(p[0], 38.7) > 1e-5]
        results.append({"feature": "removed rear USB feather", "part": "lid",
                        "method": "absence of former fin on named ray", "ray_axis": "Y",
                        "fixed_coordinates_xz_mm": [75.037, z], "inspection_window_y_mm": [38.7, 39.3],
                        "overlapping_solid_intervals_mm": overlaps, "pass": not overlaps})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--case-height-mm", type=float, default=56)
    parser.add_argument("--case-depth-mm", type=float, default=29)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    manifest, meshes, hashes = load_meshes(args.manifest, args.case_height_mm, args.case_depth_mm)
    records = measurements(meshes, args.case_depth_mm)
    result = {"revision": manifest["revision"], "source_sha256": manifest["source_sha256"],
              "stl_sha256": hashes, "manifest": str(args.manifest),
              "scope": f"Named R{manifest['revision']} production-mesh rays, steps and cross-sections only; not a global minimum wall, material-strength, or deformation certification. Run check_thickness_scan.py for the whole-part comparison.",
              "numerical_thickness_allowance_mm": 1e-5,
              "inverse_print_transform_case_height_depth_mm": [args.case_height_mm, args.case_depth_mm],
              "checks": records, "all_passed": all(r["pass"] for r in records)}
    output = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(output)
    print(output)
    return 0 if result["all_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
