"""Exercise the active M4 shader cache with real files and a Vulkan boundary stub.

Corruption must cause a compile/cache miss; a failed cache write must preserve the
previous complete file. No Vulkan device or private game shaders are required.
"""
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "runtime/nvn_vk_m4.c").read_text(encoding="utf-8")
functions = source[source.index("static VkShaderModule module_from_spv("):source.index("static void vstage_from_rec(")]
stage_rec = source[source.index("typedef struct { uint32_t magic; int32_t stage,"):source.index("typedef struct { uint32_t magic, key[4],")]
version = next(line for line in source.splitlines() if line.startswith("#define MX_VK_VERSION "))
record_magic = next(line for line in source.splitlines() if line.startswith("#define STAGEREC_MAGIC "))

prelude = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int short_write, close_error;
static size_t test_fwrite(const void *p, size_t size, size_t n, FILE *fp) {
    if (short_write) { short_write = 0; return fwrite(p, size, n ? n - 1 : 0, fp); }
    return fwrite(p, size, n, fp);
}
static int test_fclose(FILE *fp) {
    int result = fclose(fp);
    if (close_error && !--close_error) { errno = EIO; return EOF; }
    return result;
}
#define fwrite test_fwrite
#define fclose test_fclose
#if __has_include("nvn_vk_cache.h")
#include "nvn_vk_cache.h"
#endif
#define VKC(expr) assert((expr) == 0)
#define VK_SUCCESS 0
#define VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO 1
typedef void *VkShaderModule;
typedef struct { int sType; void *pNext; int flags; size_t codeSize; const uint32_t *pCode; } VkShaderModuleCreateInfo;
typedef struct { uint8_t unit, dim, shadow; uint16_t binding; } VSmp;
static struct { void *device; } V;
static struct { long long modules; } R;
static char cache_dir[240];
static int g_offline, compile_count, fail_module_once, different_compile;
static uint64_t g_glsl_cache_id;
static const char *vkc_dir(void) { return cache_dir; }
static void InterlockedIncrement64(long long *n) { ++*n; }
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * UINT64_C(0x100000001b3);
    return h;
}
/* Complete instruction boundaries; driver semantics are outside this cache test. */
static const uint32_t shader[] = {
    0x07230203, 0x00010600, 0, 7, 0,
    0x00020011, 1, 0x0003000e, 0, 1, 0x0005000f, 5, 1, 0x6e69616d, 0,
};
static int vkCreateShaderModule(void *device, const VkShaderModuleCreateInfo *ci, void *alloc, VkShaderModule *mod) {
    (void)device; (void)alloc;
    assert(ci->codeSize > 0 && ci->pCode);
    if (fail_module_once) { --fail_module_once; return -1; }
    *mod = (VkShaderModule)(uintptr_t)42;
    return 0;
}
static int g_glsl_to_spirv(int stage, const char *glsl, uint32_t **words, size_t *count, char *log, size_t n) {
    (void)stage; (void)glsl; (void)log; (void)n;
    ++compile_count;
    *count = sizeof shader / sizeof *shader;
    *words = malloc(sizeof shader);
    memcpy(*words, shader, sizeof shader);
    if (different_compile) (*words)[2] = 1;
    return 1;
}
static void g_spirv_free(uint32_t *words) { free(words); }
'''

tests = r'''
#undef fwrite
#undef fclose
static void path_of(char *path, size_t n, uint64_t hash) {
    snprintf(path, n, "%s/spv/%016llx.spv", cache_dir, (unsigned long long)hash);
}
static void put(const char *path, const void *data, size_t n) {
    FILE *fp = fopen(path, "wb"); assert(fp);
    assert(fwrite(data, 1, n, fp) == n); assert(!fclose(fp));
}
static int bytes_equal(const char *path, const void *data, size_t n) {
    unsigned char buf[512]; FILE *fp = fopen(path, "rb");
    assert(n < sizeof buf);
    if (!fp) return 0;
    size_t got = fread(buf, 1, sizeof buf, fp); fclose(fp);
    return got == n && !memcmp(buf, data, n);
}
int main(int argc, char **argv) {
    assert(argc == 3); snprintf(cache_dir, sizeof cache_dir, "%s", argv[2]);
    char path[512], dir[512];
    snprintf(dir, sizeof dir, "%s/spv", cache_dir); assert(!mkdir(dir, 0700));
    snprintf(dir, sizeof dir, "%s/stage", cache_dir); assert(!mkdir(dir, 0700));
    path_of(path, sizeof path, 1);
    if (!strcmp(argv[1], "offline_empty")) {
        put(path, NULL, 0); g_offline = 1;
        assert(module_from_spv(1) == NULL);
    } else if (!strcmp(argv[1], "invalid_instruction")) {
        uint32_t broken[sizeof shader / 4]; memcpy(broken, shader, sizeof shader); broken[5] = 17;
        put(path, broken, sizeof broken);
        assert(module_from_spv(1) == NULL);
    } else if (!strcmp(argv[1], "truncated_instruction")) {
        put(path, shader, sizeof shader - 4);
        assert(module_from_spv(1) == NULL);
    } else if (!strcmp(argv[1], "oversize")) {
        put(path, shader, sizeof shader);
        assert(!truncate(path, 64 * 1024 * 1024 + 4));
        assert(module_from_spv(1) == NULL);
    } else if (!strcmp(argv[1], "valid_hit")) {
        put(path, shader, sizeof shader);
        assert(module_from_spv(1) != NULL);
        g_offline = 1; assert(module_from_spv(1) != NULL);
    } else if (!strcmp(argv[1], "compile_reuse") || !strcmp(argv[1], "long_path")) {
        uint64_t hash;
        assert(module_from_glsl(5, "shader", "fixture", &hash));
        assert(compile_count == 1);
        assert(module_from_glsl(5, "shader", "fixture", &hash));
        assert(compile_count == 1);
        path_of(path, sizeof path, hash); put(path, shader, 16);
        assert(module_from_glsl(5, "shader", "fixture", &hash));
        assert(compile_count == 2);
    } else if (!strcmp(argv[1], "compiler_change")) {
        uint64_t first, second;
        assert(module_from_glsl(5, "shader", "fixture", &first));
        g_glsl_cache_id = 7;
        assert(module_from_glsl(5, "shader", "fixture", &second));
        assert(first != second && compile_count == 2);
    } else if (!strcmp(argv[1], "short_write") || !strcmp(argv[1], "close_error")) {
        uint64_t hash;
        assert(module_from_glsl(5, "shader", "fixture", &hash));
        path_of(path, sizeof path, hash);
        fail_module_once = 1;
        different_compile = 1;
        if (!strcmp(argv[1], "short_write")) short_write = 1;
        else close_error = 2; /* The cache read closes first, then the writer. */
        assert(module_from_glsl(5, "shader", "fixture", &hash));
        assert(bytes_equal(path, shader, sizeof shader));
    } else if (!strcmp(argv[1], "record_identity") || !strcmp(argv[1], "record_trailing") || !strcmp(argv[1], "record_valid") || !strcmp(argv[1], "record_corrupt_metadata")) {
        StageRec r = {0}, got; r.magic = STAGEREC_MAGIC; r.stage = 1; r.code_len = 8;
        r.hb = 12; r.spv = 1;
        uint64_t hash = fnv(fnv(r.hb, r.key, 16), &r.fo, 4);
        snprintf(path, sizeof path, "%s/stage/1_%016llx.st", cache_dir, (unsigned long long)hash);
#if VKC_STAGE_REC_VERSION >= 2
        r.checksum = fnv(UINT64_C(0xcbf29ce484222325), &r, offsetof(StageRec, checksum));
#endif
        if (!strcmp(argv[1], "record_identity")) r.hb = 13;
        if (!strcmp(argv[1], "record_corrupt_metadata")) r.tex_units ^= 4;
        put(path, &r, sizeof r);
        if (!strcmp(argv[1], "record_trailing")) { FILE *fp = fopen(path, "ab"); assert(fp); fputc(1, fp); fclose(fp); }
        assert(stage_rec_read(1, hash, &got) == !strcmp(argv[1], "record_valid"));
    } else { assert(!"unknown case"); }
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="vulkan-cache-tests-") as temporary:
    temp = Path(temporary)
    unit = temp / "test.c"
    unit.write_text(prelude + version + "\n" + stage_rec + "\n" + record_magic + "\n" + functions + tests, encoding="utf-8")
    exe = temp / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-Wno-missing-field-initializers", "-I", str(ROOT / "runtime"), str(unit), "-o", str(exe)], check=True)
    failed = []
    for case in ["offline_empty", "invalid_instruction", "truncated_instruction", "oversize", "valid_hit", "compile_reuse", "long_path", "compiler_change", "short_write", "close_error", "record_valid", "record_identity", "record_trailing", "record_corrupt_metadata"]:
        directory = temp / ("x" * 160 if case == "long_path" else case)
        directory.mkdir()
        result = subprocess.run([str(exe), case, str(directory)], capture_output=True, text=True)
        print(f"{case}: {'PASS' if result.returncode == 0 else 'FAIL'}")
        if result.returncode:
            failed.append(case)
            print(result.stderr.strip())
    if failed:
        raise SystemExit(f"Failed cache behavior: {', '.join(failed)}")
print("M4 shader cache filesystem behaviors passed")
