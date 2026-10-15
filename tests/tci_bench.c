// Off-radio bench for the TCI server: the real tci.c, tci_ws.c,
// tci_stream.c, tci_audio.c and iq_baseband.c, with tci_radio.h implemented
// over plain variables and a fake audio thread that delivers 1024-sample
// blocks every 10.667 ms (as the WM8731 does):
//   - ADC: a real tone at IF + 1500 Hz, so I/Q shows a signal at +1500
//   - speaker: a 1000 Hz tone at the AGC's usual peak, so RX audio has one
//   - mic: whatever tci_audio_tx() writes while a client transmits; the
//     bench reports its level and frequency once a second
// Every radio change prints a "radio:" line; tests/tci_test.py reads them.
//
//   make tci-bench && ./tests/tci_bench [port]

#include "../src/iq_baseband.h"
#include "../src/tci.h"
#include "../src/tci_audio.h"
#include "../src/tci_radio.h"
#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// HPSDR isn't part of this bench
int hpsdr_is_connected(void) { return 0; }
void hpsdr_send_iq(const double *i, const double *q, int n) { (void)i, (void)q, (void)n; }

/* ---- radio stub ------------------------------------------------------- */

static int r_freq = 14074000, r_mode = TR_USB, r_tx, r_drive = 40, r_volume = 60, r_rit_on, r_rit,
           r_wpm = 18, r_tx_tci;
static char r_cw[4096];
static int r_cw_len;

int tr_freq(void) { return r_freq; }
void tr_tune(int hz) {
  r_freq = hz;
  printf("radio: freq %d\n", hz);
}
int tr_mode(void) { return r_mode; }
void tr_set_mode(int m) {
  r_mode = m;
  printf("radio: mode %d\n", m);
}
// Like zbitx's tx_on()/tx_off(): requests queue up in order (the UI
// thread's queue) and each lands tx_switch_ms after it was made, or after
// the one before it, whichever is later. TCI_BENCH_TX_DELAY_MS overrides
// the delay (zbitx's UI thread can stall for seconds, e.g. FFTW planning
// after a mode change).
static int tx_switch_ms = 300;
static struct {
  int on;
  long long at_ms;
} tx_q[64];
static int tx_q_head, tx_q_tail;
long long ms_now(void);
int tr_in_tx(void) {
  long long now = ms_now();
  while (tx_q_tail != tx_q_head && now >= tx_q[tx_q_tail % 64].at_ms) {
    int on = tx_q[tx_q_tail++ % 64].on;
    if (on != r_tx) {
      r_tx = on;
      printf("radio: tx is now %d\n", r_tx);
    }
  }
  return r_tx;
}
int tr_set_tx(int on, int tci_audio) {
  if (on && r_freq < 1800000)
    return -1; // bench rule: refuse below 160 m
  long long at = ms_now() + tx_switch_ms;
  if (tx_q_head != tx_q_tail && tx_q[(tx_q_head - 1) % 64].at_ms > at)
    at = tx_q[(tx_q_head - 1) % 64].at_ms;
  tx_q[tx_q_head % 64].on = on;
  tx_q[tx_q_head % 64].at_ms = at;
  tx_q_head++;
  r_tx_tci = on && tci_audio;
  printf("radio: tx %d (%s)\n", on, on ? (tci_audio ? "tci audio" : "loopback") : "off");
  return 0;
}
int tr_drive(void) { return r_drive; }
void tr_set_drive(int p) {
  r_drive = p;
  printf("radio: drive %d\n", p);
}
int tr_volume(void) { return r_volume; }
void tr_set_volume(int p) {
  r_volume = p;
  printf("radio: volume %d\n", p);
}
int tr_rit_on(void) { return r_rit_on; }
int tr_rit(void) { return r_rit; }
void tr_set_rit_on(int on) {
  r_rit_on = on;
  printf("radio: rit_on %d\n", on);
}
void tr_set_rit(int hz) {
  r_rit = hz;
  printf("radio: rit %d\n", hz);
}
int tr_wpm(void) { return r_wpm; }
void tr_set_wpm(int w) {
  r_wpm = w;
  printf("radio: wpm %d\n", w);
}
double tr_smeter_dbm(void) { return -73.0; }
// CW: "keys" one character every 20 ms
static long long cw_next_ms;
long long ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int tr_cw_send(const char *t) {
  int n = 0;
  for (; t[n] && r_cw_len < (int)sizeof(r_cw) - 1; n++)
    r_cw[r_cw_len++] = t[n];
  r_cw[r_cw_len] = 0;
  printf("radio: cw queued \"%s\"\n", t);
  return n;
}
void tr_cw_stop(void) {
  r_cw_len = 0;
  r_cw[0] = 0;
  printf("radio: cw stop\n");
}
int tr_cw_pending(void) { return r_cw_len; }
// The keyer's runtime drops TX unless asked again; the bench just counts
// the requests and reports them once a second.
static long cw_holds;
static long long cw_hold_report_ms;
void tr_cw_hold(void) {
  cw_holds++;
  long long now = ms_now();
  if (now - cw_hold_report_ms >= 1000) {
    printf("radio: cw hold x%ld\n", cw_holds);
    cw_holds = 0;
    cw_hold_report_ms = now;
  }
}
void tr_cw_service(void) {
  long long now = ms_now();
  if (r_cw_len && now >= cw_next_ms) {
    memmove(r_cw, r_cw + 1, (size_t)r_cw_len);
    r_cw_len--;
    cw_next_ms = now + 20;
    if (!r_cw_len)
      printf("radio: cw sent\n");
  }
}

/* ---- fake audio thread -------------------------------------------------- */

static void *audio_main(void *arg) {
  (void)arg;
  const double IF = 600 * (96000.0 / 2048);
  static int32_t adc[1024], spk[1024], mic[1024];
  long n = 0;
  double tx_sum2 = 0, tx_prev = 0;
  long tx_count = 0, tx_zc = 0, blocks = 0;
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  for (;;) {
    for (int k = 0; k < 1024; k++, n++) {
      adc[k] = (int32_t)(0.25 * 2147483647.0 * cos(2 * M_PI * (IF + 1500.0) * n / 96000.0));
      spk[k] = (int32_t)(1e8 * sin(2 * M_PI * 1000.0 * n / 96000.0));
      mic[k] = 0;
    }
    int tx = tr_in_tx();
    iq_baseband_process(adc, 1024, tx);
    if (tci_audio_tx(mic, 1024, tx)) {
      for (int k = 0; k < 1024; k++) {
        double v = mic[k] / 2147483647.0;
        tx_sum2 += v * v;
        if ((v >= 0) != (tx_prev >= 0))
          tx_zc++;
        tx_prev = v;
        tx_count++;
      }
    }
    tci_audio_rx(spk, 1024, tx);

    if (++blocks % 94 == 0 && tx_count) { // about once a second
      printf("radio: tx audio rms %.3f, %.0f Hz\n", sqrt(tx_sum2 / tx_count),
             tx_zc / 2.0 / (tx_count / 96000.0));
      tx_sum2 = 0;
      tx_count = tx_zc = 0;
    }
    next.tv_nsec += 10666667;
    while (next.tv_nsec >= 1000000000L) {
      next.tv_nsec -= 1000000000L;
      next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
  }
  return NULL;
}

// Commands on stdin act as the radio's own controls would:
//   drop      TX drops on its own (panel, timeout) while a client holds it
//   key       the operator keys (mic PTT)
//   tune HZ   the dial is moved from the panel
static void *stdin_main(void *arg) {
  (void)arg;
  char line[64];
  while (fgets(line, sizeof(line), stdin)) {
    if (!strncmp(line, "drop", 4)) {
      tx_q[tx_q_head % 64].on = 0;
      tx_q[tx_q_head % 64].at_ms = 0;
      tx_q_head++;
      printf("radio: dropped TX on its own\n");
    } else if (!strncmp(line, "key", 3)) {
      tx_q[tx_q_head % 64].on = 1;
      tx_q[tx_q_head % 64].at_ms = 0;
      tx_q_head++;
      printf("radio: keyed by the operator (mic PTT)\n");
    } else if (!strncmp(line, "tune ", 5)) {
      r_freq = atoi(line + 5);
      printf("radio: dial moved to %d on the panel\n", r_freq);
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int port = argc > 1 ? atoi(argv[1]) : TCI_DEFAULT_PORT;
  const char *d = getenv("TCI_BENCH_TX_DELAY_MS");
  if (d)
    tx_switch_ms = atoi(d);
  iq_baseband_init(600 * (96000.0 / 2048));
  if (tci_init("127.0.0.1", port, TCI_DEFAULT_MAX_CLIENTS) < 0) {
    fprintf(stderr, "bench: can't listen on %d\n", port);
    return 1;
  }
  pthread_t t;
  pthread_create(&t, NULL, audio_main, NULL);
  pthread_t t2;
  pthread_create(&t2, NULL, stdin_main, NULL);
  printf("bench: TCI on 127.0.0.1:%d\n", port);
  for (;;)
    pause();
}
