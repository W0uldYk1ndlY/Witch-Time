#!/usr/bin/env python3
"""Verify ordered memory helpers code generation and disassembly.
Checks:
- Cross-compiled AArch64 binary contains required ordered accesses (LDAR, STLR) and barriers (DMB ISH).
- Host x86-64 binary introduces no new hardware fences (no mfence, sfence, lfence, lock).
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "tests/ordered_helpers_test.c"

def find_tool(name):
    env_tool = os.environ.get(name.upper())
    if env_tool and Path(env_tool).exists():
        return env_tool
    ndk_bin = Path(r"D:\android-sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin")
    if (ndk_bin / f"{name}.exe").exists():
        return str(ndk_bin / f"{name}.exe")
    if (ndk_bin / name).exists():
        return str(ndk_bin / name)
    found = shutil.which(name)
    if found:
        return found
    return name

def main():
    clang = find_tool("clang")
    objdump = find_tool("llvm-objdump")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)

        # 1. AArch64 cross-compilation check (NOT executed on host)
        arm_obj = tmp / "test_arm64.o"
        cmd_arm = [clang, "-target", "aarch64-linux-android", "-O2", "-I", str(ROOT / "runtime"), "-c", str(SRC), "-o", str(arm_obj)]
        res = subprocess.run(cmd_arm, capture_output=True, text=True)
        if res.returncode != 0:
            print("[CROSS-COMPILE: AArch64] FAIL: compilation error:\n" + res.stderr, file=sys.stderr)
            sys.exit(1)

        dis_arm = subprocess.run([objdump, "-d", str(arm_obj)], capture_output=True, text=True, check=True).stdout

        checks_arm64 = [
            ("harness_load8", r"ldarb\s+w0, \[x0\]"),
            ("harness_load16", r"ldarh\s+w0, \[x0\]"),
            ("harness_load32", r"ldar\s+w0, \[x0\]"),
            ("harness_load64", r"ldar\s+x0, \[x0\]"),
            ("harness_store8", r"stlrb\s+w1, \[x0\]"),
            ("harness_store16", r"stlrh\s+w1, \[x0\]"),
            ("harness_store32", r"stlr\s+w1, \[x0\]"),
            ("harness_store64", r"stlr\s+x1, \[x0\]"),
            ("harness_barrier", r"dmb\s+ish"),
        ]

        for name, pat in checks_arm64:
            m = re.search(r"<" + name + r">:.*?(?=<[a-zA-Z0-9_]+>:|\Z)", dis_arm, re.DOTALL)
            assert m, f"Missing function {name} in aarch64 disassembly"
            assert re.search(pat, m.group(0)), f"Pattern {pat} not found in {name}:\n{m.group(0)}"

        print("[CROSS-COMPILE: AArch64] PASS: Verified required ordered accesses and barriers (LDAR/STLR/DMB) are emitted.")

        # 2. x86-64 host checks
        x86_obj = tmp / "test_x86.o"
        cmd_x86 = [clang, "-O2", "-I", str(ROOT / "runtime"), "-c", str(SRC), "-o", str(x86_obj)]
        res = subprocess.run(cmd_x86, capture_output=True, text=True)
        if res.returncode != 0:
            print("[HOST: x86-64] FAIL: compilation error:\n" + res.stderr, file=sys.stderr)
            sys.exit(1)

        dis_x86 = subprocess.run([objdump, "-d", str(x86_obj)], capture_output=True, text=True, check=True).stdout

        fnames = [
            "harness_load8", "harness_load16", "harness_load32", "harness_load64",
            "harness_store8", "harness_store16", "harness_store32", "harness_store64",
            "harness_barrier"
        ]
        for name in fnames:
            m = re.search(r"<" + name + r">:.*?(?=<[a-zA-Z0-9_]+>:|\Z)", dis_x86, re.DOTALL)
            assert m, f"Missing function {name} in x86 disassembly"
            fn_body = m.group(0)
            for fence in ["mfence", "sfence", "lfence", "lock"]:
                assert fence not in fn_body, f"Unexpected hardware fence {fence} in {name}:\n{fn_body}"

        print("[HOST: x86-64] PASS: Verified helpers introduce no new hardware fences.")

if __name__ == "__main__":
    main()
