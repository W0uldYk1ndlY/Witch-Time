#!/usr/bin/env python3
"""Run the menu self-test with embedded assets expanded for older host compilers."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
compiler = os.environ.get("HOST_CC") or shutil.which("gcc") or shutil.which("clang")
if not compiler:
    raise RuntimeError("No host C compiler found")
with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    source = (ROOT / "runtime/pcmenu.c").read_text(encoding="utf-8")
    def embed(match):
        data = (ROOT / "runtime" / match.group(1)).read_bytes()
        return ",".join(str(value) for value in data)
    source = re.sub(r'^\s*#embed "([^"]+)"', embed, source, flags=re.MULTILINE)
    (temp / "pcmenu.c").write_text(source, encoding="utf-8")
    test = (ROOT / "tests/pcmenu_selftest.c").read_text(encoding="utf-8")
    test = test.replace('#include "../runtime/pcmenu.c"', '#include "pcmenu.c"')
    (temp / "test.c").write_text(test, encoding="utf-8")
    for android in (False, True):
        exe = temp / (("android" if android else "desktop") + (".exe" if os.name == "nt" else ""))
        cmd = [compiler, "-std=gnu11", "-D_GNU_SOURCE", "-D__declspec(x)=", "-I" + str(ROOT / "runtime"),
               "-I" + str(ROOT / "runtime/gen"), str(temp / "test.c"), str(ROOT / "runtime/gfx_options.c")]
        if android:
            cmd += ["-D__ANDROID__", str(ROOT / "runtime/touch_controls.c")]
        if os.name != "nt":
            cmd += ["-I" + str(ROOT / "runtime/posix"), str(ROOT / "runtime/plat_posix.c")]
        cmd += ["-lm", "-o", str(exe)]
        subprocess.run(cmd, check=True)
        result = subprocess.run([str(exe), str(temp / ("android_output" if android else "desktop_output"))], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"menu self-test failed ({'Android guards' if android else 'desktop'}):\n{result.stdout}\n{result.stderr}")
        print("menu self-test passed:", "Android guards" if android else "desktop")
