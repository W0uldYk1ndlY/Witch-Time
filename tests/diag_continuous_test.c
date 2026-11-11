#include "../runtime/diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
#include <windows.h>
typedef HANDLE wt_thread_t;
typedef DWORD (WINAPI *wt_thread_fn)(void *);

static inline wt_thread_t wt_thread_create(wt_thread_fn fn, void *arg) {
    return CreateThread(NULL, 0, fn, arg, 0, NULL);
}

static inline void wt_thread_join(wt_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline void wt_sleep_ms(uint32_t ms) {
    Sleep(ms);
}
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t wt_thread_t;
typedef void *(*wt_thread_fn)(void *);

static inline wt_thread_t wt_thread_create(wt_thread_fn fn, void *arg) {
    pthread_t t;
    pthread_create(&t, NULL, fn, arg);
    return t;
}

static inline void wt_thread_join(wt_thread_t t) {
    pthread_join(t, NULL);
}

static inline void wt_sleep_ms(uint32_t ms) {
    usleep(ms * 1000);
}
#endif

/* 1. Test producer registration limits and clean failure */
static void test_producer_registration_bounds(void) {
    printf("[1/7] Testing producer registration bounds and overflow failure...\n");
    diag_shutdown();
    assert(diag_init("test-continuous-session") == 0);
    assert(diag_is_enabled());

    /* Unregister default producers to test full range 0..15 */
    for (int p = 0; p < DIAG_MAX_PRODUCERS; p++) {
        diag_unregister_producer(p);
    }

    int ids[DIAG_MAX_PRODUCERS];
    for (int i = 0; i < DIAG_MAX_PRODUCERS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "prod_%d", i);
        ids[i] = diag_register_producer(name);
        assert(ids[i] >= 0);
        assert(ids[i] < DIAG_MAX_PRODUCERS);
    }

    /* 17th producer must fail cleanly with -1 without memory corruption or crash */
    int overflow_id = diag_register_producer("overflow_producer");
    assert(overflow_id == -1);

    /* Unregistering a slot allows reuse */
    diag_unregister_producer(ids[3]);
    int reused_id = diag_register_producer("reused_prod");
    assert(reused_id == ids[3]);

    /* Cleanup and re-init for following tests */
    diag_shutdown();
    assert(diag_init("test-continuous-session") == 0);
    printf("      Producer registration bounds and overflow safety (passed)\n");
}

/* 2. Test intervals, timing accuracy, and counter deltas */
static void test_intervals_and_counter_deltas(void) {
    printf("[2/7] Testing intervals and monotonic counter deltas...\n");
    diag_reset_continuous_counters();

    diag_continuous_counters_t initial;
    diag_get_continuous_counters(&initial);
    assert(initial.total_wait_events == 0);
    assert(initial.total_wait_ns == 0);

    /* Emit simulated wait intervals */
    uint32_t tid = 1001;
    diag_hook_thread_role(tid, DIAG_THREAD_ROLE_RENDER, 0x1234abcd);
    diag_hook_thread_wait(tid, DIAG_WAIT_MUTEX, 1500000, 0);  /* 1.5 ms wait */
    diag_hook_thread_wait(tid, DIAG_WAIT_CONDVAR, 3500000, 0); /* 3.5 ms wait */

    diag_continuous_counters_t after;
    diag_get_continuous_counters(&after);
    assert(after.total_wait_events == 2);
    assert(after.total_wait_ns == 5000000); /* 1.5ms + 3.5ms = 5.0ms */

    /* Counter delta calculation */
    uint64_t delta_events = after.total_wait_events - initial.total_wait_events;
    uint64_t delta_ns = after.total_wait_ns - initial.total_wait_ns;
    assert(delta_events == 2);
    assert(delta_ns == 5000000);

    printf("      Intervals and counter deltas accurate (passed)\n");
}

/* 3. Test unavailable POSIX/Android backend handling and no-fake-zeros rule */
static void test_unavailable_backend_and_no_fake_zeros(void) {
    printf("[3/7] Testing unavailable backend and strict null/sentinel handling...\n");
    diag_reset_continuous_counters();

    diag_continuous_counters_t c1;
    diag_get_continuous_counters(&c1);
    uint64_t prev_events = c1.total_wait_events;
    uint64_t prev_ns = c1.total_wait_ns;

    /* If a platform/backend wait observation is unavailable (e.g. Bionic internal mutex
     * where inspecting internal struct fields is strictly prohibited, or an OS without
     * timer query capability), it emits DIAG_MEASUREMENT_UNAVAILABLE (-1) */
    diag_hook_thread_wait(1002, DIAG_WAIT_MUTEX, DIAG_MEASUREMENT_UNAVAILABLE, -1);

    diag_continuous_counters_t c2;
    diag_get_continuous_counters(&c2);

    /* An unavailable measurement MUST NOT fabricate 0 or corrupt the aggregate wait ns */
    assert(c2.total_wait_events == prev_events);
    assert(c2.total_wait_ns == prev_ns);

    /* Drain the emitted record to verify it carries DIAG_MEASUREMENT_UNAVAILABLE */
    diag_record_t recs[8];
    size_t drained = diag_drain_producer(0, recs, 8);
    bool found_unavailable = false;
    for (size_t i = 0; i < drained; i++) {
        if (recs[i].type == DIAG_EV_THREAD_WAIT && recs[i].payload[2] == DIAG_MEASUREMENT_UNAVAILABLE) {
            found_unavailable = true;
            break;
        }
    }
    assert(found_unavailable);

    printf("      Unavailable measurement strictly distinguished from 0 (passed)\n");
}

/* 4. Test shader queue metrics and compile duration bounds */
static void test_shader_queue_and_compilation_bounds(void) {
    printf("[4/7] Testing shader queue and compilation tracking...\n");
    diag_reset_continuous_counters();

    /* Hook 4 queued jobs, 12 completed, 4 active workers, 2,500,000 ns avg compile time */
    diag_hook_shader_queue(4, 12, 4, 2500000);

    diag_continuous_counters_t c;
    diag_get_continuous_counters(&c);
    assert(c.shader_queued_jobs == 4);
    assert(c.shader_completed_jobs == 12);
    assert(c.shader_active_workers == 4);
    assert(c.shader_avg_compile_ns == 2500000);

    /* Drain and verify emitted record */
    diag_record_t recs[8];
    size_t drained = diag_drain_producer(0, recs, 8);
    bool found_shader_ev = false;
    for (size_t i = 0; i < drained; i++) {
        if (recs[i].type == DIAG_EV_SHADER_QUEUE) {
            assert(recs[i].payload[0] == 4);
            assert(recs[i].payload[1] == 12);
            assert(recs[i].payload[2] == 4);
            assert(recs[i].payload[3] == 2500000);
            found_shader_ev = true;
            break;
        }
    }
    assert(found_shader_ev);
    printf("      Shader queue and compilation bounds verified (passed)\n");
}

/* 5. Test file operation durations and byte transfers */
static void test_file_io_durations_and_bytes(void) {
    printf("[5/7] Testing file I/O operations and byte accounting...\n");
    diag_reset_continuous_counters();

    /* Simulate file open: 120,000 ns, 0 bytes, status 0 */
    diag_hook_file_op(DIAG_FILE_OP_OPEN, 120000, 0, 0);

    /* Simulate file read: 850,000 ns, 65536 bytes, status 0 */
    diag_hook_file_op(DIAG_FILE_OP_READ, 850000, 65536, 0);

    /* Simulate file write: 1,200,000 ns, 32768 bytes, status 0 */
    diag_hook_file_op(DIAG_FILE_OP_WRITE, 1200000, 32768, 0);

    diag_continuous_counters_t c;
    diag_get_continuous_counters(&c);
    assert(c.file_ops_count == 3);
    assert(c.file_total_duration_ns == (120000 + 850000 + 1200000));
    assert(c.file_total_bytes == (65536 + 32768));

    /* Test unavailable duration handling (e.g. platform error) */
    diag_hook_file_op(DIAG_FILE_OP_READ, DIAG_MEASUREMENT_UNAVAILABLE, 1024, 0);
    diag_get_continuous_counters(&c);
    assert(c.file_ops_count == 3); /* Unmeasured duration does not increment duration-measured ops */
    assert(c.file_total_bytes == (65536 + 32768 + 1024));

    printf("      File I/O durations and byte accounting verified (passed)\n");
}

/* 6. Test memory compare, copy, stream, and allocation observations */
static void test_memory_streaming_and_allocations(void) {
    printf("[6/7] Testing memory compare/copy/stream and allocation observations...\n");
    diag_reset_continuous_counters();

    /* Compare 1024 bytes, copy 2048 bytes, stream 4096 bytes, 1 upload */
    diag_hook_mem_stream(1024, 2048, 4096, 1);

    /* Compare 512 bytes, copy 1024 bytes, stream 2048 bytes, 1 upload */
    diag_hook_mem_stream(512, 1024, 2048, 1);

    diag_continuous_counters_t c;
    diag_get_continuous_counters(&c);
    assert(c.mem_compare_bytes == 1536);
    assert(c.mem_copy_bytes == 3072);
    assert(c.mem_stream_bytes == 6144);

    /* Track allocations: current 10 MB, peak 12 MB */
    diag_hook_mem_alloc(10 * 1024 * 1024, 12 * 1024 * 1024, 500, 200);
    diag_get_continuous_counters(&c);
    assert(c.mem_allocated_bytes == 10 * 1024 * 1024);
    assert(c.mem_peak_allocated_bytes == 12 * 1024 * 1024);

    /* Track new peak: 15 MB */
    diag_hook_mem_alloc(14 * 1024 * 1024, 15 * 1024 * 1024, 600, 250);
    diag_get_continuous_counters(&c);
    assert(c.mem_allocated_bytes == 14 * 1024 * 1024);
    assert(c.mem_peak_allocated_bytes == 15 * 1024 * 1024);

    /* Lower peak reported must not regress the highest recorded peak */
    diag_hook_mem_alloc(8 * 1024 * 1024, 11 * 1024 * 1024, 700, 400);
    diag_get_continuous_counters(&c);
    assert(c.mem_allocated_bytes == 8 * 1024 * 1024);
    assert(c.mem_peak_allocated_bytes == 15 * 1024 * 1024);

    printf("      Memory streaming and allocation tracking verified (passed)\n");
}

/* 7. Test audio counters and starvation tracking */
static void test_audio_counters_and_starvation(void) {
    printf("[7/7] Testing audio buffer and starvation counters...\n");
    diag_reset_continuous_counters();

    /* 100 appended, 96 released, 0 underruns, 0 starved */
    diag_hook_audio_counters(100, 96, 0, 0);

    diag_continuous_counters_t c;
    diag_get_continuous_counters(&c);
    assert(c.audio_appended_frames == 100);
    assert(c.audio_released_frames == 96);
    assert(c.audio_underruns == 0);
    assert(c.audio_starved == 0);

    /* Update with 2 underruns and 1 starved */
    diag_hook_audio_counters(250, 240, 2, 1);
    diag_get_continuous_counters(&c);
    assert(c.audio_appended_frames == 250);
    assert(c.audio_released_frames == 240);
    assert(c.audio_underruns == 2);
    assert(c.audio_starved == 1);

    printf("      Audio buffer and starvation counters verified (passed)\n");
}

int main(void) {
    printf("=== Witch-Time Continuous State & Counters Harness ===\n");
    test_producer_registration_bounds();
    test_intervals_and_counter_deltas();
    test_unavailable_backend_and_no_fake_zeros();
    test_shader_queue_and_compilation_bounds();
    test_file_io_durations_and_bytes();
    test_memory_streaming_and_allocations();
    test_audio_counters_and_starvation();
    printf("=== All 7 Continuous State Tests Passed ===\n");
    return 0;
}
