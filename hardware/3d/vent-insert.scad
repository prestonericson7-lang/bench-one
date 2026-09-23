// ============================================================
//  vent-insert.scad — BMW F30 driver-side air vent → screen + encoder insert
//
//  CONCEPT: do NOT reprint the vent. Keep the OEM assembly (wood trim, chrome
//  strip, thumbwheel, spring clips, duct + foam gasket, shutoff damper). Cut out
//  ONLY the louvers and the centre tab, then drop this insert into the opening.
//  Its flange hides the cut edges. Reversible in spirit, and it keeps every hard
//  part (sealing, retention, damper) as the factory made it.
//
//  ── MEASUREMENT POLICY ──────────────────────────────────────────────────────
//  Numbers marked  ### MEASURE ###  are NOT known. They are placeholders so the
//  model previews. The real BMW vent aperture is not published anywhere (checked),
//  and the supplied Meshy scan has no real-world scale (arbitrary units, 2.2 M
//  verts, AI-reconstructed) — so it CANNOT be used for fitting dimensions.
//  Replace each one with a caliper reading before printing anything.
//
//  Numbers marked  [VERIFIED]  are from datasheets/vendor specs — leave them
//  alone unless your specific part differs.
//
//  PRINT IN ASA OR PETG. NOT PLA. A dash in summer sun exceeds PLA's ~60 °C
//  glass transition and the part will sag and fall out.
// ============================================================

/* [Scan-derived proportions — USE THESE TO CROSS-CHECK YOUR MEASUREMENTS]
   Extracted from the supplied Meshy scan by bounding-box analysis over all
   2,197,028 vertices. A bounding box over that many points is statistically
   robust even though the surface itself is noisy, so these RATIOS are usable
   even though the scan's absolute scale is meaningless.

        overall  W : H : D  =  1 : 0.4709 : 0.6065

   So: measure the overall vent width once, and the other two follow.
   Set vent_overall_w below and the model will echo the expected height/depth —
   if your calipers disagree badly, something's wrong and you should re-measure.

   NOT extractable from the scan (proven, not assumed): the louver aperture,
   corner radii, and internal depth. Two computational attempts failed because
   the mesh has no dominant surface plane — the largest normal cluster is only
   0.1% of vertices, i.e. it's reconstructed blob, not flat molded faces.       */
vent_overall_w    = 0;     // ### MEASURE ### overall vent width; 0 = skip check

/* [Vent aperture — MEASURE THESE FIRST] */
// Width of the louver opening (inner aperture of the black surround)
vent_w            = 150;   // ### MEASURE ###
// Height of that opening
vent_h            = 55;    // ### MEASURE ###
// Corner radius of the opening
vent_corner_r     = 6;     // ### MEASURE ###
// Usable depth from the face plane back to the first obstruction (damper/vane).
// THIS IS THE CRITICAL ONE — it decides whether the screen+PCB fit at all.
vent_depth_avail  = 25;    // ### MEASURE ###
// How much lip/bezel the flange can overlap without fouling the trim
flange_overlap    = 4;     // ### MEASURE ###

/* [Insert construction] */
plate_t           = 2.4;   // faceplate thickness
flange_t          = 1.6;   // flange thickness (sits proud on the bezel)
wall              = 2.0;   // side wall thickness
fit_clearance     = 0.35;  // per-side gap so it actually drops in (print-tuned)
screen_bezel_lap  = 0.6;   // faceplate overlaps LCD active edge, hides the border

/* [Airflow — keep the vent working]
   A screen that blocks the whole aperture turns a working vent into a blank.
   P3Cars' vent-integrated display retains airflow, and your own servo/temp plan
   depends on air still moving (cold air = free cooling for the stack, hot air =
   servo shuts the damper). So the insert carries flow slots around the screen. */
airflow           = true;
slot_w            = 3.2;   // slot width
slot_gap          = 3.0;   // gap between slots
slot_margin       = 3.0;   // keep slots this far off the screen pocket + edges

/* [2.4" ILI9341 module — VERIFIED, change only if your module differs] */
// PCB 43 x 72 x 5 mm; active area 36.72 x 48.96 mm
scr_pcb_w         = 72.0;  // [VERIFIED] long axis — mounted LANDSCAPE
scr_pcb_h         = 43.0;  // [VERIFIED]
scr_pcb_t         = 5.0;   // [VERIFIED] board + components
scr_act_w         = 48.96; // [VERIFIED] active area long axis
scr_act_h         = 36.72; // [VERIFIED]
// Offset of the active area centre from the PCB centre.
scr_act_dx        = 0;     // ### MEASURE ### (on your module)
scr_act_dy        = 0;     // ### MEASURE ###
scr_pocket_clear  = 0.4;   // slack around the PCB in its pocket

/* [EC11 encoder — partially verified] */
ec11_body         = 12.5;  // [VERIFIED] ~12.5 mm square body
ec11_body_h       = 7.5;   // [VERIFIED] body height behind panel
ec11_bush_d       = 7.2;   // M7x0.75 bushing -> 7 mm hole + clearance  ### CONFIRM ###
ec11_shaft_len    = 20;    // ### MEASURE ### varies by variant; sets knob standoff
// Encoder centre, measured from the insert centre (+x = toward the thumbwheel side)
enc_x             = 45;    // ### CHOOSE after measuring aperture ###
enc_y             = 0;

/* [Teensy carrier — mounts behind the insert]
   The Teensy lives AT the vent, not in the remote box: the TFT is SPI at tens of
   MHz and will not survive a multi-foot cable run, and encoder edges pick up
   noise. Local MCU -> one robust cable (12 V + UART/CAN) back to the main box.
   See ../interconnect.md.                                                     */
teensy_w          = 61.0;  // ### CONFIRM ### Teensy 4.1 length
teensy_h          = 18.0;  // ### CONFIRM ### width
teensy_t          = 4.0;   // board + headers clearance
carrier_t         = 2.6;
boss_d            = 7.0;   // mounting bosses on the insert's back
boss_screw_d      = 2.6;   // M2.5 self-tap
boss_inset        = 9.0;   // from the aperture edge

/* [Render] */
$fn               = 64;
explode           = 0;     // set >0 to separate parts in preview
// "print" = insert only, ready to slice.  "preview" = adds ghost screen/encoder/
// depth-envelope so you can eyeball clearances. NEVER slice a preview export:
// the ghosts are separate solids and would be printed.
render_mode       = "preview";   // ["print","preview"]

// ============================================================
//  Sanity checks — the model tells you if it cannot fit
// ============================================================
inner_w = vent_w - 2*fit_clearance;
inner_h = vent_h - 2*fit_clearance;

// Scan-derived proportions (bounding box over 2,197,028 vertices)
SCAN_H_RATIO = 0.4709;
SCAN_D_RATIO = 0.6065;

module _checks() {
    if (vent_overall_w > 0) {
        echo(str("SCAN CROSS-CHECK: for overall width ", vent_overall_w,
                 " mm, the scan proportions predict overall height ",
                 vent_overall_w * SCAN_H_RATIO, " mm and overall depth ",
                 vent_overall_w * SCAN_D_RATIO, " mm."));
        echo("  -> compare against your calipers. Large disagreement = re-measure.");
        if (vent_depth_avail > vent_overall_w * SCAN_D_RATIO)
            echo(str("*** WARN: usable depth ", vent_depth_avail,
                     " mm exceeds the part's whole predicted depth ",
                     vent_overall_w * SCAN_D_RATIO, " mm. Check measurement #3."));
    }
    if (scr_pcb_w > inner_w)
        echo(str("*** FAIL: screen PCB ", scr_pcb_w, " mm wider than aperture ",
                 inner_w, " mm. Screen will not fit landscape."));
    if (scr_pcb_h > inner_h)
        echo(str("*** FAIL: screen PCB ", scr_pcb_h, " mm taller than aperture ",
                 inner_h, " mm."));
    if (scr_pcb_t + plate_t > vent_depth_avail)
        echo(str("*** FAIL: screen stack ", scr_pcb_t + plate_t,
                 " mm deeper than available ", vent_depth_avail, " mm."));
    if (abs(enc_x) + ec11_body/2 > inner_w/2)
        echo("*** FAIL: encoder falls outside the aperture.");
    // Does the screen + encoder both fit across the width?
    needed = scr_pcb_w + ec11_body + 4;
    if (needed > inner_w)
        echo(str("*** WARN: screen + encoder need ", needed,
                 " mm across; aperture is ", inner_w,
                 " mm. Consider stacking the encoder below, or a smaller screen."));
}

// ============================================================
//  Helpers
// ============================================================
module rrect(w, h, r, t) {           // rounded rectangular prism
    linear_extrude(height = t)
        offset(r = r) offset(r = -r)
            square([w, h], center = true);
}

// ============================================================
//  The insert
// ============================================================
// Parts must OVERLAP, not merely touch. Coincident planes are not reliably fused
// by CGAL and produce disconnected solids (caught in testing: Volumes reported 4,
// i.e. 3 separate bodies, instead of 2 for one closed solid).
ov = 0.5;   // interference between mating features

module vent_insert() {
    difference() {
        union() {
            // faceplate that sits in the aperture
            rrect(inner_w, inner_h, vent_corner_r, plate_t);
            // flange, proud of the face, hides the cut louver edges.
            // Extended UP into the plate by `ov` so the two fuse.
            translate([0, 0, -flange_t])
                rrect(inner_w + 2*flange_overlap,
                      inner_h + 2*flange_overlap,
                      vent_corner_r + flange_overlap, flange_t + ov);
            // side walls going back into the vent, stiffen + locate.
            // Started `ov` BELOW the plate top so they bite into it.
            translate([0, 0, plate_t - ov])
                difference() {
                    rrect(inner_w, inner_h, vent_corner_r, wall_depth() + ov);
                    translate([0, 0, -0.1])
                        rrect(inner_w - 2*wall, inner_h - 2*wall,
                              max(vent_corner_r - wall, 0.5), wall_depth() + ov + 0.2);
                }
            // pocket walls that capture the screen PCB (also overlapped)
            translate([0, 0, plate_t - ov]) screen_pocket_walls();
            // bosses the Teensy carrier screws into
            mount_bosses();
        }

        // --- screen window (active area, minus a small overlap to hide the border)
        translate([scr_act_dx, scr_act_dy, -flange_t - 1])
            rrect(scr_act_w - 2*screen_bezel_lap,
                  scr_act_h - 2*screen_bezel_lap, 1.5,
                  plate_t + flange_t + 2);

        // --- encoder bushing hole
        translate([enc_x, enc_y, -flange_t - 1])
            cylinder(d = ec11_bush_d, h = plate_t + flange_t + 2);

        // --- airflow slots (keep the vent venting)
        if (airflow) airflow_slots();
    }
}

// Vertical slots through the faceplate + flange, in the clear areas either side
// of the screen pocket, skipping anything that would hit the encoder.
module airflow_slots() {
    pocket_half = (scr_pcb_w + 2*scr_pocket_clear + 2*1.6) / 2;
    slot_h = inner_h - 2*slot_margin - 2*wall;
    pitch  = slot_w + slot_gap;
    // how far out we may go before hitting the side wall
    outer  = inner_w/2 - wall - slot_margin;
    inner_edge = pocket_half + slot_margin;

    if (outer > inner_edge + slot_w) {
        n = floor((outer - inner_edge) / pitch);
        for (side = [-1, 1])
            for (i = [0 : n - 1]) {
                x = side * (inner_edge + slot_w/2 + i * pitch);
                // skip if this slot would foul the encoder bushing
                if (abs(x - enc_x) > (ec11_bush_d/2 + slot_w/2 + 1.5))
                    translate([x, 0, -flange_t - 1])
                        linear_extrude(height = plate_t + flange_t + 2)
                            offset(r = slot_w/2) offset(r = -slot_w/2)
                                square([slot_w, slot_h], center = true);
            }
    } else {
        echo("*** NOTE: no room for airflow slots beside the screen. Vent will be blocked - consider a smaller screen, or slots above/below.");
    }
}

function wall_depth() = min(vent_depth_avail - plate_t, scr_pcb_t + 6);

// Mounting boss positions, shared by the insert and the carrier so the two
// parts can never drift out of alignment.
function boss_pts() = [
    [-(inner_w/2 - boss_inset),  (inner_h/2 - boss_inset)],
    [ (inner_w/2 - boss_inset),  (inner_h/2 - boss_inset)],
    [-(inner_w/2 - boss_inset), -(inner_h/2 - boss_inset)],
    [ (inner_w/2 - boss_inset), -(inner_h/2 - boss_inset)]
];

// Bosses standing off the back of the faceplate for the carrier to screw into.
module mount_bosses() {
    h = wall_depth();
    for (p = boss_pts())
        translate([p[0], p[1], plate_t - ov])
            difference() {
                cylinder(d = boss_d, h = h + ov);
                translate([0, 0, 1.5])
                    cylinder(d = boss_screw_d, h = h + ov);
            }
}

// Carrier plate: holds the Teensy, relieves the loom, screws to the bosses.
module carrier() {
    difference() {
        union() {
            rrect(inner_w - 2*wall - 1, inner_h - 2*wall - 1, 3, carrier_t);
            // Teensy pocket rails
            translate([0, 0, carrier_t - ov])
                difference() {
                    rrect(teensy_w + 2*2.2, teensy_h + 2*2.2, 2, teensy_t + ov);
                    translate([0, 0, ov])
                        rrect(teensy_w + 0.6, teensy_h + 0.6, 1, teensy_t + 1);
                }
        }
        // screw clearance matching the insert's bosses
        for (p = boss_pts())
            translate([p[0], p[1], -1])
                cylinder(d = boss_screw_d + 0.9, h = carrier_t + 4);
        // loom exit + strain-relief tie slots
        translate([0, -(inner_h/2 - wall - 8), -1])
            rrect(16, 7, 2.5, carrier_t + 4);
        for (s = [-1, 1])
            translate([s*13, -(inner_h/2 - wall - 8), -1])
                rrect(2.6, 7, 1.2, carrier_t + 4);
    }
}

module screen_pocket_walls() {
    pw = scr_pcb_w + 2*scr_pocket_clear;
    ph = scr_pcb_h + 2*scr_pocket_clear;
    difference() {
        rrect(pw + 2*1.6, ph + 2*1.6, 2, scr_pcb_t + ov);
        // inner cavity kept clear of the plate so the ring still fuses to it
        translate([0, 0, ov]) rrect(pw, ph, 1.5, scr_pcb_t + 0.2);
    }
}

// ============================================================
//  Reference ghosts (not printed) — check clearances visually
// ============================================================
module ghost_screen() {
    color("SteelBlue", 0.35)
        translate([0, 0, plate_t + explode])
            rrect(scr_pcb_w, scr_pcb_h, 1.5, scr_pcb_t);
}
module ghost_encoder() {
    color("Tomato", 0.35)
        translate([enc_x, enc_y, plate_t + explode]) {
            rrect(ec11_body, ec11_body, 0.5, ec11_body_h);
            translate([0, 0, -plate_t - ec11_shaft_len])
                cylinder(d = 6, h = ec11_shaft_len);
        }
}
// Envelope of the depth we are allowed to occupy
module ghost_depth_limit() {
    color("Green", 0.10)
        translate([0, 0, plate_t])
            rrect(inner_w, inner_h, vent_corner_r, vent_depth_avail - plate_t);
}

// ============================================================
_checks();

if (render_mode == "print_carrier") {
    carrier();
} else {
    vent_insert();
    // Ghosts are reference-only and EXCLUDED from print modes so they can never
    // end up in a sliced STL.
    if (render_mode == "preview") {
        ghost_screen();
        ghost_encoder();
        ghost_depth_limit();
        translate([0, 0, plate_t + wall_depth() + 2 + explode])
            color("Plum", 0.5) carrier();
    }
}
