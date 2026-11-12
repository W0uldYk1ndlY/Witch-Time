"""Offline production-CS motion and temporal oracles; no HDR-quality claim.

Usage: python tests/sgsr_cs_focused_gpu_test.py <glslc> <spirv-dis> [--compile-only]

Extract the existing disposable Vulkan setup with checked, single-occurrence edits.
No runtime, existing test, device, or capture files are modified.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from sgsr_cs_source_test import main as compile_sources, ROOT


def replace(source, old, new):
    assert source.count(old) == 1, f"GPU helper extraction drift: {old[:100]}"
    return source.replace(old, new)


def harness():
    source = (ROOT / "tests/sgsr_cs_gpu_test.c").read_text()
    source = replace(source, '#include "sgsr_gpu_test.c"', '#include "sgsr_gpu_test.c"\n#include "sgsr_cs_focused_oracle.h"')
    source = replace(source, 'float intensity = (float)atof(argv[7]);', '''float intensity = (float)atof(argv[7]);
    int temporal = getenv("WT_CS_PATTERN") ? 2 : getenv("WT_CS_TEMPORAL") != NULL;''')
    source = replace(source, 'Buffer uniform[3];', '''if (temporal) params.min_lerp_contribution = 1;
    else {
        params.jitter_offset[0] = 0.25f; params.jitter_offset[1] = -0.125f;
        params.clip_to_prev_clip[0] = 0.875f;
        params.clip_to_prev_clip[4] = 0.125f;
        params.clip_to_prev_clip[8] = 0.25f;
        params.clip_to_prev_clip[9] = -0.125f;
        params.clip_to_prev_clip[11] = 0.5f;
        params.clip_to_prev_clip[12] = 0.0625f;
        params.clip_to_prev_clip[13] = -0.03125f;
        if (getenv("WT_CS_WRONG_MATRIX")) params.clip_to_prev_clip[8] = 0;
    }
    Buffer uniform[3];''')
    source = replace(source, 'params.reset = f != 1;', 'params.reset = temporal == 2 || f != 1; if (f == 2 && getenv("WT_CS_SKIP_RESET")) params.reset = 0;')
    source = replace(source, 'VkSamplerCreateInfo sci', '''Buffer extra[3]; for (int f = 0; f < 3; f++) extra[f] = buffer_new((VkDeviceSize)(temporal ? ow * oh : w * h) * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer upload = buffer_new((VkDeviceSize)w * h * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 4; c++)
        ((uint16_t *)upload.mapped)[(y * w + x) * 4 + c] = float_half(c == 3 ? 0 : (getenv("WT_CS_FLAT_PATTERN") ? 1 : focused_level(x, y)) / (1 << c));
    VkSamplerCreateInfo sci''')
    source = replace(source, 'if (i == DEPTH) clear.float32[0] = 1;', 'if (i == DEPTH) clear.float32[0] = 0.25f;')
    source = replace(source, 'p ? previous : COLOR', 'p ? (getenv("WT_CS_SKIP_HISTORY") ? HISTORY0 : previous) : COLOR')
    source = replace(source, '/* Poison output each frame:', '''if (temporal == 2) {
            barrier(cmd, images[COLOR].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy pattern = {0}; pattern.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            pattern.imageExtent = (VkExtent3D){(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(cmd, upload.buffer, images[COLOR].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &pattern);
            barrier(cmd, images[COLOR].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        if (temporal == 1) {
            barrier(cmd, images[COLOR].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            float level = f == 0 ? 1 : f == 1 ? 4 : 2;
            VkClearColorValue color = {{level, level * 0.5f, level * 0.25f, 0}};
            vkCmdClearColorImage(cmd, images[COLOR].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
            barrier(cmd, images[COLOR].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        /* Poison output each frame:''')
    source = replace(source, 'barrier(cmd, images[OUTPUT].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);', '''barrier(cmd, images[OUTPUT].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        int observed = temporal ? HISTORY0 + ((f & 1) ^ 1) : MOTION;
        barrier(cmd, images[observed].image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        copy.imageExtent = (VkExtent3D){(uint32_t)(temporal ? ow : w), (uint32_t)(temporal ? oh : h), 1};
        vkCmdCopyImageToBuffer(cmd, images[observed].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, extra[f].buffer, 1, &copy);
        barrier(cmd, images[observed].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);''')
    start = source.index('    int failed = 0;')
    end = source.index('    vkDestroyDescriptorPool', start)
    source = source[:start] + '''    int failed = focused_check(temporal, w, h, ow, oh, extra, readback);
''' + source[end:]
    source = replace(source, 'Buffer buffers[] = {uniform[f], readback[f]};', 'Buffer buffers[] = {uniform[f], readback[f], extra[f]};')
    source = replace(source, 'for (int i = 0; i < 2; i++) { vkUnmapMemory', 'for (int i = 0; i < 3; i++) { vkUnmapMemory')
    source = replace(source, 'vkDestroyCommandPool(device, pool, NULL);', 'vkUnmapMemory(device, upload.memory); vkDestroyBuffer(device, upload.buffer, NULL); vkFreeMemory(device, upload.memory, NULL);\n    vkDestroyCommandPool(device, pool, NULL);')
    return source


def main(glslc, dis, compile_only=False):
    stages = compile_sources(glslc, dis)
    with tempfile.TemporaryDirectory(prefix="sgsr-cs-focused-") as tmp:
        tmp = Path(tmp)
        paths = []
        for stage in ("convert", "upscale"):
            path = tmp / (stage + ".spv")
            path.write_bytes(stages[stage])
            paths.append(str(path))
        c = tmp / "focused.c"
        c.write_text(harness())
        exe = tmp / ("focused.exe" if os.name == "nt" else "focused")
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
                   "-I", str(ROOT / "runtime"), "-I", str(ROOT / "tests"),
                   "-I", str(ROOT / "deps/android-arm64/include"), str(c), "-o", str(exe)]
        if os.name != "nt": command += ["-ldl", "-lm"]
        subprocess.run(command, check=True)
        if compile_only:
            print("Focused CS shaders and harness compiled (GPU numerical checks and mutations not run)")
            return
        env = {k: v for k, v in os.environ.items() if not k.startswith(("WT_CS_", "WT_SGSR_TEST_"))}
        for temporal in (0, 1, 2):
            case_env = {**env, **({"WT_CS_PATTERN" if temporal == 2 else "WT_CS_TEMPORAL": "1"} if temporal else {})}
            for w, h in ((1, 1), (17, 13), (9, 7), (17, 13)):
                args = [str(exe), *paths, str(w), str(h), str(w), str(h), "4"]
                subprocess.run(args, env=case_env, check=True, timeout=60)
            controls = ("WT_CS_FLAT_PATTERN", "WT_SGSR_TEST_SKIP_FINAL") if temporal == 2 else ("WT_CS_SKIP_HISTORY", "WT_CS_SKIP_RESET") if temporal else ("WT_CS_WRONG_MATRIX",)
            for control in controls:
                result = subprocess.run(args, env={**case_env, control: "1"}, capture_output=True, text=True, timeout=60)
                assert result.returncode == 1 and "FAIL" in result.stdout, (control, result.stdout, result.stderr)
                print(f"negative control rejected: {control}")
        print("PASS: analytic camera/depth/jitter, temporal/reset shader roles, 1:1 patterned reset edges")
        print("NOT validated: host resize orchestration, scaled color reconstruction, HDR quality, device behavior")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4) or (len(sys.argv) == 4 and sys.argv[3] != "--compile-only"):
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2], len(sys.argv) == 4)
