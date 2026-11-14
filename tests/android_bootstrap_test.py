#!/usr/bin/env python3
"""Regression test for Android bootstrap order and SDL poll sentinel hint.
Verifies that SDL_SetHint(SDL_HINT_POLL_SENTINEL, "0") is called before
runtime loading and SDL display/event initialization, and that missing-game
failure behavior remains intact.
"""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN_C = ROOT / "android" / "app" / "src" / "main" / "cpp" / "main.c"


def get_compiler():
    if "HOST_CC" in os.environ:
        return shlex.split(os.environ["HOST_CC"])
    for candidate in ["clang", "gcc", "cc"]:
        found = shutil.which(candidate)
        if found:
            return [found]
    if os.name == "nt":
        msys_gcc = Path("C:/msys64/ucrt64/bin/gcc.exe")
        if msys_gcc.is_file():
            return [str(msys_gcc)]
    raise RuntimeError("No suitable C compiler found for bootstrap test")


MOCK_HEADERS = {
    "SDL3/SDL.h": """#pragma once
#define SDL_HINT_POLL_SENTINEL "SDL_POLL_SENTINEL"
#define SDL_HINT_VULKAN_LIBRARY "SDL_VULKAN_LIBRARY"
#define SDL_LOG_CATEGORY_APPLICATION 0
#define SDL_MESSAGEBOX_ERROR 0x10
int SDL_SetHint(const char *name, const char *value);
void SDL_ResetHint(const char *name);
void *SDL_GetAndroidJNIEnv(void);
void *SDL_GetAndroidActivity(void);
char *SDL_strdup(const char *text);
void SDL_free(void *ptr);
const char *SDL_GetAndroidExternalStoragePath(void);
void SDL_LogError(int category, const char *fmt, ...);
void SDL_LogWarn(int category, const char *fmt, ...);
void SDL_LogInfo(int category, const char *fmt, ...);
int SDL_ShowSimpleMessageBox(unsigned flags, const char *title, const char *message, void *window);
""",
    "SDL3/SDL_main.h": """#pragma once
""",
    "dlfcn.h": """#pragma once
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
#define RTLD_NOLOAD 4
void *dlopen(const char *filename, int flags);
int dlclose(void *handle);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);
""",
    "unistd.h": """#pragma once
#include <stddef.h>
#define R_OK 4
int chdir(const char *path);
int access(const char *path, int mode);
void _exit(int status);
int setenv(const char *name, const char *value, int overwrite);
char *getenv(const char *name);
""",
    "sys/stat.h": """#pragma once
struct stat { unsigned st_mode; };
#define S_ISREG(mode) (((mode) & 0170000) == 0100000)
int lstat(const char *path, struct stat *buf);
#if defined(_WIN32)
#include <direct.h>
int test_record_mkdir(const char *path);
#define mkdir(p, m) test_record_mkdir(p)
#else
int mkdir(const char *path, int mode);
#endif
""",
    "jni.h": """#pragma once
#define JNIEXPORT
#define JNICALL
#define JNI_FALSE 0
#define JNI_TRUE 1
typedef void *jobject;
typedef jobject jclass;
typedef jobject jobjectArray;
typedef jobject jstring;
typedef void *jmethodID;
typedef int jsize;
typedef unsigned char jboolean;
typedef const struct JNINativeInterface_ *JNIEnv;
struct JNINativeInterface_ {
    jclass (*GetObjectClass)(JNIEnv *, jobject);
    jmethodID (*GetMethodID)(JNIEnv *, jclass, const char *, const char *);
    jobject (*CallObjectMethod)(JNIEnv *, jobject, jmethodID, ...);
    void (*CallVoidMethod)(JNIEnv *, jobject, jmethodID, ...);
    jboolean (*CallBooleanMethod)(JNIEnv *, jobject, jmethodID, ...);
    jstring (*NewStringUTF)(JNIEnv *, const char *);
    jboolean (*ExceptionCheck)(JNIEnv *);
    jsize (*GetArrayLength)(JNIEnv *, jobjectArray);
    jobject (*GetObjectArrayElement)(JNIEnv *, jobjectArray, jsize);
    const char *(*GetStringUTFChars)(JNIEnv *, jstring, jboolean *);
    void (*ReleaseStringUTFChars)(JNIEnv *, jstring, const char *);
    void (*DeleteLocalRef)(JNIEnv *, jobject);
    void (*ExceptionClear)(JNIEnv *);
};
""",
}

TEST_DRIVER_C = r"""
#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <jni.h>
#include <sys/stat.h>

/* Tracked events */
typedef enum {
    EV_NONE = 0,
    EV_SET_HINT,
    EV_GET_STORAGE,
    EV_CHDIR,
    EV_FREOPEN,
    EV_ACCESS,
    EV_MKDIR,
    EV_SETENV,
    EV_DLOPEN,
    EV_DLSYM,
    EV_DISPLAY_RUN,
    EV_LOG_ERROR,
    EV_MESSAGE_BOX
} EventKind;

typedef struct {
    EventKind kind;
    char name[128];
    char value[256];
} TraceEvent;

static TraceEvent g_trace[64];
static int g_trace_count = 0;
static int g_test_mode = 0;

static void record(EventKind kind, const char *name, const char *val) {
    if (g_trace_count < 64) {
        g_trace[g_trace_count].kind = kind;
        strncpy(g_trace[g_trace_count].name, name ? name : "", sizeof(g_trace[0].name) - 1);
        strncpy(g_trace[g_trace_count].value, val ? val : "", sizeof(g_trace[0].value) - 1);
        g_trace_count++;
    }
}

/* Mock implementations */
static int g_jni_class_calls, g_jni_deletes, g_hint_resets;
static jclass mock_get_class(JNIEnv *env, jobject activity) {
    (void)env; (void)activity; ++g_jni_class_calls; return NULL;
}
static jboolean mock_exception_check(JNIEnv *env) { (void)env; return 0; }
static void mock_delete_ref(JNIEnv *env, jobject ref) { (void)env; (void)ref; ++g_jni_deletes; }
static const struct JNINativeInterface_ mock_jni = {
    .GetObjectClass = mock_get_class,
    .ExceptionCheck = mock_exception_check,
    .DeleteLocalRef = mock_delete_ref
};
static JNIEnv mock_env = &mock_jni;
void *SDL_GetAndroidJNIEnv(void) { return g_test_mode == 5 ? &mock_env : NULL; }
void *SDL_GetAndroidActivity(void) { return g_test_mode == 5 ? (void *)1 : NULL; }
void SDL_ResetHint(const char *name) { assert(!strcmp(name, "SDL_VULKAN_LIBRARY")); ++g_hint_resets; }
char *SDL_strdup(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}
void SDL_free(void *ptr) { free(ptr); }
int lstat(const char *path, struct stat *buf) { (void)path; (void)buf; return -1; }

int SDL_SetHint(const char *name, const char *value) {
    record(EV_SET_HINT, name, value);
    return 1;
}

const char *SDL_GetAndroidExternalStoragePath(void) {
    record(EV_GET_STORAGE, "storage_path", NULL);
    if (g_test_mode == 3) return NULL; /* Mode 3: storage failure */
    return "/fake/storage";
}

void SDL_LogError(int category, const char *fmt, ...) {
    (void)category;
    record(EV_LOG_ERROR, "log_error", fmt);
}

void SDL_LogWarn(int category, const char *fmt, ...) {
    (void)category;
    (void)fmt;
}

void SDL_LogInfo(int category, const char *fmt, ...) {
    (void)category;
    (void)fmt;
}

int SDL_ShowSimpleMessageBox(unsigned flags, const char *title, const char *message, void *window) {
    (void)flags; (void)window;
    record(EV_MESSAGE_BOX, title, message);
    return 0;
}

int chdir(const char *path) {
    record(EV_CHDIR, path, NULL);
    return 0;
}

int access(const char *path, int mode) {
    (void)mode;
    record(EV_ACCESS, path, NULL);
    if (g_test_mode == 1) {
        /* Mode 1: missing targets */
        return -1;
    }
    return 0;
}

#if defined(_WIN32)
int test_record_mkdir(const char *path) {
    record(EV_MKDIR, path, NULL);
    return 0;
}
#else
int mkdir(const char *path, int mode) {
    (void)mode;
    record(EV_MKDIR, path, NULL);
    return 0;
}
#endif

int setenv(const char *name, const char *value, int overwrite) {
    (void)overwrite;
    record(EV_SETENV, name, value);
    return 0;
}

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

static int mock_display_android_run(int argc, char **argv) {
    char buf[64];
    snprintf(buf, sizeof(buf), "argc=%d", argc);
    record(EV_DISPLAY_RUN, argv && argv[0] ? argv[0] : "", buf);
    return 0;
}

void *dlopen(const char *filename, int flags) {
    if (flags & RTLD_NOLOAD) return g_test_mode == 4 ? (void *)(uintptr_t)0x5678 : NULL;
    record(EV_DLOPEN, filename, NULL);
    return (void *)(uintptr_t)0x1234;
}

int dlclose(void *handle) { (void)handle; return 0; }
static int mock_exporting(void) { return 1; }

void *dlsym(void *handle, const char *symbol) {
    (void)handle;
    record(EV_DLSYM, symbol, NULL);
    if (strcmp(symbol, "wt_save_export_in_progress") == 0) return (void *)(uintptr_t)mock_exporting;
    if (strcmp(symbol, "display_android_run") == 0) {
        return (void *)(uintptr_t)mock_display_android_run;
    }
    return NULL;
}

char *dlerror(void) {
    return "mock dlerror";
}

/* Redefine freopen so it doesn't fail on /fake/storage/runtime.log */
FILE *test_freopen(const char *filename, const char *mode, FILE *stream) {
    (void)filename; (void)mode;
    record(EV_FREOPEN, filename, NULL);
    return stream;
}
#define freopen test_freopen

/* Include production main.c directly */
#include "main_tested.c"

int main(int argc, char **argv) {
    if (argc < 2) return 99;
    g_test_mode = atoi(argv[1]);
    int ret = SDL_main(0, NULL);
    if (g_test_mode == 5) {
        assert(g_jni_class_calls == 1 && g_jni_deletes == 1 && g_hint_resets == 1);
    }
    if (g_test_mode == 2) assert(g_jni_class_calls == 0 && g_hint_resets == 1);
    printf("MAIN_RET=%d\n", ret);
    for (int i = 0; i < g_trace_count; i++) {
        const char *kstr = "UNKNOWN";
        switch (g_trace[i].kind) {
            case EV_SET_HINT: kstr = "SET_HINT"; break;
            case EV_GET_STORAGE: kstr = "GET_STORAGE"; break;
            case EV_CHDIR: kstr = "CHDIR"; break;
            case EV_FREOPEN: kstr = "FREOPEN"; break;
            case EV_ACCESS: kstr = "ACCESS"; break;
            case EV_MKDIR: kstr = "MKDIR"; break;
            case EV_SETENV: kstr = "SETENV"; break;
            case EV_DLOPEN: kstr = "DLOPEN"; break;
            case EV_DLSYM: kstr = "DLSYM"; break;
            case EV_DISPLAY_RUN: kstr = "DISPLAY_RUN"; break;
            case EV_LOG_ERROR: kstr = "LOG_ERROR"; break;
            case EV_MESSAGE_BOX: kstr = "MESSAGE_BOX"; break;
            default: break;
        }
        printf("TRACE %d: %s | %s | %s\n", i, kstr, g_trace[i].name, g_trace[i].value);
    }
    return 0;
}
"""


class AndroidBootstrapOrderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = get_compiler()
        cls.tmpdir = tempfile.TemporaryDirectory()
        cls.tmppath = Path(cls.tmpdir.name)

        # Write mock headers
        for path_str, content in MOCK_HEADERS.items():
            full = cls.tmppath / path_str
            full.parent.mkdir(parents=True, exist_ok=True)
            full.write_text(content, encoding="utf-8")

        # Copy production main.c
        main_content = MAIN_C.read_text(encoding="utf-8")
        (cls.tmppath / "main_tested.c").write_text(main_content, encoding="utf-8")

        # Copy bootstrap diagnostic sources
        diag_h = MAIN_C.parent / "diag_bootstrap.h"
        diag_c = MAIN_C.parent / "diag_bootstrap.c"
        if diag_h.is_file():
            (cls.tmppath / "diag_bootstrap.h").write_text(diag_h.read_text(encoding="utf-8"), encoding="utf-8")
        if diag_c.is_file():
            (cls.tmppath / "diag_bootstrap.c").write_text(diag_c.read_text(encoding="utf-8"), encoding="utf-8")

        # Write test driver
        (cls.tmppath / "driver.c").write_text(TEST_DRIVER_C, encoding="utf-8")

        # Compile test binary
        cls.exe = cls.tmppath / ("bootstrap_test" + (".exe" if os.name == "nt" else ""))
        cmd = cls.cc + [
            "-O2",
            "-Wall",
            "-pthread",
            f"-I{cls.tmppath}",
            f"-I{MAIN_C.parent}",
            f"-I{ROOT / 'runtime'}",
            str(cls.tmppath / "driver.c"),
            str(cls.tmppath / "diag_bootstrap.c"),
            "-o",
            str(cls.exe),
        ]
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise RuntimeError(f"Build failed:\n{build.stderr}\nCommand: {' '.join(cmd)}")

    @classmethod
    def tearDownClass(cls):
        cls.tmpdir.cleanup()

    def run_mode(self, mode: int):
        proc = subprocess.run([str(self.exe), str(mode)], capture_output=True, text=True, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        lines = proc.stdout.strip().splitlines()
        ret_val = None
        events = []
        for line in lines:
            if line.startswith("MAIN_RET="):
                ret_val = int(line.split("=", 1)[1])
            elif line.startswith("TRACE "):
                payload = line.split(":", 1)[1].strip("\r\n")
                if payload.startswith(" "):
                    payload = payload[1:]
                parts = payload.split(" | ")
                kind = parts[0]
                name = parts[1] if len(parts) > 1 else ""
                val = parts[2] if len(parts) > 2 else ""
                events.append({"kind": kind, "name": name, "value": val})
        return ret_val, events

    def test_missing_game_files_failure(self):
        # Mode 1: missing game files
        ret, events = self.run_mode(1)
        self.assertEqual(ret, 1)

        # Check event order: SET_HINT must be first
        self.assertGreater(len(events), 0)
        self.assertEqual(events[0]["kind"], "SET_HINT")
        self.assertEqual(events[0]["name"], "SDL_POLL_SENTINEL")
        self.assertEqual(events[0]["value"], "0")

        # Must report missing files via message box
        msg_boxes = [e for e in events if e["kind"] == "MESSAGE_BOX"]
        self.assertEqual(len(msg_boxes), 1)
        self.assertIn("Missing your locally converted game files", msg_boxes[0]["value"])

        # DLOPEN and DISPLAY_RUN must NEVER have been called
        dlopens = [e for e in events if e["kind"] == "DLOPEN"]
        self.assertEqual(len(dlopens), 0)
        runs = [e for e in events if e["kind"] == "DISPLAY_RUN"]
        self.assertEqual(len(runs), 0)

    def test_successful_bootstrap_order(self):
        # Mode 2: all game files present
        ret, events = self.run_mode(2)
        self.assertEqual(ret, 0)

        # Event sequence indices
        idx_set_hint = None
        idx_dlopen = None
        idx_display_run = None

        for i, e in enumerate(events):
            if e["kind"] == "SET_HINT" and e["name"] == "SDL_POLL_SENTINEL":
                idx_set_hint = i
            elif e["kind"] == "DLOPEN" and "libwitchtime.so" in e["name"]:
                idx_dlopen = i
            elif e["kind"] == "DISPLAY_RUN":
                idx_display_run = i

        self.assertIsNotNone(idx_set_hint, "SDL_SetHint was not called")
        self.assertIsNotNone(idx_dlopen, "dlopen was not called")
        self.assertIsNotNone(idx_display_run, "display_android_run was not called")

        # Crucial ordering assertion: SET_HINT before DLOPEN and before DISPLAY_RUN
        self.assertLess(idx_set_hint, idx_dlopen, "SDL_SetHint must be called before dlopen")
        self.assertLess(idx_dlopen, idx_display_run, "dlopen must precede display_android_run")
        # Ordinary startup must not acquire diagnostic trace ownership.
        self.assertFalse(any(e["kind"] == "SETENV" and e["name"] == "WT_TRACE" for e in events))

    def test_storage_failure(self):
        # Mode 3: SDL_GetAndroidExternalStoragePath returns NULL
        ret, events = self.run_mode(3)
        self.assertEqual(ret, 1)

        # SET_HINT was still called first
        self.assertEqual(events[0]["kind"], "SET_HINT")
        self.assertEqual(events[0]["name"], "SDL_POLL_SENTINEL")
        self.assertEqual(events[0]["value"], "0")

        # Error message shown
        msg_boxes = [e for e in events if e["kind"] == "MESSAGE_BOX"]
        self.assertEqual(len(msg_boxes), 1)
        self.assertIn("Cannot open the app's private external files directory", msg_boxes[0]["value"])

        # No dlopen
        self.assertEqual(len([e for e in events if e["kind"] == "DLOPEN"]), 0)

    def test_jni_class_failure_falls_back_without_custom_library(self):
        ret, events = self.run_mode(5)
        self.assertEqual(ret, 0)
        self.assertEqual([e['name'] for e in events if e['kind'] == 'DLOPEN'], ['libwitchtime.so'])
        self.assertEqual(len([e for e in events if e['kind'] == 'DISPLAY_RUN']), 1)
        self.assertFalse(any(e['kind'] == 'SET_HINT' and e['name'] == 'SDL_VULKAN_LIBRARY' for e in events))

    def test_export_blocks_game_before_storage_or_runtime(self):
        ret, events = self.run_mode(4)
        self.assertEqual(ret, 1)
        self.assertFalse(any(e['kind'] in ('GET_STORAGE', 'FREOPEN', 'MKDIR', 'SETENV', 'DLOPEN', 'DISPLAY_RUN') for e in events))



if __name__ == "__main__":
    unittest.main()
