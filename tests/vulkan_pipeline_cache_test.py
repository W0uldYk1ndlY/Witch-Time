"""Run M4 pipeline-cache load/save behavior against files and a driver boundary stub."""
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "runtime/nvn_vk_m4.c").read_text(encoding="utf-8")
load = source[source.index("    {   /* the driver's pipeline cache,"):source.index("    /* Blind prediction")]
path_declaration = next(line for line in source.splitlines() if line.startswith("static char g_pcache_path["))
write = source[source.index("typedef struct { void *data; size_t n; int marker,"):source.index("/* TEST ONLY (with WT_VK_PCSAVE=prep)")]
save_start = source.index("static void pcache_save_at(int where, int marker) {")
save = source[save_start:source.index("static void pcache_save(void) {", save_start)]

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
static int short_write;
static size_t test_fwrite(const void *p, size_t size, size_t n, FILE *fp) {
    if (short_write) { short_write = 0; return fwrite(p, size, n ? n - 1 : 0, fp); }
    return fwrite(p, size, n, fp);
}
#define fwrite test_fwrite
#include "nvn_vk_cache.h"
typedef long long LONG64;
typedef int LONG, DWORD, VkResult;
typedef void *HANDLE;
typedef struct { LONG64 QuadPart; } LARGE_INTEGER;
typedef struct { uint32_t nFileSizeLow, nFileSizeHigh; } WIN32_FILE_ATTRIBUTE_DATA;
#define WINAPI
#define VK_SUCCESS 0
#define VK_INCOMPLETE 5
#define VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO 1
#define MOVEFILE_REPLACE_EXISTING 1
#define GetFileExInfoStandard 0
#define WAIT_OBJECT_0 0
#define TRUE 1
#define FALSE 0
#define VKC(x) assert((x) == VK_SUCCESS)
#define _fseeki64 fseeko
#define _ftelli64 ftello
typedef struct { int sType; void *pNext; unsigned flags; size_t initialDataSize; const void *pInitialData; } VkPipelineCacheCreateInfo;
static struct { void *device; struct { uint32_t vendorID, deviceID, driverVersion; uint8_t pipelineCacheUUID[16]; } props; } V;
static char cache_dir[100], g_prep_marker[VKC_PATH_CAP];
static HANDLE g_pcache, g_pcw_thread;
static LONG64 g_pc_dirty, g_pc_dirty_saved, g_gap_max, g_pcprobe_before, g_pcprobe_n, g_pcprobe_max, g_prep_vars;
static int g_prep_limited, g_prep_nfiles, g_in_fight, g_pcprobe_stop, g_pcsave_want, g_pcache_file_valid;
static int save_error, driver_gets, wait_busy;
static size_t driver_initial_size;
static unsigned char driver_data[64] = {32, 0, 0, 0, 1, 0, 0, 0, 100, 0, 0, 0, 200, 0, 0, 0};
static const char *vkc_dir(void) { return cache_dir; }
static LONG64 tk(void) { return 0; }
static void QueryPerformanceFrequency(LARGE_INTEGER *v) { v->QuadPart = 1000; }
static LONG64 nvn_front_frame_next(void) { return 0; }
static void Sleep(int ms) { (void)ms; }
static int MoveFileExA(const char *a, const char *b, int flags) { (void)flags; return !rename(a, b); }
static void *GetCurrentThread(void) { return NULL; }
static int GetThreadPriority(void *p) { (void)p; return 0; }
static int GetFileAttributesExA(const char *path, int type, WIN32_FILE_ATTRIBUTE_DATA *out) {
    (void)type; struct stat st; if (stat(path, &st)) return 0;
    out->nFileSizeLow = (uint32_t)st.st_size; out->nFileSizeHigh = (uint32_t)((uint64_t)st.st_size >> 32); return 1;
}
static int WaitForSingleObject(HANDLE h, int timeout) { (void)h; (void)timeout; return wait_busy ? 258 : WAIT_OBJECT_0; }
static void CloseHandle(HANDLE h) { (void)h; }
static HANDLE CreateEventA(void *a, int b, int c, void *d) { (void)a; (void)b; (void)c; (void)d; return NULL; }
static DWORD pcprobe_main(void *arg) { (void)arg; return 0; }
static HANDLE CreateThread(void *a, int b, DWORD (*f)(void *), void *d, int e, void *g) { (void)a; (void)b; (void)f; (void)d; (void)e; (void)g; return NULL; }
static LONG InterlockedExchange(LONG *p, LONG value) { LONG old = *p; *p = value; return old; }
static LONG64 InterlockedExchange64(LONG64 *p, LONG64 value) { LONG64 old = *p; *p = value; return old; }
static LONG64 InterlockedCompareExchange64(LONG64 *p, LONG64 value, LONG64 compare) { LONG64 old = *p; if (old == compare) *p = value; return old; }
static VkResult vkCreatePipelineCache(void *device, const VkPipelineCacheCreateInfo *ci, void *allocator, HANDLE *cache) {
    (void)device; (void)allocator; driver_initial_size = ci->initialDataSize; *cache = (HANDLE)(uintptr_t)1; return VK_SUCCESS;
}
static VkResult vkGetPipelineCacheData(void *device, HANDLE cache, size_t *size, void *data) {
    (void)device; (void)cache; ++driver_gets;
    if (save_error) return -1;
    if (data && *size >= sizeof driver_data) memcpy(data, driver_data, sizeof driver_data);
    *size = sizeof driver_data; return VK_SUCCESS;
}
'''

tests = r'''
#undef fwrite
static void put(const char *path, const void *data, size_t n) {
    FILE *fp = fopen(path, "wb"); assert(fp); assert(fwrite(data, 1, n, fp) == n); assert(!fclose(fp));
}
int main(int argc, char **argv) {
    assert(argc == 3); snprintf(cache_dir, sizeof cache_dir, "%s", argv[2]);
    V.props.vendorID = 100; V.props.deviceID = 200; V.props.driverVersion = 300;
    snprintf(g_pcache_path, sizeof g_pcache_path, "%s/pipelines_0064_00c8_0000012c.bin", cache_dir);
    unsigned char fixture[sizeof driver_data]; memcpy(fixture, driver_data, sizeof fixture);
    size_t length = sizeof fixture;
    if (!strcmp(argv[1], "bad_uuid")) fixture[16] = 1;
    if (!strcmp(argv[1], "bad_vendor")) fixture[8] = 101;
    if (!strcmp(argv[1], "bad_device")) fixture[12] = 201;
    if (!strcmp(argv[1], "bad_version")) fixture[4] = 2;
    if (!strcmp(argv[1], "bad_size")) fixture[0] = 64;
    if (!strcmp(argv[1], "truncated")) length = 20;
    if (!strcmp(argv[1], "invalid_exit_repair")) fixture[16] = 1;
    put(g_pcache_path, fixture, length);
    test_load();
    if (!strcmp(argv[1], "valid")) assert(driver_initial_size == sizeof fixture);
    else if (!strncmp(argv[1], "bad_", 4) || !strcmp(argv[1], "truncated")) assert(driver_initial_size == 0);
    else if (!strcmp(argv[1], "invalid_exit_repair")) {
        pcache_save_at(0, 0);
        assert(driver_gets > 0);
        size_t n = 0; unsigned char *saved = vkc_read_file(g_pcache_path, 128, &n);
        assert(saved && n == sizeof driver_data && !memcmp(saved, driver_data, n)); free(saved);
    } else {
        g_pc_dirty = 13; g_pc_dirty_saved = 0;
        if (!strcmp(argv[1], "failed_query")) save_error = 1;
        if (!strcmp(argv[1], "failed_write")) short_write = 1;
        if (!strcmp(argv[1], "busy_writer")) { g_pcw_thread = (HANDLE)(uintptr_t)1; wait_busy = 1; }
        pcache_save_at(!strcmp(argv[1], "busy_writer") ? 1 : 0, 1);
        if (!strcmp(argv[1], "successful_save")) assert(g_pc_dirty_saved == 13);
        else assert(g_pc_dirty_saved == 0 && g_pcsave_want == 1);
    }
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="pipeline-cache-tests-") as directory:
    temp = Path(directory)
    unit = temp / "test.c"
    unit.write_text(prelude + "\n" + path_declaration + "\nstatic void test_load(void) {\n" + load + "\n}\n" + write + save + tests, encoding="utf-8")
    exe = temp / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-missing-field-initializers", "-I", str(ROOT / "runtime"), str(unit), "-o", str(exe)], check=True)
    failed = []
    for case in ["valid", "bad_uuid", "bad_vendor", "bad_device", "bad_version", "bad_size", "truncated", "failed_query", "failed_write", "busy_writer", "successful_save", "invalid_exit_repair"]:
        run = temp / case; run.mkdir()
        result = subprocess.run([str(exe), case, str(run)], capture_output=True, text=True)
        print(f"{case}: {'PASS' if result.returncode == 0 else 'FAIL'}")
        if result.returncode:
            failed.append(case); print(result.stderr.strip())
    if failed:
        raise SystemExit(f"Failed pipeline cache behavior: {', '.join(failed)}")
print("M4 pipeline cache filesystem behaviors passed")
