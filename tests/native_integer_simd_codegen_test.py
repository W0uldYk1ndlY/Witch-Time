#!/usr/bin/env python3
"""Compile actual constant-form helper calls with the NDK and check vector lowering.

Requires ANDROID_NDK_HOME (or --cc pointing at NDK clang). Uses the production
lifted-code flags and freestanding headers; no device or proprietary dump needed.
The backend-off build is retained as the scalar reference for size comparisons.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def instructions(assembly):
    result = {}
    for name, body in re.findall(r"^(i3_\w+):[^\n]*\n(.*?)^\.Lfunc_end", assembly, re.M | re.S):
        result[name] = [line.strip() for line in body.splitlines()
                        if re.match(r"\s+[a-z][a-z0-9]+(?:\s|$)", line)]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    ndk = os.environ.get("ANDROID_NDK_HOME", os.environ.get("ANDROID_NDK_ROOT", ""))
    parser.add_argument("--cc", default=str(Path(ndk) / "toolchains/llvm/prebuilt/linux-x86_64/bin/clang") if ndk else None)
    args = parser.parse_args()
    if not args.cc:
        parser.error("set ANDROID_NDK_HOME or pass --cc with NDK clang")
    root = Path(__file__).resolve().parent.parent
    operations = ("add", "sub", "mul", "cmeq", "cmgt", "cmhi", "cmge", "cmhs", "cmtst")
    source = '#include "runtime.h"\n'
    expected = {}
    for size in range(3):
        for width in (64, 128):
            for op in operations:
                name = f"i3_{op}_{size}_{width}"
                expected[name] = op
                source += f"void {name}(V128 *d, const V128 *a, const V128 *b) {{ v_i3v(d, a, b, I_{op.upper()}, {size}, {width // (8 << size)}); }}\n"
    env = os.environ.copy()
    env.pop("CPATH", None)  # Host SDK headers cannot enter an Android freestanding build.
    with tempfile.TemporaryDirectory(prefix="wt-integer-codegen-") as tmp:
        tmp = Path(tmp)
        input_path = tmp / "integer.c"
        input_path.write_text(source)
        output = {}
        for enabled in (0, 1, None):
            assembly_path = tmp / f"integer-{enabled}.s"
            command = [args.cc, "--target=aarch64-linux-android29", "-O2", "-ffp-contract=off",
                       "-fno-strict-aliasing", "-Dinline=inline __attribute__((always_inline))", "-fPIC",
                       "-nostdlibinc", "-isystem", str(root / "runtime/freestanding"),
                       "-I", str(root / "runtime")]
            if enabled is not None:
                command += [f"-DWT_NATIVE_INT_SIMD={enabled}"]
            subprocess.run(command + ["-S", str(input_path), "-o", str(assembly_path)], check=True, env=env)
            output[enabled] = instructions(assembly_path.read_text())
    if any(set(forms) != set(expected) for forms in output.values()):
        raise AssertionError("compiler output did not contain every helper wrapper")
    for name, op in expected.items():
        scalar, native = output[0][name], output[1][name]
        assert any(re.match(rf"{op}\s+v\d+\.", line) for line in native), (name, "missing vector operation", native)
        assert len(native) <= 8, (name, "unexpected scalarization or helper call", native)
        assert len(native) < len(scalar), (name, "backend did not improve instruction count", scalar, native)
        assert output[None][name] == native, (name, "Android default differs from enabled backend")
    print(f"native_integer_simd_codegen_test: {len(expected)} Android NDK helper forms use vector instructions and shrink")
    for name in ("i3_add_0_128", "i3_cmgt_0_128", "i3_mul_1_128", "i3_add_2_128", "i3_cmtst_2_64"):
        print(f"  {name}: {len(output[0][name])} -> {len(output[1][name])} instructions (including ret)")


if __name__ == "__main__":
    main()
