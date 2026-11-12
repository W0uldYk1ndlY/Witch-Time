"""Host contracts from production reset/retirement code, not GPU acceptance."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def source(name):
    return (ROOT / "runtime" / name).read_text(encoding="utf-8")


def between(text, start, end):
    assert text.count(start) == 1, start
    return text.split(start, 1)[1].split(end, 1)[0]


def function(file, name):
    match = re.search(r"static void " + name + r"\([^\n]*\)[^\n]*\{.*?\n\}", source(file), re.S)
    assert match, name
    return match.group(0)


def compile_run(code, *, must_fail=False):
    temp_root = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "opencode" if os.name == "nt" else None
    with tempfile.TemporaryDirectory(prefix="upscale-reset-", dir=temp_root) as directory:
        path = Path(directory)
        c, exe = path / "test.c", path / ("test.exe" if os.name == "nt" else "test")
        c.write_text(code, encoding="utf-8")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "runtime"), "-I", str(ROOT / "deps/android-arm64/include"),
                        str(c), "-o", str(exe), "-lm"], check=True)
        result = subprocess.run([str(exe)], capture_output=must_fail, text=True)
        if must_fail:
            assert result.returncode != 0, "negative control unexpectedly passed"
            assert "Assertion" in result.stderr or "assertion" in result.stderr, result.stderr
        else:
            result.check_returncode()


code = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "upscale_scene.h"
typedef struct Tex { int unused; } Tex;
typedef struct { uint64_t user; int destroyed,w,h,levels; VkFormat fmt; VkImage img; } VTex;
typedef struct { VTex *vt; } FImg;
typedef long long LONG64;
typedef struct { long long QuadPart; } LARGE_INTEGER;
static LONG64 tk(void) { return 1; }
static void QueryPerformanceFrequency(LARGE_INTEGER *p) { p->QuadPart = 1; }
static long long nvn_front_frame_next(void) { return 0; }
/* PRODUCTION_FSR_RESOURCES */
/* PRODUCTION_SGSR_RESOURCES */
static struct { uint64_t serial, done; VkCommandBuffer cmd; VkBuffer ring; } R;
static struct { VTex *vt; uint64_t serial; } *g_garbage;
static int g_ngarbage, g_garbage_cap;
static void vt_destroy(VTex *v) { assert(!v->destroyed && R.done >= v->user); v->destroyed = 1; }
static struct { FImg img[FI_N]; int w,h,uw,uh,have_prev,t_last,res_index,fresh,blit; float prev_fwd[3],prev_pos[3]; } F;
static struct Backend {
    FImg image[SC_IMAGES]; int w,h,ow,oh,history,have_history,frame_reset;
    Tex *previous_color,*previous_depth; VTex *previous_color_image,*previous_depth_image;
    float previous_projection[16],previous_position[3],previous_forward[3];
} SG, SC;
static UpscaleScene U = UPSCALE_SCENE_INITIALIZER;
static int game_fsr_w,game_fsr_h,variant;
static int gfx_sgsr_variant(void) { return variant; }
static long long upscale_diag_run_frame[2];
static int upscale_diag_pending,upscale_diag_bound,upscale_diag_consumer;
static uint32_t upscale_diag_native_pending_w,upscale_diag_native_pending_h,upscale_diag_native_w,upscale_diag_native_h;
static long long g_diag_surface_gen;
'''
resources = "enum { FI_ACC1," + between(source("nvn_vk_fsr.c"), "enum { FI_ACC1,", "typedef struct { VTex *vt; VkImageView")
code = code.replace("/* PRODUCTION_FSR_RESOURCES */", resources)
resources = []
for file, prefix in [("nvn_vk_sgsr.c", "SG"), ("nvn_vk_sgsr_cs.c", "SC")]:
    match = re.search(r"enum \{ " + prefix + r"_MOTION,.*?\};", source(file))
    assert match, prefix
    resources.append(match.group(0))
code = code.replace("/* PRODUCTION_SGSR_RESOURCES */", "\n".join(resources))
for file, name in [("nvn_vk_draw.c", "garbage_collect"), ("nvn_vk_draw.c", "vt_garbage"),
                   ("nvn_vk_sgsr.c", "sgsr_retire_images"), ("nvn_vk_sgsr_cs.c", "sgsr_cs_retire_images"),
                   ("nvn_vk_upscale.c", "upscale_mode_changed")]:
    code += function(file, name) + "\n"

# Only allocation/command boundaries are mocked; resize and sizing are production.
code += r'''
static VTex allocated[4*FI_N];
static int allocations,uploads;
static int fsr_output_format(VkFormat f) { return f==VK_FORMAT_R16G16B16A16_SFLOAT; }
static void fsr_img(FImg *out,VkFormat f,int w,int h,int levels,int storage) {
    assert(allocations<4*FI_N && storage>=0 && storage<=1);
    VTex *v=&allocated[allocations++]; v->w=w; v->h=h; v->levels=levels; v->fmt=f;
    v->user=R.serial+1; out->vt=v;
}
static uint64_t ring_raw(const void *data,size_t n) { assert(data && n==128*sizeof(int16_t)); return 0; }
static void cmd_begin(void) {}
static void vkCmdCopyBufferToImage(VkCommandBuffer cmd,VkBuffer buffer,VkImage image,VkImageLayout layout,uint32_t n,const VkBufferImageCopy *copy) {
    (void)cmd; (void)buffer; (void)image;
    assert(layout==VK_IMAGE_LAYOUT_GENERAL && n==1 && copy->imageExtent.width==128 && copy->imageExtent.height==1);
    uploads++;
}
'''
code += function("nvn_vk_fsr.c", "fsr_image_size") + "\n"
code += function("nvn_vk_fsr.c", "fsr_resize") + "\n"

# Execute the exact production camera/compatibility decision blocks with mocked images.
for file, prefix in [("nvn_vk_sgsr.c", "SG"), ("nvn_vk_sgsr_cs.c", "SC")]:
    body = between(source(file), "    float position[3] =", "    if (reset)")
    code += f"static int reset_{prefix}(Tex *color, Tex *depth, VTex *vc, VTex *vd, int w,int h,int ow,int oh) {{\nconst float *camera=U.c3;\nfloat position[3] =" + body + "return reset;\n}\n"
body = between(source("nvn_vk_fsr.c"), "    float fwd[3] =", "    F.evidence_reset = reset;")
code += "static int reset_F(void) { const float *m=U.c3;\nfloat fwd[3] =" + body + "return reset;\n}\n"
code += r'''
static void baseline(Tex *c,Tex *d,VTex *vc,VTex *vd) {
    memset(&U,0,sizeof U); U.frame=20; U.ran=19; U.have_surface=1; U.surface_generation=3; g_diag_surface_gen=3;
    U.c3[10]=1; U.c3[30]=-1; U.c3[21]=1;
    SG.w=SC.w=640; SG.h=SC.h=360; SG.ow=SC.ow=1280; SG.oh=SC.oh=720;
    SG.have_history=SC.have_history=F.have_prev=1; F.fresh=0;
    SG.previous_color=SC.previous_color=c; SG.previous_depth=SC.previous_depth=d;
    SG.previous_color_image=SC.previous_color_image=vc; SG.previous_depth_image=SC.previous_depth_image=vd;
    memcpy(SG.previous_projection,U.c3+16,sizeof SG.previous_projection);
    memcpy(SC.previous_projection,U.c3+16,sizeof SC.previous_projection);
    memset(SG.previous_position,0,sizeof SG.previous_position); memset(SC.previous_position,0,sizeof SC.previous_position);
    memset(SG.previous_forward,0,sizeof SG.previous_forward); memset(SC.previous_forward,0,sizeof SC.previous_forward);
    SG.previous_forward[2]=SC.previous_forward[2]=-1;
    memset(F.prev_pos,0,sizeof F.prev_pos); memset(F.prev_fwd,0,sizeof F.prev_fwd); F.prev_fwd[2]=-1;
}
int main(void) {
    Tex c={0},d={0}; VTex vc={0},vd={0},other={0};
    baseline(&c,&d,&vc,&vd);
    /* Projection-only changes currently preserve FSR history; FS/CS reject
       non-jitter projection changes. Record that distinction, not a repair. */
    U.c3[16]=2; assert(!reset_F());
    assert(reset_SG(&c,&d,&vc,&vd,640,360,1280,720));
    assert(reset_SC(&c,&d,&vc,&vd,640,360,1280,720));
    baseline(&c,&d,&vc,&vd); U.c3[18]=0.001f; U.c3[22]=0.001f;
    assert(!reset_F());
    baseline(&c,&d,&vc,&vd); R.serial=10; R.done=9;
    const int sizes[3][4]={{640,360,1280,720},{800,450,1280,720},{800,450,1280,800}};
    for(int generation=0;generation<3;generation++) {
        VTex *old[FI_N]; for(int i=0;i<FI_N;i++) old[i]=F.img[i].vt;
        F.fresh=0; assert(!reset_F());
        const int *s=sizes[generation];
        fsr_resize(s[0],s[1],s[2],s[3],VK_FORMAT_R16G16B16A16_SFLOAT);
        assert(F.w==s[0] && F.h==s[1] && F.uw==s[2] && F.uh==s[3]);
        assert(allocations==(generation+1)*FI_N && uploads==generation+1);
        assert(F.fresh && reset_F()); /* All other reset causes are false. */
        F.fresh=0; assert(!reset_F());
        for(int i=0;i<FI_N;i++) {
            int w,h,levels; fsr_image_size(i,s[0],s[1],s[2],s[3],&w,&h,&levels);
            assert(F.img[i].vt!=old[i] && F.img[i].vt->w==w && F.img[i].vt->h==h && F.img[i].vt->levels==levels);
            if(old[i]) assert(!old[i]->destroyed);
        }
        assert(F.img[FI_OUT].vt->w==s[2] && F.img[FI_OUT].vt->h==s[3]);
        garbage_collect(); assert(g_ngarbage==generation*FI_N);
    }
    /* The last generation is still owned; retire it explicitly for cleanup. */
    for(int i=0;i<FI_N;i++) { vt_garbage(F.img[i].vt); F.img[i].vt=NULL; }
    R.done=R.serial+1; garbage_collect(); assert(!g_ngarbage);
    for(int i=0;i<allocations;i++) assert(allocated[i].destroyed);
    for (int backend=0;backend<2;backend++) {
        int (*reset)(Tex*,Tex*,VTex*,VTex*,int,int,int,int)=backend?reset_SC:reset_SG;
        baseline(&c,&d,&vc,&vd); assert(!reset(&c,&d,&vc,&vd,640,360,1280,720)); assert(!reset_F());
        for (int reason=0;reason<10;reason++) {
            baseline(&c,&d,&vc,&vd);
            int w=640,h=360,ow=1280,oh=720; VTex *image=&vc;
            switch(reason) {
                case 0: U.c3[51]=6; break; /* translation cut */
                case 1: U.c3[10]=-1; break; /* angle cut */
                case 2: w=800; break;
                case 3: oh=800; break; /* output/aspect change */
                case 4: U.c3[16]=2; break; /* projection/aspect change */
                case 5: U.frame++; break; /* skipped scene */
                case 6: g_diag_surface_gen++; break; /* resume/new surface */
                case 7: U.have_surface=0; break;
                case 8: SG.have_history=SC.have_history=F.have_prev=0; break;
                case 9: image=&other; break; /* backing image replaced */
            }
            assert(reset(&c,&d,image,&vd,w,h,ow,oh));
            if (reason==0 || reason==1 || (reason>=5 && reason<=8)) assert(reset_F());
        }
        baseline(&c,&d,&vc,&vd); U.c3[18]=0.001f; U.c3[22]=0.001f;
        assert(!reset(&c,&d,&vc,&vd,640,360,1280,720)); /* jitter-only projection is compatible */
    }
    /* All effective modes, including native/legacy FSR, retire both backend histories. */
    VTex images[FI_N+SG_IMAGES+SC_IMAGES];
    for (int before=0;before<=8;before++) for (int after=0;after<=8;after++) {
        if(before==after) continue;
        memset(images,0,sizeof images); R.serial+=2; R.done=R.serial;
        for(int i=0;i<FI_N+SG_IMAGES+SC_IMAGES;i++) images[i].user=R.serial+1;
        for(int i=0;i<FI_N;i++) F.img[i].vt=&images[i];
        for(int i=0;i<SG_IMAGES;i++) SG.image[i].vt=&images[FI_N+i];
        for(int i=0;i<SC_IMAGES;i++) SC.image[i].vt=&images[FI_N+SG_IMAGES+i];
        F.have_prev=SG.have_history=SC.have_history=U.have_surface=1;
        upscale_mode_changed(before,after); garbage_collect();
        assert(!F.have_prev && !SG.have_history && !SC.have_history && !U.have_surface);
        assert(g_ngarbage==FI_N+SG_IMAGES+SC_IMAGES);
        for(int i=0;i<FI_N+SG_IMAGES+SC_IMAGES;i++) assert(!images[i].destroyed);
        R.done++; garbage_collect(); assert(!g_ngarbage);
        for(int i=0;i<FI_N+SG_IMAGES+SC_IMAGES;i++) assert(images[i].destroyed);
    }
    for(int mode=6;mode<=8;mode++) for(int direction=0;direction<2;direction++) {
        upscale_mode_changed(0,mode);
        memset(images,0,sizeof images); R.serial+=2; R.done=R.serial;
        for(int i=0;i<FI_N+SG_IMAGES+SC_IMAGES;i++) images[i].user=R.serial+1;
        for(int i=0;i<FI_N;i++) F.img[i].vt=&images[i];
        for(int i=0;i<SG_IMAGES;i++) SG.image[i].vt=&images[FI_N+i];
        for(int i=0;i<SC_IMAGES;i++) SC.image[i].vt=&images[FI_N+SG_IMAGES+i];
        F.have_prev=SG.have_history=SC.have_history=U.have_surface=1;
        variant^=1; upscale_mode_changed(mode,mode);
        assert(!F.have_prev && !SG.have_history && !SC.have_history && !U.have_surface);
        garbage_collect(); assert(g_ngarbage==FI_N+SG_IMAGES+SC_IMAGES);
        for(int i=0;i<FI_N+SG_IMAGES+SC_IMAGES;i++) assert(!images[i].destroyed);
        for(int i=0;i<FI_N;i++) assert(!F.img[i].vt);
        for(int i=0;i<SG_IMAGES;i++) assert(!SG.image[i].vt);
        for(int i=0;i<SC_IMAGES;i++) assert(!SC.image[i].vt);
        R.done=R.serial+1; garbage_collect(); assert(!g_ngarbage);
        for(int i=0;i<FI_N+SG_IMAGES+SC_IMAGES;i++) assert(images[i].destroyed);
        F.have_prev=SG.have_history=SC.have_history=1;
        upscale_mode_changed(mode,mode); assert(F.have_prev && SG.have_history && SC.have_history);
    }
    /* Resume/surface changes invalidate history without freeing pending users.
       Subsequent backend image replacement uses the real retirement queue. */
    VTex pending[4][SG_IMAGES+SC_IMAGES]; memset(pending,0,sizeof pending);
    baseline(&c,&d,&vc,&vd); R.done=999;
    for(int generation=0;generation<4;generation++) {
        R.serial=1000+(uint64_t)generation;
        for(int i=0;i<SG_IMAGES+SC_IMAGES;i++) pending[generation][i].user=R.serial+1;
        for(int i=0;i<SG_IMAGES;i++) SG.image[i].vt=&pending[generation][i];
        for(int i=0;i<SC_IMAGES;i++) SC.image[i].vt=&pending[generation][SG_IMAGES+i];
        g_diag_surface_gen++;
        assert(reset_SG(&c,&d,&vc,&vd,640,360,1280,720));
        assert(reset_SC(&c,&d,&vc,&vd,640,360,1280,720));
        for(int i=0;i<SG_IMAGES+SC_IMAGES;i++) assert(!pending[generation][i].destroyed);
        sgsr_retire_images(); sgsr_cs_retire_images(); garbage_collect();
    }
    assert(g_ngarbage==4*(SG_IMAGES+SC_IMAGES));
    for(R.done=1000;R.done<=1004;R.done++) {
        garbage_collect();
        for(int generation=0;generation<4;generation++) for(int i=0;i<SG_IMAGES+SC_IMAGES;i++)
            assert(pending[generation][i].destroyed==(R.done>=1001+(uint64_t)generation));
    }
    assert(!g_ngarbage);
    free(g_garbage); return 0;
}
'''
compile_run(code)
for original, mutant in [
    ("F.fresh = 1;", "F.fresh = 0;"),
    ("\n    sgsr_retire_images();\n", "\n    if (changed) sgsr_retire_images();\n"),
    ("\n    sgsr_cs_retire_images();\n", "\n    if (changed) sgsr_cs_retire_images();\n"),
]:
    assert code.count(original)==1, original
    compile_run(code.replace(original,mutant), must_fail=True)

# Actual progress branch and temporal call ordering, stripped only of unrelated WSI work.
draw = source("nvn_vk_draw.c")
calls = "    if (progress) goto swap;" + between(draw, "    if (progress) goto swap;", "    if (g_tl_t0")
assert calls.count("fsr_present();") == calls.count("sgsr_present();") == calls.count("sgsr_cs_present();") == 1
flush = between(draw, "static void present_flush(", "static void present_progress(")
assert not re.search(r"\b(?:fsr|sgsr|sgsr_cs)_present\s*\(", flush)
compile_run(r'''
#include <assert.h>
#include <stdint.h>
static struct { uint64_t frame; } U = {42};
static int scene,jitter,history,capture;
static int wt_tsc_requested(void) { return 1; }
static void wt_tsc_frame(uint64_t frame, int eligible) {
    assert(frame == U.frame && !eligible);
    capture++;
}
static void fsr_present(void) { scene++; }
static void sgsr_present(void) { jitter++; }
static void sgsr_cs_present(void) { history++; }
static void present(int progress) {
''' + calls + r'''
swap: ;
}
int main(void) {
    for(int i=0;i<100;i++) present(1);
    assert(!scene && !jitter && !history && !capture);
    present(0); assert(scene==1 && jitter==1 && history==1 && capture==1);
    for(int i=0;i<100;i++) present(1);
    assert(scene==1 && jitter==1 && history==1 && capture==1);
}
''')
print("production reset decisions: FS/CS camera, dimensions/aspect, skipped scene, surface generation; FSR camera/gap/surface")
print("production retirement: 72 mode transitions, six variant-only switches, four pending surface generations; mock fences only")
print("production present branch: 200 progress repeats do not call temporal hooks; held flush source contract only")
print("FSR resize: production resource table/sizing/resize, fresh-only reset, dimension/output-aspect allocation; Vulkan allocator/commands mocked")
print("projection: FSR preserves projection-only history; FS/CS reject non-jitter changes; three negative controls fail as expected")
