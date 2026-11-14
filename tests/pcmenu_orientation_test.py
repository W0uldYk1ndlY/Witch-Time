#!/usr/bin/env python3
"""Pure menu orientation oracles and renderer compile/structure checks; no GPU work."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
panel = (root / "runtime/nvn_vk_pcmenu.c").read_text(encoding="utf-8")
menu = (root / "runtime/pcmenu.c").read_text(encoding="utf-8")
assert "touch_presentation_map(" not in menu
assert "panel_w = p.sdl_width; panel_h = p.sdl_height;" in panel
assert "p.generation != g_gpu.placement.generation" in panel
assert "ba.blendEnable = VK_FALSE" in panel
assert "si.magFilter = si.minFilter = VK_FILTER_NEAREST;" in panel
assert "pcm_descriptor(g_gpu.canvas_set, g_gpu.canvas_view, g_gpu.nearest);" in panel
assert "pcm_descriptor(g_gpu.scratch.set, g_gpu.scratch.view, g_gpu.nearest);" in panel
assert "pcm_descriptor(t->set, t->view, g_gpu.smp);" in panel
assert "VkDescriptorImageInfo ii = {sampler, view," in panel
assert "vkDestroySampler(V.device, g_gpu.nearest, NULL)" in panel
assert "g_gpu.canvas_set, &final" in panel and "g_gpu.scratch.set, &pc" in panel
assert "VK_IMAGE_USAGE_SAMPLED_BIT, &g_gpu.canvas" in panel
images = panel.split("static int pcm_menu_images(", 1)[1].split("static void pcm_menu_sample(", 1)[0]
assert "pcm_gpu_pipeline" not in images
assert "format, &g_gpu.cpu.img, &g_gpu.cpu.mem, NULL" in images
assert "VK_FILTER_NEAREST" in panel.split("if (!g_gpu.cpu.ready)", 1)[1]
assert "v_uv = uv;" in panel and "q = (q * pc.view - pc.region.xy) / pc.region.zw" in panel

pure = r'''
#include "touch_controls.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
static void bounds_oracle(TouchPresentation p, const int r[4]) {
    int result[4], expected[4], lo[2], hi[2], den[2];
    int x=r[0], y=r[1], w=r[2], h=r[3], sw=p.sdl_width, sh=p.sdl_height;
    switch(p.transform) {
    case 1: lo[0]=x; hi[0]=x+w; den[0]=sw; lo[1]=y; hi[1]=y+h; den[1]=sh; break;
    case 2: lo[0]=sh-y-h; hi[0]=sh-y; den[0]=sh; lo[1]=x; hi[1]=x+w; den[1]=sw; break;
    case 4: lo[0]=sw-x-w; hi[0]=sw-x; den[0]=sw; lo[1]=sh-y-h; hi[1]=sh-y; den[1]=sh; break;
    default: lo[0]=y; hi[0]=y+h; den[0]=sh; lo[1]=sw-x-w; hi[1]=sw-x; den[1]=sw; break;
    }
    for(int i=0;i<2;i++) {
        int size=i ? p.height : p.width;
        expected[i]=(int)((int64_t)lo[i]*size/den[i]);
        expected[i+2]=(int)(((int64_t)hi[i]*size+den[i]-1)/den[i])-expected[i];
    }
    assert(touch_presentation_bounds(&p,r,result));
    assert(!memcmp(result,expected,sizeof result));
}
int main(void) {
    const uint32_t src[6]={0x11223344,0x55667788,0x99aabbcc,0xddeeff01,0x23456789,0xabcdef02};
    const int flags[4]={1,2,4,8};
    const int order[4][6]={{0,1,2,3,4,5},{3,0,4,1,5,2},{5,4,3,2,1,0},{2,5,1,4,0,3}};
    uint32_t dst[6]; int ow,oh,out[4];
    for(int f=0;f<4;f++) {
        assert(touch_bitmap_rotate(src,sizeof src,dst,sizeof dst,3,2,flags[f],&ow,&oh));
        assert(ow==(f==1||f==3 ? 2:3) && oh==(f==1||f==3 ? 3:2));
        for(int i=0;i<6;i++) assert(dst[i]==src[order[f][i]]);
        const int sizes[4][2]={{8,16},{16,8},{13,23},{2248,2480}};
        for(int s=0;s<4;s++) {
            TouchPresentation p={flags[f],sizes[s][0],sizes[s][1],8,16,1,0};
            bounds_oracle(p,(int[]){0,0,8,16});
            bounds_oracle(p,(int[]){1,2,4,8});
            bounds_oracle(p,(int[]){7,15,1,1});
            assert(!touch_presentation_bounds(&p,(int[]){-1,0,8,16},out));
            assert(!touch_presentation_bounds(&p,(int[]){1,0,8,16},out));
            assert(!touch_presentation_bounds(&p,(int[]){0,0,0,16},out));
            assert(!touch_presentation_bounds(&p,(int[]){0,16,1,1},out));
            p.transform=16; assert(!touch_presentation_bounds(&p,(int[]){0,0,8,16},out));
        }
        for(int swapped=0;swapped<2;swapped++) {
            TouchPresentation p={flags[f],swapped?2480:2248,swapped?2248:2480,2248,2480,1,0};
            bounds_oracle(p,(int[]){0,0,2248,2480});
            bounds_oracle(p,(int[]){562,620,1124,1240});
        }
    }
    assert(!touch_bitmap_rotate(src,sizeof src,dst,sizeof dst,3,2,16,&ow,&oh));
    assert(!touch_bitmap_rotate(src,sizeof src,dst,sizeof dst,0,2,1,&ow,&oh));
    assert(!touch_bitmap_rotate(src,sizeof src,dst,sizeof dst-1,3,2,1,&ow,&oh));
    assert(!touch_bitmap_rotate(dst,sizeof dst,dst,sizeof dst,3,2,1,&ow,&oh));
    assert(!touch_bitmap_rotate(dst,sizeof dst,(char*)dst+4,sizeof dst,3,2,1,&ow,&oh));
    return 0;
}
'''

# Compile the actual included renderer against Vulkan declarations, without linking or calling Vulkan.
prelude = r'''
#define __ANDROID__ 1
#include <vulkan/vulkan.h>
#include "touch_controls.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
typedef struct { VkDeviceMemory mem; VkDeviceSize off; void *map; } VMem;
static struct { VkDevice device; VkPhysicalDevice gpu; VkExtent2D extent; VkSurfaceFormatKHR format; int alive,hdr; VkSwapchainKHR swapchain; } V;
static struct { VkCommandBuffer cmd; VkPipeline pipe_bound; int dyn_dirty,dyn3_dirty; } R;
static int g_spv_unused, g_sc_src, g_nsmp_old, g_nsmp;
static long long g_vk_recreated;
static long g_bmp_pending;
static VkSampler g_smp_old[256]; static struct { VkSampler s; } g_smp[256];
typedef struct { void *win; } PresentJob;
static PresentJob g_pcm_last;
typedef struct { char path[300]; uint8_t *px; int w,h,bgr; } BmpJob;
static struct { int aniso; float texture_lod_bias; } options;
#define gfx_options() (&options)
#define VKC(call) ((void)(call))
extern VMem vmem_alloc(VkMemoryRequirements r, VkFlags flags, int host);
extern void vmem_free(VMem mem);
extern VkBuffer vbuf_new(uint64_t size, VkFlags flags, VMem *mem);
extern VkShaderModule module_from_glsl(int stage, const char *source, const char *name, int *unused);
extern void cmd_wait_all(void), xfer_wait(void), dsh_forget(void), present_mode_pick(void), swapchain_destroy(void), rs_apply(void);
extern void image_barrier(VkImage image, VkFlags aspect, VkImageLayout old, VkImageLayout next);
extern const char *vk_result_name(VkResult result);
extern void bmp_write_thread(BmpJob *job), vk_present2(void *job,int progress);
static void pcm_upload(int wait);
extern void InterlockedIncrement(long *n), InterlockedIncrement64(long long *n);
'''

def function(signature):
    start = panel.index(signature)
    return panel[start:panel.index("\n}", start) + 2]

globals_start = panel.index("int pcmenu_is_open(void)")
globals_end = panel.index("/* ---- the GPU path ---- */")
fallback_mocks = r'''
#include "diag.h"
#include <assert.h>
static int pipelines, uploads, blits, features=1;
static const uint32_t colors[6]={0x11223344,0x55667788,0x99aabbcc,0xddeeff01,0x23456789,0xabcdef02};
bool diag_is_enabled(void) { return false; }
static VkPipeline pcm_gpu_pipeline(void) { pipelines++; return NULL; }
void pcmenu_gpu(int on) { assert(!on); }
int pcmenu_panel_size(int w,int h,int r[4],unsigned *gen) {
    assert(!g_gpu.on && w==8 && h==16); int local[4]={1,2,3,2}; memcpy(r,local,sizeof local); *gen=1; return 1;
}
void pcmenu_panel_read(void *dst,int w,int h) { assert(w==3 && h==2); memcpy(dst,colors,sizeof colors); }
int pcmenu_kind(void) { return 1; }
int pcmenu_layer_read(int k,void *dst,int w,int h) { (void)k;(void)dst;(void)w;(void)h; assert(0); return 0; }
int pcmenu_settled(void) { return 0; }
static void pcm_gpu_art_stage(void) { assert(0); }
static void pcm_gpu_canvas(int w,int h) { (void)w;(void)h; assert(0); }
static void pcm_gpu_draw(VkImage tgt,const int r[4]) { (void)tgt;(void)r; assert(0); }
void cmd_wait_all(void) { }
void xfer_wait(void) { }
void vmem_free(VMem mem) { free(mem.map); }
VkBuffer vbuf_new(uint64_t size,VkFlags flags,VMem *mem) {
    assert(flags==VK_BUFFER_USAGE_TRANSFER_SRC_BIT); mem->map=malloc((size_t)size); assert(mem->map); return (VkBuffer)(uintptr_t)1;
}
void image_barrier(VkImage image,VkFlags aspect,VkImageLayout old,VkImageLayout next) {
    (void)image;(void)aspect;(void)old;(void)next;
}
static void pcm_descriptor(VkDescriptorSet set,VkImageView view,VkSampler sampler) { (void)set;(void)view;(void)sampler; assert(0); }
static void pcm_image_new_format(int w,int h,VkFlags usage,VkFormat format,VkImage *img,VMem *mem,VkImageView *view) {
    assert(w*h==6 && view==NULL); assert(format==VK_FORMAT_B8G8R8A8_SRGB);
    assert(usage==(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    *img=(VkImage)(uintptr_t)2; *mem=(VMem){0};
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice gpu,VkFormat format,VkFormatProperties *p) {
    (void)gpu;(void)format; memset(p,0,sizeof *p);
    /* CPU fallback must not require shader/sampled/color-attachment format features. */
    if(features) p->optimalTilingFeatures=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d,VkImageView v,const VkAllocationCallbacks *a) { (void)d;(void)v;(void)a; }
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice d,VkImage v,const VkAllocationCallbacks *a) { (void)d;(void)v;(void)a; }
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice d,VkBuffer v,const VkAllocationCallbacks *a) { (void)d;(void)v;(void)a; }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer cmd,VkBuffer buf,VkImage dst,VkImageLayout layout,uint32_t n,const VkBufferImageCopy *c) {
    (void)cmd;(void)buf; assert(dst==g_gpu.cpu.img && layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && n==1);
    assert(c->imageOffset.x==0 && c->imageOffset.y==0 && c->bufferOffset==0);
    uint32_t expected[6]; int w,h;
    assert(touch_bitmap_rotate(colors,sizeof colors,expected,sizeof expected,3,2,g_gpu.placement.transform,&w,&h));
    assert(c->imageExtent.width==(uint32_t)w && c->imageExtent.height==(uint32_t)h);
    assert(!memcmp(g_pcm.mem.map,expected,sizeof expected)); uploads++;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer cmd,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t n,const VkImageBlit *b,VkFilter filter) {
    (void)cmd; assert(src==g_gpu.cpu.img && dst==(VkImage)(uintptr_t)3);
    assert(sl==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && dl==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && n==1 && filter==VK_FILTER_NEAREST);
    int expected[4]; assert(touch_presentation_bounds(&g_gpu.placement,g_pcm.r,expected));
    assert(b->dstOffsets[0].x==expected[0] && b->dstOffsets[0].y==expected[1]);
    assert(b->dstOffsets[1].x==expected[0]+expected[2] && b->dstOffsets[1].y==expected[1]+expected[3]); blits++;
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cmd,VkImage image,VkImageLayout layout,VkBuffer buf,uint32_t n,const VkBufferImageCopy *c) {
    (void)cmd;(void)image;(void)layout;(void)buf;(void)n;(void)c; assert(0);
}
'''
fallback_main = r'''
int main(void) {
    V.extent=(VkExtent2D){8,16}; V.format.format=VK_FORMAT_R8G8B8A8_SRGB;
    touch_runtime_init(NULL); touch_runtime_geometry((TouchRect){0,0,8,16},.01f,8,16);
    touch_runtime_presentation((TouchPresentation){8,8,16,8,16,1,0});
    pcm_upload(1); assert(pipelines==1 && !g_gpu.on && g_pcm.ready);
    pcm_vk_draw((VkImage)(uintptr_t)3); pcm_vk_draw((VkImage)(uintptr_t)3);
    assert(uploads==1 && blits==2);
    touch_runtime_presentation((TouchPresentation){4,8,16,8,16,1,0});
    pcm_vk_draw((VkImage)(uintptr_t)3); assert(blits==2); /* epoch invalidates placement before reupload */
    pcm_upload(1); pcm_vk_draw((VkImage)(uintptr_t)3); assert(uploads==2 && blits==3 && pipelines==1);
    g_gpu.available=g_gpu.on=1; g_sc_src=0; /* Even an available pipeline needs the complete CPU panel without target TRANSFER_SRC. */
    pcm_upload(1); pcm_vk_draw((VkImage)(uintptr_t)3); assert(!g_gpu.on && uploads==3 && blits==4);
    features=0; touch_runtime_presentation((TouchPresentation){2,8,16,8,16,1,0});
    pcm_upload(1); pcm_vk_draw((VkImage)(uintptr_t)3); assert(!g_pcm.ready && blits==4);
    free(g_pcm.mem.map); pcm_tex_free(&g_gpu.cpu);
    return 0;
}
'''
cc = os.environ.get("HOST_CC") or shutil.which("gcc") or shutil.which("clang")
if not cc:
    raise RuntimeError("C compiler required")
with tempfile.TemporaryDirectory(prefix="pcmenu-orientation-") as temp:
    temp = Path(temp)
    src = temp / "pure.c"
    src.write_text(pure, encoding="utf-8")
    exe = temp / ("pure.exe" if os.name == "nt" else "pure")
    subprocess.run([cc,"-std=gnu11","-Wall","-Wextra","-Werror","-I",str(root/"runtime"),
                    str(src),str(root/"runtime/touch_controls.c"),"-lm","-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    for android in (False, True):
        declarations = prelude if android else prelude.replace("#define __ANDROID__ 1", "")
        src.write_text(declarations + panel + '\n_Static_assert(offsetof(PcmPush,region)==80,"GLSL vec4 alignment");\n',encoding="utf-8")
        subprocess.run([cc,"-std=gnu11","-Werror=implicit-function-declaration","-fsyntax-only",
                        "-I",str(root/"runtime"),"-I",str(root/"deps/android-arm64/include"),str(src)],check=True)
    src.write_text(prelude + panel[globals_start:globals_end] + fallback_mocks +
                   function("static void pcm_tex_free(") + function("static int pcm_menu_images(") +
                   function("static void pcm_upload(") + function("static void pcm_vk_draw(") + fallback_main,
                   encoding="utf-8")
    subprocess.run([cc,"-std=gnu11","-Werror=implicit-function-declaration","-D__ANDROID__",
                    "-I",str(root/"runtime"),"-I",str(root/"deps/android-arm64/include"),
                    str(src),str(root/"runtime/touch_controls.c"),"-lm","-o",str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print("menu rotations, edge bounds/scaling, renderer syntax and CPU pipeline-failure fallback passed (host mocks; no GPU)")
