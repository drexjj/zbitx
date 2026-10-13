// Ported from maxibitx (https://github.com/MikeJohnshoy/maxibitx, GPL-3.0).
// tci_stream.c
//
// TCI's streams. Three lock-free single-producer, single-consumer rings sit
// between the audio thread and TCI; TCI's side of each runs only under
// tci.c's lock, so it is one side whichever TCI thread is running it:
//
//   receive audio  48 kHz float, audio thread -> service thread
//   I/Q            96 kHz complex float, audio thread -> service thread
//   transmit audio 48 kHz float, a client's reader thread -> audio thread
//
// For each client that has started a stream, the service thread converts
// what arrived to that client's rate, sample type, channel count and frame
// size, and queues the frames on its WebSocket. Receive audio at 24, 12 or
// 8 kHz, and I/Q at 48 kHz, go through windowed-sinc decimating lowpass
// filters designed at start-up.
//
// Transmit audio is pulled: the client that holds TX is sent TX_CHRONO
// frames asking for 1024 stereo frames each, enough to keep its
// tx_stream_audio_buffering requested ahead of what the audio thread has
// still to play, and answers each with a TX audio frame. What arrives is
// trusted by the header's length alone (JTDX leaves channels unset and pads
// the payload): stereo is assumed, and the left channel is used.
// maxibitx docs/dsp_design_notes/tci_design_study.md §4 and §7.

#pragma GCC optimize("O2") // zbitx's default build is -O0; this runs per sample on the service thread

#include "tci.h"
#include "tci_stream.h"
#include "tci_ws.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// zBitx: tci_audio.c hands over receive audio already scaled to +-1, so no
// further scaling here.
#define TCI_RX_AUDIO_SCALE 1.0

#define RX_RING 16384 // 341 ms at 48 kHz
#define IQ_RING 32768 // 341 ms at 96 kHz, complex
#define TX_RING 32768 // 683 ms at 48 kHz: the longest buffering plus a request
#define IQ_FRAME_SAMPLES 2048 // complex samples per I/Q frame at 96 kHz (21 ms)
#define TX_CHRONO_TIMEOUT_NS 300000000LL // requests unanswered this long are forgotten
#define TX_CHRONO_MAX_PER_SERVICE 8

#define AUDIO_TAPS_MAX 191
#define IQ_TAPS 95

static float rx_ring[RX_RING];
static float iq_ring[2 * IQ_RING];
static float tx_ring[TX_RING];
static atomic_uint rx_head, rx_tail, iq_head, iq_tail, tx_head, tx_tail;
static atomic_int want_rx, want_iq, tx_owned;

/* ---- Audio thread ------------------------------------------------------ */

void tci_push_audio_rx(const double *samples, int n) {
  if (!atomic_load_explicit(&want_rx, memory_order_relaxed))
    return;
  unsigned head = atomic_load_explicit(&rx_head, memory_order_relaxed);
  unsigned tail = atomic_load_explicit(&rx_tail, memory_order_acquire);
  unsigned room = RX_RING - (head - tail);
  if ((unsigned)n > room)
    n = (int)room; // the service thread has stalled; drop the newest
  for (int k = 0; k < n; k++)
    rx_ring[(head + (unsigned)k) & (RX_RING - 1)] = (float)(samples[k] * TCI_RX_AUDIO_SCALE);
  atomic_store_explicit(&rx_head, head + (unsigned)n, memory_order_release);
}

void tci_push_iq(const double *i_samples, const double *q_samples, int n) {
  if (!atomic_load_explicit(&want_iq, memory_order_relaxed))
    return;
  unsigned head = atomic_load_explicit(&iq_head, memory_order_relaxed);
  unsigned tail = atomic_load_explicit(&iq_tail, memory_order_acquire);
  unsigned room = IQ_RING - (head - tail);
  if ((unsigned)n > room)
    n = (int)room;
  for (int k = 0; k < n; k++) {
    unsigned at = (head + (unsigned)k) & (IQ_RING - 1);
    iq_ring[2 * at] = (float)i_samples[k];
    iq_ring[2 * at + 1] = (float)q_samples[k]; // iq_baseband.c's I/Q is already the right way up
  }
  atomic_store_explicit(&iq_head, head + (unsigned)n, memory_order_release);
}

int tci_rx_audio_wanted(void) { return atomic_load_explicit(&want_rx, memory_order_relaxed); }
int tci_iq_wanted(void) { return atomic_load_explicit(&want_iq, memory_order_relaxed); }

int tci_tx_audio_owned(void) { return atomic_load_explicit(&tx_owned, memory_order_relaxed); }

int tci_pull_audio_tx(double *out, int n) {
  unsigned tail = atomic_load_explicit(&tx_tail, memory_order_relaxed);
  unsigned head = atomic_load_explicit(&tx_head, memory_order_acquire);
  unsigned avail = head - tail;
  if ((unsigned)n > avail)
    n = (int)avail;
  for (int k = 0; k < n; k++)
    out[k] = tx_ring[(tail + (unsigned)k) & (TX_RING - 1)];
  atomic_store_explicit(&tx_tail, tail + (unsigned)n, memory_order_release);
  return n;
}

void tci_tx_audio_idle(void) {
  unsigned head = atomic_load_explicit(&tx_head, memory_order_acquire);
  atomic_store_explicit(&tx_tail, head, memory_order_release);
}

/* ---- Filters ------------------------------------------------------------ */

// Blackman-windowed sinc lowpass, unity gain at DC; cutoff in cycles per
// input sample.
static void design_lowpass(float *taps, int n, double cutoff) {
  double sum = 0.0, h[AUDIO_TAPS_MAX];
  int m = n - 1;
  for (int k = 0; k < n; k++) {
    double x = k - m / 2.0;
    double s = x == 0.0 ? 2.0 * cutoff : sin(2.0 * M_PI * cutoff * x) / (M_PI * x);
    double w = 0.42 - 0.5 * cos(2.0 * M_PI * k / m) + 0.08 * cos(4.0 * M_PI * k / m);
    h[k] = s * w;
    sum += h[k];
  }
  for (int k = 0; k < n; k++)
    taps[k] = (float)(h[k] / sum);
}

// Receive audio: 48 kHz to 24, 12 and 8 kHz, cutoff at 0.45 of the output
// rate. I/Q: 96 to 48 kHz, likewise (+-21.6 kHz).
static float taps_d2[63], taps_d4[127], taps_d6[AUDIO_TAPS_MAX], taps_iq[IQ_TAPS];

struct decimator {
  const float *taps;
  int ntaps, factor, phase, pos;
  float hist[2 * AUDIO_TAPS_MAX]; // each sample stored twice, so a window is contiguous
};

static void decimator_setup(struct decimator *d, int factor) {
  memset(d, 0, sizeof(*d));
  d->factor = factor;
  d->taps = factor == 2 ? taps_d2 : factor == 4 ? taps_d4 : taps_d6;
  d->ntaps = factor == 2 ? 63 : factor == 4 ? 127 : AUDIO_TAPS_MAX;
}

// Returns 1 and sets *y on every factor'th input.
static int decimator_step(struct decimator *d, float x, float *y) {
  d->hist[d->pos] = d->hist[d->pos + d->ntaps] = x;
  if (++d->pos == d->ntaps)
    d->pos = 0;
  if (++d->phase < d->factor)
    return 0;
  d->phase = 0;
  const float *w = d->hist + d->pos; // oldest first; the taps are symmetric
  float acc = 0.0f;
  for (int k = 0; k < d->ntaps; k++)
    acc += d->taps[k] * w[k];
  *y = acc;
  return 1;
}

/* ---- Per-client state ---------------------------------------------------- */

struct client_stream {
  int open, ws_id;
  struct tci_stream_cfg cfg;
  // receive audio
  struct decimator rx_dec;
  int rx_frames; // frames per packet
  float rx_pend[2048];
  int rx_npend;
  // I/Q at 48 kHz
  float iq_hist_i[2 * IQ_TAPS], iq_hist_q[2 * IQ_TAPS];
  int iq_pos, iq_phase;
  int iq_frames;
  float iq_pend[2 * IQ_FRAME_SAMPLES];
  int iq_npend;
};

static struct client_stream cs[WS_MAX_CLIENTS];

static struct {
  int active, slot;
  int buffering_ms;
  long outstanding; // frames asked for and not yet received
  long long last_ns;
} tx;

static long long now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int default_samples(int rate) {
  return rate == 8000 ? 256 : rate == 12000 ? 512 : rate == 24000 ? 1024 : 2048;
}

static void refresh_wants(void) {
  int rx = 0, iq = 0;
  for (int s = 0; s < WS_MAX_CLIENTS; s++) {
    rx |= cs[s].open && cs[s].cfg.audio_on;
    iq |= cs[s].open && cs[s].cfg.iq_on;
  }
  // The consuming side runs only under tci.c's lock (the service thread,
  // or here), so on the way in it can drop what is left from the last time
  // the stream ran.
  if (rx && !atomic_load(&want_rx))
    atomic_store(&rx_tail, atomic_load(&rx_head));
  if (iq && !atomic_load(&want_iq))
    atomic_store(&iq_tail, atomic_load(&iq_head));
  atomic_store(&want_rx, rx);
  atomic_store(&want_iq, iq);
}

void tci_stream_init(void) {
  design_lowpass(taps_d2, 63, 0.45 * 24000.0 / 48000.0);
  design_lowpass(taps_d4, 127, 0.45 * 12000.0 / 48000.0);
  design_lowpass(taps_d6, AUDIO_TAPS_MAX, 0.45 * 8000.0 / 48000.0);
  design_lowpass(taps_iq, IQ_TAPS, 0.45 * 48000.0 / 96000.0);
  memset(cs, 0, sizeof(cs));
  memset(&tx, 0, sizeof(tx));
  atomic_store(&want_rx, 0);
  atomic_store(&want_iq, 0);
  atomic_store(&tx_owned, 0);
}

void tci_stream_open(int slot, int ws_id) {
  memset(&cs[slot], 0, sizeof(cs[slot]));
  cs[slot].open = 1;
  cs[slot].ws_id = ws_id;
}

void tci_stream_close(int slot) {
  if (tx.active && tx.slot == slot)
    tci_stream_tx_end();
  cs[slot].open = 0;
  cs[slot].cfg.audio_on = cs[slot].cfg.iq_on = 0;
  refresh_wants();
}

void tci_stream_set(int slot, const struct tci_stream_cfg *cfg) {
  struct client_stream *c = &cs[slot];
  int restart_rx = cfg->audio_rate != c->cfg.audio_rate || cfg->audio_on != c->cfg.audio_on ||
                   cfg->audio_channels != c->cfg.audio_channels ||
                   cfg->audio_samples != c->cfg.audio_samples || cfg->audio_type != c->cfg.audio_type;
  int restart_iq = cfg->iq_rate != c->cfg.iq_rate || cfg->iq_on != c->cfg.iq_on;
  c->cfg = *cfg;
  if (restart_rx) {
    int factor = 48000 / cfg->audio_rate;
    decimator_setup(&c->rx_dec, factor < 2 ? 2 : factor);
    c->rx_dec.factor = factor; // 1 at 48 kHz: the filter is then unused
    int samples = cfg->audio_samples ? cfg->audio_samples : default_samples(cfg->audio_rate);
    c->rx_frames = samples / cfg->audio_channels;
    c->rx_npend = 0;
  }
  if (restart_iq) {
    memset(c->iq_hist_i, 0, sizeof(c->iq_hist_i));
    memset(c->iq_hist_q, 0, sizeof(c->iq_hist_q));
    c->iq_pos = c->iq_phase = 0;
    c->iq_frames = cfg->iq_rate == 96000 ? IQ_FRAME_SAMPLES : IQ_FRAME_SAMPLES / 2;
    c->iq_npend = 0;
  }
  refresh_wants();
}

/* ---- Frames out --------------------------------------------------------- */

static uint8_t frame_buf[TCI_HEADER_BYTES + 4 * 2 * IQ_FRAME_SAMPLES];

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void put_header(uint32_t rate, uint32_t format, uint32_t length, uint32_t type,
                       uint32_t channels) {
  memset(frame_buf, 0, TCI_HEADER_BYTES);
  put_u32(frame_buf + 0, 0); // receiver
  put_u32(frame_buf + 4, rate);
  put_u32(frame_buf + 8, format);
  put_u32(frame_buf + 20, length);
  put_u32(frame_buf + 24, type);
  put_u32(frame_buf + 28, channels);
}

static int sample_bytes(int type) {
  return type == TCI_INT16 ? 2 : type == TCI_INT24 ? 3 : 4;
}

static double clamp1(double x) { return x > 1.0 ? 1.0 : x < -1.0 ? -1.0 : x; }

static uint8_t *put_sample(uint8_t *p, int type, float x) {
  switch (type) {
  case TCI_INT16: {
    int16_t v = (int16_t)lrint(clamp1(x) * 32767.0);
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)((uint16_t)v >> 8);
    return p + 2;
  }
  case TCI_INT24: {
    int32_t v = (int32_t)lrint(clamp1(x) * 8388607.0);
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    return p + 3;
  }
  case TCI_INT32: {
    int32_t v = (int32_t)llrint(clamp1(x) * 2147483647.0);
    put_u32(p, (uint32_t)v);
    return p + 4;
  }
  default: {
    uint32_t v;
    memcpy(&v, &x, 4);
    put_u32(p, v);
    return p + 4;
  }
  }
}

static void rx_audio_out(struct client_stream *c, float y) {
  c->rx_pend[c->rx_npend++] = y;
  if (c->rx_npend < c->rx_frames)
    return;
  int ch = c->cfg.audio_channels;
  put_header((uint32_t)c->cfg.audio_rate, (uint32_t)c->cfg.audio_type,
             (uint32_t)(c->rx_frames * ch), TCI_RX_AUDIO_STREAM, (uint32_t)ch);
  uint8_t *p = frame_buf + TCI_HEADER_BYTES;
  for (int k = 0; k < c->rx_frames; k++)
    for (int j = 0; j < ch; j++)
      p = put_sample(p, c->cfg.audio_type, c->rx_pend[k]);
  ws_send_binary(c->ws_id, frame_buf, (size_t)(p - frame_buf));
  c->rx_npend = 0;
}

static void iq_out(struct client_stream *c, float i, float q) {
  c->iq_pend[2 * c->iq_npend] = i;
  c->iq_pend[2 * c->iq_npend + 1] = q;
  if (++c->iq_npend < c->iq_frames)
    return;
  put_header((uint32_t)c->cfg.iq_rate, TCI_FLOAT32, (uint32_t)(2 * c->iq_frames), TCI_IQ_STREAM, 2);
  uint8_t *p = frame_buf + TCI_HEADER_BYTES;
  for (int k = 0; k < 2 * c->iq_frames; k++)
    p = put_sample(p, TCI_FLOAT32, c->iq_pend[k]);
  ws_send_binary(c->ws_id, frame_buf, (size_t)(p - frame_buf));
  c->iq_npend = 0;
}

static void iq_step(struct client_stream *c, float i, float q) {
  if (c->cfg.iq_rate == 96000) {
    iq_out(c, i, q);
    return;
  }
  c->iq_hist_i[c->iq_pos] = c->iq_hist_i[c->iq_pos + IQ_TAPS] = i;
  c->iq_hist_q[c->iq_pos] = c->iq_hist_q[c->iq_pos + IQ_TAPS] = q;
  if (++c->iq_pos == IQ_TAPS)
    c->iq_pos = 0;
  if (++c->iq_phase < 2)
    return;
  c->iq_phase = 0;
  const float *wi = c->iq_hist_i + c->iq_pos, *wq = c->iq_hist_q + c->iq_pos;
  float ai = 0.0f, aq = 0.0f;
  for (int k = 0; k < IQ_TAPS; k++) {
    ai += taps_iq[k] * wi[k];
    aq += taps_iq[k] * wq[k];
  }
  iq_out(c, ai, aq);
}

static void send_chrono(int ws_id) {
  put_header(48000, TCI_FLOAT32, 2 * TCI_CHRONO_FRAMES, TCI_TX_CHRONO, 2);
  ws_send_binary(ws_id, frame_buf, TCI_HEADER_BYTES);
}

void tci_stream_service(void) {
  // Receive audio
  unsigned head = atomic_load_explicit(&rx_head, memory_order_acquire);
  unsigned tail = atomic_load_explicit(&rx_tail, memory_order_relaxed);
  for (; tail != head; tail++) {
    float x = rx_ring[tail & (RX_RING - 1)];
    for (int s = 0; s < WS_MAX_CLIENTS; s++) {
      struct client_stream *c = &cs[s];
      if (!c->open || !c->cfg.audio_on)
        continue;
      float y;
      if (c->rx_dec.factor == 1)
        rx_audio_out(c, x);
      else if (decimator_step(&c->rx_dec, x, &y))
        rx_audio_out(c, y);
    }
  }
  atomic_store_explicit(&rx_tail, tail, memory_order_release);

  // I/Q
  head = atomic_load_explicit(&iq_head, memory_order_acquire);
  tail = atomic_load_explicit(&iq_tail, memory_order_relaxed);
  for (; tail != head; tail++) {
    unsigned at = tail & (IQ_RING - 1);
    for (int s = 0; s < WS_MAX_CLIENTS; s++)
      if (cs[s].open && cs[s].cfg.iq_on)
        iq_step(&cs[s], iq_ring[2 * at], iq_ring[2 * at + 1]);
  }
  atomic_store_explicit(&iq_tail, tail, memory_order_release);

  // TX_CHRONO: keep buffering_ms plus one request's worth asked for ahead of
  // what is queued for the audio thread.
  if (tx.active) {
    long long now = now_ns();
    if (tx.outstanding > 0 && now - tx.last_ns > TX_CHRONO_TIMEOUT_NS) {
      tx.outstanding = 0; // the client let them go unanswered, as it may
      tx.last_ns = now;
    }
    long queued = (long)(atomic_load(&tx_head) - atomic_load(&tx_tail));
    long target = (long)tx.buffering_ms * 48 + TCI_CHRONO_FRAMES;
    for (int k = 0; k < TX_CHRONO_MAX_PER_SERVICE && queued + tx.outstanding < target; k++) {
      if (tx.outstanding == 0)
        tx.last_ns = now;
      send_chrono(cs[tx.slot].ws_id);
      tx.outstanding += TCI_CHRONO_FRAMES;
    }
  }
}

/* ---- Transmit audio in -------------------------------------------------- */

void tci_stream_tx_begin(int slot, int buffering_ms) {
  tx.active = 1;
  tx.slot = slot;
  tx.buffering_ms = buffering_ms;
  tx.outstanding = 0;
  tx.last_ns = now_ns();
  atomic_store(&tx_owned, 1);
}

void tci_stream_tx_end(void) {
  tx.active = 0;
  atomic_store(&tx_owned, 0);
}

static uint32_t get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static float get_sample(const uint8_t *p, int type) {
  switch (type) {
  case TCI_INT16:
    return (float)((int16_t)(p[0] | p[1] << 8) / 32768.0);
  case TCI_INT24: {
    int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
    return (float)(v / 8388608.0);
  }
  case TCI_INT32:
    return (float)((int32_t)get_u32(p) / 2147483648.0);
  default: {
    uint32_t v = get_u32(p);
    float f;
    memcpy(&f, &v, 4);
    return isfinite(f) ? f : 0.0f;
  }
  }
}

void tci_stream_tx_frame(int slot, const uint8_t *frame, size_t len) {
  if (len < TCI_HEADER_BYTES || get_u32(frame + 24) != TCI_TX_AUDIO_STREAM)
    return;
  if (!tx.active || tx.slot != slot)
    return;
  uint32_t rate = get_u32(frame + 4);
  if (rate != 48000 && rate != 0) {
    static int warned = 0;
    if (!warned)
      fprintf(stderr, "tci: TX audio at %u Hz ignored - TX_CHRONO asks for 48000\n", rate);
    warned = 1;
    return;
  }
  int type = (int)get_u32(frame + 8);
  if (type < TCI_INT16 || type > TCI_FLOAT32)
    type = TCI_FLOAT32;
  int bps = sample_bytes(type);
  size_t length = get_u32(frame + 20);
  size_t have = (len - TCI_HEADER_BYTES) / (size_t)bps;
  if (length > have)
    length = have;
  size_t frames = length / 2;

  unsigned head = atomic_load_explicit(&tx_head, memory_order_relaxed);
  unsigned tail = atomic_load_explicit(&tx_tail, memory_order_acquire);
  size_t room = TX_RING - (head - tail);
  const uint8_t *p = frame + TCI_HEADER_BYTES;
  for (size_t k = 0; k < frames && k < room; k++)
    tx_ring[(head + (unsigned)k) & (TX_RING - 1)] = get_sample(p + 2 * k * (size_t)bps, type);
  atomic_store_explicit(&tx_head, head + (unsigned)(frames < room ? frames : room),
                        memory_order_release);

  tx.outstanding -= (long)frames;
  if (tx.outstanding < 0)
    tx.outstanding = 0;
  tx.last_ns = now_ns();
}
