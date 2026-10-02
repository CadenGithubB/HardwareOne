import copy, json
from pathlib import Path
root=Path('mechanical/p4x-eye-enclosure')
p=root/'README.md'
s=p.read_text().replace('Revision 16, 2026-09-27.', 'Revision 17, 2026-09-28.',1)
s=s.replace('USB ports and microSD slot. The detachable camera is omitted. Revision 16 adds\na joined battery-plug passage and wire route through the midframe.', '''USB ports and microSD slot. The detachable camera is omitted. Revision 17
widens the LCD flex relief to **24 x 6.35 mm**, with rounded corners and an
extra **0.5 mm of upward clearance**. Three locating keys on the midframe
slide into blind pockets in the lid to resist lateral movement and twisting.
The existing four case screws hold the cover closed. **Reprint the lid and
midframe together**; the revision 16 base and button strip can be reused.

The revision 16 joined battery-plug passage and wire route remain in the midframe.''',1)
s=s.replace('- `p4x-eye-stl-r16.zip`: package of the four separate revision 16 STL parts.','- `p4x-eye-stl-r17.zip`: current four-part package; export validation is pending.\n- `p4x-eye-stl-r16.zip`: retained prior revision, before the flex relief and lid keys.',1)
start=s.index('All four revision 16 parts were exported')
end=s.index('An inline 3D viewer was added',start)
s=s[:start]+'''Revision 17 CAD and viewer changes are in progress. **Export and mesh validation
are pending**; no revision 17 print-readiness claim is made here yet. The new
lid and midframe are a matched pair. Their locating keys do not require a
replacement base or button strip.

| Current STL file | Intended export orientation |
|---|---|
| `stl/p4x-eye-base-r17.stl` | Floor down; same geometry as revision 16 |
| `stl/p4x-eye-lid-r17.stl` | Exterior face down; enlarged flex relief and receiver pockets |
| `stl/p4x-eye-midframe-r17.stl` | Flat underside down; three locating keys added |
| `stl/p4x-eye-button-strip-r17.stl` | Stems down; same geometry as revision 16; supports needed |

The retained revision 16 ZIP contains four separate STL files, its printing
notes and export manifest. All four revision 16 meshes passed watertightness,
consistent winding and single closed-component checks, with no degenerate or
duplicate triangles, boundary edges or nonmanifold edges. The base and lid
were exported with CGAL; the frame and strip used Manifold, through the
SHA-verified official OpenSCAD 2026.09.23 Mac snapshot. Five zero-area base
faces were retriangulated using existing vertices; 342 zero-area button-strip
faces were removed without changing dimensions. Those historical checks do
not validate the changed revision 17 lid and frame.

Delivery meshes are positioned at Z=0 and intended for import as separate
objects in millimeters at 100% scale. Review `stl/PRINTING.md` and slice for the
selected printer and material; physical fit, insert installation, key return
and printed strength remain unverified.

'''+s[end:]
s=s.replace('Revision 16 export validation is recorded above; physical fit remains unchecked.','Revision 16 export validation is recorded above; revision 17 viewer and export\nvalidation are pending. Physical fit remains unchecked.',1)
s=s.replace('''module's rear frame perimeter. A **22 mm flex relief** occupies
**X=19-41, Y=6.65-11.5, Z=25.8-27.5**. Confirm the actual module has suitable
rear frame bearing areas and that the cable clears this relief.''','''module's rear frame perimeter. The revision 17 **24 x 6.35 mm flex relief**
occupies **X=18-42, Y=5.9-12.25, Z=25.8-28.0**, with **R0.6 corners**.
Compared with revision 16, this adds 1 mm on each side, 0.75 mm at each end,
and 0.5 mm upward for the cable bend. The relieved faceplate region retains
**1 mm to the exterior surface at Z29**. The LCD pocket, flush seating depth
and remaining support ledge stay unchanged. Confirm the actual module has
suitable rear frame bearing areas and that its cable clears the relief.''',1)
needle="The upper-cover part's local Z=0 corresponds to case Z=14.5."
section='''Three **midframe-owned locating keys** rise **2.5 mm above the plate**, from
Z14.5 to Z17.0, and enter matching blind receivers inside the lid. Each is
8 mm long and 1.5 mm thick, at these case XY bounds:

| Key | X span, mm | Y span, mm |
|---|---|---|
| Left | 2.3-3.8 | 20-28 |
| Front | 40-48 | 2.3-3.8 |
| Rear | 24-32 | 52.2-53.7 |

The tips taper inward **0.3 mm per side over the upper 0.4 mm**. Each pocket
has **0.2 mm clearance per side**, **0.3 mm above the key**, and a lead-in that
widens by another **0.2 mm per side over its first 0.4 mm**. The lid's receiver
blocks span **Z14.5-17.8** and join its interior wall; the pockets open downward.
Their XY rectangles are **(1.5,18.8,3.5,10.4)**,
**(38.8,1.5,10.4,3.5)** and **(22.8,51,10.4,3.5)**, expressed as X, Y, width,
depth. The exterior shell and its existing wall thicknesses remain unchanged.

These three separated keys locate the lid laterally and resist twisting;
the four case screws provide closure. The cover still installs and lifts
straight vertically. The receivers stop 1.9 mm below the PCB underside and
remain outside its XY outline, with nominal gaps of 0.5 mm at the left and
1.5 mm at the front and rear. They avoid the USB saddle, wheel, cable channel
and frame screw heads. Check the printed sliding fit before installing the
display; do not use the case screws to pull a binding key into its pocket.

'''
s=s.replace(needle,section+needle,1)
s=s.replace('''frame screws at the front-left and rear. Connect and route the LCD flex, then lower
the cover past the wheel to close the USB openings.''','''frame screws at the front-left and rear. Connect and route the LCD flex, then lower
the cover straight down over the three frame keys and past the wheel to close
the USB openings. Confirm all three keys enter freely and the seam closes.''',1)
p.write_text(s)
p=root/'stl/PRINTING.md'
s=p.read_text().replace('# Revision 16 — separate enclosure parts','# Revision 17 — separate enclosure parts',1)
s=s.replace('have been individually positioned on Z=0; their import positions are not the\nassembled positions. Print one of each file.','are intended to be individually positioned on Z=0; their import positions are\nnot the assembled positions. Revision 17 export validation is pending.\n\n**Reprint the lid and midframe together.** The base and button-strip geometry\nis unchanged from revision 16, so those existing parts can be reused. The\nrevision 16 STL files and ZIP are retained separately.',1)
s=s.replace('Inspect support needs around the display opening and R2 edge.','Inspect support needs around the display opening, receiver pockets and R2 edge.',1)
s=s.replace('Includes the battery cable opening and connected wire channel.','Includes the battery cable route and three upward locating keys.',1)
s=s.replace('## Battery cable','''## Lid alignment and screen cable

The midframe carries three 8 x 1.5 mm locating keys that rise 2.5 mm above its
plate. Chamfered tips enter blind pockets in the lid with 0.2 mm nominal
clearance per side and 0.3 mm above each key. Fit the lid and frame together
before installing electronics. The lid should lower vertically and seat freely;
the case screws secure it after the keys have located it. Do not draw a binding
key into its pocket by tightening the screws.

The LCD flex relief is now **24 x 6.35 mm**, with R0.6 corners and 0.5 mm more
upward clearance than revision 16. It retains a 1 mm skin beneath the exterior
face. Check the cable bend with the screen seated; the LCD pocket and flush
seat height are unchanged.

## Battery cable''',1)
s=s[:s.index('## Export checks')]+'''## Export checks

Revision 17 export and mesh checks are pending. Consult the final export
manifest before printing the new files. The retained revision 16 meshes passed
single closed-component, watertightness, winding and topology checks; those
historical results do not validate the new lid and midframe. Physical fit and
slicer support settings remain unverified.
'''
p.write_text(s)
p=root/'dimensions.json'
j=json.loads(p.read_text())
j['previous_stl_export_revision_16']=copy.deepcopy(j['stl_export'])
j['previous_viewer_validation_revision_16']=j['viewer_validation']
j['revision']=17
j['status']='Revision 17 enlarges the LCD flex relief to 24 x 6.35 mm with R0.6 corners and Z28 top, and adds three midframe-owned locating keys with matching blind lid pockets. Reprint lid and midframe together; base and button-strip geometry is unchanged from revision 16. Viewer, CAD export and mesh validation pending; physical fit unverified.'
j['viewer_validation']='Revision 17 viewer validation pending. Historical revision 16 results retained separately; those do not validate the changed keys, receivers and LCD flex relief.'
j['separator']+=' Revision 17 adds three upward keys locating the lid in matching blind receivers.'
j['stack_z_spans']['lid_alignment_keys']=[14.5,17]
j['stack_z_spans']['lid_alignment_receivers']=[14.5,17.8]
j['midframe']['status']='Provisional removable plate with independent blind PCB mount, two M2 x 8 mm insert-based case fixings, support seats, filled PCB locator, battery cable route and three lid-locating keys; physical fit unverified'
j['midframe']['verification_needed'].append('Three 0.2 mm-clearance lid locating pockets, chamfered key entry, vertical assembly and no binding before case screws are tightened')
j['midframe']['assembly_sequence']=j['midframe']['assembly_sequence'].replace('lower cover past wheel and close USB notches','lower cover vertically over its three frame keys and past wheel to close USB notches; confirm free key entry and a closed seam')
j['midframe']['former_capture_tabs']='Earlier capture tabs remain removed. Revision 17 adds upward locating keys that permit straight vertical cover removal.'
j['midframe']['lid_alignment_keys']={
 'introduced_revision':17,'owner':'midframe','receiver_owner':'lid','function':'Locate cover laterally and resist twisting; four case screws retain cover axially',
 'key_rects_xywh':[[2.3,20,1.5,8],[40,2.3,8,1.5],[24,52.2,8,1.5]],'key_nominal_z_span':[14.5,17],
 'key_height':2.5,'union_overlap_into_plate':0.01,'tip_chamfer_xy_per_side':0.3,'tip_chamfer_height':0.4,
 'receiver_rects_xywh':[[1.5,18.8,3.5,10.4],[38.8,1.5,10.4,3.5],[22.8,51,10.4,3.5]],'receiver_z_span':[14.5,17.8],
 'pocket_clearance_per_side':0.2,'pocket_top_clearance':0.3,'pocket_top_case_z':17.3,
 'mouth_flare_additional_clearance_per_side':0.2,'mouth_flare_height':0.4,'receiver_inboard_wall_at_flared_entry_minimum_nominal':0.8,
 'receiver_xy_clearance_to_pcb_outline':{'left':0.5,'front':1.5,'rear':1.5},'receiver_top_to_pcb_underside':1.9,
 'assembly':'Lower cover straight down; chamfered keys enter downward-open blind pockets. Verify free fit before tightening case screws.',
 'geometry_scope':'Exterior profile, outer wall thicknesses, base, battery/cable positions, USB saddle, wheel and button strip unchanged.',
 'print_update':'Replace lid and midframe as a pair; retain revision 16 base and button strip if already printed.',
 'validation':'Source coordinate review only; compiled meshes, viewer checks and physical fit pending.'}
j['lcd_access']['previous_flex_relief_revision_16']=copy.deepcopy(j['lcd_access']['flex_relief'])
j['lcd_access']['flex_relief']={'x_span':[18,42],'y_span':[5.9,12.25],'z_span':[25.8,28], 'width':24,'depth':6.35,'corner_radius_xy':0.6,'remaining_top_skin_outside_lcd_pocket':1,'change_from_revision_16':{'width_addition_each_side':1,'depth_addition_each_end':0.75,'upward_clearance_addition':0.5},'scope':'LCD module pocket, flush seat height and remaining support ledge unchanged; physical cable bend clearance requires checking.'}
j['lid_and_other_access']+=' Revision 17 enlarges the LCD flex relief and adds three midframe-owned lid locating keys, with downward-open blind pockets in added lid receiver ribs. Reprint the lid and midframe as a pair; the base and button strip retain revision 16 geometry.'
prev=j['previous_stl_export_revision_16']
j['stl_export']={'revision':17,'units':'mm','status':'Export and mesh validation pending','package_path':'mechanical/p4x-eye-enclosure/p4x-eye-stl-r17.zip','directory':'mechanical/p4x-eye-enclosure/stl/','printing_notes':'mechanical/p4x-eye-enclosure/stl/PRINTING.md','minimum_xyz_for_delivery':[0,0,0],'physical_fit_verified':False,'slicer_support_setup_validated':False,'gcode_provided':False,'parts':[{'part':x['part'],'path':x['path'].replace('-r16.stl','-r17.stl'),'orientation':x['orientation'],'geometry_changed_from_revision_16':x['part'] in ['lid','midframe']} for x in prev['parts']],'watertight_validation':'Pending','reprint_required':['lid','midframe'],'reusable_revision_16_parts':['base','button-strip'],'historical_export':'previous_stl_export_revision_16'}
p.write_text(json.dumps(j,indent=2)+'\n')
