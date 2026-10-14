#!/usr/bin/env python3
"""Whole-part thickness scan of a production mesh set, diffed against a prior set.

For every triangle of every part, a ray is cast from the face centroid inward
along the face normal and the distance to the next surface is recorded. That
is the quantity a supplier's thickness-analysis tool colours, so this catches
thin regions the named section checks never sample. Faces below the threshold
are binned into cells in assembly coordinates; a cell that is thin in the new
revision but not the old one, with real area, is reported as a regression.

Limits: faces near acute convex edges legitimately read small (the ray exits
through the adjacent face), so sub-threshold readings must be inspected, not
treated as certified failures. Degenerate (near-zero-area) triangles are listed
separately. This is a comparison aid, not a strength or deformation model.
Requires NumPy. Example:

  check_thickness_scan.py --old-manifest ../stl/export-manifest-r25.json \
      --new-manifest ../stl/export-manifest-r26.json --output result.json
"""
import argparse
import json
from pathlib import Path

import numpy as np

from check_wall_sections import load_meshes

CHUNK = 128


def scan(tris):
    v0, v1, v2 = tris[:, 0], tris[:, 1], tris[:, 2]
    e1, e2 = v1 - v0, v2 - v0
    n = np.cross(e1, e2)
    area2 = np.linalg.norm(n, axis=1)
    nn = n / np.maximum(area2[:, None], 1e-30)
    cent = tris.mean(axis=1)
    t_min = np.full(len(tris), np.inf)
    for s in range(0, len(tris), CHUNK):
        o = cent[s:s + CHUNK][:, None, :]
        d = -nn[s:s + CHUNK][:, None, :]
        p = np.cross(d, e2[None, :, :])
        det = (p * e1[None, :, :]).sum(-1)
        ok = np.abs(det) > 1e-12
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        tvec = o - v0[None, :, :]
        u = (tvec * p).sum(-1) * inv
        q = np.cross(tvec, e1[None, :, :])
        v = (q * d).sum(-1) * inv
        t = (q * e2[None, :, :]).sum(-1) * inv
        hit = ok & (u >= -1e-9) & (v >= -1e-9) & (u + v <= 1 + 1e-9) & (t > 1e-6)
        t_min[s:s + CHUNK] = np.where(hit, t, np.inf).min(axis=1)
    return t_min, cent, area2 / 2


def analyse(manifest, case_height, case_depth, threshold, cell, degenerate_area):
    _, raw, hashes = load_meshes(manifest, case_height, case_depth)
    result = {}
    for part, tri_list in raw.items():
        tris = np.array(tri_list, dtype=float)
        volume = np.einsum('ij,ij->i', tris[:, 0], np.cross(tris[:, 1], tris[:, 2])).sum() / 6
        if volume < 0:  # load_meshes mirrors the lid and slider; restore outward normals
            tris = tris[:, ::-1]
            volume = -volume
        t, cent, area = scan(tris)
        finite = np.isfinite(t)
        thin = finite & (t < threshold)
        cells = {}
        for i in np.nonzero(thin)[0]:
            key = ','.join(str(int(k)) for k in np.floor(cent[i] / cell))
            c = cells.setdefault(key, {'min_thickness_mm': float('inf'), 'area_mm2': 0.0, 'faces': 0,
                                       'bbox_mm': [cent[i].tolist(), cent[i].tolist()]})
            c['min_thickness_mm'] = min(c['min_thickness_mm'], float(t[i]))
            c['area_mm2'] += float(area[i]); c['faces'] += 1
            c['bbox_mm'] = [np.minimum(c['bbox_mm'][0], cent[i]).round(3).tolist(),
                            np.maximum(c['bbox_mm'][1], cent[i]).round(3).tolist()]
        degenerate = np.nonzero(area < degenerate_area)[0]
        result[part] = {
            'sha256': hashes[part], 'triangles': int(len(tris)), 'volume_mm3': float(volume),
            'area_mm2': float(area.sum()), 'rays_without_hit': int((~finite).sum()),
            'min_thickness_mm': float(t[finite].min()),
            'area_below_mm2': {f'{th:.1f}': float(area[finite & (t < th)].sum()) for th in (0.3, 0.5, 0.8, 1.0)},
            'degenerate_faces': [{'centroid_mm': cent[i].round(3).tolist(), 'area_mm2': float(area[i]),
                                  'thickness_mm': (float(t[i]) if np.isfinite(t[i]) else None)} for i in degenerate],
            'thin_cells': cells,
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--old-manifest', type=Path, required=True)
    parser.add_argument('--new-manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--threshold-mm', type=float, default=1.0)
    parser.add_argument('--cell-mm', type=float, default=2.0)
    parser.add_argument('--regression-tolerance-mm', type=float, default=0.05,
                        help='a new cell counts as a regression only if it reads this much below the threshold')
    parser.add_argument('--regression-min-area-mm2', type=float, default=0.05,
                        help='ignore new cells with less summed face area than this (edge/sliver noise)')
    parser.add_argument('--degenerate-area-mm2', type=float, default=1e-6)
    parser.add_argument('--case-height-mm', type=float, default=56)
    parser.add_argument('--case-depth-mm', type=float, default=29)
    args = parser.parse_args()
    old = analyse(args.old_manifest, args.case_height_mm, args.case_depth_mm, args.threshold_mm, args.cell_mm, args.degenerate_area_mm2)
    new = analyse(args.new_manifest, args.case_height_mm, args.case_depth_mm, args.threshold_mm, args.cell_mm, args.degenerate_area_mm2)
    diff, regressions = {}, []
    for part in new:
        o, n = old[part], new[part]
        new_cells = {k: v for k, v in n['thin_cells'].items() if k not in o['thin_cells']}
        thinner = {k: {'old_min_thickness_mm': o['thin_cells'][k]['min_thickness_mm'], **v}
                   for k, v in n['thin_cells'].items()
                   if k in o['thin_cells'] and v['min_thickness_mm'] < o['thin_cells'][k]['min_thickness_mm'] - args.regression_tolerance_mm}
        for k, v in list(new_cells.items()) + list(thinner.items()):
            if v['min_thickness_mm'] < args.threshold_mm - args.regression_tolerance_mm and v['area_mm2'] >= args.regression_min_area_mm2:
                regressions.append({'part': part, 'cell': k, **v})
        diff[part] = {'identical_mesh': o['sha256'] == n['sha256'],
                      'thin_cells_old': len(o['thin_cells']), 'thin_cells_new': len(n['thin_cells']),
                      'new_thin_cells': new_cells, 'cells_thinner_than_before': thinner,
                      'resolved_thin_cells': sorted(k for k in o['thin_cells'] if k not in n['thin_cells'])}
    report = {'old_revision': json.loads(args.old_manifest.read_text())['revision'],
              'new_revision': json.loads(args.new_manifest.read_text())['revision'],
              'method': 'inward normal ray from each face centroid; cells of cell_mm in assembly coordinates',
              'threshold_mm': args.threshold_mm, 'cell_mm': args.cell_mm,
              'regression_criteria': {'below_threshold_by_mm': args.regression_tolerance_mm, 'minimum_cell_area_mm2': args.regression_min_area_mm2},
              'scope': 'Comparison of sampled face thickness between two mesh sets; acute-edge readings need inspection; not a strength, deformation or global-minimum certification.',
              'old': old, 'new': new, 'diff': diff, 'regressions': regressions, 'no_regressions': not regressions}
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'{"part":14s} {"rev":>3s} {"tris":>6s} {"min t":>7s} {"<0.3":>8s} {"<0.5":>8s} {"<0.8":>8s} {"<1.0":>8s}  (area mm2 below each)')
    for part in new:
        for rev, r in ((report['old_revision'], old[part]), (report['new_revision'], new[part])):
            a = r['area_below_mm2']
            print(f'{part:14s} {rev:3d} {r["triangles"]:6d} {r["min_thickness_mm"]:7.3f} {a["0.3"]:8.2f} {a["0.5"]:8.2f} {a["0.8"]:8.2f} {a["1.0"]:8.2f}'
                  + ('' if r['rays_without_hit'] == 0 else f'  [{r["rays_without_hit"]} rays without hit]')
                  + ('' if not r['degenerate_faces'] else f'  [{len(r["degenerate_faces"])} degenerate faces]'))
    for part, d in diff.items():
        print(f'{part}: identical={d["identical_mesh"]} thin cells {d["thin_cells_old"]}->{d["thin_cells_new"]}; '
              f'new={len(d["new_thin_cells"])} thinner={len(d["cells_thinner_than_before"])} resolved={len(d["resolved_thin_cells"])}')
    print(f'regressions: {len(regressions)}')
    for r in regressions:
        print(f'  {r["part"]} cell {r["cell"]} min={r["min_thickness_mm"]:.3f} area={r["area_mm2"]:.2f} bbox={r["bbox_mm"]}')
    return 0 if not regressions else 1


if __name__ == '__main__':
    raise SystemExit(main())
