#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define make_dir(p) _mkdir(p)
static void yield_thread(void) { SwitchToThread(); }
#else
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0700)
static void yield_thread(void) { sched_yield(); }
#endif
static uint32_t pause_writer, pause_admit, pause_before_admit, pause_arm, arm_thread, entered, release_writer;
static void before_admit(void) {
    if (!__atomic_load_n(&pause_before_admit, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_writer, __ATOMIC_ACQUIRE)) yield_thread();
}
static void before_arm(void) {
    if (!__atomic_load_n(&pause_arm, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_writer, __ATOMIC_ACQUIRE)) yield_thread();
}
static void after_admit(void) {
    if (!__atomic_load_n(&pause_admit, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_writer, __ATOMIC_ACQUIRE)) yield_thread();
}
static void before_publish(void) {
    if (!__atomic_load_n(&pause_writer, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_writer, __ATOMIC_ACQUIRE)) yield_thread();
}
#define WT_TSC_TEST_BEFORE_PUBLISH before_publish
#define WT_TSC_TEST_BEFORE_ADMIT before_admit
#define WT_TSC_TEST_AFTER_ADMIT after_admit
#define WT_TSC_TEST_BEFORE_ARM before_arm
#include "../runtime/temporal_source_capture.c"
static WtTscEmit emitter;
static uint64_t emitter_epoch;
static int bind_capture(uint32_t v, uint32_t s, uint64_t epoch, const uint32_t *gate, WtTscEmit emit) {
    assert(v == WT_TSC_VERSION && s == sizeof(WtTscRecord));
    assert(!!gate == !!emit && !!gate == !!epoch);
    emitter = emit; emitter_epoch = epoch; return 0;
}
static void run_writer(void *arg) {
    if (__atomic_load_n(&arm_thread, __ATOMIC_ACQUIRE)) { wt_tsc_frame(50, 1); return; }
    WtTscEmit emit = emitter;
    uint64_t epoch = emitter_epoch;
    for (unsigned i = 0; i < 200; i++) {
        WtTscRecord r = {0}; r.version = WT_TSC_VERSION; r.size = sizeof r; r.kind = WT_TSC_GUEST;
        r.site = (uint64_t)(uintptr_t)arg; r.word[0] = i; emit(epoch, &r);
    }
}
#ifdef _WIN32
typedef HANDLE Thread;
static DWORD WINAPI worker(LPVOID arg) { run_writer(arg); return 0; }
static Thread launch(uintptr_t id) { HANDLE t = CreateThread(NULL, 0, worker, (void *)id, 0, NULL); assert(t); return t; }
static void join(Thread t) { assert(WaitForSingleObject(t, 10000) == WAIT_OBJECT_0); CloseHandle(t); }
#else
typedef pthread_t Thread;
static void *worker(void *arg) { run_writer(arg); return NULL; }
static Thread launch(uintptr_t id) { pthread_t t; assert(!pthread_create(&t, NULL, worker, (void *)id)); return t; }
static void join(Thread t) { assert(!pthread_join(t, NULL)); }
#endif
int main(int argc, char **argv) {
    assert(argc == 2); char dir[1200], file[1300];
    snprintf(dir, sizeof dir, "%s/concurrent", argv[1]); assert(!make_dir(dir));
    assert(!wt_tsc_start(dir, 1, bind_capture)); wt_tsc_frame(10, 1);
    Thread threads[4];
    for (unsigned i = 0; i < 4; i++) threads[i] = launch(i + 1);
    for (unsigned i = 0; i < 4; i++) join(threads[i]);
    wt_tsc_frame(14, 1); wt_tsc_core_loss(0); assert(!wt_tsc_finish());
    snprintf(file, sizeof file, "%s/temporal_source.bin", dir);
    FILE *f = fopen(file, "rb"); assert(f);
    WtTscRecord r; unsigned count = 0, seen[4][200] = {{0}};
    while (fread(&r, sizeof r, 1, f)) {
        assert(r.sequence == ++count && r.site >= 1 && r.site <= 4 && r.word[0] < 200);
        assert(!seen[r.site - 1][r.word[0]]++);
    }
    assert(count == 800); fclose(f);

    snprintf(dir, sizeof dir, "%s/unpublished", argv[1]); assert(!make_dir(dir));
    assert(!wt_tsc_start(dir, 1, bind_capture)); wt_tsc_frame(1, 1);
    __atomic_store_n(&pause_writer, 1, __ATOMIC_RELEASE);
    Thread paused = launch(1);
    unsigned tries = 0;
    while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE) && ++tries < 10000000) yield_thread();
    assert(__atomic_load_n(&entered, __ATOMIC_ACQUIRE));
    wt_tsc_frame(5, 1); wt_tsc_core_loss(0);
    assert(wt_tsc_finish() == -1); /* Must not wait for or copy the unpublished slot. */
    assert(wt_tsc_start(dir, 1, bind_capture) == -1); /* Still-live writer blocks reset. */
    __atomic_store_n(&release_writer, 1, __ATOMIC_RELEASE); join(paused);
    __atomic_store_n(&pause_writer, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&pause_admit, 1, __ATOMIC_RELEASE);
    for (int interior = 0; interior < 2; interior++) {
        snprintf(dir, sizeof dir, "%s/boundary_%d", argv[1], interior); assert(!make_dir(dir));
        assert(!wt_tsc_start(dir, 1, bind_capture)); wt_tsc_frame(10, 1);
        __atomic_store_n(&entered, 0, __ATOMIC_RELEASE); __atomic_store_n(&release_writer, 0, __ATOMIC_RELEASE);
        paused = launch(1); tries = 0;
        while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE) && ++tries < 10000000) yield_thread();
        assert(__atomic_load_n(&entered, __ATOMIC_ACQUIRE));
        if (interior) wt_tsc_frame(13, 1);
        wt_tsc_frame(14, 0);
        __atomic_store_n(&release_writer, 1, __ATOMIC_RELEASE); join(paused);
        wt_tsc_core_loss(0); assert(wt_tsc_finish() == (interior ? -1 : 0));
        snprintf(file, sizeof file, "%s/temporal_source.bin", dir); f = fopen(file, "rb"); assert(f);
        count = 0;
        while (fread(&r, sizeof r, 1, f)) { assert(r.frame >= 10 && r.frame < 14); count++; }
        fclose(f); assert(count == (interior ? 0u : 1u));
    }
    /* A callback paused before admission must not enter a subsequent run, even when
       both runs use the same frame and the admission word has returned to OPEN. */
    __atomic_store_n(&pause_admit, 0, __ATOMIC_RELEASE);
    snprintf(dir, sizeof dir, "%s/old_epoch", argv[1]); assert(!make_dir(dir));
    assert(!wt_tsc_start(dir, 1, bind_capture)); wt_tsc_frame(20, 1);
    WtTscEmit old_emit = emitter; uint64_t old_epoch = emitter_epoch;
    __atomic_store_n(&pause_before_admit, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&entered, 0, __ATOMIC_RELEASE); __atomic_store_n(&release_writer, 0, __ATOMIC_RELEASE);
    paused = launch(1); tries = 0;
    while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE) && ++tries < 10000000) yield_thread();
    assert(__atomic_load_n(&entered, __ATOMIC_ACQUIRE));
    wt_tsc_frame(24, 1); wt_tsc_core_loss(0); assert(wt_tsc_finish() == -1);
    snprintf(dir, sizeof dir, "%s/new_epoch", argv[1]); assert(!make_dir(dir));
    assert(!wt_tsc_start(dir, 1, bind_capture)); assert(emitter_epoch && emitter_epoch != old_epoch);
    wt_tsc_frame(20, 1);
    __atomic_store_n(&release_writer, 1, __ATOMIC_RELEASE); join(paused);
    __atomic_store_n(&pause_before_admit, 0, __ATOMIC_RELEASE);
    r = (WtTscRecord){0};
    old_emit(old_epoch, NULL); old_emit(old_epoch, &r); /* Invalid stale input cannot stop the new run. */
    assert(wt_tsc_enabled());
    r.version = WT_TSC_VERSION; r.size = sizeof r; r.kind = WT_TSC_GUEST; r.site = 99;
    emitter(emitter_epoch, &r);
    wt_tsc_frame(24, 1); wt_tsc_core_loss(0); assert(!wt_tsc_finish());
    snprintf(file, sizeof file, "%s/temporal_source.bin", dir); f = fopen(file, "rb"); assert(f);
    assert(fread(&r, sizeof r, 1, f) == 1 && r.site == 99 && r.sequence == 1);
    assert(fread(&r, sizeof r, 1, f) == 0); fclose(f);

    snprintf(dir, sizeof dir, "%s/finish_arm", argv[1]); assert(!make_dir(dir));
    assert(!wt_tsc_start(dir, 1, bind_capture));
    __atomic_store_n(&pause_arm, 1, __ATOMIC_RELEASE); __atomic_store_n(&arm_thread, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&entered, 0, __ATOMIC_RELEASE); __atomic_store_n(&release_writer, 0, __ATOMIC_RELEASE);
    paused = launch(1); tries = 0;
    while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE) && ++tries < 10000000) yield_thread();
    assert(__atomic_load_n(&entered, __ATOMIC_ACQUIRE));
    wt_tsc_core_loss(0); assert(wt_tsc_finish() == -1);
    assert(!wt_tsc_enabled() && !wt_tsc_requested());
    assert(wt_tsc_start(dir, 1, bind_capture) == -1); /* Frame hook is still inside its bounded map lock. */
    __atomic_store_n(&release_writer, 1, __ATOMIC_RELEASE); join(paused);
    assert(!wt_tsc_enabled() && !wt_tsc_requested()); /* A late first arm cannot reopen the terminal gate. */
    puts("Concurrent publication and nonblocking incomplete-finalization checks passed");
    return 0;
}
