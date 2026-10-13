// hpsdr_p1.h - openHPSDR Protocol 1 server: RX I/Q out, tuning (and
// optionally PTT) in. Ported from maxibitx's interfaces/hpsdr_p1.c.

#ifndef HPSDR_P1_H
#define HPSDR_P1_H

#include <stdint.h>

#define HPSDR_PORT 1024

// Settings, from hw_settings.ini. Call before hpsdr_init().
//   enable     0 = server off
//   allow_mox  1 = the SDR app's MOX bit keys the radio (TX audio comes from
//              the loopback, as with Hamlib PTT; the app's TX I/Q is ignored)
//   gain_db    gain applied to the I/Q before it is packed into 24 bits
//   board_id   board type reported in discovery (1 = Hermes)
void hpsdr_configure(int enable, int allow_mox, double gain_db, int board_id);

int  hpsdr_init(void);   // opens UDP 1024 and starts the threads; <0 on failure
void hpsdr_stop(void);
int  hpsdr_is_connected(void);

// Audio thread: one block of 96 kHz baseband I/Q. Never blocks.
void hpsdr_send_iq(const double *i_samples, const double *q_samples, int n);

#endif
