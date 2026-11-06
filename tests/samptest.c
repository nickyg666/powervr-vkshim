/*
 * Does sampling from a LINEAR image work?  Also tests descriptor offsets.
 *
 * Renders a fullscreen triangle sampling a 64x64 unique pattern; runs it
 * twice: once from an OPTIMAL image (control), once from a LINEAR image.
 * The rendered output is read back and compared to the pattern.
 *
 *   DISABLE_VK_LAYER_POWERVR_COMPAT=1 ./samptest
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static VkCommandPool pool;

static uint32_t pick_mem(uint32_t bits, uint32_t flags)
{
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof(mp));
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags))
            return i;
    return 0;
}

static const uint32_t vert_spv[] = {
    0x07230203,
    0x00010000,
    0x0008000b,
    0x0000002e,
    0x00000000,
    0x00020011,
    0x00000001,
    0x0006000b,
    0x00000001,
    0x4c534c47,
    0x6474732e,
    0x3035342e,
    0x00000000,
    0x0003000e,
    0x00000000,
    0x00000001,
    0x0008000f,
    0x00000000,
    0x00000004,
    0x6e69616d,
    0x00000000,
    0x0000000c,
    0x00000018,
    0x00000020,
    0x00030003,
    0x00000002,
    0x000001c2,
    0x00040005,
    0x00000004,
    0x6e69616d,
    0x00000000,
    0x00030005,
    0x00000009,
    0x00000070,
    0x00060005,
    0x0000000c,
    0x565f6c67,
    0x65747265,
    0x646e4978,
    0x00007865,
    0x00030005,
    0x00000018,
    0x00007675,
    0x00060005,
    0x0000001e,
    0x505f6c67,
    0x65567265,
    0x78657472,
    0x00000000,
    0x00060006,
    0x0000001e,
    0x00000000,
    0x505f6c67,
    0x7469736f,
    0x006e6f69,
    0x00070006,
    0x0000001e,
    0x00000001,
    0x505f6c67,
    0x746e696f,
    0x657a6953,
    0x00000000,
    0x00070006,
    0x0000001e,
    0x00000002,
    0x435f6c67,
    0x4470696c,
    0x61747369,
    0x0065636e,
    0x00070006,
    0x0000001e,
    0x00000003,
    0x435f6c67,
    0x446c6c75,
    0x61747369,
    0x0065636e,
    0x00030005,
    0x00000020,
    0x00000000,
    0x00040047,
    0x0000000c,
    0x0000000b,
    0x0000002a,
    0x00040047,
    0x00000018,
    0x0000001e,
    0x00000000,
    0x00030047,
    0x0000001e,
    0x00000002,
    0x00050048,
    0x0000001e,
    0x00000000,
    0x0000000b,
    0x00000000,
    0x00050048,
    0x0000001e,
    0x00000001,
    0x0000000b,
    0x00000001,
    0x00050048,
    0x0000001e,
    0x00000002,
    0x0000000b,
    0x00000003,
    0x00050048,
    0x0000001e,
    0x00000003,
    0x0000000b,
    0x00000004,
    0x00020013,
    0x00000002,
    0x00030021,
    0x00000003,
    0x00000002,
    0x00030016,
    0x00000006,
    0x00000020,
    0x00040017,
    0x00000007,
    0x00000006,
    0x00000002,
    0x00040020,
    0x00000008,
    0x00000007,
    0x00000007,
    0x00040015,
    0x0000000a,
    0x00000020,
    0x00000001,
    0x00040020,
    0x0000000b,
    0x00000001,
    0x0000000a,
    0x0004003b,
    0x0000000b,
    0x0000000c,
    0x00000001,
    0x0004002b,
    0x0000000a,
    0x0000000e,
    0x00000001,
    0x0004002b,
    0x0000000a,
    0x00000010,
    0x00000002,
    0x00040020,
    0x00000017,
    0x00000003,
    0x00000007,
    0x0004003b,
    0x00000017,
    0x00000018,
    0x00000003,
    0x00040017,
    0x0000001a,
    0x00000006,
    0x00000004,
    0x00040015,
    0x0000001b,
    0x00000020,
    0x00000000,
    0x0004002b,
    0x0000001b,
    0x0000001c,
    0x00000001,
    0x0004001c,
    0x0000001d,
    0x00000006,
    0x0000001c,
    0x0006001e,
    0x0000001e,
    0x0000001a,
    0x00000006,
    0x0000001d,
    0x0000001d,
    0x00040020,
    0x0000001f,
    0x00000003,
    0x0000001e,
    0x0004003b,
    0x0000001f,
    0x00000020,
    0x00000003,
    0x0004002b,
    0x0000000a,
    0x00000021,
    0x00000000,
    0x0004002b,
    0x00000006,
    0x00000023,
    0x40000000,
    0x0004002b,
    0x00000006,
    0x00000025,
    0x3f800000,
    0x0004002b,
    0x00000006,
    0x00000028,
    0x00000000,
    0x00040020,
    0x0000002c,
    0x00000003,
    0x0000001a,
    0x00050036,
    0x00000002,
    0x00000004,
    0x00000000,
    0x00000003,
    0x000200f8,
    0x00000005,
    0x0004003b,
    0x00000008,
    0x00000009,
    0x00000007,
    0x0004003d,
    0x0000000a,
    0x0000000d,
    0x0000000c,
    0x000500c4,
    0x0000000a,
    0x0000000f,
    0x0000000d,
    0x0000000e,
    0x000500c7,
    0x0000000a,
    0x00000011,
    0x0000000f,
    0x00000010,
    0x0004006f,
    0x00000006,
    0x00000012,
    0x00000011,
    0x0004003d,
    0x0000000a,
    0x00000013,
    0x0000000c,
    0x000500c7,
    0x0000000a,
    0x00000014,
    0x00000013,
    0x00000010,
    0x0004006f,
    0x00000006,
    0x00000015,
    0x00000014,
    0x00050050,
    0x00000007,
    0x00000016,
    0x00000012,
    0x00000015,
    0x0003003e,
    0x00000009,
    0x00000016,
    0x0004003d,
    0x00000007,
    0x00000019,
    0x00000009,
    0x0003003e,
    0x00000018,
    0x00000019,
    0x0004003d,
    0x00000007,
    0x00000022,
    0x00000009,
    0x0005008e,
    0x00000007,
    0x00000024,
    0x00000022,
    0x00000023,
    0x00050050,
    0x00000007,
    0x00000026,
    0x00000025,
    0x00000025,
    0x00050083,
    0x00000007,
    0x00000027,
    0x00000024,
    0x00000026,
    0x00050051,
    0x00000006,
    0x00000029,
    0x00000027,
    0x00000000,
    0x00050051,
    0x00000006,
    0x0000002a,
    0x00000027,
    0x00000001,
    0x00070050,
    0x0000001a,
    0x0000002b,
    0x00000029,
    0x0000002a,
    0x00000028,
    0x00000025,
    0x00050041,
    0x0000002c,
    0x0000002d,
    0x00000020,
    0x00000021,
    0x0003003e,
    0x0000002d,
    0x0000002b,
    0x000100fd,
    0x00010038,
};
static const uint32_t frag_spv[] = {
    0x07230203,
    0x00010000,
    0x0008000b,
    0x00000014,
    0x00000000,
    0x00020011,
    0x00000001,
    0x0006000b,
    0x00000001,
    0x4c534c47,
    0x6474732e,
    0x3035342e,
    0x00000000,
    0x0003000e,
    0x00000000,
    0x00000001,
    0x0007000f,
    0x00000004,
    0x00000004,
    0x6e69616d,
    0x00000000,
    0x00000009,
    0x00000011,
    0x00030010,
    0x00000004,
    0x00000007,
    0x00030003,
    0x00000002,
    0x000001c2,
    0x00040005,
    0x00000004,
    0x6e69616d,
    0x00000000,
    0x00040005,
    0x00000009,
    0x6f6c6f63,
    0x00000072,
    0x00030005,
    0x0000000d,
    0x00786574,
    0x00030005,
    0x00000011,
    0x00007675,
    0x00040047,
    0x00000009,
    0x0000001e,
    0x00000000,
    0x00040047,
    0x0000000d,
    0x00000021,
    0x00000000,
    0x00040047,
    0x0000000d,
    0x00000022,
    0x00000000,
    0x00040047,
    0x00000011,
    0x0000001e,
    0x00000000,
    0x00020013,
    0x00000002,
    0x00030021,
    0x00000003,
    0x00000002,
    0x00030016,
    0x00000006,
    0x00000020,
    0x00040017,
    0x00000007,
    0x00000006,
    0x00000004,
    0x00040020,
    0x00000008,
    0x00000003,
    0x00000007,
    0x0004003b,
    0x00000008,
    0x00000009,
    0x00000003,
    0x00090019,
    0x0000000a,
    0x00000006,
    0x00000001,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000001,
    0x00000000,
    0x0003001b,
    0x0000000b,
    0x0000000a,
    0x00040020,
    0x0000000c,
    0x00000000,
    0x0000000b,
    0x0004003b,
    0x0000000c,
    0x0000000d,
    0x00000000,
    0x00040017,
    0x0000000f,
    0x00000006,
    0x00000002,
    0x00040020,
    0x00000010,
    0x00000001,
    0x0000000f,
    0x0004003b,
    0x00000010,
    0x00000011,
    0x00000001,
    0x00050036,
    0x00000002,
    0x00000004,
    0x00000000,
    0x00000003,
    0x000200f8,
    0x00000005,
    0x0004003d,
    0x0000000b,
    0x0000000e,
    0x0000000d,
    0x0004003d,
    0x0000000f,
    0x00000012,
    0x00000011,
    0x00050057,
    0x00000007,
    0x00000013,
    0x0000000e,
    0x00000012,
    0x0003003e,
    0x00000009,
    0x00000013,
    0x000100fd,
    0x00010038,
};
int main(void)
{
    VkApplicationInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS)
        return printf("no instance\n"), 1;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &n, pds);
    pd = pds[0];
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS)
        return printf("no device\n"), 1;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    vkCreateCommandPool(dev, &pci, NULL, &pool);

    /* pattern buffer */
    VkBufferCreateInfo bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = W * H * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer pbuf;
    vkCreateBuffer(dev, &bci, NULL, &pbuf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, pbuf, &mr);
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = pick_mem(mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory pmem;
    vkAllocateMemory(dev, &mai, NULL, &pmem);
    vkBindBufferMemory(dev, pbuf, pmem, 0);
    void *pp;
    vkMapMemory(dev, pmem, 0, VK_WHOLE_SIZE, 0, &pp);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = pp + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)x;
            p[1] = (uint8_t)y;
            p[2] = (uint8_t)(x + y);
            p[3] = 255;
        }
    vkUnmapMemory(dev, pmem);

    /* render targets + readback buffer */
    VkBufferCreateInfo rbci = bci;
    rbci.size = W * H * 4;
    rbci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer rbuf;
    vkCreateBuffer(dev, &rbci, NULL, &rbuf);
    VkMemoryRequirements rmr;
    vkGetBufferMemoryRequirements(dev, rbuf, &rmr);
    VkMemoryAllocateInfo rbmai = mai;
    rbmai.allocationSize = rmr.size;
    VkDeviceMemory rmem;
    vkAllocateMemory(dev, &rbmai, NULL, &rmem);
    vkBindBufferMemory(dev, rbuf, rmem, 0);
    void *rptr;
    vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, &rptr);

    /* classic render pass for the render target */
    VkRenderPass rpass;
    {
        VkAttachmentDescription ad = {0};
        ad.format = VK_FORMAT_R8G8B8A8_UNORM;
        ad.samples = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ar = {0};
        ar.attachment = 0;
        ar.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription sd = {0};
        sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sd.colorAttachmentCount = 1;
        sd.pColorAttachments = &ar;
        VkRenderPassCreateInfo rci = {0};
        rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rci.attachmentCount = 1;
        rci.pAttachments = &ad;
        rci.subpassCount = 1;
        rci.pSubpasses = &sd;
        vkCreateRenderPass(dev, &rci, NULL, &rpass);
    }
    VkFramebuffer rfb;

    /* separate render target (always OPTIMAL) */
    VkImage rimg;
    VkDeviceMemory rmemimg[2];
    {
        VkImageCreateInfo ii = {0};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent.width = W;
        ii.extent.height = H;
        ii.extent.depth = 1;
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vkCreateImage(dev, &ii, NULL, &rimg);
    }
    {
        VkMemoryRequirements imr;
        vkGetImageMemoryRequirements(dev, rimg, &imr);
        VkMemoryAllocateInfo imai = mai;
        imai.allocationSize = imr.size;
        imai.memoryTypeIndex = pick_mem(imr.memoryTypeBits, 0);
        vkAllocateMemory(dev, &imai, NULL, &rmemimg[0]);
        vkBindImageMemory(dev, rimg, rmemimg[0], 0);
    }
    VkImageViewCreateInfo rivci = {0};
    rivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    rivci.image = rimg;
    rivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    rivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    rivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rivci.subresourceRange.levelCount = 1;
    rivci.subresourceRange.layerCount = 1;
    VkImageView rview;
    vkCreateImageView(dev, &rivci, NULL, &rview);
    {
        VkFramebufferCreateInfo fci = {0};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = rpass;
        fci.width = W;
        fci.height = H;
        fci.layers = 1;
        fci.attachmentCount = 1;
        fci.pAttachments = &rview;
        vkCreateFramebuffer(dev, &fci, NULL, &rfb);
    }

    /* pipeline for both images */
    VkShaderModule vs = 0, fs = 0;
    {
        VkShaderModuleCreateInfo sci = {0};
        sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sci.codeSize = sizeof(vert_spv);
        sci.pCode = vert_spv;
        vkCreateShaderModule(dev, &sci, NULL, &vs);
        sci.codeSize = sizeof(frag_spv);
        sci.pCode = frag_spv;
        vkCreateShaderModule(dev, &sci, NULL, &fs);
    }
    VkDescriptorSetLayoutBinding db[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, 0, NULL},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, 0, NULL},
    };
    VkDescriptorSetLayoutCreateInfo dslci = {0};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = db;
    VkDescriptorSetLayout dsl;
    vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);
    VkPipelineLayoutCreateInfo pli = {0};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &pli, NULL, &pl);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {0}, {0}
    };
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = {0, 0, W, H, 0, 1};
    VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps = {0};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo ras = {0};
    ras.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    ras.polygonMode = VK_POLYGON_MODE_FILL;
    ras.cullMode = VK_CULL_MODE_NONE;
    ras.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {0};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    VkPipelineRenderingCreateInfo ri = {0};
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = (VkFormat[]){VK_FORMAT_R8G8B8A8_UNORM};
    VkGraphicsPipelineCreateInfo gpi = {0};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps;
    gpi.pRasterizationState = &ras;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cbs;
    gpi.layout = pl;
    gpi.pNext = &ri;
    VkPipeline pipe;
    VkResult gir = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe);
    printf("pipeline create: r=%d\n", gir);
    if (gir != VK_SUCCESS)
        return 1;

    VkSamplerCreateInfo sampci = {0};
    sampci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampci.magFilter = VK_FILTER_NEAREST;
    sampci.minFilter = VK_FILTER_NEAREST;
    sampci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSampler samp;
    vkCreateSampler(dev, &sampci, NULL, &samp);

    VkDescriptorPoolCreateInfo dpci = {0};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = (VkDescriptorPoolSize[]){
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
    };
    VkDescriptorPool dpool;
    vkCreateDescriptorPool(dev, &dpci, NULL, &dpool);
    VkDescriptorSet ds;
    VkDescriptorSetAllocateInfo dsai = {0};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    vkAllocateDescriptorSets(dev, &dsai, &ds);

    VkBufferCreateInfo ubci = {0};
    ubci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ubci.size = 256;
    ubci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VkBuffer ubuf;
    vkCreateBuffer(dev, &ubci, NULL, &ubuf);
    VkMemoryRequirements umr;
    vkGetBufferMemoryRequirements(dev, ubuf, &umr);
    VkMemoryAllocateInfo umai = mai;
    umai.allocationSize = umr.size;
    VkDeviceMemory umem;
    vkAllocateMemory(dev, &umai, NULL, &umem);
    vkBindBufferMemory(dev, ubuf, umem, 0);
    /* no UBO use in this simple frag (binding 1 is extraUNUSED for practice) */

    for (int t = 0; t < 1; t++) {
        VkImageTiling tiling = t ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
        const char *tname = t ? "LINEAR" : "OPTIMAL";
        VkImage img;
        VkDeviceMemory imem[1];
        {
            VkImageCreateInfo ii = {0};
            ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = VK_FORMAT_R8G8B8A8_UNORM;
            ii.extent.width = W;
            ii.extent.height = H;
            ii.extent.depth = 1;
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = tiling;
            ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if (vkCreateImage(dev, &ii, NULL, &img) != VK_SUCCESS) {
                printf("%s image create failed\n", tname);
                continue;
            }
        }
        VkMemoryRequirements imr;
        vkGetImageMemoryRequirements(dev, img, &imr);
        VkMemoryAllocateInfo imai = mai;
        imai.allocationSize = imr.size;
        imai.memoryTypeIndex = pick_mem(imr.memoryTypeBits, 0);
        vkAllocateMemory(dev, &imai, NULL, &imem[0]);
        vkBindImageMemory(dev, img, imem[0], 0);

        VkImageViewCreateInfo ivci = {0};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = img;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        VkImageView view;
        vkCreateImageView(dev, &ivci, NULL, &view);
        (void)view;

        VkWriteDescriptorSet wds[2] = {
            {0}, {0}
        };
        wds[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[0].dstSet = ds;
        wds[0].dstBinding = 0;
        wds[0].descriptorCount = 1;
        wds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        VkDescriptorImageInfo dii = {samp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        wds[0].pImageInfo = &dii;
        wds[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[1].dstSet = ds;
        wds[1].dstBinding = 1;
        wds[1].descriptorCount = 1;
        wds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        VkDescriptorBufferInfo dbi = {ubuf, 0, 256};
        wds[1].pBufferInfo = &dbi;
        vkUpdateDescriptorSets(dev, 2, wds, 0, NULL);

        VkCommandBufferAllocateInfo cai = {0};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer c;
        vkAllocateCommandBuffers(dev, &cai, &c);
        VkCommandBufferBeginInfo bi = {0};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(c, &bi);
        VkImageMemoryBarrier b = {0};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.image = img;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                             1, &b);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageExtent.width = W;
        r.imageExtent.height = H;
        r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(c, pbuf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b);
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
        VkRenderingAttachmentInfo att = {0};
        att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView = rview;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkRenderingInfo ri2 = {0};
        ri2.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri2.renderArea.extent.width = W;
        ri2.renderArea.extent.height = H;
        ri2.colorAttachmentCount = 1;
        ri2.pColorAttachments = &att;
        {
            VkImageMemoryBarrier rb = {0};
            rb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            rb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            rb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            rb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            rb.image = rimg;
            rb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rb.subresourceRange.levelCount = 1;
            rb.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 0, NULL, 0, NULL, 1, &rb);
        }
        VkRenderPassBeginInfo rp2 = {0};
        rp2.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp2.renderPass = rpass;
        rp2.framebuffer = rfb;
        rp2.renderArea.extent.width = W;
        rp2.renderArea.extent.height = H;
        vkCmdBeginRenderPass(c, &rp2, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdDraw(c, 3, 1, 0, 0);
        vkCmdEndRenderPass(c);
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.image = rimg;
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                             1, &b);
        r.imageSubresource.mipLevel = 0;
        vkCmdCopyImageToBuffer(c, rimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &r);
        vkEndCommandBuffer(c);
        VkSubmitInfo si = {0};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &c;
        vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(q);

        uint8_t *rb = rptr;
        /* sampled pixel (10,10) should be pattern(10,10) = 0a0a14 */
        uint8_t *p1 = rb + ((size_t)10 * W + 10) * 4;
        uint8_t *p2 = rb + ((size_t)40 * W + 5) * 4;
        printf("SAMP %-8s: (10,10)=%02x%02x%02x%02x(want 0a0a14ff) (5,40)=%02x%02x%02x%02x(want 05282dff)\n",
               tname, p1[0], p1[1], p1[2], p1[3], p2[0], p2[1], p2[2], p2[3]);
        memset(rptr, 0, W * H * 4);
    }
    printf("done\n");
    return 0;
}
