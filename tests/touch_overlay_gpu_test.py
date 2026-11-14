#!/usr/bin/env python3
"""Headless Vulkan format/resize round trips; not WSI or Android validation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
gpu = (ROOT / "tests/sgsr_gpu_test.c").read_text(encoding="utf-8")
draw = (ROOT / "runtime/nvn_vk_pcmenu.c").read_text(encoding="utf-8")
assert "vkCmdBlitImage(R.cmd, tgt" in draw and "vkCmdBlitImage(R.cmd, g_touch_gpu.canvas" in draw
shape = draw.split("static const unsigned char *touch_glyph", 1)[1].split("static int touch_vk_prepare", 1)[0]
shape = "static const unsigned char *touch_glyph" + shape
shape = shape.replace("    for (int y = 0;", """    assert(n == 0 || (left >= 0 && left + (n * 6 - 1) * scale <= TOUCH_TEX_SIZE));
    assert(7 * scale <= TOUCH_TEX_SIZE);
    for (int y = 0;""", 1)
prefix = gpu.split("int main(", 1)[0].replace(
    "X(QueueSubmit)", "X(CmdBlitImage) X(CmdCopyImage) X(ResetCommandPool) X(QueueSubmit)")
setup = "#ifdef _WIN32" + gpu.split("#ifdef _WIN32", 2)[2].split("    enum { IMAGES = 6 };", 1)[0]
code = prefix + r'''
#include <assert.h>
static Image image_format(uint32_t w, uint32_t h, VkFormat fmt) {
    Image image = {0};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt;
    ci.extent = (VkExtent3D){w,h,1}; ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    CHECK(vkCreateImage(device, &ci, NULL, &image.image));
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(device,image.image,&rq);
    image.memory=allocate(rq,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); CHECK(vkBindImageMemory(device,image.image,image.memory,0));
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image.image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
    vi.format=(fmt==VK_FORMAT_R8G8B8A8_SRGB || fmt==VK_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    CHECK(vkCreateImageView(device,&vi,NULL,&image.view)); return image;
}
static void image_free(Image image) { vkDestroyImageView(device,image.view,NULL); vkDestroyImage(device,image.image,NULL); vkFreeMemory(device,image.memory,NULL); }
int main(void) {
''' + setup + r'''
    const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
    for (int f=0; f<4; f++) for (int indirect=0; indirect<2; indirect++) for (int dark=0; dark<2; dark++) {
        uint32_t w=17+(uint32_t)f, h=13+(uint32_t)indirect; size_t bytes=(size_t)w*h*4;
        VkFormat canvas_format=f>=2 ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
        Image target=image_format(w,h,formats[f]), canvas=image_format(w,h,canvas_format), final=image_format(w,h,formats[f]);
        Buffer before=buffer_new(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT), after=buffer_new(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&begin));
        barrier(cmd,target.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue color={{dark ? .002f : .2f,dark ? .008f : .4f,dark ? .018f : .8f,1}}; VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
        barrier(cmd,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bc={0}; bc.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; bc.imageExtent=(VkExtent3D){w,h,1};
        vkCmdCopyImageToBuffer(cmd,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,before.buffer,1,&bc);
        barrier(cmd,canvas.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit={0}; blit.srcSubresource=bc.imageSubresource; blit.dstSubresource=bc.imageSubresource;
        blit.srcOffsets[1]=(VkOffset3D){(int32_t)w,(int32_t)h,1}; blit.dstOffsets[1]=blit.srcOffsets[1];
        vkCmdBlitImage(cmd,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,canvas.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
        barrier(cmd,canvas.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(cmd,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(cmd,canvas.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
        barrier(cmd,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (indirect) {
            barrier(cmd,final.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy copy={0}; copy.srcSubresource=bc.imageSubresource; copy.dstSubresource=bc.imageSubresource; copy.extent=(VkExtent3D){w,h,1};
            vkCmdCopyImage(cmd,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,final.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            barrier(cmd,final.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        vkCmdCopyImageToBuffer(cmd,indirect ? final.image : target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,after.buffer,1,&bc);
        VkMemoryBarrier host={VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
        CHECK(vkEndCommandBuffer(cmd)); VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
        CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        unsigned char *a=before.mapped,*b=after.mapped;
        assert(a[0]!=a[2]); for(size_t i=0;i<bytes;i++) if(abs((int)a[i]-(int)b[i])>2) { fprintf(stderr,"format %d indirect %d byte %zu: %d != %d\n",f,indirect,i,a[i],b[i]); return 1; }
        vkUnmapMemory(device,before.memory); vkDestroyBuffer(device,before.buffer,NULL); vkFreeMemory(device,before.memory,NULL);
        vkUnmapMemory(device,after.memory); vkDestroyBuffer(device,after.buffer,NULL); vkFreeMemory(device,after.memory,NULL);
        image_free(target); image_free(canvas); image_free(final); CHECK(vkResetCommandPool(device,pool,0));
    }
    vkDestroyCommandPool(device,pool,NULL); vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL);
    puts("touch overlay: BGRA/RGBA UNORM/sRGB direct and intermediate blits passed (no WSI or validation layer)");
    return 0;
}
'''
cc = os.environ.get("HOST_CC") or shutil.which("gcc") or shutil.which("cc")
if not cc:
    raise RuntimeError("C compiler required")
with tempfile.TemporaryDirectory(prefix="touch-vulkan-") as temp:
    temp = Path(temp)
    shape_source = temp / "touch_shape_test.c"
    shape_source.write_text("#include <assert.h>\n#include <stdint.h>\n#include <string.h>\n#include <math.h>\n#include \"touch_controls.h\"\n#define TOUCH_TEX_SIZE 96\n" + shape + r'''
int main(void) {
    uint8_t pixels[96 * 96 * 4];
    for (int id = 0; id < TOUCH_COUNT + 10; id++) touch_shape(pixels, id);
    touch_shape(pixels, TOUCH_COUNT);
    const uint8_t *edge = pixels + (48 * 96 + 90) * 4;
    assert(edge[0] == 152 && edge[1] == 175 && edge[2] == 184 && edge[3] == 205);
    return 0;
}
''', encoding="utf-8")
    shape_exe = temp / ("touch_shape_test.exe" if os.name == "nt" else "touch_shape_test")
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "runtime"), str(shape_source), "-lm", "-o", str(shape_exe)], check=True)
    subprocess.run([str(shape_exe)], check=True)
    source = temp / "touch_gpu.c"
    source.write_text(code, encoding="utf-8")
    exe = temp / ("touch_gpu.exe" if os.name == "nt" else "touch_gpu")
    cmd = [cc, "-std=gnu11", "-O2", "-I", str(ROOT / "runtime"), "-I", str(ROOT / "deps/android-arm64/include"), str(source), "-lm"]
    if os.name != "nt": cmd += ["-ldl"]
    subprocess.run(cmd + ["-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
