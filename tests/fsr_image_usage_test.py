"""Exercise the production image-usage queries with mocked Vulkan properties."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "runtime" / "nvn_vk_fsr.c").read_text(encoding="utf-8")
usage = re.search(r'static int fsr_image_usage\(.*?\n\}', source, re.S)
output = re.search(r'static int fsr_query_output_format\(.*?\n\}', source, re.S)
assert usage and output
harness = r'''
#include <assert.h>
#include <stdint.h>
typedef int VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkResult;
typedef uint32_t VkImageUsageFlags, VkImageCreateFlags, VkSampleCountFlags;
typedef uint64_t VkFormatFeatureFlags2;
typedef struct { uint32_t width, height, depth; } Extent;
typedef struct { Extent maxExtent; uint32_t maxMipLevels, maxArrayLayers; VkSampleCountFlags sampleCounts; } VkImageFormatProperties;
typedef struct { int sType; void *pNext; VkFormatFeatureFlags2 optimalTilingFeatures; } VkFormatProperties3;
typedef struct { int sType; void *pNext; } VkFormatProperties2;
typedef struct { struct { int shaderStorageImageWriteWithoutFormat; } features; } VkPhysicalDeviceFeatures2;
enum { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 = 1, VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 = 2,
       VK_IMAGE_USAGE_SAMPLED_BIT = 1, VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 2, VK_IMAGE_USAGE_TRANSFER_DST_BIT = 4,
       VK_IMAGE_USAGE_STORAGE_BIT = 8, VK_IMAGE_TYPE_2D = 1, VK_IMAGE_TILING_OPTIMAL = 1,
       VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT = 1, VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT = 2,
       VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT = 4, VK_SUCCESS = 0, VK_ERROR_FORMAT_NOT_SUPPORTED = -1,
       VK_SAMPLE_COUNT_1_BIT = 1, VK_FORMAT_R16_SFLOAT = 1, VK_FORMAT_R16G16B16A16_SFLOAT = 2, VK_TRUE = 1 };
#include "fsr_limits.h"
static struct { VkPhysicalDevice gpu; } V;
static struct { VkPhysicalDeviceFeatures2 f2; } D;
static VkFormatFeatureFlags2 flags;
static VkResult image_result;
static VkImageUsageFlags observed_usage;
static uint32_t max_width = 2048, max_height = 1024, max_mips = 12, max_layers = 1;
static VkSampleCountFlags samples = VK_SAMPLE_COUNT_1_BIT;
static void vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice gpu, VkFormat format, VkFormatProperties2 *out) {
    (void)gpu; (void)format;
    ((VkFormatProperties3 *)out->pNext)->optimalTilingFeatures = flags;
}
static VkResult vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice gpu, VkFormat format, VkImageType type,
    VkImageTiling tiling, VkImageUsageFlags usage, VkImageCreateFlags create, VkImageFormatProperties *out) {
    (void)gpu; (void)format; (void)type; (void)tiling; (void)create;
    out->maxExtent.width = max_width; out->maxExtent.height = max_height; out->maxExtent.depth = 1;
    out->maxMipLevels = max_mips; out->maxArrayLayers = max_layers; out->sampleCounts = samples;
    observed_usage = usage;
    return image_result;
}
''' + usage.group(0) + '\n' + output.group(0) + r'''
int main(void) {
    VkImageUsageFlags common = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    flags = VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
    image_result = VK_SUCCESS;
    assert(fsr_image_usage(VK_FORMAT_R16_SFLOAT, 0, 2048, 1024, 12));
    assert(observed_usage == common);
    assert(fsr_image_usage(VK_FORMAT_R16_SFLOAT, 1, 2048, 1024, 12));
    assert(observed_usage == (common | VK_IMAGE_USAGE_STORAGE_BIT));
    assert(!fsr_image_usage(VK_FORMAT_R16_SFLOAT, 1, 2049, 1024, 12));
    assert(!fsr_image_usage(VK_FORMAT_R16_SFLOAT, 1, 2048, 1025, 12));
    assert(!fsr_image_usage(VK_FORMAT_R16_SFLOAT, 1, 2048, 1024, 13));
    max_layers = 0; assert(!fsr_image_usage(VK_FORMAT_R16_SFLOAT, 1, 1, 1, 1)); max_layers = 1;
    samples = 0; assert(!fsr_image_usage(VK_FORMAT_R16_SFLOAT, 1, 1, 1, 1)); samples = VK_SAMPLE_COUNT_1_BIT;
    assert(!fsr_query_output_format(VK_FORMAT_R16G16B16A16_SFLOAT));
    flags |= VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    assert(fsr_query_output_format(VK_FORMAT_R16G16B16A16_SFLOAT));
    flags &= ~VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    D.f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    assert(fsr_query_output_format(VK_FORMAT_R16G16B16A16_SFLOAT));
    image_result = VK_ERROR_FORMAT_NOT_SUPPORTED;
    assert(!fsr_query_output_format(VK_FORMAT_R16G16B16A16_SFLOAT));
    image_result = VK_SUCCESS;
    flags &= ~VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
    assert(!fsr_query_output_format(VK_FORMAT_R16G16B16A16_SFLOAT));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="fsr-image-") as temp:
    temp = Path(temp)
    c_file, exe = temp / "test.c", temp / ("test.exe" if os.name == "nt" else "test")
    c_file.write_text(harness, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-missing-field-initializers",
                    "-I", str(ROOT / "runtime"),
                    str(c_file), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("FSR image usage checks passed (mock Vulkan properties)")
