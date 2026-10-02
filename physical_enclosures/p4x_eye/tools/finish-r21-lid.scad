// Exact union of the earlier R21 CGAL lid and the final source-defined guides.
use <../../mechanical/p4x-eye-enclosure/enclosure.scad>;
union() {
    import("stl-raw/lid-r21-before-shelf-guides-cgal.stl");
    translate([0,0,-14.5]) button_power_shelf_guides();
}
