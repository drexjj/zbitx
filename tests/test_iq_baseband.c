// Bench test for iq_baseband.c: feeds synthetic real IF tones through the
// I/Q front end and checks where they land in the complex baseband.
//
//   make test-iq
//
// Checks, at center_bin=600 (IF 28125 Hz):
//   - a tone d Hz above the dial comes out at +d (not mirrored)
//   - the mirror image (-d) is suppressed
//   - CW/CWR pitch offsets put the dial back at DC
//   - a full-scale tone doesn't exceed full scale
//   - iq_swap mirrors the spectrum

#include "../src/iq_baseband.h"
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int hpsdr_is_connected(void) { return 1; }
void hpsdr_send_iq(const double *i, const double *q, int n) {
  (void)i;
  (void)q;
  (void)n;
}

#define FS 96000.0
#define N 16384
#define SETTLE 2048

static int32_t in[N + SETTLE];
static double oi[N + SETTLE], oq[N + SETTLE];
static int failures = 0;

// Power (dB, relative to a unit complex exponential) of the baseband at f,
// via a windowed single-bin DFT over the settled part of the output.
static double power_at(double f) {
  double complex acc = 0;
  double wsum = 0;
  for (int n = 0; n < N; n++) {
    double w = 0.5 - 0.5 * cos(2 * M_PI * n / (N - 1));
    double complex x = oi[SETTLE + n] + I * oq[SETTLE + n];
    acc += w * x * cexp(-I * 2 * M_PI * f * n / FS);
    wsum += w;
  }
  return 20 * log10(cabs(acc) / wsum + 1e-20);
}

static void run(double if_hz, double tone_audio_hz, double amp) {
  for (int n = 0; n < N + SETTLE; n++)
    in[n] = (int32_t)(amp * 2147483647.0 * cos(2 * M_PI * tone_audio_hz * n / FS));
  // process in audio-thread-sized blocks, as sound_process() does
  for (int off = 0; off < N + SETTLE; off += 1024)
    iq_baseband_convert(in + off, oi + off, oq + off, 1024);
  (void)if_hz;
}

static void check(const char *name, int ok, const char *fmt, double a, double b) {
  printf("%-46s %s  ", name, ok ? "PASS" : "FAIL");
  printf(fmt, a, b);
  printf("\n");
  if (!ok)
    failures++;
}

int main(void) {
  const double IF = 600 * (96000.0 / 2048); // center_bin=600 -> 28125 Hz
  iq_baseband_init(IF);

  // USB/LSB/AM: no offset. Tone 1500 Hz above dial sits at IF+1500 at the ADC.
  iq_baseband_set_offset(0);
  double offsets[] = {1500, -1500, 300, -2800, 8000, -10000};
  for (unsigned k = 0; k < sizeof(offsets) / sizeof(offsets[0]); k++) {
    double d = offsets[k];
    run(IF, IF + d, 0.5);
    double want = power_at(d), image = power_at(-d);
    char name[64];
    snprintf(name, sizeof(name), "tone at dial%+6.0f Hz lands at %+6.0f", d, d);
    check(name, want > -7.0 && want < -5.0, "level %.1f dB, image %.1f dB", want, image);
    snprintf(name, sizeof(name), "  image rejection at %+6.0f", -d);
    check(name, want - image > 60.0, "%.1f dB (%.0f)", want - image, 0);
  }

  // The real signal's self-image lands at fs - 2*IF - d; must be gone.
  run(IF, IF + 2000, 0.5);
  double self_image = power_at(96000 - 2 * IF - 2000 - 96000); // wrapped to -ve side
  double self_image2 = power_at(96000 - 2 * IF - 2000);
  double worst = self_image > self_image2 ? self_image : self_image2;
  check("self-image of IF suppressed", worst < -66.0, "%.1f dB (%.0f)", worst, 0);

  // CW: radio_tune_to() lowers clk2 by pitch, so a carrier on the dial
  // reaches the ADC at IF + pitch. With offset +pitch it must be DC.
  iq_baseband_init(IF);
  iq_baseband_set_offset(700);
  run(IF, IF + 700, 0.5);
  check("CW: carrier on dial lands at DC", power_at(0) > -7.0, "DC %.1f dB, +700 %.1f dB",
        power_at(0), power_at(700));
  iq_baseband_set_offset(-700);
  run(IF, IF - 700, 0.5);
  check("CWR: carrier on dial lands at DC", power_at(0) > -7.0, "DC %.1f dB, -700 %.1f dB",
        power_at(0), power_at(-700));
  iq_baseband_set_offset(0);

  // Full-scale input must not exceed full scale at the output (HPSDR packs
  // +-1.0 into 24 bits).
  run(IF, IF + 1000, 1.0);
  double peak = 0;
  for (int n = SETTLE; n < N + SETTLE; n++) {
    double m = cabs(oi[n] + I * oq[n]);
    if (m > peak)
      peak = m;
  }
  check("full-scale tone stays within +-1.0", peak <= 1.0001, "peak |IQ| %.4f (%.0f)", peak, 0);

  // Swap mirrors the spectrum.
  iq_baseband_set_swap(1);
  run(IF, IF + 1500, 0.5);
  check("iq_swap=1 mirrors +1500 to -1500", power_at(-1500) - power_at(1500) > 60.0,
        "-1500 %.1f dB, +1500 %.1f dB", power_at(-1500), power_at(1500));
  iq_baseband_set_swap(0);

  // Also works at the old center_bin=512 (IF 24000).
  iq_baseband_init(24000);
  run(24000, 24000 + 2500, 0.5);
  check("center_bin=512: +2500 lands at +2500", power_at(2500) - power_at(-2500) > 60.0,
        "%.1f dB vs image %.1f dB", power_at(2500), power_at(-2500));

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
