#include "../runtime/diag.h"
#include "../runtime/diag_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(_WIN32)
#include <windows.h>
static void clean_dir(const char *dir) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", dir);
    system(cmd);
}
#else
#include <unistd.h>
static void clean_dir(const char *dir) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" 2>/dev/null", dir);
    system(cmd);
}
#endif

/* 1. Test streaming more than 40,000 frames without legacy cap */
static void test_stream_over_40000_frames(void) {
    printf("[1/5] Testing streaming more than 40,000 frames...\n");
    const char *test_dir = "tests/disp_stream_40k";
    clean_dir(test_dir);

    assert(diag_init("run_stream_40k") == 0);
    diag_writer_config_t cfg = {0};
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_stream_40k", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 64 * 1024 * 1024; /* 64 MiB */
    cfg.incident_reserve_bytes = 4 * 1024 * 1024;
    cfg.chunk_record_limit = 500;

    assert(diag_writer_init(&cfg) == 0);

    /* Stream 45,000 frames in batches, flushing regularly */
    const int total_frames = 45000;
    for (int f = 0; f < total_frames; f++) {
        diag_stream_frame_sample(f, 16666666, 16666666, 4 /* completed */);
        if ((f & 0xFF) == 0) {
            diag_writer_flush_chunks();
        }
    }
    diag_writer_stop();
    diag_writer_shutdown();
    diag_shutdown();

    /* Verify files and chunks */
    char status_file[320];
    snprintf(status_file, sizeof(status_file), "%s/status.json", test_dir);
    FILE *fstat = fopen(status_file, "r");
    assert(fstat != NULL);
    char buf[1024];
    size_t r = fread(buf, 1, sizeof(buf) - 1, fstat);
    buf[r] = '\0';
    fclose(fstat);

    assert(strstr(buf, "\"total_records_written\": 45000") != NULL);
    clean_dir(test_dir);
    printf("      Successfully streamed and committed 45,000 frames (passed)\n");
}

/* 2. Test delayed query completion and shutdown with unfinished slots */
static void test_delayed_query_and_unfinished_slots(void) {
    printf("[2/5] Testing delayed query completion and unfinished slots at shutdown...\n");
    const char *test_dir = "tests/disp_stream_delayed";
    clean_dir(test_dir);

    assert(diag_init("run_stream_delayed") == 0);
    diag_writer_config_t cfg = {0};
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_stream_delayed", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 10 * 1024 * 1024;
    cfg.incident_reserve_bytes = 1024 * 1024;

    assert(diag_writer_init(&cfg) == 0);

    /* Frame 1: GPU slot 0 completes with 8.5 ms */
    diag_stream_gpu_sample(1, 8500000, 0, 0 /* COMPLETED */);
    /* Frame 2: GPU slot 1 is still pending when unexpected shutdown begins */
    diag_stream_gpu_sample(2, -2, 1, 1 /* PENDING */);

    /* Clean stop must commit completed data and finalize status without stalling */
    diag_writer_stop();

    diag_writer_stats_t stats;
    diag_writer_get_stats(&stats);
    assert(stats.total_records_written == 2);
    assert(stats.state == DIAG_WRITER_STOPPED);

    diag_writer_shutdown();
    diag_shutdown();
    clean_dir(test_dir);
    printf("      Delayed queries and unfinished slots recorded without stalling (passed)\n");
}

/* 3. Test timestamp valid bits and wrap arithmetic */
static void test_timestamp_wrap(void) {
    printf("[3/5] Testing GPU timestamp wrapping arithmetic...\n");
    /* Emulate a 36-bit GPU timestamp counter wrapping */
    const uint64_t mask_36 = (1ULL << 36) - 1;
    uint64_t ts_begin = (1ULL << 36) - 1000; /* Near wrap */
    uint64_t ts_end = 2500;                  /* Wrapped past zero */

    /* Unwrapped calculation */
    uint64_t delta = (ts_end - ts_begin) & mask_36;
    assert(delta == 3500);

    /* Nanoseconds calculation at 1.0 ns/tick */
    double period = 1.0;
    int64_t duration_ns = (int64_t)((double)delta * period);
    assert(duration_ns == 3500);

    /* Stream wrapped sample with status 0 */
    assert(diag_init("run_stream_wrap") == 0);
    diag_stream_gpu_sample(10, duration_ns, 0, 0 /* COMPLETED */);

    diag_record_t recs[2];
    size_t n = diag_drain_producer(0, recs, 2);
    assert(n == 1);
    assert(recs[0].type == DIAG_EV_GPU_SAMPLE);
    assert(recs[0].payload[0] == 10);
    assert(recs[0].payload[1] == 3500);
    assert(recs[0].payload[3] == 0);

    diag_shutdown();
    printf("      36-bit counter wrap properly resolved (passed)\n");
}

/* 4. Test monotonic domain anchors against wall-clock changes */
static void test_domain_anchors_and_wall_clock_jump(void) {
    printf("[4/5] Testing monotonic domain anchors and wall-clock jumps...\n");
    assert(diag_init("run_stream_anchor") == 0);

    /* Startup anchor: monotonic 1,000,000 ns, wall clock 1700000000 s */
    diag_stream_domain_anchor(1 /* startup */, 1000000ULL, 1700000000ULL * 1000000000ULL, 0);

    /* Emulate NTP wall-clock jump backwards 10 seconds while monotonic advances */
    diag_stream_domain_anchor(4 /* wall_sync */, 2000000ULL, (1700000000ULL - 10ULL) * 1000000000ULL, 60);

    diag_record_t recs[4];
    size_t n = diag_drain_producer(0, recs, 4);
    assert(n == 2);
    assert(recs[0].type == DIAG_EV_DOMAIN_ANCHOR && recs[0].payload[0] == 1);
    assert(recs[1].type == DIAG_EV_DOMAIN_ANCHOR && recs[1].payload[0] == 4);
    /* Monotonic strictly advanced */
    assert(recs[1].payload[1] > recs[0].payload[1]);

    diag_shutdown();
    printf("      Domain anchors decouple monotonic timeline from wall-clock jump (passed)\n");
}

/* 5. Test suspend and resume anchors */
static void test_suspend_resume_anchors(void) {
    printf("[5/5] Testing suspend and resume anchors...\n");
    assert(diag_init("run_stream_suspend") == 0);

    /* Frame 300: game suspended */
    diag_stream_domain_anchor(2 /* suspend */, 5000000000ULL, 1700000005ULL * 1000000000ULL, 300);

    /* Resumed 3 seconds later */
    diag_stream_domain_anchor(3 /* resume */, 8000000000ULL, 1700000008ULL * 1000000000ULL, 300);

    diag_record_t recs[4];
    size_t n = diag_drain_producer(0, recs, 4);
    assert(n == 2);
    assert(recs[0].type == DIAG_EV_DOMAIN_ANCHOR && recs[0].payload[0] == 2 && recs[0].payload[3] == 300);
    assert(recs[1].type == DIAG_EV_DOMAIN_ANCHOR && recs[1].payload[0] == 3 && recs[1].payload[3] == 300);

    diag_shutdown();
    printf("      Suspend and resume lifecycle anchors (passed)\n");
}

int main(void) {
    printf("=== Witch-Time Streaming Diagnostics Test ===\n");
    test_stream_over_40000_frames();
    test_delayed_query_and_unfinished_slots();
    test_timestamp_wrap();
    test_domain_anchors_and_wall_clock_jump();
    test_suspend_resume_anchors();
    printf("=== All 5 Streaming Diagnostics Tests Passed ===\n");
    return 0;
}
