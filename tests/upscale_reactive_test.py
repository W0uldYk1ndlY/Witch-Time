"""Synthetic GLSL contract/readback only; no scene boundary or backend qualification.

Usage: python tests/upscale_reactive_test.py <glslc> <spirv-dis> [--compile-only]
"""
import ast
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from sgsr_source_test import main as compile_fragments, ROOT


def main(glslc, dis, compile_only=False):
    fragments = compile_fragments(glslc, dis)
    with tempfile.TemporaryDirectory(prefix="reactive-gpu-") as temporary:
        directory = Path(temporary)
        emit = directory / "emit.c"
        emit.write_text('#include <stdio.h>\n#include "upscale_reactive.h"\n'
                        'int main(void) { puts(WT_UPSCALE_REACTIVE_GLSL); return 0; }\n')
        emitter = directory / ("emit.exe" if os.name == "nt" else "emit")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "runtime"), str(emit), "-o", str(emitter)], check=True)
        helper = subprocess.check_output([str(emitter)], text=True)
        source = "#version 450\n" + helper + r'''
layout(location=0) out vec4 color;
layout(location=1) out vec4 history;
bool near2(vec2 a, vec2 b) { return all(lessThan(abs(a-b), vec2(0.00001))); }
void main() {
    bool ok = true;
    // Bright additive particles; no previous-frame color participates.
    vec2 bright = wtReactiveMasks(vec3(1),vec3(101,1,1),0,0,true,false,true);
    ok = ok && near2(bright,vec2(100.0/101.0));
    ok = ok && near2(wtReactiveMasks(vec3(0),vec3(10),0,0,true,false,true),vec2(1));
    ok = ok && near2(wtReactiveMasks(vec3(4),vec3(4),0,0,true,false,true),vec2(0));
    // Unchanged hair needs explicit classification; RGB alone cannot identify it.
    ok = ok && near2(wtReactiveMasks(vec3(4),vec3(4),0.75,0,true,false,true),vec2(0.75));
    ok = ok && near2(wtReactiveMasks(vec3(4),vec3(4),0,1,true,false,true),vec2(1,0));
    ok = ok && near2(wtReactiveMasks(vec3(4),vec3(4),0,0,false,false,true),vec2(1));
    ok = ok && near2(wtReactiveMasks(vec3(4),vec3(4),0,0,false,false,false),vec2(1));
    ok = ok && near2(wtReactiveMasks(vec3(4),vec3(4),0,0,true,true,false),vec2(1));
    ok = ok && near2(wtReactiveMasks(vec3(0),vec3(10),1,1,true,false,false),vec2(0));
    // Synthetic history mix: mask-on removes a stale bright trail, mask-off retains it.
    vec2 full = wtReactiveMasks(vec3(1),vec3(1),0,1,true,false,true);
    float onWeight = wtReactiveHistoryWeight(0.9,full);
    float offWeight = wtReactiveHistoryWeight(0.9,vec2(0));
    ok = ok && abs(mix(1.0,10.0,onWeight)-1.0)<0.00001;
    ok = ok && mix(1.0,10.0,offWeight)>9.0;
    // Opaque alternating fine detail retains the exact original history weight.
    float detail = (int(gl_FragCoord.x)+int(gl_FragCoord.y))%2 == 0 ? 0.25 : 4.0;
    vec2 unchanged = wtReactiveMasks(vec3(detail),vec3(detail),0,0,true,false,true);
    ok = ok && abs(wtReactiveHistoryWeight(0.9,unchanged)-0.9)<0.00001;
    // Stateless reset/pair rejection must not poison the next valid unchanged pair.
    ok = ok && near2(wtReactiveMasks(vec3(0),vec3(0),0,0,true,false,true),vec2(0));
    vec2 bounded = wtReactiveMasks(vec3(0),vec3(1),2,-1,true,false,true);
    ok = ok && near2(bounded,vec2(1));
    float nanValue = uintBitsToFloat(0x7fc00000u);
    ok = ok && near2(wtReactiveMasks(vec3(nanValue),vec3(1),0,0,true,false,true),vec2(1));
    ok = ok && near2(wtReactiveMasks(vec3(nanValue),vec3(1),0,0,true,false,false),vec2(1));
    ok = ok && near2(wtReactiveMasks(vec3(1),vec3(1),nanValue,0,true,false,true),vec2(1));
    ok = ok && near2(wtReactiveMasks(vec3(1),vec3(1),nanValue,0,true,false,false),vec2(1));
    ok = ok && near2(wtReactiveMasks(vec3(1),vec3(1),0,uintBitsToFloat(0x7f800000u),true,false,true),vec2(1));
    color = history = ok ? vec4(4,2,1,1) : vec4(0,0,0,1);
}
'''
        shader, test_spv = directory / "reactive.frag", directory / "reactive.spv"
        def compile_shader(text):
            shader.write_text(text)
            subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=fragment",
                            str(shader), "-o", str(test_spv)], check=True)
        compile_shader(source)
        header = (ROOT / "runtime/sgsr2_vertex.h").read_text()
        body = header.split("static const char k_sgsr2_vs[] =", 1)[1].split(";\n", 1)[0]
        vertex = directory / "vertex.vert"
        vertex.write_text("".join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', body)))
        vertex_spv = directory / "vertex.spv"
        subprocess.run([glslc, "--target-env=vulkan1.3", str(vertex), "-o", str(vertex_spv)], check=True)
        if compile_only:
            print("Reactive contract compiled; numerical GPU tests NOT run")
            return
        convert = directory / "convert.spv"
        convert.write_bytes(fragments["sgsr2_convert.fs"])
        exe = directory / ("gpu.exe" if os.name == "nt" else "gpu")
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
                   "-I", str(ROOT / "runtime"), "-I", str(ROOT / "deps/android-arm64/include"),
                   str(ROOT / "tests/sgsr_gpu_test.c"), "-o", str(exe)]
        if os.name != "nt":
            command += ["-ldl", "-lm"]
        subprocess.run(command, check=True)
        env = dict(os.environ)
        env.pop("WT_SGSR_TEST_SKIP_FINAL", None)
        args = [str(exe), str(vertex_spv), str(convert), str(test_spv), "17", "13", "25", "19", "4", "0"]
        subprocess.run(args, env=env, check=True, timeout=60)
        for name, mutant in [
            ("missing-mask", source.replace("return vec2(max(composition, clamp(invalidMotion, 0.0, 1.0)), composition);", "return vec2(0.0);")),
            ("missing-history-rejection", source.replace("(1.0 - clamp(max(masks.x, masks.y), 0.0, 1.0))", "1.0")),
        ]:
            assert mutant != source
            compile_shader(mutant)
            result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode == 1 and "readback: FAIL" in result.stdout, name
            print(f"Reactive negative control: {name} rejected")
        print("Reactive GLSL synthetic bright/additive/opaque/hair/coverage/reset/trail/detail PASS")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4) or (len(sys.argv) == 4 and sys.argv[3] != "--compile-only"):
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2], len(sys.argv) == 4)
