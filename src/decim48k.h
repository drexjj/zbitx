// Ported from maxibitx (https://github.com/MikeJohnshoy/maxibitx, GPL-3.0).
// decim48k.h
//
// Decimates minibitx's native 96kHz baseband I/Q by 2, to the 48kHz
// usb_gadget.c's UAC2 gadget actually advertises. See
// docs/dsp_design_notes/usb_uac_decimation_design.md for the filter
// design and why this exists.

#ifndef DECIM48K_H
#define DECIM48K_H

#define DECIM48K_TAPS 25

// One instance per rail - I and Q each need independent history and
// decimation phase, but share the same coefficient table (decim48k.c).
// Zero-initialize (static/global storage does this automatically)
// before first use. Same double-write history layout as
// antialias_state - see antialias.h for why.
struct decim48k_state {
    double hist[2 * DECIM48K_TAPS];
    int pos;     // write cursor, always in [0, DECIM48K_TAPS)
    int phase;   // toggles every call; only every other call emits output
};

// Feed one 96kHz-rate input sample. Every call updates the filter's
// history (needed for correct output on the calls that do emit), but
// only every other call actually produces a decimated 48kHz output
// sample - that call writes *out and returns 1; the discarded call
// returns 0 and leaves *out untouched. Call once per input sample, per
// rail, in lockstep with the other rail (both need to be fed on every
// input sample so their phases stay aligned with each other).
int decim48k_apply(struct decim48k_state *f, double x, double *out);

#endif /* DECIM48K_H */
