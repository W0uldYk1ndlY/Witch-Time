"""Host-only native restart checks; never opens a driver or device."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeRestartTests(unittest.TestCase):
    def compile_run(self, source, headers=None):
        compiler = shutil.which("gcc") or shutil.which("clang")
        self.assertIsNotNone(compiler, "Host C compiler required")
        base = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir()))
        temp_root = base / "Temp" / "opencode" if os.name == "nt" else Path(tempfile.gettempdir())
        temp_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="wt_restart_", dir=temp_root) as scratch:
            scratch = Path(scratch)
            for name, contents in (headers or {}).items():
                header = scratch / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(contents)
            if isinstance(source, str):
                path = scratch / "test.c"
                path.write_text(source)
            else:
                path = source
            executable = scratch / "test.exe"
            command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(scratch), str(path), "-o", str(executable)]
            subprocess.run(command, check=True, timeout=30)
            subprocess.run([str(executable)], check=True, timeout=15)

    def test_admission_and_lease(self):
        self.compile_run(ROOT / "tests/native_driver_restart_test.c")

    def test_every_special_route_executes_admission_wrapper(self):
        source = (ROOT / "runtime/nvn_stub.c").read_text()
        wrappers = re.findall(r"^SPECIAL_WRAPPER\((\w+)\)", source, re.M)
        table = re.search(r"special\[\] = \{(.*?)\n    \};", source, re.S).group(1)
        entries = re.findall(r'\{"([^"]+)", SPECIAL_FN\((\w+)\)\}', table)
        self.assertEqual(len(entries), table.count('{"'))
        self.assertEqual(set(wrappers), {handler for _, handler in entries})
        wrapper_macro = re.search(r"^#define SPECIAL_WRAPPER.*$", source, re.M).group(0)
        program = '#include <assert.h>\n#include <stdio.h>\ntypedef struct { int called; } Ctx;\n'
        program += 'static int depth, enters, leaves;\nvoid wt_gpu_restart_enter(void) { ++depth; ++enters; }\nvoid wt_gpu_restart_leave(void) { assert(depth == 1); --depth; ++leaves; }\n'
        for handler in wrappers:
            program += f'static void {handler}(Ctx *c) {{ assert(depth == 1); ++c->called; }}\n'
        program += wrapper_macro + '\n'
        program += '\n'.join(f'SPECIAL_WRAPPER({handler})' for handler in wrappers)
        program += '\n#define SPECIAL_FN(fn) restart_##fn\nstatic const struct { const char *name; void (*fn)(Ctx *); } special[] = {' + table + '\n};\n'
        program += 'int main(void) { Ctx c = {0}; for (unsigned i=0; i<sizeof special/sizeof *special; ++i) { special[i].fn(&c); assert(depth == 0); } assert(c.called == enters && enters == leaves); puts("all native NVN special routes admitted and balanced"); return 0; }\n'
        self.compile_run(program)

    def test_mutation_boundaries_have_outer_admission(self):
        source = (ROOT / "runtime/nnfs.c").read_text()
        names = ["fs_roots", "fs_delete_directory", "mount_add", "fs_MountRom", "fs_MountSaveData", "fs_Unmount", "fs_EnsureSaveData", "fs_Commit", "fs_OpenFile", "fs_CloseFile", "fs_WriteFile", "fs_FlushFile", "fs_SetFileSize", "fs_CreateFile", "fs_DeleteFile", "fs_CreateDirectory", "fs_mount_plain"]
        for name in names:
            self.assertRegex(source, rf"\b{name}\([^;{{}}]*\)\s*\{{\s*(?:/\*.*?\*/\s*)?FS_ADMIT\(\);", name)

    def test_production_fs_reentrant_admission(self):
        source = (ROOT / "runtime/nnfs.c").read_text()
        admission = re.search(r'(static WtRestartAdmission g_fs_admission.*?)(?=\n#else)', source, re.S).group(1)
        header = (ROOT / "runtime/restart_admission.h").as_posix()
        program = f'#include <assert.h>\n#include <sched.h>\n#include "{header}"\n' + admission
        program += r'''
static int ready;
static pthread_t closer;
static void *close_thread(void *unused) { (void)unused; ready = wt_fs_restart_prepare(1000); return NULL; }
static void nested_mutation(void) { FS_ADMIT(); assert(g_fs_admission.active == 1); }
static void outer_mutation(void) {
    FS_ADMIT();
    assert(!pthread_create(&closer, NULL, close_thread, NULL));
    for (;;) {
        pthread_mutex_lock(&g_fs_admission.mutex);
        int closed = g_fs_admission.closed;
        pthread_mutex_unlock(&g_fs_admission.mutex);
        if (closed) break;
        sched_yield();
    }
    nested_mutation();
    /* Return cleanup must release the single outer count; don't join while admitted. */
}
int main(void) {
    outer_mutation();
    pthread_join(closer, NULL);
    assert(ready && !g_fs_admission.active);
    wt_fs_restart_cancel();
    nested_mutation();
    return 0;
}
'''
        self.compile_run(program)

    def test_bundled_profile_environment_and_fallback(self):
        bridge = (ROOT / "android/app/src/main/cpp/wt_gpu_driver.c").as_posix()
        headers = {
            "adrenotools/driver.h": '#define ADRENOTOOLS_DRIVER_CUSTOM 1\nvoid *adrenotools_open_libvulkan(int, int, void *, const char *, const char *, const char *, void *, void *);\n',
            "vulkan/vulkan.h": '#define VKAPI_ATTR\n#define VKAPI_CALL\ntypedef void *VkInstance;\ntypedef void (*PFN_vkVoidFunction)(void);\ntypedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char *);\n',
            "dlfcn.h": '#define RTLD_NOW 1\n#define RTLD_LOCAL 0\nvoid *dlsym(void *, const char *);\n',
        }
        program = r'''
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
static char environment[128];
static int environment_present, fail_loader;
static char *fake_getenv(const char *key) { assert(!strcmp(key, "TU_DEBUG")); return environment_present ? environment : NULL; }
static int fake_setenv(const char *key, const char *value, int overwrite) { assert(!strcmp(key, "TU_DEBUG") && overwrite); strcpy(environment, value); environment_present = 1; return 0; }
static int fake_unsetenv(const char *key) { assert(!strcmp(key, "TU_DEBUG")); environment_present = 0; return 0; }
static int fake_lstat(const char *path, struct stat *out) { assert(!strcmp(path, "/marker")); memset(out, 0, sizeof *out); out->st_mode = S_IFREG; return 0; }
#define getenv fake_getenv
#define setenv fake_setenv
#define unsetenv fake_unsetenv
#define lstat fake_lstat
'''
        program += f'\n#include "{bridge}"\n'
        program += r'''
void *adrenotools_open_libvulkan(int mode, int kind, void *unused, const char *native, const char *package, const char *library, void *a, void *b) {
    (void)mode; (void)kind; (void)unused; (void)native; (void)package; (void)library; (void)a; (void)b;
    return fail_loader ? NULL : (void *)1;
}
void *dlsym(void *handle, const char *name) { (void)handle; (void)name; return (void *)1; }
static void reset(void) {
    wt_android_driver_restore_environment();
    free(owned_marker); owned_marker = NULL;
    attempted = 0; loader = NULL; loader_gipa = NULL;
}
int main(void) {
    const char *profile = "turnip-adreno830-unleashed-mesa26.3-gralloc";
    assert(wt_android_driver_open("/native", "/package/", "driver.so", "/marker", profile));
    assert(environment_present && !strcmp(environment, "sysmem,nobin"));
    reset(); assert(!environment_present);
    fake_setenv("TU_DEBUG", "original", 1);
    assert(wt_android_driver_open("/native", "/package/", "driver.so", "/marker", ""));
    assert(!strcmp(environment, "original")); reset();
    fail_loader = 1;
    assert(!wt_android_driver_open("/native", "/package/", "driver.so", "/marker", profile));
    assert(!strcmp(environment, "original")); reset();
    fake_unsetenv("TU_DEBUG");
    assert(!wt_android_driver_open("/native", "/package/", "driver.so", "/marker", profile));
    assert(!environment_present); reset();
    puts("native bundled/imported/fallback environment assertions passed"); return 0;
}
'''
        self.compile_run(program, headers)


if __name__ == "__main__":
    unittest.main()
