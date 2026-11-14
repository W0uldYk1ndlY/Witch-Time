"""Build the real shader shim and verify stable, compiler-specific cache identity.

Only the exported identity is called, so a Vulkan device and linked glslang are
unneeded. WT_GLSLANG_TEST_INCLUDE selects installed glslang development headers.
"""
import ctypes
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
candidates = [Path(os.environ.get("WT_GLSLANG_TEST_INCLUDE", "/usr/include")), ROOT / "deps/android-arm64/include"]
include = next((p for p in candidates if (p / "glslang/build_info.h").is_file()), None)
if include is None:
    raise SystemExit("Set WT_GLSLANG_TEST_INCLUDE to an installed glslang include directory")

with tempfile.TemporaryDirectory(prefix="shader-identity-") as directory:
    temp = Path(directory)
    build_info = (include / "glslang/build_info.h").read_text(encoding="utf-8")
    identities = []
    for variant in ["current", "upgraded"]:
        override = temp / variant
        (override / "glslang").mkdir(parents=True)
        text = build_info
        if variant == "upgraded":
            text, count = re.subn(r"(#define GLSLANG_VERSION_PATCH\s+)(\d+)", lambda m: m[1] + str(int(m[2]) + 1), text)
            assert count == 1, "glslang build_info must expose the version patch"
        (override / "glslang/build_info.h").write_text(text, encoding="utf-8")
        so = override / "shim.so"
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-shared", "-fPIC", "-pthread", "-D_GNU_SOURCE", "-D__declspec(x)=", "-I", str(override), "-I", str(ROOT / "runtime/posix"), "-I", str(ROOT / "runtime"), "-I", str(include), str(ROOT / "runtime/vk_glslang_shim.c"), "-o", str(so)], check=True)
        lib = ctypes.CDLL(str(so), mode=os.RTLD_LOCAL | os.RTLD_LAZY)
        identity = getattr(lib, "wt_glsl_cache_identity", None)
        assert identity is not None, "Shader shim does not expose a compiler/profile cache identity"
        identity.restype = ctypes.c_uint64
        first = identity()
        assert first != 0 and first == identity(), "Cache identity must be stable and nonzero"
        identities.append(first)
    assert identities[0] != identities[1], "A compiler upgrade must invalidate cached translations"
print("Shader shim identity is stable and changes with compiler version")
