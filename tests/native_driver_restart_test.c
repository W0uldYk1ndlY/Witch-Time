#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>
#include "../runtime/restart_admission.h"
#include "../android/app/src/main/cpp/driver_restart.h"

static WtRestartAdmission admission = WT_RESTART_ADMISSION_INIT;
static atomic_int late_started, late_finished;
static int closed_ready;
static void *close_gate(void *unused) { (void)unused; closed_ready = wt_restart_close(&admission, 1000); return NULL; }
static void *late_mutation(void *unused) {
    (void)unused;
    atomic_store(&late_started, 1);
    wt_restart_enter(&admission);
    atomic_store(&late_finished, 1);
    wt_restart_leave(&admission);
    return NULL;
}
static void gate_tests(void) {
    pthread_t closer, late;
    wt_restart_enter(&admission);
    assert(!pthread_create(&closer, NULL, close_gate, NULL));
    for (;;) {
        pthread_mutex_lock(&admission.mutex);
        int closed = admission.closed;
        pthread_mutex_unlock(&admission.mutex);
        if (closed) break;
        sched_yield();
    }
    assert(!pthread_create(&late, NULL, late_mutation, NULL));
    while (!atomic_load(&late_started)) sched_yield();
    pthread_mutex_lock(&admission.mutex);
    assert(admission.active == 1 && !atomic_load(&late_finished));
    pthread_mutex_unlock(&admission.mutex);
    wt_restart_leave(&admission);
    pthread_join(closer, NULL);
    assert(closed_ready && !atomic_load(&late_finished));
    wt_restart_open(&admission);
    pthread_join(late, NULL);
    assert(atomic_load(&late_finished));
    wt_restart_enter(&admission);
    assert(!wt_restart_close(&admission, 1));
    assert(admission.closed && admission.active == 1);
    wt_restart_open(&admission);
    wt_restart_leave(&admission);
}

static WtDriverRestart restart = WT_DRIVER_RESTART_INIT;
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_changed = PTHREAD_COND_INITIALIZER;
static int gpu_result = 1, fs_result = 1, gpu_block, gpu_entered, barrier_block, barrier_entered;
static int fs_live, gpu_live, seals, exits, seal_result, prepared;
static char order[64];
static unsigned order_size;
static void record(char c) { assert(order_size + 1 < sizeof order); order[order_size++] = c; order[order_size] = 0; }
static int gpu_prepare(unsigned timeout) {
    assert(timeout == 5000);
    pthread_mutex_lock(&fake_lock);
    record('G'); gpu_entered = 1; gpu_live = 1;
    pthread_cond_broadcast(&fake_changed);
    while (gpu_block) pthread_cond_wait(&fake_changed, &fake_lock);
    pthread_mutex_unlock(&fake_lock);
    return gpu_result;
}
static int fs_prepare(unsigned timeout) { assert(timeout == 5000 && gpu_live); record('F'); fs_live = 1; return fs_result; }
static void fs_cancel(void) { record('f'); fs_live = 0; }
static void gpu_cancel(void) {
    pthread_mutex_lock(&fake_lock);
    assert(!fs_live); record('g'); barrier_entered = 1;
    pthread_cond_broadcast(&fake_changed);
    while (barrier_block) pthread_cond_wait(&fake_changed, &fake_lock);
    gpu_live = 0;
    pthread_mutex_unlock(&fake_lock);
}
static int seal(void *unused, const char *nonce) { (void)unused; assert(!strcmp(nonce, "token")); assert(fs_live && gpu_live); ++seals; return seal_result; }
static void terminate(void *unused) { (void)unused; assert(fs_live && gpu_live && seal_result); ++exits; }
static void *prepare_thread(void *unused) { (void)unused; prepared = wt_driver_restart_prepare(&restart, "token"); return NULL; }
static void *cancel_thread(void *unused) { (void)unused; wt_driver_restart_cancel(&restart, "token"); return NULL; }
static void lease_tests(void) {
    restart.hooks = (WtRestartHooks){gpu_prepare, gpu_cancel, fs_prepare, fs_cancel};
    assert(!wt_driver_restart_prepare(&restart, ""));
    assert(wt_driver_restart_prepare(&restart, "token"));
    assert(!wt_driver_restart_prepare(&restart, "other"));
    assert(!wt_driver_restart_cancel(&restart, "other"));
    assert(!wt_driver_restart_commit(&restart, "other", seal, terminate, NULL));
    assert(!wt_driver_restart_commit(&restart, "token", seal, terminate, NULL));
    assert(seals == 1 && !exits && fs_live && gpu_live);
    assert(!wt_driver_restart_commit(&restart, "token", seal, terminate, NULL));
    assert(seals == 1 && !exits && fs_live && gpu_live);
    assert(wt_driver_restart_cancel(&restart, "token"));
    assert(!strcmp(order, "GFfg"));
    order_size = 0; order[0] = 0;
    assert(wt_driver_restart_prepare(&restart, "token"));
    seal_result = 1;
    assert(wt_driver_restart_commit(&restart, "token", seal, terminate, NULL));
    assert(exits == 1);
    assert(wt_driver_restart_cancel(&restart, "token"));
    assert(!strcmp(order, "GFfg"));
    assert(!wt_driver_restart_commit(&restart, "token", seal, terminate, NULL));
    gpu_result = 0;
    assert(!wt_driver_restart_prepare(&restart, "timeout"));
    assert(!gpu_live && !fs_live && seals == 2 && exits == 1);
    gpu_result = 1; fs_result = 0;
    assert(!wt_driver_restart_prepare(&restart, "fs-timeout"));
    assert(!gpu_live && !fs_live && seals == 2 && exits == 1);
    fs_result = 1;

    /* A cancel during prepare wins; even an outstanding GPU barrier must finish before release. */
    pthread_t preparation, cancellation;
    gpu_block = 1; gpu_entered = 0; barrier_block = 1; barrier_entered = 0;
    assert(!pthread_create(&preparation, NULL, prepare_thread, NULL));
    pthread_mutex_lock(&fake_lock);
    while (!gpu_entered) pthread_cond_wait(&fake_changed, &fake_lock);
    pthread_mutex_unlock(&fake_lock);
    assert(!pthread_create(&cancellation, NULL, cancel_thread, NULL));
    for (;;) {
        pthread_mutex_lock(&restart.lock);
        int cancelled = restart.cancelled;
        pthread_mutex_unlock(&restart.lock);
        if (cancelled) break;
        sched_yield();
    }
    pthread_mutex_lock(&fake_lock);
    gpu_block = 0;
    pthread_cond_broadcast(&fake_changed);
    while (!barrier_entered) pthread_cond_wait(&fake_changed, &fake_lock);
    pthread_mutex_unlock(&fake_lock);
    assert(!wt_driver_restart_commit(&restart, "token", seal, terminate, NULL));
    pthread_mutex_lock(&restart.lock);
    assert(restart.cancelling && restart.nonce[0]);
    pthread_mutex_unlock(&restart.lock);
    pthread_mutex_lock(&fake_lock);
    barrier_block = 0;
    pthread_cond_broadcast(&fake_changed);
    pthread_mutex_unlock(&fake_lock);
    pthread_join(preparation, NULL); pthread_join(cancellation, NULL);
    assert(!prepared && !restart.nonce[0] && !gpu_live && !fs_live);
    assert(seals == 2 && exits == 1);
}
int main(void) { gate_tests(); lease_tests(); puts("native restart admission/lease assertions passed"); return 0; }
