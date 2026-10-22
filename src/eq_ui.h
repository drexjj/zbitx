#ifndef EQ_UI_H_
#define EQ_UI_H_

#include "para_eq.h"

#define NUM_BANDS 5

int field_set(const char *label, const char *new_value);

// Declare the modify functions
void modify_eq_band_frequency(parametriceq *eq, int band_index, double new_frequency);
void modify_eq_band_gain(parametriceq *eq, int band_index, double new_gain);
void modify_eq_band_bandwidth(parametriceq *eq, int band_index, double new_bandwidth);

// Declare the instances for TX and RX EQ
extern parametriceq tx_eq;
extern parametriceq rx_eq;

#endif // EQ_UI_H_
