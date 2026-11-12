#ifndef WT_UPSCALE_MOTION_H
#define WT_UPSCALE_MOTION_H

/* Raw RG16F is previousUV-currentUV, top-left, unjittered, TOTAL displacement.
 * Separate R8 validity (0 missing, 1 valid) is mandatory: valid zero is not missing.
 * No producer is implied by this adapter. Missing owned inputs remain camera-only. */
#define WT_UPSCALE_MOTION_GLSL \
"vec2 wtMotionUnjitterNdc(vec2 rasterNdc, vec2 jitterPixels, vec2 renderRcp) {\n" \
"    return rasterNdc - vec2(2.0, -2.0) * jitterPixels * renderRcp;\n" \
"}\n" \
"vec2 wtMotionCamera(vec2 rasterUV, float depth, mat4 clipToPrevious, vec2 jitterPixels, vec2 renderRcp) {\n" \
"    vec2 current = wtMotionUnjitterNdc(vec2(2.0, -2.0) * rasterUV + vec2(-1.0, 1.0), jitterPixels, renderRcp);\n" \
"    vec4 previous = clipToPrevious * vec4(current, depth, 1.0);\n" \
"    return (previous.xy / previous.w - current) * vec2(0.5, -0.5);\n" \
"}\n" \
"vec2 wtMotionSelect(vec2 rawTotal, bool valid, vec2 camera) { return valid ? rawTotal : camera; }\n" \
"vec2 wtMotionFsr(vec2 canonical) { return canonical; }\n" \
"vec2 wtMotionSgsr(vec2 canonical) { return vec2(-2.0, 2.0) * canonical; }\n"

#endif
