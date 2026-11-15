#!/usr/bin/env python3
"""Execute the production cache paths with temporary files and scripted Vulkan/I/O.

No GPU, game data, or device behavior is simulated or qualified by this test.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "runtime/nvn_vk_m4.c").read_text()


def section(start, end):
    begin = SOURCE.index(start)
    return SOURCE[begin:SOURCE.index(end, begin)]


PRELUDE = r'''
#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
typedef int32_t LONG;
typedef int64_t LONG64;
typedef void *HANDLE;
typedef unsigned DWORD;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define INFINITE 0xffffffffu
#define MOVEFILE_REPLACE_EXISTING 1
#define GetFileExInfoStandard 0
#define INVALID_FILE_ATTRIBUTES 0xffffffffu
typedef struct { LONG64 QuadPart; } LARGE_INTEGER;
typedef struct { DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
static int fail_close, fail_write, fail_rename, fail_delete, fail_get, busy, fail_alloc, shader_calls, bad_driver_input, dirty_during_get;
static volatile LONG64 g_pc_dirty = 21, g_pc_dirty_saved = 7;
static volatile LONG g_pcsave_want;
static volatile LONG g_pcache_valid;
static volatile LONG g_prep_marker_stale;
static LONG64 g_gap_max;
static int g_in_fight, g_prep_limited, g_prep_nfiles;
static LONG64 g_prep_vars;
static char g_pcache_path[260], g_prep_marker[260];
static HANDLE g_pcw_thread;
static volatile LONG g_pcprobe_stop;
static LONG64 g_pcprobe_max, g_pcprobe_n, g_pcprobe_before;
static VkPipelineCache g_pcache = (VkPipelineCache)(uintptr_t)1;
static struct { VkDevice device; VkPhysicalDeviceProperties props; } V;
static int g_offline;
static LONG64 tk(void) { return 1; }
static int QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000; return 1; }
static int GetThreadPriority(HANDLE h) { (void)h; return 0; }
static HANDLE GetCurrentThread(void) { return NULL; }
static void Sleep(unsigned ms) { (void)ms; }
static long long nvn_front_frame_next(void) { return 0; }
static LONG InterlockedExchange(volatile LONG *p, LONG value) { return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST); }
static LONG64 InterlockedExchange64(volatile LONG64 *p, LONG64 value) { return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST); }
static LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 value, LONG64 cmp) {
    __atomic_compare_exchange_n(p, &cmp, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return cmp;
}
static LONG InterlockedCompareExchange(volatile LONG *p, LONG value, LONG cmp) {
    __atomic_compare_exchange_n(p, &cmp, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return cmp;
}
static DWORD WaitForSingleObject(HANDLE h, DWORD timeout) { (void)h; (void)timeout; return busy ? WAIT_TIMEOUT : WAIT_OBJECT_0; }
static int CloseHandle(HANDLE h) { (void)h; return 1; }
static HANDLE CreateEventA(void *a, int b, int c, void *d) { (void)a; (void)b; (void)c; (void)d; return NULL; }
static HANDLE CreateThread(void *a, size_t b, DWORD (*f)(void *), void *arg, DWORD c, DWORD *d) {
    (void)a; (void)b; (void)f; (void)arg; (void)c; (void)d; return NULL;
}
static DWORD pcprobe_main(void *arg) { (void)arg; return 0; }
static int GetFileAttributesExA(const char *path, int mode, WIN32_FILE_ATTRIBUTE_DATA *a) {
    struct stat st; (void)mode;
    if (stat(path, &st)) return 0;
    a->nFileSizeLow = (DWORD)st.st_size; a->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); return 1;
}
static int MoveFileExA(const char *from, const char *to, int flags) {
    (void)flags; return !fail_rename && rename(from, to) == 0;
}
static int DeleteFileA(const char *path) { return !fail_delete && remove(path) == 0; }
static DWORD GetFileAttributesA(const char *path) { struct stat st; return stat(path, &st) ? INVALID_FILE_ATTRIBUTES : 0; }
static int injected_close(FILE *f) { int r = fclose(f); return fail_close ? EOF : r; }
static void *injected_malloc(size_t n) { return fail_alloc ? NULL : malloc(n); }
static size_t injected_write(const void *p, size_t size, size_t count, FILE *fp) {
    return fwrite(p, size, fail_write ? count / 2 : count, fp);
}
static int injected_rename(const char *from, const char *to) {
    return fail_rename ? -1 : rename(from, to);
}
#define fclose injected_close
#define malloc injected_malloc
#define fwrite injected_write
#define rename injected_rename
#define _fseeki64 fseeko
#define _ftelli64 ftello
#if __has_include("vk_cache.h")
#include "vk_cache.h"
#endif
#include "nvn_vk_cache.h"
static const char *vkc_dir(void) { return "."; }
static const char *vkc_file(const char *name) { return name; }
static const uint32_t spv_good[] = {0x07230203u, 0x00010000u, 0, 4, 0, 0x00010000u};
static VkResult vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci, const void *a, VkShaderModule *m) {
    (void)device; (void)a; shader_calls++; assert(ci->codeSize == sizeof spv_good);
    *m = (VkShaderModule)(uintptr_t)2; return VK_SUCCESS;
}
static VkResult vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo *ci, const void *a, VkPipelineCache *cache) {
    (void)device; (void)a;
    if (ci->initialDataSize) {
        const unsigned char *b = ci->pInitialData;
        if (ci->initialDataSize < 32 || b[0] != 32 || b[4] != 1 || b[8] != 1 || b[12] != 2 || b[16] != 42) bad_driver_input++;
    }
    *cache = (VkPipelineCache)(uintptr_t)1; return VK_SUCCESS;
}
#define VKC(x) assert((x) == VK_SUCCESS)
static VkResult vkGetPipelineCacheData(VkDevice device, VkPipelineCache cache, size_t *n, void *data) {
    (void)device; (void)cache;
    if (fail_get) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!data) { *n = 32; return VK_SUCCESS; }
    if (*n < 32) return VK_INCOMPLETE;
    if (dirty_during_get) { g_pc_dirty++; dirty_during_get = 0; }
    memset(data, 0, 32);
    ((uint32_t *)data)[0] = 32; ((uint32_t *)data)[1] = 1;
    ((uint32_t *)data)[2] = V.props.vendorID; ((uint32_t *)data)[3] = V.props.deviceID;
    memcpy((unsigned char *)data + 16, V.props.pipelineCacheUUID, 16);
    *n = 32; return VK_SUCCESS;
}
static int g_pw_n;
typedef struct { uint32_t layout; } PKey;
typedef struct { uint32_t magic, compute; uint64_t vs_h, fs_h; PKey k; } PipeRec;
#define PIPEREC_MAGIC 42u
static PipeRec *g_pw_rec;
static struct { int has_vids; } D;
static uint32_t pkey_vlay(const PKey *k) { return k->layout; }
static void *vlay_of(uint32_t n) { return n ? (void *)(uintptr_t)1 : NULL; }
static int piperec_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(PipeRec)); }
'''


TEST_BODY = r'''
static void write_bytes(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(data, 1, n, f) == n); assert(!fclose(f));
}
static void expect_old(void) {
    char data[4] = {0}; FILE *f = fopen(g_pcache_path, "rb"); assert(f);
    assert(fread(data, 1, 3, f) == 3); fclose(f); assert(!memcmp(data, "old", 3));
}
int main(int argc, char **argv) {
    assert(argc == 2); const char *mode = argv[1];
    strcpy(g_pcache_path, "cache.bin"); strcpy(g_prep_marker, "prepared.txt");
    write_bytes(g_pcache_path, "old", 3);
    if (!strcmp(mode, "spv-corrupt-offline")) {
        g_offline = 1; write_bytes("spv/0000000000000001.spv", "bad", 3);
        assert(module_from_spv(1) == NULL && shader_calls == 0);
    } else if (!strcmp(mode, "spv-valid")) {
        write_bytes("spv/0000000000000001.spv", spv_good, sizeof spv_good);
        g_offline = 1; assert(module_from_spv(1)); assert(shader_calls == 0);
        g_offline = 0; assert(module_from_spv(1)); assert(shader_calls == 1);
    } else if (!strcmp(mode, "spv-bad-version") || !strcmp(mode, "spv-truncated-instruction")) {
        uint32_t words[6]; memcpy(words, spv_good, sizeof words);
        if (!strcmp(mode, "spv-bad-version")) words[1] = 0xffffffffu;
        else words[5] = 0x00020000u;
        write_bytes("spv/0000000000000001.spv", words, sizeof words);
        assert(!module_from_spv(1) && shader_calls == 0);
    } else if (!strncmp(mode, "pipeline-", 9)) {
        V.props.vendorID = 1; V.props.deviceID = 2; V.props.pipelineCacheUUID[0] = 42;
        unsigned char bytes[32] = {32, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2};
        if (strcmp(mode, "pipeline-uuid")) bytes[16] = 42;
        if (!strcmp(mode, "pipeline-vendor")) bytes[8] = 3;
        if (!strcmp(mode, "pipeline-device")) bytes[12] = 3;
        write_bytes("pipelines_0001_0002_00000000.bin", bytes, !strcmp(mode, "pipeline-corrupt") ? 8 : 32);
        fail_alloc = !strcmp(mode, "pipeline-oom");
        if (!strcmp(mode, "pipeline-oversized")) {
            FILE *f = fopen("pipelines_0001_0002_00000000.bin", "wb"); assert(f);
            assert(!fseek(f, (1L << 30), SEEK_SET)); assert(fputc(0, f) == 0); fclose(f);
        }
        load_driver_cache(); assert(bad_driver_input == 0);
        assert(g_pcache_valid == !strcmp(mode, "pipeline-valid"));
    } else if (!strncmp(mode, "marker-", 7)) {
        V.props.vendorID = 1; V.props.deviceID = 2; V.props.pipelineCacheUUID[0] = 42;
        strcpy(g_pcache_path, "pipelines_0001_0002_00000000.bin"); write_bytes(g_pcache_path, "old", 3);
        write_bytes(g_prep_marker, "old completion", 14); fail_delete = 1;
        invalidate_marker(); assert(g_prep_marker_stale);
        g_prep_limited = !strcmp(mode, "marker-limited");
        int complete = !strcmp(mode, "marker-complete");
        pcache_save_at(0, complete || g_prep_limited);
        if (!complete) { expect_old(); assert(g_pc_dirty_saved == 7); }
        g_pcache_valid = g_prep_marker_stale = 0; /* a fresh process */
        load_driver_cache(); invalidate_marker();
        assert(g_pcache_valid == complete);
        assert(g_prep_marker_stale == !complete);
    } else if (!strcmp(mode, "save-busy")) {
        g_pcw_thread = (void *)(uintptr_t)1; busy = 1;
        pcache_save_at(1, 1);
        assert(g_pc_dirty_saved == 7 && g_pcsave_want == 1 && g_pcw_thread);
    } else if (!strcmp(mode, "save-read-failure")) {
        fail_get = 1; pcache_save_at(1, 1);
        assert(g_pc_dirty_saved == 7 && g_pcsave_want == 1); expect_old();
    } else if (!strcmp(mode, "save-close-failure") || !strcmp(mode, "save-rename-failure") || !strcmp(mode, "save-short-write")) {
        fail_close = !strcmp(mode, "save-close-failure"); fail_rename = !strcmp(mode, "save-rename-failure");
        fail_write = !strcmp(mode, "save-short-write");
        pcache_save_at(1, 1); fail_close = 0;
        assert(g_pc_dirty_saved == 7 && g_pcsave_want == 1); expect_old();
        assert(fopen("prepared.txt", "rb") == NULL);
    } else if (!strcmp(mode, "save-success") || !strcmp(mode, "save-concurrent-generation")) {
        dirty_during_get = !strcmp(mode, "save-concurrent-generation");
        pcache_save_at(1, 1);
        assert(g_pc_dirty_saved == 21 && !g_pcsave_want);
        assert(g_pc_dirty == (!strcmp(mode, "save-concurrent-generation") ? 22 : 21));
        FILE *f = fopen("prepared.txt", "rb"); assert(f); fclose(f);
    } else if (!strcmp(mode, "records-partial") || !strcmp(mode, "records-oom") || !strcmp(mode, "records-oversized")) {
        PipeRec rec[2] = {0}; rec[0].magic = PIPEREC_MAGIC; rec[0].compute = 1;
        write_bytes("pipes.bin", rec, sizeof(PipeRec) + (!strcmp(mode, "records-partial") ? 1 : 0));
        if (!strcmp(mode, "records-oversized")) {
            FILE *f = fopen("pipes.bin", "wb"); assert(f);
            assert(!fseek(f, (64L << 20) + sizeof(PipeRec) - 1, SEEK_SET)); assert(fputc(0, f) == 0); fclose(f);
        }
        fail_alloc = !strcmp(mode, "records-oom"); load_records();
        assert(g_pw_n == 0 && !g_pw_rec);
    } else if (!strcmp(mode, "records-valid")) {
        PipeRec rec[3] = {0}; rec[0].magic = rec[1].magic = PIPEREC_MAGIC;
        rec[0].compute = rec[1].compute = 1;
        write_bytes("pipes.bin", rec, sizeof rec); load_records();
        assert(g_pw_n == 1); free(g_pw_rec);
    } else return 2;
    return 0;
}
'''


class VulkanCacheTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="wt-cache-test-")
        directory = Path(cls.temp.name)
        (directory / "spv").mkdir()
        driver_block = section("    {   /* the driver's pipeline cache, ours on disk:", "    /* Blind prediction")
        records_block = section('    FILE *fp = ahead ? fopen(vkc_file("pipes.bin")', "    SYSTEM_INFO si;")
        body = PRELUDE
        body += section("static VkShaderModule module_from_spv(", "static VkShaderModule module_from_glsl(")
        body += "\nstatic void load_driver_cache(void) {\n" + driver_block + "}\n"
        body += "\nstatic void load_records(void) { int ahead = 1;\n" + records_block + "}\n"
        marker_block = section("    /* A marker cannot qualify", "    if (on && InterlockedCompareExchange")
        body += "\nstatic void invalidate_marker(void) {\n" + marker_block + "}\n"
        body += section("typedef struct { void *data; size_t n;", "/* TEST ONLY (with WT_VK_PCSAVE=prep)")
        body += section("static void pcache_save_at(int where, int marker) {", "static void pcache_save(void) { pcache_save_at")
        body += TEST_BODY
        src = directory / "cache.c"; src.write_text(body)
        cls.exe = directory / "cache"
        cc = shlex.split(os.environ.get("HOST_CC", shutil.which("clang") or "cc"))
        subprocess.run(cc + ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function",
                            "-Wno-unused-variable", "-Wno-missing-field-initializers", "-I", str(ROOT / "runtime"), str(src), "-o", str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, case):
        with tempfile.TemporaryDirectory(prefix="wt-cache-case-") as work:
            (Path(work) / "spv").mkdir()
            result = subprocess.run([str(self.exe), case], cwd=work, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for case in ["spv-corrupt-offline", "spv-valid", "spv-bad-version", "spv-truncated-instruction",
             "pipeline-corrupt", "pipeline-uuid", "pipeline-vendor", "pipeline-device", "pipeline-valid", "pipeline-oom", "pipeline-oversized",
             "marker-restart", "marker-limited", "marker-complete",
             "save-busy", "save-read-failure", "save-close-failure", "save-short-write", "save-concurrent-generation",
             "save-rename-failure", "save-success", "records-partial", "records-oom", "records-oversized", "records-valid"]:
    setattr(VulkanCacheTest, "test_" + case.replace("-", "_"), lambda self, c=case: self.run_case(c))

if __name__ == "__main__":
    unittest.main()
