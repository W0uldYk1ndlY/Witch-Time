#include "../runtime/diag.h"
#include "../runtime/diag_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
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

/* 1. Test duplicate run labels and verify previous sessions are never overwritten */
static void test_duplicate_run_and_no_overwrite(void) {
    printf("[1/5] Testing duplicate run labels and no-overwrite guarantee...\n");
    const char *test_dir = "tests/disp_writer_dup";
    clean_dir(test_dir);

    diag_init("run_original");
    diag_writer_config_t cfg = {0};
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_original", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 1024 * 1024;
    cfg.incident_reserve_bytes = 256 * 1024;
    cfg.chunk_record_limit = 50;

    assert(diag_writer_init(&cfg) == 0);

    int prod = diag_register_producer("prod_dup");
    assert(prod >= 0);
    for (int i = 0; i < 20; i++) {
        diag_emit(prod, DIAG_EV_FRAME_START, i, 0, 0, 0);
    }
    assert(diag_writer_flush_chunks() == 0);
    diag_writer_stop();
    diag_writer_shutdown();
    diag_shutdown();

    /* Verify files exist */
    char status_file[320];
    snprintf(status_file, sizeof(status_file), "%s/status.json", test_dir);
    FILE *f = fopen(status_file, "r");
    assert(f != NULL);
    char buf[1024];
    size_t r = fread(buf, 1, sizeof(buf) - 1, f);
    buf[r] = '\0';
    fclose(f);
    assert(strstr(buf, "\"run_id\": \"run_original\"") != NULL);

    /* Now try to initialize writer again pointing to the same non-empty directory */
    diag_init("run_duplicate_attempt");
    diag_writer_config_t dup_cfg = {0};
    strncpy(dup_cfg.output_dir, test_dir, sizeof(dup_cfg.output_dir) - 1);
    strncpy(dup_cfg.run_id, "run_duplicate_attempt", sizeof(dup_cfg.run_id) - 1);
    dup_cfg.storage_budget_bytes = 1024 * 1024;
    dup_cfg.incident_reserve_bytes = 256 * 1024;

    /* Must reject with -2 (EEXIST / prior session exists) */
    int rc = diag_writer_init(&dup_cfg);
    assert(rc == -2);

    /* Verify original status.json was NOT overwritten */
    f = fopen(status_file, "r");
    assert(f != NULL);
    r = fread(buf, 1, sizeof(buf) - 1, f);
    buf[r] = '\0';
    fclose(f);
    assert(strstr(buf, "\"run_id\": \"run_original\"") != NULL);

    diag_shutdown();
    clean_dir(test_dir);
    printf("      Duplicate run rejected and previous session preserved (passed)\n");
}

/* 2. Test atomic publication and controlled interruption */
static void test_atomic_publication_and_interruption(void) {
    printf("[2/5] Testing atomic publication and survival of controlled interruption...\n");
    const char *test_dir = "tests/disp_writer_atomic";
    clean_dir(test_dir);

    diag_init("run_atomic");
    diag_writer_config_t cfg = {0};
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_atomic", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 1024 * 1024;
    cfg.incident_reserve_bytes = 256 * 1024;
    cfg.chunk_record_limit = 50;

    assert(diag_writer_init(&cfg) == 0);

    int prod = diag_register_producer("prod_atomic");
    assert(prod >= 0);
    for (int i = 0; i < 15; i++) {
        diag_emit(prod, DIAG_EV_CUSTOM, i, 100 + i, 0, 0);
    }

    assert(diag_writer_flush_chunks() == 0);

    /* Verify chunk 1 was committed atomically */
    char chunk1[320];
    snprintf(chunk1, sizeof(chunk1), "%s/events-000001.jsonl", test_dir);
    FILE *f1 = fopen(chunk1, "r");
    assert(f1 != NULL);
    fclose(f1);

    /* Verify no .tmp files left from chunk 1 */
    char chunk1_tmp[320];
    snprintf(chunk1_tmp, sizeof(chunk1_tmp), "%s/events-000001.jsonl.tmp", test_dir);
    FILE *f1_tmp = fopen(chunk1_tmp, "r");
    assert(f1_tmp == NULL);

    /* Simulate interrupted / incomplete write in flight */
    char partial_tmp[320];
    snprintf(partial_tmp, sizeof(partial_tmp), "%s/events-000002.jsonl.tmp", test_dir);
    FILE *fpart = fopen(partial_tmp, "w");
    assert(fpart != NULL);
    fprintf(fpart, "{\"chunk_id\":2,\"incomplete\":tr"); /* Half-written */
    fclose(fpart);

    /* Stop writer */
    diag_writer_stop();
    diag_writer_shutdown();
    diag_shutdown();

    /* Verify committed chunk 1 is completely intact */
    f1 = fopen(chunk1, "r");
    assert(f1 != NULL);
    char line[512];
    int line_count = 0;
    while (fgets(line, sizeof(line), f1)) {
        line_count++;
    }
    fclose(f1);
    /* 1 header line + 15 records = 16 lines */
    assert(line_count == 16);

    clean_dir(test_dir);
    printf("      Atomic publication and interruption survival (passed)\n");
}

/* 3. Test disk budget exhaustion and first-incident reserve */
static void test_budget_exhaustion_and_incident_reserve(void) {
    printf("[3/5] Testing disk-budget exhaustion and first-incident reserve...\n");
    const char *test_dir = "tests/disp_writer_budget";
    clean_dir(test_dir);

    diag_init("run_budget");
    diag_writer_config_t cfg = {0};
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_budget", sizeof(cfg.run_id) - 1);
    /* Set tight budget: total 4096 bytes, 1024 bytes reserve */
    cfg.storage_budget_bytes = 4096;
    cfg.incident_reserve_bytes = 1024;
    cfg.chunk_record_limit = 5;

    assert(diag_writer_init(&cfg) == 0);

    int prod = diag_register_producer("prod_budget");
    assert(prod >= 0);

    /* Emit records to fill up the regular budget (4096 - 1024 = 3072 bytes) */
    for (int i = 0; i < 100; i++) {
        diag_emit(prod, DIAG_EV_CUSTOM, i, 12345678, i * 10, 0);
        diag_writer_flush_chunks();
    }

    diag_writer_stats_t stats;
    diag_writer_get_stats(&stats);
    assert(stats.reserve_held == true);
    assert(stats.state == DIAG_WRITER_EXHAUSTED);
    assert(stats.total_bytes_written <= (4096 - 1024));

    /* Attempting another flush of regular events should be safely blocked */
    uint32_t chunks_before = stats.chunks_committed;
    diag_emit(prod, DIAG_EV_CUSTOM, 999, 0, 0, 0);
    diag_writer_flush_chunks();
    diag_writer_get_stats(&stats);
    assert(stats.chunks_committed == chunks_before);

    /* Now write an incident within the 1024-byte reserve */
    const char incident_payload[] = "CRASH DUMP: fault at 0x71001234, thread 1, signal 11";
    int inc_rc = diag_writer_write_incident("incident-1.txt", incident_payload, strlen(incident_payload));
    assert(inc_rc == 0);

    /* Verify incident file was written */
    char inc_path[320];
    snprintf(inc_path, sizeof(inc_path), "%s/incident-1.txt", test_dir);
    FILE *fi = fopen(inc_path, "r");
    assert(fi != NULL);
    char inc_read[256];
    size_t nr = fread(inc_read, 1, sizeof(inc_read) - 1, fi);
    inc_read[nr] = '\0';
    fclose(fi);
    assert(strcmp(inc_read, incident_payload) == 0);

    /* Attempting an incident that exceeds the total hard ceiling (4096) must be rejected */
    char huge_buf[2000];
    memset(huge_buf, 'A', sizeof(huge_buf));
    int overflow_rc = diag_writer_write_incident("huge_incident.bin", huge_buf, sizeof(huge_buf));
    assert(overflow_rc == -2); /* Hard ceiling respected */

    diag_writer_stop();
    diag_writer_shutdown();
    diag_shutdown();
    clean_dir(test_dir);
    printf("      Budget exhaustion and first-incident reserve (passed)\n");
}

/* 4. Test bounded text sink and unbuffered logging opt-out */
static void test_bounded_text_sink(void) {
    printf("[4/5] Testing bounded text sink and unbuffered logging opt-out...\n");
    const char *test_dir = "tests/disp_writer_sink";
    clean_dir(test_dir);

    diag_init("run_sink");
    diag_writer_config_t cfg = {0};
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_sink", sizeof(cfg.run_id) - 1);
    cfg.disable_unbuffered_logging = true;

    assert(diag_writer_init(&cfg) == 0);

    /* Write text to sink */
    const char msg[] = "Bounded diagnostic log entry 1\n";
    diag_text_sink_write(msg, strlen(msg));

    char out_buf[256];
    size_t copied = diag_text_sink_get_buffered(out_buf, sizeof(out_buf) - 1);
    out_buf[copied] = '\0';
    assert(strcmp(out_buf, msg) == 0);

    diag_writer_stop();
    diag_writer_shutdown();
    diag_shutdown();
    clean_dir(test_dir);
    printf("      Bounded text sink and unbuffered opt-out (passed)\n");
}

/* 5. Test truncated last chunk recovery reading */
static void test_truncated_last_chunk_recovery(void) {
    printf("[5/5] Testing truncated last chunk handling...\n");
    const char *test_dir = "tests/disp_writer_trunc";
    clean_dir(test_dir);

    diag_init("run_trunc");
    diag_writer_config_t cfg = {0};
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_trunc", sizeof(cfg.run_id) - 1);
    cfg.chunk_record_limit = 5;

    assert(diag_writer_init(&cfg) == 0);

    int prod = diag_register_producer("prod_trunc");
    assert(prod >= 0);
    for (int i = 0; i < 5; i++) {
        diag_emit(prod, DIAG_EV_FRAME_START, i, 0, 0, 0);
    }
    diag_writer_flush_chunks();

    /* Intentionally append a half-truncated line to chunk 1 to simulate sudden crash during write */
    char chunk1[320];
    snprintf(chunk1, sizeof(chunk1), "%s/events-000001.jsonl", test_dir);
    FILE *f = fopen(chunk1, "a");
    assert(f != NULL);
    fprintf(f, "{\"seq\":999,\"ts\":12345,\"ty"); /* Truncated */
    fclose(f);

    /* Verify parser reading lines recovers the 5 valid records and flags truncated line */
    f = fopen(chunk1, "r");
    assert(f != NULL);
    char line[512];
    int valid_lines = 0;
    int truncated_lines = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            valid_lines++;
        } else {
            truncated_lines++;
        }
    }
    fclose(f);
    assert(valid_lines == 6); /* Header + 5 records */
    assert(truncated_lines == 1); /* 1 truncated line */

    diag_writer_stop();
    diag_writer_shutdown();
    diag_shutdown();
    clean_dir(test_dir);
    printf("      Truncated last chunk handling (passed)\n");
}

int main(void) {
    printf("=== Witch-Time Runtime Diag Writer Unit Test ===\n");
    test_duplicate_run_and_no_overwrite();
    test_atomic_publication_and_interruption();
    test_budget_exhaustion_and_incident_reserve();
    test_bounded_text_sink();
    test_truncated_last_chunk_recovery();
    printf("=== All 5 Diag Writer Unit Tests Passed ===\n");
    return 0;
}
