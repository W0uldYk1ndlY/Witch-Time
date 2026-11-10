#!/usr/bin/env python3
"""Policy and regression test for the flash detector default policy and capture guard.
Verifies that the flash detector defaults to OFF on Android and ON on desktop,
respects explicit WT_VK_FLASHLOG=0/1/2 overrides on both platforms, and that
the off-path performs zero diagnostic image creation, buffer allocation, blits, or copies.
"""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FLASH_C = ROOT / "runtime" / "nvn_vk_flash.c"


def get_compiler():
    if "HOST_CC" in os.environ:
        return shlex.split(os.environ["HOST_CC"])
    for candidate in ["clang", "gcc", "cc"]:
        found = shutil.which(candidate)
        if found:
            return [found]
    if os.name == "nt":
        msys_gcc = Path("C:/msys64/ucrt64/bin/gcc.exe")
        if msys_gcc.is_file():
            return [str(msys_gcc)]
    raise RuntimeError("No suitable C compiler found for flash policy test")


HARNESS_SOURCE = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

typedef int64_t LONG64;
typedef uint32_t DWORD;
#define MAX_PATH 260
typedef struct { uint16_t wYear, wMonth, wDay, wHour, wMinute, wSecond; } SYSTEMTIME;
static void GetLocalTime(SYSTEMTIME *st) { memset(st, 0, sizeof *st); }
static DWORD GetModuleFileNameA(void *h, char *p, DWORD n) { (void)h; (void)p; (void)n; return 0; }

typedef void *VkImage;
typedef void *VkBuffer;
typedef void *VkDevice;
typedef void *VkCommandBuffer;
typedef int VkResult;
#define VK_SUCCESS 0
#define VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO 14
#define VK_IMAGE_TYPE_2D 1
#define VK_FORMAT_R8G8B8A8_UNORM 37
#define VK_FORMAT_R8G8B8A8_SRGB 43
#define VK_FORMAT_B8G8R8A8_SRGB 50
#define VK_SAMPLE_COUNT_1_BIT 1
#define VK_IMAGE_TILING_OPTIMAL 0
#define VK_IMAGE_USAGE_TRANSFER_SRC_BIT 1
#define VK_IMAGE_USAGE_TRANSFER_DST_BIT 2
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT 2
#define VK_SHARING_MODE_EXCLUSIVE 0
#define VK_IMAGE_LAYOUT_UNDEFINED 0
#define VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL 7
#define VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL 6
#define VK_IMAGE_ASPECT_COLOR_BIT 1
#define VK_FILTER_LINEAR 1
#define VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT 1

typedef struct {
    int sType; const void *pNext; int flags; int imageType; int format;
    struct { uint32_t width, height, depth; } extent;
    uint32_t mipLevels, arrayLayers, samples, tiling, usage, sharingMode;
    int initialLayout;
} VkImageCreateInfo;

typedef struct { uint64_t size, alignment; uint32_t memoryTypeBits; } VkMemoryRequirements;

typedef struct { void *mem; size_t off; void *map; } VMem;
typedef struct { VkImage img; int w, h, fmt, samples; int layout; } VTex;

typedef struct {
    int aspectMask; uint32_t mipLevel, baseArrayLayer, layerCount;
} VkImageSubresourceLayers;

typedef struct {
    VkImageSubresourceLayers srcSubresource;
    struct { int x, y, z; } srcOffsets[2];
    VkImageSubresourceLayers dstSubresource;
    struct { int x, y, z; } dstOffsets[2];
} VkImageBlit;

typedef struct { float float32[4]; } VkClearColorValue;
typedef struct { int aspectMask; uint32_t baseMipLevel, levelCount, baseArrayLayer, layerCount; } VkImageSubresourceRange;
typedef struct {
    uint64_t bufferOffset; uint32_t bufferRowLength, bufferImageHeight;
    VkImageSubresourceLayers imageSubresource;
    struct { int x, y, z; } imageOffset;
    struct { uint32_t width, height, depth; } imageExtent;
} VkBufferImageCopy;

static int g_alloc_count = 0;
static int g_image_count = 0;
static int g_copy_count = 0;

static struct { VkDevice device; } V = { (VkDevice)(uintptr_t)1 };
static struct {
    uint64_t frame_draws, frame_clears, frame_skipped, uploads, pipelines, modules, images;
    VkCommandBuffer cmd;
} R = { 0, 0, 0, 0, 0, 0, 0, (VkCommandBuffer)(uintptr_t)2 };

#define H_N 1
static LONG64 g_hold_n[1];
static int g_in_fight;
static LONG64 g_gpl_dlink;
static LONG64 g_ovl_presents;

#define VKC(x) do { VkResult r = (x); (void)r; } while(0)

static VkResult vkCreateImage(VkDevice d, const VkImageCreateInfo *ci, const void *a, VkImage *img) {
    (void)d; (void)ci; (void)a;
    g_image_count++;
    *img = (VkImage)(uintptr_t)0x100;
    return VK_SUCCESS;
}

static void vkGetImageMemoryRequirements(VkDevice d, VkImage img, VkMemoryRequirements *rq) {
    (void)d; (void)img;
    rq->size = 1024;
}

static VMem vmem_alloc(VkMemoryRequirements rq, int prop, int flags) {
    (void)rq; (void)prop; (void)flags;
    g_alloc_count++;
    VMem m = { (void *)(uintptr_t)0x200, 0, NULL };
    return m;
}

static void vmem_free(VMem m) { (void)m; }

static VkResult vkBindImageMemory(VkDevice d, VkImage img, void *mem, size_t off) {
    (void)d; (void)img; (void)mem; (void)off;
    return VK_SUCCESS;
}

static uint8_t g_fake_buf_map[8][16 * 9 * 4];

static VkBuffer vbuf_new(size_t sz, int usage, VMem *out_mem) {
    (void)sz; (void)usage;
    g_alloc_count++;
    static int idx = 0;
    out_mem->mem = (void *)(uintptr_t)0x300;
    out_mem->off = 0;
    out_mem->map = g_fake_buf_map[idx % 8];
    idx++;
    return (VkBuffer)(uintptr_t)0x400;
}

static void vkDestroyBuffer(VkDevice d, VkBuffer b, const void *a) { (void)d; (void)b; (void)a; }
static void vkDestroyImage(VkDevice d, VkImage i, const void *a) { (void)d; (void)i; (void)a; }
static void image_barrier(VkImage img, int aspect, int old_layout, int new_layout) { (void)img; (void)aspect; (void)old_layout; (void)new_layout; }
static void vkCmdBlitImage(VkCommandBuffer cmd, VkImage src, int sl, VkImage dst, int dl, uint32_t cnt, const VkImageBlit *b, int f) {
    (void)cmd; (void)src; (void)sl; (void)dst; (void)dl; (void)cnt; (void)b; (void)f;
    g_copy_count++;
}
static void vkCmdClearColorImage(VkCommandBuffer cmd, VkImage img, int l, const VkClearColorValue *v, uint32_t cnt, const VkImageSubresourceRange *r) {
    (void)cmd; (void)img; (void)l; (void)v; (void)cnt; (void)r;
    g_copy_count++;
}
static void vkCmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage img, int l, VkBuffer buf, uint32_t cnt, const VkBufferImageCopy *c) {
    (void)cmd; (void)img; (void)l; (void)buf; (void)cnt; (void)c;
    g_copy_count++;
}

#include "runtime/nvn_vk_flash.c"

int main(void) {
    int on = fl_on();
    printf("FL_ON=%d\n", on);

    /* Test fl_pass */
    fl_pass(1280, 720, 0x19, 1, 1, 0);
    printf("PASSES=%d\n", g_fl.cur.npass);

    /* Test fl_capture */
    VTex vt = { (VkImage)(uintptr_t)0x500, 1280, 720, VK_FORMAT_R8G8B8A8_UNORM, 1, 0 };
    int s4[4] = { 0, 0, 1280, 720 };
    fl_capture(&vt, s4, 1, 0);

    printf("MADE=%d\n", g_fl.made);
    printf("ALLOC_COUNT=%d\n", g_alloc_count);
    printf("IMAGE_COUNT=%d\n", g_image_count);
    printf("COPY_COUNT=%d\n", g_copy_count);

    if (g_fl.made) {
        fl_shutdown();
    }
    return 0;
}
"""


class FlashDetectorPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = get_compiler()
        cls.tmpdir = tempfile.TemporaryDirectory()
        cls.tmppath = Path(cls.tmpdir.name)

        src_path = cls.tmppath / "flash_test.c"
        src_path.write_text(HARNESS_SOURCE, encoding="utf-8")

        # Compile Android binary
        cls.exe_android = cls.tmppath / ("flash_test_android" + (".exe" if os.name == "nt" else ""))
        cmd_android = cls.cc + [
            "-O2",
            "-D__ANDROID__=1",
            f"-I{ROOT}",
            str(src_path),
            "-o",
            str(cls.exe_android),
            "-lm",
        ]
        build_android = subprocess.run(cmd_android, capture_output=True, text=True)
        if build_android.returncode != 0:
            raise RuntimeError(f"Android test build failed:\n{build_android.stderr}\nCmd: {' '.join(cmd_android)}")

        # Compile Desktop binary
        cls.exe_desktop = cls.tmppath / ("flash_test_desktop" + (".exe" if os.name == "nt" else ""))
        cmd_desktop = cls.cc + [
            "-O2",
            f"-I{ROOT}",
            str(src_path),
            "-o",
            str(cls.exe_desktop),
            "-lm",
        ]
        build_desktop = subprocess.run(cmd_desktop, capture_output=True, text=True)
        if build_desktop.returncode != 0:
            raise RuntimeError(f"Desktop test build failed:\n{build_desktop.stderr}\nCmd: {' '.join(cmd_desktop)}")

    @classmethod
    def tearDownClass(cls):
        cls.tmpdir.cleanup()

    def run_binary(self, exe: Path, env_override=None):
        env = os.environ.copy()
        if env_override is not None:
            env["WT_VK_FLASHLOG"] = str(env_override)
        else:
            env.pop("WT_VK_FLASHLOG", None)
        proc = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5, env=env)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        stats = {}
        for line in proc.stdout.strip().splitlines():
            line = line.strip()
            for key in ["FL_ON", "PASSES", "MADE", "ALLOC_COUNT", "IMAGE_COUNT", "COPY_COUNT"]:
                if line.startswith(key + "="):
                    stats[key] = int(line.split("=", 1)[1].strip())
        return stats

    def test_android_default_off(self):
        # On Android with no environment override, flash detector must default to OFF (0)
        # and issue NO diagnostic memory allocations, images, or copy/blit commands
        stats = self.run_binary(self.exe_android, env_override=None)
        self.assertEqual(stats["FL_ON"], 0)
        self.assertEqual(stats["PASSES"], 0)
        self.assertEqual(stats["MADE"], 0)
        self.assertEqual(stats["ALLOC_COUNT"], 0)
        self.assertEqual(stats["IMAGE_COUNT"], 0)
        self.assertEqual(stats["COPY_COUNT"], 0)

    def test_android_override_0(self):
        # WT_VK_FLASHLOG=0 -> OFF
        stats = self.run_binary(self.exe_android, env_override="0")
        self.assertEqual(stats["FL_ON"], 0)
        self.assertEqual(stats["ALLOC_COUNT"], 0)

    def test_android_override_1(self):
        # WT_VK_FLASHLOG=1 -> ON
        stats = self.run_binary(self.exe_android, env_override="1")
        self.assertEqual(stats["FL_ON"], 1)
        self.assertEqual(stats["PASSES"], 1)
        self.assertEqual(stats["MADE"], 1)
        self.assertGreater(stats["ALLOC_COUNT"], 0)
        self.assertEqual(stats["IMAGE_COUNT"], 1)
        self.assertGreater(stats["COPY_COUNT"], 0)

    def test_android_override_2(self):
        # WT_VK_FLASHLOG=2 -> ON (extended jump detection)
        stats = self.run_binary(self.exe_android, env_override="2")
        self.assertEqual(stats["FL_ON"], 2)
        self.assertEqual(stats["MADE"], 1)

    def test_desktop_default_on(self):
        # On desktop with no environment override, flash detector must default to ON (1)
        stats = self.run_binary(self.exe_desktop, env_override=None)
        self.assertEqual(stats["FL_ON"], 1)
        self.assertEqual(stats["PASSES"], 1)
        self.assertEqual(stats["MADE"], 1)
        self.assertGreater(stats["ALLOC_COUNT"], 0)
        self.assertEqual(stats["IMAGE_COUNT"], 1)
        self.assertGreater(stats["COPY_COUNT"], 0)

    def test_desktop_override_0(self):
        # WT_VK_FLASHLOG=0 -> OFF
        stats = self.run_binary(self.exe_desktop, env_override="0")
        self.assertEqual(stats["FL_ON"], 0)
        self.assertEqual(stats["ALLOC_COUNT"], 0)
        self.assertEqual(stats["MADE"], 0)

    def test_desktop_override_1(self):
        # WT_VK_FLASHLOG=1 -> ON
        stats = self.run_binary(self.exe_desktop, env_override="1")
        self.assertEqual(stats["FL_ON"], 1)
        self.assertEqual(stats["MADE"], 1)

    def test_desktop_override_2(self):
        # WT_VK_FLASHLOG=2 -> ON (extended)
        stats = self.run_binary(self.exe_desktop, env_override="2")
        self.assertEqual(stats["FL_ON"], 2)
        self.assertEqual(stats["MADE"], 1)


if __name__ == "__main__":
    unittest.main()
