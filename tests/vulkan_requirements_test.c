/* cc -I runtime -I deps/android-arm64/include tests/vulkan_requirements_test.c -o /tmp/vulkan_requirements_test */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nvn_vk_requirements.h"
int main(void) {
    VkPhysicalDeviceFeatures f = {0};
    VkPhysicalDeviceVulkan12Features f12 = {0};
    VkPhysicalDeviceVulkan13Features f13 = {0};
    VkPhysicalDeviceVulkan11Properties p11 = {0};
    VkPhysicalDeviceVulkan12Properties p12 = {0};
    f.textureCompressionBC = f.shaderInt64 = f.independentBlend = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    f13.dynamicRendering = f13.synchronization2 = VK_TRUE;
    p12.shaderSignedZeroInfNanPreserveFloat32 = VK_TRUE;
    p11.subgroupSize = 32;
    p11.subgroupSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    p11.subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
        VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    assert(nvn_vk_requirement_error(&f, &f12, &f13, &p11, &p12) == NULL);
#define REJECT(field, expected) do { field = VK_FALSE; const char *e = nvn_vk_requirement_error(&f, &f12, &f13, &p11, &p12); assert(e && strstr(e, expected)); field = VK_TRUE; } while (0)
    REJECT(f.textureCompressionBC, "textureCompressionBC");
    REJECT(f.shaderInt64, "shaderInt64");
    REJECT(f.independentBlend, "independentBlend");
    REJECT(f12.hostQueryReset, "hostQueryReset");
    REJECT(f13.dynamicRendering, "dynamicRendering");
    REJECT(f13.synchronization2, "synchronization2");
    REJECT(p12.shaderSignedZeroInfNanPreserveFloat32, "shaderSignedZeroInfNanPreserveFloat32");
    p11.subgroupSize = 16;
    assert(strstr(nvn_vk_requirement_error(&f, &f12, &f13, &p11, &p12), "32 or 64"));
    p11.subgroupSize = 64;
    assert(!nvn_vk_requirement_error(&f, &f12, &f13, &p11, &p12));
    p11.subgroupSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
    assert(strstr(nvn_vk_requirement_error(&f, &f12, &f13, &p11, &p12), "fragment"));
    p11.subgroupSupportedStages |= VK_SHADER_STAGE_FRAGMENT_BIT;
    p11.subgroupSupportedOperations &= ~VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    assert(strstr(nvn_vk_requirement_error(&f, &f12, &f13, &p11, &p12), "subgroup"));
    puts("vulkan requirements: ok");
}
