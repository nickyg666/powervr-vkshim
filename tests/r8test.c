/*
 * Test how the PowerVR driver samples an R8_UNORM texture.
 *
 * The game renders its text from an R8_UNORM font atlas; if the driver does
 * not sample single channel textures correctly, the text comes out as a solid
 * block of colour.
 *
 * usage: r8test
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GI(inst, name) ((PFN_vkVoidFunction)vkGetInstanceProcAddr((inst), (name)))
#define GD(dev, name) ((PFN_vkVoidFunction)vkGetInstanceProcAddr(inst, (name)))

static const char *VS =
    "#version 330\n"
    "layout(location=0) out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  uv = p;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *FS =
    "#version 330\n"
    "uniform sampler2D tex;\n"
    "layout(location=0) in vec2 uv;\n"
    "layout(location=0) out vec4 outColor;\n"
    "void main() {\n"
    "  outColor = texture(tex, uv);\n"
    "}\n";

static VkShaderModule compile(PFN_vkCreateShaderModule p, VkDevice dev, const char *src)
{
    VkShaderModuleCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = strlen(src);
    ci.pCode = (const uint32_t *)src;
    VkShaderModule m = VK_NULL_HANDLE;
    if (p(dev, &ci, NULL, &m) != VK_SUCCESS) {
        fprintf(stderr, "shader module failed\n");
        exit(1);
    }
    return m;
}

int main(void)
{
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    if (((PFN_vkCreateInstance)GI(NULL, "vkCreateInstance"))(&ici, NULL, &inst) != VK_SUCCESS) {
        fprintf(stderr, "instance failed\n");
        return 1;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = pds[0];

    PFN_vkGetPhysicalDeviceQueueFamilyProperties pQFam =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)GI(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t qn = 0;
    pQFam(pd, &qn, NULL);
    VkQueueFamilyProperties *q = calloc(qn, sizeof(*q));
    pQFam(pd, &qn, q);
    uint32_t qfi = 0;
    for (uint32_t i = 0; i < qn; i++)
        if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
        fprintf(stderr, "device failed\n");
        return 1;
    }
    VkQueue queue;
    vkGetDeviceQueue(dev, qfi, 0, &queue);

    const uint32_t W = 64, H = 64;
    /* ---- the R8 texture: the left half is 0x00, the right half 0xff ---- */
    uint8_t tex[W * H];
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            tex[y * W + x] = (x < W / 2) ? 0x00 : 0xff;

    PFN_vkCreateBuffer pCB = (PFN_vkCreateBuffer)GD(dev, "vkCreateBuffer");
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(tex);
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf;
    pCB(dev, &bci, NULL, &buf);
    PFN_vkGetBufferMemoryRequirements pBMR = (PFN_vkGetBufferMemoryRequirements)GD(dev, "vkGetBufferMemoryRequirements");
    VkMemoryRequirements mr;
    pBMR(dev, buf, &mr);
    PFN_vkAllocateMemory pAM = (PFN_vkAllocateMemory)GD(dev, "vkAllocateMemory");
    PFN_vkBindBufferMemory pBBM = (PFN_vkBindBufferMemory)GD(dev, "vkBindBufferMemory");
    VkDeviceMemory mem;
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = 0;
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof(mp));
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t ntypes = mp.memoryTypeCount;
    for (uint32_t i = 0; i < ntypes; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mai.memoryTypeIndex = i; break;
        }
    pAM(dev, &mai, NULL, &mem);
    pBBM(dev, buf, mem, 0);
    void *ptr = NULL;
    vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr);
    memcpy(ptr, tex, sizeof(tex));
    vkUnmapMemory(dev, mem);

    fprintf(stderr, "step: buffers ok\n");
    PFN_vkCreateImage pCI = (PFN_vkCreateImage)GD(dev, "vkCreateImage");
    VkImageCreateInfo ici2;
    memset(&ici2, 0, sizeof(ici2));
    ici2.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici2.imageType = VK_IMAGE_TYPE_2D;
    ici2.format = VK_FORMAT_R8_UNORM;
    ici2.extent.width = W; ici2.extent.height = H; ici2.extent.depth = 1;
    ici2.mipLevels = 1; ici2.arrayLayers = 1;
    ici2.samples = VK_SAMPLE_COUNT_1_BIT;
    ici2.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici2.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    if (pCI(dev, &ici2, NULL, &img) != VK_SUCCESS) { fprintf(stderr, "image failed\n"); return 1; }
    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(dev, img, &imr);
    VkMemoryAllocateInfo imai = mai;
    imai.allocationSize = imr.size;
    for (uint32_t i = 0; i < ntypes; i++)
        if (imr.memoryTypeBits & (1u << i)) { imai.memoryTypeIndex = i; break; }
    VkDeviceMemory imem;
    pAM(dev, &imai, NULL, &imem);
    vkBindImageMemory(dev, img, imem, 0);

    /* ---- a render target ---- */
    VkImageCreateInfo rci = ici2;
    rci.format = VK_FORMAT_R8G8B8A8_UNORM;
    rci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage rt;
    pCI(dev, &rci, NULL, &rt);
    VkMemoryRequirements rmr;
    vkGetImageMemoryRequirements(dev, rt, &rmr);
    VkMemoryAllocateInfo rmai = mai;
    rmai.allocationSize = rmr.size;
    for (uint32_t i = 0; i < ntypes; i++)
        if (rmr.memoryTypeBits & (1u << i)) { rmai.memoryTypeIndex = i; break; }
    VkDeviceMemory rmem;
    pAM(dev, &rmai, NULL, &rmem);
    vkBindImageMemory(dev, rt, rmem, 0);

    VkImageViewCreateInfo vci;
    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VkImageView view;
    vkCreateImageView(dev, &vci, NULL, &view);
    vci.image = rt;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageView rtview;
    vkCreateImageView(dev, &vci, NULL, &rtview);

    VkSamplerCreateInfo sci;
    memset(&sci, 0, sizeof(sci));
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 1.0f;
    VkSampler samp;
    vkCreateSampler(dev, &sci, NULL, &samp);

    fprintf(stderr, "step: images ok\n");
    /* ---- pipeline ---- */
    PFN_vkCreateShaderModule pCSM = (PFN_vkCreateShaderModule)GD(dev, "vkCreateShaderModule");
    VkShaderModule vs = compile(pCSM, dev, VS);
    VkShaderModule fs = compile(pCSM, dev, FS);

    VkDescriptorSetLayoutBinding b;
    memset(&b, 0, sizeof(b));
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci;
    memset(&dslci, 0, sizeof(dslci));
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1; dslci.pBindings = &b;
    VkDescriptorSetLayout dsl;
    vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);
    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
    VkPipelineLayout playout;
    vkCreatePipelineLayout(dev, &plci, NULL, &playout);

    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof(st));
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi;
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia;
    memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
    VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps;
    memset(&vps, 0, sizeof(vps));
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs;
    memset(&rs, 0, sizeof(rs));
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms;
    memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss;
    memset(&dss, 0, sizeof(dss));
    dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkPipelineColorBlendAttachmentState att;
    memset(&att, 0, sizeof(att));
    att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb;
    memset(&cb, 0, sizeof(cb));
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &att;
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo pr;
    memset(&pr, 0, sizeof(pr));
    pr.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pr.colorAttachmentCount = 1; pr.pColorAttachmentFormats = &cf;
    pr.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo gci;
    memset(&gci, 0, sizeof(gci));
    gci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gci.pNext = &pr;
    gci.stageCount = 2; gci.pStages = st;
    gci.pVertexInputState = &vi; gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vps; gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms; gci.pDepthStencilState = &dss;
    gci.pColorBlendState = &cb;
    gci.layout = playout;
    fprintf(stderr, "step: pipeline state ok\n");
    VkPipeline pipe;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe);
    fprintf(stderr, "step: pipeline created r=%d\n", (int)r);
    printf("pipeline r=%d\n", (int)r);
    if (r != VK_SUCCESS) return 1;

    fprintf(stderr, "step: pipeline created\n");
    /* ---- descriptor set ---- */
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof(dpci));
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &ps;
    VkDescriptorPool pool;
    vkCreateDescriptorPool(dev, &dpci, NULL, &pool);
    VkDescriptorSetAllocateInfo dsai;
    memset(&dsai, 0, sizeof(dsai));
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet set;
    vkAllocateDescriptorSets(dev, &dsai, &set);
    VkDescriptorImageInfo dii;
    memset(&dii, 0, sizeof(dii));
    dii.sampler = samp; dii.imageView = view; dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w;
    memset(&w, 0, sizeof(w));
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    fprintf(stderr, "step: descriptor ok\n");
    /* ---- command buffer ---- */
    VkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = qfi;
    VkCommandPool pool2;
    vkCreateCommandPool(dev, &cpci, NULL, &pool2);
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool2; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &cbai, &cmd);
    VkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &cbbi);

    VkImageMemoryBarrier b1;
    memset(&b1, 0, sizeof(b1));
    b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.image = img; b1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b1.subresourceRange.levelCount = 1; b1.subresourceRange.layerCount = 1;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    VkBufferImageCopy bic;
    memset(&bic, 0, sizeof(bic));
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = W; bic.imageExtent.height = H; bic.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);

    b1.image = rt; b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b1.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b1.srcAccessMask = 0; b1.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    VkRenderingAttachmentInfo ai2;
    memset(&ai2, 0, sizeof(ai2));
    ai2.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ai2.imageView = rtview; ai2.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ai2.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ai2.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri;
    memset(&ri, 0, sizeof(ri));
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = W; ri.renderArea.extent.height = H;
    ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ai2;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &set, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ull);

    /* ---- read back ---- */
    VkBufferCreateInfo rbci = bci;
    rbci.size = W * H * 4;
    rbci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer rbuf;
    pCB(dev, &rbci, NULL, &rbuf);
    VkMemoryRequirements rbmr;
    pBMR(dev, rbuf, &rbmr);
    VkMemoryAllocateInfo rbmai = mai;
    rbmai.allocationSize = rbmr.size;
    for (uint32_t i = 0; i < ntypes; i++)
        if ((rbmr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            rbmai.memoryTypeIndex = i; break;
        }
    VkDeviceMemory rbmem;
    pAM(dev, &rbmai, NULL, &rbmem);
    pBBM(dev, rbuf, rbmem, 0);
    void *rptr = NULL;
    vkMapMemory(dev, rbmem, 0, VK_WHOLE_SIZE, 0, &rptr);

    VkCommandBufferBeginInfo cbbi2 = cbbi;
    vkBeginCommandBuffer(cmd, &cbbi2);
    b1.image = rt; b1.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b1.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
    VkBufferImageCopy bic2 = bic;
    bic2.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyImageToBuffer(cmd, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &bic2);
    vkEndCommandBuffer(cmd);
    vkResetFences(dev, 1, &fence);
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ull);

    uint8_t *out = (uint8_t *)rptr;
    printf("R8 sampling test (left half should be 0x00, right half 0xff):\n");
    printf("  pixel(8,32)   = %02x %02x %02x %02x\n", out[(32 * W + 8) * 4 + 0], out[(32 * W + 8) * 4 + 1], out[(32 * W + 8) * 4 + 2], out[(32 * W + 8) * 4 + 3]);
    printf("  pixel(56,32)  = %02x %02x %02x %02x\n", out[(32 * W + 56) * 4 + 0], out[(32 * W + 56) * 4 + 1], out[(32 * W + 56) * 4 + 2], out[(32 * W + 56) * 4 + 3]);
    return 0;
}
