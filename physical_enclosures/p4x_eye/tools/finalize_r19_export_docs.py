import hashlib,json
from pathlib import Path
root=Path('mechanical/p4x-eye-enclosure');m=json.loads((root/'stl/export-manifest.json').read_text());assert m['revision']==19
for x in m['parts']:
 assert x['watertight'] and x['consistent_winding'] and x['volume_mm3']>0 and x['closed_components']==1
 assert all(x[k]==0 for k in ['degenerate_triangles','duplicate_triangles','boundary_edges','nonmanifold_edges'])
 assert hashlib.sha256((root/'stl'/x['file']).read_bytes()).hexdigest()==x['sha256']
p=root/'dimensions.json';j=json.loads(p.read_text())
old={x['part']:x for x in j['previous_stl_export_revision_18']['parts']}
for x in m['parts']:
 if x['part'] in ['base','midframe']:assert old[x['part']]['sha256']==x['sha256']
j['status']='Revision 19 adds a captive printed power slider, a smaller covered lid slot and integral button-strip retainer. All five final STL meshes passed watertightness, winding, positive-volume and single-component topology checks with zero degenerate/duplicate triangles or boundary/nonmanifold edges. Lid, strip and slider are new; base/midframe hashes match revision 18. Viewer and PDF visual review in progress; physical fit and supplier acceptance unverified.'
s=j['stl_export'];s.update({'status':'All five final oriented revision 19 STL parts passed mesh checks','compiler':m['compiler'],'source_sha256':m['source_sha256'],'mesh_cleanup':m['mesh_cleanup'],'hardwarnings':True,'watertight_validation':'All five parts watertight with consistent winding, positive volume and one closed component each; no degenerate or duplicate triangles, boundary edges or nonmanifold edges','backend_by_part':{x['part'].replace('_','-'):'CGAL' for x in m['parts']},'manifest_path':'mechanical/p4x-eye-enclosure/stl/export-manifest.json','dimensions_status':'Measured from final oriented revision 19 STL files; detailed bounds and transforms recorded in manifest','reused_part_verification':'Base and midframe hashes match revision 18 exactly','parts':[]})
for x in m['parts']:
 entry=dict(x);entry['part']=entry['part'].replace('_','-');entry['path']='mechanical/p4x-eye-enclosure/stl/'+entry.pop('file');entry['dimensions_xyz']=entry.pop('size_mm');entry['geometry_changed_from_revision_18']=entry['part'] not in ['base','midframe'];s['parts'].append(entry)
j['power_slider']['validation']='Final revision 19 slider mesh passed watertightness, winding, positive-volume, single-component and topology checks. Four-page PDF text/dimensions match CAD and manifest; viewer and visual PDF checks in progress; physical fit unverified.'
j['power_switch_access']['validation']='Final revision 19 lid mesh checks and PDF text/dimension audit passed; viewer, PDF visual inspection and physical engagement/load-path checks remain pending.'
j['button_actuators']['power_slider_retainer']['validation']='Final retainer-equipped button strip mesh checks passed; physical strength/operation unverified and viewer/PDF visual inspection pending.'
j['technical_drawing']['status']='Four-sheet PDF generated; independent text, dimensions, coordinates and hash check passed; visual review in progress';j['technical_drawing']['pages']=4;j['technical_drawing'].pop('planned_pages',None);j['technical_drawing']['text_dimension_check']='All five filenames/hash prefixes and envelope dimensions match r19 manifest. Exported lid switch center63.073/44.800, running slot9x4.4 and underside guide17x8.2 match CAD. Six insert coordinates, directions and depths retained. Slider page dimensions,3mm travel,.3side gaps,.4total float,2mm socket/1.5mm lever,1.2roof and1/1.6overlaps checked.'
p.write_text(json.dumps(j,indent=2)+'\n')
p=root/'README.md';s=p.read_text();s=s.replace('- `p4x-eye-stl-r19.zip`: current five-part prototype package; export checks in progress.','- `p4x-eye-stl-r19.zip`: current five-part prototype package.',1);s=s.replace('- `p4x-eye-pcbway-technical-drawing-r19.pdf`: four-sheet quotation drawing in\n  preparation, including the new slider and its retaining shelf. Current insert','- `p4x-eye-pcbway-technical-drawing-r19.pdf`: four-sheet quotation drawing\n  including the new slider and retaining shelf. Text, dimensions and hashes\n  have been checked; rendered visual review is in progress. Current insert',1)
a=s.index('Revision 19 **export, mesh, viewer and drawing checks');b=s.index('All four revision 18 meshes',a)
s=s[:a]+'''All five final revision 19 STL meshes passed validation: **watertight,
consistent winding, positive volume and one closed component each**, with no
degenerate or duplicate triangles, boundary edges or nonmanifold edges. Each
mesh has minimum X, Y and Z at zero. The manifest records source/mesh hashes,
measured sizes and orientation transforms. Viewer and PDF visual inspections
are in progress; physical fit and supplier acceptance remain unverified.

Use the new lid, retainer-equipped button strip and separate power slider as
a matched set. Base and midframe mesh hashes match revision 18 exactly, and
all six M2 case/frame fastenings retain their positions.

| Current STL file | Exported size X x Y x Z, mm | Export orientation |
|---|---|---|
| `stl/p4x-eye-base-r19.stl` | 80 x 56 x 22.83 | Floor down |
| `stl/p4x-eye-lid-r19.stl` | 80 x 56 x 14.5 | Exterior face down |
| `stl/p4x-eye-midframe-r19.stl` | 72.35463 x 51.4 x 8.1 | Flat underside down |
| `stl/p4x-eye-button-strip-r19.stl` | 22.77320 x 31.3 x 5.9 | Stems down; supports under arms, bar and retainer |
| `stl/p4x-eye-power-slider-r19.stl` | 14 x 7.6 x 4.6 | Nub face down; socket upward |

The source exports use OpenSCAD 2026.09.23 with CGAL and `--hardwarnings`.
Final triangle counts are 30,586 base, 59,780 lid, 1,676 midframe, 3,256 button
strip and 972 power slider. The base retains its previous cleanup of five
zero-area faces using existing vertices without moving coordinates; the
midframe retains its clean revision 18 export. Lid, strip and slider use clean
revision 19 CGAL exports.

'''+s[b:]
s=s.replace('Delivery meshes are intended to sit at Z=0','Delivery meshes sit at Z=0',1);p.write_text(s)
p=root/'stl/PRINTING.md';s=p.read_text().replace('are intended to be positioned on Z=0; their import positions are not the\nassembled positions. Revision 19 export checks are in progress.','have been positioned on Z=0; their import positions are not the assembled\npositions. All five final STL files passed the export checks below.',1)
s=s.replace('| Power slider | To be recorded with final export |','| Power slider | Nub face down; socket upward |',1)
a=s.index('## Export checks');s=s[:a]+'''## Export checks

All five final revision 19 meshes are watertight with consistent winding,
positive volume and one closed component each. There are no degenerate or
duplicate triangles, boundary edges or nonmanifold edges. Each mesh starts
at minimum X, Y and Z of zero. `export-manifest.json` records dimensions,
orientation transforms, source/mesh hashes and individual check results.

Base and midframe hashes match revision 18. The base retains its prior cleanup
of five zero-area faces using existing vertices without moving coordinates;
the frame is unchanged. Lid, strip and power slider are clean revision 19
CGAL exports. All five output hashes were checked against the manifest.

Four-page drawing text and dimensions were independently checked against the
CAD and manifest. Viewer and rendered PDF inspection are in progress. Physical
fit, supplier acceptance and support settings remain unverified. The supplier
may reorient parts for resin supports; preserve the socket and guide surfaces.
''';p.write_text(s)
p=root/'JLC3DP_REVIEW.md';s=p.read_text().replace('Revision 19 five-part export, viewer and four-page drawing validation are in progress.','All five revision 19 STL meshes passed watertightness, winding, positive-volume and single-component topology checks. Four-page drawing text/dimensions were checked; viewer and rendered drawing inspections are in progress.',1);p.write_text(s)
