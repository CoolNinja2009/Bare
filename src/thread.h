/* thread.h: the few threading pieces the background loaders (picture.c,
 * video.c) need, on Windows' own locks or pthreads. Not part of the core. */
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#ifdef _WIN32
typedef SRWLOCK Lock;
typedef CONDITION_VARIABLE Cond;
typedef HANDLE Thread;
static inline void lock_init(Lock *l) { InitializeSRWLock(l); }
static inline void lock(Lock *l) { AcquireSRWLockExclusive(l); }
static inline void unlock(Lock *l) { ReleaseSRWLockExclusive(l); }
static inline void cond_init(Cond *c) { InitializeConditionVariable(c); }
static inline void cond_wait(Cond *c, Lock *l) { SleepConditionVariableSRW(c, l, INFINITE, 0); }
static inline void cond_wait_ms(Cond *c, Lock *l, int ms) { SleepConditionVariableSRW(c, l, (DWORD)ms, 0); }
static inline void cond_one(Cond *c) { WakeConditionVariable(c); }
static inline void cond_all(Cond *c) { WakeAllConditionVariable(c); }
#else
typedef pthread_mutex_t Lock;
typedef pthread_cond_t Cond;
typedef pthread_t Thread;
static inline void lock_init(Lock *l) { pthread_mutex_init(l, NULL); }
static inline void lock(Lock *l) { pthread_mutex_lock(l); }
static inline void unlock(Lock *l) { pthread_mutex_unlock(l); }
static inline void cond_init(Cond *c) { pthread_cond_init(c, NULL); }
static inline void cond_wait(Cond *c, Lock *l) { pthread_cond_wait(c, l); }
static inline void cond_wait_ms(Cond *c, Lock *l, int ms) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) t.tv_sec++, t.tv_nsec -= 1000000000L;
    pthread_cond_timedwait(c, l, &t);
}
static inline void cond_one(Cond *c) { pthread_cond_signal(c); }
static inline void cond_all(Cond *c) { pthread_cond_broadcast(c); }
#endif
