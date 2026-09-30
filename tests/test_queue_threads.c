#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "cw_runtime.h"
#ifdef _WIN32
#include <windows.h>
#define THREAD_RESULT DWORD WINAPI
#define YIELD() SwitchToThread()
#else
#include <pthread.h>
#include <sched.h>
#define THREAD_RESULT void *
#define YIELD() sched_yield()
#endif
#define COUNT 1000000u
static struct cw_event_queue q;
int cw_radio_set(int on, const struct cw_config *c, unsigned e) {
    (void)on; (void)c; (void)e; return 0;
}
static THREAD_RESULT producer(void *unused) {
    (void)unused;
    for(unsigned i=0;i<COUNT;i++) {
        struct cw_event e={i, i ^ 0xabcdef01u, (int)(i % 65)};
        while(!cw_events_push(&q,e)) YIELD();
    }
    return 0;
}
static THREAD_RESULT consumer(void *unused) {
    (void)unused;
    for(unsigned i=0;i<COUNT;i++) {
        struct cw_event e;
        while(!cw_events_peek(&q,&e)) YIELD();
        assert(e.us==i && e.epoch==(i ^ 0xabcdef01u) && e.state==(int)(i%65));
        cw_events_pop(&q);
    }
    return 0;
}
int main(void) {
#ifdef _WIN32
    HANDLE a=CreateThread(NULL,0,producer,NULL,0,NULL);
    HANDLE b=CreateThread(NULL,0,consumer,NULL,0,NULL);
    assert(a && b);
    assert(WaitForSingleObject(a,30000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(b,30000)==WAIT_OBJECT_0);
    CloseHandle(a); CloseHandle(b);
#else
    pthread_t a,b;
    assert(!pthread_create(&a,NULL,producer,NULL));
    assert(!pthread_create(&b,NULL,consumer,NULL));
    assert(!pthread_join(a,NULL)); assert(!pthread_join(b,NULL));
#endif
    puts("PASS 1,000,000 ordered events across concurrent producer/consumer threads");
    return 0;
}
