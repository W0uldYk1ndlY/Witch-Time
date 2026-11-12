"""Run a disposable headless SGSR HDR GPU check using existing tools and headers.

Usage: python tests/sgsr_gpu_test.py <glslc> <spirv-dis>
Requires an existing Vulkan 1.3 graphics device; never installs or contacts an app.
"""
import ast
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from sgsr_source_test import main as compile_fragments

ROOT = Path(__file__).resolve().parents[1]


def main(glslc, dis):
    fragments = compile_fragments(glslc, dis)
    with tempfile.TemporaryDirectory(prefix="sgsr-gpu-") as directory:
        directory = Path(directory)
        paths = []
        for name in ["sgsr2_convert.fs", "sgsr2_upscale.fs"]:
            path = directory / (name + ".spv")
            path.write_bytes(fragments[name]); paths.append(str(path))
        header = (ROOT / "runtime/sgsr2_vertex.h").read_text(encoding="utf-8")
        body = header.split("static const char k_sgsr2_vs[] =", 1)[1].split(";\n", 1)[0]
        vertex = "".join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', body))
        src, spv = directory / "vertex.vert", directory / "vertex.spv"
        src.write_text(vertex, encoding="utf-8")
        subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=vertex", str(src), "-o", str(spv)], check=True)
        exe = directory / ("gpu.exe" if os.name == "nt" else "gpu")
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
                   "-I", str(ROOT / "runtime"), "-I", str(ROOT / "deps/android-arm64/include"),
                   str(ROOT / "tests/sgsr_gpu_test.c"), "-o", str(exe)]
        if os.name != "nt": command += ["-ldl", "-lm"]
        subprocess.run(command, check=True)
        for case in [(17, 13, 17, 13, 4), (17, 13, 25, 19, 4), (1, 1, 1, 1, 4),
                     (17, 13, 25, 19, 0), (17, 13, 25, 19, 1000), (17, 13, 25, 19, 60000)]:
            for reset in (1, 0):
                subprocess.run([str(exe), str(spv), *paths, *map(str, case), str(reset)], check=True, timeout=60)
        for case in [(17, 13, 17, 13, 4), (17, 13, 25, 19, 4), (1, 1, 1, 1, 4)]:
            subprocess.run([str(exe), str(spv), *paths, *map(str, case), "1", "1"], check=True, timeout=60)
        for current, previous in [(1000, 4), (4, 1000), (60000, 0), (0, 60000)]:
            subprocess.run([str(exe), str(spv), *paths, "17", "13", "25", "19", str(current), "0", "0", str(previous)], check=True, timeout=60)
        for camera in (1, 2, 3, 4, 5):
            subprocess.run([str(exe), str(spv), *paths, "17", "13", "25", "19", "4", "0"],
                           env={**os.environ, "WT_SGSR_TEST_CAMERA": str(camera)}, check=True, timeout=60)
        for intensity, reset in [(4, 0), (0, 1)]:
            negative = subprocess.run([str(exe), str(spv), *paths, "17", "13", "25", "19", str(intensity), str(reset)],
                                      env={**os.environ, "WT_SGSR_TEST_SKIP_FINAL": "1"}, capture_output=True, text=True, timeout=60)
            assert negative.returncode == 1 and "readback: FAIL" in negative.stdout, "GPU sentinel failed to detect a missing final draw"
            print(f"negative control intensity={intensity} reset={reset}: missing final draw correctly rejected")


if __name__ == "__main__":
    if len(sys.argv) != 3: raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
