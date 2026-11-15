#!/usr/bin/env python3
"""Byte-level native texture upload regression against real layout/deswizzle.

Runs the production front-end reader with both scratch and mapped destinations,
and its backend upload callback with recorded Vulkan copy operands. Synthetic
data only: no game dump or GPU is required.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FRONT = (ROOT / "runtime/nvn_gl.c").read_text()
DRAW = (ROOT / "runtime/nvn_vk_draw.c").read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def build_run(source, sources=()):
    with tempfile.TemporaryDirectory(prefix="vk-texture-upload-") as td:
        path = Path(td)
        (path / "test.c").write_text(source)
        args = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-D_GNU_SOURCE", "-D__declspec(x)=", "-O2",
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-Wno-unused-variable", "-Wno-missing-field-initializers",
            "-Wno-unused-parameter", "-Wno-misleading-indentation", "-I", str(ROOT / "runtime/posix"),
            "-I", str(ROOT / "runtime"), str(path / "test.c"),
        ]
        subprocess.run(args + [str(ROOT / p) for p in sources] + [
            "-lm", "-o", str(path / "test")], check=True)
        return subprocess.run([str(path / "test")], text=True, capture_output=True)


PRELUDE = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_front.h"
#include "tex_decode.h"
#include "tex_pack.h"
#undef fopen
#define P(a) ((void *)(uintptr_t)(a))
typedef struct TexPrep { struct TexPrep *next; Tex *t; const uint8_t *src; size_t size, bytes; uint8_t *snap, *out; volatile LONG owner; int ok, bc7; } TexPrep;
static struct { LONG64 tex_bytes; } g_st;
static LONG64 g_pack_redo, g_pack_hit, g_pack_lowq, g_tp_diff;
static double g_pack_worst = 99;
static TexPrep *tp_take(Tex *t, const uint8_t *src) { (void)t; (void)src; return NULL; }
static void tp_free(TexPrep *p) { assert(!p); }
static const TexPackEntry *pack_find(const Tex *t, const uint8_t *src) { (void)t; (void)src; return NULL; }
static unsigned decoded;
static void decode_par(int nf, const void *src, int w, int h, uint8_t *out, uint8_t *bc) {
    decoded++; assert(!bc); assert(tex_decode_rgba8(nf, src, w, h, out) == 0);
}
static int fmt_get(int nf, Fmt *f) {
    const TexFormat *fmt = tex_format(nf);
    if (!fmt) return 0;
    memset(f, 0, sizeof *f); f->bpb = fmt->bpb; f->bw = fmt->bw; f->bh = fmt->bh;
    f->kind = fmt->compressed ? 4 : fmt->kind; return 1;
}
'''


class VulkanTextureUploadTest(unittest.TestCase):
    def test_direct_native_upload_preserves_every_mip_layer_and_byte(self):
        signature = "int nvn_front_tex_read_into("
        new_reader = signature in FRONT
        code = function(FRONT, "static void tex_lay(") + "\n" + function(FRONT, "int nvn_front_tex_levels(")
        if not new_reader:
            code += "\ntypedef void *nvn_front_reserve_fn(Tex *, int, int, size_t);\n"
        code += "\n" + function(FRONT, signature if new_reader else "int nvn_front_tex_read(")
        if not new_reader:
            code += r'''
int nvn_front_tex_read_into(Tex *t, int decode, nvn_front_put_fn *put, nvn_front_reserve_fn *reserve) {
    (void)reserve; return nvn_front_tex_read(t, decode, put);
}
'''
        else:
            code += "\n" + function(FRONT, "int nvn_front_tex_read(")
        result = build_run(PRELUDE + code + r'''
static uint8_t expected[16][16][65536], mapped[65536 + 32];
static size_t sizes[16][16];
static int widths[16][16], heights[16][16], depths[16][16];
static unsigned puts_count, reserved, direct_count;
static int reference, reject_reservation;
static size_t active_size;
static void *reserve_destination(Tex *t, int level, int layer, size_t size) {
    (void)t; (void)level; (void)layer;
    reserved++; assert(size <= 65536);
    if (reject_reservation) return NULL;
    memset(mapped, 0xa5, sizeof mapped); active_size = size; return mapped + 16;
}
static void collect(Tex *t, int level, int layer, int w, int h, int d,
        const void *data, size_t size, int rgba8, int64_t stride) {
    (void)t; (void)rgba8; (void)stride; puts_count++;
    assert(level < 16 && layer < 16 && size <= 65536);
    if (reference) {
        sizes[layer][level] = size; memcpy(expected[layer][level], data, size);
        widths[layer][level] = w; heights[layer][level] = h; depths[layer][level] = d;
    } else {
        assert(size == sizes[layer][level]);
        assert(w == widths[layer][level] && h == heights[layer][level] && d == depths[layer][level]);
        assert(!memcmp(expected[layer][level], data, size));
        if (data == mapped + 16) {
            direct_count++; assert(size == active_size);
            for (size_t i = 0; i < 16; i++) assert(mapped[i] == 0xa5 && mapped[16 + size + i] == 0xa5);
        }
    }
}
static void texture_case(int format, int target, int w, int h, int d, int levels, int packaged) {
    Tex t = {0}; TexLayout layout;
    t.b.format = format; t.b.target = target; t.b.width = w; t.b.height = h;
    t.b.depth = d; t.b.levels = levels;
    tex_lay(&t.b, &layout); t.data_size = layout.data_size;
    t.mem = malloc(layout.size); assert(t.mem); t.limit = t.mem + layout.size;
    for (size_t i = 0; i < layout.size; i++) t.mem[i] = (uint8_t)(i * 37 + i / 43);
    if (packaged) t.b.packaged = (uint64_t)(uintptr_t)t.mem;
    reference = 1; puts_count = 0;
    assert(nvn_front_tex_read(&t, 0, collect));
    unsigned count = puts_count; assert(count == (unsigned)(layout.layers * layout.levels));
    reference = 0; reserved = direct_count = puts_count = 0;
    assert(nvn_front_tex_read_into(&t, 0, collect, reserve_destination));
    assert(puts_count == count && reserved == count && direct_count == count);
    assert(decoded == 0); /* Native ASTC must never enter a CPU decoder. */
    reject_reservation = 1; reserved = direct_count = puts_count = 0;
    assert(nvn_front_tex_read_into(&t, 0, collect, reserve_destination));
    assert(puts_count == count && reserved == count && direct_count == 0);
    reject_reservation = 0;
    if (!packaged) {
        t.limit = t.mem; reserved = puts_count = 0;
        assert(!nvn_front_tex_read_into(&t, 0, collect, reserve_destination));
        assert(!reserved && !puts_count); /* No reservation for unreadable input. */
    }
    free(t.mem);
}
int main(void) {
    for (int f = 0x79; f <= 0x94; f++) {
        texture_case(f, TEX_TARGET_2D_ARRAY, 35, 27, 3, 6, 0);
        texture_case(f, TEX_TARGET_CUBEMAP, 13, 13, 1, 4, 1);
    }
    texture_case(TEX_FMT_RGBA8, TEX_TARGET_3D, 19, 15, 7, 5, 0);
    texture_case(TEX_FMT_RGBA8, TEX_TARGET_2D, 127, 73, 1, 7, 0);
    texture_case(TEX_FMT_BPTC_UNORM, TEX_TARGET_2D_ARRAY, 33, 29, 2, 5, 0);
    /* Strided linear textures already have no deswizzle scratch copy. */
    Tex t = {0}; uint8_t rows[96]; memset(rows, 0x71, sizeof rows);
    t.b.flags = TEX_FLAG_LINEAR; t.b.format = TEX_FMT_RGBA8;
    t.b.width = 5; t.b.height = 3; t.b.stride = 32; t.mem = rows;
    reference = 1; assert(nvn_front_tex_read(&t, 0, collect));
    reference = 0; reserved = direct_count = 0;
    assert(nvn_front_tex_read_into(&t, 0, collect, reserve_destination));
    assert(!reserved && !direct_count);
    /* Software decode keeps its original destination and exact output. */
    uint8_t astc[512] = {0}; t = (Tex){0}; t.b.format = TEX_FMT_ASTC_4x4;
    t.b.target = TEX_TARGET_2D; t.b.width = t.b.height = 4; t.b.levels = 1;
    t.mem = astc; t.limit = astc + sizeof astc;
    reference = 1; assert(nvn_front_tex_read(&t, 1, collect));
    reference = 0; reserved = 0;
    assert(nvn_front_tex_read_into(&t, 1, collect, reserve_destination));
    assert(!reserved && decoded == 2);
    puts("native ASTC/all footprints, mips, arrays, cubes, 3D, linear and decode fallback passed");
    return 0;
}
''', ("runtime/tex_decode.c", "runtime/tex_pack.c"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_backend_consumes_reserved_bytes_once_and_survives_ring_wrap(self):
        code = function(DRAW, "static uint64_t ring_raw(")
        code += "\n" + DRAW[DRAW.index("static VTex *g_put_vt;"):DRAW.index("/* The image of a texture:")]
        result = build_run(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "nvn_front.h"
typedef struct {
    Tex *t; int expand, decode, levels, layers, is3d, rs, bpb;
    VkImage img;
} VTex;
static struct {
    uint64_t ring_pos, ring_size; uint32_t gen; LONG64 ring_wraps, upload_bytes;
    struct { void *map; } ring_mem; VkBuffer ring; VkCommandBuffer cmd;
} R;
static unsigned waits, wrapped, copies, begins, uploads, scaled;
static VkBufferImageCopy observed;
static uint8_t memory[2048];
static void vkd_fail(const char *fmt, ...) { (void)fmt; abort(); }
static void cmd_wait_all(void) { waits++; }
static void cb_ring_wrap(void) { wrapped++; }
static void pass_end(void) {}
static void cmd_begin(void) { begins++; }
static void vt_layout(VTex *vt, VkImageLayout layout) { (void)vt; assert(layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); }
static void rs_upload(VTex *vt, int level, int layer, uint64_t off, uint32_t rowlen,
        uint32_t rows, int x, int y, int w, int h) { (void)vt; scaled++; }
static void vkCmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer b, VkImage image,
        VkImageLayout layout, uint32_t count, const VkBufferImageCopy *regions) {
    (void)cmd; assert(b == R.ring && image);
    assert(layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && count == 1);
    observed = regions[0]; uploads++;
}
static void *counted_memcpy(void *out, const void *in, size_t n) {
    copies++; return memcpy(out, in, n);
}
#define memcpy counted_memcpy
#define RING_ALIGN 256
''' + code + r'''
int main(void) {
    Tex t = {0}; VTex vt = {.t=&t, .levels=6, .layers=3, .bpb=16, .img=(VkImage)(uintptr_t)2};
    R.ring_mem.map = memory; R.ring_size = sizeof memory; R.ring_pos = sizeof memory - 8;
    R.ring = (VkBuffer)(uintptr_t)1; g_put_vt = &vt;
    void *p = vt_reserve(&t, 2, 1, 64);
    assert(p == memory && waits == 1 && wrapped == 1 && R.gen == 1);
    for (unsigned i = 0; i < 64; i++) ((uint8_t *)p)[i] = (uint8_t)(i + 7);
    vt_put(&t, 2, 1, 8, 8, 1, p, 64, 0, 0);
    assert(copies == 0 && R.ring_pos == 64 && uploads == 1);
    assert(g_put_direct_ptr == NULL && g_put_direct_bytes == 64 && g_put_direct_levels == 1);
    assert(observed.bufferOffset == 0 && observed.bufferRowLength == 0);
    assert(observed.imageSubresource.mipLevel == 2 && observed.imageSubresource.baseArrayLayer == 1);
    assert(observed.imageSubresource.layerCount == 1 && observed.imageExtent.depth == 1);
    assert(observed.imageExtent.width == 8 && observed.imageExtent.height == 8);
    for (unsigned i = 0; i < 64; i++) assert(memory[i] == (uint8_t)(i + 7));
    uint8_t data[64]; memset(data, 0x42, sizeof data);
    vt_put(&t, 0, 0, 8, 8, 1, data, sizeof data, 0, 0);
    assert(copies == 1 && uploads == 2 && observed.bufferOffset == 256);
    assert(!memcmp(memory + 256, data, sizeof data));
    vt.is3d = 1; p = vt_reserve(&t, 1, 0, 64); assert(p);
    vt_put(&t, 1, 0, 4, 4, 4, p, 64, 0, 0);
    assert(copies == 1 && observed.imageExtent.depth == 4 && observed.imageSubresource.baseArrayLayer == 0);
    vt.is3d = 0; vt.rs = 1; p = vt_reserve(&t, 0, 0, 64); assert(p);
    vt_put(&t, 0, 0, 4, 4, 1, p, 64, 0, 0);
    assert(scaled == 1 && copies == 1 && uploads == 3);
    vt.rs = 0; vt.expand = 1; assert(!vt_reserve(&t, 0, 0, 64));
    vt.expand = 0; vt.decode = 1; assert(!vt_reserve(&t, 0, 0, 64));
    vt.decode = 0; assert(!vt_reserve(&t, 6, 0, 64));
    assert(!vt_reserve(&t, 0, 3, 64)); assert(!vt_reserve(&t, 0, 0, 0));
    assert(!vt_reserve(&t, 0, 0, 513));
    assert(R.upload_bytes == 256 && begins == 4);
    puts("one reservation, no redundant memcpy, correct mip/layer/depth, ring wrap and conversion fallbacks passed");
    return 0;
}
''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
