#define _POSIX_C_SOURCE 200809L
#include "cw_runtime.h"
#include <string.h>
#include <errno.h>
#ifndef CW_RUNTIME_TEST
#include <pthread.h>
#include <time.h>
#include <stdio.h>
extern int key_poll(int input_method);
#endif

/* One producer (GPIO), one consumer (audio). 32-bit atomics on ARM32. */
int cw_events_push(struct cw_event_queue *q, struct cw_event e) {
    unsigned w = atomic_load_explicit(&q->write, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&q->read, memory_order_acquire);
    if (w - r == CW_EVENTS) return 0;
    q->entries[w % CW_EVENTS] = e;
    atomic_store_explicit(&q->write, w + 1, memory_order_release);
    return 1;
}
int cw_events_peek(struct cw_event_queue *q, struct cw_event *e) {
    unsigned r = atomic_load_explicit(&q->read, memory_order_relaxed);
    if (r == atomic_load_explicit(&q->write, memory_order_acquire)) return 0;
    *e = q->entries[r % CW_EVENTS];
    return 1;
}
void cw_events_pop(struct cw_event_queue *q) {
    unsigned r = atomic_load_explicit(&q->read, memory_order_relaxed);
    atomic_store_explicit(&q->read, r + 1, memory_order_release);
}

static struct cw_event_queue events;
static struct cw_config config;
static atomic_uint epoch = 1, input_epoch, ready_epoch, queued_epoch;
static atomic_int enabled, physical, audio_busy, text_pending, manual_request;
static atomic_int overflow, stopping;
static atomic_uint overflows;
static atomic_uint playback_us = 43000;
#ifndef CW_RUNTIME_TEST
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t config_changed = PTHREAD_COND_INITIALIZER;
static pthread_t input_thread, control_thread;
static int started;
#define LOCK() pthread_mutex_lock(&config_lock)
#define UNLOCK() pthread_mutex_unlock(&config_lock)
static uint32_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000u + t.tv_nsec / 1000);
}
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif

unsigned cw_runtime_epoch(void) { return atomic_load(&epoch); }
int cw_runtime_valid(unsigned e) {
    return atomic_load(&enabled) && e == atomic_load(&epoch) && !atomic_load(&overflow);
}
void cw_runtime_cancel(void) {
    atomic_store(&ready_epoch, 0);
    atomic_store(&text_pending, 0);
    atomic_store(&manual_request, 0);
    atomic_store(&audio_busy, 0);
    atomic_fetch_add(&epoch, 1);
}
void cw_runtime_disable(void) {
    atomic_store(&enabled, 0);
    cw_runtime_cancel();
}
void cw_runtime_configure(const struct cw_config *c) {
    LOCK();
    int reset = config.enabled != c->enabled || config.radio_mode != c->radio_mode ||
        config.input_method != c->input_method || config.rx_hz != c->rx_hz ||
        config.tx_hz != c->tx_hz;
    config = *c;
    if (config.wpm < 5) config.wpm = 5;
    if (config.wpm > 80) config.wpm = 80;
    if (config.hang_ms < 0) config.hang_ms = 0;
    if (config.hang_ms > 5000) config.hang_ms = 5000;
    if (reset) cw_runtime_cancel();
    atomic_store(&enabled, c->enabled);
#ifndef CW_RUNTIME_TEST
    pthread_cond_broadcast(&config_changed);
#endif
    UNLOCK();
}
void cw_runtime_request_tx(void) { atomic_store(&manual_request, 1); }
void cw_runtime_text(int n) { atomic_store(&text_pending, n > 0); }
void cw_runtime_busy(int b) {
    if (!!b != atomic_load_explicit(&audio_busy, memory_order_relaxed))
        atomic_store(&audio_busy, !!b);
}
int cw_runtime_enabled(void) { return atomic_load(&enabled); }
void cw_runtime_pause(void) { atomic_store(&ready_epoch, 0); }
int cw_runtime_can_stop(unsigned e) {
    if (!cw_runtime_valid(e)) return 1;
    return !atomic_load(&physical) && !atomic_load(&audio_busy) &&
        !atomic_load(&text_pending) && !atomic_load(&manual_request) &&
        !(atomic_load(&queued_epoch) == e &&
          atomic_load(&events.write) != atomic_load(&events.read));
}
void cw_runtime_playback_frames(unsigned frames) {
    if (frames > 96000) frames = 96000;
    atomic_store(&playback_us, (frames * 125u + 11u) / 12u);
}
int cw_runtime_ready(void) {
    return cw_runtime_valid(atomic_load(&ready_epoch));
}

/* Input state belongs exclusively to the GPIO thread. Debounce requires 1 ms
 * of stable state and timestamps the beginning of that stable interval. */
static unsigned producer_epoch;
static int armed, stable, candidate;
static uint32_t candidate_since;
static void input_step(uint32_t us, int raw, unsigned e) {
    if (e != atomic_load(&epoch)) return;
    if (e != producer_epoch) {
        producer_epoch = e;
        armed = 0;
        stable = candidate = raw;
        candidate_since = us;
        atomic_store(&physical, 0);
        atomic_store(&input_epoch, e);
    }
    if (!atomic_load(&enabled) || atomic_load(&overflow)) return;
    if (raw != candidate) { candidate = raw; candidate_since = us; }
    if ((uint32_t)(us - candidate_since) < 1000u) return;
    if (!armed) {
        if (!raw) { armed = 1; stable = 0; }
        return;
    }
    if (stable == candidate) return;
    stable = candidate;
    atomic_store(&physical, stable);
    if (!cw_events_push(&events, (struct cw_event){candidate_since, e, stable})) {
        atomic_fetch_add(&overflows, 1);
        atomic_store(&overflow, 1); /* audio immediately mutes; controller releases PA */
    } else atomic_store(&queued_epoch, e);
}

/* Controller never holds config_lock while doing I/O or taking radio locks. */
static unsigned owner;
static uint32_t last_busy;
static void control_step(uint32_t us) {
    struct cw_config c;
    unsigned e;
    LOCK(); c = config; e = atomic_load(&epoch); UNLOCK();
    if (atomic_exchange(&overflow, 0)) {
        cw_runtime_cancel();
        e = atomic_load(&epoch);
    }
    if (owner && (!cw_runtime_valid(owner) || owner != e)) {
        atomic_store(&ready_epoch, 0);
        cw_radio_set(-1, &c, owner);
        owner = 0;
    }
    if (!cw_runtime_valid(e) || atomic_load(&input_epoch) != e) return;
    int queued = atomic_load(&queued_epoch) == e &&
        atomic_load(&events.write) != atomic_load(&events.read);
    int requested = atomic_exchange(&manual_request, 0);
    int demand = atomic_load(&physical) || queued || atomic_load(&text_pending) || requested;
    if (!owner && demand) {
        if (cw_radio_set(1, &c, e)) {
            owner = e;
            if (cw_runtime_valid(e)) atomic_store(&ready_epoch, e);
            last_busy = us;
        }
    }
    if (owner) {
        if (demand || atomic_load(&audio_busy)) last_busy = us;
        /* Include one audio period so the last envelope block is submitted. */
        if ((uint32_t)(us - last_busy) >= (uint32_t)c.hang_ms * 1000u +
                atomic_load(&playback_us) + 12000u) {
            /* Backend rechecks demand after excluding the DSP, so a new
             * element cannot be consumed between idle detection and PA-off. */
            if (cw_radio_set(0, &c, owner)) owner = 0;
        }
    }
}

/* Audio timeline, owned by sound_thread. Events keep their spacing even when
 * down AND up arrived during TX_PREPARE. Never read GPIO in the sample loop. */
static unsigned consumer_epoch;
static int config_available;
static int replaying, replay_state;
static uint32_t replay_us, sample_fraction;
int cw_runtime_begin(struct cw_config *c) {
    unsigned e;
#ifndef CW_RUNTIME_TEST
    /* A UI publication is short, but the realtime thread must never wait. */
    config_available = pthread_mutex_trylock(&config_lock) == 0;
    if (config_available) {
        *c = config;
        e = atomic_load(&epoch);
        pthread_mutex_unlock(&config_lock);
    } else return -1;
#else
    *c = config;
    config_available = 1;
    e = atomic_load(&epoch);
#endif
    if (e != atomic_load(&epoch)) { config_available = 0; return -1; }
    int reset = consumer_epoch != e;
    if (reset) {
        consumer_epoch = e;
        replay_state = replaying = 0;
        sample_fraction = 0;
        atomic_store(&audio_busy, 0);
    }
    /* Only the consumer advances read, including on cancellation. */
    struct cw_event ev;
    while (cw_events_peek(&events, &ev) && ev.epoch != e) cw_events_pop(&events);
    if (!cw_runtime_ready()) replaying = 0;
    return reset;
}
int cw_runtime_sample(void) {
    if (!config_available || consumer_epoch != atomic_load(&epoch) || !cw_runtime_ready()) return 0;
    struct cw_event ev;
    if (!replay_state && !atomic_load_explicit(&audio_busy, memory_order_relaxed) &&
        !cw_events_peek(&events, &ev)) replaying = 0;
    if (!replaying && cw_events_peek(&events, &ev)) {
        replay_us = ev.us;
        sample_fraction = 0;
        replaying = 1;
    }
    if (replaying) {
        while (cw_events_peek(&events, &ev)) {
            if (ev.epoch != consumer_epoch) { cw_events_pop(&events); continue; }
            if ((int32_t)(ev.us - replay_us) > 0) break;
            replay_state = ev.state;
            cw_events_pop(&events);
        }
        /* 1 sample = 125/12 us at 96 kHz, without per-sample floating point. */
        sample_fraction += 125;
        replay_us += sample_fraction / 12;
        sample_fraction %= 12;
    }
    return replay_state;
}

#ifndef CW_RUNTIME_TEST
static void wait_for_cw(void) {
    LOCK();
    while (!atomic_load(&enabled) && !atomic_load(&stopping))
        pthread_cond_wait(&config_changed, &config_lock);
    UNLOCK();
}
static void sleep_tick(struct timespec *deadline) {
    deadline->tv_nsec += 1000000;
    if (deadline->tv_nsec >= 1000000000) { deadline->tv_sec++; deadline->tv_nsec -= 1000000000; }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    /* Do not busy-loop to catch up after a scheduling stall. */
    if (now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec > deadline->tv_nsec))
        *deadline = now;
    int rc;
    do { rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL); }
    while (rc == EINTR && !atomic_load(&stopping));
}
static void *input_run(void *unused) {
    (void)unused;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    while (!atomic_load(&stopping)) {
        wait_for_cw();
        if (atomic_load(&stopping)) break;
        struct cw_config c;
        unsigned e;
        LOCK(); c = config; e = atomic_load(&epoch); UNLOCK();
        input_step(now_us(), key_poll(c.input_method), e);
        sleep_tick(&deadline);
    }
    return NULL;
}
static void *control_run(void *unused) {
    (void)unused;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    while (!atomic_load(&stopping)) {
        control_step(now_us());
        if (!owner) wait_for_cw(); /* a disabled owner must first release PA */
        sleep_tick(&deadline);
    }
    if (owner) cw_radio_set(-1, &config, owner);
    return NULL;
}
int cw_runtime_start(void) {
    if (started) return 0;
    if (!atomic_is_lock_free(&epoch) || !atomic_is_lock_free(&events.write)) return ENOTSUP;
    atomic_store(&stopping, 0);
    int rc = pthread_create(&input_thread, NULL, input_run, NULL);
    if (rc) return rc;
    rc = pthread_create(&control_thread, NULL, control_run, NULL);
    if (rc) {
        atomic_store(&stopping, 1);
        LOCK(); pthread_cond_broadcast(&config_changed); UNLOCK();
        pthread_join(input_thread, NULL);
        return rc;
    }
    started = 1;
    return 0;
}
void cw_runtime_stop(void) {
    if (!started) return;
    cw_runtime_disable();
    atomic_store(&stopping, 1);
    LOCK(); pthread_cond_broadcast(&config_changed); UNLOCK();
    pthread_join(input_thread, NULL);
    pthread_join(control_thread, NULL);
    started = 0;
    fprintf(stderr, "CW event queue overflows: %u\n", atomic_load(&overflows));
}
#else
int cw_runtime_start(void) { return 0; }
void cw_runtime_stop(void) { cw_runtime_disable(); }
void cw_test_input(uint32_t t, int s) { input_step(t, s, atomic_load(&epoch)); }
void cw_test_control(uint32_t t) { control_step(t); }
#endif
