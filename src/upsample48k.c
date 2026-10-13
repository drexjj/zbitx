// Ported from maxibitx (https://github.com/MikeJohnshoy/maxibitx, GPL-3.0).
// upsample48k.c
//
// See upsample48k.h for what this is and why it reuses decim48k.c's own
// coefficient table.

#include "upsample48k.h"

// Reused verbatim from decim48k.c (docs/dsp_design_notes/
// usb_uac_decimation_design.md) - see this file's header for why the same
// prototype lowpass is correct for both decimation and interpolation.
static const double fir_coeffs[UPSAMPLE48K_TAPS] = {
     0.00359949,  0.00298201, -0.00687610, -0.01768023, -0.00898327,
     0.02024500,  0.03267755, -0.00730499, -0.06718464, -0.05092443,
     0.09648483,  0.29725521,
     0.39168302,
     0.29725521,  0.09648483, -0.05092443, -0.06718464, -0.00730499,
     0.03267755,  0.02024500, -0.00898327, -0.01768023, -0.00687610,
     0.00298201,  0.00359949,
};

// Interpolation makeup gain: zero-stuffing halves the signal's average
// energy (half of every two output samples is now exactly zero going
// into the filter) - this restores unity passband gain for the
// interpolated output. The standard L=2 interpolator correction, not
// specific to this filter's own coefficients.
#define UPSAMPLE48K_GAIN 2.0

// Pushes one new sample (a real input sample, or an inserted zero) into
// the shared history and returns this filter's output for it - the same
// history-buffer/double-write trick decim48k_apply() uses (a contiguous
// TAPS-long read starting anywhere from pos+1 to pos+TAPS never needs to
// wrap), just run once per OUTPUT sample here rather than once per input
// sample, since interpolation emits more samples than it consumes.
static double push_and_convolve(struct upsample48k_state *f, double x) {
    f->hist[f->pos] = x;
    f->hist[f->pos + UPSAMPLE48K_TAPS] = x;

    int base = f->pos + 1;
    f->pos++;
    if (f->pos == UPSAMPLE48K_TAPS) f->pos = 0;

    double acc = 0.0;
    for (int i = 0; i < UPSAMPLE48K_TAPS; i++)
        acc += fir_coeffs[i] * f->hist[base + i];
    return acc;
}

void upsample48k_apply(struct upsample48k_state *f, double x, double out[2]) {
    // Zero-stuff: this call's real input sample, then an inserted zero -
    // the two 96kHz-rate slots this one 48kHz-rate input expands into.
    // Both go through the exact same lowpass history/convolution
    // (push_and_convolve() above); unlike decim48k_apply(), which skips a
    // whole convolution on its discarded phase, neither call here is
    // discarded - the "zero" is a genuinely zero-valued INPUT sample the
    // filter still has to process, not a computation being skipped.
    out[0] = UPSAMPLE48K_GAIN * push_and_convolve(f, x);
    out[1] = UPSAMPLE48K_GAIN * push_and_convolve(f, 0.0);
}
