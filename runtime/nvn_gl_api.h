/* The NVN front end still shares a translation unit with the Windows GL
 * backend. Vulkan-only builds need its enum vocabulary, never a GL driver.
 * These declarations do not implement OpenGL: an accidental GL call is fatal.
 * Keep the ordinary desktop build on the system GL header. */
#ifndef WT_NVN_GL_API_H
#define WT_NVN_GL_API_H
#if defined(__ANDROID__) && !defined(WT_VULKAN_ONLY)
#define WT_VULKAN_ONLY 1
#endif
#ifndef WT_VULKAN_ONLY
#include <GL/gl.h>
#else
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef APIENTRY
#define APIENTRY
#endif
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float GLfloat;
typedef double GLdouble;

/* OpenGL 1.1 enum values referenced by the shared front end. */
enum {
    GL_FALSE = 0, GL_TRUE = 1, GL_NONE = 0, GL_ZERO = 0, GL_ONE = 1,
    GL_POINTS = 0, GL_LINES = 1, GL_LINE_LOOP = 2, GL_LINE_STRIP = 3,
    GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5, GL_TRIANGLE_FAN = 6,
    GL_QUADS = 7, GL_QUAD_STRIP = 8, GL_POLYGON = 9,
    GL_NEVER = 0x0200, GL_LESS = 0x0201, GL_ALWAYS = 0x0207,
    GL_FRONT = 0x0404, GL_BACK = 0x0405, GL_FRONT_AND_BACK = 0x0408,
    GL_CW = 0x0900, GL_CCW = 0x0901,
    GL_POINT = 0x1B00, GL_COLOR = 0x1800,
    GL_CLEAR = 0x1500, GL_INVERT = 0x150A,
    GL_KEEP = 0x1E00, GL_REPLACE = 0x1E01, GL_INCR = 0x1E02, GL_DECR = 0x1E03,
    GL_BYTE = 0x1400, GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405,
    GL_RED = 0x1903, GL_GREEN = 0x1904, GL_BLUE = 0x1905, GL_ALPHA = 0x1906, GL_RGB = 0x1907, GL_RGBA = 0x1908,
    GL_DEPTH_BUFFER_BIT = 0x0100, GL_STENCIL_BUFFER_BIT = 0x0400, GL_COLOR_BUFFER_BIT = 0x4000,
    GL_CULL_FACE = 0x0B44, GL_DEPTH_TEST = 0x0B71, GL_DEPTH_FUNC = 0x0B74, GL_STENCIL_TEST = 0x0B90,
    GL_ALPHA_TEST = 0x0BC0, GL_ALPHA_TEST_FUNC = 0x0BC1, GL_ALPHA_TEST_REF = 0x0BC2,
    GL_BLEND = 0x0BE2, GL_COLOR_LOGIC_OP = 0x0BF2, GL_SCISSOR_TEST = 0x0C11,
    GL_VIEWPORT = 0x0BA2, GL_SCISSOR_BOX = 0x0C10, GL_COLOR_WRITEMASK = 0x0C23,
    GL_POLYGON_OFFSET_POINT = 0x2A01, GL_POLYGON_OFFSET_LINE = 0x2A02, GL_POLYGON_OFFSET_FILL = 0x8037,
    GL_TEXTURE_1D = 0x0DE0, GL_TEXTURE_2D = 0x0DE1,
    GL_TEXTURE_WIDTH = 0x1000, GL_TEXTURE_HEIGHT = 0x1001, GL_TEXTURE_BORDER_COLOR = 0x1004,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_NEAREST_MIPMAP_NEAREST = 0x2700,
    GL_LINEAR_MIPMAP_NEAREST = 0x2701, GL_NEAREST_MIPMAP_LINEAR = 0x2702, GL_LINEAR_MIPMAP_LINEAR = 0x2703,
    GL_CLAMP = 0x2900, GL_REPEAT = 0x2901, GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02
};

static inline _Noreturn void nvn_gl_unavailable(const char *name) {
    fprintf(stderr, "nvn: %s reached the unavailable desktop OpenGL backend in a Vulkan-only build\n", name);
    fflush(stderr);
    exit(5);
}
/* This is deliberately not a no-op fallback: every legacy core entry point
 * traps. Extended entry points remain private pointers in nvn_gl.c. */
#define glAlphaFunc(...) nvn_gl_unavailable("glAlphaFunc")
#define glBindTexture(...) nvn_gl_unavailable("glBindTexture")
#define glClear(...) nvn_gl_unavailable("glClear")
#define glClearColor(...) nvn_gl_unavailable("glClearColor")
#define glClearDepth(...) nvn_gl_unavailable("glClearDepth")
#define glClearStencil(...) nvn_gl_unavailable("glClearStencil")
#define glCullFace(...) nvn_gl_unavailable("glCullFace")
#define glDeleteTextures(...) nvn_gl_unavailable("glDeleteTextures")
#define glDepthFunc(...) nvn_gl_unavailable("glDepthFunc")
#define glDepthMask(...) nvn_gl_unavailable("glDepthMask")
#define glDisable(...) nvn_gl_unavailable("glDisable")
#define glDrawArrays(...) nvn_gl_unavailable("glDrawArrays")
#define glEnable(...) nvn_gl_unavailable("glEnable")
#define glFinish(...) nvn_gl_unavailable("glFinish")
#define glFlush(...) nvn_gl_unavailable("glFlush")
#define glFrontFace(...) nvn_gl_unavailable("glFrontFace")
#define glGenTextures(...) nvn_gl_unavailable("glGenTextures")
#define glGetBooleanv(...) nvn_gl_unavailable("glGetBooleanv")
#define glGetFloatv(...) nvn_gl_unavailable("glGetFloatv")
#define glGetIntegerv(...) nvn_gl_unavailable("glGetIntegerv")
#define glGetTexImage(...) nvn_gl_unavailable("glGetTexImage")
#define glGetTexLevelParameteriv(...) nvn_gl_unavailable("glGetTexLevelParameteriv")
#define glLineWidth(...) nvn_gl_unavailable("glLineWidth")
#define glLogicOp(...) nvn_gl_unavailable("glLogicOp")
#define glPixelStorei(...) nvn_gl_unavailable("glPixelStorei")
#define glPointSize(...) nvn_gl_unavailable("glPointSize")
#define glPolygonMode(...) nvn_gl_unavailable("glPolygonMode")
#define glReadPixels(...) nvn_gl_unavailable("glReadPixels")
#define glTexParameteri(...) nvn_gl_unavailable("glTexParameteri")
#define glTexParameteriv(...) nvn_gl_unavailable("glTexParameteriv")
#define glTexSubImage1D(...) nvn_gl_unavailable("glTexSubImage1D")
#define glTexSubImage2D(...) nvn_gl_unavailable("glTexSubImage2D")
#define glViewport(...) nvn_gl_unavailable("glViewport")
#define glStencilFunc(...) nvn_gl_unavailable("glStencilFunc")
#define glScissor(...) nvn_gl_unavailable("glScissor")
#define glGetError(...) (nvn_gl_unavailable("glGetError"), (GLenum)0)
#define glIsEnabled(...) (nvn_gl_unavailable("glIsEnabled"), (GLboolean)0)
#define glGetString(...) (nvn_gl_unavailable("glGetString"), (const GLubyte *)0)
#endif
#endif
