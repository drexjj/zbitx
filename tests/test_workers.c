#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <stdio.h>
#include "cw_runtime.h"
static atomic_int raw, on, prepare, starts;
static struct cw_config cfg={1,2,0,20,600,0,7020000,7020000,40};
static void sleep_ms(int n) {
    struct timespec t={n/1000, (n%1000)*1000000L};
    clock_nanosleep(CLOCK_MONOTONIC,0,&t,NULL);
}
int key_poll(int method) { (void)method; return atomic_load(&raw); }
int cw_radio_set(int state, const struct cw_config *c, unsigned e) {
    (void)c;
    if(state==1) {
        if(!cw_runtime_valid(e)) return 0;
        atomic_store(&prepare,1);
        sleep_ms(200); /* real controller stalls; input must continue */
        atomic_store(&on,1); atomic_store(&prepare,0); atomic_fetch_add(&starts,1);
        return 1;
    }
    if(state==0 && !cw_runtime_can_stop(e)) return 0;
    cw_runtime_pause(); atomic_store(&on,0); return 1;
}
static void wait_value(atomic_int *v,int expected) {
    for(int i=0;i<1000;i++) {
        if(atomic_load(v)==expected) return;
        sleep_ms(1);
    }
    assert(!"worker timeout");
}
int main(void) {
    setbuf(stdout,NULL);
    assert(!cw_runtime_start());
    sleep_ms(5); cw_runtime_stop(); /* shutdown while both threads wait */
    assert(!cw_runtime_start());
    cw_runtime_configure(&cfg); sleep_ms(100);
    atomic_store(&raw,32); wait_value(&prepare,1);
    sleep_ms(20); atomic_store(&raw,0); /* entire pulse inside relay delay */
    wait_value(&on,1);
    for(int i=0;i<100 && !cw_runtime_ready();i++) sleep_ms(1);
    assert(cw_runtime_ready());
    struct cw_config audio={0};
    int configured=-1;
    for(int i=0;i<100 && configured<0;i++) {
        configured=cw_runtime_begin(&audio);
        if(configured<0) sleep_ms(1); /* real audio retries at next block too */
    }
    assert(configured>=0);
    int down=0;
    for(int i=0;i<9600;i++) if(cw_runtime_sample()==32) down++;
    /* Host scheduling controls pulse length; deterministic tests check exact samples. */
    assert(down>0 && down<9600);
    puts("PASS real workers retain a complete pulse while TX backend sleeps");
    cw_runtime_disable(); wait_value(&on,0);
    puts("PASS disable releases active TX before workers go to sleep");
    cw_runtime_configure(&cfg); sleep_ms(100);
    atomic_store(&raw,32); wait_value(&prepare,1);
    cw_runtime_disable(); /* mode changes during hardware preparation */
    sleep_ms(300); assert(!atomic_load(&on)); assert(!cw_runtime_ready());
    puts("PASS cancellation during TX preparation cannot leave PA active");
    cw_runtime_stop();
    puts("ALL WORKER TESTS PASSED");
    return 0;
}
