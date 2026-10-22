// hpsdr_p1.c - openHPSDR Protocol 1 server for the zBitx
//
// Ported from maxibitx (https://github.com/MikeJohnshoy/maxibitx,
// src/interfaces/hpsdr_p1.c, GPL-3.0), whose discovery and EP2 parsing are
// based on hpsdrsim.c from the piHPSDR project (GPL-2.0-or-later).
//
// What it does:
//   - answers discovery on UDP 1024 as a single-ADC Hermes-class radio
//   - streams the receive I/Q from iq_baseband.c as EP6 packets at the rate
//     the app asks for (48 or 96 kHz; 192/384 are answered at 96 kHz), to as
//     many receivers as the app asks for (each gets the same I/Q)
//   - follows the app's RX1 frequency
//   - optionally keys the radio from the app's MOX bit (hpsdr_allow_mox)
// What it does not do: transmit the app's TX I/Q, or report the radio's own
// frequency back (Protocol 1 has no way to); pair the app with Hamlib CAT on
// port 4532 for two-way frequency sync.
//
// Threads: the audio thread only ever appends to a lock-free SPSC ring
// (hpsdr_send_iq()); a pacer thread drains it one packet per packet-time on
// an absolute clock; a poll thread handles inbound UDP. Sending from the
// audio thread, sleeping in it, or locking it against the network all cause
// xruns (see maxibitx docs/04_remote_control_and_iq_output.md).

#include "hpsdr_p1.h"
#include <arpa/inet.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// zBitx side
extern void cmd_exec(char *cmd);
extern void hamlib_tx(int tx_input);
extern int is_in_tx(void);

#define EP_PKT_SIZE 1032
#define FRAME_PAYLOAD 504 // bytes of samples in each 512-byte sub-frame
#define MAX_RX 8
#define CLIENT_TIMEOUT_MS 3000

#define IQ_QUEUE_CAP 8192 // power of two; ~85 ms at 96 kHz
#define IQ_QUEUE_MASK (IQ_QUEUE_CAP - 1)

// --- configuration -----------------------------------------------------------
static int cfg_enable = 1;
static int cfg_allow_mox = 0;
static double cfg_gain = 1.0;
static int cfg_board_id = 0x01;

#define FIRMWARE_VERSION 0x1F
static const uint8_t mac[6] = {0x00, 0x1C, 0xC0, 0xA2, 0x22, 0x5B};

// --- state -------------------------------------------------------------------
static int sock = -1;
static atomic_int running = 0;
static atomic_int client_active = 0;
static struct sockaddr_in stream_dest;
static pthread_mutex_t dest_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t poll_thread, pacer_thread;
static uint32_t tx_seq = 0;
static atomic_long last_heard_ms = 0;

// What the app asked for in C&C address 0.
static atomic_int app_rate = 96000;
static atomic_int app_nrx = 1;

// SPSC ring: audio thread writes q_head, pacer thread writes q_tail.
static float q_i[IQ_QUEUE_CAP];
static float q_q[IQ_QUEUE_CAP];
static atomic_uint q_head = 0;
static atomic_uint q_tail = 0;
static atomic_int reset_requested = 0;

// Inbound tracking (poll thread only).
static uint32_t app_rx_freq = 0, app_tx_freq = 0;
static int net_mox = 0;

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void hpsdr_configure(int enable, int allow_mox, double gain_db, int board_id) {
  cfg_enable = enable;
  cfg_allow_mox = allow_mox;
  cfg_gain = pow(10.0, gain_db / 20.0);
  cfg_board_id = board_id;
}

int hpsdr_is_connected(void) {
  return atomic_load_explicit(&client_active, memory_order_relaxed);
}

// --- outbound: audio thread --------------------------------------------------

void hpsdr_send_iq(const double *i_samples, const double *q_samples, int n) {
  static int decim_phase = 0;
  if (!hpsdr_is_connected())
    return;

  // 48 kHz: keep every other sample. iq_baseband.c's lowpass is already
  // below 20 kHz, so this needs no further filtering.
  int step = (atomic_load_explicit(&app_rate, memory_order_relaxed) == 48000) ? 2 : 1;

  unsigned head = atomic_load_explicit(&q_head, memory_order_relaxed);
  unsigned tail = atomic_load_explicit(&q_tail, memory_order_acquire);
  for (int k = 0; k < n; k++) {
    if (step == 2) {
      decim_phase ^= 1;
      if (decim_phase)
        continue;
    }
    unsigned next = (head + 1) & IQ_QUEUE_MASK;
    if (next == tail)
      continue; // full: drop rather than block the audio thread
    q_i[head] = (float)(i_samples[k] * cfg_gain);
    q_q[head] = (float)(q_samples[k] * cfg_gain);
    head = next;
  }
  atomic_store_explicit(&q_head, head, memory_order_release);
}

// --- outbound: pacer thread --------------------------------------------------

static inline void put24(uint8_t *p, float v) {
  long s = lrintf(v * 8388607.0f);
  if (s > 8388607)
    s = 8388607;
  if (s < -8388608)
    s = -8388608;
  p[0] = (s >> 16) & 0xFF;
  p[1] = (s >> 8) & 0xFF;
  p[2] = s & 0xFF;
}

// Samples per sub-frame for nrx receivers: each sample is nrx * (I24,Q24)
// followed by a 16-bit mic sample.
static int samples_per_frame(int nrx) { return FRAME_PAYLOAD / (6 * nrx + 2); }

static void build_and_send(const float *bi, const float *bq, int nrx, int spf) {
  uint8_t pkt[EP_PKT_SIZE];
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = 0xEF;
  pkt[1] = 0xFE;
  pkt[2] = 0x01;
  pkt[3] = 0x06; // EP6
  uint32_t seq = tx_seq++;
  pkt[4] = seq >> 24;
  pkt[5] = seq >> 16;
  pkt[6] = seq >> 8;
  pkt[7] = seq;

  for (int frame = 0; frame < 2; frame++) {
    uint8_t *fp = pkt + 8 + frame * 512;
    fp[0] = fp[1] = fp[2] = 0x7F;
    // C0: bits 7:3 = status field address (rotated 0-4 as real hardware
    // does), bit 0 = PTT. Paddle bits stay 0.
    int addr = (int)((seq * 2 + frame) % 5);
    fp[3] = (uint8_t)((addr << 3) | (is_in_tx() ? 1 : 0));
    if (addr == 0)
      fp[6] = FIRMWARE_VERSION; // C3; C1 bit 0 (ADC overload) left clear

    uint8_t *sp = fp + 8;
    for (int s = 0; s < spf; s++) {
      int idx = frame * spf + s;
      for (int r = 0; r < nrx; r++) {
        put24(sp, bi[idx]);
        put24(sp + 3, bq[idx]);
        sp += 6;
      }
      sp += 2; // mic sample, unused
    }
  }

  struct sockaddr_in dest;
  pthread_mutex_lock(&dest_lock);
  dest = stream_dest;
  pthread_mutex_unlock(&dest_lock);
  sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dest, sizeof(dest));
}

static void *pacer_main(void *arg) {
  (void)arg;
  static float bi[2 * 63], bq[2 * 63];
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  long backlog_log_ms = 0;

  while (atomic_load(&running)) {
    int nrx = atomic_load_explicit(&app_nrx, memory_order_relaxed);
    int rate = atomic_load_explicit(&app_rate, memory_order_relaxed);
    int spf = samples_per_frame(nrx);
    long interval_ns = (long)(2LL * spf * 1000000000LL / rate);

    next.tv_nsec += interval_ns;
    while (next.tv_nsec >= 1000000000L) {
      next.tv_nsec -= 1000000000L;
      next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

    if (!hpsdr_is_connected()) {
      clock_gettime(CLOCK_MONOTONIC, &next); // don't burst-catch-up on reconnect
      continue;
    }

    // App went away without sending STOP (crash, Wi-Fi drop).
    if (now_ms() - atomic_load(&last_heard_ms) > CLIENT_TIMEOUT_MS) {
      atomic_store(&client_active, 0);
      printf("hpsdr: client silent for %d ms, streaming stopped\n", CLIENT_TIMEOUT_MS);
      continue;
    }

    if (atomic_exchange_explicit(&reset_requested, 0, memory_order_relaxed)) {
      unsigned h = atomic_load_explicit(&q_head, memory_order_acquire);
      atomic_store_explicit(&q_tail, h, memory_order_release);
    }

    unsigned head = atomic_load_explicit(&q_head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&q_tail, memory_order_relaxed);
    unsigned avail = (head - tail) & IQ_QUEUE_MASK;
    int need = 2 * spf;

    if ((int)avail < need)
      continue; // just after connect, before the ring has filled

    // The audio clock and CLOCK_MONOTONIC drift apart, and ticks that found
    // the ring short are lost; if a backlog builds past ~40 ms, send an
    // extra packet this tick to drain it. Only a backlog past 100 ms (the
    // pacer really falling behind) is worth a log line.
    int packets = (avail > (unsigned)(need + rate / 25)) ? 2 : 1;
    if (avail > (unsigned)(rate / 10) && now_ms() - backlog_log_ms > 10000) {
      backlog_log_ms = now_ms();
      printf("hpsdr: I/Q backlog %u samples, catching up\n", avail);
    }

    for (int p = 0; p < packets; p++) {
      for (int s = 0; s < need; s++) {
        bi[s] = q_i[tail];
        bq[s] = q_q[tail];
        tail = (tail + 1) & IQ_QUEUE_MASK;
      }
      atomic_store_explicit(&q_tail, tail, memory_order_release);
      build_and_send(bi, bq, nrx, spf);
    }
  }
  return NULL;
}

// --- inbound: poll thread ----------------------------------------------------

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void process_ep2_frame(const uint8_t *fp) {
  if (fp[0] != 0x7F || fp[1] != 0x7F || fp[2] != 0x7F)
    return;
  int addr = (fp[3] >> 1) & 0x7F;
  const uint8_t *c = fp + 4; // C1..C4

  switch (addr) {
  case 0x00: {
    static const int rates[4] = {48000, 96000, 192000, 384000};
    static int last_want = 0;
    int want = rates[c[0] & 0x03];
    int rate = (want == 48000) ? 48000 : 96000;
    if (want != last_want) {
      last_want = want;
      if (want > 96000)
        printf("hpsdr: app asked for %d Hz, sending 96000 (the radio's native rate)\n", want);
      else
        printf("hpsdr: sample rate %d Hz\n", rate);
    }
    if (rate != atomic_load(&app_rate)) {
      atomic_store(&app_rate, rate);
      atomic_store(&reset_requested, 1);
    }
    int nrx = ((c[3] >> 3) & 0x07) + 1;
    if (nrx != atomic_load(&app_nrx)) {
      printf("hpsdr: %d receiver%s (all carry the same I/Q)\n", nrx, nrx > 1 ? "s" : "");
      atomic_store(&app_nrx, nrx);
    }

    // MOX lives in C0 bit 0 of every frame, but only address 0 is trusted
    // (maxibitx found other addresses carry stale bits from some apps).
    int want_tx = fp[3] & 0x01;
    if (want_tx != net_mox) {
      net_mox = want_tx;
      if (cfg_allow_mox && want_tx != is_in_tx()) {
        if (want_tx) {
          uint32_t f = app_tx_freq ? app_tx_freq : app_rx_freq;
          if (f) {
            char cmd[40];
            snprintf(cmd, sizeof(cmd), "freq %u", f);
            cmd_exec(cmd);
          }
        }
        hamlib_tx(want_tx);
        printf("hpsdr: MOX -> %s\n", want_tx ? "TX on" : "TX off");
      } else if (!cfg_allow_mox && want_tx) {
        printf("hpsdr: MOX ignored (hpsdr_allow_mox=0)\n");
      }
    }
  } break;

  case 0x01: { // TX frequency
    uint32_t f = be32(c);
    if (f)
      app_tx_freq = f;
  } break;

  case 0x02: { // RX1 frequency. Act only when the app changes it, so the
               // radio's own knob isn't snapped back on every packet.
    uint32_t f = be32(c);
    if (f && f != app_rx_freq) {
      app_rx_freq = f;
      if (!is_in_tx() && f >= 100000 && f <= 60000000) {
        char cmd[40];
        snprintf(cmd, sizeof(cmd), "freq %u", f);
        cmd_exec(cmd);
        printf("hpsdr: tune -> %u Hz\n", f);
      }
    }
  } break;

  default:
    break;
  }
}

static int same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b) {
  return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void handle_packet(const uint8_t *buf, int len, struct sockaddr_in *from) {
  if (len < 4 || buf[0] != 0xEF || buf[1] != 0xFE)
    return;

  switch (buf[2]) {
  case 0x02: { // discovery (63 bytes from real apps)
    if (len != 63 && len != 64)
      break;
    uint8_t reply[60];
    memset(reply, 0, sizeof(reply));
    reply[0] = 0xEF;
    reply[1] = 0xFE;
    pthread_mutex_lock(&dest_lock);
    int busy = hpsdr_is_connected() && !same_peer(from, &stream_dest);
    pthread_mutex_unlock(&dest_lock);
    reply[2] = busy ? 0x03 : 0x02;
    memcpy(reply + 3, mac, 6);
    reply[9] = FIRMWARE_VERSION;
    reply[10] = (uint8_t)cfg_board_id;
    sendto(sock, reply, sizeof(reply), 0, (struct sockaddr *)from, sizeof(*from));
    printf("hpsdr: discovery from %s\n", inet_ntoa(from->sin_addr));
  } break;

  case 0x04: { // start / stop
    if (len != 64)
      break;
    if (buf[3] & 0x01) {
      pthread_mutex_lock(&dest_lock);
      int fresh = !hpsdr_is_connected() || !same_peer(from, &stream_dest);
      stream_dest = *from; // EP6 goes back to the app's own port
      pthread_mutex_unlock(&dest_lock);
      atomic_store(&last_heard_ms, now_ms());
      if (fresh) {
        tx_seq = 0;
        app_rx_freq = app_tx_freq = 0;
        atomic_store(&reset_requested, 1);
        printf("hpsdr: streaming started to %s:%d\n", inet_ntoa(from->sin_addr),
               ntohs(from->sin_port));
      }
      atomic_store(&client_active, 1);
    } else {
      atomic_store(&client_active, 0);
      if (cfg_allow_mox && net_mox && is_in_tx())
        hamlib_tx(0);
      net_mox = 0;
      printf("hpsdr: streaming stopped\n");
    }
  } break;

  case 0x01: { // EP2: host commands + TX I/Q (exactly 1032 bytes)
    if (len != EP_PKT_SIZE || buf[3] != 0x02)
      break;
    atomic_store(&last_heard_ms, now_ms());
    process_ep2_frame(buf + 8);
    process_ep2_frame(buf + 8 + 512);
  } break;

  default:
    break;
  }
}

static void *poll_main(void *arg) {
  (void)arg;
  uint8_t buf[2048];
  while (atomic_load(&running)) {
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
    if (n > 0)
      handle_packet(buf, n, &from);
  }
  return NULL;
}

// --- lifecycle ---------------------------------------------------------------

int hpsdr_init(void) {
  if (!cfg_enable) {
    printf("hpsdr: off (hpsdr_enable=0)\n");
    return 0;
  }
  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    return -1;
  int one = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(HPSDR_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (struct sockaddr *)&a, sizeof(a)) < 0) {
    perror("hpsdr: bind UDP 1024");
    close(sock);
    sock = -1;
    return -1;
  }
  struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  atomic_store(&running, 1);
  pthread_create(&poll_thread, NULL, poll_main, NULL);
  pthread_create(&pacer_thread, NULL, pacer_main, NULL);
  printf("hpsdr: Protocol 1 server on UDP %d (MOX %s)\n", HPSDR_PORT,
         cfg_allow_mox ? "enabled" : "ignored");
  return 0;
}

void hpsdr_stop(void) {
  if (!atomic_load(&running))
    return;
  atomic_store(&client_active, 0);
  atomic_store(&running, 0);
  pthread_join(poll_thread, NULL);
  pthread_join(pacer_thread, NULL);
  close(sock);
  sock = -1;
}
