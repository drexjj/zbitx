// tci_audio.h - TCI audio on zbitx's audio thread (see tci_audio.c)

#ifndef TCI_AUDIO_H
#define TCI_AUDIO_H

#include <stdint.h>

#define TCI_AUDIO_MAX_BLOCK 4096

// After rx_linear()/tx_process(): one block of 96 kHz speaker audio.
void tci_audio_rx(const int32_t *speaker, int n, int in_tx);

// Before tx_process(): while a TCI client transmits its own audio, fills
// mic with it and returns 1; otherwise leaves mic alone and returns 0.
int tci_audio_tx(int32_t *mic, int n, int in_tx);

#endif
