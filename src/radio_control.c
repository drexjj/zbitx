#define _GNU_SOURCE
#include <pthread.h>
#include "radio_control.h"
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t control, dsp;
static void init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&control, &a);
    pthread_mutex_init(&dsp, &a);
    pthread_mutexattr_destroy(&a);
}
void radio_control_lock(void) { pthread_once(&once, init); pthread_mutex_lock(&control); }
void radio_control_unlock(void) { pthread_mutex_unlock(&control); }
void radio_dsp_lock(void) { pthread_once(&once, init); pthread_mutex_lock(&dsp); }
void radio_dsp_unlock(void) { pthread_mutex_unlock(&dsp); }
int radio_dsp_trylock(void) { pthread_once(&once, init); return pthread_mutex_trylock(&dsp) == 0; }
