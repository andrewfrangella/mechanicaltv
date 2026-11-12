// nipkow_disk.scad - one-piece, thicker Nipkow disk for the mechanical TV.
//
// Geometry reproduced from bitluni's "mechanical tv disk.stl"
// (https://github.com/bitluni/MechanicalTV, Models/). Everything that the
// software and the index sensor depend on is identical to the original:
// 250 mm diameter, 32 x 1.0 mm square holes on the same spiral, the 1 mm
// sync notch at the same angle, the four cutouts and 10 mm spokes. The plate
// is thicker (bitluni's is 0.6 mm), and the hub is a clamping hub: a D-shaped
// bore that matches the NEMA 17 shaft's flat, and a radial M3 screw into a
// captive nut. bitluni's press-fit cone and round 5 mm bore wear out when the
// disk is taken on and off; hub_style = "press" still builds them.
//
// The hub points AWAY from the motor (toward the screen holder).
//
// Fits the Elegoo Centauri 2 bed (256 x 256 mm) in one piece.
//
// Render from the command line (OpenSCAD 2021.01), e.g. a 2.0 mm plate:
//   /Applications/OpenSCAD-2021.01.app/Contents/MacOS/OpenSCAD \
//       -o nipkow_disk_2.0mm.stl -D plate_thickness=2.0 nipkow_disk.scad
// or open this file in the OpenSCAD GUI, change plate_thickness, press F6, export STL.

// ---------------------------------------------------------------- parameters
// Plate thickness in mm (bitluni original: 0.6). It must pass through the
// index sensor slot: keep it <= slot width - 1.5 mm (0.75 mm clearance per side).
plate_thickness = 1.75;

// Share of the plate that lies on the hub side of the original plate's
// mid-plane (the hub side faces away from the motor). 0.5 grows the plate
// equally on both faces, so the rim stays centred where the thin disk's rim was
// and the sensor slot alignment does not change. 1.0 would grow only toward the
// hub, 0.0 only toward the motor.
hub_side_fraction = 0.5;

// Each image hole keeps its full 1.0 mm square size through this much plate
// (mm) and is opened out at 45 degrees on both faces. This keeps the light
// cone through the holes as wide as it was with the 0.6 mm plate. Set to 0
// for plain straight holes.
hole_web = 0.8;

// "clamp": D-bore + M3 clamping screw (default). "press": bitluni's cone hub
// with a plain round 5 mm bore.
hub_style = "clamp";

// Clamping hub (17HE08-1004S shaft: 5.0 mm, flat 4.5 +-0.1 mm across, flat
// 17 mm long from the tip). bore_clearance is radial: 0.1 slides on by hand
// and the screw does the holding. Raise it if your printer makes holes small.
shaft_d        = 5.0;
shaft_flat     = 4.5;
bore_clearance = 0.1;
hub_r          = 11;      // hub radius (bitluni's cone: r 5 -> 10)
hub_chamfer    = 1.0;     // 45-degree chamfer round the tip
screw_z        = -6.5;    // screw axis, original frame: 5.2 mm from the plate's
                          // mid-plane toward the hub tip (4.3 mm from the hub-side
                          // face, 5.0 mm from the tip). With base_v2 it is 3.3 / 4.3 /
                          // 5.3 mm from the end of a 19 / 20 / 21 mm shaft
m3_clear_d     = 3.4;
nut_af         = 5.5;     // M3 nut across flats (plain nut, not nyloc)
nut_t          = 2.4;     // M3 nut thickness
nut_clear      = 0.3;
nut_x0         = 3.6;     // nut's inner face, from the axis (1.5 mm wall to the flat)
// Optional counterbore for a socket head. Default: none (head_x0 >= hub_r), so the
// nut is backed by 4.6 mm of solid hub. Use an M3x8 set screw (cup point), or an
// M3x10 socket head that seats on the hub surface.
head_d         = 6.2;
head_x0        = 11;

// true: only the hub and the plate out to r = 16 mm (a few grams), to check
// the bore and the screw on your shaft before printing the whole disk.
hub_test = false;

// true : flat face on z = 0, hub pointing up - ready to slice.
// false: bitluni's original frame (plate mid-plane at z = -1.3, hub toward -z).
print_orientation = true;

// ------------------------------------------ bitluni geometry (do not change)
disk_radius   = 125;
mid_plane_z   = -1.3;        // original plate occupies z -1.6 .. -1.0
hole_count    = 32;
hole_r0       = 84.5;        // hole i: radius hole_r0 + i * hole_r_step, angle 180 - i * hole_ang_step
hole_r_step   = 1.0;
hole_ang_step = 360 / hole_count;
hole_size     = 1.0;         // square hole, sides radial / tangential
notch_angle   = -168.75;     // sync notch: 1.0 mm wide, from r = 120 to the rim
notch_width   = 1.0;
notch_r_in    = 120;
cutout_r_in   = 15;          // four cutouts between two crossed 10 mm spokes
cutout_r_out  = 70;
spoke_width   = 10;
ring_r_in     = 73;          // stiffener ring on the hub side, r 73..75, from z -2.5
ring_r_out    = 75;
ring_z_bottom = -2.5;
bore_d        = 5.0;         // "press" hub: NEMA 17 shaft, 32 facets like the original
bore_fn       = 32;
hub_r_tip     = 5;           // "press" hub: cone r 5 at z -11.5 -> r 10 at z -1.5
hub_r_base    = 10;
hub_z_tip     = -11.5;
hub_z_base    = -1.5;
hub_fn        = 32;

// ------------------------------------------------------------------- derived
plate_z0 = mid_plane_z - plate_thickness * hub_side_fraction;   // hub-side face
plate_z1 = plate_z0 + plate_thickness;                           // far face
eps = 0.01;

module plate_2d() {
    difference() {
        circle(r = disk_radius, $fn = 360);
        // four lightening cutouts: annulus r 15..70 minus the two crossed spokes
        difference() {
            circle(r = cutout_r_out, $fn = 180);
            circle(r = cutout_r_in, $fn = 90);
            square([spoke_width, 2 * cutout_r_out + 2], center = true);
            square([2 * cutout_r_out + 2, spoke_width], center = true);
        }
        // sync notch for the index sensor
        rotate(notch_angle)
            translate([notch_r_in, -notch_width / 2])
                square([disk_radius - notch_r_in + 5, notch_width]);
    }
}

// One image hole, in a frame where the hole is at the origin and +x is radial.
module hole_cutter() {
    // straight 1 mm square through the full plate
    translate([0, 0, plate_z0 - 1])
        linear_extrude(plate_thickness + 2)
            square(hole_size, center = true);
    if (hole_web > 0 && hole_web < plate_thickness) {
        depth = (plate_thickness - hole_web) / 2;          // per face
        s = (hole_size + 2 * (depth + eps)) / hole_size;   // 45 degree flare
        // far face
        translate([0, 0, plate_z1 - depth])
            linear_extrude(height = depth + eps, scale = s)
                square(hole_size, center = true);
        // hub-side face
        translate([0, 0, plate_z0 + depth])
            mirror([0, 0, 1])
                linear_extrude(height = depth + eps, scale = s)
                    square(hole_size, center = true);
    }
}

// D-shaped shaft bore, flat facing +x, with clearance.
module d_bore_2d() {
    r = shaft_d / 2 + bore_clearance;
    flat_x = shaft_flat - shaft_d / 2 + bore_clearance;
    intersection() {
        circle(r = r, $fn = 64);
        translate([-r - 1, -r - 1]) square([flat_x + r + 1, 2 * r + 2]);
    }
}

// The hub always reaches 0.5 mm into the plate, whatever hub_side_fraction is.
hub_top = max(hub_z_base, plate_z0 + 0.5);
nut_corner = nut_af / cos(30);
nut_slot_end = screw_z + (nut_corner + nut_clear) / 2 + 0.05;   // toward the plate

assert(hub_style == "clamp" || hub_style == "press", "hub_style must be \"clamp\" or \"press\"");
assert(hub_style != "clamp" || nut_slot_end <= plate_z0 - 0.8,
       "screw_z too close to the plate: the nut slot would break through");
assert(hub_style != "clamp" || screw_z - m3_clear_d / 2 >= hub_z_tip + hub_chamfer,
       "screw_z too close to the hub tip");
assert(hub_style != "clamp" || head_x0 >= nut_x0 + nut_t + nut_clear + 2.5,
       "counterbore too deep: less than 2.5 mm of wall behind the nut");

module clamp_hub_solid() {
    translate([0, 0, hub_z_tip])
        cylinder(h = hub_chamfer, r1 = hub_r - hub_chamfer, r2 = hub_r, $fn = 96);
    translate([0, 0, hub_z_tip + hub_chamfer - eps])
        cylinder(h = hub_top - hub_z_tip - hub_chamfer + eps, r = hub_r, $fn = 96);
}

module clamp_hub_cuts() {
    z_top = plate_z1 + 1;
    // D bore through hub and plate, 0.5 mm lead-in chamfer at both ends
    translate([0, 0, hub_z_tip - 1])
        linear_extrude(z_top - hub_z_tip + 1) d_bore_2d();
    translate([0, 0, hub_z_tip - eps])
        cylinder(h = 0.5, r1 = shaft_d / 2 + bore_clearance + 0.5, r2 = shaft_d / 2 + bore_clearance, $fn = 64);
    translate([0, 0, plate_z1 - 0.5 + eps])
        cylinder(h = 0.5, r1 = shaft_d / 2 + bore_clearance, r2 = shaft_d / 2 + bore_clearance + 0.5, $fn = 64);
    // radial M3 along +x onto the flat
    translate([0, 0, screw_z]) rotate([0, 90, 0])
        cylinder(h = hub_r + 1, d = m3_clear_d, $fn = 32);
    if (head_x0 < hub_r)
        translate([head_x0, 0, screw_z]) rotate([0, 90, 0])
            cylinder(h = hub_r, d = head_d, $fn = 48);
    // captive nut: flats facing +-y, dropped in from the hub tip, centred on the screw
    translate([nut_x0, -(nut_af + nut_clear) / 2, hub_z_tip - 1])
        cube([nut_t + nut_clear, nut_af + nut_clear, nut_slot_end - (hub_z_tip - 1)]);
}

module disk() {
    difference() {
        union() {
            translate([0, 0, plate_z0])
                linear_extrude(plate_thickness) plate_2d();
            if (hub_style == "clamp")
                clamp_hub_solid();
            else
                translate([0, 0, hub_z_tip])
                    cylinder(h = hub_top - hub_z_tip, r1 = hub_r_tip,
                             r2 = hub_r_tip + (hub_r_base - hub_r_tip) * (hub_top - hub_z_tip) / (hub_z_base - hub_z_tip),
                             $fn = hub_fn);
            // stiffener ring, only where it still protrudes below the plate
            if (ring_z_bottom < plate_z0 - eps)
                translate([0, 0, ring_z_bottom])
                    linear_extrude(plate_z0 + 0.5 - ring_z_bottom)
                        difference() {
                            circle(r = ring_r_out, $fn = 256);
                            circle(r = ring_r_in, $fn = 256);
                        }
        }
        // shaft bore through hub and plate
        if (hub_style == "clamp")
            clamp_hub_cuts();
        else
            translate([0, 0, hub_z_tip - 1])
                cylinder(h = 40, d = bore_d, $fn = bore_fn);
        // 32 image holes on bitluni's spiral
        for (i = [0 : hole_count - 1])
            rotate([0, 0, 180 - i * hole_ang_step])
                translate([hole_r0 + i * hole_r_step, 0, 0])
                    hole_cutter();
    }
}

module part() {
    if (hub_test)
        intersection() {
            disk();
            translate([0, 0, -50]) cylinder(h = 100, r = 16, $fn = 96);
        }
    else
        disk();
}

if (print_orientation)
    rotate([180, 0, 0]) translate([0, 0, -plate_z1]) part();
else
    part();
