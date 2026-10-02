import copy,json
from pathlib import Path
root=Path('mechanical/p4x-eye-enclosure')
p=root/'README.md';s=p.read_text().replace('Revision 17, 2026-09-28.','Revision 18, 2026-09-28.',1)
a=s.index('USB ports and microSD slot.'); b=s.index("Revision 15's local rounded scallop",a)
s=s[:a]+'''USB ports and microSD slot. The detachable camera is omitted. Revision 18
replaces the narrow battery-cable strips with **one open bay at the front-right
of the midframe** and adds a **12 x 5 mm power-switch opening** in the lid,
flared to 14 x 7 mm at its outer face. The front button-strip anchor moves to
clear that opening. **Reprint the lid, midframe and button strip together**;
the revision 17 base can be reused.

The revision 17 **24 x 6.35 mm LCD flex relief** and three midframe locating
keys are retained. The keys slide into blind lid pockets to resist lateral
movement and twisting, while the existing four case screws hold it closed.
The battery cable bay is cut by an R1 rectangle at **(64.5,-1)** with size
**16.5 x 29.65 mm**, extending through the plate's front and right edges. The
midframe still covers approximately **91.3% of the battery's XY footprint**.
Its front PCB seat moves another 5 mm left, to X60-63, to clear the larger bay.

'''+s[b:]
s=s.replace('except at its cable-entry notch','except at its open cable bay',1)
a=s.index('- `p4x-eye-stl-r17.zip`:');b=s.index('An inline 3D viewer was added',a)
s=s[:a]+'''- `p4x-eye-stl-r18.zip`: current four-part prototype package; exports and validation in progress.
- `p4x-eye-pcbway-technical-drawing-r18.pdf`: updated quotation drawing in progress,
  to match the revision 18 STLs. Current insert pilots remain provisional until
  the supplier identifies its hardware and approves any required changes.
- `p4x-eye-stl-r17.zip` and `p4x-eye-pcbway-technical-drawing-r17.pdf`: retained
  prior revision, before the open cable bay and power-switch access.
- `p4x-eye-stl-r16.zip`: retained prior revision, before the flex relief and lid keys.
- `stl/`: individual STL files, export manifest and `PRINTING.md` notes.

Revision 18 source changes are complete; **current export, mesh, viewer and PDF
checks are in progress**. Reprint the lid, midframe and shortened button strip
as a set. The base and six M2 case/frame fastening positions remain unchanged.

| Current STL file | Intended export orientation |
|---|---|
| `stl/p4x-eye-base-r18.stl` | Floor down; revision 17 base can be reused |
| `stl/p4x-eye-lid-r18.stl` | Exterior face down; new power opening and shifted strip anchor |
| `stl/p4x-eye-midframe-r18.stl` | Flat underside down; open cable bay and relocated front support |
| `stl/p4x-eye-button-strip-r18.stl` | Stems down; shortened anchor bar; supports needed |

The retained revision 17 STL files passed watertightness, winding, positive
volume and single closed-component checks, with no degenerate or duplicate
triangles, boundary edges or nonmanifold edges. Its ZIP contents and hashes
were checked, and its quotation drawing was rendered and independently
reviewed. Those historical checks do not validate the changed revision 18
parts or drawing. Prior revision 16 exports are also retained.

Delivery meshes are intended to be positioned at Z=0 and imported as separate
objects in millimeters at 100% scale. Review `stl/PRINTING.md` and slice for the
selected printer and material; physical fit, insert installation, key return
and printed strength remain unverified.

'''+s[b:]
s=s.replace('reported. All four final exported STL meshes passed the checks described above;\nphysical fit remains unchecked.','reported. Those are historical revision 17 results; revision 18 validation is\nin progress. Physical fit remains unchecked.',1)
a=s.index('Revision 16 cuts a connected cable route'); b=s.index('The Mitsumi SIQ-02FVS3 drawing',a)
s=s[:a]+'''Revision 18 replaces the former plug opening, wire slot and entry relief with
**one R1 rounded cable bay** through the **Z13-14.5 mm** plate. Its cutter spans
**X=64.5-81, Y=-1-28.65**, extending through the plate's front and right edges.
This removes the narrow strips around the previous route, leaving a single
open space near J25. Approximately **91.3% of the battery's XY footprint**
remains beneath solid midframe; this area figure is not a measured clamping
load. The outer case, battery floor and four screw wells remain closed.

Orient the battery's lead corner toward **(73.5,6)**, bring the lead into the
open bay, and connect to J25 while accessible. Keep slack inside the case and
away from the PCB supports and screws. Do not squeeze the wire through the
**0.5 mm gap under the remaining plate** or the narrow screw-well clearance.
The reported plug width is 5 mm; its other dimensions, wire bend radius and
physical routing still require checking.

'''+s[b:]
s=s.replace('**X=60.5-65.5, Y=12-38, Z=25.8-26.6**.','**X=60.5-65.5, Y=16.5-38, Z=25.8-26.6**, shortened to 21.5 mm along Y. ',1)
s=s.replace('Two strip screws are centered at **(63,14)** and **(63,36)**.','Two strip screws are centered at **(63,19)** and **(63,36)**. Revision 18\nmoves the first anchor 5 mm toward positive Y to clear the power-switch opening.',1)
needle='Three pressable top buttons are enabled independently'
section='''Revision 18 provides direct access to **SW7**, the board's power switch, at
case XY **(63.0732,11.2)**. The lid opening is **12 x 5 mm with R1 corners**,
at **X=57.0732-69.0732, Y=8.7-13.7**, cut from Z26.8 through the outer face.
An outward bevel from **Z28.2 to Z29.0** widens the mouth to **14 x 7 mm,
R2**, at **X=56.0732-70.0732, Y=7.7-14.7**. A fingernail can reach the
original slider; no separate printed actuator is added.

The board placement and HRO **K3-1235S-F1** manufacturer drawing locate the
switch and specify a **9 x 3.5 x 3.5 mm body**, a **1.5 mm-square handle** and
**2 mm travel along X**. The F1 drawing shows a **2 mm handle height**, while
the board BOM says **2.5 mm**. Its actual installed height is therefore
unverified: the modeled handle top is Z26.8, or Z27.3 if the BOM height applies,
versus the Z29 cover face. Check physical reach and full travel through the
opening. [HRO switch drawing](https://static.chipdip.ru/lib/844/DOC012844376.pdf)

'''
s=s.replace(needle,section+needle,1)
s=s.replace('''plug through the **7 x 8 mm opening**, bring the lead through the rounded
entry relief, and lay it in the connected **3 mm-wide channel**. Connect it''','''lead into the **open front-right cable bay** and connect it to J25''',1)
s=s.replace('**(10,43.1), (66.5,7.0) and (72.5,42.5)**. The front seat and stop now span\n**X=65-68**, moved 4 mm left to clear the battery-wire channel; the seat stays','**(10,43.1), (61.5,7.0) and (72.5,42.5)**. The front seat and stop now span\n**X=60-63**, moved another 5 mm left in revision 18 to clear the open cable bay;\nthe seat stays',1)
s=s.replace('head fit, driver access and no bottoming before tightening. Check that each key','head fit, driver access and no bottoming before tightening. Check SW7 access\nand full slider travel. Check that each key',1)
p.write_text(s)
p=root/'stl/PRINTING.md';s=p.read_text().replace('# Revision 17 — prototype enclosure parts','# Revision 18 — prototype enclosure parts',1)
s=s.replace('have been individually positioned on Z=0; their import positions are not the\nassembled positions. All four final STL meshes passed the export checks below.','are intended to be individually positioned on Z=0; their import positions are\nnot the assembled positions. Revision 18 export checks are in progress.',1)
s=s.replace('**Reprint the lid and midframe together.** The base and button-strip geometry\nis unchanged from revision 16, so those existing parts can be reused. The\nrevision 16 STL files and ZIP are retained separately.','**Reprint the lid, midframe and button strip together.** The base geometry is\nunchanged from revision 17, so an existing base can be reused. Prior revision\n17 STL files, ZIP and quotation drawing are retained separately.',1)
s=s.replace('Includes the battery cable route and three upward locating keys.','Includes the open front-right cable bay and three upward locating keys.',1)
s=s.replace('Requires support under the arms and anchor bar; inspect the small contact areas before printing.','Shortened anchor bar; front fixing moved to (63,19). Inspect supports below arms and bar.',1)
a=s.index('## Battery cable');b=s.index('## Hardware and assembly',a)
s=s[:a]+'''## Battery cable and power access

The midframe's former small opening and wire slot are replaced by **one open
front-right bay** with R1 corners. Its cutter spans X64.5-81, Y-1-28.65 and
opens through the front and right plate edges, removing the thin strips.
Approximately 91.3% of the battery's XY footprint remains under the plate.
The base floor and all four case-screw wells stay closed.

Orient the battery's lead corner toward case XY **(73.5,6)** and route the
lead upward through the open bay to J25. Keep slack inside the case and clear
of screws, the wheel and PCB supports. Do not trap wires in the 0.5 mm gap
beneath the remaining plate. The front PCB seat is now X60-63, with its
previous Y and Z placement retained.

SW7 has a **12 x 5 mm R1 lid opening**, widening to a **14 x 7 mm R2 mouth**
with an outward bevel. Reach the original power slider with a fingernail;
there is no extra actuator part. The drawing and BOM disagree on handle
height (2 versus 2.5 mm), so check actual reach and the full 2 mm slider travel
before ordering or final assembly. The front button-strip anchor moved from
(63,14) to **(63,19)**, and its bar now spans Y16.5-38. Use the revision 18 lid
and strip together.

'''+s[b:]
a=s.index('## Export checks');s=s[:a]+'''## Export checks

Revision 18 export, mesh and viewer validation are in progress. The previous
revision 17 meshes and drawing passed their documented checks; those results
do not establish the revised parts' validity. Final results will be recorded
in `export-manifest.json`. Physical fit, supplier acceptance and slicer support
settings remain unverified. The manufacturer remains undecided; Protolabs was
ruled out by the user on price.
''';p.write_text(s)
p=root/'JLC3DP_REVIEW.md';s=p.read_text().replace('# Revision 17 manufacturing review','# Revision 18 manufacturing review',1)
s=s.replace('The four STL meshes pass topology checks; this does not establish material strength, printed fit, finish quality or supplier acceptance.','Revision 18 exports and validation are in progress; historical revision 17 topology checks do not validate these changes. Mesh integrity does not establish material strength, printed fit, finish quality or supplier acceptance.',1)
s=s.replace('The assembled body is 80 × 56 × 29 mm, or 30 mm including raised buttons. Revision 17 enlarges the LCD-flex relief to 24 × 6.35 mm and adds three midframe keys with matching blind lid receivers. The bottom remains closed.','The assembled body is 80 × 56 × 29 mm, or 30 mm including raised buttons. Revision 18 opens the midframe cable bay through its front/right edges, relocates its front PCB support to X60-63, and adds direct SW7 access through a 12 × 5 mm lid opening with a 14 × 7 mm beveled mouth. Its front button-strip anchor moves to (63,19), shortening the bar to Y16.5-38. The revision 17 LCD-flex relief and three locating keys remain. The bottom remains closed. Reprint the lid, midframe and button strip; the base and six M2 insert locations are unchanged.',1)
s=s.replace('| PCB/button anchor pilots | Ø1.2 mm | Review blind-hole formation, screw engagement and coating blockage. |','| PCB/button anchor pilots | Ø1.2 mm | Review blind-hole formation, screw engagement and coating blockage. |\n| Open cable bay | R1 cutter X64.5-81, Y-1-28.65 | Review midframe stiffness and battery retention; approximately 91.3% of battery footprint remains covered. |\n| Power-switch access | 12 × 5 mm R1 throat; 14 × 7 mm R2 mouth | Confirm actual handle height, full 2 mm travel and fingernail reach after finishing. |',1)
s=s.replace('## Inserts and clear finish','The HRO K3-1235S-F1 drawing specifies a 2 mm slider handle height, while the board BOM states 2.5 mm. The installed height and access feel remain unverified. [HRO switch drawing](https://static.chipdip.ru/lib/844/DOC012844376.pdf)\n\n## Inserts and clear finish',1)
p.write_text(s)
p=root/'dimensions.json';j=json.loads(p.read_text());j['previous_stl_export_revision_17']=copy.deepcopy(j['stl_export']);j['previous_viewer_validation_revision_17']=j['viewer_validation'];j['revision']=18
j['status']='Revision 18 replaces the three-part cable route with a front/right-open midframe bay, moves its front PCB support to X60-63, adds beveled direct SW7 access in the lid and shifts/shortens the button strip anchor bar. Reprint lid, midframe and strip; base unchanged. Current export, viewer and drawing checks are in progress; physical fit and supplier acceptance unverified.'
j['viewer_validation']='Revision 18 validation in progress; historical revision 17 results retained separately.'
j['separator']='Removable 1.5 mm midframe plate at Z13-14.5, with open front-right battery cable bay, integral blind PCB mount, two independent case fixings, three lid-locating keys, sparse board seats and solid notch locator. USB saddle belongs to base.'
f=j['midframe'];f['previous_battery_cable_passage_revision_17']=copy.deepcopy(f['battery_cable_passage']);f['status']='Provisional removable plate with three lid keys, blind PCB mount and open front-right battery cable bay; front PCB support relocated to X60-63. Revision 18 validation in progress; physical fit unverified.'
f['board_edge_seats']['display_case_xy'][1]=[61.5,7.0]
f['board_edge_seats']['front_seat_xy_bounds']['x']=[60,63];f['board_edge_seats']['front_stop_xy_bounds']['x']=[60,63]
f['board_edge_seats']['front_support_move_x_from_revision_17']=-5;f['board_edge_seats']['front_support_move_reason']='Clear the larger revision 18 cable bay while preserving seat and stop Y/Z bounds'
f['verification_needed']=[('Battery cable routing through open bay, bend radius, edge clearance and retention without pinching beneath the remaining plate' if 'reported 5 mm battery plug fit through' in x else x) for x in f['verification_needed']]
f['verification_needed'].append('SW7 direct access, actual installed handle height and full 2 mm slider travel')
f['assembly_sequence']=f['assembly_sequence'].replace('feed its plug through the 7 x 8 mm passage and lead through the rounded entry relief into the connected 3 mm-wide channel','route the lead through the open front-right cable bay toward J25').replace('button rest gaps and free movement','button rest gaps, free movement and full SW7 power-slider travel')
f['outer_perimeter']['construction']='Actual lower-interior outline offset inward by 0.3 mm, with reliefs around four upper insert pads; revision 18 cable bay opens through front and right edges and removes narrow cable-route strips.'
f['battery_cable_passage']={'introduced_revision':18,'construction':'One rounded rectangular through-cut opens midframe to front and right; former three joined cuts and intervening narrow strips removed. Base floor, outer shell and screw wells remain closed.','plate_z_span':[13,14.5],'user_plug_reported_width':5,'cutter_xy':[64.5,-1],'cutter_size_xy':[16.5,29.65],'corner_radius':1,'cutter_xy_bounds':{'x':[64.5,81],'y':[-1,28.65]},'actual_opening':'Cutter intersected with actual plate perimeter; opens front/right edges','battery_lead_corner_case_xy':[73.5,6],'battery_footprint_covered_percent_approximate':91.3,'coverage_note':'Plan area only, not verified retention load or stiffness','route':'Bring lead from battery corner into open bay to J25; keep slack inside case, clear of screws/supports and out of the 0.5 mm gap beneath remaining plate.','fit_status':'Revision 18 checks in progress; physical route, bend radius and retention require validation.'}
f['lid_alignment_keys']['validation']='Geometry retained from revision 17; current revision 18 export/viewer validation in progress.'
j['lid_and_other_access']+=' Revision 18 opens the battery cable bay through the front/right frame edges, moves its front PCB support left by 5 mm, adds a beveled SW7 opening and moves the front strip fixing to (63,19). New lid, frame and strip are a matched set; base unchanged.'
b=j['button_actuators'];b['previous_mounting_bar_revision_17']=copy.deepcopy(b['mounting_bar']);b['previous_strip_fixings_revision_17']=copy.deepcopy(b['strip_fixings']);b['mounting_bar']['y_span']=[16.5,38];b['mounting_bar']['length_y']=21.5;b['strip_fixings']['case_xy']=[[63,19],[63,36]];b['revision_18_change']='Front fixing moves from (63,14) to (63,19), bar shortened from Y12-38 to Y16.5-38 to clear power-switch opening. Keys, stems, return arms and switch gaps unchanged.'
j['power_switch_access']={'introduced_revision':18,'ref':'SW7','part':'HRO K3-1235S-F1','manufacturer_drawing_url':'https://static.chipdip.ru/lib/844/DOC012844376.pdf','local_reference':'work/ref/sw7-K3-1235S-F1.pdf','gerber_top_xy':[11.4268,4.7],'case_xy':[63.0732,11.2],'pcb_side':'Display-facing','body_size_xyz':[9,3.5,3.5],'body_base_case_z':21.3,'body_top_case_z':24.8,'handle_size_xy':[1.5,1.5],'slider_travel_x':2,'handle_height_drawing':2,'handle_height_bom':2.5,'installed_handle_top_case_z_provisional':[26.8,27.3],'height_status':'Manufacturer F1 drawing and board BOM disagree; solder/installed height and reach unverified','opening_size_xy':[12,5],'opening_corner_radius':1,'opening_xy_bounds':{'x':[57.0732,69.0732],'y':[8.7,13.7]},'opening_cutter_z_span':[26.8,29.1],'bevel_z_span_nominal':[28.2,29],'mouth_size_xy':[14,7],'mouth_corner_radius':2,'mouth_xy_bounds':{'x':[56.0732,70.0732],'y':[7.7,14.7]},'actuation':'Direct fingernail access to original slider; no additional printed actuator','button_bar_and_anchor_clearance_to_mouth_y':1.8,'validation':'Revision 18 CAD/export/viewer checks in progress; physical access and slider travel unverified'}
prev=j['previous_stl_export_revision_17'];j['stl_export']={'revision':18,'units':'mm','status':'Export and mesh validation in progress','package_path':'mechanical/p4x-eye-enclosure/p4x-eye-stl-r18.zip','directory':'mechanical/p4x-eye-enclosure/stl/','printing_notes':'mechanical/p4x-eye-enclosure/stl/PRINTING.md','minimum_xyz_for_delivery':[0,0,0],'physical_fit_verified':False,'slicer_support_setup_validated':False,'gcode_provided':False,'parts':[{'part':x['part'],'path':x['path'].replace('-r17.stl','-r18.stl'),'orientation':x['orientation'],'geometry_changed_from_revision_17':x['part']!='base'} for x in prev['parts']],'watertight_validation':'In progress','reprint_required':['lid','midframe','button-strip'],'reusable_revision_17_parts':['base'],'historical_export':'previous_stl_export_revision_17'}
j['manufacturing_review']['current_revision']=18;j['manufacturing_review']['geometry_action']='Revision 18 prototype geometry adds open cable bay and direct power access. Six M2 insert positions and pilots remain unchanged; supplier insert geometry is still unknown.'
j['manufacturing_review']['geometry_concerns_for_review']+=['Open cable bay frame stiffness and battery retention, with approximately 91.3% plan coverage','SW7 handle height discrepancy and direct access after finish']
j['technical_drawing']={'revision':18,'path':'mechanical/p4x-eye-enclosure/p4x-eye-pcbway-technical-drawing-r18.pdf','status':'Updated drawing and validation in progress','purpose':'Quotation and supplier review; current insert pilots not approved supplier-installation dimensions','previous_revision_path':'mechanical/p4x-eye-enclosure/p4x-eye-pcbway-technical-drawing-r17.pdf'}
p.write_text(json.dumps(j,indent=2)+'\n')
