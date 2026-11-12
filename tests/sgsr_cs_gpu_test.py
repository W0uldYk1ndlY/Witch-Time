"""Check constant-input dispatch/coverage fidelity or HDR quality, not history/reset correctness."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from sgsr_cs_source_test import main as compile_sources, ROOT


def gpu_environment(quality):
    env = dict(os.environ)
    env.pop("WT_SGSR_TEST_OFFICIAL_REFERENCE", None)
    env.pop("WT_SGSR_TEST_SKIP_FINAL", None)
    if not quality:
        env["WT_SGSR_TEST_OFFICIAL_REFERENCE"] = "1"
    return env


def main(glslc, dis, quality=False):
    stages = compile_sources(glslc, dis)
    with tempfile.TemporaryDirectory(prefix="sgsr-cs-gpu-") as directory:
        directory = Path(directory)
        paths = []
        for name in ["convert", "upscale"]:
            path = directory / (name + ".spv"); path.write_bytes(stages[name]); paths.append(str(path))
        exe = directory / ("gpu.exe" if os.name == "nt" else "gpu")
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
                   "-I", str(ROOT / "runtime"), "-I", str(ROOT / "deps/android-arm64/include"),
                   str(ROOT / "tests/sgsr_cs_gpu_test.c"), "-o", str(exe)]
        if os.name != "nt": command += ["-ldl", "-lm"]
        subprocess.run(command, check=True)
        failed = []
        env = gpu_environment(quality)
        for case in [(1, 1, 1, 1, 4), (17, 13, 17, 13, 4), (17, 13, 25, 19, 4),
                     (17, 13, 25, 19, 0), (17, 13, 25, 19, 1000), (17, 13, 25, 19, 60000)]:
            result = subprocess.run([str(exe), *paths, *map(str, case)], env=env, timeout=60)
            if result.returncode: failed.append(case)
        control = subprocess.run([str(exe), *paths, "17", "13", "25", "19", "4"],
                                 env={**env, "WT_SGSR_TEST_SKIP_FINAL": "1"}, capture_output=True, text=True, timeout=60)
        assert control.returncode == 1 and "readback: FAIL" in control.stdout, "missing-dispatch sentinel did not fail"
        print("negative control: skipped Upscale rejected")
        if failed: raise SystemExit(f"{'Unqualified HDR quality' if quality else 'Constant-input dispatch/coverage fidelity failed'} CS cases: {failed}")
        if not quality:
            print("Constant-input dispatch/coverage fidelity passed; history/reset and nonzero reprojection are NOT validated")
            print("HDR quality is separate and remains UNQUALIFIED (run with --quality)")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4) or (len(sys.argv) == 4 and sys.argv[3] != "--quality"): raise SystemExit(__doc__)
    main(*sys.argv[1:3], quality=len(sys.argv) == 4)
