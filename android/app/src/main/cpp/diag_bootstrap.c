#include "diag_bootstrap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
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

#if !defined(_WIN32)
#include <sys/stat.h>
#include <dirent.h>
#endif

/* -------------------------------------------------------------------------------------------------
 * File and Environment Helpers
 * ------------------------------------------------------------------------------------------------- */

#if !defined(_WIN32)
static void make_accessible_recursive(const char *dir_path) {
    if (!dir_path) return;
    chmod(dir_path, 0777);
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *de;
    char sub[512];
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(sub, sizeof(sub), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (stat(sub, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                chmod(sub, 0777);
                make_accessible_recursive(sub);
            } else {
                chmod(sub, 0666);
            }
        }
    }
    closedir(d);
}
#endif

static int make_dir(const char *path) {
#if defined(_WIN32)
    return _mkdir(path);
#else
    int res = mkdir(path, 0777);
    if (res == 0) chmod(path, 0777);
    return res;
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

/* Only these typed selectors may affect renderer startup, never an environment map. */
static const char *diag_temporal_mode;
static const char *diag_temporal_variant;
static int diag_collectors_present, diag_gpu_timing, diag_gpu_components;
static int diag_temporal_source;
static int diag_save_export;
int diag_bootstrap_save_export_requested(void) {
    return diag_bootstrap_is_diagnostic_variant() && diag_save_export;
}
int diag_bootstrap_temporal_source_requested(void) {
    return diag_bootstrap_is_diagnostic_variant() && diag_temporal_source;
}

static const char *diag_json_space(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Decode ASCII keys/selectors; other Unicode remains nonmatching, not an alias. */
static const char *diag_json_string(const char *p, char *out, size_t cap) {
    size_t n = 0;
    if (*p++ != '"') return NULL;
    while (*p && *p != '"') {
        unsigned c = (unsigned char)*p++;
        if (c < 32) return NULL;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (!c) return NULL;
            if (c == 'u') {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    unsigned h = (unsigned char)*p++;
                    if (h >= '0' && h <= '9') h -= '0';
                    else if (h >= 'a' && h <= 'f') h = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') h = h - 'A' + 10;
                    else return NULL;
                    v = (v << 4) | h;
                }
                c = v && v < 128 ? v : 128;
            } else if (!strchr("\"\\/bfnrt", (int)c)) return NULL;
            else if (c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't') c = 128;
        }
        if (out && n + 1 < cap) out[n] = (char)c;
        n++;
    }
    if (*p != '"' || (out && n >= cap)) return NULL;
    if (out) out[n] = 0;
    return p + 1;
}

static const char *diag_json_skip(const char *p, unsigned depth) {
    p = diag_json_space(p);
    if (depth > 32 || !*p) return NULL;
    if (*p == '"') return diag_json_string(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        int object = *p == '{';
        char close = object ? '}' : ']';
        p = diag_json_space(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (object) {
                p = diag_json_string(p, NULL, 0);
                if (!p || *(p = diag_json_space(p)) != ':') return NULL;
                p++;
            }
            p = diag_json_skip(p, depth + 1);
            if (!p) return NULL;
            p = diag_json_space(p);
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p = diag_json_space(p + 1);
        }
    }
    static const char *literals[] = {"true", "false", "null"};
    for (unsigned i = 0; i < 3; i++) {
        size_t n = strlen(literals[i]);
        if (!strncmp(p, literals[i], n) && (!p[n] || strchr(",]} \t\r\n", p[n]))) return p + n;
    }
    if (*p == '-') p++;
    if (*p == '0') p++;
    else {
        if (*p < '1' || *p > '9') return NULL;
        do { p++; } while (*p >= '0' && *p <= '9');
    }
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return NULL;
        do { p++; } while (*p >= '0' && *p <= '9');
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (*p < '0' || *p > '9') return NULL;
        do { p++; } while (*p >= '0' && *p <= '9');
    }
    return !*p || strchr(",]} \t\r\n", *p) ? p : NULL;
}

/* Find a unique member, optionally rejecting every other key. */
static int diag_json_member(const char *p, const char *key, const char *allowed,
                            const char **value) {
    *value = NULL;
    p = diag_json_space(p);
    if (*p++ != '{') return -1;
    p = diag_json_space(p);
    if (*p == '}') return 0;
    for (;;) {
        char name[128];
        p = diag_json_string(p, name, sizeof(name));
        if (!p || *(p = diag_json_space(p)) != ':') return -1;
        if (allowed) {
            const char *candidate = allowed;
            while (*candidate) {
                if (*candidate == '|') { candidate++; continue; }
                size_t length = strcspn(candidate, "|");
                if (strlen(name) == length && !memcmp(candidate, name, length)) break;
                candidate += length;
            }
            if (!*candidate) return -1;
        }
        p = diag_json_space(p + 1);
        if (!strcmp(name, key)) {
            if (*value) return -1;
            *value = p;
        }
        p = diag_json_skip(p, 0);
        if (!p) return -1;
        p = diag_json_space(p);
        if (*p == '}') return *value ? 1 : 0;
        if (*p++ != ',') return -1;
        p = diag_json_space(p);
    }
}

static int diag_parse_temporal(const char *json) {
    const char *t, *s, *v, *c;
    char mode[40], variant[24], cohort[24];
    diag_temporal_mode = diag_temporal_variant = NULL;
    int r = diag_json_member(json, "treatments", NULL, &t);
    if (r <= 0) return r;
    if (!strncmp(t, "null", 4) && strchr(",]} \t\r\n", t[4])) return 0;
    r = diag_json_member(t, "temporal", NULL, &t);
    if (r <= 0) return r;
    if (!strncmp(t, "null", 4) && strchr(",]} \t\r\n", t[4])) return 0;
    if (diag_json_member(t, "selection", "|selection|cohort|", &s) != 1 ||
        diag_json_member(t, "cohort", "|selection|cohort|", &c) != 1 ||
        !diag_json_string(c, cohort, sizeof(cohort)) ||
        (strcmp(cohort, "equal_output") && strcmp(cohort, "equal_render"))) return -1;
    if (diag_json_member(s, "mode", "|mode|variant|", &c) != 1 ||
        !diag_json_string(c, mode, sizeof(mode))) return -1;
    r = diag_json_member(s, "variant", "|mode|variant|", &v);
    if (r < 0) return -1;
    static const char *modes[] = {"native", "fsr3", "fsr3_quality", "fsr3_balanced",
        "fsr3_performance", "fsr3_ultra_performance", "sgsr2", "sgsr2_quality", "sgsr2_performance"};
    int i;
    for (i = 0; i < 9 && strcmp(mode, modes[i]); i++) {}
    if (i == 9 || (i < 6 && r != 0) || (i >= 6 && r != 1)) return -1;
    if (i >= 6) {
        if (!diag_json_string(v, variant, sizeof(variant))) return -1;
        if (!strcmp(variant, "2_pass_fs")) diag_temporal_variant = "2-pass-fs";
        else if (!strcmp(variant, "2_pass_cs")) diag_temporal_variant = "2-pass-cs";
        else if (!strcmp(variant, "3_pass_cs")) diag_temporal_variant = "3-pass-cs";
        else return -1;
    }
    diag_temporal_mode = i ? modes[i] : "off";
    return 0;
}

static int diag_parse_collectors(const char *json) {
    const char *profile, *p;
    diag_collectors_present = diag_gpu_timing = diag_gpu_components = 0;
    diag_temporal_source = 0;
    int r = diag_json_member(json, "profile", NULL, &profile);
    if (r <= 0) return r;
    r = diag_json_member(profile, "collectors", NULL, &p);
    if (r <= 0) return r;
    if (*p++ != '[') return -1;
    unsigned seen = 0;
    p = diag_json_space(p);
    while (*p != ']') {
        char collector[64];
        p = diag_json_string(p, collector, sizeof(collector));
        if (!p) return -1;
        static const char *allowed[] = {"events", "metrics", "frame_timing", "gpu_timing", "live_status",
            "atrace", "perfetto", "simpleperf", "perfetto_trace", "deep_memory_dump", "raw_frame_capture", "gpu_components", "temporal_source_capture"};
        unsigned i;
        for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]) && strcmp(collector, allowed[i]); i++) {}
        if (i == sizeof(allowed) / sizeof(allowed[0]) || (seen & (1u << i))) return -1;
        seen |= 1u << i;
        if (!strcmp(collector, "gpu_timing")) diag_gpu_timing = 1;
        if (!strcmp(collector, "gpu_components")) diag_gpu_components = 1;
        if (!strcmp(collector, "temporal_source_capture")) diag_temporal_source = 1;
        p = diag_json_space(p);
        if (*p == ']') break;
        if (*p++ != ',') return -1;
        p = diag_json_space(p);
        if (*p == ']') return -1;
    }
    if (diag_temporal_source) {
        const char *name;
        char value[32];
        if (diag_json_member(profile, "profile_name", NULL, &name) != 1 ||
            !diag_json_string(name, value, sizeof value) || strcmp(value, "validation")) return -1;
    }
    diag_collectors_present = 1;
    return 0;
}

int diag_bootstrap_validate_request(const char *run_dir, const char *run_id, const char *expected_digest, char *out_error_reason, size_t error_cap) {
    diag_save_export = 0;
    diag_temporal_source = 0;
    diag_temporal_mode = diag_temporal_variant = NULL;
    diag_collectors_present = diag_gpu_timing = diag_gpu_components = 0;
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

    int result = diag_bootstrap_validate_request_data((const uint8_t *)buf, read_bytes,
        run_id, expected_digest, out_error_reason, error_cap);
    free(buf);
    return result;
}

int diag_bootstrap_validate_request_data(const uint8_t *data, size_t read_bytes, const char *run_id,
                                         const char *expected_digest, char *out_error_reason, size_t error_cap) {
    diag_save_export = diag_temporal_source = 0;
    diag_temporal_mode = diag_temporal_variant = NULL;
    diag_collectors_present = diag_gpu_timing = diag_gpu_components = 0;
    if (!data || !run_id || !expected_digest || !diag_bootstrap_validate_run_id(run_id) ||
        !read_bytes || read_bytes > DIAG_BOOTSTRAP_MAX_REQUEST_BYTES || memchr(data, 0, read_bytes))
        return DIAG_BOOTSTRAP_ERR_INVALID_ARGS;
    char *buf = malloc(read_bytes + 1);
    if (!buf) return DIAG_BOOTSTRAP_ERR_IO;
    memcpy(buf, data, read_bytes);
    buf[read_bytes] = 0;

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
    const char *build, *member;
    int build_found = diag_json_member(buf, "build", NULL, &build);
    static const char *build_keys[] = {"variant", "target", "native_optimization"};
    static const char *build_values[] = {"diagnostic", "android-arm64", "release"};
    static const char *build_errors[] = {"INVALID_BUILD_VARIANT", "INVALID_TARGET", "INVALID_NATIVE_OPTIMIZATION"};
    for (size_t i = 0; i < 3; i++) {
        char value[32];
        if (build_found != 1 || diag_json_member(build, build_keys[i], NULL, &member) != 1 ||
            !diag_json_string(member, value, sizeof(value)) || strcmp(value, build_values[i])) {
            free(buf);
            if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "%s", build_errors[i]);
            return DIAG_BOOTSTRAP_ERR_INVALID_JSON;
        }
    }

    if (diag_parse_temporal(buf) != 0) {
        diag_temporal_mode = diag_temporal_variant = NULL;
        free(buf);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "INVALID_TEMPORAL_TREATMENT");
        return DIAG_BOOTSTRAP_ERR_INVALID_JSON;
    }
    if (diag_parse_collectors(buf) != 0) {
        diag_temporal_source = 0;
        diag_temporal_mode = diag_temporal_variant = NULL;
        diag_collectors_present = diag_gpu_timing = diag_gpu_components = 0;
        free(buf);
        if (out_error_reason && error_cap > 0) snprintf(out_error_reason, error_cap, "INVALID_DIAGNOSTIC_COLLECTOR");
        return DIAG_BOOTSTRAP_ERR_INVALID_JSON;
    }
    const char *profile, *name;
    char profile_name[64];
    if (diag_json_member(buf, "profile", NULL, &profile) == 1 &&
        diag_json_member(profile, "profile_name", NULL, &name) == 1 &&
        diag_json_string(name, profile_name, sizeof profile_name) &&
        !strcmp(profile_name, "committed_save_export")) {
        const char *collectors, *treatment, *id, *schema;
        char value[80];
        const char *end = diag_json_skip(buf, 0);
        if (!end || *diag_json_space(end) ||
            diag_json_member(buf, "run_id", "|schema_version|run_id|build|profile|created_utc|", &id) != 1 ||
            !diag_json_string(id, value, sizeof value) || strcmp(value, run_id) ||
            diag_json_member(buf, "schema_version", NULL, &schema) != 1 ||
            !diag_json_string(schema, value, sizeof value) || strcmp(value, "1.0") ||
            diag_json_member(profile, "collectors", "|profile_name|collectors|", &collectors) != 1 ||
            *collectors != '[' || *diag_json_space(collectors + 1) != ']' ||
            diag_json_member(buf, "treatments", NULL, &treatment) != 0) {
            free(buf);
            return DIAG_BOOTSTRAP_ERR_INVALID_JSON;
        }
        diag_save_export = 1;
        const char *created;
        int created_found = diag_json_member(buf, "created_utc", NULL, &created);
        if (created_found < 0 || (created_found && !diag_json_string(created, NULL, 0))) {
            diag_save_export = 0;
            free(buf);
            return DIAG_BOOTSTRAP_ERR_INVALID_JSON;
        }
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
#if !defined(_WIN32)
    chmod(path, 0666);
#endif
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
#if !defined(_WIN32)
    chmod(path, 0666);
#endif
    return DIAG_BOOTSTRAP_OK;
}

int diag_bootstrap_apply_session_environment(const char *run_dir, const char *run_id) {
    if (!run_dir || !run_id || diag_save_export) return -1;
    char subpath[512];
    int trace_len = snprintf(subpath, sizeof(subpath), "%s/boot_trace.txt", run_dir);
    if (trace_len < 0 || (size_t)trace_len >= sizeof(subpath)) return DIAG_BOOTSTRAP_ERR_INVALID_ARGS;
    /* The runtime's relative trace default writes in the app root, outside this run. */
    set_env_var("WT_TRACE", subpath);
    if (diag_collectors_present) {
        set_env_var("WT_VK_GPUTIME", diag_gpu_timing || diag_gpu_components ? "1" : "0");
        set_env_var("WT_VK_PASSTIME", diag_gpu_components ? "1" : "0");
    }
    if (diag_temporal_mode) {
        set_env_var("WT_UPSCALER", diag_temporal_mode);
        /* Explicitly clear a stale selector even for native/FSR treatments. */
        set_env_var("WT_SGSR_VARIANT", diag_temporal_variant ? diag_temporal_variant : "2-pass-fs");
    }

#if !defined(_WIN32)
    umask(0000);
#endif

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

    /* Determine scenario timeout from request.json if present */
    char req_file[512];
    snprintf(req_file, sizeof(req_file), "%s/request.json", run_dir);
    FILE *f_req = fopen(req_file, "r");
    if (f_req) {
        char rline[256];
        int set_sec = 0;
        while (fgets(rline, sizeof(rline), f_req)) {
            char *p = strstr(rline, "\"scenario_deadline_secs\":");
            if (!p) p = strstr(rline, "\"scenario_deadline_secs\" :");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    double d = atof(p + 1);
                    if (d > 5.0) {
                        int run_secs = (int)(d - 10.0);
                        if (run_secs < 5) run_secs = 5;
                        char sbuf[32];
                        snprintf(sbuf, sizeof(sbuf), "%d", run_secs);
                        set_env_var("WT_SECONDS", sbuf);
                        set_sec = 1;
                        break;
                    }
                }
            }
        }
        fclose(f_req);
        if (!set_sec) {
            set_env_var("WT_SECONDS", "30");
        }
    } else {
        set_env_var("WT_SECONDS", "30");
    }

    /* Redirect output to session.log */
    snprintf(subpath, sizeof(subpath), "%s/session.log", run_dir);
    freopen(subpath, "a", stdout);
    freopen(subpath, "a", stderr);
#if !defined(_WIN32)
    chmod(subpath, 0666);
    make_accessible_recursive(run_dir);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    return DIAG_BOOTSTRAP_OK;
}
