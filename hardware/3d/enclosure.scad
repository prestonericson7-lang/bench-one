// ============================================================
//  enclosure.scad — remote compute enclosure (Orange Pi + Zynq + power + fan)
//
//  WHY REMOTE: this does NOT go behind the vent. Real footprints —
//  Zynq StarLite 90 x 60 mm, Orange Pi 4 Pro ~89 x 56 mm — plus storage, a
//  DC-DC and a fan land around 200 x 130 x 70 mm. There is nowhere near that
//  volume behind an F30 dash vent. Mount this in the glovebox / under-seat /
//  behind the console, and run ONE cable to the vent module.
//  (See ../car-system-architecture.md and ../interconnect.md.)
//
//  ── MEASUREMENT POLICY ──────────────────────────────────────────────────────
//  ### MEASURE ###  = not verified. Placeholder so the model previews.
//                     Mounting-hole patterns are NOT published for either board —
//                     measure them off the real PCB before printing.
//  [VERIFIED]       = vendor/datasheet figure.
//
//  PRINT IN ASA OR PETG. NOT PLA — a parked car interior exceeds PLA's ~60 C Tg.
// ============================================================

/* [Boards — outlines] */
zynq_w   = 90.0;  zynq_h   = 60.0;   // [VERIFIED] Puzhi PZ7020-StarLite
pi_w     = 89.0;  pi_h     = 56.0;   // ### MEASURE ### single-sourced
teensy_w = 61.0;  teensy_h = 18.0;   // ### MEASURE ### confirm

/* [Boards — mounting holes]
   Each board: list of [x, y] hole centres measured from the board's OWN
   bottom-left corner. NOT PUBLISHED for either board — measure them.        */
zynq_holes = [[3.5, 3.5], [86.5, 3.5], [3.5, 56.5], [86.5, 56.5]];  // ### MEASURE ###
pi_holes   = [[3.5, 3.5], [85.5, 3.5], [3.5, 52.5], [85.5, 52.5]];  // ### MEASURE ###
hole_d     = 2.7;    // M2.5 clearance-ish; ### CONFIRM ### screw size
standoff_d = 6.0;
standoff_h = 6.0;    // board-to-floor clearance (solder side + airflow)

/* [Stack layout] */
// Boards sit side-by-side on the floor by default. Set stacked=true to put the
// Pi on a second level above the Zynq (smaller footprint, worse cooling).
stacked        = false;
board_gap      = 12;    // space between boards for cabling/airflow
level_gap      = 28;    // vertical gap if stacked (tallest component + air)

/* [Aux volumes — reserve space, ### MEASURE ### your actual parts] */
dcdc_w   = 65; dcdc_h = 45; dcdc_t = 22;   // 12V->5V automotive converter
ssd_w    = 100; ssd_h = 55; ssd_t = 12;    // 2 TB USB-C SSD
reserve_aux = true;

/* [Enclosure] */
wall        = 2.4;
floor_t     = 2.6;
lid_t       = 2.4;
clearance   = 6;     // margin around board envelope for connectors
corner_r    = 4;
vent_slots  = true;
fan_size    = 40;    // 40 or 60 mm fan; 0 = none
fan_wall    = "end"; // ["end","side"]

/* [Which box]
   The model's own envelope report showed the DC-DC + SSD add ~116 mm of depth.
   Splitting them into a second box drops the compute enclosure from
   207.8 x 192.8 to ~107 x 77 mm, and puts the converter (the heat source) near
   the power feed instead of wrapped around the boards.
     "compute" = Zynq + Pi        "power" = DC-DC + SSD + fuse/relay          */
box         = "compute";   // ["compute","power"]

/* [Render] */
$fn         = 48;
render_mode = "preview";   // ["print_base","print_lid","preview"]

// ============================================================
//  Derived envelope
// ============================================================
is_power = (box == "power");

boards_w = is_power ? 0 : (stacked ? max(zynq_w, pi_w) : (zynq_w + board_gap + pi_w));
boards_h = is_power ? 0 : max(zynq_h, pi_h);

// the power box always carries the aux volumes; the compute box only if asked
want_aux = is_power ? true : reserve_aux;
aux_w    = want_aux ? max(dcdc_w, ssd_w) : 0;
aux_h    = want_aux ? (dcdc_h + 8 + ssd_h) : 0;

inner_w  = max(boards_w, aux_w) + 2*clearance;
inner_h  = boards_h + (want_aux ? aux_h + (is_power ? 0 : 8) : 0) + 2*clearance;
inner_z  = is_power ? (max(dcdc_t, ssd_t) + 14)
                    : ((stacked ? standoff_h + level_gap + 12 : standoff_h + 22) + 6);

out_w = inner_w + 2*wall;
out_h = inner_h + 2*wall;
out_z = inner_z + floor_t;

module _report() {
    echo(str("BOX = ", box, (is_power ? "  (DC-DC + SSD)" : "  (Zynq + Pi)")));
    echo(str("ENVELOPE outer  = ", out_w, " x ", out_h, " x ", out_z + lid_t, " mm"));
    echo(str("ENVELOPE inner  = ", inner_w, " x ", inner_h, " x ", inner_z, " mm"));
    if (fan_size > 0 && fan_size + 6 > inner_h)
        echo(str("*** WARN: ", fan_size, " mm fan taller than inner height ", inner_h));
    if (out_w > 260 || out_h > 200)
        echo("*** WARN: enclosure is getting large for a glovebox. Consider stacked=true.");
}

// ============================================================
//  Helpers
// ============================================================
module rrect(w, h, r, t) {
    linear_extrude(height = t)
        offset(r = r) offset(r = -r) square([w, h], center = true);
}

module standoffs(holes, ox, oy, h) {
    for (p = holes)
        translate([ox + p[0], oy + p[1], 0])
            difference() {
                cylinder(d = standoff_d, h = h);
                translate([0, 0, -0.1]) cylinder(d = hole_d, h = h + 0.2);
            }
}

// ============================================================
//  Base
// ============================================================
module base() {
    difference() {
        union() {
            // shell
            difference() {
                rrect(out_w, out_h, corner_r, out_z);
                translate([0, 0, floor_t])
                    rrect(inner_w, inner_h, max(corner_r - wall, 0.5), inner_z + 1);
            }
            // board standoffs (overlap the floor by 0.5 so they fuse — same
            // lesson as vent-insert.scad: touching planes don't reliably merge)
            translate([0, 0, floor_t - 0.5]) board_standoffs();
        }
        if (vent_slots) vent_cuts();
        if (fan_size > 0) fan_cut();
        cable_entry();
    }
}

module board_standoffs() {
    if (!is_power) {
        zx = -inner_w/2 + clearance;
        zy = -inner_h/2 + clearance;
        standoffs(zynq_holes, zx, zy, standoff_h + 0.5);
        if (!stacked)
            standoffs(pi_holes, zx + zynq_w + board_gap, zy, standoff_h + 0.5);
        else
            // Upper level: pillars must run from the FLOOR up to the second
            // level, not float at height. (Floating discs made the base export
            // as 5 disconnected solids — caught by the Volumes check.)
            standoffs(pi_holes, zx, zy, standoff_h + 0.5 + level_gap);
    }
}

module vent_cuts() {
    // slots in both long walls, above the floor
    for (side = [-1, 1])
        for (i = [-4 : 4])
            translate([i * 12, side * (out_h/2), floor_t + inner_z/2])
                rotate([90, 0, 0])
                    linear_extrude(height = wall * 3, center = true)
                        offset(r = 1.2) offset(r = -1.2)
                            square([4, inner_z * 0.5], center = true);
}

module fan_cut() {
    // fan aperture + 4 screw holes, on the chosen wall
    pitch = (fan_size == 60) ? 50 : 32;   // 60mm fan = 50mm pitch, 40mm = 32mm
    x = (fan_wall == "end") ? out_w/2 : 0;
    rot = (fan_wall == "end") ? [0, 90, 0] : [90, 0, 0];
    y = (fan_wall == "end") ? 0 : out_h/2;
    translate([x, y, floor_t + inner_z/2]) rotate(rot) {
        cylinder(d = fan_size - 4, h = wall * 4, center = true);
        for (a = [45 : 90 : 315])
            rotate([0, 0, a])
                translate([pitch/sqrt(2), 0, 0])
                    cylinder(d = 3.2, h = wall * 4, center = true);
    }
}

module cable_entry() {
    // slot for the loom to the vent module + power in
    translate([-out_w/2, 0, floor_t + 8])
        rotate([0, 90, 0])
            linear_extrude(height = wall * 3, center = true)
                offset(r = 2) offset(r = -2) square([14, 26], center = true);
}

// ============================================================
//  Lid
// ============================================================
module lid() {
    difference() {
        union() {
            rrect(out_w, out_h, corner_r, lid_t);
            // lip that drops into the opening
            translate([0, 0, lid_t - 0.5])
                rrect(inner_w - 0.4, inner_h - 0.4,
                      max(corner_r - wall, 0.5), 3);
        }
        if (vent_slots)
            for (i = [-5 : 5])
                translate([i * 14, 0, -1])
                    linear_extrude(height = lid_t + 6)
                        offset(r = 1.5) offset(r = -1.5)
                            square([5, out_h * 0.5], center = true);
    }
}

// ============================================================
//  Ghost boards (preview only — never in a print export)
// ============================================================
module ghosts() {
    zx = -inner_w/2 + clearance;
    zy = -inner_h/2 + clearance;
    if (!is_power) {
        color("SteelBlue", 0.35)
            translate([zx + zynq_w/2, zy + zynq_h/2, floor_t + standoff_h])
                rrect(zynq_w, zynq_h, 1, 1.6);
        color("ForestGreen", 0.35)
            translate(stacked
                ? [zx + pi_w/2, zy + pi_h/2, floor_t + standoff_h + level_gap]
                : [zx + zynq_w + board_gap + pi_w/2, zy + pi_h/2, floor_t + standoff_h])
                rrect(pi_w, pi_h, 1, 1.6);
    }
    if (want_aux) {
        color("Goldenrod", 0.30)
            translate([0, inner_h/2 - clearance - dcdc_h/2, floor_t + dcdc_t/2])
                cube([dcdc_w, dcdc_h, dcdc_t], center = true);
        color("Gray", 0.30)
            translate([0, inner_h/2 - clearance - dcdc_h - 8 - ssd_h/2,
                       floor_t + ssd_t/2])
                cube([ssd_w, ssd_h, ssd_t], center = true);
    }
}

// ============================================================
_report();
if (render_mode == "print_base") base();
else if (render_mode == "print_lid") lid();
else { base(); ghosts(); translate([0, 0, out_z + 14]) lid(); }
