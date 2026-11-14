#!/usr/bin/env python3
"""Host-check Android HID composition using production handlers and mocked devices."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cc = os.environ.get("HOST_CC") or shutil.which("gcc") or shutil.which("cc")
if not cc:
    raise RuntimeError("C compiler required")
with tempfile.TemporaryDirectory(prefix="touch-hid-") as temp:
    exe = Path(temp) / ("touch_hid.exe" if os.name == "nt" else "touch_hid")
    cmd = [cc, "-std=gnu11", "-D__ANDROID__", "-D__declspec(x)=", "-ffunction-sections", "-fdata-sections",
           "-I" + str(root / "runtime"), str(root / "tests/touch_hid_selftest.c"), str(root / "runtime/touch_controls.c"),
           str(root / "runtime/diag_replay.c"), "-Wl,--gc-sections", "-lm", "-o", str(exe)]
    if os.name != "nt": cmd.insert(4, "-I" + str(root / "runtime/posix"))
    subprocess.run(cmd, check=True)
    env = os.environ.copy(); env["WT_REC"] = str(Path(temp) / "record.rec")
    subprocess.run([str(exe)], env=env, check=True)
    replay = Path(temp) / "input.rec"
    replay.write_text("0 4 123 456\n", encoding="ascii")
    env["WT_PLAY"] = str(replay)
    subprocess.run([str(exe), "wt-play"], env=env, check=True)
