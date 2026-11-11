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

static int fsr_output_writable(int image_supported, int global_write, int format_write) {
    return image_supported && (global_write || format_write);
}

static int fsr_descriptor_limits(unsigned sampled_stage, unsigned sampled_set,
                                 unsigned uniforms_stage, unsigned uniforms_set,
                                 unsigned samplers_stage, unsigned samplers_set, unsigned stage_resources) {
    return sampled_stage >= 9 && sampled_set >= 9 && uniforms_stage >= 2 && uniforms_set >= 2 &&
           samplers_stage >= 2 && samplers_set >= 2 && stage_resources >= 15;
}

static int fsr_lut_supported(int sampled, int linear, int uses_lut) {
    return sampled && (!uses_lut || linear);
}

static int fsr_dispatch_fits(unsigned width, unsigned height, unsigned max_x, unsigned max_y) {
    return width > 0 && height > 0 && (width / 8 + (width % 8 != 0)) <= max_x &&
           (height / 8 + (height % 8 != 0)) <= max_y;
}

#endif
