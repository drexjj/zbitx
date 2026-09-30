#ifdef NDEBUG
#undef NDEBUG
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <assert.h>
#include "modem_cw.h"
#define CW_IDLE 0
#define CW_DASH 1
#define CW_DOT 2
#define CW_DOT_DELAY 4
#define CW_DASH_DELAY 8
#define CW_WORD_DELAY 16
#define CW_DOWN 32
#define CW_SQUEEZE 64
#define CW_STRAIGHT 0
#define CW_IAMBIC 1
#define CW_IAMBICB 2
#define CW_KBD 3
#define CW_ULTIMATIC 5
#define CW_BUG 6
#define FONT_CW_TX 1
#define FONT_CW_RX 2
struct vfo { int freq_hz; };
static void vfo_start(struct vfo *v, int f, int phase) { (void)phase; v->freq_hz = f; }
/* Constant carrier exposes the actual envelope for timing assertions. */
static int vfo_read(struct vfo *v) { (void)v; return 1073741824; }
static int get_tx_data_byte(char *c);
static void write_console(int font, char *text) { (void)font; (void)text; }
