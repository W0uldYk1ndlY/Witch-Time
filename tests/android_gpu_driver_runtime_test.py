"""Compile the runtime's actual selection/callback code against host mocks; no device."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RuntimeDriverTest(unittest.TestCase):
    def test_runtime_contract(self):
        core = (ROOT / "runtime/nvn_vk.c").read_text()
        draw = (ROOT / "runtime/nvn_vk_draw.c").read_text()
        helpers = core.split("/* Bootstrap sets these", 1)[1]
        helpers = "#ifdef __ANDROID__\n/* Bootstrap sets these" + helpers.split("static const char *vk_result_name", 1)[0]
        selection = core.split("    V.hwnd = hwnd;", 1)[1].split("    if (!V.loader)", 1)[0]
        sites = re.findall(r"(?:VkResult )?r = vkQueuePresentKHR\(V.queue, &pr\);\s*vk_presented\(r\);", core + draw)
        self.assertEqual(len(sites), 3)
        deferred = draw.split("if (!progress && display_present_defer())", 1)[1].split("} else {", 1)[0]
        self.assertNotIn("vk_presented", deferred)
        self.assertIn("r = VK_SUCCESS;", deferred)
        compiler = shutil.which("gcc") or shutil.which("clang")
        self.assertIsNotNone(compiler, "A host C compiler is required")
        source = r'''
#include <assert.h>
#include <stdio.h>
typedef int VkResult;
#define VK_SUCCESS 0
#define VK_SUBOPTIMAL_KHR 1000001003
static struct { void *loader; int queue; } V;
static int loads, callbacks, presents, result;
static void *LoadLibraryA(const char *name) { (void)name; ++loads; return (void *)1; }
static void callback(void) { ++callbacks; }
static int vkQueuePresentKHR(int queue, void *pr) { (void)queue; (void)pr; ++presents; return result; }
''' + helpers + '\nstatic void select_loader(void) {\n' + selection + r'''
}
static void present(int site) {
    int pr = 0; VkResult r;
    switch (site) {
''' + '\n'.join(f'case {i}: {{ {s.replace("VkResult r =", "r =")} break; }}' for i, s in enumerate(sites)) + r'''
    }
}
int main(void) {
#ifdef __ANDROID__
    nvn_vk_android_set_driver(NULL, callback);
    select_loader(); assert(V.loader == (void *)1 && loads == 1);
    nvn_vk_android_set_driver((void *)9, NULL);
    assert(g_android_vk_loader == NULL && g_android_vk_presented == callback);
    g_android_vk_init_started = 0;
    nvn_vk_android_set_driver((void *)2, callback);
    select_loader(); assert(V.loader == (void *)2 && loads == 1);
    for (int site = 0; site < 3; ++site) {
        g_android_vk_presented = callback;
        int before = callbacks;
        result = -4; present(site); assert(callbacks == before);
        /* A queued/deferred frame has only synthetic success, not a present call. */
        int prior_presents = presents; result = VK_SUCCESS;
        assert(callbacks == before && presents == prior_presents);
        result = site == 1 ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
        present(site); assert(callbacks == before + 1);
        present(site); assert(callbacks == before + 1);
    }
#else
    select_loader(); assert(V.loader == (void *)1 && loads == 1);
    for (int site = 0; site < 3; ++site) { result = VK_SUCCESS; present(site); }
    assert(callbacks == 0 && presents == 3);
#endif
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="android_gpu_driver_runtime_", dir=os.environ.get("TEMP")) as tmp:
            path = Path(tmp) / "mock.c"
            path.write_text(source)
            for android in (False, True):
                exe = Path(tmp) / ("android.exe" if android else "desktop.exe")
                command = [compiler, "-std=c11", "-Werror", str(path), "-o", str(exe)]
                if android:
                    command.append("-D__ANDROID__")
                subprocess.run(command, check=True, capture_output=True, text=True)
                subprocess.run([str(exe)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
