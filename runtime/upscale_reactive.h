#ifndef WT_UPSCALE_REACTIVE_H
#define WT_UPSCALE_REACTIVE_H

/* Contract groundwork, NOT wired to a scene producer or backend.
 * Inputs are same-pixel, render-size, finite nonnegative linear RGB with equal
 * exposure/jitter/viewport, from a validated opaque boundary and current final.
 * pairValid requires matching frame, resource generation and render extent.
 * transparentCoverage is explicit classification, never inferred from blending.
 * Missing motion is coverage (1 missing), not a zero-vector sentinel.
 * Output RG: reactive, transparency/composition, both bounded [0,1]. */
#define WT_UPSCALE_REACTIVE_GLSL \
"vec2 wtReactiveMasks(vec3 opaque, vec3 finalColor, float transparentCoverage, float invalidMotion, bool pairValid, bool reset, bool enabled) {\n" \
"    if (reset || !pairValid) return vec2(1.0);\n" \
"    if (isnan(transparentCoverage) || isinf(transparentCoverage) || isnan(invalidMotion) || isinf(invalidMotion)) return vec2(1.0);\n" \
"    if (any(isnan(opaque)) || any(isinf(opaque)) || any(isnan(finalColor)) || any(isinf(finalColor))) return vec2(1.0);\n" \
"    if (!enabled) return vec2(0.0);\n" \
"    vec3 difference = abs(finalColor - opaque);\n" \
"    float peak = max(max(max(opaque.r, opaque.g), opaque.b), max(max(finalColor.r, finalColor.g), finalColor.b));\n" \
"    float change = clamp(max(max(difference.r, difference.g), difference.b) / max(peak, 0.0001), 0.0, 1.0);\n" \
"    float composition = max(change, clamp(transparentCoverage, 0.0, 1.0));\n" \
"    return vec2(max(composition, clamp(invalidMotion, 0.0, 1.0)), composition);\n" \
"}\n" \
"float wtReactiveHistoryWeight(float historyWeight, vec2 masks) {\n" \
"    return clamp(historyWeight, 0.0, 1.0) * (1.0 - clamp(max(masks.x, masks.y), 0.0, 1.0));\n" \
"}\n"

#endif
