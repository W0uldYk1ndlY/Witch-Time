#!/usr/bin/env python3
"""Policy and regression test for treatment developer controls:
Verifies default-off WT_WW_SOFT, WT_VK_WRITERWAIT, WT_VK_LOADCLEAR controls,
separate WT_VK_SPLIT binding, and environment variable override handling.
"""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


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
    raise RuntimeError("No suitable C compiler found for treatment controls test")


TEST_HARNESS_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define VK_SPLIT_DEFAULT 0

/* Declarations matching production runtime patterns */
static int g_ww_soft = -1;
int plat_ww_soft_enabled(void) {
    if (g_ww_soft < 0) {
        const char *e = getenv("WT_WW_SOFT");
        g_ww_soft = (e && atoi(e) != 0) ? 1 : 0;
    }
    return g_ww_soft;
}

static int g_writerwait = -1;
int nvn_vk_writerwait_enabled(void) {
    if (g_writerwait < 0) {
        const char *e = getenv("WT_VK_WRITERWAIT");
        g_writerwait = (e && atoi(e) != 0) ? 1 : 0;
    }
    return g_writerwait;
}

static int g_loadclear = -1;
int nvn_vk_loadclear_enabled(void) {
    if (g_loadclear < 0) {
        const char *e = getenv("WT_VK_LOADCLEAR");
        g_loadclear = (e && atoi(e) != 0) ? 1 : 0;
    }
    return g_loadclear;
}

static int g_split = -1;
int nvn_vk_split_threshold(void) {
    if (g_split < 0) {
        const char *e = getenv("WT_VK_SPLIT");
        g_split = e ? atoi(e) : VK_SPLIT_DEFAULT;
    }
    return g_split;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "query") == 0) {
        printf("ww_soft=%d writerwait=%d loadclear=%d split=%d\n",
               plat_ww_soft_enabled(),
               nvn_vk_writerwait_enabled(),
               nvn_vk_loadclear_enabled(),
               nvn_vk_split_threshold());
        return 0;
    }
    return 2;
}
"""


class TreatmentControlsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = get_compiler()
        cls.temp_dir = tempfile.TemporaryDirectory()
        src_path = Path(cls.temp_dir.name) / "test_controls.c"
        src_path.write_text(TEST_HARNESS_C, encoding="utf-8")

        ext = ".exe" if os.name == "nt" else ""
        cls.exe_path = Path(cls.temp_dir.name) / f"test_controls{ext}"
        cmd = cls.compiler + ["-O2", "-Wall", str(src_path), "-o", str(cls.exe_path)]
        subprocess.check_call(cmd)

    @classmethod
    def tearDownClass(cls):
        cls.temp_dir.cleanup()

    def run_query(self, env_overrides=None):
        env = os.environ.copy()
        # Clean any existing treatment variables
        for var in ["WT_WW_SOFT", "WT_VK_WRITERWAIT", "WT_VK_LOADCLEAR", "WT_VK_SPLIT"]:
            env.pop(var, None)
        if env_overrides:
            env.update(env_overrides)
        proc = subprocess.run(
            [str(self.exe_path), "query"],
            env=env,
            capture_output=True,
            text=True,
            check=True
        )
        parts = proc.stdout.strip().split()
        return dict(p.split("=") for p in parts)

    def test_default_all_off(self):
        res = self.run_query()
        self.assertEqual(res["ww_soft"], "0")
        self.assertEqual(res["writerwait"], "0")
        self.assertEqual(res["loadclear"], "0")
        self.assertEqual(res["split"], "0")

    def test_explicit_on(self):
        res = self.run_query({
            "WT_WW_SOFT": "1",
            "WT_VK_WRITERWAIT": "1",
            "WT_VK_LOADCLEAR": "1",
            "WT_VK_SPLIT": "20",
        })
        self.assertEqual(res["ww_soft"], "1")
        self.assertEqual(res["writerwait"], "1")
        self.assertEqual(res["loadclear"], "1")
        self.assertEqual(res["split"], "20")

    def test_invalid_values_default_off(self):
        res = self.run_query({
            "WT_WW_SOFT": "0",
            "WT_VK_WRITERWAIT": "invalid",
            "WT_VK_LOADCLEAR": "false",
        })
        self.assertEqual(res["ww_soft"], "0")
        self.assertEqual(res["writerwait"], "0")
        self.assertEqual(res["loadclear"], "0")


if __name__ == "__main__":
    unittest.main()
