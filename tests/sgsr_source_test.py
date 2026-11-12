"""Compile shaders emitted by the production SGSR source adapter; no GPU is used."""
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
    host = (ROOT / "runtime/nvn_vk_sgsr.c").read_text(encoding="utf-8")
    fsr = (ROOT / "runtime/nvn_vk_fsr.c").read_text(encoding="utf-8")
    adapter = re.search(r'static char \*sgsr_fragment_source\(.*?\n\}', host, re.S).group(0)
    append = re.search(r'static void fsr_put\(.*?\n\}', fsr, re.S).group(0)
    harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "upscale_motion.h"
typedef struct { char *p; size_t n, cap; } FsrStr;
''' + append + '\n' + adapter + r'''
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    if (fseek(f, 0, SEEK_END)) return 3;
    long n = ftell(f); if (n < 0) return 4;
    rewind(f); char *data = calloc((size_t)n + 1, 1); if (!data) return 5;
    if (fread(data, 1, (size_t)n, f) != (size_t)n) return 6;
    fclose(f); char *source = sgsr_fragment_source(data); free(data);
    if (!source) return 7;
    fputs(source, stdout); free(source); return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="sgsr-source-") as directory:
        directory = Path(directory)
        c = directory / "emit.c"
        exe = directory / ("emit.exe" if os.name == "nt" else "emit")
        c.write_text(harness, encoding="utf-8")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "runtime"), str(c), "-o", str(exe)], check=True)
        for name, count in [("sgsr2_convert.fs", 3), ("sgsr2_upscale.fs", 4)]:
            upstream = ROOT / "runtime/third_party/sgsr2/glsl_2_pass_fs" / name
            result = subprocess.run([str(exe), str(upstream)], check=True, capture_output=True, text=True)
            source = result.stdout
            assert source.startswith("#version 450\n") and "mediump" not in source
            if name == "sgsr2_convert.fs":
                correction = "ScreenPos = wtMotionUnjitterNdc(ScreenPos, params.jitterOffset, params.renderSizeRcp);"
                assert correction in source and source.index(correction) < source.index("vec3 Position = vec3(ScreenPos,")
                # Nonidentity camera reprojection: removing raster jitter must preserve both current and previous positions.
                x, y, z = 0.2, -0.3, 0.7
                previous = (1.1 * x + 0.2 * z, 0.9 * y - 0.1 * z)
                for jx, jy in [(0.0, 0.0), (0.375, -0.25), (-0.125, 0.375)]:
                    current = (x + 2 * jx / 17, y - 2 * jy / 13)
                    unjittered = (current[0] - 2 * jx / 17, current[1] + 2 * jy / 13)
                    projected = (1.1 * unjittered[0] + 0.2 * z, 0.9 * unjittered[1] - 0.1 * z)
                    assert max(abs(a - b) for a, b in zip(projected, previous)) < 1e-12
            src, spv = directory / "shader.frag", directory / "shader.spv"
            src.write_text(source, encoding="utf-8")
            subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=fragment", str(src), "-o", str(spv)], check=True)
            assembly = subprocess.run([dis, str(spv)], check=True, capture_output=True, text=True).stdout
            assert descriptor_types(assembly) == {0: "C", **{i: "combined" for i in range(1, count)}}
            assert "SClamp" in assembly and "OpImageQuerySizeLod" in assembly
            assert "RelaxedPrecision" not in assembly, "HDR source still permits relaxed precision"
            compiled[name] = spv.read_bytes()
            print(f"{name}: production adapter compiled, descriptors/bounds/highp verified")
    return compiled


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("Usage: python tests/sgsr_source_test.py <glslc> <spirv-dis>")
    main(sys.argv[1], sys.argv[2])
