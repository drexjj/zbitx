#ifndef RADIO_CONTROL_H
#define RADIO_CONTROL_H
void radio_control_lock(void);
void radio_control_unlock(void);
void radio_dsp_lock(void);
void radio_dsp_unlock(void);
int radio_dsp_trylock(void);
#endif
