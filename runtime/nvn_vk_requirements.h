/* Baseline requirements of the current Maxwell -> Vulkan implementation.
 * API version alone is not enough, especially on mobile GPUs. No diagnostic
 * override may turn these into warnings and produce an invalid device. */
#ifndef WT_NVN_VK_REQUIREMENTS_H
#define WT_NVN_VK_REQUIREMENTS_H
#include <stddef.h>
#include <vulkan/vulkan.h>
static inline const char *nvn_vk_requirement_error(
    const VkPhysicalDeviceFeatures *f, const VkPhysicalDeviceVulkan12Features *f12,
    const VkPhysicalDeviceVulkan13Features *f13, const VkPhysicalDeviceVulkan11Properties *p11,
    const VkPhysicalDeviceVulkan12Properties *p12) {
    if (!f13->dynamicRendering) return "dynamicRendering";
    if (!f13->synchronization2) return "synchronization2";
    if (!f12->hostQueryReset) return "hostQueryReset (host-side query pool resets)";
    if (!f->textureCompressionBC) return "textureCompressionBC (BC1-BC7 textures have no portable fallback yet)";
    if (!f->shaderInt64) return "shaderInt64 (Maxwell integer shader operations)";
    if (!f->independentBlend) return "independentBlend (independent render-target blend state)";
    if (!p12->shaderSignedZeroInfNanPreserveFloat32) return "shaderSignedZeroInfNanPreserveFloat32 (translated shader execution mode)";
    if (p11->subgroupSize != 32 && p11->subgroupSize != 64) return "subgroupSize of 32 or 64 (Maxwell warp emulation)";
    VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if ((p11->subgroupSupportedStages & stages) != stages) return "subgroups in compute and fragment stages";
    VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
        VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    if ((p11->subgroupSupportedOperations & ops) != ops) return "subgroup basic, vote, arithmetic, ballot and shuffle operations";
    return NULL;
}
#endif
