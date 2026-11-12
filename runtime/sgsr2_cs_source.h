#ifndef WT_SGSR2_CS_SOURCE_H
#define WT_SGSR2_CS_SOURCE_H
#include <stdlib.h>
#include <string.h>

/* Adapt copies only: the vendored Qualcomm sources remain byte-for-byte pinned. */
static char *sgsr_cs_source(const char *upstream, int pass) {
    static const struct { const char *from, *to; } edits[] = {
        {"#version 320 es", "#version 450\n#define REQUEST_NDC_Y_UP 1\n#define texelFetch(s,p,l) texelFetch(s,clamp(p,ivec2(0),textureSize(s,l)-ivec2(1)),l)"},
        {"layout(binding = 0, rgba16f)", "layout(binding = 10, rgba16f)"},
        {"layout(binding = 1, r32ui)", "layout(binding = 11, r32ui)"},
        {"layout(binding = 1, rgba16f)", "layout(binding = 11, rgba16f)"},
        {"mediump", "highp"},
        {"vec3 Position = vec3(ScreenPos,", "ScreenPos -= vec2(2.0 * params.jitterOffset.x * params.ViewportSizeInverse.x, -2.0 * params.jitterOffset.y * params.ViewportSizeInverse.y);\n        vec3 Position = vec3(ScreenPos,"}
    };
    if (!upstream || (pass != 0 && pass != 1)) return NULL;
    /* Every replacement is bounded by input length plus this fixed per-token margin. */
    size_t capacity = strlen(upstream) * 8 + 1024;
    char *out = malloc(capacity);
    if (!out) return NULL;
    size_t n = 0;
    for (const char *p = upstream; *p;) {
        const char *replacement = NULL;
        size_t consumed = 1;
        for (size_t i = 0; i < sizeof edits / sizeof edits[0]; i++) {
            size_t length = strlen(edits[i].from);
            if (!strncmp(p, edits[i].from, length)) {
                replacement = edits[i].to; consumed = length; break;
            }
        }
        if (!strncmp(p, "void main()\n{", 13)) {
            replacement = pass ? "void main()\n{\n    if (any(greaterThanEqual(gl_GlobalInvocationID.xy, params.displaySize))) return;" :
                "void main()\n{\n    if (any(greaterThanEqual(gl_GlobalInvocationID.xy, params.renderSize))) return;";
            consumed = 13;
        }
        size_t length = replacement ? strlen(replacement) : 1;
        if (n + length + 1 > capacity) { free(out); return NULL; }
        memcpy(out + n, replacement ? replacement : p, length); n += length; p += consumed;
    }
    out[n] = 0;
    return out;
}
#endif
