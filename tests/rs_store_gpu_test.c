/* Narrow GPU model: actual replay decisions, modeled draw preparation/targets/query state.
 * Reuse unchanged headless FS utilities, not its main or shaders. No production exec/ring test. */
#define main unused_sgsr_main
#include "sgsr_gpu_test.c"
#undef main
#include <assert.h>
typedef long long LONG64;
typedef struct { int texture, view; } TargetRef;
static struct { int in_pass, pass_rs; float pass_sx, pass_sy; TargetRef col[8], depth; int ncol, have_targets, dyn_dirty; } R;
static int g_dp_fs_stores, g_rs_on = 1, g_oq_on = 1, marker_ends;
#define RS_ON g_rs_on
static TargetRef g_sf_col[8], g_sf_dep;
static int g_sf_ncol, g_sf_have, g_sf_oq, g_sf_on = 1;
static LONG64 g_sf_draws;
static VkCommandBuffer active_cmd;
static void pass_end(void) { if (R.in_pass) vkCmdEndRendering(active_cmd); R.in_pass = 0; }
static void pt_end(void) { marker_ends++; }
#include "rs_helpers.h"

int main(int argc, char **argv) {
    if (argc != 8) return 2;
    int w=atoi(argv[3]), h=atoi(argv[4]), indexed=atoi(argv[5]), buffers=atoi(argv[6]), skip=atoi(argv[7]);
    if (w<3 || h<3 || w>4096 || h>4096) return 2;
#ifdef _WIN32
    HMODULE library = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr get = library ? (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(library,"vkGetInstanceProcAddr") : NULL;
#else
    void *library=dlopen("libvulkan.so.1",RTLD_NOW);
    PFN_vkGetInstanceProcAddr get=library ? (PFN_vkGetInstanceProcAddr)dlsym(library,"vkGetInstanceProcAddr") : NULL;
#endif
    if (!get) return 2;
    vkCreateInstance=(PFN_vkCreateInstance)get(NULL,"vkCreateInstance");
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    VkInstance instance; CHECK(vkCreateInstance(&ici,NULL,&instance));
#define LOAD_RS(name) if (!vk##name) vk##name=(PFN_vk##name)get(instance,"vk" #name); if (!vk##name) { fprintf(stderr,"missing vk%s\n",#name); return 2; }
    FUNCTIONS(LOAD_RS)
    PFN_vkCmdDrawIndexed draw_indexed=(PFN_vkCmdDrawIndexed)get(instance,"vkCmdDrawIndexed");
    PFN_vkCmdBindIndexBuffer bind_index=(PFN_vkCmdBindIndexBuffer)get(instance,"vkCmdBindIndexBuffer");
    PFN_vkCmdPushConstants push=(PFN_vkCmdPushConstants)get(instance,"vkCmdPushConstants");
    if (!draw_indexed || !bind_index || !push) return 2;
    uint32_t count=0; CHECK(vkEnumeratePhysicalDevices(instance,&count,NULL)); if (!count) return 2;
    VkPhysicalDevice *gpus=calloc(count,sizeof *gpus); CHECK(vkEnumeratePhysicalDevices(instance,&count,gpus)); VkPhysicalDevice gpu=gpus[0]; free(gpus);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(gpu,&props); printf("GPU: %s\n",props.deviceName);
    vkGetPhysicalDeviceMemoryProperties(gpu,&memory);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,NULL);
    VkQueueFamilyProperties *families=calloc(count,sizeof *families); vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,families);
    uint32_t family; for(family=0;family<count;family++) if(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT) break;
    free(families); if(family==count) return 2;
    float priority=1; VkDeviceQueueCreateInfo qci={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex=family; qci.queueCount=1; qci.pQueuePriorities=&priority;
    VkPhysicalDeviceVulkan13Features features={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; features.dynamicRendering=1;
    VkPhysicalDeviceFeatures base={0}; base.fragmentStoresAndAtomics=1;
    VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext=&features; dci.pEnabledFeatures=&base; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
    CHECK(vkCreateDevice(gpu,&dci,NULL,&device)); VkQueue queue; vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo pci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex=family;
    VkCommandPool pool; CHECK(vkCreateCommandPool(device,&pci,NULL,&pool));
    VkCommandBufferAllocateInfo cai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool=pool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device,&cai,&cmd)); active_cmd=cmd;
    Image target={0}; VkImageCreateInfo ii={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType=VK_IMAGE_TYPE_2D; ii.format=VK_FORMAT_R32_UINT; ii.extent=(VkExtent3D){w,h,1}; ii.mipLevels=ii.arrayLayers=1; ii.samples=1; ii.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CHECK(vkCreateImage(device,&ii,NULL,&target.image)); VkMemoryRequirements req; vkGetImageMemoryRequirements(device,target.image,&req); target.memory=allocate(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); CHECK(vkBindImageMemory(device,target.image,target.memory,0));
    VkImageViewCreateInfo iv={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; iv.image=target.image; iv.viewType=VK_IMAGE_VIEW_TYPE_2D; iv.format=ii.format; iv.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; CHECK(vkCreateImageView(device,&iv,NULL,&target.view));
    Buffer counts=buffer_new((VkDeviceSize)w*h*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT), readback=buffer_new((VkDeviceSize)w*h*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT), indices=buffer_new(12,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    memset(counts.mapped,0,(size_t)w*h*4); uint32_t triangle[]={0,1,2}; memcpy(indices.mapped,triangle,12);
    VkDescriptorSetLayoutBinding bindings[2]={{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo li={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount=2; li.pBindings=bindings; VkDescriptorSetLayout layout; CHECK(vkCreateDescriptorSetLayout(device,&li,NULL,&layout));
    VkPushConstantRange range={VK_SHADER_STAGE_FRAGMENT_BIT,0,8}; VkPipelineLayoutCreateInfo pli={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount=1; pli.pSetLayouts=&layout; pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&range; VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(device,&pli,NULL,&pl));
    VkShaderModule modules[]={shader(argv[1]),shader(argv[2])};
    VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    for(int i=0;i<2;i++){stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT; stages[i].module=modules[i]; stages[i].pName="main";}
    VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode=VK_POLYGON_MODE_FILL; rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=1;
    VkPipelineColorBlendStateCreateInfo blend={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo dyn={VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dyn.dynamicStateCount=2; dyn.pDynamicStates=states;
    VkPipelineRenderingCreateInfo render={VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; VkGraphicsPipelineCreateInfo ci={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; ci.pNext=&render; ci.stageCount=2; ci.pStages=stages; ci.pVertexInputState=&vi; ci.pInputAssemblyState=&ia; ci.pViewportState=&vp; ci.pRasterizationState=&rs; ci.pMultisampleState=&ms; ci.pColorBlendState=&blend; ci.pDynamicState=&dyn; ci.layout=pl;
    VkPipeline pipeline; CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&ci,NULL,&pipeline));
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}; VkDescriptorPoolCreateInfo dpi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets=1; dpi.poolSizeCount=2; dpi.pPoolSizes=sizes; VkDescriptorPool descriptors; CHECK(vkCreateDescriptorPool(device,&dpi,NULL,&descriptors));
    VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=descriptors; ai.descriptorSetCount=1; ai.pSetLayouts=&layout; VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device,&ai,&set));
    VkDescriptorImageInfo di={VK_NULL_HANDLE,target.view,VK_IMAGE_LAYOUT_GENERAL}; VkDescriptorBufferInfo db={counts.buffer,0,VK_WHOLE_SIZE}; VkWriteDescriptorSet writes[2]={{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for(int i=0;i<2;i++){writes[i].dstSet=set; writes[i].dstBinding=i; writes[i].descriptorCount=1; writes[i].descriptorType=bindings[i].descriptorType;} writes[0].pImageInfo=&di; writes[1].pBufferInfo=&db; vkUpdateDescriptorSets(device,2,writes,0,NULL);
    VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&begin)); barrier(cmd,target.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
    VkClearColorValue sentinel={.uint32={0xdeadbeef,0,0,0}}; vkCmdClearColorImage(cmd,target.image,VK_IMAGE_LAYOUT_GENERAL,&sentinel,1,&iv.subresourceRange); barrier(cmd,target.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    R.col[0]=(TargetRef){11,12}; R.depth=(TargetRef){13,14}; R.ncol=R.have_targets=R.pass_rs=1; R.pass_sx=R.pass_sy=0.5f;
    typedef struct {int images,ssbo_w;} VStage;
    VStage stage={1,buffers}, *fs=&stage, *sv[]={fs}; int sx[]={1}, nsv=1;
#define resolve_image(s,v,k) ((void)(s), (void)(v), (void)(k), 1)
#include "rs_eligibility.h"
#undef resolve_image
    int replay=0;
    for(int pass=0;pass<2;pass++) {
        if(pass){ replay=rs_store_low(); if(!replay) break; assert(!g_oq_on && !R.ncol && !R.have_targets && !R.col[0].texture && !R.depth.texture); if(skip){rs_store_done(0); break;} }
        if(pass) barrier(cmd,target.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
        int rw=pass?w:w/2, rh=pass?h:h/2;
        VkRenderingInfo rendering={VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea.extent=(VkExtent2D){rw,rh}; rendering.layerCount=1; vkCmdBeginRendering(cmd,&rendering); R.in_pass=1;
        VkViewport viewport={0,0,(float)rw,(float)rh,0,1}; VkRect2D scissor={{0,0},{rw,rh}}; vkCmdSetViewport(cmd,0,1,&viewport); vkCmdSetScissor(cmd,0,1,&scissor); vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline); vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&set,0,NULL);
        uint32_t params[]={w,buffers}; push(cmd,pl,VK_SHADER_STAGE_FRAGMENT_BIT,0,8,params);
        if(indexed){bind_index(cmd,indices.buffer,0,VK_INDEX_TYPE_UINT32); draw_indexed(cmd,3,1,0,0,0);} else vkCmdDraw(cmd,3,1,0,0);
        if(pass) rs_store_done(1);
    }
    pass_end(); assert(g_oq_on==1 && R.col[0].texture==11 && R.col[0].view==12 && R.depth.texture==13 && R.depth.view==14 && R.ncol==1 && R.have_targets);
    assert(g_sf_draws==(!buffers&&!skip) && marker_ends==g_sf_draws); if(replay) assert(R.dyn_dirty);
    barrier(cmd,target.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); VkBufferImageCopy copy={0}; copy.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent=(VkExtent3D){w,h,1}; vkCmdCopyImageToBuffer(cmd,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&copy);
    VkMemoryBarrier host={VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT; vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(cmd)); VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd; CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    int bad=0, rows=0, cols=0; uint32_t *pixels=readback.mapped,*hits=counts.mapped;
    for(int y=0;y<h;y++){int untouched=1; for(int x=0;x<w;x++){int low=x<w/2&&y<h/2; uint32_t expected=buffers&&!low?0xdeadbeef:123; if(pixels[y*w+x]!=expected || hits[y*w+x]!=(uint32_t)(buffers&&low)) bad++; if(pixels[y*w+x]!=0xdeadbeef) untouched=0;} rows+=untouched;}
    for(int x=0;x<w;x++){int untouched=1; for(int y=0;y<h;y++) if(pixels[y*w+x]!=0xdeadbeef) untouched=0; cols+=untouched;}
    printf("%dx%d %s buffers=%d replay=%d untouched rows=%d cols=%d readback: %s\n",w,h,indexed?"indexed":"direct",buffers,replay,rows,cols,bad?"FAIL":"PASS");
    vkDestroyPipeline(device,pipeline,NULL); vkDestroyPipelineLayout(device,pl,NULL);
    vkDestroyDescriptorPool(device,descriptors,NULL); vkDestroyDescriptorSetLayout(device,layout,NULL);
    for(int i=0;i<2;i++) vkDestroyShaderModule(device,modules[i],NULL);
    Buffer allocated[]={counts,readback,indices};
    for(int i=0;i<3;i++){vkUnmapMemory(device,allocated[i].memory); vkDestroyBuffer(device,allocated[i].buffer,NULL); vkFreeMemory(device,allocated[i].memory,NULL);}
    vkDestroyImageView(device,target.view,NULL); vkDestroyImage(device,target.image,NULL); vkFreeMemory(device,target.memory,NULL);
    vkDestroyCommandPool(device,pool,NULL);
    vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL); return bad?1:0;
}
