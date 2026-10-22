// Ported from maxibitx (https://github.com/MikeJohnshoy/maxibitx, GPL-3.0).
// decim48k.c
//
// Coefficients from docs/dsp_design_notes/usb_uac_decimation_design.md
//
// This filter's job is narrower than antialias.c's: by the time IQ
// reaches here (sound_process() applies antialias_apply() to every
// sample before this), content above ~47.5kHz is already suppressed
// by that filter's own ~-81dB stopband. This filter only needs to
// guard the "gap" between the new 24kHz Nyquist (once decimated to
// 48kHz) and where antialias.c's own rejection has already taken over
// - see the design doc for the full cascade reasoning and why that
// keeps this filter cheap despite a much narrower transition band
// (15kHz-24kHz here, vs. antialias.c's 32kHz-47.5kHz).

#include "decim48k.h"

static const double fir_coeffs[DECIM48K_TAPS] = {
     0.00359949,  0.00298201, -0.00687610, -0.01768023, -0.00898327,
     0.02024500,  0.03267755, -0.00730499, -0.06718464, -0.05092443,
     0.09648483,  0.29725521,
     0.39168302,
     0.29725521,  0.09648483, -0.05092443, -0.06718464, -0.00730499,
     0.03267755,  0.02024500, -0.00898327, -0.01768023, -0.00687610,
     0.00298201,  0.00359949,
};

int decim48k_apply(struct decim48k_state *f, double x, double *out) {
    // Double-write: hist[pos] and hist[pos+TAPS] always hold the same
    // value, so a contiguous TAPS-long read starting anywhere from
    // pos+1 to pos+TAPS never needs to wrap around the buffer (see
    // antialias.c, same trick).
    f->hist[f->pos] = x;
    f->hist[f->pos + DECIM48K_TAPS] = x;

    int base = f->pos + 1;
    f->pos++;
    if (f->pos == DECIM48K_TAPS) f->pos = 0;

    // Decimate by 2: history must be updated on every call (above) so
    // the convolution below always has correct, unbroken context, but
    // the actual filter multiply-accumulate only needs to run on the
    // calls that produce a kept output - skipping it on the discarded
    // half is a genuine, free compute saving, not just simpler code.
    f->phase ^= 1;
    if (!f->phase) return 0;

    double acc = 0.0;
    for (int i = 0; i < DECIM48K_TAPS; i++)
        acc += fir_coeffs[i] * f->hist[base + i];

    *out = acc;
    return 1;
}
