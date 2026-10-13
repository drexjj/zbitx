// Ported from maxibitx (https://github.com/MikeJohnshoy/maxibitx, GPL-3.0).
// upsample48k.h
//
// Upsamples 48kHz real audio to minibitx's native 96kHz TX rate - the
// mirror-image operation of decim48k.c's 96kHz->96kHz->48kHz decimation.
// First (and so far only) user: usb_gadget.c's UAC2 gadget, once it
// carries real bidirectional audio instead of I/Q (docs/ARCHITECTURE.md's
// WSJT-X TX audio bridge) - the host (WSJT-X) only ever sends 48kHz audio
// in, but sound.c's TX audio block (and tx_pipeline.c underneath it) is
// fixed at 96kHz, the same way every other TX audio source (cw.c's
// sidetone, mic_buf) already is. Nothing before this needed to go this
// direction - RX audio/I/Q only ever LEFT the box at a lower rate, never
// arrived at one needing to be raised - so no upsampler existed yet.
//
// Classic zero-stuff-then-lowpass L=2 interpolation, not a novel design:
// insert one zero sample after each real input sample (doubling the rate),
// then run the combined stream through a lowpass filter aimed at the
// ORIGINAL signal's 24kHz Nyquist edge to reject the spectral images
// zero-stuffing introduces at multiples of 48kHz, then restore the
// average energy the inserted zeros halved with a gain-of-2 makeup factor
// (upsample48k.c's UPSAMPLE48K_GAIN).
//
// Deliberately reuses decim48k.c's own 25-tap coefficient table rather
// than deriving a new one (duplicated here, not shared via a public
// symbol - decim48k.c keeps its own table file-static, and this filter's
// use is different enough - interpolation vs. decimation - to warrant its
// own name and header even though the numbers are identical): an
// anti-aliasing filter for M=2 decimation and an anti-imaging filter for
// L=2 interpolation are the SAME prototype lowpass (same cutoff, same
// transition band) whenever the up/down factor matches - only where the
// zeros go (before the filter, for interpolation; nowhere, for
// decimation, which instead only evaluates the filter on every other
// call) and the gain-of-L correction differ. See
// docs/dsp_design_notes/usb_uac_decimation_design.md for that filter's own
// passband/transition-band derivation (15kHz passband edge, 24kHz Nyquist
// stopband edge) - it applies unchanged here, just run in the opposite
// direction.

#ifndef UPSAMPLE48K_H
#define UPSAMPLE48K_H

#define UPSAMPLE48K_TAPS 25

// One instance per real-audio rail (only one is needed today - mono
// WSJT-X TX audio - but this is sized/shaped like decim48k_state so a
// second rail is a one-line addition if a future caller ever needs
// stereo). Zero-initialize (static/global storage does this
// automatically) before first use. Same double-write history layout as
// decim48k_state/antialias_state - see antialias.h for why.
struct upsample48k_state {
    double hist[2 * UPSAMPLE48K_TAPS];
    int pos; // write cursor, always in [0, UPSAMPLE48K_TAPS)
};

// Feed one 48kHz-rate input sample; always produces exactly two 96kHz-rate
// output samples in out[0]/out[1] (the interpolated pair straddling this
// input sample) - unlike decim48k_apply()'s one-in/sometimes-one-out
// shape (decimation discards half its calls' worth of output),
// interpolation is one-in/always-two-out, so this has no bool return to
// check. Call once per 48kHz-rate input sample; out[]'s two samples are
// then this input's contribution to the 96kHz stream, in order.
void upsample48k_apply(struct upsample48k_state *f, double x, double out[2]);

#endif /* UPSAMPLE48K_H */
