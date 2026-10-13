// iq_baseband.h - complex baseband I/Q from the zBitx's real IF samples
//
// The WM8731 captures one real channel at 96 kHz with the crystal filter's
// passband sitting at the IF (tx_shift bins, 28125 Hz for center_bin=600).
// rx_linear() never produces time-domain I/Q - it filters in the frequency
// domain - so external I/Q consumers (HPSDR Protocol 1, and later TCI and
// an ALSA I/Q loopback) get their own front end here:
//
//   real ADC sample -> complex NCO at -(IF + offset) -> lowpass per rail
//
// The result is 96 kHz complex baseband, not spectrally inverted: a signal
// d Hz above the dial frequency comes out at +d Hz. offset compensates for
// the CW/CWR pitch radio_tune_to() folds into the LO, so DC is always the
// dial frequency whatever the mode.

#ifndef IQ_BASEBAND_H
#define IQ_BASEBAND_H

#include <stdint.h>

#define IQ_BB_RATE 96000
#define IQ_BB_MAX_BLOCK 4096

// Call once at startup, after center_bin (tx_shift) is known. if_hz is the
// IF the crystal filter passband is centered on at the ADC.
void iq_baseband_init(double if_hz);

// Extra NCO offset in Hz, from radio_tune_to(): +pitch in CW, -pitch in
// CWR, 0 otherwise. Safe to call from any thread.
void iq_baseband_set_offset(int offset_hz);

// Set to 1 to swap I and Q (mirror the spectrum) if a unit's mixing turns
// out the other way round. From hw_settings.ini's iq_swap.
void iq_baseband_set_swap(int swap);

// Audio thread only. Converts one block of raw ADC samples to I/Q and hands
// it to every I/Q consumer. While transmitting, pass in_tx=1: consumers get
// silence so their packet cadence never stalls.
void iq_baseband_process(const int32_t *input_rx, int n, int in_tx);

// Pure DSP step, exposed for the bench test: fills out_i/out_q, no fan-out.
void iq_baseband_convert(const int32_t *input_rx, double *out_i, double *out_q, int n);

#endif
