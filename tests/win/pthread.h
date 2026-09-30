/* Test-only Windows adapter; never included by the Raspberry Pi build. */
#ifndef CW_TEST_PTHREAD_H
#define CW_TEST_PTHREAD_H
#include <windows.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
typedef HANDLE pthread_t;
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT
static int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive(m); return 0; }
static int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive(m); return 0; }
static int pthread_mutex_trylock(pthread_mutex_t *m) { return TryAcquireSRWLockExclusive(m) ? 0 : EBUSY; }
static int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return SleepConditionVariableSRW(c,m,INFINITE,0) ? 0 : EINVAL;
}
static int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable(c); return 0; }
struct cw_thread_args { void *(*fn)(void *); void *arg; };
static DWORD WINAPI cw_thread_thunk(void *v) {
    struct cw_thread_args a=*(struct cw_thread_args *)v;
    free(v); a.fn(a.arg); return 0;
}
static int pthread_create(pthread_t *t, const void *attr, void *(*fn)(void *), void *arg) {
    (void)attr;
    struct cw_thread_args *a=malloc(sizeof(*a));
    if(!a) return ENOMEM;
    a->fn=fn; a->arg=arg;
    *t=CreateThread(NULL,0,cw_thread_thunk,a,0,NULL);
    if(!*t) { free(a); return EAGAIN; }
    return 0;
}
static int pthread_join(pthread_t t, void **result) {
    (void)result;
    if(WaitForSingleObject(t,5000)!=WAIT_OBJECT_0) return EINVAL;
    CloseHandle(t); return 0;
}
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#define TIMER_ABSTIME 1
static int cw_test_clock_gettime(int clock, struct timespec *t) {
    (void)clock;
    LARGE_INTEGER n,f;
    QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f);
    t->tv_sec=n.QuadPart/f.QuadPart;
    t->tv_nsec=(long)((n.QuadPart%f.QuadPart)*1000000000LL/f.QuadPart);
    return 0;
}
static int cw_test_clock_nanosleep(int clock, int flags, const struct timespec *d, struct timespec *r) {
    (void)r;
    struct timespec n;
    cw_test_clock_gettime(clock,&n);
    long long ns=(long long)d->tv_sec*1000000000LL+d->tv_nsec;
    if(flags==TIMER_ABSTIME) ns-=(long long)n.tv_sec*1000000000LL+n.tv_nsec;
    if(ns>0) Sleep((DWORD)((ns+999999)/1000000));
    return 0;
}
#define clock_gettime cw_test_clock_gettime
#define clock_nanosleep cw_test_clock_nanosleep
#endif
