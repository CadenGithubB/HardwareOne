"""Orient validated revision-21 meshes without changing their surfaces."""
from pathlib import Path
import hashlib
import json
import sys
import numpy as np

PROJECT = Path(__file__).resolve().parents[2]
WORK = PROJECT / 'work/p4x-enclosure'
sys.path.insert(0, str(WORK / 'mesh-tools'))
import trimesh

SOURCE = PROJECT / 'mechanical/p4x-eye-enclosure/enclosure.scad'
OUT = PROJECT / 'mechanical/p4x-eye-enclosure/stl'
OUT.mkdir(exist_ok=True)
manifest = {
    'revision': 21,
    'units': 'mm',
    'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
    'compiler': 'OpenSCAD 2026.09.23; CGAL all six parts; --hardwarnings',
    'mesh_cleanup': 'Unchanged base: conform_collinear_stl.py previously retriangulated five zero-area faces using existing vertices, without moving coordinates. Midframe retains its revision 20 export; slider retains revision 19. Lid, button strip and new keeper use clean revision 21 exports. The lid is the main R21 CGAL render unioned with the two final source-defined shelf guides via finish-r21-lid.scad; no surface coordinates were altered.',
    'physical_fit_verified': False,
    'parts': [],
}
orientations = {
    'base': 'Floor down',
    'lid': 'Exterior face down; insert openings up',
    'midframe': 'Flat battery-facing underside down',
    'button_strip': 'Actuator stems down; support required below arms, bar and slider retainer',
    'button_keeper': 'Flat underside down; separate rigid keeper',
    'power_slider': 'Nub face down; switch socket upward; supplier may reorient for resin supports',
}
for part, orientation in orientations.items():
    source_name = {'base':'base-cgal-conforming', 'lid':'lid-r21-cgal', 'midframe':'midframe-r20-cgal', 'button_strip':'button_strip-r21-cgal', 'button_keeper':'button_keeper-r21-cgal', 'power_slider':'power_slider-r19-cgal'}.get(part,part)
    original = WORK / f'stl-raw/{source_name}.stl'
    mesh = trimesh.load_mesh(original, process=True)
    assert mesh.is_watertight and mesh.is_winding_consistent and mesh.volume > 0, part
    before_volume = mesh.volume
    transform = np.eye(4)
    if part in ('lid','power_slider'):
        transform[1, 1] = transform[2, 2] = -1
        mesh.apply_transform(transform)
    translation = -mesh.bounds[0]
    mesh.apply_translation(translation)
    transform[:3, 3] = translation
    target = OUT / f'p4x-eye-{part.replace("_", "-")}-r21.stl'
    mesh.export(target, file_type='stl')
    reloaded = trimesh.load_mesh(target, process=True)
    assert reloaded.is_watertight and reloaded.is_winding_consistent, part
    assert (reloaded.area_faces > 1e-12).all(), part
    face_keys = np.sort(reloaded.faces,axis=1)
    assert len(np.unique(face_keys,axis=0)) == len(face_keys), part
    # Edges shared by exactly two faces plus union-find confirms one closed shell.
    edge_keys = np.sort(reloaded.edges,axis=1)
    _, counts = np.unique(edge_keys,axis=0,return_counts=True)
    assert (counts == 2).all(), part
    parents = np.arange(len(reloaded.vertices))
    def root(v):
        while parents[v] != v:
            parents[v] = parents[parents[v]]
            v = parents[v]
        return v
    for a,b in edge_keys:
        ra,rb = root(a),root(b)
        if ra != rb: parents[ra] = rb
    assert len({root(i) for i in range(len(parents))}) == 1, part
    assert abs(reloaded.volume - before_volume) / before_volume < 1e-5, part
    assert np.allclose(reloaded.bounds[0], 0, atol=1e-6), part
    manifest['parts'].append({
        'part': part, 'file': target.name, 'orientation': orientation,
        'triangles': len(reloaded.faces),
        'bounds_mm': reloaded.bounds.tolist(),
        'size_mm': reloaded.extents.tolist(),
        'volume_mm3': float(reloaded.volume),
        'watertight': bool(reloaded.is_watertight),
        'consistent_winding': bool(reloaded.is_winding_consistent),
        'closed_components': 1, 'degenerate_triangles': 0,
        'duplicate_triangles': 0, 'boundary_edges': 0, 'nonmanifold_edges': 0,
        'raw_to_export_transform': transform.tolist(),
        'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
    })
(OUT / 'export-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
