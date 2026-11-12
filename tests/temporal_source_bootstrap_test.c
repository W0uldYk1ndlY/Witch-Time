#include "../android/app/src/main/cpp/diag_bootstrap.c"
#include <assert.h>
int main(int argc, char **argv) {
    assert(argc == 2);
    const char *profiles[] = {"validation", "standard", "lightweight"};
    for (unsigned i = 0; i < 3; i++) {
        char request[1024], file[1200], digest[65], error[128];
        int n = snprintf(request, sizeof request,
            "{\"schema_version\":\"1.0\",\"run_id\":\"capture_test\","
            "\"build\":{\"variant\":\"diagnostic\",\"target\":\"android-arm64\",\"native_optimization\":\"release\"},"
            "\"profile\":{\"profile_name\":\"%s\",\"collectors\":[\"temporal_source_capture\"]}}", profiles[i]);
        snprintf(file, sizeof file, "%s/request.json", argv[1]);
        FILE *f = fopen(file, "wb"); assert(f); assert(fwrite(request, 1, n, f) == (size_t)n); fclose(f);
        diag_bootstrap_compute_sha256((const uint8_t *)request, n, digest);
        int result = diag_bootstrap_validate_request(argv[1], "capture_test", digest, error, sizeof error);
        assert((result == 0) == (i == 0));
        assert(diag_bootstrap_temporal_source_requested() == (i == 0 && diag_bootstrap_is_diagnostic_variant()));
        digest[0] = digest[0] == '0' ? '1' : '0';
        assert(diag_bootstrap_validate_request(argv[1], "capture_test", digest, error, sizeof error) != 0);
        assert(!diag_bootstrap_temporal_source_requested());
    }
    assert(diag_parse_collectors("{\"profile\":{\"profile_name\":\"validation\",\"collectors\":[\"temporal_source_capture\",\"temporal_source_capture\"]}}") == -1);
    return 0;
}
