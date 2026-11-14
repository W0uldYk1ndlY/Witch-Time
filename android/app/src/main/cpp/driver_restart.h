#ifndef WT_DRIVER_RESTART_H
#define WT_DRIVER_RESTART_H
#include <pthread.h>
#include <string.h>

typedef struct {
    int (*gpu_prepare)(unsigned);
    void (*gpu_cancel)(void);
    int (*fs_prepare)(unsigned);
    void (*fs_cancel)(void);
} WtRestartHooks;
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    char nonce[129];
    int preparing, ready, cancelling, cancelled;
    WtRestartHooks hooks;
} WtDriverRestart;
#define WT_DRIVER_RESTART_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, "", 0, 0, 0, 0, {0} }

static inline void wt_driver_restart_release(WtDriverRestart *s) {
    /* FS work must be able to finish while the render barrier is being cancelled. */
    s->hooks.fs_cancel();
    s->hooks.gpu_cancel();
    pthread_mutex_lock(&s->lock);
    s->nonce[0] = 0;
    s->ready = s->preparing = s->cancelling = 0;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->lock);
}
static inline int wt_driver_restart_prepare(WtDriverRestart *s, const char *nonce) {
    if (!nonce || !*nonce || strlen(nonce) >= sizeof s->nonce) return 0;
    pthread_mutex_lock(&s->lock);
    if (s->nonce[0] || !s->hooks.gpu_prepare || !s->hooks.gpu_cancel ||
        !s->hooks.fs_prepare || !s->hooks.fs_cancel) {
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    strcpy(s->nonce, nonce);
    s->preparing = 1;
    s->cancelled = 0;
    pthread_mutex_unlock(&s->lock);
    /* Drain GPU while FS is open: admitted render work may depend on filesystem work. */
    int ready = s->hooks.gpu_prepare(5000);
    if (ready) ready = s->hooks.fs_prepare(5000);
    pthread_mutex_lock(&s->lock);
    ready = ready && !s->cancelled;
    s->preparing = 0;
    s->ready = ready;
    s->cancelling = !ready;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->lock);
    if (!ready) wt_driver_restart_release(s);
    return ready;
}
static inline int wt_driver_restart_cancel(WtDriverRestart *s, const char *nonce) {
    if (!nonce) return 0;
    pthread_mutex_lock(&s->lock);
    if (!s->nonce[0] || strcmp(s->nonce, nonce)) { pthread_mutex_unlock(&s->lock); return 0; }
    s->cancelled = 1;
    while (s->preparing || s->cancelling) pthread_cond_wait(&s->changed, &s->lock);
    /* A failed prepare already released this token; never release a newer one. */
    if (strcmp(s->nonce, nonce)) { pthread_mutex_unlock(&s->lock); return 1; }
    s->cancelling = 1;
    s->ready = 0;
    pthread_mutex_unlock(&s->lock);
    wt_driver_restart_release(s);
    return 1;
}
static inline int wt_driver_restart_commit(WtDriverRestart *s, const char *nonce,
                                          int (*seal)(void *, const char *),
                                          void (*terminate)(void *), void *context) {
    if (!nonce || !seal || !terminate) return 0;
    pthread_mutex_lock(&s->lock);
    if (!s->ready || s->cancelled || s->cancelling || strcmp(s->nonce, nonce)) {
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    /* Cancel and seal linearize at this lock; both leases stay live through process exit. */
    int sealed = seal(context, nonce);
    if (sealed) terminate(context);
    else s->cancelled = 1; /* A failed seal may be durable already; only coordinated discard/cancel can release. */
    pthread_mutex_unlock(&s->lock);
    return sealed;
}
#endif
