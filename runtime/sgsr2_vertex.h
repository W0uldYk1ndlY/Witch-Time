#ifndef WT_SGSR2_VERTEX_H
#define WT_SGSR2_VERTEX_H

/* Fullscreen triangle for a positive-height viewport, like the existing HDR pass. */
static const char k_sgsr2_vs[] =
    "#version 450\n"
    "layout(location = 0) out vec2 texCoord;\n"
    "void main() {\n"
    "  texCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(texCoord * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

#endif
