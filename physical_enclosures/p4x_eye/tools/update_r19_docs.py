import copy,json
from pathlib import Path
root=Path('mechanical/p4x-eye-enclosure')
p=root/'README.md';s=p.read_text().replace('Revision 18, 2026-09-28.','Revision 19, 2026-09-28.',1)
a=s.index('USB ports and microSD slot.');b=s.index('The revision 17 **24 x 6.35 mm',a)
s=s[:a]+'''USB ports and microSD slot. The detachable camera is omitted. Revision 19
adds a **captive printed power slider** over SW7, replacing the exposed
fingernail opening with a smaller running slot covered from beneath by the
slider flange. Its raised nub is **1 mm above the lid**, matching the buttons.
A retaining shelf is integrated into the button strip and held by that strip's
existing two screws. **Print the new lid, button strip and power slider**;
the revision 18 base and midframe can be reused. There are now **five printed
parts** in the complete set.

'''+s[b:]
s=s.replace('Its front PCB seat moves another 5 mm left, to X60-63, to clear the larger bay.','Its front PCB seat remains at X60-63, clearing the open bay.',1)
s=s.replace('for assembly, exploded view or individual parts, including `button_strip`.','for assembly, exploded view or individual parts, including `button_strip`\n  and the new `power_slider`.',1)
a=s.index('- `p4x-eye-stl-r18.zip`:');b=s.index('An inline 3D viewer was added',a)
s=s[:a]+'''- `p4x-eye-stl-r19.zip`: current five-part prototype package; export checks in progress.
- `p4x-eye-pcbway-technical-drawing-r19.pdf`: four-sheet quotation drawing in
  preparation, including the new slider and its retaining shelf. Current insert
  pilots remain provisional until the supplier identifies its hardware and
  approves any required changes.
- `p4x-eye-stl-r18.zip` and `p4x-eye-pcbway-technical-drawing-r18.pdf`: retained
  prior four-part revision with the exposed power-switch opening.
- Revision 17 and 16 STL packages remain available as historical outputs.
- `stl/`: individual STL files, export manifest and `PRINTING.md` notes.

Revision 19 **export, mesh, viewer and drawing checks are in progress**.
Use the new lid, retainer-equipped button strip and separate power slider as
a matched set. The base, midframe and six M2 case/frame fastening positions
retain revision 18 geometry.

| Current STL file | Intended orientation / change |
|---|---|
| `stl/p4x-eye-base-r19.stl` | Floor down; revision 18 base reusable |
| `stl/p4x-eye-lid-r19.stl` | Exterior face down; smaller slot and flange pocket |
| `stl/p4x-eye-midframe-r19.stl` | Flat underside down; revision 18 frame reusable |
| `stl/p4x-eye-button-strip-r19.stl` | Stems down; integral power-slider retainer; inspect supports |
| `stl/p4x-eye-power-slider-r19.stl` | New separate captive slider; final export orientation to be recorded |

All four revision 18 meshes passed watertightness, winding, positive-volume
and single-component topology checks. Their viewer and three-page quotation
drawing also passed the documented inspections. Those retained historical
results do not validate the changed revision 19 lid, strip, slider or drawing.

Delivery meshes are intended to sit at Z=0 and should be imported as separate
objects in millimeters at 100% scale. Review `stl/PRINTING.md` and slice for the
selected printer and material; physical fit, insert installation, key return,
slider operation and printed strength remain unverified.

'''+s[b:]
s=s.replace('and **64 simple R2 rings**. Its **Power switch** and **Midframe detail** browser\nviews were inspected with no reported console warnings or errors.','and **64 simple R2 rings**. Its **Power switch** and **Midframe detail** browser\nviews were inspected with no reported console warnings or errors. These are\nhistorical revision 18 results; revision 19 validation is in progress.',1)
a=s.index('Revision 18 provides direct access to **SW7**');b=s.index('Three pressable top buttons are enabled independently',a)
s=s[:a]+'''Revision 19 uses a **captive printed slider** to operate **SW7** at case XY
**(63.0732,11.2)**. The lid has a **9 x 4.4 mm R1 running slot** and a
**17 x 8.2 mm R1 underside flange pocket**, ending at **Z27.8** and retaining
**1.2 mm of roof** beneath the Z29 exterior. The separate slider's **6 x 3.8 mm
R1 nub** reaches **Z30**, with a softened upper edge. Its **14 x 7.6 mm R1
flange at Z26.8-27.6** overlaps the slot by at least **1 mm at the ends** and
**1.6 mm at the sides** over the nominal full travel. It covers the opening;
this overlapping arrangement has **no waterproof or IP-rating claim**.

A **4 x 4 mm lower boss**, with R0.3 corners and bottom at **Z25.4**, carries a
**2 x 2 mm socket** up to **Z27.9**. Its entry widens to **2.4 mm square** over
the lower 0.2 mm. The socket slips over the original **1.5 mm-square switch
handle**. The cap's nominal **3 mm total travel** allows 2 mm of switch travel,
0.5 mm of socket play and 0.5 mm reserve. The guide has **0.3 mm clearance per
side in Y** and **0.2 mm above and below the flange at its nominal position**.
Those vertical clearances permit 0.4 mm total float; they are not preload.

The button strip now includes an **18 x 9 mm R1 retaining shelf at
Z25.4-26.6**, with a **7 x 4.6 mm guide opening** around the slider boss.
A **5 mm-wide bridge with R0.6 roots** joins the shelf to the strip's existing
anchor bar. The same two strip screws retain the assembly. Nominal downward
loads pass from the slider flange into this shelf; the resin shelf, bridge and
anchors still need physical strength and operating checks.

Nominal clearance from the shelf underside to the switch body is **0.6 mm**.
At the slider's lowest permitted position, the boss remains **0.4 mm above the
switch body**, and the socket ceiling retains **0.4 mm above the taller BOM
handle**. The retaining shelf clears the nearby USB shell by approximately
**0.84 mm**. These are CAD clearances, not measured installed fit.

The HRO **K3-1235S-F1** drawing specifies a **9 x 3.5 x 3.5 mm body** and
**2 mm slider travel along X**. It shows a **2 mm handle height**, while the
board BOM states **2.5 mm**. The provisional handle tops are Z26.8 and Z27.3,
respectively; actual solder height, socket engagement and operation need a
fit check. [HRO switch drawing](https://static.chipdip.ru/lib/844/DOC012844376.pdf)

Insert the cap from the inside of the separate lid, then fasten the button
strip to capture its flange. Check free motion before lowering the cover and
align the socket with the original switch handle. **Do not glue the moving
slider.** A clear gloss finish may be applied to exterior faces, but keep the
running slot, flange guide, retainer opening and handle socket free of coating
buildup and adhesive.

'''+s[b:]
s=s.replace('A separate one-piece strip fastens inside the cover, with three independently','A separate one-piece strip fastens inside the cover and now also retains the\npower slider. It has three independently',1)
s=s.replace('Then fasten the button strip and test-fit the LCD.','Insert the power slider from inside the lid, then fasten the retainer-equipped\nbutton strip to capture it and test-fit the LCD.',1)
s=s.replace('the cover straight down over the three frame keys and past the wheel to close','the cover straight down over the three frame keys, aligning the power-slider\nsocket with SW7, and past the wheel to close',1)
s=s.replace('Check SW7 access\nand full slider travel.','Check the power cap engages SW7 and allows full slider travel without binding.',1)
p.write_text(s)
p=root/'stl/PRINTING.md';s=p.read_text().replace('# Revision 18 — prototype enclosure parts','# Revision 19 — prototype enclosure parts',1)
s=s.replace('have been individually positioned on Z=0; their import positions are not the\nassembled positions. All four final STL meshes passed the export checks below.','are intended to be positioned on Z=0; their import positions are not the\nassembled positions. Revision 19 export checks are in progress.',1)
s=s.replace('**Reprint the lid, midframe and button strip together.** The base geometry is\nunchanged from revision 17, so an existing base can be reused. Prior revision\n17 STL files, ZIP and quotation drawing are retained separately.','**Print the new lid, button strip and separate power slider together.** Revision\n18 base and midframe geometry is unchanged and those parts can be reused.\nThere are five parts in the complete revision 19 set. Previous files and\nquotation drawings are retained separately.',1)
s=s.replace('| Button strip | Actuator stems down | Shortened anchor bar; front fixing moved to (63,19). Inspect supports below arms and bar. |','| Button strip | Actuator stems down | Includes power-slider retainer shelf; inspect supports under shelf, arms and bar. |\n| Power slider | To be recorded with final export | New separate moving part. Preserve flange, nub and socket dimensions; remove supports without blocking the socket. |',1)
a=s.index('SW7 has a **12 x 5 mm');b=s.index('## Hardware and assembly',a)
s=s[:a]+'''Revision 19 replaces the exposed SW7 opening with a **separate captive power
slider**. Its nub is 6 x 3.8 mm and projects 1 mm above the lid. A 14 x 7.6 mm
flange covers the 9 x 4.4 mm running slot from underneath. An integral shelf on
the button strip captures that flange using the strip's existing two screws.
The socket is 2 mm square with a 2.4 mm entry, sized around the nominal
1.5 mm-square switch handle. The flange overlaps the slot throughout its
3 mm nominal travel; this is not a waterproof seal.

With the lid separate, insert the cap from inside and fit the button strip to
capture it. Check free motion. When lowering the cover, align the cap socket
with the original SW7 handle. Do not force the cap onto the switch or glue the
moving slider. Check both switch positions and the cap's full travel.

There is 0.3 mm nominal Y clearance at the running guides, and 0.2 mm above
and below the flange at its nominal height. Keep the slot, guides, retainer
opening and socket free of clear-coat buildup and adhesive; clear gloss is
for exterior cosmetic surfaces. The switch drawing and BOM disagree on handle
height (2 versus 2.5 mm), so actual engagement and the shelf/bridge load path
need a fit prototype. The strip retains anchors at (63,19) and (63,36).

'''+s[b:]
a=s.index('## Export checks');s=s[:a]+'''## Export checks

Revision 19 STL, viewer and four-page quotation-drawing checks are in progress.
The previous revision 18 four-part meshes and drawing passed their documented
checks; those do not validate the new slider or modified lid and button strip.
Final results will be recorded in `export-manifest.json`. Physical fit,
supplier acceptance and support settings remain unverified. The manufacturer
remains undecided; Protolabs was ruled out by the user on price.
''';p.write_text(s)
p=root/'JLC3DP_REVIEW.md';s=p.read_text().replace('# Revision 18 manufacturing review','# Revision 19 manufacturing review',1)
a=s.index('All four final revision 18 STL meshes');b=s.index('Physical fit remains unverified.',a)
s=s[:a]+'Revision 19 five-part export, viewer and four-page drawing validation are in progress. '+s[b:]
a=s.index('The assembled body is');b=s.index('| Feature |',a)
s=s[:a]+'''The assembled body is 80 × 56 × 29 mm, or 30 mm including raised buttons and
power nub. Revision 19 adds a separate captive power slider, reduces the lid
opening to a 9 × 4.4 mm running slot, and adds a 17 × 8.2 mm underside flange
guide. The existing button strip gains an 18 × 9 mm retaining shelf connected
by a 5 mm bridge with R0.6 roots. Two existing strip screws capture the slider;
no new insert is required. The revision 18 open cable bay and front PCB support
are retained. Print the new lid, button strip and power slider; revision 18
base and midframe remain reusable. Six M2 insert positions are unchanged.

'''+s[b:]
s=s.replace('| Power-switch access | 12 × 5 mm R1 throat; 14 × 7 mm R2 mouth | Confirm actual handle height, full 2 mm travel and fingernail reach after finishing. |','| Captive power slider | 0.3 mm Y guide gaps; 0.2 mm nominal gap above/below flange | Qualify free travel and preserve gaps after finishing. |\n| Power-slider roof and retainer | 1.2 mm roof; 1.2 mm shelf; 5 mm bridge | Review resin strength, bridge loads and anchor screw retention. |\n| Power-slider socket | 2 mm square with 2.4 mm entry | Confirm actual handle height/fit and complete switch travel; keep coating and adhesive out. |',1)
s=s.replace('The installed height and access feel remain unverified.','The installed height, socket engagement and slider feel remain unverified.',1)
s=s.replace('## Alternative providers and cost status','The user prefers a clear resin case. Exterior clear gloss is acceptable, but the\nslider running slot, flange pocket, retainer guide and handle socket must be\nkept free of coating buildup. Do not bond the moving cap. Its overlapping flange\ncovers the opening but is not a gasket and has no waterproof/IP-rating claim.\n\n## Alternative providers and cost status',1)
s=s.replace('this four-part enclosure','this five-part enclosure',1)
s=s.replace('Request one set of four separate parts: base, lid, midframe and button strip, in millimetres.','Request one set of five separate parts: base, lid, midframe, button strip with retaining shelf, and power slider, in millimetres.',1)
s=s.replace('and an itemized alternative using a tougher material for the button strip.','and an itemized alternative using a tougher material for the button strip/retainer and moving slider. Keep their functional guiding and socket surfaces finish-free.',1)
p.write_text(s)
p=root/'dimensions.json';j=json.loads(p.read_text());j['previous_stl_export_revision_18']=copy.deepcopy(j['stl_export']);j['previous_viewer_validation_revision_18']=j['viewer_validation'];j['previous_technical_drawing_revision_18']=copy.deepcopy(j['technical_drawing']);j['previous_power_switch_access_revision_18']=copy.deepcopy(j['power_switch_access']);j['revision']=19
j['status']='Revision 19 adds a captive printed power slider, a smaller covered lid running slot and an integral retainer on the button strip. Five printed parts total. New lid, button strip and power slider required; revision 18 base and midframe unchanged. STL/viewer/drawing checks in progress; physical fit and supplier acceptance unverified.'
j['viewer_validation']='Revision 19 captive-slider numerical and browser checks in progress; revision 18 results preserved separately.'
j['lid_and_other_access']+=' Revision 19 replaces the exposed SW7 opening with a separate captive slider, smaller lid slot, underside flange guide and integral retaining shelf on the button strip. Base and midframe retain revision 18 geometry.'
j['stack_z_spans']['power_slider_flange']=[26.8,27.6];j['stack_z_spans']['power_slider_retainer']=[25.4,26.6];j['stack_z_spans']['power_slider_socket']=[25.4,27.9];j['stack_z_spans']['power_slider_nub']=[27.6,30]
b=j['button_actuators'];b['previous_construction_revision_18']=b['construction'];b['construction']='One removable strip under upper cover with three independent raised keys and a rigid power-slider retaining shelf joined to the anchor bar. Same two strip screws capture the shelf and power cap.'
b['assembly']='Insert separate power slider through lid from inside first, then fasten retainer-equipped button strip with existing two screws. Check free cap travel before fitting lid over SW7 and verify no button preload.'
b['power_slider_retainer']={'introduced_revision':19,'center_case_xy':[63.0732,11.2],'size_xy':[18,9],'corner_radius':1,'z_span':[25.4,26.6],'thickness':1.2,'guide_slot_size_xy':[7,4.6],'guide_slot_corner_radius':0.3,'bridge_width':5,'bridge_root_radius':0.6,'mounting':'Integral to button-strip anchor bar, same two screws at (63,19) and (63,36)','nominal_switch_body_clearance':0.6,'nominal_usb_shell_clearance':0.84,'load_path':'Slider flange bears on shelf under downward load; shelf/bridge/strip anchors require physical resin-strength validation','validation':'Revision 19 checks in progress; physical load/operation unverified'}
j['power_switch_access']={k:copy.deepcopy(v) for k,v in j['previous_power_switch_access_revision_18'].items() if k in ['ref','part','manufacturer_drawing_url','local_reference','gerber_top_xy','case_xy','pcb_side','body_size_xyz','body_base_case_z','body_top_case_z','handle_size_xy','slider_travel_x','handle_height_drawing','handle_height_bom','installed_handle_top_case_z_provisional','height_status']}
j['power_switch_access'].update({'revised_revision':19,'construction':'Separate socketed captive cap coupled to original SW7; lid slot covered by flange, retained from beneath by button-strip shelf','running_slot_size_xy':[9,4.4],'running_slot_corner_radius':1,'running_slot_xy_bounds':{'x':[58.5732,67.5732],'y':[9,13.4]},'running_slot_cutter_z_span':[26.9,29.1],'underside_guide_size_xy':[17,8.2],'underside_guide_corner_radius':1,'underside_guide_ceiling_z':27.8,'remaining_lid_roof':1.2,'actuation':'Slide raised printed nub along X; no glue on moving cap','validation':'Revision 19 checks in progress; physical engagement, free movement and load path unverified'})
j['power_slider']={'introduced_revision':19,'cad_part':'power_slider','quantity':1,'center_case_xy_nominal':[63.0732,11.2],'assembly_reference_x_offset':1,'nub_size_xy':[6,3.8],'nub_corner_radius':1,'nub_top_case_z':30,'nub_projection_above_lid':1,'nub_upper_edge_chamfer_xy':0.3,'nub_upper_edge_chamfer_height':0.3,'flange_size_xy':[14,7.6],'flange_corner_radius':1,'flange_z_span_nominal':[26.8,27.6],'lower_boss_size_xy':[4,4],'lower_boss_corner_radius':0.3,'lower_boss_bottom_case_z':25.4,'socket_size_xy':[2,2],'socket_top_case_z':27.9,'socket_entry_size_xy':[2.4,2.4],'socket_entry_height':0.2,'socket_clearance_per_side_to_nominal_handle':0.25,'nominal_cap_travel_x':3,'switch_travel_x':2,'socket_play_x_total':0.5,'remaining_travel_reserve_total':0.5,'y_guide_clearance_per_side':0.3,'nominal_vertical_clearance_above_flange':0.2,'nominal_vertical_clearance_below_flange':0.2,'total_vertical_float':0.4,'minimum_nominal_slot_end_overlap':1,'nominal_slot_side_overlap':1.6,'minimum_boss_to_switch_body_clearance_at_lowest_cap':0.4,'minimum_socket_ceiling_to_taller_bom_handle_at_lowest_cap':0.4,'retained_by':'Lid roof and integral button-strip shelf; no extra fastener or adhesive','assembly':'Insert cap from lid interior; fasten button strip to capture flange; check free motion, then align socket over SW7 when lowering cover. Do not glue moving part.','finish':'Exterior clear gloss allowed. Mask running guides, retainer slot and handle socket against coating buildup and adhesive.','seal_status':'Overlapping cover only; no gasket, water resistance or IP rating claimed','physical_fit_verified':False,'validation':'Revision 19 mesh/viewer/drawing checks in progress'}
prev=j['previous_stl_export_revision_18'];j['stl_export']={'revision':19,'units':'mm','status':'Export and mesh validation in progress','package_path':'mechanical/p4x-eye-enclosure/p4x-eye-stl-r19.zip','directory':'mechanical/p4x-eye-enclosure/stl/','printing_notes':'mechanical/p4x-eye-enclosure/stl/PRINTING.md','minimum_xyz_for_delivery':[0,0,0],'physical_fit_verified':False,'slicer_support_setup_validated':False,'gcode_provided':False,'parts':[{'part':x['part'],'path':x['path'].replace('-r18.stl','-r19.stl'),'orientation':x['orientation'],'geometry_changed_from_revision_18':x['part'] not in ['base','midframe']} for x in prev['parts']]+[{'part':'power-slider','path':'mechanical/p4x-eye-enclosure/stl/p4x-eye-power-slider-r19.stl','orientation':'To be recorded from final export','geometry_changed_from_revision_18':True}],'watertight_validation':'In progress','reprint_required':['lid','button-strip','power-slider'],'reusable_revision_18_parts':['base','midframe'],'historical_export':'previous_stl_export_revision_18'}
j['technical_drawing']={'revision':19,'path':'mechanical/p4x-eye-enclosure/p4x-eye-pcbway-technical-drawing-r19.pdf','planned_pages':4,'status':'Updated drawing and validation in progress','purpose':'Quotation and supplier review; existing insert pilots not approved supplier-installation dimensions','previous_revision_path':'mechanical/p4x-eye-enclosure/p4x-eye-pcbway-technical-drawing-r18.pdf','physical_fit_verified':False}
j['manufacturing_review']['current_revision']=19;j['manufacturing_review']['requested_finish']='Clear resin case, exterior clear gloss; moving guides and handle socket kept finish-free';j['manufacturing_review']['geometry_action']='Captive power slider and integrated retainer remain a fit prototype. Six M2 insert positions/pilots unchanged; supplier insert geometry still unknown.';j['manufacturing_review']['geometry_concerns_for_review']=[x for x in j['manufacturing_review']['geometry_concerns_for_review'] if 'SW7 handle' not in x]+['SW7 height discrepancy, socket engagement and cap travel after finish','Power-slider retainer shelf/bridge/anchor strength','0.3 mm Y guide and 0.2 mm above/below flange clearances; finish-free guide/socket surfaces']
p.write_text(json.dumps(j,indent=2)+'\n')
