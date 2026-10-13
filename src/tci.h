// tci.h
//
// A TCI server (Expert Electronics' Transceiver Control Interface, v2.0):
// control, receive and transmit audio, and I/Q over one WebSocket
// connection per client - for JTDX, WSJT-X Improved, MSHV, loggers,
// sdrOxide and Hamlib's TCI backend. Several clients at once, kept in step:
// a change made by any of them, or anywhere else in zbitx (front panel,
// web, Hamlib), is sent to all.
//
// Ported from maxibitx (https://github.com/MikeJohnshoy/maxibitx, GPL-3.0).
// The protocol layer (tci.c), streams (tci_stream.c) and WebSocket server
// (tci_ws.c) are maxibitx's; tci_radio.c maps them onto zbitx's controls and
// tci_audio.c onto its audio thread. See src/docs/tci.md.

#ifndef TCI_H
#define TCI_H

#define TCI_DEFAULT_PORT 50001 // Hamlib's and ExpertSDR3's default
#define TCI_DEFAULT_MAX_CLIENTS 4

// Starts the server on bind_addr (dotted IPv4, or "" for every interface)
// and port. Returns 0, or -1 if it can't listen - not fatal to zbitx.
int tci_init(const char *bind_addr, int port, int max_clients);

// Disconnects every client (releasing a TX one of them holds) and stops.
void tci_stop(void);

// ---- Audio thread ---------------------------------------------------------
//
// None of these blocks, locks or allocates; each push returns at once when
// no client has started that stream.

// Demodulated receive audio at 48 kHz, scaled to about +-1 (tci_audio.c).
void tci_push_audio_rx(const double *samples, int n);

// The 96 kHz baseband I/Q from iq_baseband.c (not inverted).
void tci_push_iq(const double *i_samples, const double *q_samples, int n);

// Nonzero while some client has started that stream.
int tci_rx_audio_wanted(void);
int tci_iq_wanted(void);

// Nonzero while a TCI client holds TX with its own audio (trx with source
// "tci"): the transmit audio then comes from tci_pull_audio_tx() rather
// than the mic or the loopback.
int tci_tx_audio_owned(void);

// Up to n transmit samples at 48 kHz, about +-1 full scale; fewer (0 if
// none) rather than waiting.
int tci_pull_audio_tx(double *out, int n);

// Discards queued transmit audio. The audio thread calls it on every block
// that isn't sending TCI audio, so a transmission never starts with audio
// left from an earlier one.
void tci_tx_audio_idle(void);

#endif /* TCI_H */
