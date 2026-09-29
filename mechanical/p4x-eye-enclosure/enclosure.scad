/* ESP32-P4X-EYE camera-free enclosure — PCB anti-lift stop, revision 20.
 * Units: mm. Read README.md before manufacturing.
 * PCB XY is sourced; battery dimensions are supplied by the user.
 * Board Z clearances remain provisional.
 * No fit-verified STL is supplied. Open this editable model in OpenSCAD.
 * Camera omitted. LCD, buttons, USB, SD, encoder and mic are to be retained.
 */

part = "assembly"; // [assembly,exploded,base,midframe,lid,button_strip,power_slider,pcb_template]
show_references = true;
show_case_hardware = true; // Reference-only inserts/screws; omitted from printed-part exports.
show_midframe_hardware = true; // Same M2 x 8 hardware, installed from above the midframe.
usb_apertures = true; // Requested ports; cable allowance and installed Z remain provisional.
lcd_aperture = true; // Full-module flush seat; installed active-area XY remains provisional.
button_keys = true; // Provisional printed return strip; verify travel and material in hand.

case_w = 80;
case_h = 56;
corner_r = 5;
wall = 2;
upper_wall = 1.6;
lower_body_h = 45.5; // Both halves follow the wheel shoulder and inset USB side.
upper_body_h = 45.5;
floor_t = 2;
lid_t = 2;
face_edge_r = 2.0; // Soft outer face perimeter only; the assembly seam stays straight.
face_edge_steps = 64; // Fine angular slices avoid invalid lofts at the concave USB return.
battery_w = 65; // User dimensions 36 x 65 x 10, rotated in plane.
battery_h = 36;
battery_t = 10;
battery_gap_z = 0.5; // Thin pad/adhesive allowance below pack.
battery_shift_x = 1.0; // Revision 9: move left 0.75 mm for the closed right screw well.
battery_shift_y = -4.0; // Revision 14: move the battery another 0.5 mm forward to Y6.0.
rear_component_space = 7.2; // PROVISIONAL; midframe consumes 2 mm of this allowance.
pcb_t = 1.6; // UNMEASURED.
front_component_space = 5.7; // UNMEASURED: LCD, flex and controls.

battery_z = floor_t + battery_gap_z;
battery_xy = [(case_w-battery_w)/2+battery_shift_x,
              (case_h-battery_h)/2+battery_shift_y];
pcb_z = battery_z + battery_t + rear_component_space;
joint_z = pcb_z + pcb_t + front_component_space;
case_depth = joint_z + lid_t; // 29 at defaults, including 1.5 mm removable midframe.
mid_z = battery_z+battery_t+0.5;
mid_t = 1.5;
mid_top = mid_z+mid_t;
pcb_offset = [(case_w-69)/2, (case_h-43)/2];
boss_xy = [[3.6,3.6],[case_w-3.6,3.6],
           [3.6,case_h-3.6],[77.0,41.0]]; // Shift the right well forward 0.5 mm to align the rear edge.
boss_d = [5.2,5.2,5.2,4.4];
case_insert_od = 3.0; // User's actual M2 heat-set insert outside diameter.
case_insert_l = 3.2; // User's actual insert length.
case_insert_bore_d = 2.7; // PROVISIONAL print pilot, not a universal M2 insert specification.
case_insert_bore_depth = 3.4;
case_insert_leadin_d = 3.1;
case_insert_leadin_depth = 0.25;
case_insert_pad_d = 5.2;
case_insert_pad_top = 18.2;
case_insert_taper_top = 19.2; // Narrow before the PCB underside at Z19.7.
case_tip_relief_d = 2.4;
case_tip_relief_top = 22.5; // Blind screw-tip space, leaving the front face unpierced.
case_screw_clear_d = 2.3;
case_head_recess_d = 4.5;
case_head_recess_depth = 10.2; // Deep underside well; head bears at Z10.2.
case_screw_l = 8; // User's M2 x 8 under-head length; nominal tip Z18.2.
case_screw_head_d = 4.0; // Reference nylon pan-head envelope; check the selected screw.
case_screw_head_h = 1.3;
case_well_pad_d = 6.5; // Lower support only; clipped to the existing case outline.
case_well_pad_top = 11.5; // Leaves 1.3 mm above the head seat; meets the ledge below the frame.
case_right_well_pad_d = 6.0; // Fully closed 0.75 mm sleeve around the 4.5 mm access bore.

// Gerber TOP XY. The +Z face of this concept is the display side, so flip X.
function xy(p) = [pcb_offset[0]+69-p[0], pcb_offset[1]+p[1]];
// Lower projecting tab, positively identified by user's circled MB V2.3 photo.
mount_xy = xy([53.4,2.2]);
mount_pcb_hole_d = 2.0; // Nominal Gerber circle; check screw clearance physically.
mount_post_d = 6;
mid_screw_d = case_screw_clear_d; // Ø2.3 clearance for the user's M2 frame screws.
mid_screw_l = case_screw_l; // The same 8 mm under-head length as the case screws.
mid_tip_relief_bottom = 6.0; // Blind Ø2.4 relief; screw tip Z6.5 leaves 0.5 mm spare.
mid_right_xy = [44,48.5]; // Rear frame fixing, outside the PCB extension and battery.
mid_third_xy = [5.0,8.7]; // Front-left frame fixing; the PCB fixing belongs to the frame.
mid_third_post_d = 5.5;
mid_fit_gap = 0.3;
mid_ledge_w = 1.4;
mid_ledge_t = 1.5;
mid_battery_relief = 0.2; // Narrow the shelf locally where the shifted pack reaches beneath it.
// Edge-open cable bay removes the unsupported strips beside the old passage.
// J25 is at case (71.375,24.65), lead corner at (73.5,6), plug width 5 mm.
// Cutter exits the front and right of the MIDFRAME ONLY; base stays closed.
battery_cable_bay_xy = [64.5,-1];
battery_cable_bay_size = [16.5,29.65]; // Upper edge Y28.65, beyond the connector.
battery_cable_bay_r = 1;
front_board_seat_x = 60; // Shift inward from X65; 1.5 mm plate beyond right side.
// Move the USB-side lower seat away from SW2 RESET and its terminal pads.
// Gerber/placement nominal clearances: SW6 pad .35; USB locating hole mask .29;
// SW1 body .275 mm. Smallest physical clearances still require fit inspection.
usb_board_seat_xy = [69.3,32.75];
usb_board_seat_size = [1.2,2.0];
// Display face at the far USB end is clear in the MB V2.3 assembly/mask files.
// This is a travel stop, not a clamp. PCB thickness and print tolerances remain
// unmeasured; never use the case screws to pull a binding lid onto the board.
pcb_lift_stop_xy = [72.5,42.3];
pcb_lift_stop_size = [3,1.2];
pcb_lift_stop_root = [4,2.4];
pcb_lift_gap = 0.2;
pcb_lift_stop_z = pcb_z+pcb_t+pcb_lift_gap;
pcb_lift_stop_shoulder_z = 23.5;
// Midframe-owned alignment keys and lid-owned blind receivers, all below PCB.
// Each row is [x,y,width,depth], in case coordinates. No outer-wall thinning.
lid_key_rects = [[2.3,20,1.5,8],[40,2.3,8,1.5],[24,52.2,8,1.5]];
lid_receiver_rects = [[1.5,18.8,3.5,10.4],[38.8,1.5,10.4,3.5],
                      [22.8,51,10.4,3.5]];
lid_key_h = 2.5;
lid_key_clearance = 0.2; // Per side; sliding registration, not a friction latch.
lid_key_tip_chamfer = 0.3;
lid_key_tip_h = 0.4;
lid_key_top_gap = 0.3;
lid_receiver_h = 3.3;
lid_key_mouth_flare = 0.2;
lid_key_mouth_h = 0.4;
mount_pilot_d = 1.2; // Provisional M1.6 self-tapping screw pilot; tune for material.
mount_pilot_depth = 5;
// SW6: Mitsumi SIQ-02FVS3 datasheet, page 3. Installed solder offset unmeasured.
wheel_xy = xy([11.6,32.85]);
wheel_d = 14.5;
wheel_dial_t = 2;
wheel_projection = 4;
wheel_z = pcb_z-wheel_projection;
wheel_grip_depth = 1.0; // Total exterior relief; the rear-right corner now shares this edge.
wheel_grip_blend = 2.0; // Left transition; the relieved rear flat now continues to the right corner.
wheel_grip_span = [wheel_xy[0]-wheel_d/2,wheel_xy[0]+wheel_d/2];
wheel_lower_wall = 1.5;
wheel_upper_wall = 1.1;
wheel_hardware_wall = 0.6; // User-selected thin band preserves PCB and encoder clearance.
wheel_hardware_z = [wheel_z+wheel_dial_t,pcb_z+pcb_t+0.3]; // Z17.7..21.6.
wheel_reinforce_y = 41.5; // Preserve the unrelated 1 mm USB wall in front of this region.
// The upper housing follows the board shoulder; the wheel projects beyond it.
wheel_slot_w = 13; // Clears the dial at the reinforced inner wall, including corner rounding.
wheel_slot_y = 42.5;
wheel_slot_depth = 11.5;
wheel_slot_z = 14.4; // Open through the cover's lower edge for straight assembly.
wheel_slot_h = 3.7;
// J6/J23: HRO TYPE-C-31-M-12, mounted on the display-facing PCB surface.
// Drawing height 3.26 mm (product page says 3.16); check installed Z physically.
usb_slot_w = 9.8;
usb_slot_h = 4.2;
usb_slot_r = 0.5;
usb_center_z = pcb_z+pcb_t+1.63;
usb_case_y = [pcb_offset[1]+11,pcb_offset[1]+26];
usb_face_x = 76;
usb_wall_x = 75.8; // Socket shell projects 0.2 mm past the housing face.
usb_wall_t = 1.0; // Leaves 0.3 mm to the nominal PCB edge at X74.5.
lower_usb_wall_t = 1.6; // Battery-side wall: X74.2..75.8 along the straight USB section.
usb_inner_region = [70,7,80,40.8]; // Local thinner-wall region, joined to the general interior.
usb_panel_y = [11,38];
usb_rail_y = [11.2,37.8]; // 0.2 mm side fitting gaps to the upper cover.
usb_split_gap = 0.2;
usb_rail_top = usb_center_z-usb_split_gap/2;
usb_lid_bottom = usb_center_z+usb_split_gap/2;
usb_plug_body_w = 10.25; // User-measured plastic overmold width, not a universal USB-C envelope.
usb_plug_body_h = 6.0; // User-measured height; assumed centered on the rear socket.
usb_plug_front_x = usb_face_x; // Assumption: plastic starts at or outside socket face X76.
usb_cable_relief_x = [75.8,81];
usb_cable_relief_y = [36.8,38.6];
usb_cable_relief_z = [19.2,26.8];
usb_cable_relief_xy_r = 0.2;
usb_cable_relief_yz_r = 0.5;
lcd_center = [30,26.86]; // Provisional layout assumption, NOT a verified active-area center.
// ZJY154KC-IF17 drawing: module 31.52 x 33.72 x 1.9 (+/-0.1) mm;
// active area 27.72 mm square, 1.55 mm toward +Y from the module center.
// The flex exits toward -Y. Keep the retention load on the backlight frame,
// never a hard press-fit against the LCD glass.
lcd_module_w = 31.52;
lcd_module_h = 33.72;
lcd_module_t = 1.9;
lcd_active_size = 27.72;
lcd_module_center = lcd_center-[0,1.55];
lcd_fit_clearance = 0.2; // Per-side clearance; tune with a printer fit coupon.
lcd_seat_depth = 1.9; // Measure the actual module; adjust for flush glass height.
lcd_seat_z = case_depth-lcd_seat_depth;
lcd_support_w = 1.0; // Nominal overlap under the rear frame at each edge.
lcd_support_t = 1.2;
lcd_collar_border = 1.5;
lcd_collar_top = max(lcd_seat_z,joint_z+0.1);
lcd_pocket_w = lcd_module_w+2*lcd_fit_clearance;
lcd_pocket_h = lcd_module_h+2*lcd_fit_clearance;
lcd_pocket_xy = lcd_module_center-[lcd_pocket_w,lcd_pocket_h]/2;
lcd_module_xy = lcd_module_center-[lcd_module_w,lcd_module_h]/2;
lcd_rear_xy = lcd_module_xy+[lcd_support_w,lcd_support_w];
lcd_rear_w = lcd_module_w-2*lcd_support_w;
lcd_rear_h = lcd_module_h-2*lcd_support_w;
lcd_flex_w = 24; // Was 22; +1 mm on each side of the flex relief.
lcd_flex_depth = 6.35; // Was 4.85; +0.75 mm fore/aft.
lcd_flex_xy = [lcd_module_center[0]-lcd_flex_w/2,lcd_module_xy[1]-2.55];
lcd_flex_top_z = 28; // Was 27.5; +0.5 mm bend clearance, with 1 mm exterior skin.
lcd_flex_r = 0.6;
// SW5/SW4/SW3: TAIWAN MISAKI NTC013-AA1J-A160T, 2.5 +/-0.2 mm high;
// published travel 0.2 +/-0.1 mm. These are envelopes, not fit verification.
button_xy = [for(y=[12.5,18.5,24.5]) xy([23,y])];
button_switch_h = 2.5;
button_switch_travel = 0.2;
button_rest_gap = 0.3; // No nominal preload; tune against the actual assembled stack.
button_arm_t = 0.8;
button_arm_top = joint_z-0.4;
button_arm_z = button_arm_top-button_arm_t;
button_arm_w = 2;
button_root_r = 0.3;
button_key_d = 4.4;
button_hole_d = 5.2; // 0.4 mm radial clearance permits a small cantilever tilt.
button_protrusion = 1.0; // Raised keys aid pressing and provide nominal face-down clearance.
button_key_h = case_depth+button_protrusion-button_arm_top;
button_stem_d = 1.8;
button_stem_z = pcb_z+pcb_t+button_switch_h+button_rest_gap;
button_anchor_xy = [[63,19],[63,36]]; // Front anchor clears SW7 access opening.
button_anchor_x = 60.5;
button_anchor_w = 5;
button_anchor_y = 16.5;
button_anchor_h = 21.5;
button_anchor_d = 5;
button_pilot_d = 1.2; // Provisional M1.6; short screw engagement needs print validation.
button_screw_d = 1.8;
// SW7: HRO K3-1235S-F1, bottom/display face, placement (11.4268,4.7).
// Datasheet: body 9 x 3.5 x 3.5, travel 2 along X, slider 1.5 square.
// F1 drawing has 2 mm handle; BOM says 2.5. Installed top Z26.8..27.3
// is provisional. A sliding cap is captured by the roof and button-strip shelf.
power_switch_xy = xy([11.4268,4.7]);
power_switch_body = [9,3.5,3.5];
power_switch_handle = [1.5,1.5,2];
power_switch_travel = 2;
power_nub_size = [6,3.8];
power_nub_r = 1;
power_slot_size = [9,4.4]; // 3 mm cap travel; 2 mm switch travel plus socket play.
power_slot_r = 1;
power_flange_size = [14,7.6];
power_flange_r = 1;
power_flange_z = [26.8,27.6];
power_guide_size = [17,8.2];
power_guide_r = 1;
power_guide_top = 27.8; // 1.2 mm roof above the enclosed flange pocket.
power_retainer_size = [18,9];
power_retainer_z = [25.4,26.6]; // Rigid shelf added to the button strip's anchor bar.
power_retainer_slot = [7,4.6];
power_socket_outer = 4;
power_socket_inner = 2; // 0.25 mm per side around nominal 1.5 mm square lever.
power_socket_bottom = 25.4;
power_socket_top = 27.9; // Leaves clearance above both published handle heights.
power_slider_offset = power_switch_travel/2; // Show the same end as the reference lever.
power_slider_top = case_depth+button_protrusion;
pcb_outline = [[0,0],[57.6,0],[57.6,5.6],[69,5.6],[69,32.6],
 [68.2,32.6],[68.2,37],[54,37],[54,38.3685],[52.759,43],
 [36.241,43],[35,38.3685],[35,37],[21.8,37],
 // Quarter-radius notch is simplified to chords in the reference solid only.
 [21.3,36.5],[21.3,36.3],[20.8,35.8],[16.5,35.8],
 [16,36.3],[16,36.5],[15.5,37],[0,37]];

$fn = 64;
assert(face_edge_r > 0 && face_edge_r <= min(floor_t,lid_t) && face_edge_steps >= 32,
       "Face edge rounding must leave a continuous floor and lid face.");
assert(battery_w+8 < case_w && battery_h+8 < case_h,
       "Battery needs a larger case and a new clearance review.");
assert(battery_t >= 3 && battery_t <= 12, "Review depth outside this draft range.");
assert(mid_right_xy[1]-mount_post_d/2-(battery_xy[1]+battery_h) >= 0.7 &&
       pcb_z-mount_pilot_depth-mid_z >= 1,
       "Rear frame post needs battery clearance; the board pilot must leave a solid frame floor.");
assert(battery_xy[0]-(mid_third_xy[0]+mid_third_post_d/2) >= 0.75-0.001,
       "Battery would crowd the front-left midframe attachment.");
assert(mid_screw_d >= 2.3 && case_screw_head_d-mid_screw_d >= 1.5 &&
       min(mid_third_post_d,mount_post_d)-case_insert_od >= 2.4 &&
       min(mid_third_post_d,mount_post_d) >= case_screw_head_d+1,
       "Midframe holes, screw-head lands, and insert-post walls need positive margins.");
assert(mid_top-mid_screw_l <= mid_z-case_insert_l &&
       mid_top-mid_screw_l >= mid_tip_relief_bottom+0.5-0.001 &&
       mid_tip_relief_bottom-floor_t >= 4 &&
       mid_z-case_insert_bore_depth > mid_tip_relief_bottom,
       "Midframe screws must cross the insert fully without bottoming or opening the base.");
for(p=[mid_third_xy,mid_right_xy])
    assert(outline_edge_distance(p)-wall-mid_fit_gap-case_screw_head_d/2 >= 0.5,
           "The midframe screw head would approach the plate perimeter too closely.");
assert(norm(mid_third_xy-boss_xy[0])-
       (max(boss_d[0],case_insert_pad_d)/2+mid_fit_gap)-case_screw_head_d/2 >= 0.3,
       "The front frame screw head would overlap the neighboring case-post notch.");
assert(boss_xy[3][0]-boss_d[3]/2-(battery_xy[0]+battery_w) >= 0.5,
       "Battery would crowd the right-side lid boss.");
assert(case_insert_bore_d > case_screw_clear_d && case_insert_bore_d < case_insert_od,
       "Review the selected insert's pilot and the mating screw clearance.");
assert(case_insert_bore_depth >= case_insert_l &&
       case_tip_relief_top > mid_top+case_insert_bore_depth &&
       case_tip_relief_top <= joint_z-2,
       "Insert and screw-tip pocket must remain blind beneath the front face.");
assert(case_insert_pad_top >= mid_top+case_insert_bore_depth &&
       case_insert_taper_top < pcb_z && case_insert_taper_top > case_insert_pad_top,
       "The larger right insert pad must narrow before reaching the PCB.");
assert(case_insert_pad_d-case_insert_od >= 2 &&
       case_insert_pad_d/2 <= 3,
       "The insert pad needs surrounding material and must fit the curved corner.");
assert(case_head_recess_depth < mid_top-3 &&
       case_head_recess_depth >= case_screw_head_h &&
       case_head_recess_d >= case_screw_head_d+0.4,
       "Review the nylon screw head recess and compression-column seat.");
assert(case_well_pad_top >= case_head_recess_depth+1.3-0.001 &&
       case_well_pad_top <= mid_z-mid_ledge_t &&
       case_well_pad_d >= case_head_recess_d+2,
       "Deep screw wells need a supported head seat and must stop below the frame.");
assert(case_right_well_pad_d-case_head_recess_d >= 1.5 &&
       boss_xy[3][0]+case_right_well_pad_d/2 <= case_w &&
       boss_xy[3][0]-case_right_well_pad_d/2-(battery_xy[0]+battery_w) >= 0.5-0.001,
       "The right well needs a fully closed sleeve and battery clearance.");
for(i=[0:len(boss_xy)-1])
    assert(norm([max(battery_xy[0]-boss_xy[i][0],0,
                     boss_xy[i][0]-battery_xy[0]-battery_w),
                 max(battery_xy[1]-boss_xy[i][1],0,
                     boss_xy[i][1]-battery_xy[1]-battery_h)])
           -(i==3 ? case_right_well_pad_d : case_well_pad_d)/2 >= 0.5-0.001,
           "A lower screw-well footprint would crowd the battery.");
assert(case_head_recess_depth+case_screw_l >= mid_top+case_insert_l &&
       case_head_recess_depth+case_screw_l < case_tip_relief_top-0.5,
       "Review screw length: engage the insert without bottoming out.");
assert(battery_xy[1]+battery_h+1 <= upper_body_h-wheel_grip_depth-wheel_lower_wall+0.001,
       "Battery needs clearance inside the lower housing shoulder.");
assert(usb_wall_x-lower_usb_wall_t-(battery_xy[0]+battery_w) >= 0.7-0.001,
       "The inset lower USB wall would crowd the battery.");
assert(usb_wall_x-lower_usb_wall_t-(battery_xy[0]+battery_w+mid_battery_relief)-mid_fit_gap >= 0.2-0.001,
       "The narrowed USB-side shelf must still overlap the removable frame.");
assert(upper_body_h-upper_wall >= pcb_offset[1]+37+0.25,
       "Upper wall would crowd the PCB edge beside the wheel.");
assert(wheel_z-mid_top >= 0.5,
       "Midframe would crowd the wheel; review installed component height.");
assert(wheel_grip_depth == 1 && wheel_grip_blend >= 2 &&
       wheel_lower_wall >= 1.5 && wheel_upper_wall >= 1.1 && wheel_hardware_wall >= 0.6,
       "Review the reinforced wall layers for this uniform wheel-edge profile.");
assert(upper_body_h-wheel_grip_depth-wheel_hardware_wall >= pcb_offset[1]+37+0.3 &&
       upper_body_h-wheel_grip_depth-wheel_hardware_wall >= wheel_xy[1]+4.2+0.3 &&
       wheel_hardware_z[0] <= wheel_z+wheel_dial_t &&
       wheel_hardware_z[1] >= pcb_z+pcb_t+0.3-0.001,
       "The thin hardware band must clear the PCB edge and provisional encoder hub.");
assert(wheel_slot_w >= 2*sqrt(pow(wheel_d/2,2)-
                            pow(upper_body_h-wheel_grip_depth-wheel_upper_wall-wheel_xy[1],2))+0.8,
       "The wider wheel aperture must clear the reinforced-wall dial chord.");
assert(abs(boss_xy[3][1]+3.5-(upper_body_h-wheel_grip_depth)) < 0.001 &&
       right_blend_center()[1] >= usb_case_y[1]+usb_slot_w/2+0.4 &&
       right_blend_center()[1] > usb_rail_y[1],
       "The aligned corner must preserve the USB opening and the base rail end.");
assert(upper_body_h-wheel_grip_depth-wheel_upper_wall >=
       pcb_lift_stop_xy[1]+pcb_lift_stop_size[1]/2+0.5-0.001,
       "The PCB travel-stop foot must clear the reinforced wall.");
assert(usb_wall_x-usb_wall_t-xy([0,0])[0] >= 0.299,
       "Flush USB wall needs at least 0.3 mm nominal PCB-edge clearance.");
assert(usb_face_x-usb_wall_x >= 0 && usb_face_x-usb_wall_x <= 0.5,
       "Review socket projection beyond the USB face.");
assert(usb_cable_relief_z[0] >= case_insert_taper_top &&
       usb_cable_relief_z[1] < joint_z &&
       usb_cable_relief_y[1] <= boss_xy[3][1]-boss_d[3]/2-0.19,
       "The cable scallop must preserve the insert pad, full upper post, and top face.");
assert(usb_plug_front_x >= usb_cable_relief_x[0]+usb_cable_relief_xy_r-0.001 &&
       usb_cable_relief_y[1]-(usb_case_y[1]+usb_plug_body_w/2) >= 0.2 &&
       usb_center_z-usb_plug_body_h/2 >= usb_cable_relief_z[0]+usb_cable_relief_yz_r+0.2 &&
       usb_center_z+usb_plug_body_h/2 <= usb_cable_relief_z[1]-usb_cable_relief_yz_r-0.2,
       "Review the measured plug envelope, its assumed starting X, and scallop clearance.");
assert(lcd_fit_clearance >= 0.1 && lcd_fit_clearance <= 0.5,
       "Review LCD pocket clearance; do not press-fit the glass.");
assert(lcd_support_w > 0 && lcd_support_w < 1.45,
       "LCD ledge must remain under the module perimeter, outside the active area.");
assert(lcd_seat_depth > 0 && lcd_seat_z-lcd_support_t < joint_z,
       "LCD seat needs a positive depth and an underside support ledge.");
assert(lcd_seat_z-lcd_support_t > pcb_z+pcb_t,
       "LCD support collar would reach the PCB; review component clearance.");
assert(button_arm_t > 0 && button_stem_z < button_arm_z,
       "Review the button strip thickness and the stem-to-switch stack.");
assert(button_rest_gap > 0 && button_hole_d-button_key_d >= 0.79,
       "Buttons need positive rest clearance and room for cantilever tilt.");
assert(button_anchor_y-power_switch_xy[1]-power_guide_size[1]/2 >= 1.2-0.001,
       "Power flange guide must clear the existing button anchor.");
assert(power_slot_size[0]-power_nub_size[0] >=
       power_switch_travel+power_socket_inner-power_switch_handle[0]+0.49 &&
       power_slot_size[1]-power_nub_size[1] >= 0.59,
       "Power slider needs switch travel, socket play and resin running clearance.");
assert((power_flange_size[0]-power_slot_size[0]-(power_slot_size[0]-power_nub_size[0]))/2 >= 1 &&
       power_flange_size[1]-power_slot_size[1] >= 3.2-0.001,
       "Flange must overlap the lid slot at both travel limits.");
assert(power_flange_z[0]-power_retainer_z[1] >= 0.19 &&
       power_guide_top-power_flange_z[1] >= 0.19 &&
       power_socket_bottom-0.2-(pcb_z+pcb_t+power_switch_body[2]) >= 0.39 &&
       power_socket_top-0.2-27.3 >= 0.39,
       "Captive slider must float without loading the switch axially.");
assert(front_board_seat_x+3 <= battery_cable_bay_xy[0]-1.5+0.001,
       "Relocated front PCB support must remain on a substantial plate edge.");
assert(pcb_lift_gap >= 0.2 && pcb_lift_stop_z < pcb_lift_stop_shoulder_z &&
       pcb_lift_stop_shoulder_z < joint_z &&
       pcb_lift_stop_xy[0]+pcb_lift_stop_size[0]/2 <= 74 &&
       pcb_lift_stop_xy[1]+pcb_lift_stop_size[1]/2 <= 42.9+0.001,
       "PCB travel stop must retain its clear board land and positive gap.");
assert(usb_board_seat_xy[0] >= 69.3 &&
       usb_board_seat_xy[0]+usb_board_seat_size[0] <= 70.5 &&
       usb_board_seat_xy[1] >= 32.75 &&
       usb_board_seat_xy[1]+usb_board_seat_size[1] <= 34.75,
       "USB-side board seat must avoid SW1, SW6 pads and the USB locating hole.");
assert(button_protrusion >= 0 && button_protrusion <= 1.5 &&
       abs(button_arm_top+button_key_h-case_depth-button_protrusion) < 0.001,
       "Review the requested raised-key height without changing the actuator stack.");
assert(button_xy[0][0]-button_key_d/2 >
       lcd_pocket_xy[0]+lcd_pocket_w+lcd_collar_border,
       "Button tip would crowd the LCD support collar.");
echo("PROVISIONAL outside dimensions", [case_w,case_h,case_depth]);
echo("LCD pocket and seat", [lcd_pocket_w,lcd_pocket_h,lcd_seat_z]);
echo("Nominal key actuation travel", button_rest_gap+button_switch_travel);
echo("Raised key top and projection", [button_arm_top+button_key_h,button_protrusion]);
echo("Wheel exterior relief and centerline projection",
     [wheel_grip_depth,wheel_xy[1]+wheel_d/2-upper_body_h+wheel_grip_depth]);
echo("Wheel wall layers and thin hardware band",
     [wheel_lower_wall,wheel_upper_wall,wheel_hardware_wall,wheel_hardware_z]);
echo("Measured rear USB plug assumption and relieved rear gap",
     [usb_plug_body_w,usb_plug_body_h,usb_plug_front_x,
      usb_cable_relief_y[1]-usb_case_y[1]-usb_plug_body_w/2]);

module rounded2d(w,h,r) {
    translate([r,r]) offset(r=r) square([w-2*r,h-2*r]);
}
module roundedbox(w,h,d,r) {
    linear_extrude(d) rounded2d(w,h,r);
}
module rounded_face_edge(z0,r,top=true) {
    // A quarter-circle profile, sampled into 64 native 2D-offset slices.
    // The largest stair step is under 0.050 mm at R2. Native offset resolves
    // the tight, concave USB outline; a global convex hull would fill it in.
    // top: full outline at z0, inset r at z0+r. Bottom is the Z-mirror.
    for(i=[0:face_edge_steps-1])
        let(a0=90*i/face_edge_steps,
            a1=90*(i+1)/face_edge_steps,
            inset=r*(1-cos(a1)),
            zlo=top ? z0+r*sin(a0) : z0+r*(1-sin(a1)),
            h=r*(sin(a1)-sin(a0)))
            translate([0,0,zlo]) linear_extrude(h)
                offset(delta=-inset) children();
}
function arc_points(c,r,a0,a1,n=12) =
    [for(i=[0:n]) let(a=a0+(a1-a0)*i/n)
        [c[0]+r*cos(a),c[1]+r*sin(a)]];
function bezier_point(a,b,c,d,t) =
    (1-t)*(1-t)*(1-t)*a + 3*(1-t)*(1-t)*t*b +
    3*(1-t)*t*t*c + t*t*t*d;
function clean_outline_points(p) =
    [for(i=[0:len(p)-1]) if(norm(p[i]-p[(i+len(p)-1)%len(p)])>0.000001) p[i]];
function point_segment_distance(p,a,b) =
    let(v=b-a,t=max(0,min(1,((p-a)*v)/(v*v)))) norm(p-(a+t*v));
function outline_edge_distance(p) =
    let(q=clean_outline_points(nominal_body_points()))
        min([for(i=[0:len(q)-1]) point_segment_distance(p,q[i],q[(i+1)%len(q)])]);
function smooth_unit(t) = let(u=max(0,min(1,t))) u*u*(3-2*u);
function wheel_grip_weight(x) =
    smooth_unit((x-wheel_grip_span[0]+wheel_grip_blend)/wheel_grip_blend);
function densify_rear_points(p) =
    [for(i=[0:len(p)-1])
        let(a=p[i],b=p[(i+1)%len(p)],
            n=a[1]>=upper_body_h-0.000001 && b[1]>=upper_body_h-0.000001 ?
                max(1,ceil(norm(b-a)/0.25)) : 1)
            for(j=[0:n-1]) a+(b-a)*j/n];
function densify_open_rear_points(p) =
    concat([for(i=[0:len(p)-2])
        let(a=p[i],b=p[i+1],n=max(1,ceil(norm(b-a)/0.25)))
            for(j=[0:n-1]) a+(b-a)*j/n], [p[len(p)-1]]);
function right_blend_center() =
    let(r=0.3,R=case_right_well_pad_d/2,x=usb_wall_x+r)
        [x,boss_xy[3][1]-sqrt(pow(R+r,2)-pow(boss_xy[3][0]-x,2))];
function right_blend_angle() =
    atan2(right_blend_center()[1]-boss_xy[3][1],
          right_blend_center()[0]-boss_xy[3][0]);
module body_outline(h) {
    // Retain the helper signature for existing floor/fillet calls. Both halves
    // now deliberately use the same complete outline, including the USB side.
    polygon(clean_outline_points(upper_body_points()));
}
function usb_entry_outer(t) = bezier_point([80,5],[80,7.5],
                                          [75.8,8.5],[75.8,11],t);
function nominal_body_points(rear_steps=24,straight_steps=1) = concat(
    arc_points([5,5],5,180,270),
    arc_points([case_w-5,5],5,270,360),
    [for(i=[1:24]) usb_entry_outer(i/24)],
    [[usb_wall_x,38.1]],
    arc_points([76.2,38.1],0.4,180,90),
    [[77,38.5]],
    arc_points([77,41.5],3,-90,0),
    arc_points([76,41.5],4,0,90),
    [for(i=[1:straight_steps]) [76+(59-76)*i/straight_steps,upper_body_h]],
    [for(i=[1:rear_steps]) bezier_point([59,upper_body_h],[50,upper_body_h],
                                [48,case_h],[38,case_h],i/rear_steps)],
    arc_points([5,case_h-5],5,90,180)
);
function upper_body_points() = concat(
    arc_points([5,5],5,180,270),
    arc_points([case_w-5,5],5,270,360),
    [for(i=[1:24]) usb_entry_outer(i/24)],
    [[usb_wall_x,right_blend_center()[1]]],
    arc_points(right_blend_center(),0.3,180,right_blend_angle()+180,16),
    arc_points(boss_xy[3],case_right_well_pad_d/2,right_blend_angle(),0,48),
    arc_points([case_w-3.5,boss_xy[3][1]],3.5,0,90,36),
    // Subdivide the prior rear polygon linearly before the left smoothstep.
    // The full 1 mm relief continues to the R3.5 corner without another bump.
    [for(p=densify_open_rear_points(concat(
        [[case_w-3.5,upper_body_h],[59,upper_body_h]],
        [for(i=[1:24]) bezier_point([59,upper_body_h],[50,upper_body_h],
                                   [48,case_h],[38,case_h],i/24)],
        arc_points([5,case_h-5],5,90,180))))
        p-[0,wheel_grip_depth*wheel_grip_weight(p[0])]]
);
module upper_outline() {
    polygon(clean_outline_points(upper_body_points()));
}
module nominal_outline() {
    // Keep the exact previous cavity polygon, including its original sampling.
    // Local reinforcement clips this baseline below; unrelated clearances stay put.
    polygon(clean_outline_points(nominal_body_points()));
}
module usb_interior_region() {
    translate([usb_inner_region[0],usb_inner_region[1]])
        square([usb_inner_region[2]-usb_inner_region[0],
                usb_inner_region[3]-usb_inner_region[1]]);
}
module lower_interior_nominal() {
    // Native offsets retain the concave shoulder. The USB side is 1.6 mm
    // thick, with a 2 mm wall around the rest of the battery compartment.
    union() {
        offset(delta=-wall) nominal_outline();
        intersection() {
            offset(delta=-lower_usb_wall_t) nominal_outline();
            usb_interior_region();
        }
    }
}
module lower_interior() {
    intersection() {
        lower_interior_nominal();
        offset(delta=-wheel_lower_wall) upper_outline();
    }
}
module upper_interior_nominal() {
    union() {
        offset(delta=-upper_wall) nominal_outline();
        intersection() {
            offset(delta=-usb_wall_t) nominal_outline();
            usb_interior_region();
        }
    }
}
module upper_interior(hardware_band=false) {
    intersection() {
        upper_interior_nominal();
        offset(delta=-wheel_hardware_wall) upper_outline();
        if(!hardware_band) union() {
            offset(delta=-wheel_upper_wall) upper_outline();
            // Preserve the USB side's existing 1 mm wall outside the rear band.
            translate([-1,-1]) square([case_w+2,wheel_reinforce_y+1]);
        }
    }
}
module upper_shell_walls(z0,z1) {
    // Local Z uses the lid joint. The thin central band clears both the
    // encoder hub and the PCB; stronger material remains above and below.
    cuts=[z0,max(z0,min(z1,wheel_hardware_z[0]-mid_top)),
          max(z0,min(z1,wheel_hardware_z[1]-mid_top)),z1];
    for(i=[0:2]) if(cuts[i+1]>cuts[i])
        translate([0,0,cuts[i]]) linear_extrude(cuts[i+1]-cuts[i])
            difference() {
                upper_outline();
                upper_interior(i==1);
            }
}
module shell_walls(h,z0,z1,t) {
    translate([0,0,z0]) linear_extrude(z1-z0)
        difference() {
            body_outline(h);
            lower_interior();
        }
}
module case_lower_well_outline() {
    intersection() {
        body_outline(lower_body_h);
        union() for(i=[0:len(boss_xy)-1]) translate(boss_xy[i])
            circle(d=i==3 ? case_right_well_pad_d : case_well_pad_d);
    }
}
module base_floor() {
    // Preserve the closed screw-access rims locally. R2 around the complete
    // lower perimeter would break through the tight corner/right head bores.
    union() {
        rounded_face_edge(0,face_edge_r,false) body_outline(lower_body_h);
        linear_extrude(face_edge_r) case_lower_well_outline();
        if(floor_t>face_edge_r)
            translate([0,0,face_edge_r]) linear_extrude(floor_t-face_edge_r)
                body_outline(lower_body_h);
    }
}
module screw_bosses() {
    // Original upper compression columns retain their frame clearances.
    // Wider lower pads support the deep M2 x 8 head seats. Every access bore
    // has a closed sleeve separating the screw head from the battery bay.
    for(i=[0:len(boss_xy)-1])
        translate([boss_xy[i][0],boss_xy[i][1],floor_t-0.01])
            cylinder(h=mid_top-floor_t+0.01,d=boss_d[i]);
    translate([0,0,floor_t-0.01])
        linear_extrude(case_well_pad_top-floor_t+0.01)
            case_lower_well_outline();
}
module case_bottom_screw_cutouts() {
    // Screws enter from outside the base. Their heads bear at Z10.2, so the
    // user's 8 mm shafts reach Z18.2 through the 3.2 mm inserts at Z14.5.
    for(p=boss_xy) {
        translate([p[0],p[1],-0.1])
            cylinder(h=case_head_recess_depth+0.1,d=case_head_recess_d);
        translate([p[0],p[1],case_head_recess_depth-0.01])
            cylinder(h=mid_top-case_head_recess_depth+0.11,d=case_screw_clear_d);
    }
}
module case_upper_insert_posts() {
    // Global coordinates. Only the short right pad grows to hold the insert;
    // its upper column stays narrow beside the PCB and USB return contour.
    for(i=[0:len(boss_xy)-1])
        translate([boss_xy[i][0],boss_xy[i][1],0]) {
            translate([0,0,mid_top])
                cylinder(h=case_insert_pad_top-mid_top,d=case_insert_pad_d);
            translate([0,0,case_insert_pad_top-0.01])
                cylinder(h=case_insert_taper_top-case_insert_pad_top+0.02,
                         d1=case_insert_pad_d,d2=boss_d[i]);
            translate([0,0,case_insert_taper_top])
                cylinder(h=joint_z-case_insert_taper_top+0.01,d=boss_d[i]);
        }
}
module case_upper_insert_cutouts() {
    // Insert entry is on the cover's underside, never through its top face.
    // Actual OD3 x L3.2 insert is user-specified; test the 2.7 mm pilot in the
    // chosen printing material before heat-setting the finished enclosure.
    for(p=boss_xy) {
        translate([p[0],p[1],mid_top-0.1])
            cylinder(h=case_insert_bore_depth+0.1,d=case_insert_bore_d);
        translate([p[0],p[1],mid_top-0.01])
            cylinder(h=case_insert_leadin_depth+0.01,
                     d1=case_insert_leadin_d,d2=case_insert_bore_d);
        translate([p[0],p[1],mid_top+case_insert_bore_depth-0.01])
            cylinder(h=case_tip_relief_top-mid_top-case_insert_bore_depth+0.01,
                     d=case_tip_relief_d);
    }
}
module battery_locators() {
    // Low perimeter guides only; trim guides around posts, never the posts.
    difference() {
        translate([battery_xy[0]-2,battery_xy[1]-2,floor_t-0.01])
            difference() {
                roundedbox(battery_w+4,battery_h+4,1.4,2);
                translate([1,1,-0.1]) roundedbox(battery_w+2,battery_h+2,1.6,1);
                // Lead exit; confirm against the selected pack.
                translate([-1,4,-0.1]) cube([5,10,2]);
            }
        translate([mid_right_xy[0],mid_right_xy[1],floor_t-0.1])
            cylinder(h=1.6,r=mount_post_d/2+0.4);
        translate([mid_third_xy[0],mid_third_xy[1],floor_t-0.1])
            cylinder(h=1.6,r=mid_third_post_d/2+0.4);
        for(i=[0:len(boss_xy)-1])
            translate([boss_xy[i][0],boss_xy[i][1],floor_t-0.1])
                cylinder(h=1.6,r=(i==3 ? case_right_well_pad_d : case_well_pad_d)/2+0.4);
    }
}
module midframe_standoffs() {
    // The two frame inserts enter from above these base-owned posts. Their
    // blind bores are cut after the complete base union so ribs/ledge cannot
    // partially obstruct an insert entry. The PCB pilot remains on the frame.
    translate([mid_right_xy[0],mid_right_xy[1],floor_t-0.01])
        cylinder(h=mid_z-floor_t+0.01,d=mount_post_d);
    translate([mid_third_xy[0],mid_third_xy[1],floor_t-0.01])
        cylinder(h=mid_z-floor_t+0.01,d=mid_third_post_d);
    translate([wall-0.01,mid_third_xy[1]-0.8,floor_t-0.01])
        cube([mid_third_xy[0]-wall+0.01,1.6,4.01]);
    // Short rear-facing rib stays behind the pack. Its end overlaps the inner
    // wall by 0.2 mm, rather than extending to the exterior shoulder surface.
    intersection() {
        translate([0,0,floor_t-0.01]) linear_extrude(4.01)
            offset(delta=0.2) lower_interior();
        translate([mid_right_xy[0]-0.8,mid_right_xy[1],floor_t-0.01])
            cube([1.6,case_h-mid_right_xy[1],4.01]);
    }
}
module midframe_insert_cutouts() {
    // Same OD3 x L3.2 inserts as the case, opening upward at the Z13 post tops.
    // Installed inserts occupy Z9.8..13; provisional pilots extend to Z9.6.
    for(p=[mid_third_xy,mid_right_xy]) {
        translate([p[0],p[1],mid_z-case_insert_bore_depth])
            cylinder(h=case_insert_bore_depth+0.1,d=case_insert_bore_d);
        translate([p[0],p[1],mid_z-case_insert_leadin_depth])
            cylinder(h=case_insert_leadin_depth+0.01,
                     d1=case_insert_bore_d,d2=case_insert_leadin_d);
        translate([p[0],p[1],mid_tip_relief_bottom])
            cylinder(h=mid_z-case_insert_bore_depth-mid_tip_relief_bottom+0.01,
                     d=case_tip_relief_d);
    }
}
module midframe_ledge() {
    // The frame drops onto this shelf at Z13. Relief clears the pack's upper
    // millimeter: USB-side shelf is 0.5 mm wide (0.2 mm frame overlap), rear
    // shelf 0.8 mm wide (0.5 mm overlap); wider support remains elsewhere.
    translate([0,0,mid_z-mid_ledge_t]) linear_extrude(mid_ledge_t)
        difference() {
            offset(delta=0.01) lower_interior();
            offset(delta=-mid_ledge_w) lower_interior();
            translate([battery_xy[0]-mid_battery_relief,
                       battery_xy[1]-mid_battery_relief])
                square([battery_w+2*mid_battery_relief,battery_h+2*mid_battery_relief]);
        }
    translate([0,0,mid_z-mid_ledge_t]) linear_extrude(mid_ledge_t)
        difference() {
            intersection() {
                body_outline(lower_body_h);
                union() for(i=[0:len(boss_xy)-1])
                    translate(boss_xy[i]) circle(r=boss_d[i]/2+0.7);
            }
            // Only the support shoulders are relieved; the case bosses remain.
            translate([battery_xy[0]-0.7,battery_xy[1]-0.7])
                square([battery_w+1.4,battery_h+1.4]);
        }
}
module board_edge_seats() {
    // Provisional, sparse seats; check actual component-free edge areas.
    for(x=[10]) {
        translate([x-1.5,42.7,mid_top-0.01])
            cube([3,1.2,pcb_z-mid_top+0.01]);
        translate([x-1.5,43.9,mid_top-0.01])
            cube([3,1.2,pcb_z+pcb_t-0.2-mid_top+0.01]);
    }
    translate([usb_board_seat_xy[0],usb_board_seat_xy[1],mid_top-0.01])
        cube([usb_board_seat_size[0],usb_board_seat_size[1],pcb_z-mid_top+0.01]);
    translate([front_board_seat_x,6.1,mid_top-0.01])
        cube([3,1.4,pcb_z-mid_top+0.01]);
    translate([front_board_seat_x,5.0,mid_top-0.01])
        cube([3,1.1,pcb_z+pcb_t-0.2-mid_top+0.01]);
    // Solid locator in the empty lower-left PCB notch replaces the thin L.
    // Its right and rear faces retain 0.4 mm clearance to the sourced outline.
    notch_locator_xy = [8,8];
    notch_locator_size = [8.5,3.7];
    assert(notch_locator_xy[0]+notch_locator_size[0]+0.4 <= xy([57.6,0])[0]+0.001 &&
           notch_locator_xy[1]+notch_locator_size[1]+0.4 <= xy([69,5.6])[1]+0.001,
           "The solid board locator must remain inside the empty PCB notch.");
    translate([notch_locator_xy[0],notch_locator_xy[1],mid_top-0.01])
        cube([notch_locator_size[0],notch_locator_size[1],
              pcb_z+pcb_t-0.2-mid_top+0.01]);
}
module battery_cable_passage() {
    // Full-height cut prevents a floating remnant if a seat is later moved.
    translate([battery_cable_bay_xy[0],battery_cable_bay_xy[1],mid_z-0.1])
        linear_extrude(pcb_z+pcb_t-mid_z+0.2)
            rounded2d(battery_cable_bay_size[0],battery_cable_bay_size[1],
                      battery_cable_bay_r);
}
module lid_alignment_keys() {
    for(k=lid_key_rects) {
        translate([k[0],k[1],mid_top-0.01])
            cube([k[2],k[3],lid_key_h-lid_key_tip_h+0.01]);
        hull() {
            translate([k[0],k[1],mid_top+lid_key_h-lid_key_tip_h-0.01])
                cube([k[2],k[3],0.01]);
            translate([k[0]+lid_key_tip_chamfer,k[1]+lid_key_tip_chamfer,
                       mid_top+lid_key_h-0.01])
                cube([k[2]-2*lid_key_tip_chamfer,k[3]-2*lid_key_tip_chamfer,0.01]);
        }
    }
}
module lid_alignment_receivers() {
    for(r=lid_receiver_rects)
        translate([r[0],r[1],mid_top]) cube([r[2],r[3],lid_receiver_h]);
}
module lid_alignment_pockets() {
    for(k=lid_key_rects) {
        c=lid_key_clearance;
        f=lid_key_mouth_flare;
        translate([k[0]-c,k[1]-c,mid_top-0.1])
            cube([k[2]+2*c,k[3]+2*c,lid_key_h+lid_key_top_gap+0.1]);
        hull() {
            translate([k[0]-c-f,k[1]-c-f,mid_top-0.01])
                cube([k[2]+2*(c+f),k[3]+2*(c+f),0.01]);
            translate([k[0]-c,k[1]-c,mid_top+lid_key_mouth_h-0.01])
                cube([k[2]+2*c,k[3]+2*c,0.01]);
        }
    }
}
module midframe() {
    difference() {
        union() {
            translate([0,0,mid_z]) linear_extrude(mid_t)
                offset(delta=-mid_fit_gap) lower_interior();
            translate([mount_xy[0],mount_xy[1],mid_top-0.01])
                cylinder(h=pcb_z-mid_top+0.01,d=mount_post_d);
            board_edge_seats();
            lid_alignment_keys();
        }
        // Blind board pilot leaves 1.7 mm over the flat underside at Z13.
        translate([mount_xy[0],mount_xy[1],pcb_z-mount_pilot_depth])
            cylinder(h=mount_pilot_depth+0.1,d=mount_pilot_d);
        for(p=[mid_right_xy,mid_third_xy])
            translate([p[0],p[1],mid_z-0.1])
                cylinder(h=mid_t+0.2,d=mid_screw_d);
        for(i=[0:len(boss_xy)-1])
            translate([boss_xy[i][0],boss_xy[i][1],mid_z-0.1])
                cylinder(h=mid_t+0.2,
                         r=max(boss_d[i],case_insert_pad_d)/2+mid_fit_gap);
        if(usb_apertures) usb_port_apertures();
        battery_cable_passage();
    }
}
module usb_lower_saddle() {
    // Base-owned open-top USB surround. The board and midframe drop into it
    // together; the upper cover closes the ports without trapping the board.
    translate([usb_wall_x-usb_wall_t,usb_rail_y[0],mid_top-0.01])
        cube([usb_wall_t,usb_rail_y[1]-usb_rail_y[0],
              usb_rail_top-mid_top+0.01]);
    // Brace inward below the PCB, entirely inside the common outer contour.
    translate([0,usb_rail_y[1],0]) rotate([90,0,0])
        linear_extrude(usb_rail_y[1]-usb_rail_y[0])
            polygon([[usb_wall_x-usb_wall_t+0.01,mid_top-0.01],
                     [usb_wall_x-lower_usb_wall_t,mid_top-0.01],
                     [usb_wall_x-usb_wall_t+0.01,mid_top+3]]);
}
module usb_port_apertures() {
    // These close-fitting windows clear the metal socket shell. Its face projects
    // 0.2 mm beyond the inset side so the cable no longer enters a 4 mm deep well.
    for(y=usb_case_y)
        translate([usb_wall_x-usb_wall_t-0.5,
                   y-usb_slot_w/2,usb_center_z-usb_slot_h/2])
            rotate([90,0,90]) linear_extrude(usb_wall_t+1)
                rounded2d(usb_slot_w,usb_slot_h,usb_slot_r);
}
module usb_cable_relief() {
    // Local exterior scallop beside the rear USB port, at plug height only.
    // The screw axis remains (77,41), with its full Ø4.4 upper column intact.
    // Intersect XY and YZ rounded cuts to protect the interior corner and soften
    // the upper/lower ends; the measured 10.25 x 6 overmold clears the full-depth band.
    intersection() {
        translate([0,0,usb_cable_relief_z[0]])
            linear_extrude(usb_cable_relief_z[1]-usb_cable_relief_z[0])
                polygon(clean_outline_points(concat(
                    [[usb_cable_relief_x[0],usb_cable_relief_y[0]],
                     [usb_cable_relief_x[1],usb_cable_relief_y[0]],
                     [usb_cable_relief_x[1],usb_cable_relief_y[1]],
                     [usb_cable_relief_x[0]+usb_cable_relief_xy_r,usb_cable_relief_y[1]]],
                    arc_points([usb_cable_relief_x[0]+usb_cable_relief_xy_r,
                                usb_cable_relief_y[1]-usb_cable_relief_xy_r],
                               usb_cable_relief_xy_r,90,180))));
        translate([usb_cable_relief_x[0],usb_cable_relief_y[0],usb_cable_relief_z[0]])
            rotate([90,0,90])
                linear_extrude(usb_cable_relief_x[1]-usb_cable_relief_x[0])
                    rounded2d(usb_cable_relief_y[1]-usb_cable_relief_y[0],
                              usb_cable_relief_z[1]-usb_cable_relief_z[0],
                              usb_cable_relief_yz_r);
    }
}
module wheel_access_slot() {
    // Horizontal aperture around the wheel; no full-height finger well.
    // XZ rounded rectangle extruded from Y42.5 to Y54 at nominal dimensions.
    translate([wheel_xy[0]-wheel_slot_w/2,
               wheel_slot_y+wheel_slot_depth,wheel_slot_z])
        rotate([90,0,0])
            linear_extrude(wheel_slot_depth)
                rounded2d(wheel_slot_w,wheel_slot_h,0.5);
}
module base() {
    difference() {
        union() {
            base_floor();
            shell_walls(lower_body_h,floor_t-0.01,mid_top,wall);
            screw_bosses();
            battery_locators();
            midframe_standoffs();
            midframe_ledge();
            usb_lower_saddle();
        }
        case_bottom_screw_cutouts();
        midframe_insert_cutouts();
        if(usb_apertures) usb_port_apertures();
    }
}
module lcd_support_collar() {
    // Global coordinates. The rear frame rests at Z27.1 by default. The collar
    // joins the cover over Z27..27.1, rather than relying on a 0.1 mm skin.
    translate([lcd_pocket_xy[0]-lcd_collar_border,
               lcd_pocket_xy[1]-lcd_collar_border,lcd_seat_z-lcd_support_t])
        roundedbox(lcd_pocket_w+2*lcd_collar_border,
                   lcd_pocket_h+2*lcd_collar_border,
                   lcd_collar_top-lcd_seat_z+lcd_support_t,1);
}
module lcd_seat_cutouts() {
    // A full-module pocket from the front, with a large opening behind it.
    // Only a 1 mm perimeter of the rear frame bears on the 1.2 mm thick ledge.
    translate([lcd_pocket_xy[0],lcd_pocket_xy[1],lcd_seat_z])
        roundedbox(lcd_pocket_w,lcd_pocket_h,
                   case_depth-lcd_seat_z+0.1,0.3);
    translate([lcd_rear_xy[0],lcd_rear_xy[1],lcd_seat_z-lcd_support_t-0.1])
        roundedbox(lcd_rear_w,lcd_rear_h,
                   case_depth-lcd_seat_z+lcd_support_t+0.2,0.3);
    // Enlarged rounded relief at the -Y flex edge preserves the flush seat.
    // The extra depth and height reduce pinching without opening the top skin.
    translate([lcd_flex_xy[0],lcd_flex_xy[1],
               lcd_seat_z-lcd_support_t-0.1])
        roundedbox(lcd_flex_w,lcd_flex_depth,
                   lcd_flex_top_z-(lcd_seat_z-lcd_support_t-0.1),lcd_flex_r);
    // Rear access remains open for small silicone fillets at the frame/ledge
    // junction if needed. Keep adhesive off the glass, flex and active area.
}
module button_anchor_bosses() {
    // Global coordinates: the underside face locates the removable strip at
    // Z26.6. Pilots open from below, leaving a nominal 0.5 mm top skin.
    for(p=button_anchor_xy)
        translate([p[0],p[1],button_arm_top])
            cylinder(h=case_depth-0.01-button_arm_top,d=button_anchor_d);
}
module button_lid_cutouts() {
    for(p=button_xy)
        translate([p[0],p[1],joint_z-0.1])
            cylinder(h=lid_t+0.2,d=button_hole_d);
    for(p=button_anchor_xy)
        translate([p[0],p[1],button_arm_top-0.1])
            cylinder(h=case_depth-0.5-button_arm_top+0.1,d=button_pilot_d);
}
module button_strip_outline() {
    union() {
        translate([button_anchor_x,button_anchor_y])
            square([button_anchor_w,button_anchor_h]);
        for(p=button_xy) {
            translate([p[0],p[1]-button_arm_w/2])
                square([button_anchor_x+0.5-p[0],button_arm_w]);
            translate(p) circle(d=button_key_d);
            // Concave R0.3 root fillets on both sides of each independent arm.
            for(s=[-1,1])
                translate([button_anchor_x-button_root_r,
                           p[1]+s*(button_arm_w/2+button_root_r)])
                    difference() {
                        translate([0,s<0 ? 0 : -button_root_r])
                            square([button_root_r,button_root_r]);
                        circle(r=button_root_r);
                    }
        }
    }
}
module button_strip() {
    // One removable component: anchor bar, three elastic tongues, keys and
    // centered actuator stems. No loose plungers and no intended static load.
    // PETG is a candidate, not a qualified spring material. Print orientation,
    // return after repeated presses, creep, and screw lengths need verification.
    // No hard travel stop is claimed: the switch drawing gives operating travel
    // but does not establish allowable forced overtravel.
    difference() {
        union() {
            power_slider_retainer();
            translate([0,0,button_arm_z])
                linear_extrude(button_arm_t) button_strip_outline();
            for(p=button_xy) {
                translate([p[0],p[1],button_arm_top-0.01])
                    cylinder(h=button_key_h+0.01,d=button_key_d);
                translate([p[0],p[1],button_stem_z])
                    cylinder(h=button_arm_z-button_stem_z+0.01,d=button_stem_d);
            }
        }
        for(p=button_anchor_xy)
            translate([p[0],p[1],button_arm_z-0.1])
                cylinder(h=button_arm_t+0.2,d=button_screw_d);
    }
}
module power_slider_retainer() {
    // Captures the cap using the two existing strip screws. The shelf, rather
    // than the switch handle, carries nominal downward cap loads. Prototype:
    // validate resin strength and the short cantilever in a physical sample.
    difference() {
        union() {
            translate([power_switch_xy[0]-power_retainer_size[0]/2,
                       power_switch_xy[1]-power_retainer_size[1]/2,power_retainer_z[0]])
                roundedbox(power_retainer_size[0],power_retainer_size[1],
                           power_retainer_z[1]-power_retainer_z[0],1);
            translate([button_anchor_x,power_switch_xy[1]+power_retainer_size[1]/2-1,
                       power_retainer_z[0]])
                cube([button_anchor_w,button_anchor_y+0.6-
                      (power_switch_xy[1]+power_retainer_size[1]/2-1),
                      power_retainer_z[1]-power_retainer_z[0]]);
            // R0.6 roots between the broad shelf and its 5 mm fixing bridge.
            for(s=[-1,1]) {
                bx = button_anchor_x+(s<0 ? 0 : button_anchor_w);
                by = power_switch_xy[1]+power_retainer_size[1]/2;
                translate([0,0,power_retainer_z[0]]) linear_extrude(power_retainer_z[1]-power_retainer_z[0])
                    difference() {
                        translate([bx+(s<0 ? -0.6 : 0),by]) square([0.6,0.6]);
                        translate([bx+s*0.6,by+0.6]) circle(r=0.6);
                    }
            }
        }
        translate([power_switch_xy[0]-power_retainer_slot[0]/2,
                   power_switch_xy[1]-power_retainer_slot[1]/2,power_retainer_z[0]-0.1])
            roundedbox(power_retainer_slot[0],power_retainer_slot[1],
                       power_retainer_z[1]-power_retainer_z[0]+0.2,0.3);
    }
}
module power_switch_access() {
    // Small running slot and underside flange guide. The cap covers the slot;
    // this is an overlapping dust cover, not a gasket or water-resistant seal.
    translate([power_switch_xy[0]-power_slot_size[0]/2,
               power_switch_xy[1]-power_slot_size[1]/2,joint_z-0.1])
        roundedbox(power_slot_size[0],power_slot_size[1],lid_t+0.2,power_slot_r);
    translate([power_switch_xy[0]-power_guide_size[0]/2,
               power_switch_xy[1]-power_guide_size[1]/2,joint_z-0.1])
        roundedbox(power_guide_size[0],power_guide_size[1],
                   power_guide_top-joint_z+0.1,power_guide_r);
}
module power_slider(offset=power_slider_offset) {
    translate([power_switch_xy[0]+offset,power_switch_xy[1],0]) difference() {
        union() {
            translate([-power_flange_size[0]/2,-power_flange_size[1]/2,power_flange_z[0]])
                roundedbox(power_flange_size[0],power_flange_size[1],
                           power_flange_z[1]-power_flange_z[0],power_flange_r);
            translate([-power_socket_outer/2,-power_socket_outer/2,power_socket_bottom])
                roundedbox(power_socket_outer,power_socket_outer,
                           power_flange_z[0]-power_socket_bottom+0.01,0.3);
            translate([-power_nub_size[0]/2,-power_nub_size[1]/2,power_flange_z[1]-0.01])
                roundedbox(power_nub_size[0],power_nub_size[1],
                           power_slider_top-0.3-power_flange_z[1]+0.01,power_nub_r);
            hull() {
                translate([-power_nub_size[0]/2,-power_nub_size[1]/2,power_slider_top-0.31])
                    roundedbox(power_nub_size[0],power_nub_size[1],0.01,power_nub_r);
                translate([-power_nub_size[0]/2+0.3,-power_nub_size[1]/2+0.3,power_slider_top-0.01])
                    roundedbox(power_nub_size[0]-0.6,power_nub_size[1]-0.6,0.01,power_nub_r-0.3);
            }
        }
        translate([-power_socket_inner/2,-power_socket_inner/2,power_socket_bottom-0.1])
            cube([power_socket_inner,power_socket_inner,power_socket_top-power_socket_bottom+0.1]);
        // Small lead-in for dropping the cover over the original slider.
        hull() {
            translate([-power_socket_inner/2-0.2,-power_socket_inner/2-0.2,power_socket_bottom-0.01])
                cube([power_socket_inner+0.4,power_socket_inner+0.4,0.01]);
            translate([-power_socket_inner/2,-power_socket_inner/2,power_socket_bottom+0.19])
                cube([power_socket_inner,power_socket_inner,0.01]);
        }
    }
}
module pcb_anti_lift_stop() {
    // Flat R0.2 foot; broad R0.4 root joins the roof and wheel-side wall.
    // The lower foot hovers .2 above component-free PCB at the far USB corner.
    // It is intentionally separate from the underside support near SW1.
    translate([pcb_lift_stop_xy[0]-pcb_lift_stop_size[0]/2,
               pcb_lift_stop_xy[1]-pcb_lift_stop_size[1]/2,pcb_lift_stop_z])
        roundedbox(pcb_lift_stop_size[0],pcb_lift_stop_size[1],0.4,0.2);
    hull() {
        translate([pcb_lift_stop_xy[0]-pcb_lift_stop_size[0]/2,
                   pcb_lift_stop_xy[1]-pcb_lift_stop_size[1]/2,pcb_lift_stop_z+0.39])
            roundedbox(pcb_lift_stop_size[0],pcb_lift_stop_size[1],0.01,0.2);
        translate([pcb_lift_stop_xy[0]-pcb_lift_stop_root[0]/2,
                   pcb_lift_stop_xy[1]-pcb_lift_stop_root[1]/2,pcb_lift_stop_shoulder_z-0.01])
            roundedbox(pcb_lift_stop_root[0],pcb_lift_stop_root[1],0.01,0.4);
    }
    translate([pcb_lift_stop_xy[0]-pcb_lift_stop_root[0]/2,
               pcb_lift_stop_xy[1]-pcb_lift_stop_root[1]/2,pcb_lift_stop_shoulder_z-0.01])
        roundedbox(pcb_lift_stop_root[0],pcb_lift_stop_root[1],
                   joint_z+0.1-pcb_lift_stop_shoulder_z+0.01,0.4);
}
module lid() {
    // Local Z0 is the joint at global Z14.5. Remove this complete upper cover
    // before dropping in the full-size frame, then lower it over the PCB.
    // M2 case screws now enter from the base into blind underside inserts.
    difference() {
        union() {
            upper_shell_walls(0,joint_z-mid_top);
            if(lid_t>face_edge_r)
                translate([0,0,joint_z-mid_top])
                    linear_extrude(lid_t-face_edge_r) upper_outline();
            rounded_face_edge(case_depth-mid_top-face_edge_r,face_edge_r,true)
                upper_outline();
            translate([0,0,-mid_top]) case_upper_insert_posts();
            if(lcd_aperture) translate([0,0,-mid_top]) lcd_support_collar();
            if(button_keys) translate([0,0,-mid_top]) button_anchor_bosses();
            translate([0,0,-mid_top]) lid_alignment_receivers();
            translate([0,0,-mid_top]) pcb_anti_lift_stop();
        }
        translate([0,0,-mid_top]) case_upper_insert_cutouts();
        translate([0,0,-mid_top]) lid_alignment_pockets();
        translate([0,0,-mid_top]) wheel_access_slot();
        // The rail is part of the base. Split the USB wall at socket
        // center height, with a 0.2 mm seam, for straight vertical disassembly.
        translate([usb_wall_x-usb_wall_t-0.1,usb_panel_y[0],-0.1])
            cube([usb_wall_t+0.3,usb_panel_y[1]-usb_panel_y[0],
                  usb_lid_bottom-mid_top+0.1]);
        if(usb_apertures) translate([0,0,-mid_top]) usb_port_apertures();
        if(usb_apertures) translate([0,0,-mid_top]) usb_cable_relief();
        if(lcd_aperture) translate([0,0,-mid_top]) lcd_seat_cutouts();
        if(button_keys) translate([0,0,-mid_top]) button_lid_cutouts();
        translate([0,0,-mid_top]) power_switch_access();
    }
}
module board_reference() {
    difference() {
        linear_extrude(pcb_t) polygon([for(p=pcb_outline) xy(p)]);
        translate([mount_xy[0],mount_xy[1],-0.1])
            cylinder(h=pcb_t+0.2,d=mount_pcb_hole_d);
    }
}
module battery_reference() {
    translate([battery_xy[0],battery_xy[1],battery_z])
        roundedbox(battery_w,battery_h,battery_t,1);
}
module wheel_reference() {
    // Dial only. Base contacts/locating pins are omitted from this reference.
    translate([wheel_xy[0],wheel_xy[1],wheel_z])
        cylinder(d=wheel_d,h=wheel_dial_t);
}
module usb_reference() {
    // Outer envelope from the drawing/Gerber. Mouth detail is simplified.
    for(y=usb_case_y) translate([68.65,y-4.47,pcb_z+pcb_t])
        difference() {
            cube([7.35,8.94,3.26]);
            translate([5.3,0.42,0.5]) cube([2.2,8.1,2.26]);
        }
}
module lcd_reference() {
    // Square module envelope; the 0.2 mm pocket allowance clears its corners.
    // The two-color top surface represents the active area, not another layer.
    color([0.09,0.10,0.12])
        difference() {
            translate([lcd_module_xy[0],lcd_module_xy[1],lcd_seat_z])
                cube([lcd_module_w,lcd_module_h,lcd_module_t]);
            translate([lcd_center[0]-lcd_active_size/2,
                       lcd_center[1]-lcd_active_size/2,
                       lcd_seat_z+lcd_module_t-0.02])
                cube([lcd_active_size,lcd_active_size,0.03]);
        }
    color([0.12,0.28,0.34])
        translate([lcd_center[0]-lcd_active_size/2,
                   lcd_center[1]-lcd_active_size/2,
                   lcd_seat_z+lcd_module_t-0.02])
            cube([lcd_active_size,lcd_active_size,0.02]);
}
module button_switch_reference() {
    // Simplified full switch envelopes; actuator details and solder omitted.
    for(p=button_xy)
        translate([p[0]-2.1,p[1]-1.6,pcb_z+pcb_t])
            cube([4.2,3.2,button_switch_h]);
}
module power_switch_reference() {
    translate([power_switch_xy[0]-power_switch_body[0]/2,
               power_switch_xy[1]-power_switch_body[1]/2,pcb_z+pcb_t])
        cube(power_switch_body);
    // One end of travel. Actual ON/OFF orientation is not established here.
    translate([power_switch_xy[0]+power_switch_travel/2-power_switch_handle[0]/2,
               power_switch_xy[1]-power_switch_handle[1]/2,
               pcb_z+pcb_t+power_switch_body[2]])
        cube(power_switch_handle);
}
module case_insert_reference() {
    // Smooth envelope only; actual knurls and M2 internal threads are omitted.
    for(p=boss_xy) translate([p[0],p[1],mid_top])
        difference() {
            cylinder(h=case_insert_l,d=case_insert_od);
            translate([0,0,-0.1]) cylinder(h=case_insert_l+0.2,d=2);
        }
}
module case_screw_reference() {
    // Nylon screw envelope; no decorative helical thread on reference geometry.
    for(p=boss_xy) translate([p[0],p[1],case_head_recess_depth]) {
        cylinder(h=case_screw_l,d=2);
        translate([0,0,-case_screw_head_h])
            difference() {
                cylinder(h=case_screw_head_h,d=case_screw_head_d);
                translate([-case_screw_head_d/2,-0.3,-0.01])
                    cube([case_screw_head_d,0.6,0.45]);
            }
    }
}
module midframe_insert_reference() {
    for(p=[mid_third_xy,mid_right_xy])
        translate([p[0],p[1],mid_z-case_insert_l])
            difference() {
                cylinder(h=case_insert_l,d=case_insert_od);
                translate([0,0,-0.1]) cylinder(h=case_insert_l+0.2,d=2);
            }
}
module midframe_screw_reference() {
    // Head bears on the plate at Z14.5; the shaft points downward to Z6.5.
    for(p=[mid_third_xy,mid_right_xy]) translate([p[0],p[1],mid_top]) {
        translate([0,0,-mid_screw_l]) cylinder(h=mid_screw_l,d=2);
        difference() {
            cylinder(h=case_screw_head_h,d=case_screw_head_d);
            translate([-case_screw_head_d/2,-0.3,case_screw_head_h-0.45])
                cube([case_screw_head_d,0.6,0.46]);
        }
    }
}
module assembled(explode=0) {
    color([0.25,0.30,0.35]) base();
    color([0.2,0.65,0.55]) translate([0,0,explode*0.5]) midframe();
    color([0.75,0.78,0.80]) translate([0,0,mid_top+explode]) lid();
    if(button_keys) color([0.20,0.28,0.34])
        translate([0,0,explode]) button_strip();
    color([0.32,0.42,0.52]) translate([0,0,explode]) power_slider();
    if(show_case_hardware) {
        %color([0.75,0.58,0.23]) translate([0,0,explode]) case_insert_reference();
        %color([0.90,0.90,0.86]) translate([0,0,-explode*0.5]) case_screw_reference();
    }
    if(show_midframe_hardware) {
        %color([0.75,0.58,0.23]) midframe_insert_reference();
        %color([0.90,0.90,0.86]) translate([0,0,explode*0.5]) midframe_screw_reference();
    }
    if(show_references) {
        %color([0.15,0.65,0.35]) translate([0,0,pcb_z+explode*0.75]) board_reference();
        %color([0.9,0.65,0.15]) battery_reference();
        %color([0.6,0.35,0.75]) translate([0,0,explode*0.75]) wheel_reference();
        %color([0.7,0.72,0.75]) translate([0,0,explode*0.75]) usb_reference();
        if(lcd_aperture) %translate([0,0,explode]) lcd_reference();
        if(button_keys) %color([0.32,0.34,0.36])
            translate([0,0,explode*0.75]) button_switch_reference();
        %color([0.25,0.28,0.31]) translate([0,0,explode*0.75]) power_switch_reference();
    }
}

if(part=="base") base();
else if(part=="midframe") midframe();
else if(part=="lid") lid();
else if(part=="button_strip") translate([0,0,-button_stem_z]) button_strip();
else if(part=="power_slider") translate([0,0,-power_socket_bottom]) power_slider(0);
else if(part=="pcb_template") board_reference();
else if(part=="exploded") assembled(24);
else assembled();
