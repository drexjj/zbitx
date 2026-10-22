// tci.c
//
// The TCI protocol over tci_ws.c: the initialization burst each client
// gets on connecting, the commands, and keeping every client in step.
// Ported from maxibitx (GPL-3.0); the radio is reached only through
// tci_radio.h.
//
// What clients need that the spec doesn't say (maxibitx
// tci_design_study.md §4): keywords are sent in lowercase, one command per
// WebSocket message; every set is echoed to every client with the value
// actually in force, even when nothing changed (JTDX waits for the echo, and
// compares it as a string); start; comes before ready; and the burst stays
// well under the 256 messages Hamlib reads looking for ready. Commands are
// read in either case, several to a message. zbitx identifies as protocol
// ExpertSDR3, which JTDX keys its TCI behaviour to, and as device zbitx.
//
// One receiver, one VFO as TCI sees it: vfo channel B and dds report channel
// A's frequency, if is always 0, and split, XIT, tune and the DSP switches
// report off. Changes made elsewhere (front panel, web, Hamlib) have no
// notification, so a service thread samples the radio every 50 ms and sends
// what changed; the same thread runs tci_stream.c.
//
// PTT: trx with source "tci" takes the transmit audio from that client's
// stream; any other trx transmits the loopback audio, as Hamlib PTT does.
// While one TCI client holds TX, the others' trx is ignored; TX keyed by
// something else (mic PTT, Hamlib, the panel) is left alone; and a client
// that disconnects while holding TX releases it.

#define _GNU_SOURCE
#include "tci.h"
#include "tci_radio.h"
#include "tci_stream.h"
#include "tci_ws.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define TCI_TICK_MS 5
#define TCI_STATE_TICKS 10 // state sampled every 50 ms
#define TCI_VFO_MIN 500000
#define TCI_VFO_MAX 32000000
#define TCI_MAX_ARGS 8
#define TCI_UNMUTE_DEFAULT_PERCENT 60

struct client {
  int in_use, id;
  struct tci_stream_cfg cfg;
  int buffering_ms;
  int rx_sensors, rx_sensors_ms;
  long long rx_sensors_due;
};

// State as last sent to the clients.
struct state {
  int freq, mode, tx, drive, volume_db, mute, rit_on, rit, wpm;
};

static struct client clients[WS_MAX_CLIENTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct state pub;
static int tx_owner = -1; // slot whose trx holds TX, -1 none
static int tx_owner_audio; // the owner keyed with trx:...,tci (its own audio)
// tr_set_tx() returns before the radio switches, and zbitx's UI thread can
// be busy for seconds (FFTW planning after a mode change). So ownership
// follows what the radio does, not a timer: tx_want is the state we asked
// for until the radio reaches it (while it's pending, TX state isn't
// published, so clients see no flicker), and the owner is released for TX
// dropped elsewhere only once its TX has actually been seen on.
static int tx_want = -1; // 0/1 asked for and not yet reached, -1 none
static long long tx_want_since_ms;
static int tx_seen_on;
#define TX_WANT_MAX_MS 15000 // stop waiting for the radio to follow after this
static int unmute_percent = TCI_UNMUTE_DEFAULT_PERCENT;
static int cw_was_busy;
static int trace; // ZBITX_TCI_TRACE: print every message
static atomic_int running;
static pthread_t service_thread;

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- Sending ------------------------------------------------------------ */

static void send_to(int slot, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void broadcast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void send_to(int slot, const char *fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (trace)
    printf("tci: -> %d %s\n", slot, msg);
  ws_send_text(clients[slot].id, msg);
}

static void broadcast(const char *fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (trace)
    printf("tci: -> all %s\n", msg);
  for (int s = 0; s < WS_MAX_CLIENTS; s++)
    if (clients[s].in_use)
      ws_send_text(clients[s].id, msg);
}

/* ---- The radio's state, in TCI's terms ------------------------------------ */

static const char *mode_name(int m) {
  switch (m) {
  case TR_LSB:
    return "lsb";
  case TR_CW:
    return "cw";
  case TR_CWR:
    return "cwr";
  case TR_AM:
    return "am";
  case TR_DIGU:
  case TR_FT8:
    return "digu";
  default:
    return "usb";
  }
}

static int mode_from_name(const char *s, int *m) {
  static const struct {
    const char *name;
    int mode;
  } names[] = {{"usb", TR_USB}, {"lsb", TR_LSB}, {"cw", TR_CW},
               {"cwr", TR_CWR}, {"am", TR_AM},   {"digu", TR_DIGU}};
  for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
    if (!strcasecmp(s, names[k].name)) {
      *m = names[k].mode;
      return 1;
    }
  }
  return 0;
}

// TCI volume is dB, -60..0 (-60 silent); the front panel's AUDIO is 0-100.
// 0.6 dB a step, so every integer dB round-trips.
static int volume_db(int percent) { return percent <= 0 ? -60 : (int)lround(-60.0 + 0.6 * percent); }
static int volume_percent(int db) {
  int p = (int)lround((db + 60) / 0.6);
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

static void snapshot(struct state *s) {
  s->freq = tr_freq();
  s->mode = tr_mode();
  s->tx = tr_in_tx();
  s->drive = tr_drive();
  int percent = tr_volume();
  s->volume_db = volume_db(percent);
  s->mute = percent == 0;
  s->rit_on = tr_rit_on();
  s->rit = tr_rit();
  s->wpm = tr_wpm();
}

static const char *tf(int b) { return b ? "true" : "false"; }

// Each sends one piece of state to every client and records it as sent.
// IF offset (TCI "if"): an SDR client such as SDR oxide keeps the I/Q
// centre (dds) still and moves its VFO within it, and places its transmit
// frequency the same way. zbitx has one oscillator, so the radio is tuned to
// the VFO (dds + if) - where it demodulates and transmits - and TCI's I/Q is
// shifted back (tci_stream_set_iq_shift()) so its centre stays on dds. A
// dial change made elsewhere (panel, web, Hamlib) puts the VFO back on the
// centre, if 0, which is how SDR oxide reads a rig's dial move.
#define TCI_IF_MAX 48000
static int if_hz;
static int pending_dial = -1; // a dial frequency we asked for that may not show yet

static void set_if(int hz) {
  if_hz = hz;
  tci_stream_set_iq_shift(hz);
}

static void tune_dial(int hz) {
  pending_dial = hz;
  tr_tune(hz);
}

static void pub_freq(const struct state *s) {
  broadcast("dds:0,%d;", s->freq - if_hz);
  broadcast("if:0,0,%d;", if_hz);
  broadcast("vfo:0,0,%d;", s->freq);
  broadcast("vfo:0,1,%d;", s->freq);
  pub.freq = s->freq;
}
static void pub_mode(const struct state *s) {
  broadcast("modulation:0,%s;", mode_name(s->mode));
  pub.mode = s->mode;
}
static void pub_tx(const struct state *s) {
  broadcast("trx:0,%s;", tf(s->tx));
  pub.tx = s->tx;
}
static void pub_drive(const struct state *s) {
  broadcast("drive:0,%d;", s->drive);
  pub.drive = s->drive;
}
static void pub_volume(const struct state *s) {
  broadcast("volume:%d;", s->volume_db);
  pub.volume_db = s->volume_db;
}
static void pub_mute(const struct state *s) {
  broadcast("mute:%s;", tf(s->mute));
  broadcast("rx_mute:0,%s;", tf(s->mute));
  pub.mute = s->mute;
}
static void pub_rit_on(const struct state *s) {
  broadcast("rit_enable:0,%s;", tf(s->rit_on));
  pub.rit_on = s->rit_on;
}
static void pub_rit(const struct state *s) {
  broadcast("rit_offset:0,%d;", s->rit);
  pub.rit = s->rit;
}
static void pub_wpm(const struct state *s) {
  broadcast("cw_macros_speed:%d;", s->wpm);
  broadcast("cw_keyer_speed:%d;", s->wpm);
  pub.wpm = s->wpm;
}

// Sends whatever changed since it was last sent (a change made elsewhere).
static void publish_changes(void) {
  struct state s;
  snapshot(&s);
  if (s.freq != pub.freq) {
    if (s.freq == pending_dial)
      pending_dial = -1; // ours, landing late
    else if (if_hz != 0)
      set_if(0); // the dial was moved elsewhere
    pub_freq(&s);
  }
  if (s.mode != pub.mode)
    pub_mode(&s);
  if (tx_want >= 0 && (s.tx == tx_want || now_ms() - tx_want_since_ms > TX_WANT_MAX_MS))
    tx_want = -1;
  if (s.tx != pub.tx && tx_want < 0)
    pub_tx(&s); // not while a switch we asked for is still under way
  if (s.drive != pub.drive)
    pub_drive(&s);
  if (s.volume_db != pub.volume_db)
    pub_volume(&s);
  if (s.mute != pub.mute)
    pub_mute(&s);
  if (s.rit_on != pub.rit_on)
    pub_rit_on(&s);
  if (s.rit != pub.rit)
    pub_rit(&s);
  if (s.wpm != pub.wpm)
    pub_wpm(&s);
  if (!s.mute)
    unmute_percent = tr_volume();
}

/* ---- PTT ------------------------------------------------------------------ */

static void tx_release_owner(void) {
  tx_owner = -1;
  tci_stream_tx_end();
}

static void tx_request(int on, int tci_audio) {
  tx_want = on;
  tx_want_since_ms = now_ms();
  tr_set_tx(on, tci_audio);
}

static void cmd_trx(int slot, int on, const char *source) {
  if (tx_owner >= 0 && tx_owner != slot) {
    printf("tci: trx from client %d ignored - client %d holds TX\n", slot, tx_owner);
  } else if (tx_owner < 0 && tr_in_tx() && tx_want != 0) {
    printf("tci: trx from client %d ignored - TX is held by the radio or another control\n", slot);
  } else if (on) {
    int own_audio = !strcasecmp(source, "tci");
    if (own_audio)
      tci_stream_tx_begin(slot, clients[slot].buffering_ms);
    tx_want = 1;
    tx_want_since_ms = now_ms();
    if (tr_set_tx(1, own_audio) < 0) {
      tx_want = -1;
      if (own_audio)
        tci_stream_tx_end();
      printf("tci: TX from client %d refused at %d Hz\n", slot, tr_freq());
    } else {
      if (!own_audio)
        tci_stream_tx_end(); // this client's earlier trx may have had its own audio
      tx_owner = slot;
      tx_owner_audio = own_audio;
      tx_seen_on = 0;
      printf("tci: client %d TX on (audio from %s)\n", slot,
             own_audio ? "its TCI stream" : "the loopback");
      // echo what is now in force; the radio itself switches a moment later
      struct state s = pub;
      s.tx = 1;
      pub_tx(&s);
      return;
    }
  } else if (tx_owner == slot) {
    tx_request(0, 0);
    printf("tci: client %d TX off\n", slot);
    tx_release_owner();
    struct state s = pub;
    s.tx = 0;
    pub_tx(&s);
    return;
  }
  if (!on)
    tci_stream_tx_end_slot(slot); // its TX stream outlived a drop (see service_main)
  struct state s;
  snapshot(&s);
  pub_tx(&s);
}

/* ---- CW text --------------------------------------------------------------- */

// TCI CW text marks word gaps with '_' in some clients and sends prosigns
// in angle brackets; zbitx's sender takes plain text.
static void cw_queue(const char *text) {
  char clean[512];
  size_t n = 0;
  for (const char *p = text; *p && n < sizeof(clean) - 1; p++) {
    char c = *p == '_' ? ' ' : *p;
    if (c == '<' || c == '>')
      continue;
    clean[n++] = (char)toupper((unsigned char)c);
  }
  clean[n] = '\0';
  tr_cw_send(clean);
  cw_was_busy = 1;
}

static int cw_mode(void) { return tr_mode() == TR_CW || tr_mode() == TR_CWR; }

static int cmd_cw(int slot, const char *name, const char *args) {
  if (!strcmp(name, "cw_macros") || !strcmp(name, "cw_msg")) {
    if (!args)
      return 1;
    // first field is the transceiver; the rest is text (cw_msg: before,
    // callsign, after - sent as one)
    const char *p = strchr(args, ',');
    if (!p)
      return 1;
    if (!cw_mode()) {
      printf("tci: CW text from client %d ignored - not in CW\n", slot);
      return 1;
    }
    char text[512];
    size_t n = 0;
    for (p++; *p && n < sizeof(text) - 1; p++)
      text[n++] = (!strcmp(name, "cw_msg") && *p == ',') ? ' ' : *p;
    text[n] = '\0';
    cw_queue(text);
    return 1;
  }
  if (!strcmp(name, "cw_macros_stop") || !strcmp(name, "cw_terminal")) {
    if (!strcmp(name, "cw_macros_stop"))
      tr_cw_stop();
    send_to(slot, "%s;", name);
    return 1;
  }
  return 0;
}

static void cw_service(void) {
  tr_cw_service();
  if (cw_was_busy && tr_cw_pending() == 0) {
    cw_was_busy = 0;
    broadcast("cw_macros_empty;");
  }
}

/* ---- Commands ------------------------------------------------------------- */

static int parse_bool(const char *s, int *v) {
  if (!strcasecmp(s, "true") || !strcmp(s, "1")) {
    *v = 1;
    return 1;
  }
  if (!strcasecmp(s, "false") || !strcmp(s, "0")) {
    *v = 0;
    return 1;
  }
  return 0;
}

static int parse_int(const char *s, long long *v) {
  char *end;
  if (!*s)
    return 0;
  *v = strtoll(s, &end, 10);
  return *end == '\0';
}

static void apply_stream(int slot) { tci_stream_set(slot, &clients[slot].cfg); }

// Replies with a fixed "off" state to a per-receiver switch zbitx doesn't
// offer over TCI.
static const char *const fixed_off[] = {
    "tune",          "sql_enable",    "rx_nb_enable",  "rx_nr_enable",  "rx_anf_enable",
    "rx_anc_enable", "rx_bin_enable", "rx_apf_enable", "rx_dse_enable", "rx_nf_enable",
    "lock",          "vfo_lock",      "xit_enable"};

static void handle(int slot, char *cmd) {
  // "name" or "name:arg,arg,..."; spaces around any part are ignored,
  // except in CW text, which gets its arguments as they came
  char *argv[TCI_MAX_ARGS];
  int argc = 0;
  char *colon = strchr(cmd, ':');
  if (colon)
    *colon = '\0';
  char *name = cmd;
  while (isspace((unsigned char)*name))
    name++;
  for (char *e = name + strlen(name); e > name && isspace((unsigned char)e[-1]);)
    *--e = '\0';
  for (char *p = name; *p; p++)
    *p = (char)tolower((unsigned char)*p);
  if (!*name)
    return;
  if (cmd_cw(slot, name, colon ? colon + 1 : NULL))
    return;
  if (colon) {
    char *save = NULL;
    for (char *a = strtok_r(colon + 1, ",", &save); a && argc < TCI_MAX_ARGS;
         a = strtok_r(NULL, ",", &save)) {
      while (isspace((unsigned char)*a))
        a++;
      char *e = a + strlen(a);
      while (e > a && isspace((unsigned char)e[-1]))
        *--e = '\0';
      argv[argc++] = a;
    }
  }

  struct client *c = &clients[slot];
  struct state s;
  long long v;
  int b;
  // Most commands name a receiver/transceiver first; only 0 exists.
  int trx0 = argc >= 1 && parse_int(argv[0], &v) && v == 0;

  if (!strcmp(name, "start")) {
    send_to(slot, "start;");
  } else if (argc == 0 && (!strcmp(name, "protocol") || !strcmp(name, "device") ||
                           !strcmp(name, "receive_only") || !strcmp(name, "trx_count") ||
                           !strcmp(name, "channel_count") || !strcmp(name, "vfo_limits") ||
                           !strcmp(name, "if_limits") || !strcmp(name, "modulations_list"))) {
    // The burst's identification, re-read (Hamlib's TCI backend asks)
    if (!strcmp(name, "protocol"))
      send_to(slot, "protocol:ExpertSDR3,2.0;");
    else if (!strcmp(name, "device"))
      send_to(slot, "device:zbitx;");
    else if (!strcmp(name, "receive_only"))
      send_to(slot, "receive_only:false;");
    else if (!strcmp(name, "trx_count"))
      send_to(slot, "trx_count:1;");
    else if (!strcmp(name, "channel_count"))
      send_to(slot, "channel_count:1;");
    else if (!strcmp(name, "vfo_limits"))
      send_to(slot, "vfo_limits:%d,%d;", TCI_VFO_MIN, TCI_VFO_MAX);
    else if (!strcmp(name, "if_limits"))
      send_to(slot, "if_limits:-48000,48000;");
    else
      send_to(slot, "modulations_list:AM,LSB,USB,CW,CWR,DIGU;");
  } else if (!strcmp(name, "stop")) {
    // zbitx has no stopped state; clients send this as they close
  } else if (!strcmp(name, "vfo") && trx0 && argc >= 2) {
    long long ch = 0;
    parse_int(argv[1], &ch);
    if (argc >= 3) {
      if (ch == 0 && parse_int(argv[2], &v) && v >= TCI_VFO_MIN && v <= TCI_VFO_MAX)
        tune_dial((int)v); // the VFO moves; the centre follows, if unchanged
      snapshot(&s);
      pub_freq(&s); // echo, with the frequency in force
    } else {
      send_to(slot, "vfo:0,%lld,%d;", ch, tr_freq());
    }
  } else if (!strcmp(name, "dds") && trx0) {
    if (argc >= 2) {
      if (parse_int(argv[1], &v) && v + if_hz >= TCI_VFO_MIN && v + if_hz <= TCI_VFO_MAX)
        tune_dial((int)v + if_hz);
      snapshot(&s);
      pub_freq(&s);
    } else {
      send_to(slot, "dds:0,%d;", tr_freq() - if_hz);
    }
  } else if (!strcmp(name, "if") && trx0 && argc >= 2) {
    long long ch = -1;
    parse_int(argv[1], &ch);
    if (argc >= 3) {
      int dial = pending_dial >= 0 ? pending_dial : tr_freq();
      int centre = dial - if_hz;
      if (ch == 0 && parse_int(argv[2], &v) && v >= -TCI_IF_MAX && v <= TCI_IF_MAX &&
          centre + v >= TCI_VFO_MIN && centre + v <= TCI_VFO_MAX) {
        set_if((int)v);
        tune_dial(centre + (int)v);
      }
      snapshot(&s);
      pub_freq(&s);
      if (ch != 0)
        broadcast("if:0,%lld,0;", ch);
    } else {
      send_to(slot, "if:0,%lld,%d;", ch, ch == 0 ? if_hz : 0);
    }
  } else if (!strcmp(name, "modulation") && trx0) {
    int m;
    if (argc >= 2 && mode_from_name(argv[1], &m))
      tr_set_mode(m);
    snapshot(&s);
    if (argc >= 2)
      pub_mode(&s);
    else
      send_to(slot, "modulation:0,%s;", mode_name(s.mode));
  } else if (!strcmp(name, "trx") && trx0) {
    if (argc >= 2 && parse_bool(argv[1], &b))
      cmd_trx(slot, b, argc >= 3 ? argv[2] : "");
    else
      send_to(slot, "trx:0,%s;", tf(tr_in_tx()));
  } else if (!strcmp(name, "drive") && trx0) {
    if (argc >= 2 && parse_int(argv[1], &v) && v >= 0 && v <= 100)
      tr_set_drive((int)v);
    snapshot(&s);
    if (argc >= 2)
      pub_drive(&s);
    else
      send_to(slot, "drive:0,%d;", s.drive);
  } else if (!strcmp(name, "tune_drive") && trx0) {
    snapshot(&s);
    send_to(slot, "tune_drive:0,%d;", s.drive);
  } else if (!strcmp(name, "rit_enable") && trx0) {
    if (argc >= 2 && parse_bool(argv[1], &b))
      tr_set_rit_on(b);
    snapshot(&s);
    if (argc >= 2)
      pub_rit_on(&s);
    else
      send_to(slot, "rit_enable:0,%s;", tf(s.rit_on));
  } else if (!strcmp(name, "rit_offset") && trx0) {
    if (argc >= 2 && parse_int(argv[1], &v) && v >= -TR_RIT_MAX && v <= TR_RIT_MAX)
      tr_set_rit((int)v); // TCI sets the offset and the switch separately
    snapshot(&s);
    if (argc >= 2) {
      pub_rit(&s);
      if (s.rit_on != pub.rit_on)
        pub_rit_on(&s);
    } else {
      send_to(slot, "rit_offset:0,%d;", s.rit);
    }
  } else if (!strcmp(name, "xit_offset") && trx0) {
    if (argc >= 2)
      broadcast("xit_offset:0,0;");
    else
      send_to(slot, "xit_offset:0,0;");
  } else if (!strcmp(name, "split_enable")) {
    // Always off. WSJT-X Improved sets it with no transceiver number.
    int bare_set = argc == 1 && (!strcasecmp(argv[0], "true") || !strcasecmp(argv[0], "false"));
    if (bare_set || (argc >= 2 && trx0))
      broadcast("split_enable:0,false;");
    else if (argc == 0 || trx0)
      send_to(slot, "split_enable:0,false;");
  } else if (!strcmp(name, "volume")) {
    if (argc >= 1 && parse_int(argv[0], &v) && v >= -60 && v <= 0) {
      int percent = volume_percent((int)v);
      tr_set_volume(percent);
      if (percent)
        unmute_percent = percent;
    }
    snapshot(&s);
    if (argc >= 1) {
      pub_volume(&s);
      if (s.mute != pub.mute)
        pub_mute(&s);
    } else {
      send_to(slot, "volume:%d;", s.volume_db);
    }
  } else if (!strcmp(name, "mute") || (!strcmp(name, "rx_mute") && trx0)) {
    int at = !strcmp(name, "mute") ? 0 : 1;
    if (argc > at && parse_bool(argv[at], &b)) {
      int percent = tr_volume();
      if (b && percent > 0) {
        unmute_percent = percent;
        tr_set_volume(0);
      } else if (!b && percent == 0) {
        tr_set_volume(unmute_percent);
      }
    }
    snapshot(&s);
    if (argc > at) {
      pub_mute(&s);
      if (s.volume_db != pub.volume_db)
        pub_volume(&s);
    } else if (at == 0) {
      send_to(slot, "mute:%s;", tf(s.mute));
    } else {
      send_to(slot, "rx_mute:0,%s;", tf(s.mute));
    }
  } else if (!strcmp(name, "cw_macros_speed") || !strcmp(name, "cw_keyer_speed")) {
    if (argc >= 1 && parse_int(argv[0], &v) && v >= TR_WPM_MIN && v <= TR_WPM_MAX)
      tr_set_wpm((int)v);
    snapshot(&s);
    if (argc >= 1)
      pub_wpm(&s);
    else
      send_to(slot, "%s:%d;", name, s.wpm);
  } else if (!strcmp(name, "cw_macros_speed_up") || !strcmp(name, "cw_macros_speed_down")) {
    if (argc >= 1 && parse_int(argv[0], &v) && v > 0 && v <= TR_WPM_MAX) {
      int w = tr_wpm() + (name[16] == 'u' ? (int)v : -(int)v);
      tr_set_wpm(w < TR_WPM_MIN ? TR_WPM_MIN : w > TR_WPM_MAX ? TR_WPM_MAX : w);
    }
    snapshot(&s);
    pub_wpm(&s);
  } else if (!strcmp(name, "rx_enable") && argc >= 1 && parse_int(argv[0], &v)) {
    send_to(slot, "rx_enable:%lld,%s;", v, tf(v == 0));
  } else if (!strcmp(name, "rx_channel_enable") && argc >= 2 && parse_int(argv[0], &v)) {
    long long ch = -1;
    parse_int(argv[1], &ch);
    send_to(slot, "rx_channel_enable:%lld,%lld,%s;", v, ch, tf(v == 0 && ch == 0));
  } else if (!strcmp(name, "rx_smeter") && trx0) {
    send_to(slot, "rx_smeter:0,0,%d;", (int)lround(tr_smeter_dbm()));
  } else if (!strcmp(name, "rx_sensors_enable") && argc >= 1 && parse_bool(argv[0], &b)) {
    c->rx_sensors = b;
    c->rx_sensors_ms = 200;
    if (argc >= 2 && parse_int(argv[1], &v))
      c->rx_sensors_ms = v < 30 ? 30 : v > 1000 ? 1000 : (int)v;
    c->rx_sensors_due = now_ms();
  } else if (!strcmp(name, "tx_sensors_enable")) {
    // accepted; nothing is sent yet
  } else if ((!strcmp(name, "audio_start") || !strcmp(name, "audio_stop")) && trx0) {
    c->cfg.audio_on = !strcmp(name, "audio_start");
    apply_stream(slot);
    send_to(slot, "%s:0;", name);
  } else if ((!strcmp(name, "iq_start") || !strcmp(name, "iq_stop")) && trx0) {
    c->cfg.iq_on = !strcmp(name, "iq_start");
    apply_stream(slot);
    send_to(slot, "%s:0;", name);
  } else if (!strcmp(name, "audio_samplerate")) {
    if (argc >= 1 && parse_int(argv[0], &v) &&
        (v == 8000 || v == 12000 || v == 24000 || v == 48000)) {
      c->cfg.audio_rate = (int)v;
      apply_stream(slot);
    }
    send_to(slot, "audio_samplerate:%d;", c->cfg.audio_rate);
  } else if (!strcmp(name, "iq_samplerate")) {
    if (argc >= 1 && parse_int(argv[0], &v) && (v == 48000 || v == 96000)) {
      c->cfg.iq_rate = (int)v;
      apply_stream(slot);
    }
    send_to(slot, "iq_samplerate:%d;", c->cfg.iq_rate);
  } else if (!strcmp(name, "audio_stream_sample_type")) {
    static const char *const types[] = {"int16", "int24", "int32", "float32"};
    for (int k = 0; argc >= 1 && k < 4; k++)
      if (!strcasecmp(argv[0], types[k]))
        c->cfg.audio_type = k;
    apply_stream(slot);
    send_to(slot, "audio_stream_sample_type:%s;", types[c->cfg.audio_type]);
  } else if (!strcmp(name, "audio_stream_channels")) {
    if (argc >= 1 && parse_int(argv[0], &v) && (v == 1 || v == 2)) {
      c->cfg.audio_channels = (int)v;
      apply_stream(slot);
    }
    send_to(slot, "audio_stream_channels:%d;", c->cfg.audio_channels);
  } else if (!strcmp(name, "audio_stream_samples")) {
    if (argc >= 1 && parse_int(argv[0], &v) && v >= 100 && v <= 2048) {
      c->cfg.audio_samples = (int)v;
      apply_stream(slot);
    }
    int shown = c->cfg.audio_samples;
    if (!shown)
      shown = c->cfg.audio_rate == 8000    ? 256
              : c->cfg.audio_rate == 12000 ? 512
              : c->cfg.audio_rate == 24000 ? 1024
                                           : 2048;
    send_to(slot, "audio_stream_samples:%d;", shown);
  } else if (!strcmp(name, "tx_stream_audio_buffering")) {
    if (argc >= 1 && parse_int(argv[0], &v) && v >= 50 && v <= 500)
      c->buffering_ms = (int)v;
    send_to(slot, "tx_stream_audio_buffering:%d;", c->buffering_ms);
  } else if (!strcmp(name, "agc_mode") && trx0) {
    send_to(slot, "agc_mode:0,normal;");
  } else if (!strcmp(name, "sql_level") && trx0) {
    send_to(slot, "sql_level:0,-140;");
  } else if ((!strcmp(name, "digl_offset") || !strcmp(name, "digu_offset"))) {
    send_to(slot, "%s:0;", name);
  } else {
    for (size_t k = 0; k < sizeof(fixed_off) / sizeof(fixed_off[0]); k++) {
      if (!strcmp(name, fixed_off[k]) && trx0) {
        if (argc >= 2)
          broadcast("%s:0,false;", name);
        else
          send_to(slot, "%s:0,false;", name);
        return;
      }
    }
    // Anything else - spots, commands for other receivers - is ignored, as
    // the spec says of a command a server doesn't know.
  }
}

/* ---- Connection events ------------------------------------------------------ */

static void send_init(int slot) {
  struct state s;
  snapshot(&s);
  send_to(slot, "protocol:ExpertSDR3,2.0;");
  send_to(slot, "device:zbitx;");
  send_to(slot, "receive_only:false;");
  send_to(slot, "trx_count:1;");
  send_to(slot, "channel_count:1;");
  send_to(slot, "vfo_limits:%d,%d;", TCI_VFO_MIN, TCI_VFO_MAX);
  send_to(slot, "if_limits:-48000,48000;");
  send_to(slot, "modulations_list:AM,LSB,USB,CW,CWR,DIGU;");
  send_to(slot, "iq_samplerate:%d;", clients[slot].cfg.iq_rate);
  send_to(slot, "audio_samplerate:%d;", clients[slot].cfg.audio_rate);
  send_to(slot, "dds:0,%d;", s.freq - if_hz);
  send_to(slot, "if:0,0,%d;", if_hz);
  send_to(slot, "vfo:0,0,%d;", s.freq);
  send_to(slot, "vfo:0,1,%d;", s.freq);
  send_to(slot, "modulation:0,%s;", mode_name(s.mode));
  send_to(slot, "rx_enable:0,true;");
  send_to(slot, "tx_enable:0,true;");
  send_to(slot, "trx:0,%s;", tf(s.tx));
  send_to(slot, "tune:0,false;");
  send_to(slot, "drive:0,%d;", s.drive);
  send_to(slot, "tune_drive:0,%d;", s.drive);
  send_to(slot, "split_enable:0,false;");
  send_to(slot, "rit_enable:0,%s;", tf(s.rit_on));
  send_to(slot, "rit_offset:0,%d;", s.rit);
  send_to(slot, "xit_enable:0,false;");
  send_to(slot, "xit_offset:0,0;");
  send_to(slot, "volume:%d;", s.volume_db);
  send_to(slot, "mute:%s;", tf(s.mute));
  send_to(slot, "rx_mute:0,%s;", tf(s.mute));
  send_to(slot, "cw_macros_speed:%d;", s.wpm);
  send_to(slot, "cw_keyer_speed:%d;", s.wpm);
  send_to(slot, "start;");
  send_to(slot, "ready;");
}

static void on_open(int id, const char *peer) {
  int slot = ws_client_slot(id);
  pthread_mutex_lock(&lock);
  struct client *c = &clients[slot];
  memset(c, 0, sizeof(*c));
  c->in_use = 1;
  c->id = id;
  c->cfg.audio_rate = 48000;
  c->cfg.audio_type = TCI_FLOAT32;
  c->cfg.audio_channels = 2;
  c->cfg.iq_rate = 48000; // lighter on the Zero 2W's Wi-Fi; clients can ask for 96000
  c->buffering_ms = 50;
  tci_stream_open(slot, id);
  apply_stream(slot);
  printf("tci: client %d connected from %s\n", slot, peer);
  send_init(slot);
  pthread_mutex_unlock(&lock);
}

static void on_text(int id, const char *msg, size_t len) {
  (void)len;
  int slot = ws_client_slot(id);
  pthread_mutex_lock(&lock);
  if (clients[slot].in_use && clients[slot].id == id) {
    if (trace)
      printf("tci: <- %d %s\n", slot, msg);
    char *copy = strdup(msg), *save = NULL;
    for (char *cmd = copy ? strtok_r(copy, ";", &save) : NULL; cmd;
         cmd = strtok_r(NULL, ";", &save))
      handle(slot, cmd);
    free(copy);
  }
  pthread_mutex_unlock(&lock);
}

static void on_binary(int id, const uint8_t *data, size_t len) {
  int slot = ws_client_slot(id);
  pthread_mutex_lock(&lock);
  if (clients[slot].in_use && clients[slot].id == id)
    tci_stream_tx_frame(slot, data, len);
  pthread_mutex_unlock(&lock);
}

static void on_close(int id) {
  int slot = ws_client_slot(id);
  pthread_mutex_lock(&lock);
  if (clients[slot].in_use && clients[slot].id == id) {
    if (tx_owner == slot) {
      tx_request(0, 0); // even if the radio hasn't finished switching on yet
      tx_release_owner();
      printf("tci: client %d disconnected while holding TX - TX released\n", slot);
    }
    tci_stream_close(slot);
    clients[slot].in_use = 0;
    printf("tci: client %d disconnected\n", slot);
  }
  pthread_mutex_unlock(&lock);
}

/* ---- Service thread and lifetime ------------------------------------------ */

static void rx_sensors(void) {
  long long now = now_ms();
  double dbm = 0.0;
  int computed = 0;
  for (int s = 0; s < WS_MAX_CLIENTS; s++) {
    struct client *c = &clients[s];
    if (!c->in_use || !c->rx_sensors || now < c->rx_sensors_due)
      continue;
    if (!computed) {
      dbm = tr_smeter_dbm(); // S9 = -73 dBm; uncalibrated
      computed = 1;
    }
    send_to(s, "rx_sensors:0,%.1f;", dbm);
    send_to(s, "rx_channel_sensors:0,0,%.1f;", dbm);
    c->rx_sensors_due = now + c->rx_sensors_ms;
  }
}

static void *service_main(void *arg) {
  (void)arg;
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  for (int tick = 0; atomic_load(&running); tick++) {
    next.tv_nsec += TCI_TICK_MS * 1000000L;
    if (next.tv_nsec >= 1000000000L) {
      next.tv_nsec -= 1000000000L;
      next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    pthread_mutex_lock(&lock);
    tci_stream_service();
    // trx holds TX in CW as on any rig, not just for the keyer's hang time
    // (which can be shorter than the state tick): ask again every tick
    if (tx_owner >= 0 && !tx_owner_audio && tx_want != 0 && cw_mode())
      tr_cw_hold();
    if (tick % TCI_STATE_TICKS == 0) {
      if (tx_owner >= 0) {
        int keyer_cw = !tx_owner_audio && (tr_mode() == TR_CW || tr_mode() == TR_CWR);
        if (tr_in_tx())
          tx_seen_on = 1;
        else if (tx_seen_on && tx_want < 0 && !keyer_cw) {
          // TX dropped elsewhere (panel, Hamlib, timeout). The client no
          // longer owns the radio, but it still believes it is keyed until
          // it sends trx false: its TX stream keeps being paced with
          // TX_CHRONO (its audio discarded, tci_audio.c), since clients
          // such as SDR oxide stall without them.
          printf("tci: client %d's TX dropped by the radio\n", tx_owner);
          tci_stream_tx_orphan();
          tx_owner = -1;
        }
      }
      publish_changes();
      rx_sensors();
      cw_service();
    }
    pthread_mutex_unlock(&lock);
  }
  return NULL;
}

int tci_init(const char *bind_addr, int port, int max_clients) {
  static const struct ws_callbacks cb = {on_open, on_text, on_binary, on_close};
  const char *t = getenv("ZBITX_TCI_TRACE");
  trace = t && *t && strcmp(t, "0");
  tci_stream_init();
  snapshot(&pub);
  if (pub.volume_db > -60)
    unmute_percent = tr_volume();
  if (ws_server_start(bind_addr, port, max_clients, &cb) < 0)
    return -1;
  atomic_store(&running, 1);
  if (pthread_create(&service_thread, NULL, service_main, NULL) != 0) {
    atomic_store(&running, 0);
    ws_server_stop();
    return -1;
  }
  return 0;
}

void tci_stop(void) {
  if (!atomic_load(&running))
    return;
  ws_server_stop();
  atomic_store(&running, 0);
  pthread_join(service_thread, NULL);
}
