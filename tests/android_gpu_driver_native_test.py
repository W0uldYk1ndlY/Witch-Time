"""Focused host mocks plus bootstrap/build source contracts; no device access."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "android/app/src/main/cpp"


class NativeDriverTests(unittest.TestCase):
    def test_bootstrap_isolation_and_order(self):
        source = (CPP / "main.c").read_text()
        startup = source[source.index("int SDL_main("):]
        self.assertLess(startup.index("diag_bootstrap_apply_session_environment"), startup.index("prepare_driver()"))
        self.assertLess(startup.index('access("targets/exefs/main.elf"'), startup.index("prepare_driver()"))
        self.assertIn("if (!diag_mode) driver = prepare_driver();", startup)
        self.assertLess(startup.index("prepare_driver()"), startup.index('dlopen("libwitchtime.so"'))
        self.assertLess(startup.index("set_driver(driver.loader, driver.presented)"), startup.index("int result = run("))
        self.assertIn('if (!set_driver) return fail(', startup)
        prepare = source[source.index("static WtDriver prepare_driver"):source.index("int SDL_main(")]
        self.assertLess(prepare.index('"prepareGpuDriver"'), prepare.index('dlopen(bridge_path'))
        self.assertLess(prepare.index("lstat(values[3]"), prepare.index('dlopen(bridge_path'))
        self.assertIn("SDL_ResetHint(SDL_HINT_VULKAN_LIBRARY)", prepare)
        self.assertIn("ExceptionClear", prepare)
        self.assertNotIn("unlink", prepare)

    def test_build_is_lazy_pinned_and_aligned(self):
        cmake = (CPP / "CMakeLists.txt").read_text()
        self.assertIn("project(witch_time_android C CXX)", cmake)
        main_link = next(line for line in cmake.splitlines() if "target_link_libraries(main " in line)
        self.assertNotIn("adrenotools", main_link)
        self.assertNotIn("wt_gpu_driver", main_link)
        deps = (ROOT / "android/third_party/gpu_driver/CMakeLists.txt").read_text()
        for revision in ("8fae8ce254dfc1344527e05301e43f37dea2df80", "aa3975893d83ef1bc84c321ec60c65fbf1287887"):
            self.assertIn(revision, deps)
        self.assertIn("max-page-size=16384", deps)
        self.assertIn("hook_impl main_hook file_redirect_hook gsl_alloc_hook", deps)
        gradle = (ROOT / "android/app/build.gradle").read_text()
        self.assertIn("useLegacyPackaging = true", gradle)
        self.assertIn("assets.srcDir '../third_party/gpu_driver'", gradle)
        for name in ("libadrenotools", "liblinkernsbypass"):
            self.assertIn("BSD 2-Clause License", (ROOT / f"android/third_party/gpu_driver/{name}.LICENSE").read_text())

    def test_bridge_executable(self):
        cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
        self.assertIsNotNone(cc, "Set CC to a host C compiler for executable mock checks")
        with tempfile.TemporaryDirectory(prefix="wt-gpu-native-") as directory:
            tmp = Path(directory)
            headers = {
                "dlfcn.h": "#define RTLD_NOW 2\n#define RTLD_LOCAL 0\nvoid *dlsym(void *, const char *);\n",
                "unistd.h": "",
                "adrenotools/driver.h": "#include <stdint.h>\n#define ADRENOTOOLS_DRIVER_CUSTOM 1\nvoid *adrenotools_open_libvulkan(int,int,const char*,const char*,const char*,const char*,const char*,void**);\n",
                "vulkan/vulkan.h": "#define VKAPI_ATTR\n#define VKAPI_CALL\ntypedef void *VkInstance;\ntypedef void (*PFN_vkVoidFunction)(void);\ntypedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance,const char*);\n",
            }
            for name, text in headers.items():
                path = tmp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            exe = tmp / ("bridge.exe" if os.name == "nt" else "bridge")
            subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp),
                            str(ROOT / "tests/android_gpu_driver_native_test.c"), "-o", str(exe)], check=True)
            for mode in range(6):
                with self.subTest(mode=mode):
                    subprocess.run([str(exe), str(mode)], check=True)


if __name__ == "__main__":
    unittest.main()
