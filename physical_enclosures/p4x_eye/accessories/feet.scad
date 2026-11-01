// P4X-EYE base feet: PLA plugs that press into the four screw-access wells
// on the underside of the base so the finished shell never rests on a desk.
//
// Mating geometry (enclosure.scad, R26):
//   case_head_recess_d     = 4.5   // underside well bore; cutter circumscribed, so 4.5 is the flat-to-flat minimum
//   case_head_recess_depth = 10.2  // screw head bears at Z10.2; a 1.3 mm pan head tops out near Z8.9
//   case_sleeve_d          = 6.52  // flat, unbevelled sleeve face around each well at Z0
//   face_edge_chamfer      = 1.0   // 45-degree bevel outside the sleeves; the foot head must stay on the sleeve
//
// The shaft is split so the two halves flex; a bulge band near the tip gives
// the interference. Print head-down (flat face on the bed), no supports.
// Insert with the slot aligned any way; pull straight out to reach the screws.

part = "feet";          // "feet" = 4 identical, "fit_test" = three bulge sizes, "single"

bore_d        = 4.5;    // enclosure well bore (flat-to-flat)
shaft_d       = 4.35;   // free part of the shaft: 0.075 mm per side under the bore
bulge_d       = 4.60;   // interference band; 0.05 mm per side over the bore (4.70 printed too tight)
bulge_h       = 1.6;    // height of the interference band
bulge_ramp    = 0.5;    // 45-degree lead-in / lead-out of the band
shaft_l       = 6.0;    // 8.9 mm available above the screw head; leave margin
slot_w        = 1.0;    // flex slot across the shaft
slot_depth    = 4.8;    // slot runs from the tip toward the head, stops before the head
tip_chamfer   = 0.5;

head_d        = 6.4;    // sits inside the 6.52 mm sleeve face
head_h        = 3.0;    // stand-off height
head_chamfer  = 0.6;    // 45-degree bevel on the floor edge of the pad

fit_test_bulges = [4.50, 4.60, 4.70];
pitch = 12;

$fn = 64;

assert(head_d <= 6.52, "Head must stay on the flat sleeve face, clear of the 1 mm face bevel.");
assert(shaft_l + 0.5 <= 10.2 - 1.3, "Shaft would reach the nylon screw head.");
assert(slot_depth < shaft_l, "Slot must stop short of the head.");
assert(shaft_d < bore_d && bulge_d > bore_d, "Shaft free below the bore, bulge above it.");

module shaft(bulge) {
    // Head sits at Z[0,head_h]; shaft grows upward from the head.
    // Profile: free shaft, 45-degree lead-in, bulge band, 45-degree lead-out, tip chamfer.
    z_band0 = shaft_l - tip_chamfer - bulge_ramp - bulge_h;
    z_band1 = shaft_l - tip_chamfer - bulge_ramp;
    rotate_extrude() polygon([
        [0, 0],
        [shaft_d/2, 0],
        [shaft_d/2, z_band0 - bulge_ramp],
        [bulge/2,   z_band0],
        [bulge/2,   z_band1],
        [shaft_d/2, shaft_l - tip_chamfer],
        [shaft_d/2 - tip_chamfer, shaft_l],
        [0, shaft_l]
    ]);
}

module head() {
    rotate_extrude() polygon([
        [0, 0],
        [head_d/2 - head_chamfer, 0],
        [head_d/2, head_chamfer],
        [head_d/2, head_h],
        [0, head_h]
    ]);
}

module foot(bulge = bulge_d, ticks = 0) {
    difference() {
        union() {
            head();
            translate([0, 0, head_h - 0.01]) shaft(bulge);
        }
        // flex slot from the tip down
        translate([-slot_w/2, -head_d, head_h + shaft_l - slot_depth])
            cube([slot_w, 2*head_d, slot_depth + 1]);
        // fit-test tick marks on the head rim, one per size step
        for (i = [0 : ticks - 1])
            rotate([0, 0, 90 + i*30])
                translate([head_d/2 - 0.3, -0.25, -0.1])
                    cube([1, 0.5, head_chamfer + 0.6]);
    }
}

if (part == "single") foot();
if (part == "feet")
    for (i = [0:3]) translate([(i % 2) * pitch, floor(i / 2) * pitch, 0]) foot();
if (part == "fit_test")
    for (i = [0 : len(fit_test_bulges) - 1])
        translate([i * pitch, 0, 0]) foot(fit_test_bulges[i], i + 1);
