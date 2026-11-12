"""Execute production CS capability, resize and setup cleanup with mocked Vulkan."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "runtime/nvn_vk_sgsr_cs.c").read_text(encoding="utf-8")


def function(name):
    return re.search(r'static [^\n]+[ *]' + name + r'\(.*?\n\}', source, re.S).group(0)


harness = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sgsr2_params.h"
typedef struct { int w, h; VkImage img; VkImageLayout layout; } VTex;
typedef struct { VTex *vt; VkImageView all; } FImg;
enum { SC_MOTION, SC_PACKED, SC_HISTORY0, SC_HISTORY1, SC_OUTPUT, SC_ZERO, SC_IMAGES };
static struct { VkPhysicalDeviceProperties props; VkPhysicalDevice gpu; VkDevice device; } V;
static struct { int has_push, fsr_push_descriptors; } D;
static struct { int cmd; } R;
static struct { FImg image[SC_IMAGES]; int w, h, ow, oh, history, have_history; } SC;
static int allocated, destroyed, retired, cleared, fail_at = -1;
static VkFormat unsupported_format;
static int fmt_supported(VkFormat f, VkFormatFeatureFlags need) { (void)need; return f != unsupported_format; }
static VkResult vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice gpu, VkFormat f, VkImageType t,
        VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *p) {
    (void)gpu; (void)t; (void)tiling; (void)flags;
    assert(usage & VK_IMAGE_USAGE_SAMPLED_BIT);
    if (f == unsupported_format) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    memset(p, 0, sizeof *p); p->maxExtent = (VkExtent3D){4096, 4096, 1};
    p->maxMipLevels = p->maxArrayLayers = 1; p->sampleCounts = VK_SAMPLE_COUNT_1_BIT; return VK_SUCCESS;
}
static int sgsr_cs_image_new(FImg *out, int i, int w, int h) {
    (void)i;
    if (allocated++ == fail_at) return 0;
    out->vt = calloc(1, sizeof *out->vt); assert(out->vt);
    out->vt->w = w; out->vt->h = h; return 1;
}
static void vt_destroy(VTex *vt) { destroyed++; free(vt); }
static void vt_garbage(VTex *vt) { retired++; free(vt); }
static void vt_layout(VTex *vt, VkImageLayout to) { vt->layout = to; }
static void vkCmdClearColorImage(int cmd, VkImage image, VkImageLayout layout, const VkClearColorValue *zero,
                                uint32_t n, const VkImageSubresourceRange *range) {
    (void)cmd; (void)image; assert(layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(zero->uint32[0] == 0 && n == 1 && range->levelCount == 1); cleared++;
}
''' + '\n'.join(function(n) for n in ["sgsr_cs_format", "sgsr_cs_usage", "sgsr_cs_unable", "sgsr_cs_retire_images", "sgsr_cs_resize"]) + r'''
int main(void) {
    VkPhysicalDeviceLimits *l = &V.props.limits;
    memset(l, 0xff, sizeof *l); D.has_push = 1; D.fsr_push_descriptors = 6;
    assert(!sgsr_cs_unable());
    D.fsr_push_descriptors = 5; assert(sgsr_cs_unable()); D.fsr_push_descriptors = 6;
    l->maxComputeWorkGroupInvocations = 63; assert(sgsr_cs_unable()); l->maxComputeWorkGroupInvocations = 64;
    l->maxPerStageDescriptorStorageImages = 1; assert(sgsr_cs_unable()); l->maxPerStageDescriptorStorageImages = 2;
    l->maxUniformBufferRange = 143; assert(sgsr_cs_unable()); l->maxUniformBufferRange = 144;
    unsupported_format = VK_FORMAT_R32_UINT; assert(sgsr_cs_unable()); unsupported_format = 0;
    assert(!sgsr_cs_unable());
    for (int i = 0; i < SC_IMAGES; i++) {
        assert(sgsr_cs_format(i) == (i == SC_PACKED ? VK_FORMAT_R32_UINT : VK_FORMAT_R16G16B16A16_SFLOAT));
        assert(!!(sgsr_cs_usage(i) & VK_IMAGE_USAGE_STORAGE_BIT) == (i != SC_ZERO));
        assert(!!(sgsr_cs_usage(i) & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == (i == SC_OUTPUT));
    }
    for (int failure = 0; failure < SC_IMAGES; failure++) {
        allocated = destroyed = cleared = 0; fail_at = failure;
        SC.w = 5; SC.have_history = 1;
        assert(!sgsr_cs_resize(17, 13, 25, 19));
        assert(destroyed == failure && retired == 0 && cleared == 0 && SC.w == 5 && SC.have_history);
    }
    fail_at = -1; allocated = 0; assert(sgsr_cs_resize(17, 13, 25, 19));
    assert(cleared == SC_IMAGES && !SC.have_history);
    assert(SC.image[SC_ZERO].vt->w == 17 && SC.image[SC_PACKED].vt->h == 13);
    assert(SC.image[SC_HISTORY0].vt->w == 25 && SC.image[SC_HISTORY1].vt->h == 19 && SC.image[SC_OUTPUT].vt->w == 25);
    allocated = 0; assert(sgsr_cs_resize(19, 15, 29, 23)); assert(retired == SC_IMAGES);
    unsupported_format = VK_FORMAT_R32_UINT; allocated = destroyed = 0;
    assert(!sgsr_cs_resize(21, 17, 31, 25)); assert(destroyed == 1 && SC.w == 19);
    unsupported_format = 0; assert(!sgsr_cs_resize(5000, 13, 5000, 19));
    sgsr_cs_retire_images(); assert(retired == SC_IMAGES * 2 && !SC.have_history && !SC.w);
    sgsr_cs_retire_images(); assert(retired == SC_IMAGES * 2);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="sgsr-cs-resources-") as directory:
    directory = Path(directory)
    src, exe = directory / "test.c", directory / ("test.exe" if os.name == "nt" else "test")
    src.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "runtime"), "-I", str(ROOT / "deps/android-arm64/include"), str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("SGSR CS production capability/resize/partial allocation/deferred retirement checks passed (mock GPU)")
