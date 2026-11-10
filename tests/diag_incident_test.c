#include "../runtime/diag.h"
#include "../runtime/diag_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(_WIN32)
#include <windows.h>
static void clean_temp_dir(const char *dir) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", dir);
    system(cmd);
}
#else
#include <unistd.h>
static void clean_temp_dir(const char *dir) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
    system(cmd);
}
#endif

static char *read_file_to_string(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/* 1. Test short history prehistory capture and no fake pre-trigger screenshot claims */
static void test_short_history_prehistory_capture(void) {
    printf("[1/5] Testing short history prehistory capture and explicit coverage...\n");
    const char *test_dir = "test_incident_short_dir";
    clean_temp_dir(test_dir);

    diag_shutdown();
    diag_writer_shutdown();

    assert(diag_init("run_inc_short_01") == 0);

    diag_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_inc_short_01", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 10 * 1024 * 1024;
    cfg.incident_reserve_bytes = 2 * 1024 * 1024;
    cfg.chunk_record_limit = 100;
    cfg.flush_interval_ms = 0;
    assert(diag_writer_init(&cfg) == 0);

    /* Emit 5 events (short history) */
    int prod = diag_register_producer("gameplay");
    assert(prod >= 0);
    for (int i = 0; i < 5; i++) {
        diag_emit(prod, DIAG_EV_FRAME_START, i, 0, 0, 0);
    }
    assert(diag_writer_flush_chunks() == 0);

    /* Trigger incident */
    int inc1 = diag_writer_trigger_incident("stall", "SCENARIO_TIMEOUT", "Frame presentation hung", NULL, 0);
    assert(inc1 == 1);

    /* Verify incident.json was created and carries exact short event count */
    char *inc_json = read_file_to_string("test_incident_short_dir/incidents/incident-001/incident.json");
    assert(inc_json != NULL);
    assert(strstr(inc_json, "\"incident_id\": \"incident-001\"") != NULL);
    assert(strstr(inc_json, "\"trigger_type\": \"stall\"") != NULL);
    assert(strstr(inc_json, "\"reason_code\": \"SCENARIO_TIMEOUT\"") != NULL);
    assert(strstr(inc_json, "\"event_count\": 5") != NULL);
    /* Strict honest coverage: no fake pre-trigger screenshots claimed */
    assert(strstr(inc_json, "\"pre_trigger_screenshot\": \"none\"") != NULL);
    assert(strstr(inc_json, "\"post_trigger_coverage\": \"started\"") != NULL);
    free(inc_json);

    /* Verify prehistory.jsonl has 1 header + 5 record lines */
    char *pre_jsonl = read_file_to_string("test_incident_short_dir/incidents/incident-001/prehistory.jsonl");
    assert(pre_jsonl != NULL);
    assert(strstr(pre_jsonl, "\"incident_id\":1") != NULL);
    assert(strstr(pre_jsonl, "\"count\":5") != NULL);
    free(pre_jsonl);

    diag_writer_shutdown();
    diag_shutdown();
    clean_temp_dir(test_dir);
    printf("      Short history and explicit coverage verified (passed)\n");
}

/* 2. Test repeated storms, 3-capture ceiling, and first-occurrence preservation */
static void test_repeated_storms_and_three_capture_ceiling(void) {
    printf("[2/5] Testing repeated storms, 3-capture ceiling, and first-occurrence preservation...\n");
    const char *test_dir = "test_incident_storm_dir";
    clean_temp_dir(test_dir);

    diag_shutdown();
    diag_writer_shutdown();

    assert(diag_init("run_inc_storm_01") == 0);

    diag_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_inc_storm_01", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 10 * 1024 * 1024;
    cfg.incident_reserve_bytes = 2 * 1024 * 1024;
    cfg.chunk_record_limit = 100;
    cfg.flush_interval_ms = 0;
    assert(diag_writer_init(&cfg) == 0);

    /* Trigger 1: Original first occurrence */
    int inc1 = diag_writer_trigger_incident("stall", "FIRST_OCCURRENCE", "Initial stall event", NULL, 0);
    assert(inc1 == 1);

    /* Trigger 2 */
    int inc2 = diag_writer_trigger_incident("render_fault", "VK_DEVICE_LOST", "Second stall", NULL, 0);
    assert(inc2 == 2);

    /* Trigger 3 */
    int inc3 = diag_writer_trigger_incident("audio_underrun", "AUDIO_STARVED", "Third stall", NULL, 0);
    assert(inc3 == 3);

    /* Trigger 4 (Storm): MUST be rejected with -3 (max deep incidents reached) */
    int inc4 = diag_writer_trigger_incident("repeated_stall", "STORM_4", "Fourth stall", NULL, 0);
    assert(inc4 == -3);

    /* Trigger 5 (Storm): MUST also be rejected */
    int inc5 = diag_writer_trigger_incident("repeated_stall", "STORM_5", "Fifth stall", NULL, 0);
    assert(inc5 == -3);

    /* Verify incident-004 does NOT exist */
    char *inc4_json = read_file_to_string("test_incident_storm_dir/incidents/incident-004/incident.json");
    assert(inc4_json == NULL);

    /* Verify FIRST OCCURRENCE was strictly preserved and untouched */
    char *inc1_json = read_file_to_string("test_incident_storm_dir/incidents/incident-001/incident.json");
    assert(inc1_json != NULL);
    assert(strstr(inc1_json, "\"reason_code\": \"FIRST_OCCURRENCE\"") != NULL);
    assert(strstr(inc1_json, "Initial stall event") != NULL);
    free(inc1_json);

    diag_writer_stats_t stats;
    diag_writer_get_stats(&stats);
    assert(stats.incidents_written == 3);

    diag_writer_shutdown();
    diag_shutdown();
    clean_temp_dir(test_dir);
    printf("      3-capture ceiling and first-occurrence preservation verified (passed)\n");
}

/* 3. Test disk reserve and storage budget exhaustion */
static void test_incident_budget_exhaustion(void) {
    printf("[3/5] Testing incident disk budget exhaustion...\n");
    const char *test_dir = "test_incident_budget_dir";
    clean_temp_dir(test_dir);

    diag_shutdown();
    diag_writer_shutdown();

    assert(diag_init("run_inc_budget_01") == 0);

    /* Very small budget: 8 KiB */
    diag_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_inc_budget_01", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 8192;
    cfg.incident_reserve_bytes = 2048;
    cfg.chunk_record_limit = 100;
    cfg.flush_interval_ms = 0;
    assert(diag_writer_init(&cfg) == 0);

    /* Attempt to trigger with oversized aux data (16 KiB > 8 KiB budget) */
    char big_aux[16384];
    memset(big_aux, 0xAB, sizeof(big_aux));
    int res = diag_writer_trigger_incident("overflow", "STORAGE_EXHAUSTED", "Oversized capture", big_aux, sizeof(big_aux));

    /* Must reject with -2 (budget exhaustion) without writing or corrupting directory */
    assert(res == -2);

    diag_writer_shutdown();
    diag_shutdown();
    clean_temp_dir(test_dir);
    printf("      Incident budget exhaustion safety verified (passed)\n");
}

/* 4. Test linked recoveries under same session identity */
static void test_linked_recovery_preservation(void) {
    printf("[4/5] Testing linked recovery under original incident...\n");
    const char *test_dir = "test_incident_recovery_dir";
    clean_temp_dir(test_dir);

    diag_shutdown();
    diag_writer_shutdown();

    assert(diag_init("run_inc_rec_01") == 0);

    diag_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_inc_rec_01", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 10 * 1024 * 1024;
    cfg.incident_reserve_bytes = 2 * 1024 * 1024;
    cfg.chunk_record_limit = 100;
    cfg.flush_interval_ms = 0;
    assert(diag_writer_init(&cfg) == 0);

    /* Trigger incident */
    int inc1 = diag_writer_trigger_incident("stall", "FRAME_STALL", "Renderer stalled", NULL, 0);
    assert(inc1 == 1);

    /* Record linked recovery: frame rendering resumed */
    assert(diag_writer_record_recovery(1, "swapchain_recreated_frames_resumed", 1234567890ULL) == 0);

    /* Read incident.json and verify original failure evidence survives AND recovery is linked */
    char *inc_json = read_file_to_string("test_incident_recovery_dir/incidents/incident-001/incident.json");
    assert(inc_json != NULL);
    assert(strstr(inc_json, "\"trigger_type\": \"stall\"") != NULL);
    assert(strstr(inc_json, "\"reason_code\": \"FRAME_STALL\"") != NULL);
    assert(strstr(inc_json, "\"recovered\": true") != NULL);
    assert(strstr(inc_json, "\"reason\": \"swapchain_recreated_frames_resumed\"") != NULL);
    assert(strstr(inc_json, "\"resume_timestamp_ns\": 1234567890") != NULL);
    free(inc_json);

    diag_writer_shutdown();
    diag_shutdown();
    clean_temp_dir(test_dir);
    printf("      Linked recovery preservation verified (passed)\n");
}

/* 5. Test collector started after trigger */
static void test_collector_started_after_trigger(void) {
    printf("[5/5] Testing collector started after trigger...\n");
    const char *test_dir = "test_incident_col_dir";
    clean_temp_dir(test_dir);

    diag_shutdown();
    diag_writer_shutdown();

    assert(diag_init("run_inc_col_01") == 0);

    diag_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_inc_col_01", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 10 * 1024 * 1024;
    cfg.incident_reserve_bytes = 2 * 1024 * 1024;
    cfg.chunk_record_limit = 100;
    cfg.flush_interval_ms = 0;
    assert(diag_writer_init(&cfg) == 0);

    /* Trigger incident */
    int inc1 = diag_writer_trigger_incident("deep_trace", "TRACE_TRIGGER", "Perfetto collection requested", NULL, 0);
    assert(inc1 == 1);

    char *inc_json = read_file_to_string("test_incident_col_dir/incidents/incident-001/incident.json");
    assert(inc_json != NULL);
    assert(strstr(inc_json, "\"post_trigger_coverage\": \"started\"") != NULL);
    assert(strstr(inc_json, "\"collector_start_ns\":") != NULL);
    free(inc_json);

    diag_writer_shutdown();
    diag_shutdown();
    clean_temp_dir(test_dir);
    printf("      Collector started after trigger verified (passed)\n");
}

int main(void) {
    printf("=== Witch-Time Incident Triggering and Retention Test Harness ===\n");
    test_short_history_prehistory_capture();
    test_repeated_storms_and_three_capture_ceiling();
    test_incident_budget_exhaustion();
    test_linked_recovery_preservation();
    test_collector_started_after_trigger();
    printf("=== All 5 Incident Retention Tests Passed ===\n");
    return 0;
}
