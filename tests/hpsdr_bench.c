// Off-radio bench for hpsdr_p1.c + iq_baseband.c.
//
// Runs the real HPSDR server on UDP 1024 with a fake audio thread that
// delivers 1024-sample blocks every 10.667 ms (as the WM8731 does), carrying
// a real tone at IF + TONE_OFFSET_HZ. The zBitx calls the server makes
// (cmd_exec, hamlib_tx) are logged instead of acted on. Drive it with
// tests/hpsdr_client.py.
//
//   make hpsdr-bench && ./hpsdr_bench [allow_mox]

#include "../src/hpsdr_p1.h"
#include "../src/iq_baseband.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define TONE_OFFSET_HZ 3000.0

static int bench_in_tx = 0;
// TCI isn't part of this test
int tci_iq_wanted(void) { return 0; }
void tci_push_iq(const double *i, const double *q, int n) {
  (void)i;
  (void)q;
  (void)n;
}
int is_in_tx(void) { return bench_in_tx; }
void cmd_exec(char *cmd) {
  printf("bench: cmd_exec(\"%s\")\n", cmd);
  fflush(stdout);
}
void hamlib_tx(int on) {
  bench_in_tx = on;
  printf("bench: hamlib_tx(%d)\n", on);
  fflush(stdout);
}

static void *audio_thread(void *arg) {
  (void)arg;
  const double IF = 600 * (96000.0 / 2048);
  static int32_t block[1024];
  long n = 0;
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  for (;;) {
    for (int k = 0; k < 1024; k++, n++)
      block[k] = (int32_t)(0.25 * 2147483647.0 *
                           cos(2 * M_PI * (IF + TONE_OFFSET_HZ) * n / 96000.0));
    iq_baseband_process(block, 1024, bench_in_tx);
    next.tv_nsec += 10666667;
    while (next.tv_nsec >= 1000000000L) {
      next.tv_nsec -= 1000000000L;
      next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
  }
  return NULL;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int allow_mox = argc > 1 ? atoi(argv[1]) : 0;
  iq_baseband_init(600 * (96000.0 / 2048));
  hpsdr_configure(1, allow_mox, 0.0, 1);
  if (hpsdr_init() < 0)
    return 1;
  pthread_t t;
  pthread_create(&t, NULL, audio_thread, NULL);
  printf("bench: running, tone at dial %+.0f Hz\n", TONE_OFFSET_HZ);
  for (;;)
    pause();
}
