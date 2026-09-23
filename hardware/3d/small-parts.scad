// ============================================================
//  small-parts.scad — EC11 knob + servo bracket for the OEM vent damper
//
//  Select with -D part="knob" | "servo_bracket"
//
//  PRINT IN PETG OR ASA. Not PLA — cabin heat.
// ============================================================

part        = "knob";     // ["knob","servo_bracket"]
$fn         = 72;
ov          = 0.5;        // mating overlap; touching planes don't fuse in CGAL

// ============================================================
//  EC11 KNOB
//  EC11 shafts are 6 mm with a D-flat. Shaft LENGTH and whether yours is
//  knurled or D-type varies by variant -> ### CONFIRM YOURS ###
// ============================================================
knob_d        = 24;      // outer diameter
knob_h        = 16;      // overall height
shaft_d       = 6.0;     // [VERIFIED] EC11 shaft is 6 mm
shaft_flat    = 4.5;     // ### MEASURE ### across the D-flat
shaft_depth   = 12;      // ### MEASURE ### how far the shaft enters
shaft_clear   = 0.15;    // press-fit slack; increase if too tight
knurl_n       = 30;      // grip flutes
knurl_d       = 1.4;

module knob() {
    difference() {
        union() {
            // body, slightly tapered so it reads as a control not a cylinder
            cylinder(d1 = knob_d, d2 = knob_d - 2, h = knob_h);
            cylinder(d = knob_d, h = 2);      // skirt
        }
        // D-shaped shaft bore
        translate([0, 0, -0.1])
            intersection() {
                cylinder(d = shaft_d + shaft_clear, h = shaft_depth);
                translate([-(shaft_d)/2, -(shaft_flat + shaft_clear)/2, 0])
                    cube([shaft_d, shaft_flat + shaft_clear, shaft_depth]);
            }
        // grip flutes
        for (a = [0 : 360/knurl_n : 359])
            rotate([0, 0, a])
                translate([knob_d/2, 0, -0.1])
                    cylinder(d = knurl_d, h = knob_h + 0.2);
        // index mark so you can see position at a glance
        translate([0, knob_d/2 - 2, knob_h - 1])
            cube([1.6, 4, 2.4], center = true);
    }
}

// ============================================================
//  SERVO BRACKET — motorise the OEM shutoff damper
//
//  CONCEPT (from car-system-architecture.md): don't build a new diverter. The
//  F30 vent already has a thumbwheel-driven damper. Drive THAT shaft with a
//  micro servo:
//     air COLD (AC on)  -> open  -> free cooling for the electronics
//     air HOT  (heat on)-> close -> nothing hot reaches the stack
//
//  FAIL-SAFE: arrange the linkage so the spring//default position is CLOSED.
//  If the ESP32 dies you must not get heat blasting the Pi.
//
//  ### EVERY DIMENSION HERE DEPENDS ON MEASUREMENT #5 (damper shaft position
//  and travel). This is a parametric skeleton, not a fitted part. ###
// ============================================================
sg90_l        = 22.8;    // [VERIFIED-ish] SG90 body length  ### CONFIRM ###
sg90_w        = 12.2;    // SG90 body width
sg90_h        = 22.5;    // SG90 body height
sg90_tab_l    = 32.2;    // tab-to-tab length
sg90_tab_t    = 2.5;
sg90_screw_d  = 2.2;
sg90_shaft_dx = 5.9;     // shaft offset from body centre toward the tab end

shaft_to_servo = 25;     // ### MEASURE ### damper shaft -> servo centre distance
plate_t        = 3.0;
bracket_w      = 18;
mount_screw_d  = 3.4;

module servo_pocket() {
    // body cavity + tab slots
    cube([sg90_l, sg90_w, sg90_h + 2], center = true);
    for (s = [-1, 1])
        translate([s * (sg90_tab_l/2 - 3), 0, sg90_h/2 - sg90_tab_t/2])
            cube([6, sg90_w, sg90_tab_t + 0.4], center = true);
}

module servo_bracket() {
    difference() {
        union() {
            // base plate that straddles the damper shaft area
            translate([0, 0, plate_t/2])
                cube([shaft_to_servo + sg90_l + 16, bracket_w, plate_t], center = true);
            // servo cradle walls
            translate([shaft_to_servo/2 + 4, -(sg90_w + 6)/2, plate_t - ov])
                cube([sg90_l + 6, sg90_w + 6, sg90_h*0.6 + ov]);
        }
        // servo body pocket
        translate([shaft_to_servo/2 + 4 + (sg90_l + 6)/2, 0, plate_t + sg90_h*0.3])
            servo_pocket();
        // servo mounting screws
        for (s = [-1, 1])
            translate([shaft_to_servo/2 + 4 + (sg90_l + 6)/2 + s*(sg90_tab_l/2 - 2),
                       0, -1])
                cylinder(d = sg90_screw_d, h = plate_t + sg90_h + 4);
        // slot for the damper-shaft linkage arm
        translate([-(shaft_to_servo/2 + 4), 0, -1])
            cylinder(d = 10, h = plate_t + 4);
        // bracket mounting holes
        for (s = [-1, 1])
            translate([s * ((shaft_to_servo + sg90_l + 16)/2 - 5), 0, -1])
                cylinder(d = mount_screw_d, h = plate_t + 4);
    }
}

module _report() {
    if (part == "servo_bracket")
        echo("*** servo_bracket is a PARAMETRIC SKELETON. Every dimension depends on measurement #5 (damper shaft position + travel). Do not print before measuring.");
    if (part == "knob")
        echo(str("KNOB: ", knob_d, " mm dia x ", knob_h, " mm, D-bore ",
                 shaft_d, " mm flat ", shaft_flat, " mm. Confirm your EC11 shaft type."));
}

_report();
if (part == "knob") knob();
else if (part == "servo_bracket") servo_bracket();
