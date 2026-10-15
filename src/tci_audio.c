// tci_audio.c - TCI's receive and transmit audio on zbitx's audio thread
//
// Receive: the demodulated audio sound_process() sends to the speaker and
// the loopback (post-AGC; the AUDIO control is a codec mixer setting, so it
// doesn't touch these samples), decimated 96 -> 48 kHz with decim48k.c and
// scaled to about +-1 for tci_stream.c. Silence while transmitting.
//
// Transmit: while a TCI client holds TX with its own audio, its 48 kHz
// stream is upsampled to 96 kHz with upsample48k.c and written over the mic
// input block before tx_process() reads it - at the loopback's full-scale
// level, so a client's power setting means what it does through the
// loopback. Between the client keying and zbitx actually switching to TX
// (tx_on() takes a while), its audio waits in the queue - for up to
// TX_HOLD_MAX_SAMPLES. Past that (the radio never keyed, or dropped TX
// under the client) the audio is consumed and discarded at the real-time
// rate, so the client keeps getting TX_CHRONO requests: clients such as
// SDR oxide pace their transmit audio by them and stall without them.

#pragma GCC optimize("O2") // zbitx's default build is -O0; this runs per sample

#include "tci_audio.h"
#include "decim48k.h"
#include "tci.h"
#include "upsample48k.h"
#include <string.h>

// AGC holds rx_linear()'s output peaks near 1e8 (agc2()); this puts them
// about 15 dB below TCI full scale, leaving headroom for the AGC's attack.
#define RX_AUDIO_SCALE (0.178 / 100000000.0)

#define TX_FULL_SCALE 2147483647.0
#define TX_HOLD_MAX_SAMPLES (96000 * 3 / 4) // 750 ms of 96 kHz blocks

void tci_audio_rx(const int32_t *speaker, int n, int in_tx) {
  static struct decim48k_state dec;
  static double out[TCI_AUDIO_MAX_BLOCK / 2 + 1];
  if (!tci_rx_audio_wanted())
    return;
  if (n > TCI_AUDIO_MAX_BLOCK)
    n = TCI_AUDIO_MAX_BLOCK;
  int m = 0;
  for (int k = 0; k < n; k++) {
    double y;
    if (decim48k_apply(&dec, in_tx ? 0.0 : speaker[k] * RX_AUDIO_SCALE, &y))
      out[m++] = y;
  }
  tci_push_audio_rx(out, m);
}

int tci_audio_tx(int32_t *mic, int n, int in_tx) {
  static struct upsample48k_state up;
  static double buf48[TCI_AUDIO_MAX_BLOCK / 2];
  static long held; // 96 kHz samples spent waiting for the radio to key
  if (!tci_tx_audio_owned()) {
    tci_tx_audio_idle();
    memset(&up, 0, sizeof(up));
    held = 0;
    return 0;
  }
  if (n > TCI_AUDIO_MAX_BLOCK)
    n = TCI_AUDIO_MAX_BLOCK;
  int on_air = tci_tx_audio_on_air();
  if (!in_tx || !on_air) {
    if (on_air && held < TX_HOLD_MAX_SAMPLES) {
      held += n; // the radio is still switching: keep the audio queued
      return 0;
    }
    // Not transmitting this client (it never keyed, the radio dropped TX
    // under it, or someone else holds TX): consume at the real-time rate so
    // the client stays paced, and leave the mic input alone.
    tci_pull_audio_tx(buf48, n / 2);
    return 0;
  }
  held = TX_HOLD_MAX_SAMPLES; // once on the air, a later drop doesn't hold again
  int want = n / 2;
  int got = tci_pull_audio_tx(buf48, want);
  for (int k = got; k < want; k++)
    buf48[k] = 0.0; // underrun: the client is behind; send silence, not stale audio
  for (int k = 0; k < want; k++) {
    double pair[2];
    upsample48k_apply(&up, buf48[k], pair);
    for (int j = 0; j < 2; j++) {
      double v = pair[j];
      if (v > 1.0)
        v = 1.0;
      if (v < -1.0)
        v = -1.0;
      mic[2 * k + j] = (int32_t)(v * TX_FULL_SCALE);
    }
  }
  if (n & 1)
    mic[n - 1] = mic[n - 2];
  return 1;
}
