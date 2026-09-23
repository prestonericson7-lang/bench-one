// ============================================================
//  mic-array.scad — 4-microphone beamforming bar for the Zynq audio front-end
//
//  WHY THE SPACING IS WHAT IT IS (derived, not chosen by feel):
//    Beamforming aliases (grating lobes) when mic spacing d > lambda/2 at the
//    highest frequency of interest. Whisper runs at 16 kHz sampling, so the
//    Nyquist limit is 8 kHz.  lambda(8 kHz) = 343 m/s / 8000 Hz = 42.9 mm,
//    therefore  d <= 21.4 mm.  We use 20 mm, giving a little margin.
//
//    If you ever band-limit to 4 kHz telephony, d <= 43 mm would be legal — but
//    20 mm is valid for BOTH, so there's no reason to go wider.
//
//  PHASE COHERENCE IS THE WHOLE POINT: all four mics must sample on the SAME
//  master clock, and the spacing must be accurate. An error in d steers the
//  beam the wrong way, because the beamformer computes its delays from d.
//  Print this bar rather than hand-spacing modules on perfboard.
//
//  Mics: SPH0645 preferred (65 dB SNR) over INMP441 (61 dB). Both are I2S with
//  an L/R select pin, so TWO mics share one I2S bus -> 4 mics = 2 buses.
//
//  PRINT IN PETG OR ASA (cabin heat). PLA will sag.
// ============================================================

/* [Array geometry — derived above, don't widen without redoing the math] */
n_mics       = 4;
spacing      = 20.0;   // mm between mic acoustic centres (<= 21.4 required)

/* [Mic breakout module — ### MEASURE YOURS ###]
   Generic SPH0645/INMP441 breakouts vary by vendor. Measure the PCB and the
   port hole position before printing. */
mod_w        = 15.0;   // ### MEASURE ### module PCB width
mod_h        = 19.0;   // ### MEASURE ### module PCB height
mod_t        = 1.6;    // ### MEASURE ### PCB thickness
port_d       = 3.0;    // acoustic port clearance hole through the bar
port_dx      = 0;      // ### MEASURE ### port offset from module centre
port_dy      = 0;      // ### MEASURE ###
mod_clear    = 0.4;    // pocket slack

/* [Bar] */
bar_t        = 4.0;    // bar thickness behind the modules
end_margin   = 8.0;    // material beyond the outer mics
rail_h       = 6.0;    // pocket wall height
screw_d      = 3.4;    // M3 clearance for mounting feet
foot         = true;   // add mounting feet at both ends

/* [Render] */
$fn          = 48;
render_mode  = "preview";   // ["print","preview"]
ov           = 0.5;         // mating overlap (touching planes don't fuse)

// ---------- derived ----------
span   = (n_mics - 1) * spacing;
bar_w  = span + mod_w + 2*end_margin;
bar_h  = mod_h + 2*4;

module _report() {
    echo(str("MIC ARRAY: ", n_mics, " mics @ ", spacing, " mm -> array span ",
             span, " mm, bar ", bar_w, " x ", bar_h, " mm"));
    lambda_2 = 343000 / 8000 / 2;   // mm, half wavelength at 8 kHz
    echo(str("  spatial Nyquist limit at 8 kHz = ", lambda_2,
             " mm; spacing ", spacing, " mm is ",
             spacing <= lambda_2 ? "OK" : "*** TOO WIDE - WILL ALIAS ***"));
}

module rrect(w, h, r, t) {
    linear_extrude(height = t)
        offset(r = r) offset(r = -r) square([w, h], center = true);
}

module mic_pocket() {
    rrect(mod_w + 2*mod_clear, mod_h + 2*mod_clear, 1, mod_t + 1);
}

module bar() {
    difference() {
        union() {
            rrect(bar_w, bar_h, 3, bar_t);
            // pocket rails
            translate([0, 0, bar_t - ov])
                difference() {
                    rrect(bar_w, bar_h, 3, rail_h + ov);
                    for (i = [0 : n_mics - 1])
                        translate([-span/2 + i*spacing, 0, -0.1])
                            rrect(mod_w + 2*mod_clear, mod_h + 2*mod_clear, 1,
                                  rail_h + ov + 0.2);
                }
            if (foot)
                for (s = [-1, 1])
                    translate([s * (bar_w/2 - 5), 0, 0])
                        cylinder(d = 11, h = bar_t);
        }
        // acoustic ports straight through the bar behind each mic
        for (i = [0 : n_mics - 1])
            translate([-span/2 + i*spacing + port_dx, port_dy, -1])
                cylinder(d = port_d, h = bar_t + rail_h + 4);
        // cable relief between pockets
        for (i = [0 : n_mics - 2])
            translate([-span/2 + i*spacing + spacing/2, 0, -1])
                rrect(spacing - mod_w - 2, 6, 2, bar_t + rail_h + 4);
        // mounting holes in the feet
        if (foot)
            for (s = [-1, 1])
                translate([s * (bar_w/2 - 5), 0, -1])
                    cylinder(d = screw_d, h = bar_t + 4);
    }
}

module ghost_mics() {
    for (i = [0 : n_mics - 1])
        color("Tomato", 0.4)
            translate([-span/2 + i*spacing, 0, bar_t])
                rrect(mod_w, mod_h, 1, mod_t);
}

_report();
bar();
if (render_mode == "preview") ghost_mics();
