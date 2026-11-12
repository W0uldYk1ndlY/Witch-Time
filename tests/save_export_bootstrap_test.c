#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "diag_bootstrap.h"

static int validate(const char *profile, const char *extra, const char *id) {
    char json[4096], digest[65];
    snprintf(json, sizeof json,
        "{\"schema_version\":\"1.0\",\"run_id\":\"%s\","
        "\"build\":{\"variant\":\"diagnostic\",\"target\":\"android-arm64\",\"native_optimization\":\"release\"},"
        "\"profile\":%s%s}", id, profile, extra);
    assert(!diag_bootstrap_compute_sha256((const uint8_t *)json, strlen(json), digest));
    return diag_bootstrap_validate_request_data((const uint8_t *)json, strlen(json), "run_test", digest, NULL, 0);
}
int main(void) {
#if defined(_WIN32)
    assert(!_putenv_s("WT_TRACE", "unchanged_trace_sentinel"));
#else
    assert(!setenv("WT_TRACE", "unchanged_trace_sentinel", 1));
#endif
    const char *valid = "{\"profile_name\":\"committed_save_export\",\"collectors\":[]}";
    if (!diag_bootstrap_is_diagnostic_variant()) {
        assert(!validate(valid, "", "run_test"));
        assert(!diag_bootstrap_save_export_requested());
        return 0;
    }
    assert(!validate(valid, "", "run_test"));
    assert(diag_bootstrap_save_export_requested());
    assert(diag_bootstrap_apply_session_environment("export_must_not_create_game_dirs", "run_test") != 0);
    assert(!strcmp(getenv("WT_TRACE"), "unchanged_trace_sentinel"));
    assert(validate(valid, ",\"source\":\"userdata\"", "run_test") != 0);
    assert(!diag_bootstrap_save_export_requested());
    assert(!strcmp(getenv("WT_TRACE"), "unchanged_trace_sentinel"));
    assert(validate(valid, ",\"run_id\":\"run_test\"", "run_test") != 0);
    assert(validate(valid, ",\"treatments\":null", "run_test") != 0);
    assert(validate(valid, ",\"created_utc\":garbage", "run_test") != 0);
    assert(validate(valid, ",\"created_utc\":123", "run_test") != 0);
    assert(!validate(valid, ",\"created_utc\":\"2026-11-12\"", "run_test"));
    assert(validate(valid, "", "not_run_test") != 0);
    assert(validate("{\"profile_name\":\"committed_save_export\",\"collectors\":[\"events\"]}", "", "run_test") != 0);
    assert(validate("{\"profile_name\":\"committed_save_export\",\"collectors\":[] ,\"collectors\":[]}", "", "run_test") != 0);
    assert(!validate("{\"profile_name\":\"lightweight\",\"collectors\":[]}", "", "run_test"));
    assert(!diag_bootstrap_save_export_requested());
    assert(!strcmp(getenv("WT_TRACE"), "unchanged_trace_sentinel"));
    assert(!validate(valid, "", "run_test"));
    assert(diag_bootstrap_validate_request_data(NULL, 0, "run_test", "", NULL, 0) != 0);
    assert(!diag_bootstrap_save_export_requested());
    return 0;
}
