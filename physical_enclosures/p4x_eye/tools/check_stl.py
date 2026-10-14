#!/usr/bin/env python3
"""Check one closed printable part per STL, without repairing its geometry.

Uses only Python's standard library and welds identical coordinates exactly.
Accepts binary or ASCII STL paths. Prints JSON and exits nonzero on failure.
These topology checks do not establish thickness, fit, or freedom from all
self-intersections.
"""

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import struct


def read_stl(raw):
    if len(raw) >= 84:
        count = struct.unpack_from("<I", raw, 80)[0]
        if len(raw) == 84 + 50 * count:
            return [
                [tuple(facet[i:i + 3]) for i in (3, 6, 9)]
                for facet in struct.iter_unpack("<12fH", raw[84:])
            ]

    if not raw.lstrip().startswith(b"solid"):
        raise ValueError("Not an ASCII STL or a binary STL with a valid length")
    lines = iter(line.strip() for line in raw.decode("ascii").splitlines()
                 if line.strip())
    if next(lines, "").split(maxsplit=1)[0:1] != ["solid"]:
        raise ValueError("Not an ASCII STL or a binary STL with a valid length")
    triangles = []
    for line in lines:
        if line.split(maxsplit=1)[0] == "endsolid":
            if next(lines, None) is not None:
                raise ValueError("Unexpected content after endsolid")
            return triangles
        tokens = line.split()
        if len(tokens) != 5 or tokens[:2] != ["facet", "normal"]:
            raise ValueError("Expected facet normal")
        tuple(map(float, tokens[2:]))
        if next(lines, "") != "outer loop":
            raise ValueError("Expected outer loop")
        triangle = []
        for _ in range(3):
            tokens = next(lines, "").split()
            if len(tokens) != 4 or tokens[0] != "vertex":
                raise ValueError("Expected three vertices per facet")
            triangle.append(tuple(map(float, tokens[1:])))
        if next(lines, "") != "endloop" or next(lines, "") != "endfacet":
            raise ValueError("Expected endloop and endfacet")
        triangles.append(triangle)
    raise ValueError("Missing endsolid")


def audit(path):
    raw = path.read_bytes()
    result = {"file": str(path.resolve()), "sha256": hashlib.sha256(raw).hexdigest()}
    triangles = read_stl(raw)
    if not triangles:
        raise ValueError("STL contains no triangles")
    if not all(math.isfinite(x) for triangle in triangles for p in triangle for x in p):
        raise ValueError("STL contains nonfinite coordinates")

    vertices = {}
    faces = []
    edges = defaultdict(list)
    vertex_faces = defaultdict(list)
    areas = []
    volumes = []
    for triangle in triangles:
        face_id = len(faces)
        face = [vertices.setdefault(point, len(vertices)) for point in triangle]
        faces.append(face)
        for vertex in set(face):
            vertex_faces[vertex].append(face_id)
        for i in range(3):
            a, b = face[i], face[(i + 1) % 3]
            edges[tuple(sorted((a, b)))].append((face_id, a < b))
        a, b, c = triangle
        u = tuple(b[i] - a[i] for i in range(3))
        v = tuple(c[i] - a[i] for i in range(3))
        cross = (u[1] * v[2] - u[2] * v[1],
                 u[2] * v[0] - u[0] * v[2],
                 u[0] * v[1] - u[1] * v[0])
        areas.append(math.hypot(*cross) / 2)
        volumes.append(sum(a[i] * cross[i] for i in range(3)) / 6)

    parents = list(range(len(faces)))

    def root(index):
        while index != parents[index]:
            parents[index] = parents[parents[index]]
            index = parents[index]
        return index

    for entries in edges.values():
        for face, _ in entries[1:]:
            parents[root(face)] = root(entries[0][0])

    # Every closed-manifold vertex has one connected cyclic link. This catches
    # two shells touching only at a point, which edge incidence alone misses.
    bad_vertices = 0
    for vertex, adjacent in vertex_faces.items():
        link = defaultdict(list)
        malformed = False
        for face_id in adjacent:
            others = [v for v in faces[face_id] if v != vertex]
            if len(others) != 2 or others[0] == others[1]:
                malformed = True
                continue
            a, b = others
            link[a].append(b)
            link[b].append(a)
        seen = set()
        pending = [next(iter(link))] if link else []
        while pending:
            vertex_id = pending.pop()
            if vertex_id not in seen:
                seen.add(vertex_id)
                pending.extend(link[vertex_id])
        if malformed or any(len(neighbors) != 2 for neighbors in link.values()) \
                or len(seen) != len(link):
            bad_vertices += 1

    boundary = sum(len(entries) == 1 for entries in edges.values())
    nonmanifold = sum(len(entries) > 2 for entries in edges.values())
    inconsistent = sum(len(entries) == 2 and entries[0][1] == entries[1][1]
                       for entries in edges.values())
    duplicates = sum(count - 1 for count in
                     Counter(tuple(sorted(face)) for face in faces).values())
    components = len({root(i) for i in range(len(faces))})
    bounds = [[min(p[i] for p in vertices) for i in range(3)],
              [max(p[i] for p in vertices) for i in range(3)]]
    if not all(math.isfinite(value) for value in areas + volumes):
        raise ValueError("STL coordinates overflow geometry calculations")
    volume = math.fsum(volumes)
    result.update({
        "triangles": len(faces),
        "unique_vertices": len(vertices),
        "bounds_mm": bounds,
        "size_mm": [bounds[1][i] - bounds[0][i] for i in range(3)],
        "volume_mm3": volume,
        "positive_volume": volume > 0,
        "edge_connected_components": components,
        "boundary_edges": boundary,
        "nonmanifold_edges": nonmanifold,
        "nonmanifold_vertices": bad_vertices,
        "inconsistent_winding_edges": inconsistent,
        "duplicate_triangles": duplicates,
        "exact_degenerate_triangles": sum(area == 0 for area in areas),
        "minimum_triangle_area_mm2": min(areas),
        "watertight": boundary == 0 and nonmanifold == 0,
        "consistent_winding": inconsistent == 0,
        "finite_coordinates": True,
    })
    failures = [name for name in (
        "boundary_edges", "nonmanifold_edges", "nonmanifold_vertices",
        "inconsistent_winding_edges", "duplicate_triangles", "exact_degenerate_triangles"
    ) if result[name]]
    if components != 1:
        failures.append("expected_one_component")
    if volume <= 0:
        failures.append("nonpositive_volume")
    result.update(valid=not failures, failures=failures)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stl", type=Path, nargs="+", help="STL files to check; units assumed mm")
    args = parser.parse_args()
    results = []
    for path in args.stl:
        try:
            results.append(audit(path))
        except (OSError, ValueError, UnicodeError, struct.error) as error:
            results.append({"file": str(path.resolve()), "valid": False, "error": str(error)})
    valid = all(result["valid"] for result in results)
    print(json.dumps({"valid": valid, "files": results}, indent=2, allow_nan=False))
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
