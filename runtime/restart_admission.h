/* Reversible admission: close never commits or discards guest state. */
#ifndef WT_RESTART_ADMISSION_H
#define WT_RESTART_ADMISSION_H
#include <pthread.h>
#include <errno.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    unsigned active;
    int closed;
} WtRestartAdmission;
#define WT_RESTART_ADMISSION_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 }

static inline void wt_restart_enter(WtRestartAdmission *a) {
    pthread_mutex_lock(&a->mutex);
    while (a->closed) pthread_cond_wait(&a->changed, &a->mutex);
    ++a->active;
    pthread_mutex_unlock(&a->mutex);
}
static inline void wt_restart_leave(WtRestartAdmission *a) {
    pthread_mutex_lock(&a->mutex);
    if (--a->active == 0) pthread_cond_broadcast(&a->changed);
    pthread_mutex_unlock(&a->mutex);
}
static inline int wt_restart_close(WtRestartAdmission *a, unsigned timeout_ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&a->mutex);
    a->closed = 1;
    int error = 0;
    while (a->active && !error) error = pthread_cond_timedwait(&a->changed, &a->mutex, &until);
    int ready = !error && !a->active;
    pthread_mutex_unlock(&a->mutex);
    return ready;
}
static inline void wt_restart_open(WtRestartAdmission *a) {
    pthread_mutex_lock(&a->mutex);
    a->closed = 0;
    pthread_cond_broadcast(&a->changed);
    pthread_mutex_unlock(&a->mutex);
}
#endif
