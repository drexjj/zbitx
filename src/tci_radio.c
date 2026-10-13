// tci_radio.c - tci_radio.h over zbitx's own controls
//
// Reads come straight from the fields (as the web server's do). Every
// change is handed to the UI thread - the one that owns the fields, the
// text sender and TX switching - through a small queue that ui_tick()
// drains via tci_radio_ui_tick(). For settings the TCI thread waits (up to
// UI_WAIT_MS) for it to run, so the echo tci.c sends afterwards carries the
// value actually in force. TX switching isn't waited for: tx_on() saves the
// settings and lets the relays settle first, which can take longer than
// TCI should hold its lock (and stall the streams) for.

#include "tci_radio.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// zbitx (sbitx_main.c / sbitx.c / sbitx_sound.c)
struct rx;
extern struct rx *rx_list;
const char *field_str(const char *label);
int field_set(const char *label, const char *new_value);
void cmd_exec(char *cmd);
void tx_on(int trigger);
void tx_off(void);
void sound_input(int loop);
int is_in_tx(void);
int calculate_s_meter(struct rx *r, double rx_gain);
int get_rx_gain(void);
void abort_tx(void);
int tx_text_append(const char *s);
int get_tx_data_length(void);
#define TX_SOFT 2

#define UI_WAIT_MS 250
#define UI_QUEUE 32
#define CW_QUEUE 2048
#define CW_FEED_MAX 24 // characters kept in the text box ahead of the sender

enum ui_op { OP_FREQ, OP_MODE, OP_TX_ON_TCI, OP_TX_ON_LOOP, OP_TX_OFF, OP_FIELD, OP_RIT, OP_TEXT, OP_CW_STOP };

struct ui_cmd {
  enum ui_op op;
  int n;
  char a[32];
  char b[96];
};

static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qdone = PTHREAD_COND_INITIALIZER;
static struct ui_cmd queue[UI_QUEUE];
static unsigned q_head, q_tail; // under qlock
static unsigned long long done_seq, next_seq;

// CW text not yet handed to the text box (TCI thread only, under tci.c's lock)
static char cw_buf[CW_QUEUE];
static int cw_len;
static long long cw_last_feed_ms;

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Queue a change for the UI thread and wait for it to run.
static void ui_run_wait(enum ui_op op, int n, const char *a, const char *b, int wait) {
  pthread_mutex_lock(&qlock);
  if (q_head - q_tail >= UI_QUEUE) {
    pthread_mutex_unlock(&qlock);
    fprintf(stderr, "tci: UI queue full, change dropped\n");
    return;
  }
  struct ui_cmd *c = &queue[q_head % UI_QUEUE];
  c->op = op;
  c->n = n;
  snprintf(c->a, sizeof(c->a), "%s", a ? a : "");
  snprintf(c->b, sizeof(c->b), "%s", b ? b : "");
  q_head++;
  unsigned long long mine = ++next_seq;
  if (!wait) {
    pthread_mutex_unlock(&qlock);
    return;
  }

  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_nsec += (long)UI_WAIT_MS * 1000000L;
  if (until.tv_nsec >= 1000000000L) {
    until.tv_nsec -= 1000000000L;
    until.tv_sec++;
  }
  while (done_seq < mine)
    if (pthread_cond_timedwait(&qdone, &qlock, &until) != 0)
      break; // the UI thread is busy; the change still happens when it gets there
  pthread_mutex_unlock(&qlock);
}

static void ui_run(enum ui_op op, int n, const char *a, const char *b) {
  ui_run_wait(op, n, a, b, 1);
}

// UI thread: called from ui_tick().
void tci_radio_ui_tick(void) {
  for (;;) {
    pthread_mutex_lock(&qlock);
    if (q_tail == q_head) {
      pthread_mutex_unlock(&qlock);
      return;
    }
    struct ui_cmd c = queue[q_tail % UI_QUEUE];
    pthread_mutex_unlock(&qlock);

    char cmd[160];
    switch (c.op) {
    case OP_FREQ:
      snprintf(cmd, sizeof(cmd), "freq %d", c.n);
      cmd_exec(cmd);
      break;
    case OP_MODE:
      snprintf(cmd, sizeof(cmd), "mode %s", c.a);
      cmd_exec(cmd);
      break;
    case OP_TX_ON_TCI: // audio from tci_audio.c, which overwrites the mic input
      sound_input(0);
      tx_on(TX_SOFT);
      break;
    case OP_TX_ON_LOOP: // as Hamlib PTT: audio from the ALSA loopback
      sound_input(1);
      tx_on(TX_SOFT);
      break;
    case OP_TX_OFF:
      sound_input(0);
      tx_off();
      break;
    case OP_FIELD:
      field_set(c.a, c.b);
      break;
    case OP_RIT: { // RIT applies when the frequency is next set
      field_set("RIT_DELTA", c.b);
      field_set("RIT", c.a);
      const char *f = field_str("FREQ");
      if (f) {
        char freq[24];
        snprintf(freq, sizeof(freq), "%s", f);
        field_set("FREQ", freq);
      }
    } break;
    case OP_TEXT:
      tx_text_append(c.b);
      break;
    case OP_CW_STOP:
      abort_tx();
      break;
    }

    pthread_mutex_lock(&qlock);
    q_tail++;
    done_seq++;
    pthread_cond_broadcast(&qdone);
    pthread_mutex_unlock(&qlock);
  }
}

static int field_int_or(const char *label, int dflt) {
  const char *v = field_str(label);
  return v ? atoi(v) : dflt;
}

/* ---- tci_radio.h ---------------------------------------------------------- */

int tr_freq(void) { return field_int_or("FREQ", 0); }

void tr_tune(int hz) { ui_run(OP_FREQ, hz, NULL, NULL); }

static const char *const mode_names[] = {"USB", "LSB", "CW", "CWR", "AM", "DIGI", "FT8"};

int tr_mode(void) {
  const char *m = field_str("MODE");
  for (int k = 0; m && k < (int)(sizeof(mode_names) / sizeof(mode_names[0])); k++)
    if (!strcmp(m, mode_names[k]))
      return k;
  return TR_OTHER;
}

void tr_set_mode(int mode) {
  if (mode >= TR_USB && mode <= TR_DIGU)
    ui_run(OP_MODE, 0, mode_names[mode], NULL);
}

int tr_in_tx(void) { return is_in_tx() != 0; }

// zbitx's tx_on() has no refusal, so neither does this: the request is
// queued and the radio follows within tens of milliseconds.
int tr_set_tx(int on, int tci_audio) {
  ui_run_wait(on ? (tci_audio ? OP_TX_ON_TCI : OP_TX_ON_LOOP) : OP_TX_OFF, 0, NULL, NULL, 0);
  return 0;
}

int tr_drive(void) { return field_int_or("DRIVE", 0); }

void tr_set_drive(int percent) {
  char v[16];
  snprintf(v, sizeof(v), "%d", percent);
  ui_run(OP_FIELD, 0, "DRIVE", v);
}

int tr_volume(void) { return field_int_or("AUDIO", 0); }

void tr_set_volume(int percent) {
  char v[16];
  snprintf(v, sizeof(v), "%d", percent);
  ui_run(OP_FIELD, 0, "AUDIO", v);
}

int tr_rit_on(void) {
  const char *v = field_str("RIT");
  return v && !strcmp(v, "ON");
}

int tr_rit(void) { return field_int_or("RIT_DELTA", 0); }

void tr_set_rit_on(int on) {
  char d[16];
  snprintf(d, sizeof(d), "%d", tr_rit());
  ui_run(OP_RIT, 0, on ? "ON" : "OFF", d);
}

void tr_set_rit(int hz) {
  char d[16];
  snprintf(d, sizeof(d), "%d", hz);
  ui_run(OP_RIT, 0, tr_rit_on() ? "ON" : "OFF", d);
}

int tr_wpm(void) { return field_int_or("WPM", 12); }

void tr_set_wpm(int wpm) {
  char v[16];
  snprintf(v, sizeof(v), "%d", wpm);
  ui_run(OP_FIELD, 0, "WPM", v);
}

double tr_smeter_dbm(void) {
  if (!rx_list)
    return -127.0;
  int s = calculate_s_meter(rx_list, (double)get_rx_gain()); // S-units * 100 + dB over
  return -127.0 + 6.0 * (s / 100) + (s % 100);
}

int tr_cw_send(const char *text) {
  int n = 0;
  for (; text[n] && cw_len < CW_QUEUE - 1; n++)
    cw_buf[cw_len++] = text[n];
  cw_buf[cw_len] = '\0';
  return n;
}

void tr_cw_stop(void) {
  cw_len = 0;
  cw_buf[0] = '\0';
  ui_run(OP_CW_STOP, 0, NULL, NULL);
}

int tr_cw_pending(void) { return cw_len + get_tx_data_length(); }

void tr_cw_service(void) {
  if (!cw_len || (tr_mode() != TR_CW && tr_mode() != TR_CWR))
    return;
  long long now = now_ms();
  if (now - cw_last_feed_ms < 100)
    return;
  cw_last_feed_ms = now;
  int room = CW_FEED_MAX - get_tx_data_length();
  if (room <= 0)
    return;
  if (room > cw_len)
    room = cw_len;
  if (room > 90)
    room = 90;
  char chunk[96];
  memcpy(chunk, cw_buf, (size_t)room);
  chunk[room] = '\0';
  memmove(cw_buf, cw_buf + room, (size_t)(cw_len - room + 1));
  cw_len -= room;
  ui_run(OP_TEXT, 0, NULL, chunk);
}
