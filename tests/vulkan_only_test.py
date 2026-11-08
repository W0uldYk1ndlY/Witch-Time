#!/usr/bin/env python3
"""Compile the real NVN front end without desktop GL headers or linkage.

Run with python3 tests/vulkan_only_test.py; CC and NM may select a cross toolchain.
This is a compile/link-dependency test, not an Android device or gameplay test.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class VulkanOnlyTest(unittest.TestCase):
    def test_legacy_gl_call_is_fatal(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if any("android" in x for x in cc):
            self.skipTest("Cross-compiled binaries require an Android device")
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "trap.c", Path(tmp) / "trap"
            src.write_text('#include "nvn_gl_api.h"\nint main(void) { glClear(GL_COLOR_BUFFER_BIT); return 0; }\n')
            subprocess.run(cc + ["-DWT_VULKAN_ONLY=1", "-I", str(ROOT / "runtime"), str(src), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 5)
            self.assertIn("unavailable desktop OpenGL backend", result.stderr)

    def test_frontend_has_no_opengl_imports(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            # A poison header catches accidental system OpenGL dependencies even
            # on development machines which have Mesa installed.
            (tmp / "GL").mkdir()
            (tmp / "GL/gl.h").write_text('#error Desktop OpenGL is forbidden in a Vulkan-only build\n')
            for source in ("nvn_gl.c", "nvn_c4.c"):
                obj = tmp / (source + ".o")
                command = shlex.split(os.environ.get("CC", "cc")) + [
                    "-std=gnu11", "-O2", "-D_GNU_SOURCE", "-DWT_VULKAN_ONLY=1",
                    "-D__declspec(x)=", "-I", str(tmp), "-I", str(ROOT / "runtime/posix"),
                    "-I", str(ROOT / "runtime"), "-c", str(ROOT / "runtime" / source), "-o", str(obj)]
                build = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                symbols = subprocess.check_output(shlex.split(os.environ.get("NM", "nm")) + ["-u", str(obj)], text=True)
                self.assertFalse(re.search(r"\b(?:gl[A-Z]|wgl[A-Z])\w*", symbols), symbols)

if __name__ == "__main__":
    unittest.main()
