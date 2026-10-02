#!/usr/bin/env python3
"""Read-only STL checks. Uses exact coordinate welding, never mesh repair.

Run with the bundled Python 3.12 runtime. The local mesh-tools directory
supplies trimesh and numpy; scipy and networkx are not required.
Reports edge-connected shells as well as vertex connectivity so point-only
contacts cannot silently appear to be one printable component.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "mesh-tools"))

import numpy as np
import trimesh

PARTS = ("base.stl", "lid.stl", "midframe.stl", "button_strip.stl")


class UnionFind:
    def __init__(self, count: int):
        self.parent = list(range(count))
        self.size = [1] * count

    def find(self, item: int) -> int:
        while self.parent[item] != item:
            self.parent[item] = self.parent[self.parent[item]]
            item = self.parent[item]
        return item

    def join(self, first: int, second: int) -> None:
        a, b = self.find(int(first)), self.find(int(second))
        if a == b:
            return
        if self.size[a] < self.size[b]:
            a, b = b, a
        self.parent[b] = a
        self.size[a] += self.size[b]

    def labels(self) -> np.ndarray:
        return np.array([self.find(i) for i in range(len(self.parent))])


def connected_faces(faces: np.ndarray, edges: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Return face-component labels for shared edges and for shared vertices."""
    edge_groups = UnionFind(len(faces))
    vertex_groups = UnionFind(len(faces))
    canonical = np.sort(edges, axis=1)
    order = np.lexsort((canonical[:, 1], canonical[:, 0]))
    ordered = canonical[order]
    shared = np.flatnonzero(np.all(ordered[1:] == ordered[:-1], axis=1))
    for index in shared:
        edge_groups.join(order[index] // 3, order[index + 1] // 3)
    first_face: dict[int, int] = {}
    for face_index, face in enumerate(faces):
        for vertex in face:
            vertex = int(vertex)
            if vertex in first_face:
                vertex_groups.join(first_face[vertex], face_index)
            else:
                first_face[vertex] = face_index
    return edge_groups.labels(), vertex_groups.labels()


def inspect(path: Path, area_tolerance: float) -> dict:
    digest_before = hashlib.sha256(path.read_bytes()).hexdigest()
    raw = trimesh.load_mesh(path, file_type="stl", process=False)
    if not isinstance(raw, trimesh.Trimesh):
        raise ValueError("STL did not load as one triangle mesh")
    raw_vertices = np.asarray(raw.vertices, dtype=np.float64)
    raw_faces = np.asarray(raw.faces, dtype=np.int64)
    if not len(raw_faces) or not np.isfinite(raw_vertices).all():
        raise ValueError("Mesh is empty or contains non-finite coordinates")

    # STL repeats vertices per triangle. Merge only exactly identical values;
    # do not quantize, delete triangles, heal edges, or reorient the mesh.
    vertices, inverse = np.unique(raw_vertices, axis=0, return_inverse=True)
    faces = inverse[raw_faces]
    mesh = trimesh.Trimesh(vertices=vertices, faces=faces, process=False)
    triangles = vertices[faces]
    areas = np.linalg.norm(
        np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0]),
        axis=1,
    ) / 2.0
    repeated = np.any(faces == np.roll(faces, 1, axis=1), axis=1)
    degenerate = repeated | (areas <= area_tolerance)
    canonical_faces = np.sort(faces, axis=1)
    duplicate_count = len(faces) - len(np.unique(canonical_faces, axis=0))
    edges = faces[:, [0, 1, 1, 2, 2, 0]].reshape((-1, 2))
    _, edge_counts = np.unique(np.sort(edges, axis=1), axis=0, return_counts=True)
    edge_labels, vertex_labels = connected_faces(faces, edges)
    components = []
    for label in np.unique(edge_labels):
        selected = faces[edge_labels == label]
        used = np.unique(selected)
        component = trimesh.Trimesh(vertices=vertices, faces=selected, process=False)
        bounds = np.stack((vertices[used].min(axis=0), vertices[used].max(axis=0)))
        components.append({
            "faces": len(selected),
            "vertices": len(used),
            "watertight": bool(component.is_watertight),
            "consistent_winding": bool(component.is_winding_consistent),
            "signed_volume_mm3": float(component.volume),
            "bounds_mm": bounds.tolist(),
            "dimensions_mm": (bounds[1] - bounds[0]).tolist(),
        })
    components.sort(key=lambda item: item["faces"], reverse=True)
    digest_after = hashlib.sha256(path.read_bytes()).hexdigest()
    volume = float(mesh.volume)
    failures = []
    checks = {
        "watertight": bool(mesh.is_watertight),
        "consistent_winding": bool(mesh.is_winding_consistent),
        "positive_volume": bool(np.isfinite(volume) and volume > 0),
        "no_degenerate_triangles": not bool(degenerate.any()),
        "no_duplicate_triangles": duplicate_count == 0,
        "single_connected_component": len(components) == 1,
        "file_unchanged": digest_before == digest_after,
    }
    failures = [name for name, passed in checks.items() if not passed]
    return {
        "file": str(path.resolve()),
        "sha256": digest_before,
        "file_bytes": path.stat().st_size,
        "faces": len(faces),
        "unique_vertices": len(vertices),
        "checks": checks,
        "signed_volume_mm3": volume,
        "bounds_mm": mesh.bounds.tolist(),
        "dimensions_mm": mesh.extents.tolist(),
        "minimum_triangle_area_mm2": float(areas.min()),
        "degenerate_triangles": int(degenerate.sum()),
        "exact_zero_area_triangles": int((areas == 0).sum()),
        "repeated_vertex_triangles": int(repeated.sum()),
        "duplicate_triangles": duplicate_count,
        "boundary_edges": int((edge_counts == 1).sum()),
        "nonmanifold_edges": int((edge_counts > 2).sum()),
        "edge_connected_components": len(components),
        "vertex_connected_components": len(np.unique(vertex_labels)),
        "components": components,
        "failures": failures,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path, default=HERE / "stl-raw")
    parser.add_argument("--area-tolerance", type=float, default=1e-12, help="mm²")
    args = parser.parse_args()
    results = []
    for name in PARTS:
        path = args.directory / name
        try:
            results.append(inspect(path, args.area_tolerance))
        except Exception as error:
            results.append({"file": str(path.resolve()), "error": str(error), "failures": ["load_or_check_error"]})
    passed = all(not item["failures"] for item in results)
    report = {
        "all_passed": passed,
        "versions": {"python": sys.version.split()[0], "numpy": np.__version__, "trimesh": trimesh.__version__},
        "coordinate_units": "mm (assumed from the OpenSCAD source; STL has no unit metadata)",
        "method": "Exact-coordinate welding in memory only; no repairs or file changes. Edge components are surface shells; multiple shells require inspection to distinguish voids from separate bodies.",
        "not_checked": ["triangle self-intersections", "physical tolerances", "printer-specific manufacturability"],
        "triangle_area_tolerance_mm2": args.area_tolerance,
        "meshes": results,
    }
    print(json.dumps(report, indent=2, allow_nan=False))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
