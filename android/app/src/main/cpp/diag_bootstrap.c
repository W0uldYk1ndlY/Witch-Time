#include "diag_bootstrap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/* -------------------------------------------------------------------------------------------------
 * Portable FIPS 180-2 SHA-256 Implementation
 * ------------------------------------------------------------------------------------------------- */

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} Sha256Ctx;

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(Sha256Ctx *ctx, const uint8_t *data) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];
    for (int i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + K256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(Sha256Ctx *ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void sha256_update(Sha256Ctx *ctx, const uint8_t *data, size_t len) {
    size_t i = (size_t)(ctx->count & 0x3f);
    ctx->count += len;
    size_t part_len = 64 - i;
    size_t idx = 0;
    if (len >= part_len) {
        memcpy(&ctx->buffer[i], data, part_len);
        sha256_transform(ctx, ctx->buffer);
        for (idx = part_len; idx + 63 < len; idx += 64) {
            sha256_transform(ctx, &data[idx]);
        }
        i = 0;
    }
    if (idx < len) {
        memcpy(&ctx->buffer[i], &data[idx], len - idx);
    }
}

static void sha256_final(Sha256Ctx *ctx, uint8_t digest[32]) {
    uint8_t bits[8];
    uint64_t total_bits = ctx->count * 8;
    for (int i = 0; i < 8; i++) {
        bits[7 - i] = (uint8_t)(total_bits >> (i * 8));
    }
    size_t i = (size_t)(ctx->count & 0x3f);
    size_t pad_len = (i < 56) ? (56 - i) : (120 - i);
    static const uint8_t padding[64] = { 0x80 };
    sha256_update(ctx, padding, pad_len);
    sha256_update(ctx, bits, 8);
    for (int j = 0; j < 8; j++) {
        digest[j * 4] = (uint8_t)(ctx->state[j] >> 24);
        digest[j * 4 + 1] = (uint8_t)(ctx->state[j] >> 16);
        digest[j * 4 + 2] = (uint8_t)(ctx->state[j] >> 8);
        digest[j * 4 + 3] = (uint8_t)(ctx->state[j]);
    }
}

int diag_bootstrap_compute_sha256(const uint8_t *data, size_t len, char *out_hex) {
    if (!data && len > 0) return -1;
    if (!out_hex) return -1;
    Sha256Ctx ctx;
    sha256_init(&ctx);
    if (len > 0) {
        sha256_update(&ctx, data, len);
    }
    uint8_t hash[32];
    sha256_final(&ctx, hash);
    for (int i = 0; i < 32; i++) {
        sprintf(&out_hex[i * 2], "%02x", hash[i]);
    }
    out_hex[64] = '\0';
    return 0;
}

/* -------------------------------------------------------------------------------------------------
 * File and Environment Helpers
 * ------------------------------------------------------------------------------------------------- */

static int make_dir(const char *path) {
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static void set_env_var(const char *key, const char *val) {
#if defined(_WIN32)
    _putenv_s(key, val);
#else
    setenv(key, val, 1);
#endif
}

/* -------------------------------------------------------------------------------------------------
 * Bootstrap API Implementation
 * ------------------------------------------------------------------------------------------------- */

int diag_bootstrap_is_diagnostic_variant(void) {
#if defined(WT_DIAGNOSTIC_ENABLED) && (WT_DIAGNOSTIC_ENABLED != 0)
    return 1;
#else
    return 0;
#endif
}

int diag_bootstrap_validate_run_id(const char *run_id) {
    if (!run_id) return 0;
    size_t len = strlen(run_id);
    if (len == 0 || len > DIAG_BOOTSTRAP_MAX_RUN_ID_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = run_id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

int diag_bootstrap_parse_args(int argc, char **argv, char *out_run_id, size_t run_id_cap, char *out_digest, size_t digest_cap) {
    if (!argv || argc < 1) return 0;
    const char *found_run_id = NULL;
    const char *found_digest = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--diag-run-id") == 0) {
            if (i + 1 < argc) {
                found_run_id = argv[++i];
            } else {
                return DIAG_BOOTSTRAP_ERR_INVALID_ARGS;
            }
        } else if (strcmp(argv[i], "--diag-digest") == 0) {
            if (i + 1 < argc) {
                found_digest = argv[++i];
            } else {
                return DIAG_BOOTSTRAP_ERR_INVALID_ARGS;
            }
        }
    }
    if (!found_run_id && !found_digest) return 0;
    if (!found_run_id || !found_digest) return DIAG_BOOTSTRAP_ERR_INVALID_ARGS;
    if (!diag_bootstrap_validate_run_id(found_run_id)) return DIAG_BOOTSTRAP_ERR_INVALID_ARGS;
    if (strlen(found_digest) != DIAG_BOOTSTRAP_SHA256_HEX_LEN) return DIAG_BOOTSTRAP_ERR_INVALID_ARGS;
    for (size_t i = 0; i < DIAG_BOOTSTRAP_SHA256_HEX_LEN; i++) {
        char c = found_digest[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return DIAG_BOOTSTRAP_ERR_INVALID_ARGS;
        }
    }
    if (out_run_id && run_id_cap > strlen(found_run_id)) {
        strncpy(out_run_id, found_run_id, run_id_cap - 1);
        out_run_id[run_id_cap - 1] = '\0';
    }
    if (out_digest && digest_cap > strlen(found_digest)) {
        strncpy(out_digest, found_digest, digest_cap - 1);
        out_digest[digest_cap - 1] = '\0';
    }
    return 1;
}

static int str_contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

int diag_bootstrap_validate_request(const char *run_dir, const char *run_id, const char *expected_digest, char *out_error_reason, size_t error_cap) {
    if (!run_dir || !run_id || !expected_digest) {
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "NULL_ARGUMENT");
        return DIAG_BOOTSTRAP_ERR_INVALID_ARGS;
    }

    /* Check run_id characters */
    if (!diag_bootstrap_validate_run_id(run_id)) {
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "INVALID_RUN_ID");
        return DIAG_BOOTSTRAP_ERR_INVALID_ARGS;
    }

    char req_path[512];
    snprintf(req_path, sizeof(req_path), "%s/request.json", run_dir);

    FILE *f = fopen(req_path, "rb");
    if (!f) {
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "REQUEST_FILE_NOT_FOUND");
        return DIAG_BOOTSTRAP_ERR_IO;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > DIAG_BOOTSTRAP_MAX_REQUEST_BYTES) {
        fclose(f);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "EXCESSIVE_OR_EMPTY_PAYLOAD_SIZE");
        return DIAG_BOOTSTRAP_ERR_OVERSIZED;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "OUT_OF_MEMORY");
        return DIAG_BOOTSTRAP_ERR_IO;
    }

    size_t read_bytes = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[read_bytes] = '\0';

    /* Verify request digest */
    char computed_digest[DIAG_BOOTSTRAP_SHA256_HEX_LEN + 1];
    diag_bootstrap_compute_sha256((const uint8_t *)buf, read_bytes, computed_digest);

    #if defined(_WIN32)
    if (_stricmp(computed_digest, expected_digest) != 0) {
    #else
    if (strcasecmp(computed_digest, expected_digest) != 0) {
    #endif
        free(buf);
        if (out_error_reason && error_cap > 0) {
            snprintf(out_error_reason, error_cap, "REQUEST_DIGEST_MISMATCH");
        }
        return DIAG_BOOTSTRAP_ERR_DIGEST_MISMATCH;
    }

    /* Traversal and symlink escape check */
    if (str_contains(buf, "..") || str_contains(buf, "\\..") || str_contains(buf, "/..")) {
        free(buf);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "PATH_TRAVERSAL_REJECTED");
        return DIAG_BOOTSTRAP_ERR_TRAVERSAL;
    }

    /* Non-finite number rejection */
    if (str_contains(buf, "NaN") || str_contains(buf, "Infinity") || str_contains(buf, "-Infinity")) {
        free(buf);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "NON_FINITE_NUMBER_REJECTED");
        return DIAG_BOOTSTRAP_ERR_INVALID_JSON;
    }

    /* Schema version check */
    if (!str_contains(buf, "\"schema_version\"")) {
        free(buf);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "MISSING_SCHEMA_VERSION");
        return DIAG_BOOTSTRAP_ERR_UNSUPPORTED_SCHEMA;
    }
    if (str_contains(buf, "\"schema_version\": \"2.") || str_contains(buf, "\"schema_version\":\"2.") ||
        str_contains(buf, "\"schema_version\": \"0.") || str_contains(buf, "\"schema_version\":\"0.")) {
        free(buf);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "UNSUPPORTED_SCHEMA_MAJOR");
        return DIAG_BOOTSTRAP_ERR_UNSUPPORTED_SCHEMA;
    }

    /* Run ID match check */
    if (!str_contains(buf, run_id)) {
        free(buf);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "RUN_ID_MISMATCH");
        return DIAG_BOOTSTRAP_ERR_INVALID_JSON;
    }

    /* Build allowlist check: variant diagnostic, target android-arm64, native_optimization release */
    if (!str_contains(buf, "\"variant\": \"diagnostic\"") && !str_contains(buf, "\"variant\":\"diagnostic\"")) {
        free(buf);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "INVALID_BUILD_VARIANT");
        return DIAG_BOOTSTRAP_ERR_INVALID_JSON;
    }
    if (!str_contains(buf, "\"target\": \"android-arm64\"") && !str_contains(buf, "\"target\":\"android-arm64\"")) {
        free(buf);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "INVALID_TARGET");
        return DIAG_BOOTSTRAP_ERR_INVALID_JSON;
    }
    if (!str_contains(buf, "\"native_optimization\": \"release\"") && !str_contains(buf, "\"native_optimization\":\"release\"")) {
        free(buf);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "INVALID_NATIVE_OPTIMIZATION");
        return DIAG_BOOTSTRAP_ERR_INVALID_JSON;
    }

    free(buf);
    return DIAG_BOOTSTRAP_OK;
}

int diag_bootstrap_write_acknowledgement(const char *run_dir, const char *run_id, const char *digest) {
    if (!run_dir || !run_id || !digest) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/manifest.json", run_dir);

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    fprintf(f, "{\n"
               "  \"schema_version\": \"1.0\",\n"
               "  \"run_id\": \"%s\",\n"
               "  \"request_digest\": \"%s\",\n"
               "  \"status\": \"acknowledged\",\n"
               "  \"startup_phase\": \"bootstrap_pre_dlopen\"\n"
               "}\n",
            run_id, digest);

    fflush(f);
    fclose(f);
    return DIAG_BOOTSTRAP_OK;
}

int diag_bootstrap_write_outcome_rejection(const char *run_dir, const char *run_id, const char *reason, const char *details) {
    if (!run_dir || !run_id || !reason) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/outcome.json", run_dir);

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    fprintf(f, "{\n"
               "  \"schema_version\": \"1.0\",\n"
               "  \"run_id\": \"%s\",\n"
               "  \"status\": \"rejected\",\n"
               "  \"reason\": \"%s\",\n"
               "  \"details\": \"%s\"\n"
               "}\n",
            run_id, reason, details ? details : "");

    fflush(f);
    fclose(f);
    return DIAG_BOOTSTRAP_OK;
}

int diag_bootstrap_apply_session_environment(const char *run_dir, const char *run_id) {
    if (!run_dir || !run_id) return -1;

    char subpath[512];
    make_dir(run_dir);

    snprintf(subpath, sizeof(subpath), "%s/userdata", run_dir);
    make_dir(subpath);
    set_env_var("WT_USERDATA", subpath);

    snprintf(subpath, sizeof(subpath), "%s/cache", run_dir);
    make_dir(subpath);

    snprintf(subpath, sizeof(subpath), "%s/cache/shaders", run_dir);
    make_dir(subpath);
    set_env_var("WT_SHADER_CACHE", subpath);

    snprintf(subpath, sizeof(subpath), "%s/cache/vk", run_dir);
    make_dir(subpath);
    set_env_var("WT_VK_CACHE", subpath);

    snprintf(subpath, sizeof(subpath), "%s/config", run_dir);
    make_dir(subpath);

    snprintf(subpath, sizeof(subpath), "%s/config/controllers.ini", run_dir);
    set_env_var("WT_CONFIG", subpath);

    snprintf(subpath, sizeof(subpath), "%s/config/graphics.ini", run_dir);
    set_env_var("WT_GRAPHICS_CONFIG", subpath);

    snprintf(subpath, sizeof(subpath), "%s/artifacts", run_dir);
    make_dir(subpath);

    set_env_var("WT_DIAG_RUN_ID", run_id);
    set_env_var("WT_DIAG_DIR", run_dir);
    set_env_var("WT_ROMFS", "targets/romfs");
    set_env_var("WT_MODS", "none");
    set_env_var("WT_VOLUME", "100");

    /* Redirect output to session.log */
    snprintf(subpath, sizeof(subpath), "%s/session.log", run_dir);
    freopen(subpath, "a", stdout);
    freopen(subpath, "a", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    return DIAG_BOOTSTRAP_OK;
}
