#ifndef WT_FSR_LIMITS_H
#define WT_FSR_LIMITS_H

/* The luma-pyramid pass has nine storage images and fifteen push descriptors. */
static const char *fsr_limit_error(unsigned invocations, unsigned workgroup_x, unsigned workgroup_y, unsigned shared_memory,
                                   unsigned storage_per_stage, unsigned storage_per_set,
                                   unsigned push_descriptors, int compute_quad, int lut_sampled) {
    if (invocations < 256 || workgroup_x < 256) return "FSR needs a 256-invocation compute workgroup";
    if (workgroup_y < 8) return "FSR needs eight compute workgroup rows";
    if (shared_memory < 4100) return "FSR needs 4100 bytes of compute shared memory";
    if (storage_per_stage < 9 || storage_per_set < 9) return "FSR needs nine storage images per compute stage and descriptor set";
    if (push_descriptors < 15) return "FSR needs fifteen push descriptors";
    if (!compute_quad) return "FSR needs subgroup quad operations in compute";
    if (!lut_sampled) return "FSR needs sampled R16_SNORM LUT images";
    return 0;
}

#endif
