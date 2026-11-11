"""Check pinned SGSR fragment sources and reflect shaderc SPIR-V (no GPU required)."""
import ast
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from fsr_shader_test import descriptor_types

root = Path(__file__).resolve().parents[1] / "runtime" / "third_party" / "sgsr2"
blobs = {
    "LICENSE": "d9d513c75858739e0446ded1e48ff6c4d932518f",
    "glsl_2_pass_fs/sgsr2_vertex.vs": "93382cf655e4a7a0f9a574ac988e0540195a1bf1",
    "glsl_2_pass_fs/sgsr2_convert.fs": "d104c97abb2fc69bf962e384c3910996800dcdad",
    "glsl_2_pass_fs/sgsr2_upscale.fs": "e0b492a955d6dba9f26f59505918cb0e1bdc75c7",
    "glsl_2_pass_cs/sgsr2_convert.comp": "38ba12370273c7fdc1a56bedc0e14f1ebe1e6d76",
    "glsl_2_pass_cs/sgsr2_upscale.comp": "3994222c97a47bb3689938ff91f5ee5b5b8af539",
    "glsl_3_pass_cs/sgsr2_convert.comp": "0e48f203d91218c7b1e2726113acebde21235757",
    "glsl_3_pass_cs/sgsr2_activate.comp": "8c33d69e7a5a14d64c1ba502ea05319b2b470255",
    "glsl_3_pass_cs/sgsr2_upscale.comp": "6a4b0c565815ac110b60ce03ef080379332f6baf",
}


def main(glslc, dis):
    for name, expected in blobs.items():
        data = (root / name).read_bytes()
        assert data.endswith(b"\n"), f"{name}: expected documented final newline"
        original = data[:-1]
        digest = hashlib.sha1(f"blob {len(original)}\0".encode() + original).hexdigest()
        assert digest == expected, f"{name}: differs from pinned upstream beyond the final newline"
    with tempfile.TemporaryDirectory(prefix="sgsr-reflect-") as temp:
        temp = Path(temp)
        for name, expected in [("sgsr2_convert.fs", {0: "C", 1: "combined", 2: "combined"}),
                               ("sgsr2_upscale.fs", {0: "C", 1: "combined", 2: "combined", 3: "combined"})]:
            src, spv = root / "glsl_2_pass_fs" / name, temp / "stage.spv"
            subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=fragment", "-DREQUEST_NDC_Y_UP=1", str(src), "-o", str(spv)], check=True)
            assembly = subprocess.run([dis, str(spv)], check=True, capture_output=True, text=True).stdout
            assert descriptor_types(assembly) == expected, f"{name}: descriptor layout mismatch"
            offsets = sorted((int(i), int(n)) for i, n in re.findall(r'OpMemberDecorate\s+%Params\s+(\d+)\s+Offset\s+(\d+)', assembly))
            assert offsets == list(enumerate([0, 64, 72, 80, 88, 96, 104, 112, 116, 120, 124])), f"{name}: UBO offsets differ"
            assert re.search(r'OpDecorate\s+%texCoord\s+Location\s+0\b', assembly), f"{name}: missing fragment varying"
            # Compile a bounded-fetch experiment without changing pinned upstream files.
            bounded = src.read_text(encoding="utf-8").replace(
                "#version 320 es\n", "#version 320 es\n#define texelFetch(s,p,l) texelFetch(s,clamp(p,ivec2(0),textureSize(s,l)-ivec2(1)),l)\n", 1)
            adapted_src = temp / "bounded.frag"
            adapted_src.write_text(bounded, encoding="utf-8")
            subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=fragment", "-DREQUEST_NDC_Y_UP=1", str(adapted_src), "-o", str(spv)], check=True)
            bounded_assembly = subprocess.run([dis, str(spv)], check=True, capture_output=True, text=True).stdout
            assert descriptor_types(bounded_assembly) == expected, f"{name}: bounded fetch changed descriptors"
            assert "OpImageQuerySizeLod" in bounded_assembly and "SClamp" in bounded_assembly, f"{name}: bounded fetch was not emitted"
            print(f"{name}: descriptors, UBO and varying verified")
        # The upstream mesh vertex shader lacks a Vulkan location; the port uses a fullscreen triangle.
        vertex_header = (root.parents[1] / "sgsr2_vertex.h").read_text(encoding="utf-8")
        vertex_block = vertex_header.split("static const char k_sgsr2_vs[] =", 1)[1].split(";\n", 1)[0]
        vertex = "".join(ast.literal_eval(part) for part in re.findall(r'"(?:[^"\\]|\\.)*"', vertex_block))
        src, spv = temp / "vertex.glsl", temp / "vertex.spv"
        src.write_text(vertex, encoding="utf-8")
        subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=vertex", str(src), "-o", str(spv)], check=True)
        assembly = subprocess.run([dis, str(spv)], check=True, capture_output=True, text=True).stdout
        assert re.search(r'OpDecorate\s+%texCoord\s+Location\s+0\b', assembly)
        for name in sorted(root.glob("glsl_*_pass_cs/*.comp")):
            spv = temp / "compute.spv"
            subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=compute", str(name), "-o", str(spv)], check=True)
            assembly = subprocess.run([dis, str(spv)], check=True, capture_output=True, text=True).stdout
            size = re.search(r'OpExecutionModeId\s+%\S+\s+LocalSizeId\s+(%\S+)\s+(%\S+)\s+(%\S+)', assembly)
            if size:
                constants = {key: int(value) for key, value in re.findall(r'(%\S+)\s*=\s*OpConstant\s+%\S+\s+(\d+)', assembly)}
                assert [constants.get(key) for key in size.groups()] == [8, 8, 1], f"{name}: unexpected workgroup size"
            else:
                assert re.search(r'OpExecutionMode\s+%\S+\s+LocalSize\s+8\s+8\s+1\b', assembly), f"{name}: unexpected workgroup size"
            bindings = [int(n) for n in re.findall(r'OpDecorate\s+%\S+\s+Binding\s+(\d+)', assembly)]
            offsets = sorted((int(i), int(n)) for i, n in re.findall(r'OpMemberDecorate\s+%Params\s+(\d+)\s+Offset\s+(\d+)', assembly))
            expected_offsets = [0, 8, 16, 24, 32] + ([40] if name.parent.name == "glsl_3_pass_cs" else []) + [48, 112, 116, 120, 124, 128, 132]
            assert offsets == list(enumerate(expected_offsets)), f"{name}: compute UBO offsets changed"
            if name.parent.name == "glsl_2_pass_cs":
                assert len(bindings) != len(set(bindings)), f"{name}: upstream binding collision changed; review remapping"
                try:
                    descriptor_types(assembly)
                except AssertionError as error:
                    assert "duplicate descriptor binding" in str(error)
                else:
                    raise AssertionError(f"{name}: descriptor reflection accepted duplicate bindings")
                adapted = name.read_text(encoding="utf-8")
                for old, new in (("layout(binding = 0, rgba16f)", "layout(binding = 10, rgba16f)"),
                                 ("layout(binding = 1, r32ui)", "layout(binding = 11, r32ui)"),
                                 ("layout(binding = 1, rgba16f)", "layout(binding = 11, rgba16f)")):
                    adapted = adapted.replace(old, new)
                adapted_src = temp / "adapted.comp"
                adapted_src.write_text(adapted, encoding="utf-8")
                subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=compute", str(adapted_src), "-o", str(spv)], check=True)
                adapted_assembly = subprocess.run([dis, str(spv)], check=True, capture_output=True, text=True).stdout
                expected = ({0: "C", 1: "combined", 2: "combined", 3: "combined", 10: "U", 11: "U"}
                            if name.name == "sgsr2_convert.comp" else
                            {0: "C", 7: "combined", 8: "combined", 9: "combined", 10: "U", 11: "U"})
                assert descriptor_types(adapted_assembly) == expected, f"{name}: experimental remap failed"
            else:
                assert len(bindings) == len(set(bindings)), f"{name}: duplicate 3-pass bindings"
                expected = {
                    "sgsr2_convert.comp": {0: "C", 1: "combined", 2: "combined", 3: "combined", 4: "combined", 5: "U", 6: "U"},
                    "sgsr2_activate.comp": {0: "C", 1: "combined", 2: "combined", 3: "combined", 4: "U", 5: "U"},
                    "sgsr2_upscale.comp": {0: "C", 1: "combined", 2: "combined", 3: "combined", 4: "U", 5: "U"},
                }
                assert descriptor_types(assembly) == expected[name.name], f"{name}: descriptor types changed"
            print(f"{name.parent.name}/{name.name}: compiled; {'duplicate bindings require remapping' if len(bindings) != len(set(bindings)) else 'bindings unique'}")
    print("SGSR pinned fragment sources reflected (shaderc only; no runtime or GPU qualification)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
