/* gcc -std=gnu11 -pthread tests/diag_seal_test.c runtime/diag_writer.c -o diag_seal_test */
#include <assert.h>
#include <pthread.h>
static void admission_hook(void);
#define DIAG_TEST_ADMISSION_HOOK admission_hook
#include "../runtime/diag.c"
#include "../runtime/diag_writer.h"

static unsigned pause_emit, entered, release_emit;
static bool seal_result;
static void admission_hook(void) {
    if (!__atomic_load_n(&pause_emit, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&entered, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&release_emit, __ATOMIC_ACQUIRE)) {
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
}
static void *emit_one(void *arg) {
    diag_emit((int)(intptr_t)arg, DIAG_EV_CUSTOM, 100, 0, 0, 0);
    return NULL;
}
static void *seal(void *arg) {
    (void)arg;
    seal_result = diag_seal_producers();
    return NULL;
}
static void wait_entered(void) {
    uint64_t start = diag_time_ns();
    while (!__atomic_load_n(&entered, __ATOMIC_ACQUIRE))
        assert(diag_time_ns() - start < 1000000000ULL);
}
static void check_json(const char *dir, const char *file, const char *field) {
    char path[512], buf[8192];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *f = fopen(path, "r");
    assert(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    assert(strstr(buf, field));
}
static void test_writer_timeout(void) {
    __atomic_store_n(&pause_emit, 0, __ATOMIC_RELEASE);
    entered = release_emit = 0;
    assert(diag_init("writer-seal-timeout") == 0);
    diag_writer_config_t cfg = {0};
    snprintf(cfg.output_dir, sizeof(cfg.output_dir), "diag_seal_capture_%llu",
             (unsigned long long)diag_time_ns());
    strcpy(cfg.run_id, "writer-seal-timeout");
    cfg.disable_unbuffered_logging = true;
    assert(diag_writer_init(&cfg) == 0);
    int producer = diag_register_producer("paused");
    diag_emit(producer, DIAG_EV_CUSTOM, 0, 0, 0, 0);
    __atomic_store_n(&pause_emit, 1, __ATOMIC_RELEASE);
    pthread_t emitter;
    assert(pthread_create(&emitter, NULL, emit_one, (void *)(intptr_t)producer) == 0);
    wait_entered();
    /* Standalone writer stop must seal too, without relying on its caller. */
    diag_writer_stop();
    diag_writer_stats_t stats;
    diag_writer_get_stats(&stats);
    assert(stats.state == DIAG_WRITER_ERROR);
    assert(stats.total_records_written == 1);
    check_json(cfg.output_dir, "status.json", "\"healthy\": false");
    check_json(cfg.output_dir, "status.json", "\"state\": \"error\"");
    assert(diag_writer_write_outcome("completed", "DIAG_PRODUCERS_NOT_QUIESCENT", NULL) == 0);
    check_json(cfg.output_dir, "outcome.json", "\"clean_exit\": true");
    check_json(cfg.output_dir, "outcome.json", "\"exit_code\": 0");
    check_json(cfg.output_dir, "outcome.json", "\"incomplete_recovery\": true");
    __atomic_store_n(&release_emit, 1, __ATOMIC_RELEASE);
    assert(pthread_join(emitter, NULL) == 0);
    assert(diag_get_total_emitted() == 2);
    diag_writer_stop();
    diag_writer_shutdown();
    diag_writer_get_stats(&stats);
    assert(stats.state == DIAG_WRITER_ERROR);
    assert(stats.total_records_written == 1); /* Late accepted tail is not claimed persisted. */
    check_json(cfg.output_dir, "status.json", "\"healthy\": false");
    check_json(cfg.output_dir, "status.json", "\"state\": \"error\"");
    diag_shutdown();
}
int main(void) {
    for (int timeout = 0; timeout < 2; timeout++) {
        __atomic_store_n(&pause_emit, 0, __ATOMIC_RELEASE);
        entered = release_emit = 0;
        assert(diag_init("seal-test") == 0);
        int producer = diag_register_producer("test");
        assert(producer >= 0);
        for (int i = 0; i < 100; i++)
            diag_emit(producer, DIAG_EV_CUSTOM, i, 0, 0, 0);
        __atomic_store_n(&pause_emit, 1, __ATOMIC_RELEASE);
        pthread_t emitter, sealer;
        assert(pthread_create(&emitter, NULL, emit_one, (void *)(intptr_t)producer) == 0);
        wait_entered();
        if (timeout) {
            uint64_t start = diag_time_ns();
            assert(!diag_seal_producers());
            assert(diag_time_ns() - start < 1000000000ULL);
        } else {
            assert(pthread_create(&sealer, NULL, seal, NULL) == 0);
            uint64_t start = diag_time_ns();
            while (diag_is_enabled()) assert(diag_time_ns() - start < 1000000000ULL);
        }
        /* An emitter admitted before close is paused inside diag_emit. */
        assert(diag_get_total_emitted() == 100);
        diag_emit(producer, DIAG_EV_CUSTOM, 999, 0, 0, 0);
        assert(diag_register_producer("late") == -1);
        assert(diag_get_total_emitted() == 100);
        __atomic_store_n(&release_emit, 1, __ATOMIC_RELEASE);
        assert(pthread_join(emitter, NULL) == 0);
        if (!timeout) {
            assert(pthread_join(sealer, NULL) == 0);
            assert(seal_result);
        }
        assert(diag_seal_producers() == !timeout);
        diag_shutdown();
        diag_record_t records[DIAG_RING_CAPACITY];
        assert(diag_drain_producer(producer, records, DIAG_RING_CAPACITY) == 101);
        for (int i = 0; i <= 100; i++) assert(records[i].payload[0] == i);
        assert(diag_get_total_emitted() == 101);
        diag_emit(producer, DIAG_EV_CUSTOM, 999, 0, 0, 0);
        assert(diag_drain_producer(producer, records, DIAG_RING_CAPACITY) == 0);
    }
    test_writer_timeout();
    puts("diag seal: accepted tail retained, post-seal rejected, timeout bounded, writer failure sticky");
    return 0;
}
