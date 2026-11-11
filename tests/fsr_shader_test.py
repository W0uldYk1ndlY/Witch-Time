"""Compile and inspect the FSR shaders with existing glslc and spirv-dis tools.

Usage: python tests/fsr_shader_test.py <glslc> <spirv-dis>
This checks shaderc output, not the runtime glslang build or a GPU.
"""
import ast
import ctypes
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "runtime" / "third_party" / "fsr3"
HOST = ROOT / "runtime" / "nvn_vk_fsr.c"


def expand(name, embedded):
    if name not in embedded:
        return ""
    result = []
    for line in (SHADERS / name).read_text(encoding="utf-8").splitlines(keepends=True):
        match = re.match(r'\s*#include\s+"([^"]+)"', line)
        if match:
            result.append(expand(match.group(1).rsplit("/", 1)[-1], embedded))
        elif not line.lstrip().startswith("#extension GL_GOOGLE_include_directive"):
            result.append(line)
    return "".join(result)


def descriptor_types(assembly):
    decorations = re.findall(r'OpDecorate\s+(%\S+)\s+Binding\s+(\d+)', assembly)
    sets = {var: int(n) for var, n in re.findall(r'OpDecorate\s+(%\S+)\s+DescriptorSet\s+(\d+)', assembly)}
    variables = dict(re.findall(r'(%\S+)\s*=\s*OpVariable\s+(%\S+)\s+\w+', assembly))
    pointers = {p: (storage, target) for p, storage, target in
                re.findall(r'(%\S+)\s*=\s*OpTypePointer\s+(\w+)\s+(%\S+)', assembly)}
    images = {i: ("S" if sampled == "1" else "U") for i, sampled in
              re.findall(r'(%\S+)\s*=\s*OpTypeImage\s+%\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+([12])\s+\S+', assembly)}
    samplers = set(re.findall(r'(%\S+)\s*=\s*OpTypeSampler\b', assembly))
    combined = set(re.findall(r'(%\S+)\s*=\s*OpTypeSampledImage\b', assembly))
    result = {}
    for var, binding in decorations:
        assert sets.get(var) == 0, f"binding {binding} is not in descriptor set 0"
        assert int(binding) not in result, f"duplicate descriptor binding {binding} in set 0"
        storage, target = pointers[variables[var]]
        result[int(binding)] = "C" if storage == "Uniform" else "sampler" if target in samplers else "combined" if target in combined else images.get(target)
    return result


def main(glslc, dis):
    host = HOST.read_text(encoding="utf-8")
    image_names = re.search(r'enum \{ (FI_ACC1,.*?)\s*\};', host, re.S).group(1)
    names = [item.strip() for item in image_names.split(",") if item.strip() != "FI_N"]
    image_table = host.split("k_fi[FI_N] =", 1)[1].split("\n};", 1)[0]
    formats = re.findall(r'\{(VK_FORMAT_\w+),\s*\d+\}', image_table)
    assert len(names) == len(formats), "FSR image table and enum differ"
    image_formats = dict(zip(names, formats))
    for history in ("FI_UP1", "FI_UP2", "FI_LH1", "FI_LH2"):
        assert image_formats[history] == "VK_FORMAT_R16G16B16A16_SFLOAT", f"{history}: unexpected history format"
    const_decl = host.split("/* AMD's Fsr3UpscalerConstants", 1)[1].split("typedef struct {", 1)[1].split("} FsrConst;", 1)[0]
    fields = []
    for declaration in const_decl.split(";"):
        declaration = declaration.strip()
        if not declaration:
            continue
        match = re.fullmatch(r'(int32_t|float)\s+(.+)', declaration, re.S)
        assert match, f"unexpected FsrConst declaration: {declaration}"
        base = ctypes.c_int32 if match.group(1) == "int32_t" else ctypes.c_float
        for field in match.group(2).split(","):
            item = re.fullmatch(r'\s*(\w+)(?:\[(\d+)\])?\s*', field)
            assert item, f"unexpected FsrConst field: {field}"
            fields.append((item.group(1), base * int(item.group(2)) if item.group(2) else base))
    class FsrConst(ctypes.Structure):
        _fields_ = fields
    host_offsets = [getattr(FsrConst, name).offset for name, _ in fields]
    embedded = set(re.findall(r'^#embed "third_party/fsr3/([^"]+)"', host, re.M))
    defs_block = host.split("static const char k_fsr_defs[] =", 1)[1].split(";", 1)[0]
    defs = "".join(ast.literal_eval(part) for part in re.findall(r'"(?:[^"\\]|\\.)*"', defs_block))
    passes_block = host.split("static const FPass k_fpass[P_N] = {", 1)[1].split("\n};", 1)[0]
    passes = re.findall(r'\{"([^\"]+\.glsl)",\s*(\d+),\s*\{(.*?)\}\}', passes_block, re.S)
    assert len(passes) == 7, "expected seven vendored FSR passes"
    linear_inputs = set()
    linear_formats = set()
    with tempfile.TemporaryDirectory(prefix="fsr-reflect-") as temp:
        temp = Path(temp)
        for name, count, host_bindings in passes:
            source = expand(name, embedded)
            assert "#version" in source, name
            source = re.sub(r'(#version[^\n]*\n)', lambda m: m.group(1) + defs, source, count=1)
            src, spv = temp / "stage.glsl", temp / "stage.spv"
            src.write_text(source, encoding="utf-8")
            subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=compute", str(src), "-o", str(spv)], check=True)
            assembly = subprocess.run([dis, str(spv)], check=True, capture_output=True, text=True).stdout
            capabilities = set(re.findall(r'OpCapability\s+(\w+)', assembly))
            loads = dict(re.findall(r'(%\S+)\s*=\s*OpLoad\s+%\S+\s+(%\S+)', assembly))
            shader_bindings = {var: int(binding) for var, binding in re.findall(r'OpDecorate\s+(%\S+)\s+Binding\s+(\d+)', assembly)}
            host_resources = {int(binding): resource for binding, resource in re.findall(r'S_\(\s*(\d+)\s*,\s*(\w+)\s*\)', host_bindings)}
            aliases = {"R_ACC_S": "FI_ACC1", "R_LH_S": "FI_LH1", "R_LUMA_C": "FI_LUMA1"}
            for image, sampler in re.findall(r'%\S+\s*=\s*OpSampledImage\s+%\S+\s+(%\S+)\s+(%\S+)', assembly):
                if loads.get(sampler) == "%s_LinearClamp":
                    resource = loads.get(image, image)
                    linear_inputs.add(resource)
                    host_resource = host_resources[shader_bindings[resource]]
                    linear_formats.add(image_formats[aliases.get(host_resource, host_resource)])
            assert "StorageImageReadWithoutFormat" not in capabilities, f"{name}: unexpected formatless read"
            if "pyramid_pass" in name:
                assert {"GroupNonUniform", "GroupNonUniformQuad"} <= capabilities, f"{name}: missing subgroup capability"
            if "accumulate_pass" in name:
                assert "StorageImageWriteWithoutFormat" in capabilities, f"{name}: output capability changed"
            ubo_offsets = [(int(index), int(offset)) for index, offset in re.findall(
                r'OpMemberDecorate\s+%cbFSR3UPSCALER_t\s+(\d+)\s+Offset\s+(\d+)', assembly)]
            assert sorted(ubo_offsets) == list(enumerate(host_offsets)), f"{name}: FSR UBO offsets differ from host"
            spd_offsets = [(int(index), int(offset)) for index, offset in re.findall(
                r'OpMemberDecorate\s+%cbSPD_t\s+(\d+)\s+Offset\s+(\d+)', assembly)]
            if "pyramid_pass" in name:
                assert sorted(spd_offsets) == list(enumerate([0, 4, 8, 16])), f"{name}: SPD UBO offsets differ from host uint32[6]"
            bindings = sorted(int(n) for n in re.findall(r'OpDecorate\s+\S+\s+Binding\s+(\d+)', assembly))
            expected = sorted(int(n) for n in re.findall(r'[SUC]_\(\s*(\d+)\s*,', host_bindings))
            assert len(expected) == int(count), f"{name}: host binding count mismatch"
            expected += [1000, 1001]
            assert bindings == expected, f"{name}: bindings {bindings}, expected {expected}"
            host_types = {int(n): kind for kind, n in re.findall(r'([SUC])_\(\s*(\d+)\s*,', host_bindings)}
            host_types.update({1000: "sampler", 1001: "sampler"})
            actual = descriptor_types(assembly)
            assert actual == host_types, f"{name}: descriptor types {actual}, expected {host_types}"
            print(f"{name}: descriptor bindings and types verified")
        camera_block = host.split("static const char k_fsr_mv[] =", 1)[1].split(";\n", 1)[0]
        camera = "".join(ast.literal_eval(part) for part in re.findall(r'"(?:[^"\\]|\\.)*"', camera_block))
        src, spv = temp / "camera.glsl", temp / "camera.spv"
        src.write_text(camera, encoding="utf-8")
        subprocess.run([glslc, "--target-env=vulkan1.3", "-fshader-stage=compute", str(src), "-o", str(spv)], check=True)
        assembly = subprocess.run([dis, str(spv)], check=True, capture_output=True, text=True).stdout
        bindings = sorted(int(n) for n in re.findall(r'OpDecorate\s+\S+\s+Binding\s+(\d+)', assembly))
        assert bindings == [0, 1, 2], f"camera: unexpected bindings {bindings}"
        assert descriptor_types(assembly) == {0: "S", 1: "U", 2: "C"}, "camera: unexpected descriptor types"
        offsets = sorted((int(i), int(n)) for i, n in re.findall(r'OpMemberDecorate\s+%U\s+(\d+)\s+Offset\s+(\d+)', assembly))
        assert offsets == [(0, 0), (1, 64)], f"camera: unexpected UBO offsets {offsets}"
        print("camera: descriptor bindings and UBO offsets verified")
    expected_linear = {"%r_farthest_depth_mip1", "%r_luma_instability", "%r_dilated_reactive_masks",
                       "%r_luma_history", "%r_current_luma", "%r_transparency_and_composition_mask",
                       "%r_accumulation", "%r_shading_change", "%r_spd_mips"}
    assert linear_inputs == expected_linear, f"linear-filtered inputs changed: {linear_inputs ^ expected_linear}"
    filter_block = host.split("const VkFormat filtered[] =", 1)[1].split(";", 1)[0]
    checked_formats = set(re.findall(r'VK_FORMAT_\w+', filter_block))
    assert linear_formats == checked_formats, f"linear format checks differ from shader inputs: {linear_formats ^ checked_formats}"
    print("FSR shaderc reflection passed (runtime glslang and GPU not checked)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
