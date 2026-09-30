#ifndef CW_RUNTIME_H
#define CW_RUNTIME_H
#include <stdint.h>
#include <stdatomic.h>

/* All times are wrapping monotonic microseconds. Intervals must be < 2^31 us. */
#define CW_EVENTS 256u
struct cw_event { uint32_t us, epoch; int state; };
struct cw_event_queue {
    struct cw_event entries[CW_EVENTS];
    atomic_uint write, read;
};
struct cw_config {
    int enabled, radio_mode, input_method, wpm, pitch, hang_ms;
    int rx_hz, tx_hz, volume;
};
int cw_events_push(struct cw_event_queue *, struct cw_event);
int cw_events_peek(struct cw_event_queue *, struct cw_event *);
void cw_events_pop(struct cw_event_queue *);

void cw_runtime_configure(const struct cw_config *); /* UI only */
void cw_runtime_cancel(void); /* invalidate pending events; release to re-arm */
void cw_runtime_disable(void);
int cw_runtime_start(void);
void cw_runtime_stop(void);
void cw_runtime_request_tx(void);
unsigned cw_runtime_epoch(void);
int cw_runtime_valid(unsigned epoch);

/* Audio thread only. begin returns true when an epoch reset is needed. */
int cw_runtime_begin(struct cw_config *);
int cw_runtime_sample(void);
int cw_runtime_ready(void);
void cw_runtime_busy(int busy);
void cw_runtime_text(int available);
void cw_runtime_playback_frames(unsigned frames);
int cw_runtime_enabled(void);
int cw_runtime_can_stop(unsigned epoch);
void cw_runtime_pause(void); /* radio backend, with DSP excluded */

/* Radio backend: on=1, idle stop=0 (may defer), forced stop=-1.
 * Returns 1 if the requested transition completed, 0 if rejected/deferred. */
int cw_radio_set(int on, const struct cw_config *, unsigned epoch);
void cw_ui_tx_state(int on, int radio_mode); /* atomic notification only */
void cw_ui_config(struct cw_config *);      /* UI thread only */

#ifdef CW_RUNTIME_TEST
void cw_test_input(uint32_t us, int state);
void cw_test_control(uint32_t us);
#endif
#endif
