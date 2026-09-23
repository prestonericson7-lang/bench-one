// ============================================================
//  camera-mount.scad — Luckfox sentry camera pod for the car
//
//  Each camera node is a Luckfox Pico + a MIPI camera module. The RV1103 has a
//  hardware ISP and H.264 encoder, so a ~1 W board records continuously while
//  the Pi and Zynq are powered down — that is what makes low-power sentry mode
//  possible at all. (See ../../firmware/sentry-camera/.)
//
//  This is a POD: board + camera in one printed shell, with an adjustable ball
//  joint so aim can be set once and left. Mounting is deliberately generic
//  (adhesive pad or a single screw) because windscreen/headliner/parcel-shelf
//  positions all differ.
//
//  ── MEASUREMENT POLICY ──────────────────────────────────────────────────────
//  ### MEASURE ###  = not verified. Luckfox Pico Mini B board size and the
//                     camera module footprint must come off the real parts.
//                     Placeholders below only exist so the model previews.
//
//  PRINT IN ASA. A camera pod sits against glass in direct sun — this is the
//  hottest-running part in the whole build. PETG is marginal here; PLA will
//  droop and point your camera at the floor.
// ============================================================

/* [Luckfox board — ### MEASURE ###] */
lf_w        = 42.0;   // ### MEASURE ### Pico Mini B length
lf_h        = 18.0;   // ### MEASURE ### width
lf_t        = 1.6;    // PCB thickness
lf_clear    = 0.5;    // pocket slack
lf_standoff = 3.0;    // clearance under the board for solder side + airflow

/* [Camera module — ### MEASURE ###] */
cam_w       = 21.0;   // ### MEASURE ### module PCB
cam_h       = 21.0;   // ### MEASURE ###
cam_lens_d  = 8.0;    // ### MEASURE ### lens barrel clearance
cam_lens_dx = 0;      // ### MEASURE ### lens offset from module centre
cam_lens_dy = 0;      // ### MEASURE ###
ribbon_w    = 12.0;   // MIPI FPC slot width
ribbon_t    = 1.2;

/* [Shell] */
wall        = 2.2;
floor_t     = 2.4;
lid_t       = 2.0;
vent_slots  = true;    // convection slots; this pod bakes in the sun

/* [Ball joint aim] */
ball_d      = 14.0;
stem_d      = 7.0;
socket_grip = 0.85;    // socket opening as a fraction of ball dia (<1 = snap fit)
base_d      = 26.0;
base_t      = 4.0;
screw_d     = 4.2;     // single M4 mounting screw

/* [Render] */
$fn         = 56;
ov          = 0.5;     // mating overlap — touching planes don't fuse in CGAL
render_mode = "preview";  // ["print_body","print_lid","print_base","preview"]

// ---------------- derived ----------------
inner_w = max(lf_w, cam_w) + 2*lf_clear;
inner_h = lf_h + cam_h + 6 + 2*lf_clear;
inner_z = lf_standoff + lf_t + 8;
out_w   = inner_w + 2*wall;
out_h   = inner_h + 2*wall;
out_z   = inner_z + floor_t;
stem_z  = out_z/2;   // ball stem at mid-height so it fuses into the shell

module _report() {
    echo(str("CAMERA POD outer = ", out_w, " x ", out_h, " x ", out_z + lid_t, " mm"));
    echo(str("  ball joint ", ball_d, " mm, socket opening ",
             ball_d*socket_grip, " mm (snap fit)"));
    if (socket_grip >= 1.0)
        echo("*** WARN: socket_grip >= 1.0 — ball will not be retained.");
    if (out_w > 70 || out_h > 80)
        echo("*** WARN: pod is getting large for a windscreen mount.");
}

module rrect(w, h, r, t) {
    linear_extrude(height = t)
        offset(r = r) offset(r = -r) square([w, h], center = true);
}

// ============================================================
//  pod body
// ============================================================
module body() {
    difference() {
        union() {
            difference() {
                rrect(out_w, out_h, 3, out_z);
                translate([0, 0, floor_t])
                    rrect(inner_w, inner_h, 2, inner_z + 1);
            }
            // board standoff rails (overlapped into the floor so they fuse)
            translate([0, -inner_h/2 + lf_h/2 + 2, floor_t - ov])
                difference() {
                    rrect(lf_w + 2*2, lf_h + 2*2, 1.5, lf_standoff + ov);
                    translate([0, 0, ov])
                        rrect(lf_w + 2*lf_clear, lf_h + 2*lf_clear, 1,
                              lf_standoff + 1);
                }
            // Ball stem off the back. Sits at MID-HEIGHT of the shell and starts
            // inside the wall, so it fuses. (First version sat at z=-stem_d/2,
            // below the floor, touching only along a tangent line -- the body
            // then exported as 2 disconnected solids.)
            translate([0, 0, stem_z])
                rotate([90, 0, 0])
                    translate([0, 0, out_h/2 - 2])
                        cylinder(d = stem_d, h = 8 + 2);
            translate([0, -(out_h/2 + 8 + ball_d/2 - 2), stem_z])
                sphere(d = ball_d);
        }
        // lens aperture through the front face
        translate([cam_lens_dx, inner_h/2 - cam_h/2 - 2 + cam_lens_dy, -1])
            cylinder(d = cam_lens_d, h = floor_t + 4);
        // MIPI ribbon slot between camera and board bays
        translate([0, 0, floor_t + 1])
            rrect(ribbon_w, 3, 1, ribbon_t + 2);
        // convection slots — this pod sits in direct sun
        if (vent_slots)
            for (s = [-1, 1])
                for (i = [-2 : 2])
                    translate([s * (out_w/2), i * 6, floor_t + inner_z*0.55])
                        rotate([0, 90, 0])
                            cylinder(d = 3.0, h = wall * 3, center = true);
        // cable exit
        translate([0, -(out_h/2), floor_t + 3])
            rotate([90, 0, 0])
                rrect(10, 6, 2, wall * 3);
    }
}

// ============================================================
//  lid
// ============================================================
module lid() {
    difference() {
        union() {
            rrect(out_w, out_h, 3, lid_t);
            translate([0, 0, lid_t - ov])
                rrect(inner_w - 0.4, inner_h - 0.4, 2, 2.5 + ov);
        }
        if (vent_slots)
            for (i = [-3 : 3])
                translate([i * 7, 0, -1])
                    rrect(3, out_h * 0.55, 1.5, lid_t + 6);
    }
}

// ============================================================
//  base (socket + mounting foot)
// ============================================================
module base() {
    difference() {
        union() {
            cylinder(d = base_d, h = base_t);
            translate([0, 0, base_t - ov])
                cylinder(d = ball_d + 2*3, h = ball_d*0.62 + ov);
        }
        // spherical socket
        translate([0, 0, base_t + ball_d*0.45])
            sphere(d = ball_d + 0.35);
        // opening smaller than the ball -> snap fit, adjustable by hand
        translate([0, 0, base_t + ball_d*0.45])
            cylinder(d = ball_d * socket_grip, h = ball_d);
        // split so the socket can flex when the ball is pressed in
        translate([0, 0, base_t + ball_d*0.35])
            cube([0.9, base_d + 4, ball_d], center = true);
        // mounting screw
        translate([0, 0, -1]) cylinder(d = screw_d, h = base_t + 4);
    }
}

// ============================================================
_report();
if (render_mode == "print_body")      body();
else if (render_mode == "print_lid")  lid();
else if (render_mode == "print_base") base();
else {
    body();
    translate([0, 0, out_z + 10]) color("SkyBlue", 0.6) lid();
    translate([0, -(out_h/2 + 8 + ball_d/2 - 1) - 16, -stem_d/2])
        color("Salmon", 0.6) rotate([-90, 0, 0]) base();
}
