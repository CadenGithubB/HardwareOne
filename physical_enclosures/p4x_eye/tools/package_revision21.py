"""Package and verify the approved R21 export set without replacing archives."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import zipfile

root = Path(__file__).resolve().parents[2]
parts = root / 'mechanical/p4x-eye-enclosure'
manifest_path = parts / 'stl/export-manifest.json'
manifest = json.loads(manifest_path.read_text())
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
assert manifest['revision'] == 21
assert manifest['source_sha256'] == sha(parts / 'enclosure.scad')
files = [parts / 'stl' / p['file'] for p in manifest['parts']]
for p, path in zip(manifest['parts'], files):
    assert sha(path) == p['sha256']
    assert p['watertight'] and p['consistent_winding']
    assert p['closed_components'] == 1 and p['volume_mm3'] > 0
    assert all(p[k] == 0 for k in (
        'degenerate_triangles', 'duplicate_triangles', 'boundary_edges', 'nonmanifold_edges'))
files += [parts / 'stl/PRINTING.md', manifest_path,
          parts / 'JLC3DP_REVIEW.md', parts / 'p4x-eye-pcbway-technical-drawing-r21.pdf']
assert len(files) == 10 and len({f.name for f in files}) == 10
out = parts / 'p4x-eye-stl-r21.zip'
assert not out.exists(), 'Preserve existing archives; inspect before replacing.'
with zipfile.ZipFile(out, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for path in files:
        archive.write(path, path.name)
with zipfile.ZipFile(out) as archive:
    assert archive.testzip() is None
    assert set(archive.namelist()) == {p.name for p in files}
    for path in files:
        assert archive.read(path.name) == path.read_bytes()
validation = {
    'revision': 21,
    'checked_at_utc': datetime.now(timezone.utc).isoformat(),
    'archive': str(out.relative_to(root)),
    'archive_bytes': out.stat().st_size,
    'archive_sha256': sha(out),
    'file_count': len(files),
    'crc_check_passed': True,
    'all_member_bytes_match_current_files': True,
    'all_stl_and_source_hashes_match_manifest': True,
    'physical_fit_verified': False,
    'files': [{'file': p.name, 'bytes': p.stat().st_size, 'sha256': sha(p)} for p in files],
}
(root / 'work/p4x-enclosure/package-validation-r21.json').write_text(
    json.dumps(validation, indent=2) + '\n')
print(json.dumps({k: v for k, v in validation.items() if k != 'files'}, indent=2))
