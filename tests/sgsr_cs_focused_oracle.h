/* Analytic scenes, not an emulator of the full upstream reconstruction shader. */
/* Avoid the exact integer quantization boundary at level=4 (Co*2047.5=1638),
 * where fused arithmetic can legally choose adjacent packed codes. */
static float focused_level(int x, int y) { return (x + 2 * y) % 3 == 0 ? 0.5f : (x + y) % 2 ? 1 : 2; }

static void packed_scene(float level, float result[3]) {
    float r = level / (level + 1), g = r * 0.5f, b = r * 0.25f;
    float y = (r + 2 * g + b) * 0.25f;
    float co = (r - b) * 0.5f + 0.5f, cg = y + co - r;
    result[0] = (unsigned)(y * 2047.5f) / 2047.5f;
    result[1] = (unsigned)(co * 2047.5f) / 2047.5f - 0.5f;
    result[2] = (unsigned)(cg * 1023.5f) / 1023.5f - 0.5f;
}

static int focused_check(int temporal, int w, int h, int ow, int oh, Buffer extra[3], Buffer output[3]) {
    int failed = 0;
    float previous[3] = {0};
    for (int f = 0; f < 3; f++) {
        float current[3], full[3]; packed_scene(f == 0 ? 1 : f == 1 ? 4 : 2, current);
        /* At 1:1, zero jitter, W=0: center Lanczos weight=1/4,
         * four axial weights=0; temporal weight=1/12, baseupdate=1.
         * MinLerpContribution=1 deliberately preserves out-of-box history. */
        for (int c = 0; c < 3; c++) {
            float value = f == 1 ? (12 * previous[c] + current[c]) / 13 : current[c];
            full[c] = value;
            /* Existing harness float_half accepts positive values only. */
            current[c] = copysignf(half_float(float_half(fabsf(value))), value);
        }
        int width = temporal ? ow : w, height = temporal ? oh : h;
        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
            if (temporal == 2) {
                /* Reset at 1:1 leaves only the center tap; axial weights vanish.
                 * Thus checker/stripe discontinuities and clamped image edges
                 * have an exact packed-color oracle without copying the filter. */
                packed_scene(focused_level(x, y), current);
                memcpy(full, current, sizeof full);
                for (int c = 0; c < 3; c++) current[c] = copysignf(half_float(float_half(fabsf(current[c]))), current[c]);
            }
            float expected[4];
            if (temporal) { for (int c = 0; c < 3; c++) expected[c] = current[c]; expected[3] = 0; }
            else {
                /* Independent homogeneous projection of unjittered NDC at z=1/4.
                 * Non-diagonal matrix distinguishes transpose, Y sign and divide. */
                float sx = 2 * (x + 0.5f - 0.25f) / w - 1;
                float sy = 1 - 2 * (y + 0.5f + 0.125f) / h;
                expected[0] = sx - (0.875f * sx + 0.125f * sy + 0.25f * 0.25f + 0.0625f) / 1.125f;
                expected[1] = sy - (sy - 0.125f * 0.25f - 0.03125f) / 1.125f;
                expected[2] = 0; expected[3] = 5;
            }
            for (int c = 0; c < 4; c++) {
                float actual = half_float(((uint16_t *)extra[f].mapped)[(y * width + x) * 4 + c]);
                if (!isfinite(actual) || fabsf(actual - expected[c]) > 0.0015f) {
                    if (!failed) fprintf(stderr, "frame=%d xy=%d,%d channel=%d actual=%g expected=%g\n", f, x, y, c, actual, expected[c]);
                    failed++;
                }
            }
            /* Temporal scene output must agree with analytic history inverse-map. */
            if (temporal) {
                float rgb[3] = {full[0] - full[2] + full[1], full[0] + full[2], full[0] - full[2] - full[1]};
                float maximum = fmaxf(rgb[0], fmaxf(rgb[1], rgb[2]));
                float scale = 1 / (1 + 600.0f / 65504.0f - maximum);
                for (int c = 0; c < 3; c++) {
                    float actual = half_float(((uint16_t *)output[f].mapped)[(y * ow + x) * 4 + c]);
                    if (!isfinite(actual) || fabsf(actual - rgb[c] * scale) > 0.01f) {
                        if (!failed) fprintf(stderr, "output frame=%d xy=%d,%d channel=%d actual=%g expected=%g\n", f, x, y, c, actual, rgb[c] * scale);
                        failed++;
                    }
                }
            }
        }
        memcpy(previous, current, sizeof previous);
    }
    printf("focused CS %s %dx%d: %s (%d channels)\n", temporal == 2 ? "1:1 patterned reset edges" : temporal ? "history pingpong/reset" : "camera/depth/jitter", w, h, failed ? "FAIL" : "PASS", failed);
    return failed;
}
