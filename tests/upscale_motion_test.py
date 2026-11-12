"""Analytic + synthetic GPU motion contracts, using the unchanged SGSR GPU harness.

Usage: python tests/upscale_motion_test.py <glslc> <spirv-dis> [--compile-only]
No game/device inputs. The test fragment is NOT a new upscaling algorithm.
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


def main(glslc, dis, compile_only=False):
    fragments = compile_fragments(glslc, dis)
    with tempfile.TemporaryDirectory(prefix="motion-gpu-") as temporary:
        directory = Path(temporary)
        emit = directory / "emit.c"
        emit.write_text('#include <stdio.h>\n#include "upscale_motion.h"\n#include "sgsr2_params.h"\n'
                        'int main(void) { puts(WT_UPSCALE_MOTION_GLSL); return 0; }\n', encoding="utf-8")
        emitter = directory / ("emit.exe" if os.name == "nt" else "emit")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "runtime"), str(emit), "-o", str(emitter)], check=True)
        helpers = subprocess.check_output([str(emitter)], text=True)
        # This block matches the production FS layout; C offset/size asserts above
        # and actual UBO reads below cover CPU packing and GLSL matrix column order.
        upstream = (ROOT / "runtime/third_party/sgsr2/glsl_2_pass_fs/sgsr2_convert.fs").read_text()
        block = re.search(r'layout\(std140.*?\} params;', upstream, re.S).group(0)
        source = "#version 450\n" + block + "\n" + helpers + r'''
layout(location=0) out vec4 color;
layout(location=1) out vec4 history;
bool close2(vec2 a, vec2 b) { return all(lessThan(abs(a-b), vec2(0.000004))); }
void main() {
    vec2 rcp = params.renderSizeRcp;
    vec2 jitter = vec2(0.375, -0.25);
    vec2 current = vec2(0.2, -0.3);
    vec2 uv = (current + vec2(1.0, -1.0)) * vec2(0.5, -0.5) + jitter * rcp;
    // Nonidentity, depth-dependent, projective camera: explicit columns.
    mat4 m = mat4(vec4(1.1,0,0,0.15), vec4(0,0.9,0,-0.08),
                  vec4(0.2,-0.1,1,0), vec4(0.05,-0.03,0,1.2));
    float z = float(int(gl_FragCoord.x) % 3) * 0.5; // near, middle, far
    float pw = 0.15*0.2 - 0.08*(-0.3) + 1.2;
    vec2 previous = vec2((1.1*0.2+0.2*z+0.05)/pw,
                         (0.9*(-0.3)-0.1*z-0.03)/pw);
    vec2 expectedCamera = (previous-current)*vec2(0.5,-0.5);
    vec2 camera = wtMotionCamera(uv,z,m,jitter,rcp);
    bool ok = close2(camera,expectedCamera);
    ok = ok && close2(wtMotionCamera(uv-jitter*rcp,z,m,vec2(0),rcp),camera);
    // Synthetic projected object-only and camera+object displacement.
    vec2 objectPrevious = previous + vec2(0.125,-0.0625);
    vec2 total = (objectPrevious-current)*vec2(0.5,-0.5);
    vec2 objectOnly = vec2(0.0625,0.03125);
    ok = ok && close2(wtMotionSelect(objectOnly,true,vec2(0)),objectOnly);
    ok = ok && close2(wtMotionSelect(total,true,camera),total);
    // Valid zero must REPLACE camera; invalid nonzero must IGNORE raw contents.
    ok = ok && close2(wtMotionSelect(vec2(0),true,camera),vec2(0));
    ok = ok && close2(wtMotionSelect(vec2(99),false,camera),expectedCamera);
    int coverage = (int(gl_FragCoord.x)+int(gl_FragCoord.y)) % 3;
    vec2 raw = coverage == 0 ? vec2(0) : total;
    vec2 selected = wtMotionSelect(raw,coverage != 2,camera);
    vec2 expected = coverage == 0 ? vec2(0) : coverage == 1 ? total : expectedCamera;
    vec2 fsr = wtMotionFsr(selected), sgsr = wtMotionSgsr(selected);
    ok = ok && close2(fsr,expected) && close2(sgsr,expected*vec2(-2,2));
    vec2 currentUV = current*vec2(0.5,-0.5)+0.5;
    vec2 previousFromSgsr = (current-sgsr)*vec2(0.5,-0.5)+0.5;
    ok = ok && close2(currentUV+fsr,previousFromSgsr);
    // Uploaded FS UBO uses the same nonsymmetric projective transform in case 5.
    mat4 uploaded = mat4(params.clipToPrevClip[0],params.clipToPrevClip[1],
                         params.clipToPrevClip[2],params.clipToPrevClip[3]);
    ok = ok && close2(wtMotionCamera(uv,z,uploaded,jitter,rcp),expectedCamera);
    color = history = ok ? vec4(4,2,1,1) : vec4(0,0,0,1);
}
'''
        shader = directory / "motion.frag"
        shader.write_text(source, encoding="utf-8")
        test_spv = directory / "motion.spv"
        subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=fragment", str(shader), "-o", str(test_spv)], check=True)
        header = (ROOT / "runtime/sgsr2_vertex.h").read_text()
        body = header.split("static const char k_sgsr2_vs[] =", 1)[1].split(";\n", 1)[0]
        vertex = "".join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', body))
        vert = directory / "vertex.vert"
        vert.write_text(vertex, encoding="utf-8")
        vertex_spv = directory / "vertex.spv"
        subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=vertex", str(vert), "-o", str(vertex_spv)], check=True)
        if compile_only:
            print("Motion contract shaders compiled (GPU numerical checks and negative controls not run)")
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
        environment = {**os.environ, "WT_SGSR_TEST_CAMERA": "5"}
        environment.pop("WT_SGSR_TEST_SKIP_FINAL", None)
        args = [str(exe), str(vertex_spv), str(convert), str(test_spv), "17", "13", "25", "19", "4", "0"]
        subprocess.run(args, env=environment, check=True, timeout=60)
        # Mutation control: an omitted final pass must fail the readback, not pass silently.
        negative = subprocess.run(args, env={**environment, "WT_SGSR_TEST_SKIP_FINAL": "1"},
                                  capture_output=True, text=True, timeout=60)
        assert negative.returncode == 1 and "readback: FAIL" in negative.stdout
        for name, wrong in [
            ("wrong-SGSR-X-sign", source.replace("vec2(-2.0, 2.0) * canonical", "vec2(2.0, 2.0) * canonical")),
            ("zero-sentinel", source.replace("valid ? rawTotal : camera", "(valid && any(notEqual(rawTotal,vec2(0)))) ? rawTotal : camera")),
        ]:
            assert wrong != source
            shader.write_text(wrong, encoding="utf-8")
            subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=fragment", str(shader), "-o", str(test_spv)], check=True)
            rejected = subprocess.run(args, env=environment, capture_output=True, text=True, timeout=60)
            assert rejected.returncode == 1 and "readback: FAIL" in rejected.stdout, name
            print(f"Motion GPU negative control: {name} rejected")
        print("Motion GPU: camera/object/both, explicit valid-zero/invalid coverage, backend equivalence, jitter/depth/layout PASS")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4) or (len(sys.argv) == 4 and sys.argv[3] != "--compile-only"):
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2], len(sys.argv) == 4)
