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
#define SDL_LOG_CATEGORY_APPLICATION 0
#define SDL_MESSAGEBOX_ERROR 0x10
int SDL_SetHint(const char *name, const char *value);
const char *SDL_GetAndroidExternalStoragePath(void);
void SDL_LogError(int category, const char *fmt, ...);
int SDL_ShowSimpleMessageBox(unsigned flags, const char *title, const char *message, void *window);
""",
    "SDL3/SDL_main.h": """#pragma once
""",
    "dlfcn.h": """#pragma once
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);
""",
    "unistd.h": """#pragma once
#include <stddef.h>
#define R_OK 4
int chdir(const char *path);
int access(const char *path, int mode);
int setenv(const char *name, const char *value, int overwrite);
char *getenv(const char *name);
""",
    "sys/stat.h": """#pragma once
int mkdir(const char *path, int mode);
""",
}

TEST_DRIVER_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

int mkdir(const char *path, int mode) {
    (void)mode;
    record(EV_MKDIR, path, NULL);
    return 0;
}

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
    (void)flags;
    record(EV_DLOPEN, filename, NULL);
    return (void *)(uintptr_t)0x1234;
}

void *dlsym(void *handle, const char *symbol) {
    (void)handle;
    record(EV_DLSYM, symbol, NULL);
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

        # Write test driver
        (cls.tmppath / "driver.c").write_text(TEST_DRIVER_C, encoding="utf-8")

        # Compile test binary
        cls.exe = cls.tmppath / ("bootstrap_test" + (".exe" if os.name == "nt" else ""))
        cmd = cls.cc + [
            "-O2",
            "-Wall",
            f"-I{cls.tmppath}",
            str(cls.tmppath / "driver.c"),
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


if __name__ == "__main__":
    unittest.main()
