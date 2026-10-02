import copy,json
from pathlib import Path
root=Path('mechanical/p4x-eye-enclosure')
p=root/'README.md';s=p.read_text().replace('Revision 19, 2026-09-28.','Revision 20, 2026-09-28.',1)
a=s.index('USB ports and microSD slot.');b=s.index('The revision 17 **24 x 6.35 mm',a)
s=s[:a]+'''USB ports and microSD slot. The detachable camera is omitted. Revision 20
adds a **lid-owned PCB anti-lift stop** at the far USB corner, with a nominal
**0.2 mm gap above the display-facing PCB surface**. It also moves the lower
USB-side board seat away from RESET and its pads. The upper stop and lower
seat act at different locations; they are not a preloaded clamp.
**Print the new lid and midframe together**. The revision 19 base, button strip
and captive power slider can be reused; the full set still has **five parts**.

The retained power slider has a nub 1 mm above the lid and a flange covering
its running slot. An integral shelf on the button strip retains it using the
strip's existing two screws. PCB thickness and printing tolerances may consume
the new stop's 0.2 mm gap: the lid must close freely before the screws are
tightened. Do not use them to force the cover onto the board.

'''+s[b:]
a=s.index('- `p4x-eye-stl-r19.zip`:');b=s.index('An inline 3D viewer was added',a)
s=s[:a]+'''- `p4x-eye-stl-r20.zip`: current five-part prototype package; export checks in progress.
- `p4x-eye-pcbway-technical-drawing-r20.pdf`: four-sheet quotation drawing in
  preparation for the anti-lift stop and relocated lower support. Existing
  insert pilots remain provisional pending supplier hardware confirmation.
- `p4x-eye-stl-r19.zip` and `p4x-eye-pcbway-technical-drawing-r19.pdf`: retained
  prior revision with captive power slider, before the PCB stop and support correction.
- Earlier revision 18, 17 and 16 output packages remain available as history.
- `stl/`: individual STL files, export manifest and `PRINTING.md` notes.

Revision 20 **STL, viewer and drawing checks are in progress**. Use the new
lid and midframe as a matched set. Base, button strip, power slider and all
six M2 case/frame fastenings retain revision 19 geometry.

| Current STL file | Intended orientation / change |
|---|---|
| `stl/p4x-eye-base-r20.stl` | Floor down; revision 19 base reusable |
| `stl/p4x-eye-lid-r20.stl` | Exterior face down; added PCB anti-lift stop |
| `stl/p4x-eye-midframe-r20.stl` | Flat underside down; relocated USB-side PCB seat |
| `stl/p4x-eye-button-strip-r20.stl` | Stems down; unchanged; inspect support needs |
| `stl/p4x-eye-power-slider-r20.stl` | Nub down, socket up; unchanged |

The retained revision 19 five-part meshes passed watertightness, winding,
positive-volume and single-component checks, with no degenerate/duplicate
triangles or boundary/nonmanifold edges. Its viewer and four-page drawing
passed documented checks. Those historical results do not validate the changed
revision 20 lid, midframe or drawing.

Delivery meshes are intended to sit at Z=0 and should be imported as separate
objects in millimeters at 100% scale. Review `stl/PRINTING.md`; physical fit,
insert installation, board support, slider operation and strength remain
unverified.

'''+s[b:]
s=s.replace('were visually checked without reported warnings or errors. Physical fit remains unchecked.','were visually checked without reported warnings or errors. Those are historical\nrevision 19 results; revision 20 checks are in progress. Physical fit remains unchecked.',1)
a=s.index('Three sparse edge seats bear at');b=s.index('A solid rectangular locator fills',a)
s=s[:a]+'''Three sparse lower seats bear at **Z19.7**, near case XY **(10,43.1)**,
**(61.5,7.0)** and **(69.9,33.75)**. The front seat remains **X60-63,
Y6.1-7.5**, with its stop at Y5.0-6.1. The left seat/guide remain unchanged.
Revision 20 removes the old **X71-74, Y42.1-42.9** seat because it conflicted
with SW2 RESET and its terminal pads. Its replacement spans **X69.3-70.5,
Y32.75-34.75, Z14.49-19.7**, overlapping the plate by 0.01 mm. It retains
**2.4 mm² nominal bearing area** in a **1.2 x 2 mm** section.

The placement, Gerber, mask and drill review gives these nominal clearances
around the relocated lower seat: **0.275 mm to the BOOT body**, **0.425 mm to
its mask pad**, **0.35 mm to an encoder pad**, **0.29 mm to the USB locating
hole's mask opening**, and **0.63 mm radial clearance to the wheel**. The USB
locating hole is centered at **(69.72,35.39)**, with **0.6 mm nonplated drill**
and **0.7 mm mask opening**. These small sourced plan clearances still require
comparison with the actual populated board and printed support.

A separate **lid-owned anti-lift stop** is centered at **(72.5,42.3)**. Its
**3 x 1.2 mm R0.2 foot** spans **X71-74, Y41.7-42.9**, from **Z21.5 to
Z21.9**, leaving **0.2 mm nominal clearance** above PCB top Z21.3. The foot
widens up to a **4 x 2.4 mm R0.4 root at Z23.5**, which joins the lid through
Z27.1. The display-face contact region is a **solder-mask bearing land with
tented vias**, identified from the board placement, Gerber, mask and drill
sources; it is not unperforated bare FR4.

The upper stop limits upward board motion while the lower seats support it.
It is independent of the relocated lower seat and creates **no intended
preload or clamp force**. Print dimensions and PCB thickness can consume the
0.2 mm nominal gap. Fit the lid gently and confirm it closes without force
before tightening any case screws. Inspect the real board's bearing land,
components and solder mask; these geometry checks do not prove contact
pressure, tolerance fit or resistance to button/wheel loads.

'''+s[b:]
s=s.replace('the USB openings. Confirm all three keys enter freely and the seam closes. Insert the four M2 x 8 mm','the USB openings. Confirm all three keys enter freely, the anti-lift stop does\nnot preload the PCB, and the seam closes without force. Insert the four M2 x 8 mm',1)
p.write_text(s)
p=root/'stl/PRINTING.md';s=p.read_text().replace('# Revision 19 — prototype enclosure parts','# Revision 20 — prototype enclosure parts',1)
s=s.replace('have been positioned on Z=0; their import positions are not the assembled\npositions. All five final STL files passed the export checks below.','are intended to be positioned on Z=0; their import positions are not the\nassembled positions. Revision 20 export checks are in progress.',1)
a=s.index('**Print the new lid, button strip');b=s.index('| Part |',a)
s=s[:a]+'''**Print the new lid and midframe together.** Revision 19 base, button strip
and power slider can be reused. The full set still contains five parts.
Previous revision STL packages and quotation drawings are retained.

'''+s[b:]
s=s.replace('Insert pockets face upward. Inspect support needs around the display opening, receiver pockets and R2 edge.','Insert pockets and anti-lift stop face upward. Inspect support needs around the display opening, receivers and R2 edge.',1)
s=s.replace('Includes the open front-right cable bay and three upward locating keys.','USB-side lower support relocated to clear RESET; cable bay and locating keys retained.',1)
needle='## Battery cable and power access'
section='''## PCB support and anti-lift stop

The lid's 3 x 1.2 mm rounded stop foot begins at Z21.5, **0.2 mm above the
nominal PCB top at Z21.3**. It is a travel stop, not a clamp. The display-face
bearing land at X71-74, Y41.7-42.9 has solder mask and tented vias; inspect the
actual board before assembly. The stop broadens into the lid roof above it.

The old lower USB-side seat conflicted with RESET and its pads. Its replacement
is at **X69.3-70.5, Y32.75-34.75**, with the same 2.4 mm² bearing area and a
1.2 x 2 mm section. It remains separate from the upper stop. The smallest
nominal sourced clearance is 0.275 mm to the BOOT body; the USB locating-hole
mask opening is 0.29 mm away. Verify the real solder/component envelopes.

**The lid must close freely before tightening the case screws.** Printed size
and PCB thickness can consume the stop's 0.2 mm gap. Do not force it shut or
use the screws to apply PCB preload. Physical fit and support loads are untested.

'''
s=s.replace(needle,section+needle,1)
a=s.index('## Export checks');s=s[:a]+'''## Export checks

Revision 20 STL, numerical/viewer and four-page drawing checks are in progress.
Historical revision 19 results do not validate the changed lid and frame.
Current results will be recorded in `export-manifest.json`. Physical fit,
PCB preload clearance, operating forces, supplier acceptance and support
settings remain unverified.
''';p.write_text(s)
p=root/'JLC3DP_REVIEW.md';s=p.read_text().replace('# Revision 19 manufacturing review','# Revision 20 manufacturing review',1)
a=s.index('All five revision 19 STL meshes');b=s.index('Physical fit remains unverified.',a)
s=s[:a]+'Revision 20 STL, numerical/viewer and four-page drawing checks are in progress. '+s[b:]
a=s.index('The assembled body is');b=s.index('| Feature |',a)
s=s[:a]+'''The assembled body remains 80 × 56 × 29 mm, or 30 mm including raised controls.
Revision 20 adds a lid-owned PCB anti-lift stop: a 3 × 1.2 mm R0.2 foot at
(72.5,42.3), Z21.5, nominally 0.2 mm above the PCB. It widens to a 4 × 2.4 mm
R0.4 root at Z23.5 and joins the lid. The display-face bearing land is covered
by solder mask and includes tented vias; it is not unperforated bare FR4.

The prior lower seat at X71-74, Y42.1-42.9 conflicted with RESET and its pads.
The replacement is X69.3-70.5, Y32.75-34.75, bearing at Z19.7 with the same
2.4 mm² area and a 1.2 × 2 mm section. The upper stop and relocated lower seat
are independent, with no intended clamp force. Print the new lid and midframe;
revision 19 base, button strip and power slider remain reusable. Five parts and
six M2 insert positions are retained.

'''+s[b:]
s=s.replace('| Wheel-side hardware clearance band |','| PCB anti-lift gap | 0.2 mm above nominal PCB | Review printed/PCB tolerance stack; lid must close without force before screw tightening. |\n| Relocated USB-side lower seat | 0.275 mm minimum sourced body gap; 0.29 mm to USB-hole mask | Confirm actual solder/component envelopes and printed dimensions. |\n| Wheel-side hardware clearance band |',1)
s=s.replace('## Inserts and clear finish','The lower-seat review also found 0.425 mm to the BOOT mask pad, 0.35 mm to\nan encoder pad and 0.63 mm radial wheel clearance. The nearby nonplated USB\nlocating hole is at (69.72,35.39), drill Ø0.6 with Ø0.7 mask opening. These\nplan clearances do not establish installed fit or board contact pressure. The\n0.2 mm anti-lift gap can be consumed by tolerances; do not draw a binding lid\nonto the PCB with the case screws.\n\n## Inserts and clear finish',1)
p.write_text(s)
p=root/'dimensions.json';j=json.loads(p.read_text());j['previous_stl_export_revision_19']=copy.deepcopy(j['stl_export']);j['previous_viewer_validation_revision_19']=j['viewer_validation'];j['previous_technical_drawing_revision_19']=copy.deepcopy(j['technical_drawing']);j['revision']=20
j['status']='Revision 20 adds a lid-owned PCB anti-lift stop with 0.2 mm nominal gap and relocates the lower USB-side support to clear RESET/pads. New lid and midframe required; base, button strip and power slider unchanged from revision 19. Five-part export, viewer and drawing checks in progress; physical fit, contact pressure and supplier acceptance unverified.'
j['viewer_validation']='Revision 20 anti-lift stop and relocated-seat numerical/browser checks in progress; revision 19 results preserved separately.'
j['lid_and_other_access']+=' Revision 20 adds an independent upper PCB travel stop and relocates the lower USB-side seat away from RESET/pads. No preload or clamping force intended; lid must close freely before tightening screws. Base, button strip and power slider retain revision 19 geometry.'
f=j['midframe'];f['previous_board_edge_seats_revision_19']=copy.deepcopy(f['board_edge_seats']);f['status']='Removable midframe with lower USB-side PCB support relocated to X69.3-70.5,Y32.75-34.75 to clear RESET. Other revision 19 geometry retained. Current export/viewer checks in progress; physical fit and load support unverified.'
seats=f['board_edge_seats'];seats['display_case_xy'][2]=[69.9,33.75]
for k in list(seats):
 if k.startswith('upper_right') or k=='right_seam_correction':seats.pop(k)
seats['contact_status']='Sparse support candidates; relocated USB-side solder-mask bearing region reviewed against placements, Gerbers, mask openings and drills. Actual component/solder envelopes, contact and printed fit unverified.'
seats['usb_side_seat']={'introduced_revision':20,'xy_bounds':{'x':[69.3,70.5],'y':[32.75,34.75]},'size_xy':[1.2,2],'z_span_with_union_overlap':[14.49,19.7],'nominal_bearing_z':19.7,'nominal_contact_area':2.4,'replaces_xy_bounds':{'x':[71,74],'y':[42.1,42.9]},'reason':'Old support collided with SW2 RESET and terminal pads','source_basis':'MB V2.3 placement, Gerber copper, solder-mask and drill review','nominal_clearances':{'boot_body':0.275,'boot_mask_pad':0.425,'encoder_pad':0.35,'usb_locating_hole_mask':0.29,'wheel_radial':0.63},'usb_locating_hole':{'center_case_xy':[69.72,35.39],'nonplated_drill_diameter':0.6,'mask_opening_diameter':0.7},'validation':'Source plan clearances reviewed; current export/viewer and physical fit validation pending'}
f['verification_needed'].append('Lid anti-lift stop must leave clearance without PCB preload; check real board thickness, print dimensions and revised seat component/solder clearance')
f['assembly_sequence']=f['assembly_sequence'].replace('confirm free key entry and a closed seam','confirm free key entry and a closed seam without the anti-lift stop preloading the PCB; do not pull a binding lid down with screws')
j['pcb_anti_lift_stop']={'introduced_revision':20,'owner':'lid','center_case_xy':[72.5,42.3],'foot_size_xy':[3,1.2],'foot_corner_radius':0.2,'foot_xy_bounds':{'x':[71,74],'y':[41.7,42.9]},'foot_z_span':[21.5,21.9],'root_size_xy':[4,2.4],'root_corner_radius':0.4,'root_z_span_nominal':[23.5,27.1],'loft_z_span_nominal':[21.9,23.5],'nominal_pcb_top_z':21.3,'nominal_pcb_gap':0.2,'source_basis':'Display-face patch reviewed against MB V2.3 placement, Gerber, mask and drill sources','bearing_surface':'Solder-mask bearing land with tented vias; not unperforated bare FR4','function':'Limit upward PCB travel; separate from relocated lower support, no intended preload or clamp force','assembly_note':'PCB thickness and printing tolerances may consume 0.2 mm nominal gap. Confirm lid closes freely before tightening screws; never use screws to force cover onto PCB.','physical_fit_verified':False,'validation':'Revision 20 export/viewer/drawing checks in progress; physical pressure, fit and loads unverified'}
j['stack_z_spans']['pcb_anti_lift_foot']=[21.5,21.9];j['stack_z_spans']['pcb_anti_lift_root']=[23.5,27.1]
prev=j['previous_stl_export_revision_19'];j['stl_export']={'revision':20,'units':'mm','status':'Export and mesh validation in progress','package_path':'mechanical/p4x-eye-enclosure/p4x-eye-stl-r20.zip','directory':'mechanical/p4x-eye-enclosure/stl/','printing_notes':'mechanical/p4x-eye-enclosure/stl/PRINTING.md','minimum_xyz_for_delivery':[0,0,0],'physical_fit_verified':False,'slicer_support_setup_validated':False,'gcode_provided':False,'parts':[{'part':x['part'],'path':x['path'].replace('-r19.stl','-r20.stl'),'orientation':x['orientation'],'geometry_changed_from_revision_19':x['part'] in ['lid','midframe']} for x in prev['parts']],'watertight_validation':'In progress','reprint_required':['lid','midframe'],'reusable_revision_19_parts':['base','button-strip','power-slider'],'historical_export':'previous_stl_export_revision_19'}
j['technical_drawing']={'revision':20,'path':'mechanical/p4x-eye-enclosure/p4x-eye-pcbway-technical-drawing-r20.pdf','planned_pages':4,'status':'Updated drawing and validation in progress','purpose':'Quotation/supplier review; existing insert pilots not approved supplier installation dimensions','previous_revision_path':'mechanical/p4x-eye-enclosure/p4x-eye-pcbway-technical-drawing-r19.pdf','physical_fit_verified':False}
j['manufacturing_review']['current_revision']=20;j['manufacturing_review']['geometry_action']='Independent lid anti-lift stop and relocated lower PCB support added. Only lid/midframe changed; six M2 insert positions/pilots unchanged and supplier insert geometry remains unknown.';j['manufacturing_review']['geometry_concerns_for_review']+=['0.2 mm nominal upper PCB stop gap versus print/PCB thickness tolerances; no preload intended','Relocated lower seat has0.275 mm BOOT-body and0.29 mm USB-hole-mask clearances; inspect actual components/solder']
p.write_text(json.dumps(j,indent=2)+'\n')
