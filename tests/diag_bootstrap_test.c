#define WT_DIAGNOSTIC_ENABLED 1
#include "../android/app/src/main/cpp/diag_bootstrap.h"
#include "../android/app/src/main/cpp/diag_bootstrap.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define rmdir_recursive(p) RemoveDirectoryA(p)
#else
#include <unistd.h>
#endif

static void test_diagnostic_variant_flag(void) {
    assert(diag_bootstrap_is_diagnostic_variant() == 1);
    printf("PASS: test_diagnostic_variant_flag\n");
}

static void test_validate_run_id(void) {
    /* Valid run IDs */
    assert(diag_bootstrap_validate_run_id("run_20261110_190000_a1b2") == 1);
    assert(diag_bootstrap_validate_run_id("test-run-1") == 1);
    assert(diag_bootstrap_validate_run_id("A") == 1);
    assert(diag_bootstrap_validate_run_id("0123456789_abcdef-ABCDEF") == 1);

    /* 64 chars is valid */
    char buf64[65];
    memset(buf64, 'a', 64);
    buf64[64] = '\0';
    assert(diag_bootstrap_validate_run_id(buf64) == 1);

    /* Invalid run IDs */
    assert(diag_bootstrap_validate_run_id(NULL) == 0);
    assert(diag_bootstrap_validate_run_id("") == 0);
    assert(diag_bootstrap_validate_run_id("..") == 0);
    assert(diag_bootstrap_validate_run_id("../escape") == 0);
    assert(diag_bootstrap_validate_run_id("escape/sub") == 0);
    assert(diag_bootstrap_validate_run_id("escape\\sub") == 0);
    assert(diag_bootstrap_validate_run_id("run.id") == 0);
    assert(diag_bootstrap_validate_run_id("run with spaces") == 0);
    assert(diag_bootstrap_validate_run_id("run$cmd") == 0);
    assert(diag_bootstrap_validate_run_id("run;rm") == 0);

    /* 65 chars is rejected (exceeds DIAG_BOOTSTRAP_MAX_RUN_ID_LEN) */
    char buf65[66];
    memset(buf65, 'b', 65);
    buf65[65] = '\0';
    assert(diag_bootstrap_validate_run_id(buf65) == 0);

    printf("PASS: test_validate_run_id\n");
}

static void test_sha256_known_vectors(void) {
    char hex[65];

    /* Test vector 1: empty string */
    assert(diag_bootstrap_compute_sha256((const uint8_t *)"", 0, hex) == 0);
    fprintf(stderr, "Computed empty sha256: %s\n", hex);
    assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    /* Test vector 2: "hello" */
    assert(diag_bootstrap_compute_sha256((const uint8_t *)"hello", 5, hex) == 0);
    assert(strcmp(hex, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824") == 0);

    /* Test vector 3: "abc" */
    assert(diag_bootstrap_compute_sha256((const uint8_t *)"abc", 3, hex) == 0);
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    printf("PASS: test_sha256_known_vectors\n");
}

static void test_parse_args(void) {
    char run_id[128] = {0};
    char digest[128] = {0};

    /* No diagnostic args */
    char *argv_none[] = { "witch-time", "--other-flag", "val" };
    assert(diag_bootstrap_parse_args(3, argv_none, run_id, sizeof(run_id), digest, sizeof(digest)) == 0);

    /* Valid diagnostic args */
    const char *test_digest = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
    char *argv_valid[] = { "witch-time", "--diag-run-id", "test_run_1", "--diag-digest", (char *)test_digest };
    assert(diag_bootstrap_parse_args(5, argv_valid, run_id, sizeof(run_id), digest, sizeof(digest)) == 1);
    assert(strcmp(run_id, "test_run_1") == 0);
    assert(strcmp(digest, test_digest) == 0);

    /* Incomplete args: missing value for --diag-run-id */
    char *argv_missing_val[] = { "witch-time", "--diag-run-id" };
    assert(diag_bootstrap_parse_args(2, argv_missing_val, run_id, sizeof(run_id), digest, sizeof(digest)) == DIAG_BOOTSTRAP_ERR_INVALID_ARGS);

    /* Malformed run_id with traversal */
    char *argv_traversal[] = { "witch-time", "--diag-run-id", "../escape", "--diag-digest", (char *)test_digest };
    assert(diag_bootstrap_parse_args(5, argv_traversal, run_id, sizeof(run_id), digest, sizeof(digest)) == DIAG_BOOTSTRAP_ERR_INVALID_ARGS);

    /* Malformed digest: too short */
    char *argv_short_digest[] = { "witch-time", "--diag-run-id", "test_run_1", "--diag-digest", "abc" };
    assert(diag_bootstrap_parse_args(5, argv_short_digest, run_id, sizeof(run_id), digest, sizeof(digest)) == DIAG_BOOTSTRAP_ERR_INVALID_ARGS);

    printf("PASS: test_parse_args\n");
}

static void write_test_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
}

static void test_build_validation(void) {
    const char *dir = "build/diag_build_test_session";
    const char *keys[] = {"variant", "target", "native_optimization"};
    const char *escaped_keys[] = {"var\\u0069ant", "targ\\u0065t", "native_optimizatio\\u006e"};
    const char *values[] = {"\"diagnostic\"", "\"android-arm64\"", "\"release\""};
    const char *errors[] = {"INVALID_BUILD_VARIANT", "INVALID_TARGET", "INVALID_NATIVE_OPTIMIZATION"};
    const char *spaces[] = {"", " ", "  ", "\t\r\n "};
    const char *invalid[] = {"null", "true", "7", "[]", "{}", "\"wrong\"", "\"diagnostic \"", "\"\\u0000\"", "\"\\u0080\"", "\"\\t\""};
    char json[2048], digest[65], reason[128];
    make_dir("build");
    make_dir(dir);
    /* Each case exercises the public digest-bound validation path. */
    for (size_t field = 0; field < 3; field++) {
        for (size_t n = 0; n < sizeof(invalid) / sizeof(invalid[0]) + 3; n++) {
            char members[1024] = "";
            for (size_t j = 0; j < 3; j++) {
                if (j == field && n == sizeof(invalid) / sizeof(invalid[0])) continue;
                size_t used = strlen(members);
                snprintf(members + used, sizeof(members) - used, "%s\"%s\":%s", used ? "," : "", keys[j],
                         j == field && n < sizeof(invalid) / sizeof(invalid[0]) ? invalid[n] : values[j]);
            }
            if (n > sizeof(invalid) / sizeof(invalid[0])) {
                size_t used = strlen(members);
                snprintf(members + used, sizeof(members) - used, ",\"%s\":%s",
                         n == sizeof(invalid) / sizeof(invalid[0]) + 2 ? escaped_keys[field] : keys[field], values[field]);
            }
            snprintf(json, sizeof(json), "{\"schema_version\":\"1.0\",\"run_id\":\"build_test\",\"build\":{%s},\"decoy\":{\"variant\":\"diagnostic\",\"target\":\"android-arm64\",\"native_optimization\":\"release\"}}", members);
            write_test_file("build/diag_build_test_session/request.json", json);
            diag_bootstrap_compute_sha256((const uint8_t *)json, strlen(json), digest);
            assert(diag_bootstrap_validate_request(dir, "build_test", digest, reason, sizeof(reason)) == DIAG_BOOTSTRAP_ERR_INVALID_JSON);
            assert(!strcmp(reason, errors[field]));
        }
    }
    const char *objects[] = {
        "{\"variant\":\"diagnostic\",\"target\":\"android-arm64\",\"native_optimization\":\"release\"}",
        "{\"var\\u0069ant\":\"diag\\u006eostic\",\"target\":\"android\\u002darm64\",\"native_optimization\":\"releas\\u0065\"}",
        "null", "[]", "\"diagnostic\"", "7", "{}"
    };
    for (size_t s = 0; s < sizeof(spaces) / sizeof(spaces[0]); s++) {
        for (size_t o = 0; o < sizeof(objects) / sizeof(objects[0]); o++) {
            char spaced[1024];
            size_t used = 0;
            for (const char *p = objects[o]; *p; p++) {
                spaced[used++] = *p;
                if (*p == ':' || *p == ',') {
                    size_t len = strlen(spaces[s]);
                    memcpy(spaced + used, spaces[s], len);
                    used += len;
                }
            }
            spaced[used] = 0;
            snprintf(json, sizeof(json), "{\"schema_version\":\"1.0\",\"run_id\":\"build_test\",\"bu\\u0069ld\" %s: %s%s}", spaces[s], spaces[s], spaced);
            write_test_file("build/diag_build_test_session/request.json", json);
            diag_bootstrap_compute_sha256((const uint8_t *)json, strlen(json), digest);
            assert(diag_bootstrap_validate_request(dir, "build_test", digest, reason, sizeof(reason)) == (o < 2 ? DIAG_BOOTSTRAP_OK : DIAG_BOOTSTRAP_ERR_INVALID_JSON));
        }
    }
    const char *bad_roots[] = {
        "\"decoy\":{\"build\":%s}",
        "\"build\":%s,\"build\":{}",
        "\"build\":{},\"build\":%s",
        "\"build\":%s,\"bu\\u0069ld\":%s"
    };
    for (size_t i = 0; i < sizeof(bad_roots) / sizeof(bad_roots[0]); i++) {
        char root[1024];
        snprintf(root, sizeof(root), bad_roots[i], objects[0], objects[0]);
        snprintf(json, sizeof(json), "{\"schema_version\":\"1.0\",\"run_id\":\"build_test\",%s}", root);
        write_test_file("build/diag_build_test_session/request.json", json);
        diag_bootstrap_compute_sha256((const uint8_t *)json, strlen(json), digest);
        assert(diag_bootstrap_validate_request(dir, "build_test", digest, reason, sizeof(reason)) == DIAG_BOOTSTRAP_ERR_INVALID_JSON);
        assert(!strcmp(reason, "INVALID_BUILD_VARIANT"));
    }
    printf("PASS: test_build_validation\n");
}

static void test_validate_request_and_ordering(void) {
    const char *tmp_dir = "build/diag_test_session";
    make_dir("build");
    make_dir(tmp_dir);

    const char *valid_req_json =
        "{\n"
        "  \"schema_version\": \"1.0\",\n"
        "  \"run_id\": \"sess_42\",\n"
        "  \"build\": {\n"
        "    \"variant\": \"diagnostic\",\n"
        "    \"target\": \"android-arm64\",\n"
        "    \"native_optimization\": \"release\"\n"
        "  },\n"
        "  \"scenario\": {\n"
        "    \"scenario_id\": \"combat_test\",\n"
        "    \"route_id\": \"route_1\",\n"
        "    \"fixture_id\": \"fixture_1\",\n"
        "    \"deadlines\": {\n"
        "      \"startup_deadline_secs\": 10.0,\n"
        "      \"scenario_deadline_secs\": 30.0,\n"
        "      \"recovery_deadline_secs\": 5.0\n"
        "    }\n"
        "  }\n"
        "}\n";

    char req_path[256];
    snprintf(req_path, sizeof(req_path), "%s/request.json", tmp_dir);
    write_test_file(req_path, valid_req_json);

    char valid_digest[65];
    diag_bootstrap_compute_sha256((const uint8_t *)valid_req_json, strlen(valid_req_json), valid_digest);

    char reason[128] = {0};

    /* 1. Happy path */
    int res = diag_bootstrap_validate_request(tmp_dir, "sess_42", valid_digest, reason, sizeof(reason));
    assert(res == DIAG_BOOTSTRAP_OK);

    /* 2. Digest mismatch */
    res = diag_bootstrap_validate_request(tmp_dir, "sess_42", "0000000000000000000000000000000000000000000000000000000000000000", reason, sizeof(reason));
    assert(res == DIAG_BOOTSTRAP_ERR_DIGEST_MISMATCH);
    assert(strcmp(reason, "REQUEST_DIGEST_MISMATCH") == 0);

    /* 3. Run ID mismatch */
    res = diag_bootstrap_validate_request(tmp_dir, "other_sess", valid_digest, reason, sizeof(reason));
    assert(res == DIAG_BOOTSTRAP_ERR_INVALID_JSON);
    assert(strcmp(reason, "RUN_ID_MISMATCH") == 0);

    /* 4. Path traversal attempt inside request */
    const char *traversal_json =
        "{\n"
        "  \"schema_version\": \"1.0\",\n"
        "  \"run_id\": \"sess_42\",\n"
        "  \"build\": { \"variant\": \"diagnostic\", \"target\": \"android-arm64\", \"native_optimization\": \"release\" },\n"
        "  \"scenario\": { \"scenario_id\": \"test\", \"route_id\": \"r\", \"fixture_id\": \"f\", \"save_fixture\": \"../../../etc/passwd\", \"deadlines\": { \"startup_deadline_secs\": 10.0 } }\n"
        "}\n";
    write_test_file(req_path, traversal_json);
    char trav_digest[65];
    diag_bootstrap_compute_sha256((const uint8_t *)traversal_json, strlen(traversal_json), trav_digest);
    res = diag_bootstrap_validate_request(tmp_dir, "sess_42", trav_digest, reason, sizeof(reason));
    assert(res == DIAG_BOOTSTRAP_ERR_TRAVERSAL);
    assert(strcmp(reason, "PATH_TRAVERSAL_REJECTED") == 0);

    /* 5. Non-finite number rejection */
    const char *nan_json =
        "{\n"
        "  \"schema_version\": \"1.0\",\n"
        "  \"run_id\": \"sess_42\",\n"
        "  \"build\": { \"variant\": \"diagnostic\", \"target\": \"android-arm64\", \"native_optimization\": \"release\" },\n"
        "  \"scenario\": { \"scenario_id\": \"test\", \"route_id\": \"r\", \"fixture_id\": \"f\", \"deadlines\": { \"startup_deadline_secs\": NaN } }\n"
        "}\n";
    write_test_file(req_path, nan_json);
    char nan_digest[65];
    diag_bootstrap_compute_sha256((const uint8_t *)nan_json, strlen(nan_json), nan_digest);
    res = diag_bootstrap_validate_request(tmp_dir, "sess_42", nan_digest, reason, sizeof(reason));
    assert(res == DIAG_BOOTSTRAP_ERR_INVALID_JSON);
    assert(strcmp(reason, "NON_FINITE_NUMBER_REJECTED") == 0);

    /* 6. Unsupported major schema rejection */
    const char *v2_json =
        "{\n"
        "  \"schema_version\": \"2.0\",\n"
        "  \"run_id\": \"sess_42\",\n"
        "  \"build\": { \"variant\": \"diagnostic\", \"target\": \"android-arm64\", \"native_optimization\": \"release\" },\n"
        "  \"scenario\": { \"scenario_id\": \"test\", \"route_id\": \"r\", \"fixture_id\": \"f\", \"deadlines\": { \"startup_deadline_secs\": 10.0 } }\n"
        "}\n";
    write_test_file(req_path, v2_json);
    char v2_digest[65];
    diag_bootstrap_compute_sha256((const uint8_t *)v2_json, strlen(v2_json), v2_digest);
    res = diag_bootstrap_validate_request(tmp_dir, "sess_42", v2_digest, reason, sizeof(reason));
    assert(res == DIAG_BOOTSTRAP_ERR_UNSUPPORTED_SCHEMA);
    assert(strcmp(reason, "UNSUPPORTED_SCHEMA_MAJOR") == 0);

    /* Typed selections are validated only after digest binding, then applied before dlopen. */
    char temporal_json[2048], temporal_digest[65];
    int prefix_len = (int)(strrchr(valid_req_json, '}') - valid_req_json);
    snprintf(temporal_json, sizeof(temporal_json), "%.*s,\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"sgsr2_quality\",\"variant\":\"bad\"},\"cohort\":\"equal_output\"}}}", prefix_len, valid_req_json);
    write_test_file(req_path, temporal_json);
    diag_bootstrap_compute_sha256((const uint8_t *)temporal_json, strlen(temporal_json), temporal_digest);
    assert(diag_bootstrap_validate_request(tmp_dir, "sess_42", temporal_digest, reason, sizeof(reason)) == DIAG_BOOTSTRAP_ERR_INVALID_JSON);
    assert(!strcmp(reason, "INVALID_TEMPORAL_TREATMENT"));
    snprintf(temporal_json, sizeof(temporal_json), "%.*s,\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"native\",\"mode|variant\":false},\"cohort\":\"equal_output\"}}}", prefix_len, valid_req_json);
    write_test_file(req_path, temporal_json);
    diag_bootstrap_compute_sha256((const uint8_t *)temporal_json, strlen(temporal_json), temporal_digest);
    assert(diag_bootstrap_validate_request(tmp_dir, "sess_42", temporal_digest, reason, sizeof(reason)) == DIAG_BOOTSTRAP_ERR_INVALID_JSON);
    assert(!strcmp(reason, "INVALID_TEMPORAL_TREATMENT"));
    snprintf(temporal_json, sizeof(temporal_json), "%.*s,\"profile\":{\"collectors\":[\"gpu_components_unsupported\"]}}", prefix_len, valid_req_json);
    write_test_file(req_path, temporal_json);
    diag_bootstrap_compute_sha256((const uint8_t *)temporal_json, strlen(temporal_json), temporal_digest);
    assert(diag_bootstrap_validate_request(tmp_dir, "sess_42", temporal_digest, reason, sizeof(reason)) == DIAG_BOOTSTRAP_ERR_INVALID_JSON);
    assert(!strcmp(reason, "INVALID_DIAGNOSTIC_COLLECTOR"));
    snprintf(temporal_json, sizeof(temporal_json), "%.*s,\"profile\":{\"collectors\":[\"gpu_components\"]},\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"sgsr2_quality\",\"variant\":\"2_pass_cs\"},\"cohort\":\"equal_output\"}}}", prefix_len, valid_req_json);
    write_test_file(req_path, temporal_json);
    diag_bootstrap_compute_sha256((const uint8_t *)temporal_json, strlen(temporal_json), temporal_digest);
    assert(diag_bootstrap_validate_request(tmp_dir, "sess_42", temporal_digest, reason, sizeof(reason)) == DIAG_BOOTSTRAP_OK);

    /* 7. Startup ordering: write acknowledgement before dlopen */
    res = diag_bootstrap_write_acknowledgement(tmp_dir, "sess_42", temporal_digest);
    assert(res == DIAG_BOOTSTRAP_OK);
    char manifest_path[256];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", tmp_dir);
    FILE *mf = fopen(manifest_path, "rb");
    assert(mf != NULL);
    char mf_buf[512] = {0};
    size_t nr1 = fread(mf_buf, 1, sizeof(mf_buf) - 1, mf);
    (void)nr1;
    fclose(mf);
    assert(strstr(mf_buf, "\"status\": \"acknowledged\"") != NULL);
    assert(strstr(mf_buf, "\"run_id\": \"sess_42\"") != NULL);
    assert(strstr(mf_buf, "\"startup_phase\": \"bootstrap_pre_dlopen\"") != NULL);
    assert(strstr(mf_buf, temporal_digest) != NULL);
    assert(strstr(mf_buf, "effective_treatments") == NULL); /* Receipt is not execution evidence. */

    /* 8. Rejection outcome write */
    res = diag_bootstrap_write_outcome_rejection(tmp_dir, "sess_42", "REQUEST_VALIDATION_FAILED", "Digest mismatch");
    assert(res == DIAG_BOOTSTRAP_OK);
    char outcome_path[256];
    snprintf(outcome_path, sizeof(outcome_path), "%s/outcome.json", tmp_dir);
    FILE *of = fopen(outcome_path, "rb");
    assert(of != NULL);
    char of_buf[512] = {0};
    size_t nr2 = fread(of_buf, 1, sizeof(of_buf) - 1, of);
    (void)nr2;
    fclose(of);
    assert(strstr(of_buf, "\"status\": \"rejected\"") != NULL);
    assert(strstr(of_buf, "\"reason\": \"REQUEST_VALIDATION_FAILED\"") != NULL);

    /* 9. Session environment isolation */
    set_env_var("WT_TRACE", "outside_run_trace_must_not_be_used.txt");
    res = diag_bootstrap_apply_session_environment(tmp_dir, "sess_42");
    assert(res == DIAG_BOOTSTRAP_OK);
    assert(!strcmp(getenv("WT_TRACE"), "build/diag_test_session/boot_trace.txt"));
    assert(fopen(getenv("WT_TRACE"), "rb") == NULL); /* Bootstrap only selects the path. */
    char oversized_dir[512];
    memset(oversized_dir, 'x', sizeof(oversized_dir) - 1);
    oversized_dir[sizeof(oversized_dir) - 1] = 0;
    assert(diag_bootstrap_apply_session_environment(oversized_dir, "sess_42") == DIAG_BOOTSTRAP_ERR_INVALID_ARGS);
    assert(!strcmp(getenv("WT_TRACE"), "build/diag_test_session/boot_trace.txt"));
    const char *ud = getenv("WT_USERDATA");
    assert(ud != NULL && strstr(ud, "userdata") != NULL);
    const char *sc = getenv("WT_SHADER_CACHE");
    assert(sc != NULL && strstr(sc, "shaders") != NULL);
    const char *vc = getenv("WT_VK_CACHE");
    assert(vc != NULL && strstr(vc, "vk") != NULL);
    assert(!strcmp(getenv("WT_UPSCALER"), "sgsr2_quality"));
    assert(!strcmp(getenv("WT_SGSR_VARIANT"), "2-pass-cs"));
    assert(!strcmp(getenv("WT_VK_GPUTIME"), "1") && !strcmp(getenv("WT_VK_PASSTIME"), "1"));
    assert(diag_parse_collectors("{\"profile\":{\"collectors\":[\"gpu_timing\"]}}") == 0);
    assert(diag_bootstrap_apply_session_environment(tmp_dir, "sess_42") == 0);
    assert(!strcmp(getenv("WT_VK_GPUTIME"), "1") && !strcmp(getenv("WT_VK_PASSTIME"), "0"));
    assert(diag_parse_collectors("{\"profile\":{\"collectors\":[]}}") == 0);
    set_env_var("WT_TRACE", "stale_previous_run/boot_trace.txt");
    assert(diag_bootstrap_apply_session_environment(tmp_dir, "sess_42") == 0);
    assert(!strcmp(getenv("WT_TRACE"), "build/diag_test_session/boot_trace.txt"));
    assert(!strcmp(getenv("WT_VK_GPUTIME"), "0") && !strcmp(getenv("WT_VK_PASSTIME"), "0"));

    printf("PASS: test_validate_request_and_ordering\n");
}

static void test_temporal_parser(void) {
    const char *bad[] = {
        "{\"treatments\":nulljunk}",
        "{\"treatments\":{\"temporal\":nulljunk}}",
        "{\"treatments\":{},\"treatments\":{}}",
        "{\"treatments\":{\"temporal\":{}}}",
        "{\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"sgsr2\"},\"cohort\":\"equal_output\"}}}",
        "{\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"sgsr2\",\"variant\":\"unknown\"},\"cohort\":\"equal_output\"}}}",
        "{\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"fsr3\",\"variant\":\"2_pass_cs\"},\"cohort\":\"equal_output\"}}}",
        "{\"treatments\":{\"temporal\":{\"selection\":{\"mode\":7},\"cohort\":\"equal_output\"}}}",
        "{\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"native\",\"mode\":\"fsr3\"},\"cohort\":\"equal_output\"}}}",
        "{\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"native\",\"\":1},\"cohort\":\"equal_output\"}}}",
        "{\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"native\",\"mode|variant\":false},\"cohort\":\"equal_output\"}}}",
        "{\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"native\",\"mode\\u007cvariant\":false},\"cohort\":\"equal_output\"}}}",
        "{\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"native\"},\"cohort\":\"pooled\"}}}",
        "{\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"native\"},\"cohort\":\"equal_output\",\"env\":{}}}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(diag_parse_temporal(bad[i]) == -1);
        assert(diag_temporal_mode == NULL);
    }
    const char *modes[] = {"native", "fsr3", "fsr3_quality", "fsr3_balanced", "fsr3_performance", "fsr3_ultra_performance", "sgsr2", "sgsr2_quality", "sgsr2_performance"};
    const char *variants[] = {"2_pass_fs", "2_pass_cs", "3_pass_cs"};
    for (int i = 0; i < 9; i++) for (int j = 0; j < (i < 6 ? 1 : 3); j++) {
        char json[512], variant[80] = "";
        if (i >= 6) snprintf(variant, sizeof(variant), ",\"variant\":\"%s\"", variants[j]);
        snprintf(json, sizeof(json), "{\"treatments\":{\"temporal\":{\"selection\":{\"mode\":\"%s\"%s},\"cohort\":\"equal_render\"}}}", modes[i], variant);
        assert(diag_parse_temporal(json) == 0);
        assert(!strcmp(diag_temporal_mode, i ? modes[i] : "off"));
    }
    assert(diag_parse_temporal("{\"treatments\":{\"temp\\u006fral\":{\"selection\":{\"mode\":\"native\"},\"cohort\":\"equal_output\"}}}") == 0);
    assert(!strcmp(diag_temporal_mode, "off"));
    assert(diag_parse_temporal("{}") == 0 && diag_temporal_mode == NULL);
    assert(diag_parse_temporal("{\"treatments\":null}") == 0 && diag_temporal_mode == NULL);
    printf("PASS: test_temporal_parser\n");
}

static void test_collector_parser(void) {
    const char *bad[] = {
        "{\"profile\":{\"collectors\":true}}",
        "{\"profile\":{\"collectors\":[true]}}",
        "{\"profile\":{\"collectors\":[\"gpu_components_unsupported\"]}}",
        "{\"profile\":{\"collectors\":[\"gpu_components\",\"gpu_components\"]}}",
        "{\"profile\":{\"collectors\":[{\"WT_VK_PASSTIME\":1}]}}",
        "{\"profile\":{\"collectors\":[\"gpu_components\",]}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(diag_parse_collectors(bad[i]) == -1);
        assert(!diag_collectors_present);
    }
    assert(diag_parse_collectors("{}") == 0 && !diag_collectors_present);
    assert(diag_parse_collectors("{\"profile\":{\"collectors\":[\"gpu_timing\"]}}") == 0);
    assert(diag_collectors_present && diag_gpu_timing && !diag_gpu_components);
    assert(diag_parse_collectors("{\"profile\":{\"collectors\":[\"gpu_compon\\u0065nts\"]}}") == 0);
    assert(diag_collectors_present && diag_gpu_components);
    printf("PASS: test_collector_parser\n");
}

int main(void) {
    printf("=== Starting diag_bootstrap_test ===\n");
    test_diagnostic_variant_flag();
    test_validate_run_id();
    test_sha256_known_vectors();
    test_parse_args();
    test_temporal_parser();
    test_collector_parser();
    test_build_validation();
    test_validate_request_and_ordering();
    printf("=== ALL DIAG BOOTSTRAP TESTS PASSED ===\n");
    return 0;
}
