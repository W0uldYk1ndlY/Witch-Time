#include "../runtime/diag.h"
#include "../runtime/diag_writer.h"
#include "../runtime/diag_shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

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

/* 1. Test background versus active stall and foreground recovery */
static void test_background_vs_active_stall(void) {
    printf("[1/5] Testing background vs active stall and foreground recovery...\n");
    const char *test_dir = "test_live_status_dir";
    clean_temp_dir(test_dir);

    diag_shutdown();
    diag_writer_shutdown();

    assert(diag_init("run_live_01") == 0);

    diag_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_live_01", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 10 * 1024 * 1024;
    cfg.incident_reserve_bytes = 2 * 1024 * 1024;
    cfg.chunk_record_limit = 100;
    cfg.flush_interval_ms = 0;
    assert(diag_writer_init(&cfg) == 0);

    /* Set active foreground lifecycle */
    diag_hook_lifecycle(DIAG_LIFECYCLE_RESUMED);
    diag_hook_surface_gen(1, 1920, 1080, 1);

    diag_live_state_t live;
    diag_get_live_state(&live);
    assert(live.is_foreground == true);
    assert(live.lifecycle_state == DIAG_LIFECYCLE_RESUMED);
    assert(live.surface_generation == 1);

    /* Render a frame */
    diag_hook_render_end(1, 50, 0, 1);
    diag_get_live_state(&live);
    assert(live.last_render_timestamp_ns > 0);

    assert(diag_writer_write_status() == 0);
    char *json1 = read_file_to_string("test_live_status_dir/status.json");
    assert(json1 != NULL);
    assert(strstr(json1, "\"is_foreground\": true") != NULL);
    assert(strstr(json1, "\"state\": \"resumed\"") != NULL);
    assert(strstr(json1, "\"surface_generation\": 1") != NULL);
    assert(strstr(json1, "\"expected_inactivity\": false") != NULL);
    free(json1);

    /* Transition to background (expected inactivity) */
    diag_hook_lifecycle(DIAG_LIFECYCLE_STOPPED);
    diag_get_live_state(&live);
    assert(live.is_foreground == false);
    assert(live.lifecycle_state == DIAG_LIFECYCLE_STOPPED);

    assert(diag_writer_write_status() == 0);
    char *json2 = read_file_to_string("test_live_status_dir/status.json");
    assert(json2 != NULL);
    assert(strstr(json2, "\"is_foreground\": false") != NULL);
    assert(strstr(json2, "\"state\": \"stopped\"") != NULL);
    assert(strstr(json2, "\"expected_inactivity\": true") != NULL);
    assert(strstr(json2, "\"is_active_stall\": false") != NULL);
    free(json2);

    /* Resume to foreground (recovery) */
    diag_hook_lifecycle(DIAG_LIFECYCLE_RESUMED);
    diag_hook_render_end(2, 60, 0, 1);
    diag_get_live_state(&live);
    assert(live.is_foreground == true);
    assert(live.lifecycle_state == DIAG_LIFECYCLE_RESUMED);

    assert(diag_writer_write_status() == 0);
    char *json3 = read_file_to_string("test_live_status_dir/status.json");
    assert(json3 != NULL);
    assert(strstr(json3, "\"is_foreground\": true") != NULL);
    assert(strstr(json3, "\"expected_inactivity\": false") != NULL);
    free(json3);

    diag_writer_shutdown();
    diag_shutdown();
    clean_temp_dir(test_dir);
    printf("      Background vs active stall and recovery verified (passed)\n");
}

/* 2. Test thermal observations, headroom polling limits, and unsupported NaN handling */
static void test_thermal_polling_and_nan_handling(void) {
    printf("[2/5] Testing thermal headroom polling limits and NaN handling...\n");
    diag_shutdown();
    assert(diag_init("run_thermal_01") == 0);

    /* First poll should succeed */
    assert(diag_hook_thermal(0.85f, DIAG_THERMAL_STATUS_LIGHT) == true);

    diag_live_state_t live;
    diag_get_live_state(&live);
    assert(fabsf(live.thermal_headroom - 0.85f) < 0.001f);
    assert(live.thermal_status == DIAG_THERMAL_STATUS_LIGHT);

    /* Immediate second poll (< 1 sec) must be rate-limited and return false */
    assert(diag_hook_thermal(0.90f, DIAG_THERMAL_STATUS_MODERATE) == false);

    /* Verify state was NOT overwritten by rate-limited call */
    diag_get_live_state(&live);
    assert(fabsf(live.thermal_headroom - 0.85f) < 0.001f);

    /* Test NaN handling (unsupported platform capability) */
    diag_reset_live_state();
    assert(diag_hook_thermal(NAN, DIAG_THERMAL_STATUS_UNSUPPORTED) == true);

    diag_get_live_state(&live);
    /* Must report unsupported sentinel (-1.0f) rather than NaN or 0.0 */
    assert(live.thermal_headroom == -1.0f);
    assert(live.thermal_status == DIAG_THERMAL_STATUS_UNSUPPORTED);

    diag_shutdown();
    printf("      Thermal rate limiting and unsupported NaN handling verified (passed)\n");
}

/* 3. Test semantic freshness and progress ages */
static void test_semantic_freshness_and_progress_ages(void) {
    printf("[3/5] Testing semantic freshness and progress ages...\n");
    const char *test_dir = "test_semantic_status_dir";
    clean_temp_dir(test_dir);

    assert(diag_init("run_semantic_01") == 0);

    diag_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_semantic_01", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 10 * 1024 * 1024;
    cfg.incident_reserve_bytes = 2 * 1024 * 1024;
    cfg.chunk_record_limit = 100;
    cfg.flush_interval_ms = 0;
    assert(diag_writer_init(&cfg) == 0);

    /* Publish semantic observation: Chapter 1, Verse 2, Difficulty 1, Area 3, movie false, draws 1500 */
    diag_publish_semantic_observation(1, 2, 1, 3, false, 1500);

    diag_live_state_t live;
    diag_get_live_state(&live);
    assert(live.has_semantic_obs == true);
    assert(live.last_chapter == 1);
    assert(live.last_verse == 2);
    assert(live.last_semantic_timestamp_ns > 0);

    assert(diag_writer_write_status() == 0);
    char *json = read_file_to_string("test_semantic_status_dir/status.json");
    assert(json != NULL);
    assert(strstr(json, "\"last_chapter\": 1") != NULL);
    assert(strstr(json, "\"last_verse\": 2") != NULL);
    assert(strstr(json, "\"semantic_freshness_ms\":") != NULL);
    free(json);

    diag_writer_shutdown();
    diag_shutdown();
    clean_temp_dir(test_dir);
    printf("      Semantic freshness and progress ages verified (passed)\n");
}

/* 4. Test surface generation increments */
static void test_surface_generation_tracking(void) {
    printf("[4/5] Testing surface generation recreation tracking...\n");
    assert(diag_init("run_surface_01") == 0);

    diag_hook_surface_gen(1, 1280, 720, 1);
    diag_live_state_t live;
    diag_get_live_state(&live);
    assert(live.surface_generation == 1);

    /* Recreate surface (orientation change or resize) */
    diag_hook_surface_gen(2, 1920, 1080, 1);
    diag_get_live_state(&live);
    assert(live.surface_generation == 2);

    diag_shutdown();
    printf("      Surface generation recreation tracking verified (passed)\n");
}

/* 5. Test recorder health reporting in status */
static void test_recorder_health_reporting(void) {
    printf("[5/5] Testing independent recorder health reporting in status...\n");
    const char *test_dir = "test_health_status_dir";
    clean_temp_dir(test_dir);

    assert(diag_init("run_health_01") == 0);

    diag_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.output_dir, test_dir, sizeof(cfg.output_dir) - 1);
    strncpy(cfg.run_id, "run_health_01", sizeof(cfg.run_id) - 1);
    cfg.storage_budget_bytes = 5 * 1024 * 1024;
    cfg.incident_reserve_bytes = 1 * 1024 * 1024;
    cfg.chunk_record_limit = 100;
    cfg.flush_interval_ms = 0;
    assert(diag_writer_init(&cfg) == 0);

    /* Write status and verify health fields */
    assert(diag_writer_write_status() == 0);
    char *json = read_file_to_string("test_health_status_dir/status.json");
    assert(json != NULL);
    assert(strstr(json, "\"healthy\": true") != NULL);
    assert(strstr(json, "\"state\": \"active\"") != NULL);
    assert(strstr(json, "\"remaining_budget\":") != NULL);
    assert(strstr(json, "\"reserve_held\": false") != NULL);
    free(json);

    diag_writer_shutdown();
    diag_shutdown();
    clean_temp_dir(test_dir);
    printf("      Recorder health reporting verified (passed)\n");
}

int main(void) {
    printf("=== Witch-Time Live Status and Lifecycle Test Harness ===\n");
    test_background_vs_active_stall();
    test_thermal_polling_and_nan_handling();
    test_semantic_freshness_and_progress_ages();
    test_surface_generation_tracking();
    test_recorder_health_reporting();
    printf("=== All 5 Live Status Tests Passed ===\n");
    return 0;
}
