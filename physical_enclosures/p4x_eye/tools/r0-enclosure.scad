/* ESP32-P4X-EYE camera-free enclosure — dimensional concept, revision 0.
 * Units: mm. Read README.md before manufacturing.
 * PCB XY is sourced; battery dimensions are supplied by the user.
 * Board Z clearances remain provisional.
 * No fit-verified STL is supplied. Open this editable model in OpenSCAD.
 * Camera omitted. LCD, buttons, USB, SD, encoder and mic are to be retained.
 */

part = "assembly"; // [assembly,exploded,base,lid,separator,pcb_template]
show_references = true;
// Enable only after measuring actual aperture centers, heights and envelopes.
provisional_apertures = false;

case_w = 80;
case_h = 56;
corner_r = 5;
wall = 2;
floor_t = 2;
lid_t = 2;
battery_w = 65; // User dimensions 36 x 65 x 10, rotated in plane.
battery_h = 36;
battery_t = 10;
battery_gap_z = 0.75;
separator_t = 1.2;
rear_component_space = 10; // UNMEASURED: includes header and encoder.
pcb_t = 1.6; // UNMEASURED.
front_component_space = 5.7; // UNMEASURED: LCD, flex and controls.

separator_z = floor_t + battery_gap_z*2 + battery_t;
pcb_z = separator_z + separator_t + rear_component_space;
joint_z = pcb_z + pcb_t + front_component_space;
case_depth = joint_z + lid_t; // 34 at defaults; changes with battery thickness.
pcb_offset = [(case_w-69)/2, (case_h-43)/2];
boss_xy = [[3.6,3.6],[case_w-3.6,3.6],
           [3.6,case_h-3.6],[case_w-3.6,case_h-3.6]];

// Gerber TOP XY. The +Z face of this concept is the display side, so flip X.
function xy(p) = [pcb_offset[0]+69-p[0], pcb_offset[1]+p[1]];
pcb_outline = [[0,0],[57.6,0],[57.6,5.6],[69,5.6],[69,32.6],
 [68.2,32.6],[68.2,37],[54,37],[54,38.3685],[52.759,43],
 [36.241,43],[35,38.3685],[35,37],[21.8,37],
 // Quarter-radius notch is simplified to chords in the reference solid only.
 [21.3,36.5],[21.3,36.3],[20.8,35.8],[16.5,35.8],
 [16,36.3],[16,36.5],[15.5,37],[0,37]];

$fn = 64;
assert(battery_w+8 < case_w && battery_h+8 < case_h,
       "Battery needs a larger case and a new clearance review.");
assert(battery_t >= 3 && battery_t <= 12, "Review depth outside this draft range.");
echo("PROVISIONAL outside dimensions", [case_w,case_h,case_depth]);

module rounded2d(w,h,r) {
    translate([r,r]) offset(r=r) square([w-2*r,h-2*r]);
}
module roundedbox(w,h,d,r) {
    linear_extrude(d) rounded2d(w,h,r);
}
module screw_bosses() {
    for(p=boss_xy) translate([p[0],p[1],floor_t-0.01])
        difference() {
            cylinder(h=joint_z-floor_t+0.01,d=5.2);
            translate([0,0,joint_z-floor_t-8]) cylinder(h=8.1,d=1.6);
        }
}
module battery_locators() {
    // Low perimeter guides only; no clamping force on the pouch.
    translate([(case_w-battery_w)/2-2,(case_h-battery_h)/2-2,floor_t-0.01])
        difference() {
            roundedbox(battery_w+4,battery_h+4,1.4,2);
            translate([1,1,-0.1]) roundedbox(battery_w+2,battery_h+2,1.6,1);
            // Lead exit; confirm against the selected pack.
            translate([-1,4,-0.1]) cube([5,10,2]);
        }
}
module concept_usb_apertures() {
    // Placement Y is sourced. Z and plug clearance are placeholders.
    for(y=[11,26])
        translate([case_w-wall-1,pcb_offset[1]+y-5.5,pcb_z+pcb_t-0.5])
            cube([wall+2,11,6]);
}
module base() {
    difference() {
        union() {
            difference() {
                roundedbox(case_w,case_h,joint_z,corner_r);
                translate([wall,wall,floor_t])
                    roundedbox(case_w-2*wall,case_h-2*wall,
                               joint_z,corner_r-wall);
            }
            screw_bosses();
            battery_locators();
            // Two ledges support a removable insulating separator.
            for(x=[wall-0.01,case_w-wall-2.2])
                translate([x,10,separator_z-1.2]) cube([2.21,case_h-20,1.2]);
        }
        if(provisional_apertures) concept_usb_apertures();
    }
}
module lid() {
    difference() {
        roundedbox(case_w,case_h,lid_t,corner_r);
        for(p=boss_xy) translate([p[0],p[1],-0.1]) cylinder(h=lid_t+0.2,d=2.4);
        if(provisional_apertures) {
            // LCD active area 27.72 square + 0.3 each edge from datasheet.
            // Actual LCD placement is NOT specified in the board placement file.
            p=xy([44.5,20.36]);
            translate([p[0]-14.16,p[1]-14.16,-0.1])
                roundedbox(28.32,28.32,lid_t+0.2,0.8);
            // Button placement centers only; actuators must still be designed.
            for(y=[12.5,18.5,24.5]) {
                b=xy([23,y]);
                translate([b[0],b[1],-0.1]) cylinder(h=lid_t+0.2,d=4);
            }
        }
    }
}
module separator() {
    difference() {
        translate([wall+0.4,wall+0.4])
            roundedbox(case_w-2*wall-0.8,case_h-2*wall-0.8,
                       separator_t,corner_r-wall-0.4);
        for(p=boss_xy) translate([p[0],p[1],-0.1]) cylinder(h=separator_t+0.2,d=6.2);
        // Open wire notches, not a pinch slot.
        for(x=[0,case_w-wall-5])
            translate([x,case_h/2-5,-0.1]) cube([wall+5,10,separator_t+0.2]);
    }
}
module board_reference() {
    linear_extrude(pcb_t) polygon([for(p=pcb_outline) xy(p)]);
}
module battery_reference() {
    translate([(case_w-battery_w)/2,(case_h-battery_h)/2,
               floor_t+battery_gap_z])
        roundedbox(battery_w,battery_h,battery_t,1);
}
module assembled(explode=0) {
    color([0.25,0.30,0.35]) base();
    color([0.75,0.78,0.80]) translate([0,0,joint_z+explode]) lid();
    color([0.60,0.65,0.70]) translate([0,0,separator_z+explode/4]) separator();
    if(show_references) {
        %color([0.15,0.65,0.35]) translate([0,0,pcb_z+explode/2]) board_reference();
        %color([0.9,0.65,0.15]) battery_reference();
    }
}

if(part=="base") base();
else if(part=="lid") lid();
else if(part=="separator") separator();
else if(part=="pcb_template") board_reference();
else if(part=="exploded") assembled(24);
else assembled();
