#!/usr/bin/env python3
"""Execute production allocator/image barriers with recorded Vulkan calls.

No Vulkan device is needed. This checks allocation lifetime and synchronization
operands, not GPU rendering or mobile memory bandwidth.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "runtime/nvn_vk_draw.c").read_text()


def section(start, end):
    return SOURCE[SOURCE.index(start):SOURCE.index(end, SOURCE.index(start))]


def run_c(source):
    with tempfile.TemporaryDirectory(prefix="vk-memory-barriers-") as td:
        path = Path(td)
        (path / "test.c").write_text(source)
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(compiler + ["-std=c11", "-O2", "-Wall", "-Wextra",
            "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-missing-field-initializers",
            str(path / "test.c"), "-o", str(path / "test")], check=True)
        return subprocess.run([str(path / "test")], text=True, capture_output=True)


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
typedef int64_t LONG64;
static struct { VkDevice device; VkPhysicalDevice gpu; } V;
static struct { int has_budget; VkPhysicalDeviceMemoryProperties mp; VkPhysicalDeviceFeatures2 f2; } D;
static struct { int in_pass; VkCommandBuffer cmd; } R;
#define VKC(expr) assert((expr) == VK_SUCCESS)
#define DBG_ASSERT(expr, ...) assert(expr)
static void vkd_fail(const char *fmt, ...) {
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
    abort();
}
static const char *vk_result_name(VkResult r) { (void)r; return "failed"; }
static unsigned allocations, frees, maps;
static VkResult vkAllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *a,
        const VkAllocationCallbacks *cb, VkDeviceMemory *m) {
    (void)dev; (void)cb; assert(a->allocationSize); allocations++;
    *m = (VkDeviceMemory)(uintptr_t)allocations; return VK_SUCCESS;
}
static void vkFreeMemory(VkDevice dev, VkDeviceMemory m,
        const VkAllocationCallbacks *cb) {
    (void)dev; (void)cb; assert(m); frees++;
}
static VkResult vkMapMemory(VkDevice dev, VkDeviceMemory m, VkDeviceSize off,
        VkDeviceSize size, VkMemoryMapFlags flags, void **p) {
    (void)dev; (void)off; (void)size; (void)flags; maps++;
    *p = (void *)(uintptr_t)((uintptr_t)m << 28); return VK_SUCCESS;
}
static void vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice gpu,
        VkPhysicalDeviceMemoryProperties2 *p) { (void)gpu; (void)p; abort(); }
static VkFlags observed_src, observed_dst;
static VkImageMemoryBarrier observed_image;
static unsigned barriers;
static void vkCmdPipelineBarrier(VkCommandBuffer cmd, VkFlags src, VkFlags dst,
        VkDependencyFlags flags, uint32_t nmem, const VkMemoryBarrier *mem,
        uint32_t nbuf, const VkBufferMemoryBarrier *buf, uint32_t nimage,
        const VkImageMemoryBarrier *image) {
    (void)cmd; (void)flags; (void)mem; (void)buf;
    assert(!nmem && !nbuf && nimage == 1); barriers++;
    observed_src = src; observed_dst = dst; observed_image = *image;
}
'''


class VulkanMemoryBarrierTest(unittest.TestCase):
    def test_image_memory_is_unmapped_and_empty_blocks_are_bounded(self):
        code = section("typedef struct { int block;", "/* [issue 153] what the blocks hold")
        result = run_c(PRELUDE + code + r'''
int main(void) {
    D.mp.memoryTypeCount = 1; D.mp.memoryHeapCount = 1;
    D.mp.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    D.mp.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    D.mp.memoryHeaps[0].size = UINT64_C(8) << 30;
    VkMemoryRequirements rq = {UINT64_C(16) << 20, 4096, 1};
    VMem images[8];
    for (unsigned i = 0; i < 8; i++) {
        images[i] = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        assert(images[i].off % 4096 == 0);
        assert(images[i].map == NULL); /* Optimal images need no CPU mapping. */
    }
    assert(allocations == 2 && maps == 0 && frees == 0);
    for (unsigned i = 0; i < 7; i++) vmem_free(images[i]);
    assert(frees == 0); /* The second block still has a live image. */
    vmem_free(images[7]);
    assert(frees == 1 && g_mem_total == (INT64_C(64) << 20));
    assert(g_mem_used == 0);
    VMem reused = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    assert(allocations == 2 && maps == 0); /* One warm block is reused. */
    vmem_free(reused);
    VMem buffer = vmem_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1);
    assert(buffer.map && maps == 1);
    memset(&rq, 0, sizeof rq); rq.size = UINT64_C(128) << 20;
    rq.alignment = 4096; rq.memoryTypeBits = 1;
    VMem large = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    assert(large.map == NULL && maps == 1);
    unsigned before = frees; vmem_free(large); assert(frees == before + 1);
    vmem_free(buffer);
    puts("image mapping and empty-block lifetime passed"); return 0;
}
''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_image_barriers_keep_dependencies_with_narrow_known_stages(self):
        code = section("static VkFlags layout_access(", "static void vt_layout(")
        result = run_c(PRELUDE + code + r'''
int main(void) {
    VkImage image = (VkImage)(uintptr_t)9;
    image_barrier(image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert(observed_src == VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    assert(observed_dst & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    assert(observed_dst & VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    assert(observed_dst & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    assert(!(observed_dst & (VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT)));
    assert(!(observed_dst & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
    assert(observed_image.srcAccessMask & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    assert(observed_image.dstAccessMask == VK_ACCESS_SHADER_READ_BIT);
    assert(observed_image.image == image);
    assert(observed_image.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    assert(observed_image.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    assert(observed_image.subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS);
    assert(observed_image.subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS);
    image_barrier(image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(observed_src == VK_PIPELINE_STAGE_TRANSFER_BIT && observed_dst == VK_PIPELINE_STAGE_TRANSFER_BIT);
    assert(observed_image.srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
    assert(observed_image.dstAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
    image_barrier(image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    assert(observed_src == (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT));
    assert(observed_dst == VK_PIPELINE_STAGE_TRANSFER_BIT);
    assert(observed_image.srcAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    image_barrier(image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    /* Reused temporary images can discard content while earlier reads still
       need to complete: UNDEFINED is not proof that this allocation is new. */
    assert(observed_src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    assert(observed_image.srcAccessMask == 0 && observed_dst == VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    assert(observed_src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && observed_dst == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    assert(observed_image.srcAccessMask & VK_ACCESS_MEMORY_WRITE_BIT);
    assert(observed_image.dstAccessMask & VK_ACCESS_MEMORY_READ_BIT);
    D.f2.features.geometryShader = D.f2.features.tessellationShader = VK_TRUE;
    image_barrier(image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert((observed_dst & (VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT)) ==
        (VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT));
    assert(barriers == 6); puts("image dependency scopes passed"); return 0;
}
''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
