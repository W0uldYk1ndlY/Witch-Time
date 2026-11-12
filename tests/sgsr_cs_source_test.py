"""Compile and reflect the production 2-pass-CS source adapter (no app/device)."""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from fsr_shader_test import descriptor_types

ROOT = Path(__file__).resolve().parents[1]


def main(glslc, dis):
    compiled = {}
    with tempfile.TemporaryDirectory(prefix="sgsr-cs-") as directory:
        directory = Path(directory)
        harness = directory / "emit.c"
        harness.write_text(r'''
#include <stdio.h>
#include "sgsr2_params.h"
#include "sgsr2_cs_source.h"
int main(int argc, char **argv) {
    if (argc != 3) return 1;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n <= 0) return 3;
    char *data = calloc((size_t)n + 1, 1); if (!data) return 4;
    if (fread(data, 1, (size_t)n, f) != (size_t)n) return 5;
    fclose(f); char *source = sgsr_cs_source(data, atoi(argv[2])); free(data);
    if (!source) return 6;
    fputs(source, stdout); free(source); return 0;
}
''', encoding="utf-8")
        exe = directory / ("emit.exe" if os.name == "nt" else "emit")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "runtime"), str(harness), "-o", str(exe)], check=True)
        for p, name in enumerate(["convert", "upscale"]):
            upstream = ROOT / f"runtime/third_party/sgsr2/glsl_2_pass_cs/sgsr2_{name}.comp"
            source = subprocess.run([str(exe), str(upstream), str(p)], check=True, capture_output=True, text=True).stdout
            assert "mediump" not in source
            assert f"params.{'displaySize' if p else 'renderSize'}))) return;" in source
            if not p:
                assert source.index("ScreenPos -=") < source.index("vec3 Position =")
            src, spv = directory / "shader.comp", directory / "shader.spv"
            src.write_text(source, encoding="utf-8")
            subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=compute", str(src), "-o", str(spv)], check=True)
            assembly = subprocess.run([dis, str(spv)], check=True, capture_output=True, text=True).stdout
            expected = {0: "C", **{i: "combined" for i in ([7, 8, 9] if p else [1, 2, 3])}, 10: "U", 11: "U"}
            assert descriptor_types(assembly) == expected
            offsets = sorted((int(i), int(n)) for i, n in re.findall(r'OpMemberDecorate\s+%Params\s+(\d+)\s+Offset\s+(\d+)', assembly))
            assert offsets == list(enumerate([0, 8, 16, 24, 32, 48, 112, 116, 120, 124, 128, 132]))
            assert "OpUGreaterThanEqual" in assembly and "OpReturn" in assembly
            assert "SClamp" in assembly and "OpImageQuerySizeLod" in assembly
            assert "RelaxedPrecision" not in assembly
            assert "StorageImageExtendedFormats" not in assembly
            compiled[name] = spv.read_bytes()
            print(f"2-pass-CS {name}: production adapter compiled; unique bindings, UBO, bounds, highp verified")
    return compiled


if __name__ == "__main__":
    if len(sys.argv) != 3: raise SystemExit(__doc__)
    main(*sys.argv[1:])
