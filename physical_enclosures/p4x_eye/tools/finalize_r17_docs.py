import json
from pathlib import Path
root=Path('mechanical/p4x-eye-enclosure')
m=json.loads((root/'stl/export-manifest.json').read_text())
log=json.loads(Path('work/p4x-enclosure/final-export-log-r17.json').read_text())
assert m==log and m['revision']==17
for x in m['parts']:
 assert x['watertight'] and x['consistent_winding'] and x['closed_components']==1 and x['volume_mm3']>0
 assert all(x[k]==0 for k in ('degenerate_triangles','duplicate_triangles','boundary_edges','nonmanifold_edges'))
p=root/'README.md';s=p.read_text()
s=s.replace('- `p4x-eye-stl-r17.zip`: current four-part package; export validation is pending.','- `p4x-eye-stl-r17.zip`: current four-part prototype package.',1)
start=s.index('The changed revision 17 lid and midframe compiled successfully with CGAL.')
end=s.index('The retained revision 16 ZIP',start)
s=s[:start]+'''All four final revision 17 STL files passed mesh validation: **watertight,
consistent winding, positive volume and one closed component each**, with no
degenerate or duplicate triangles, boundary edges or nonmanifold edges. Each
part's minimum X, Y and Z is zero. The export manifest records the CAD source
hash, mesh hashes, measured sizes and orientation transforms. These checks
establish closed geometry; supplier acceptance and physical fit remain unverified.

The new lid and midframe are a matched pair. Their locating keys do not require
a replacement base or button strip; those two exported meshes have the same
hashes as revision 16.

| Current STL file | Exported size X x Y x Z, mm | Export orientation |
|---|---|---|
| `stl/p4x-eye-base-r17.stl` | 80 x 56 x 22.83 | Floor down |
| `stl/p4x-eye-lid-r17.stl` | 80 x 56 x 14.5 | Exterior face down |
| `stl/p4x-eye-midframe-r17.stl` | 72.35463 x 51.4 x 8.1 | Flat underside down |
| `stl/p4x-eye-button-strip-r17.stl` | 16.2 x 26 x 5.9 | Stems down; supports needed |

The source exports use OpenSCAD 2026.09.23 with `--hardwarnings`: CGAL for the
base, lid and midframe; Manifold for the button strip. Revision 17 final triangle
counts are 30,586 for the base, 59,232 for the lid, 1,992 for the midframe and
3,250 for the button strip. The retained base cleanup retriangulates five
zero-area faces using existing vertices; the strip cleanup removes 342
zero-area faces. Neither cleanup changes dimensions.

'''+s[end:]
s=s.replace('Browser inspection and final export-package validation are pending. Physical\nfit remains unchecked.','Browser inspection is pending. All four final exported STL meshes passed the\nchecks described above; physical fit remains unchecked.',1)
p.write_text(s)
p=root/'stl/PRINTING.md';s=p.read_text()
s=s.replace('are intended to be individually positioned on Z=0; their import positions are\nnot the assembled positions. Revision 17 export validation is pending.','have been individually positioned on Z=0; their import positions are not the\nassembled positions. All four final STL meshes passed the export checks below.',1)
s=s[:s.index('## Export checks')]+'''## Export checks

All four final revision 17 files are watertight single closed-component meshes
with consistent winding and positive volume. Checks found no degenerate or
duplicate triangles, boundary edges, or nonmanifold edges. Each part has
minimum X, Y and Z at zero. `export-manifest.json` records measured dimensions,
orientation transforms, source and mesh hashes, and per-part check results.

The base and button strip match their validated revision 16 meshes exactly.
Their retained cleanup retriangulates five zero-area base faces and removes
342 zero-area button-strip faces, without changing dimensions. Mesh validation
does not establish supplier acceptance, physical fit, or slicer support settings.
'''
p.write_text(s)
p=root/'dimensions.json';j=json.loads(p.read_text())
j['status']='Revision 17 enlarges the LCD flex relief and adds three midframe-owned lid locating keys. All four final oriented STL meshes passed watertightness, winding, positive-volume and single closed-component checks, with zero degenerate/duplicate triangles or boundary/nonmanifold edges. Numerical viewer checks passed; browser inspection pending. Prototype remains unqualified for JLC3DP production or supplier insert installation, and physical fit is unverified.'
j['midframe']['lid_alignment_keys']['validation']='Source coordinate review and numerical viewer checks passed. All four final exported STL meshes passed watertightness, winding, single-component and topology checks. Browser inspection pending; physical fit and supplier acceptance unverified.'
s=j['stl_export'];s.update({
 'status':'All four final oriented revision 17 STL parts passed mesh checks',
 'compiler':m['compiler'],'source_sha256':m['source_sha256'],'mesh_cleanup':m['mesh_cleanup'],
 'hardwarnings':True,'watertight_validation':'All four parts watertight with consistent winding, positive volume and one closed component each; no degenerate or duplicate triangles, boundary edges or nonmanifold edges',
 'backend_by_part':{'base':'CGAL','lid':'CGAL','midframe':'CGAL','button-strip':'Manifold'},
 'manifest_path':'mechanical/p4x-eye-enclosure/stl/export-manifest.json',
 'dimensions_status':'Measured from final oriented revision 17 STL files; detailed bounds and transforms recorded in manifest',
 'base_and_button_strip_reuse':'Exported hashes match revision 16 exactly; no geometry changes',
 'parts':[]})
s.pop('preliminary_changed_part_meshes',None)
for part in m['parts']:
 entry=dict(part)
 entry['part']=entry['part'].replace('_','-')
 entry['path']='mechanical/p4x-eye-enclosure/stl/'+entry.pop('file')
 entry['dimensions_xyz']=entry.pop('size_mm')
 entry['geometry_changed_from_revision_16']=entry['part'] in ['lid','midframe']
 s['parts'].append(entry)
j['manufacturing_review']['candidate_service']='Protolabs offers a candidate SLA epoxy-installed M2 insert and custom clear WaterShed route; final insert geometry, finish and thin-feature acceptance require supplier review.'
j['manufacturing_review']['geometry_action']='Current prototype dimensions preserved; no insert bore adapted to an unknown supplier insert.'
p.write_text(json.dumps(j,indent=2)+'\n')
