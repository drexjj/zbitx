// iq_baseband.c - complex baseband I/Q from the zBitx's real IF samples
// See iq_baseband.h for the signal path.

// zBitx's default build is -O0 (-g); this runs at 96 kHz on the audio
// thread, so optimize this one file regardless.
#pragma GCC optimize("O2")

#include "iq_baseband.h"
#include "hpsdr_p1.h"
#include <complex.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

// Lowpass applied to I and Q after mixing. The self-image of the real IF
// signal lands at -(2*IF + d), which wraps to fs - 2*IF - d; the cutoff sits
// halfway between the wanted band and that image (19875 Hz at IF 28125),
// capped below Nyquist. 63-tap Blackman windowed sinc: transition ~8 kHz,
// stopband better than 70 dB.
// The filter runs in float (the Pi's 32-bit build has hard float but no
// auto-vectorized double) and folds its symmetric taps, so each rail costs
// 32 multiplies a sample.
#define LPF_TAPS 63
#define LPF_HALF (LPF_TAPS / 2) // 31 tap pairs + the center tap
#define LPF_MAX_CUTOFF_HZ 20000.0

static float lpf_coeff[LPF_TAPS];

struct lpf_state {
  float hist[2 * LPF_TAPS]; // every sample written twice: no wrap in the MAC loop
  int pos;
};

static struct lpf_state lpf_i, lpf_q;

static double if_freq_hz = 28125.0;
static atomic_int nco_offset_hz = 0;
static atomic_int iq_swap = 0;

static double complex nco = 1.0;      // current LO phasor
static double complex nco_step = 1.0; // per-sample rotation
static int nco_step_for_offset = -1000000;

static void design_lowpass(double cutoff_hz) {
  double fc = cutoff_hz / IQ_BB_RATE;
  int m = LPF_TAPS - 1;
  double c[LPF_TAPS], sum = 0.0;
  for (int n = 0; n < LPF_TAPS; n++) {
    double x = n - m / 2.0;
    double sinc = (x == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * x) / (M_PI * x);
    double w = 0.42 - 0.5 * cos(2.0 * M_PI * n / m) + 0.08 * cos(4.0 * M_PI * n / m);
    c[n] = sinc * w;
    sum += c[n];
  }
  for (int n = 0; n < LPF_TAPS; n++)
    lpf_coeff[n] = (float)(c[n] / sum); // unity gain at DC
}

static inline float lpf_apply(struct lpf_state *f, float x) {
  f->hist[f->pos] = x;
  f->hist[f->pos + LPF_TAPS] = x;
  const float *h = &f->hist[f->pos + 1];
  float acc = lpf_coeff[LPF_HALF] * h[LPF_HALF];
  for (int i = 0; i < LPF_HALF; i++)
    acc += lpf_coeff[i] * (h[i] + h[LPF_TAPS - 1 - i]);
  if (++f->pos == LPF_TAPS)
    f->pos = 0;
  return acc;
}

void iq_baseband_init(double if_hz) {
  if_freq_hz = if_hz;
  double cutoff = 0.5 * (IQ_BB_RATE - 2.0 * if_hz);
  if (cutoff > LPF_MAX_CUTOFF_HZ || cutoff <= 0.0)
    cutoff = LPF_MAX_CUTOFF_HZ;
  design_lowpass(cutoff);
  memset(&lpf_i, 0, sizeof(lpf_i));
  memset(&lpf_q, 0, sizeof(lpf_q));
  nco = 1.0;
  nco_step_for_offset = -1000000; // force the step to be recomputed
  printf("iq: baseband I/Q from IF %.1f Hz, lowpass %.0f Hz\n", if_hz, cutoff);
}

void iq_baseband_set_offset(int offset_hz) {
  atomic_store_explicit(&nco_offset_hz, offset_hz, memory_order_relaxed);
}

void iq_baseband_set_swap(int swap) {
  atomic_store_explicit(&iq_swap, swap ? 1 : 0, memory_order_relaxed);
}

void iq_baseband_convert(const int32_t *input_rx, double *out_i, double *out_q, int n) {
  int off = atomic_load_explicit(&nco_offset_hz, memory_order_relaxed);
  if (off != nco_step_for_offset) {
    double w = -2.0 * M_PI * (if_freq_hz + off) / IQ_BB_RATE;
    nco_step = cos(w) + I * sin(w);
    nco_step_for_offset = off;
  }
  int swap = atomic_load_explicit(&iq_swap, memory_order_relaxed);

  for (int k = 0; k < n; k++) {
    // x2: a real cosine splits its energy between +f and -f; keep the
    // wanted half at the input's own amplitude.
    double x = 2.0 * (double)input_rx[k] / 2147483648.0;
    double lo_re = creal(nco), lo_im = cimag(nco);
    double st_re = creal(nco_step), st_im = cimag(nco_step);
    // written out by hand: a C complex multiply calls __muldc3 for NaN/inf
    // handling, which costs more than the rest of this loop
    nco = (lo_re * st_re - lo_im * st_im) + I * (lo_re * st_im + lo_im * st_re);
    double i = lpf_apply(&lpf_i, (float)(x * lo_re));
    double q = lpf_apply(&lpf_q, (float)(x * lo_im));
    if (swap) {
      out_i[k] = q;
      out_q[k] = i;
    } else {
      out_i[k] = i;
      out_q[k] = q;
    }
  }
  // Rounding slowly walks |nco| away from 1; pull it back once a block.
  nco /= cabs(nco);
}

void iq_baseband_process(const int32_t *input_rx, int n, int in_tx) {
  static double bb_i[IQ_BB_MAX_BLOCK];
  static double bb_q[IQ_BB_MAX_BLOCK];
  if (n > IQ_BB_MAX_BLOCK)
    n = IQ_BB_MAX_BLOCK;

  // Nothing listening: skip the DSP entirely.
  if (!hpsdr_is_connected())
    return;

  if (in_tx) {
    memset(bb_i, 0, n * sizeof(double));
    memset(bb_q, 0, n * sizeof(double));
  } else {
    iq_baseband_convert(input_rx, bb_i, bb_q, n);
  }

  hpsdr_send_iq(bb_i, bb_q, n);
}
