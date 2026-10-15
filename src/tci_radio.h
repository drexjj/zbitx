// tci_radio.h - the radio as tci.c sees it
//
// tci.c (from maxibitx) talks to the radio only through these functions.
// tci_radio.c implements them over zbitx's fields and commands, the same
// ones the front panel, web UI and Hamlib use; tests/tci_radio_stub.c
// implements them over plain variables for the bench.
//
// All are called from TCI's own threads (never the audio thread), with
// tci.c's lock held.

#ifndef TCI_RADIO_H
#define TCI_RADIO_H

enum tr_mode { TR_USB, TR_LSB, TR_CW, TR_CWR, TR_AM, TR_DIGU, TR_FT8, TR_OTHER };

#define TR_RIT_MAX 9999   // Hz, either way
#define TR_WPM_MIN 1
#define TR_WPM_MAX 50

int tr_freq(void);            // dial frequency, Hz
void tr_tune(int hz);
int tr_mode(void);            // enum tr_mode
void tr_set_mode(int mode);   // TR_USB..TR_DIGU; others ignored

int tr_in_tx(void);
// Key or unkey. tci_audio: the transmit audio comes from the TCI stream
// (tci_audio.c) instead of the loopback that Hamlib PTT uses. Returns 0, or
// -1 if refused. May return before the radio has switched; tr_in_tx()
// follows shortly.
int tr_set_tx(int on, int tci_audio);

int tr_drive(void);           // 0-100
void tr_set_drive(int percent);
int tr_volume(void);          // 0-100 (front panel AUDIO)
void tr_set_volume(int percent);

int tr_rit_on(void);
int tr_rit(void);             // Hz
void tr_set_rit_on(int on);
void tr_set_rit(int hz);

int tr_wpm(void);
void tr_set_wpm(int wpm);

double tr_smeter_dbm(void);   // uncalibrated: S9 = -73 dBm

// CW text: queued and fed to the radio's text sender a little at a time
// by tr_cw_service(). Sent only in CW/CWR.
int tr_cw_send(const char *text);  // returns characters accepted
void tr_cw_stop(void);
int tr_cw_pending(void);           // characters not yet keyed
void tr_cw_service(void);          // every TCI service tick
void tr_cw_hold(void);             // keep CW TX keyed (trx held, no TCI audio)

#endif
