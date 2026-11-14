#!/usr/bin/env python3
"""Run the standalone touch model and persistence checks without the game."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
compiler = os.environ.get("HOST_CC") or shutil.which("clang") or shutil.which("gcc")
if not compiler:
    raise RuntimeError("No host C compiler found")

with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    doc = (ROOT / "docs/ANDROID_TOUCH_CONTROLS.md").read_text(encoding="utf-8")
    example = temp / "example.ini"
    example.write_text(doc.split("```ini\n", 1)[1].split("\n```", 1)[0] + "\n", encoding="utf-8")
    for name in ("touch_controls_selftest", "touch_config_selftest"):
        exe = temp / (name + (".exe" if os.name == "nt" else ""))
        subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests" / (name + ".c")), str(ROOT / "runtime/touch_controls.c"),
                        "-lm", "-o", str(exe)], check=True)
        args = [str(exe)]
        if name == "touch_config_selftest":
            config_dir = temp / "config"
            config_dir.mkdir()
            args += [str(config_dir), str(example)]
        subprocess.run(args, check=True)
