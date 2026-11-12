"""Dump-free desktop Vulkan replay model; not production draw-orchestration qualification.

Usage: python tests/rs_store_gpu_test.py <glslc> [--compile-only]
No renderer, pinned shader, Android device or dependencies are changed.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main(glslc, compile_only=False):
    source = (ROOT / "runtime/nvn_vk_draw.c").read_text(encoding="utf-8")
    helpers = []
    for signature in ("static int rs_store_low(void)", "static void rs_store_done(int drawn)"):
        match = re.search(re.escape(signature) + r" \{.*?\n\}", source, re.S)
        assert match, signature
        helpers.append(match.group())
    eligibility = re.search(r"g_dp_fs_stores = fs && fs->images && !buffer_writes;", source)
    assert eligibility, "production eligibility changed; update harness deliberately"
    image_loop = source.split("    int stores = 0, buffer_writes = 0;", 1)[1].split("    PT(1);", 1)[0]
    image_loop = "int stores = 0, buffer_writes = 0;" + image_loop
    temp_root = Path(os.environ["LOCALAPPDATA"]) / "Temp/opencode" if os.name == "nt" else None
    with tempfile.TemporaryDirectory(prefix="rs-store-gpu-", dir=temp_root) as temp:
        temp = Path(temp)
        (temp / "rs_helpers.h").write_text("\n".join(helpers), encoding="utf-8")
        (temp / "rs_eligibility.h").write_text(image_loop + eligibility.group() + "\n(void)stores;", encoding="utf-8")
        shaders = {
            "vert": """#version 450
void main() { vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); gl_Position=vec4(p*2-1,0,1); }
""",
            "frag": """#version 450
layout(set=0,binding=0,r32ui) uniform uimage2D target;
layout(set=0,binding=1,std430) buffer Counts { uint counts[]; };
layout(push_constant) uniform Params { uint width; uint buffers; } p;
void main() { ivec2 xy=ivec2(gl_FragCoord.xy); imageStore(target,xy,uvec4(123));
if(p.buffers!=0) atomicAdd(counts[xy.y*int(p.width)+xy.x],1); }
""",
        }
        paths = []
        for stage, text in shaders.items():
            src, spv = temp / ("test." + stage), temp / (stage + ".spv")
            src.write_text(text, encoding="utf-8")
            subprocess.run([glslc, "--target-env=vulkan1.3", str(src), "-o", str(spv)], check=True)
            paths.append(str(spv))
        exe = temp / ("test.exe" if os.name == "nt" else "test")
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra",
                   "-Wno-missing-field-initializers", "-I", str(temp), "-I", str(ROOT / "runtime"),
                   "-I", str(ROOT / "deps/android-arm64/include"), str(ROOT / "tests/rs_store_gpu_test.c"), "-o", str(exe)]
        if os.name != "nt":
            command += ["-ldl", "-lm"]
        subprocess.run(command, check=True)
        if compile_only:
            print("Full-grid store shaders and production-helper harness compiled (GPU coverage not run)")
            return
        for w, h in ((17, 13), (31, 9), (3, 5)):
            for indexed in (0, 1):
                for buffers in (0, 1):
                    subprocess.run([str(exe), *paths, str(w), str(h), str(indexed), str(buffers), "0"], check=True, timeout=60)
                negative = subprocess.run([str(exe), *paths, str(w), str(h), str(indexed), "0", "1"], capture_output=True, text=True, timeout=60)
                assert negative.returncode == 1 and "untouched rows=" in negative.stdout and "FAIL" in negative.stdout, negative.stdout + negative.stderr
                missing = re.search(r"untouched rows=(\d+) cols=(\d+)", negative.stdout)
                assert missing and all(int(n) > 0 for n in missing.groups()), negative.stdout
                print("skipped-replay negative control rejected:", negative.stdout.strip())


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != "--compile-only"):
        raise SystemExit(__doc__)
    main(sys.argv[1], len(sys.argv) == 3)
