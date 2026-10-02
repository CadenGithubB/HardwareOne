import json,hashlib
from pathlib import Path
root=Path('mechanical/p4x-eye-enclosure'); m=json.loads((root/'stl/export-manifest.json').read_text());assert m['revision']==18
for part in m['parts']:
 assert part['watertight'] and part['consistent_winding'] and part['closed_components']==1 and part['volume_mm3']>0
 assert all(part[k]==0 for k in ['degenerate_triangles','duplicate_triangles','boundary_edges','nonmanifold_edges'])
 assert hashlib.sha256((root/'stl'/part['file']).read_bytes()).hexdigest()==part['sha256']
p=root/'README.md';s=p.read_text();s=s.replace('- `p4x-eye-stl-r18.zip`: current four-part prototype package; exports and validation in progress.','- `p4x-eye-stl-r18.zip`: current four-part prototype package.',1)
s=s.replace('- `p4x-eye-pcbway-technical-drawing-r18.pdf`: updated quotation drawing in progress,\n  to match the revision 18 STLs. Current insert pilots remain provisional until\n  the supplier identifies its hardware and approves any required changes.','- `p4x-eye-pcbway-technical-drawing-r18.pdf`: three-sheet quotation drawing\n  matched to the revision 18 STLs. Text, dimensions, coordinates and file hashes\n  have been independently checked; visual review is in progress. Current insert\n  pilots remain provisional until the supplier identifies its hardware and\n  approves any required changes.',1)
a=s.index('Revision 18 source changes are complete;'); b=s.index('The retained revision 17 STL files',a)
s=s[:a]+'''All four final revision 18 STL files passed mesh validation: **watertight,
consistent winding, positive volume and one closed component each**, with no
degenerate or duplicate triangles, boundary edges or nonmanifold edges. Each
part's minimum X, Y and Z is zero. The export manifest records source and mesh
hashes, measured sizes and orientation transforms. Mesh validity does not
establish physical fit or supplier acceptance. Viewer and PDF visual review
are still in progress.

Reprint the lid, midframe and shortened button strip as a set. The base and six
M2 case/frame fastening positions remain unchanged; the final base mesh hash
matches revision 17 exactly.

| Current STL file | Exported size X x Y x Z, mm | Export orientation |
|---|---|---|
| `stl/p4x-eye-base-r18.stl` | 80 x 56 x 22.83 | Floor down |
| `stl/p4x-eye-lid-r18.stl` | 80 x 56 x 14.5 | Exterior face down |
| `stl/p4x-eye-midframe-r18.stl` | 72.35463 x 51.4 x 8.1 | Flat underside down |
| `stl/p4x-eye-button-strip-r18.stl` | 16.2 x 21.5 x 5.9 | Stems down; supports needed |

The exports use OpenSCAD 2026.09.23 with CGAL and `--hardwarnings`. Final
triangle counts are 30,586 for the base, 59,644 for the lid, 1,676 for the
midframe and 2,572 for the button strip. The unchanged base retains its prior
cleanup of five zero-area faces using existing vertices without moving
coordinates. The other parts are clean revision 18 CGAL exports.

'''+s[b:]
s=s.replace('Delivery meshes are intended to be positioned at Z=0 and imported as separate','Delivery meshes are positioned at Z=0 and should be imported as separate',1)
s=s.replace('reported. Those are historical revision 17 results; revision 18 validation is\nin progress.','reported. Those are historical revision 17 viewer results; revision 18 browser\nvalidation is in progress.',1);p.write_text(s)
p=root/'stl/PRINTING.md';s=p.read_text();s=s.replace('are intended to be individually positioned on Z=0; their import positions are\nnot the assembled positions. Revision 18 export checks are in progress.','have been individually positioned on Z=0; their import positions are not the\nassembled positions. All four final STL meshes passed the export checks below.',1)
a=s.index('## Export checks');s=s[:a]+'''## Export checks

All four final revision 18 meshes are watertight, have consistent winding and
positive volume, and each has one closed component. Checks found no degenerate
or duplicate triangles, boundary edges or nonmanifold edges. Minimum X, Y and
Z are zero for every part. `export-manifest.json` records measured sizes,
orientation transforms, source and mesh hashes, and per-part results.

The unchanged base matches its revision 17 mesh hash. Its prior cleanup
retriangulated five zero-area faces using existing vertices without moving
coordinates. Lid, midframe and strip use clean revision 18 CGAL exports.
Viewer and PDF visual review are in progress. Physical fit, supplier acceptance
and slicer support settings remain unverified. The manufacturer remains
undecided; Protolabs was ruled out by the user on price.
''';p.write_text(s)
p=root/'JLC3DP_REVIEW.md';s=p.read_text().replace('Revision 18 exports and validation are in progress; historical revision 17 topology checks do not validate these changes.','All four final revision 18 STL meshes passed watertightness, winding, positive-volume and single-component topology checks; viewer and drawing visual review are in progress.',1);p.write_text(s)
p=root/'dimensions.json';j=json.loads(p.read_text());j['status']='Revision 18 adds an open front/right midframe cable bay and direct SW7 access, with relocated PCB seat and shortened button strip. All four final oriented STL meshes passed watertightness, winding, positive-volume and single-component checks, with zero degenerate/duplicate triangles or boundary/nonmanifold edges. Reprint lid, frame and strip; base unchanged. Viewer and drawing visual review in progress; physical fit and supplier acceptance unverified.'
s=j['stl_export'];s.update({'status':'All four final oriented revision 18 STL parts passed mesh checks','compiler':m['compiler'],'source_sha256':m['source_sha256'],'mesh_cleanup':m['mesh_cleanup'],'hardwarnings':True,'watertight_validation':'All four parts watertight with consistent winding, positive volume and one closed component each; no degenerate or duplicate triangles, boundary edges or nonmanifold edges','backend_by_part':{x:'CGAL' for x in ['base','lid','midframe','button-strip']},'manifest_path':'mechanical/p4x-eye-enclosure/stl/export-manifest.json','dimensions_status':'Measured from final oriented revision 18 STL files; detailed bounds and transforms recorded in manifest','base_reuse':'Final mesh hash matches revision 17 exactly','parts':[]})
for part in m['parts']:
 entry=dict(part);entry['part']=entry['part'].replace('_','-');entry['path']='mechanical/p4x-eye-enclosure/stl/'+entry.pop('file');entry['dimensions_xyz']=entry.pop('size_mm');entry['geometry_changed_from_revision_17']=entry['part']!='base';s['parts'].append(entry)
f=j['midframe'];f['status']='Provisional removable plate with three lid keys, blind PCB mount and open front-right battery cable bay; front PCB support at X60-63. Final revision 18 STL mesh checks passed; viewer and physical fit remain unverified.'
f['battery_cable_passage']['fit_status']='Final revision 18 STL mesh checks passed; viewer review in progress. Physical route, bend radius and retention require validation.'
f['lid_alignment_keys']['validation']='Geometry retained from revision 17. All four final revision 18 STL mesh checks passed; current viewer review and physical fit pending.'
j['power_switch_access']['local_reference']='work/p4x-enclosure/reference/sw7-K3-1235S-F1.pdf';j['power_switch_access']['validation']='Final revision 18 lid mesh checks passed. PDF text/dimension cross-check agrees with CAD and exported coordinates. Viewer, visual drawing review and physical switch access/travel pending.'
j['technical_drawing']['status']='Three-sheet PDF generated; independent text/dimension/coordinate/hash check passed, visual review in progress';j['technical_drawing']['text_dimension_check']='All four filenames/hash prefixes and part bounds match final r18 manifest. Lid SW7 coordinate applies exported Y=56-caseY. Insert XY/entry directions, pilot3.4 and total blind8.0/7.0 depths match CAD; six M2 inserts and supplier-review caveats retained.'
p.write_text(json.dumps(j,indent=2)+'\n')
