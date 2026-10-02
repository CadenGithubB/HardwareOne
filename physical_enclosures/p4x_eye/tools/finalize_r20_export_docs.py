import hashlib,json
from pathlib import Path
root=Path('mechanical/p4x-eye-enclosure');m=json.loads((root/'stl/export-manifest.json').read_text());assert m['revision']==20
for x in m['parts']:
 assert x['watertight'] and x['consistent_winding'] and x['volume_mm3']>0 and x['closed_components']==1
 assert all(x[k]==0 for k in ['degenerate_triangles','duplicate_triangles','boundary_edges','nonmanifold_edges'])
 assert hashlib.sha256((root/'stl'/x['file']).read_bytes()).hexdigest()==x['sha256']
p=root/'dimensions.json';j=json.loads(p.read_text());old={x['part']:x for x in j['previous_stl_export_revision_19']['parts']}
for x in m['parts']:
 if x['part'] in ['base','button_strip','power_slider']:assert x['sha256']==old[x['part'].replace('_','-')]['sha256']
j['status']='Revision 20 adds a lid-owned PCB anti-lift stop with 0.2 mm nominal gap and relocates the lower USB-side support away from RESET. All five final STL meshes passed watertightness, winding, positive-volume and single-component topology checks; four-page drawing dimensions/hashes and rendered pages passed inspection. Browser/numerical checks in progress. Only lid/midframe changed; physical fit, contact pressure and supplier acceptance unverified.'
s=j['stl_export'];s.update({'status':'All five final oriented revision 20 STL parts passed mesh checks','compiler':m['compiler'],'source_sha256':m['source_sha256'],'mesh_cleanup':m['mesh_cleanup'],'hardwarnings':True,'watertight_validation':'All five parts watertight with consistent winding, positive volume and one closed component each; no degenerate or duplicate triangles, boundary edges or nonmanifold edges','backend_by_part':{x['part'].replace('_','-'):'CGAL' for x in m['parts']},'manifest_path':'mechanical/p4x-eye-enclosure/stl/export-manifest.json','dimensions_status':'Measured from final oriented revision 20 STL files; detailed bounds and transforms recorded in manifest','reused_part_verification':'Base, button strip and power slider hashes match revision 19 exactly','parts':[]})
for x in m['parts']:
 entry=dict(x);entry['part']=entry['part'].replace('_','-');entry['path']='mechanical/p4x-eye-enclosure/stl/'+entry.pop('file');entry['dimensions_xyz']=entry.pop('size_mm');entry['geometry_changed_from_revision_19']=entry['part'] in ['lid','midframe'];s['parts'].append(entry)
f=j['midframe'];f['status']='Revision 20 lower USB-side support relocated to clear RESET; final mesh checks passed. Numerical/browser checks in progress; physical fit, contact pressure and load support unverified.'
seat=f['board_edge_seats']['usb_side_seat'];seat['bearing_surface']='Solder-mask bearing land containing tented vias; not unperforated bare FR4';seat['validation']='Nominal plan clearances reviewed against source placements, Gerbers, masks and drills; final mesh and PDF checks passed. Current viewer checks and physical fit remain pending.'
stop=j['pcb_anti_lift_stop'];stop['bearing_surface']='Component-free display-face solder-mask patch; no drill centers, component bodies, mask openings or paste openings found within the reviewed upper footprint';stop['validation']='Final lid mesh and four-page PDF dimensions/hash/rendered checks passed. Current viewer checks pending; physical gap/contact pressure and support loads unverified.'
j['manufacturing_review']['geometry_concerns_for_review']=[x.replace('has0.275','has 0.275').replace('and0.29','and 0.29') for x in j['manufacturing_review']['geometry_concerns_for_review']]
j['technical_drawing'].update({'status':'All four PDF pages rendered and visually inspected; independent text/dimension/coordinate/hash audit passed against CAD and final manifest. S1 anti-lift callout and new lower-seat note visible.','pages':4,'text_dimension_check':'S1 exported72.50/13.70/Z7.50 maps assembly72.5/42.3/Z21.5;3x1.2foot and0.20gap match. Lower-seat assembly X69.30-70.50,Y32.75-34.75,topZ19.70 matches source. Five filenames/hash identities, part envelopes and six unchanged insert positions/directions/depths checked.','physical_fit_verified':False});j['technical_drawing'].pop('planned_pages',None)
p.write_text(json.dumps(j,indent=2)+'\n')
p=root/'README.md';s=p.read_text();s=s.replace('- `p4x-eye-stl-r20.zip`: current five-part prototype package; export checks in progress.','- `p4x-eye-stl-r20.zip`: current five-part prototype package.',1)
s=s.replace('- `p4x-eye-pcbway-technical-drawing-r20.pdf`: four-sheet quotation drawing in\n  preparation for the anti-lift stop and relocated lower support. Existing\n  insert pilots remain provisional pending supplier hardware confirmation.','- `p4x-eye-pcbway-technical-drawing-r20.pdf`: four-sheet quotation drawing with\n  the anti-lift stop and relocated lower support. Text, dimensions and file\n  hashes were independently checked; all four rendered pages passed visual\n  inspection. Insert pilots remain provisional pending supplier hardware confirmation.',1)
a=s.index('Revision 20 **STL, viewer and drawing checks');b=s.index('The retained revision 19 five-part meshes',a)
s=s[:a]+'''All five final revision 20 STL meshes passed validation: **watertight,
consistent winding, positive volume and one closed component each**, with no
degenerate or duplicate triangles, boundary edges or nonmanifold edges. Each
mesh has minimum X, Y and Z at zero. The manifest records sizes, transforms,
source and mesh hashes. All four quotation-drawing pages passed independent
text/dimension/hash checks and rendered visual inspection. Numerical viewer
and browser checks are in progress. Physical fit and supplier acceptance remain
unverified.

Use the new lid and midframe as a matched set. Base, button strip and power
slider hashes match revision 19 exactly. Six M2 case/frame fastenings retain
their positions.

| Current STL file | Exported size X x Y x Z, mm | Export orientation |
|---|---|---|
| `stl/p4x-eye-base-r20.stl` | 80 x 56 x 22.83 | Floor down |
| `stl/p4x-eye-lid-r20.stl` | 80 x 56 x 14.5 | Exterior face down |
| `stl/p4x-eye-midframe-r20.stl` | 72.35463 x 51.4 x 8.1 | Flat underside down |
| `stl/p4x-eye-button-strip-r20.stl` | 22.77320 x 31.3 x 5.9 | Stems down; supports needed |
| `stl/p4x-eye-power-slider-r20.stl` | 14 x 7.6 x 4.6 | Nub down; socket up |

OpenSCAD 2026.09.23 CGAL exports used `--hardwarnings`. Final triangle counts
are 30,586 base, 60,324 lid, 1,668 midframe, 3,256 strip and 972 slider. The
base retains its prior cleanup of five zero-area faces using existing vertices
without moving coordinates. Strip and slider retain clean revision 19 meshes;
lid and frame are clean revision 20 exports.

'''+s[b:]
s=s.replace('Delivery meshes are intended to sit at Z=0','Delivery meshes sit at Z=0',1)
s=s.replace('**2.4 mm² nominal bearing area** in a **1.2 x 2 mm** section.','**2.4 mm² nominal bearing area** in a **1.2 x 2 mm** section. This lower\ncontact is a solder-mask bearing land containing tented vias; it is not\nunperforated bare FR4.',1)
s=s.replace('The display-face contact region is a **solder-mask bearing land with\ntented vias**, identified from the board placement, Gerber, mask and drill\nsources; it is not unperforated bare FR4.','The upper display-face contact is a **component-free solder-mask patch**.\nThe reviewed footprint has no drill centers, component bodies, mask openings\nor paste openings in the placement, Gerber, mask and drill sources. The\ntented vias noted above belong to the separate lower support land.',1)
p.write_text(s)
p=root/'stl/PRINTING.md';s=p.read_text().replace('are intended to be positioned on Z=0; their import positions are not the\nassembled positions. Revision 20 export checks are in progress.','have been positioned on Z=0; their import positions are not the assembled\npositions. All five final STL files passed the export checks below.',1)
s=s.replace('bearing land at X71-74, Y41.7-42.9 has solder mask and tented vias; inspect the\nactual board before assembly.','bearing land at X71-74, Y41.7-42.9 is component-free solder mask, with no\ndrill centers or mask/paste openings found in the source review; inspect the\nactual board before assembly.',1)
s=s.replace('1.2 x 2 mm section. It remains separate from the upper stop.','1.2 x 2 mm section. This lower solder-mask land contains tented vias. It\nremains separate from the upper stop.',1)
a=s.index('## Export checks');s=s[:a]+'''## Export checks

All five final revision 20 meshes are watertight with consistent winding,
positive volume and one closed component each. Checks found no degenerate or
duplicate triangles, boundary edges or nonmanifold edges. Each mesh begins
at minimum X, Y and Z of zero. The manifest records sizes, orientation
transforms, source/mesh hashes and per-part results.

Base, button strip and slider hashes match revision 19. The base retains its
previous cleanup of five zero-area faces using existing vertices without
moving coordinates; lid and midframe are clean revision 20 CGAL exports.
All four drawing pages passed independent text/dimension/hash checks and
rendered visual inspection. Numerical viewer and browser checks are in
progress. Physical fit, PCB preload clearance, operating forces, supplier
acceptance and support settings remain unverified.
''';p.write_text(s)
p=root/'JLC3DP_REVIEW.md';s=p.read_text().replace('Revision 20 STL, numerical/viewer and four-page drawing checks are in progress.','All five final revision 20 STL meshes passed topology checks and all four drawing pages passed independent dimension/hash and rendered visual checks. Numerical viewer/browser checks remain in progress.',1)
s=s.replace('The display-face bearing land is covered\nby solder mask and includes tented vias; it is not unperforated bare FR4.','The upper display-face bearing patch is component-free solder mask: no drill\ncenters, component bodies or mask/paste openings were found inside the reviewed\nfootprint.',1)
s=s.replace('2.4 mm² area and a 1.2 × 2 mm section. The upper stop and relocated lower seat','2.4 mm² area and a 1.2 × 2 mm section. This separate lower bearing land includes\ntented vias beneath solder mask; it is not unperforated bare FR4. The upper\nstop and relocated lower seat',1);p.write_text(s)
