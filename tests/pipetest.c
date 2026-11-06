/*
 * Pipeline creation test against the raw PowerVR ICD.
 *
 * Each combination is run in its own process (see run-matrix.sh) so a driver
 * crash cannot affect the next test.
 *
 * usage: pipetest <n_out> <color_fmt> <blend> <depth_fmt> <depth_test>
 *                 <vertex.spv> <fragment.spv>
 */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef PFN_vkVoidFunction (*PFN_gipa)(void *, const char *);

static uint32_t *read_spv(const char *path, size_t *words)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc(n);
    if (fread(w, 1, n, f) != (size_t)n)
        exit(1);
    fclose(f);
    *words = n / 4;
    return w;
}

int main(int argc, char **argv)
{
    if (argc < 9) {
        fprintf(stderr, "usage: %s n_out color_fmt blend depth_fmt depth_test vs.spv fs.spv flags\n", argv[0]);
        return 2;
    }
    int flags = atoi(argv[8]);
    int n_out = atoi(argv[1]);
    VkFormat color_fmt = (VkFormat)strtoul(argv[2], NULL, 0);
    int blend = atoi(argv[3]);
    VkFormat depth_fmt = (VkFormat)strtoul(argv[4], NULL, 0);
    int depth_test = atoi(argv[5]);
    const char *vs_path = argv[6];
    const char *fs_path = argv[7];

    PFN_gipa gipa = (PFN_gipa)vkGetInstanceProcAddr;

    PFN_vkCreateInstance pCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    VkInstance inst;
    if (pCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        fprintf(stderr, "instance failed\n");
        return 1;
    }

    PFN_vkEnumeratePhysicalDevices pEnum = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    uint32_t n = 0;
    pEnum(inst, &n, NULL);
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    pEnum(inst, &n, pds);
    VkPhysicalDevice pd = pds[0];

    PFN_vkGetPhysicalDeviceQueueFamilyProperties pQFam =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t qn = 0;
    pQFam(pd, &qn, NULL);
    VkQueueFamilyProperties *q = calloc(qn, sizeof(*q));
    pQFam(pd, &qn, q);
    uint32_t qfi = 0;
    for (uint32_t i = 0; i < qn; i++)
        if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            qfi = i;
            break;
        }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfi;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *base[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                          VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
    const char *dext[8];
    int ndext = 0;
    for (int i = 0; i < 3; i++)
        dext[ndext++] = base[i];
    if (flags & 256) dext[ndext++] = "VK_KHR_synchronization2";
    if (flags & 512) dext[ndext++] = "VK_EXT_calibrated_timestamps";
    if (flags & 1024) dext[ndext++] = "VK_EXT_vertex_attribute_divisor";
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)ndext;
    dci.ppEnabledExtensionNames = dext;
    PFN_vkCreateDevice pCreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    VkDevice dev;
    if (pCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
        fprintf(stderr, "device failed\n");
        return 1;
    }

    PFN_vkVoidFunction t0 = vkGetDeviceProcAddr(dev, "vkCreateShaderModule");
    PFN_vkVoidFunction t1 = vkGetDeviceProcAddr(dev, "vkCreateGraphicsPipelines");
    printf("gdpa(vkCreateShaderModule)=%p gdpa(vkCreateGraphicsPipelines)=%p\n", (void *)t0, (void *)t1);
    fflush(stdout);
    PFN_vkCreateShaderModule pCSM = (PFN_vkCreateShaderModule)t0;
    size_t vw, fw;
    uint32_t *vcode = read_spv(vs_path, &vw);
    uint32_t *fcode = read_spv(fs_path, &fw);
    VkShaderModuleCreateInfo smci;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    memset(&smci, 0, sizeof(smci));
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = vw * 4;
    smci.pCode = vcode;
    if (pCSM(dev, &smci, NULL, &vs) != VK_SUCCESS) {
        fprintf(stderr, "vertex module failed\n");
        return 1;
    }
    smci.codeSize = fw * 4;
    smci.pCode = fcode;
    if (pCSM(dev, &smci, NULL, &fs) != VK_SUCCESS) {
        fprintf(stderr, "fragment module failed\n");
        return 1;
    }

    /* optional descriptor set layout with one sampled image */
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    if (flags & 4) {
        VkDescriptorSetLayoutBinding b[8];
        memset(b, 0, sizeof(b));
        int nb = (flags & 64) ? 5 : ((flags & 32) ? 7 : 4);
        for (int i = 0; i < nb; i++) {
            b[i].binding = (uint32_t)i;
            if (flags & 16) {
                b[i].descriptorType = (i < 4) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            } else if (flags & 32) {
                /* the layout Minecraft uses: 0-2 uniform buffers, 3-6 samplers */
                b[i].descriptorType = (i < 3) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            } else if (flags & 64) {
                /* the layout Minecraft uses: 0,1,2 uniform buffers, 3 uniform
                 * buffer (mismatched!), 4 sampler */
                b[i].descriptorType = (i < 4) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            } else {
                b[i].descriptorType = (i < 2) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslci;
        memset(&dslci, 0, sizeof(dslci));
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = (uint32_t)nb;
        dslci.pBindings = b;
        PFN_vkCreateDescriptorSetLayout pCDSL =
            (PFN_vkCreateDescriptorSetLayout)vkGetDeviceProcAddr(dev, "vkCreateDescriptorSetLayout");
        if (pCDSL)
            pCDSL(dev, &dslci, NULL, &dsl);
    }

    /* minimal pipeline layout: no descriptors, no push constants */
    VkPushConstantRange pcr;
    memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = (flags & 64) ? 128 : 16;
    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.pushConstantRangeCount = (flags & 1) ? 1 : 0;
    plci.pPushConstantRanges = (flags & 1) ? &pcr : NULL;
    if (dsl != VK_NULL_HANDLE) {
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dsl;
    }
    PFN_vkCreatePipelineLayout pCPL = (PFN_vkCreatePipelineLayout)vkGetDeviceProcAddr(dev, "vkCreatePipelineLayout");
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (pCPL(dev, &plci, NULL, &layout) != VK_SUCCESS) {
        fprintf(stderr, "pipeline layout failed\n");
        return 1;
    }

    VkFormat fmts[8];
    for (int i = 0; i < n_out; i++)
        fmts[i] = color_fmt;

    VkPipelineRenderingCreateInfo prci;
    memset(&prci, 0, sizeof(prci));
    prci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    prci.colorAttachmentCount = n_out;
    prci.pColorAttachmentFormats = fmts;
    prci.depthAttachmentFormat = depth_fmt;

    VkPipelineColorBlendAttachmentState att[8];
    memset(att, 0, sizeof(att));
    for (int i = 0; i < n_out; i++) {
        att[i].blendEnable = blend ? VK_TRUE : VK_FALSE;
        att[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att[i].colorBlendOp = (flags & 8) ? (VkBlendOp)4 /* MINIMUM */ : VK_BLEND_OP_ADD;
        att[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att[i].alphaBlendOp = (flags & 8) ? (VkBlendOp)4 /* MINIMUM */ : VK_BLEND_OP_ADD;
        att[i].colorWriteMask = 0xF;
    }
    VkPipelineColorBlendStateCreateInfo cb;
    memset(&cb, 0, sizeof(cb));
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = n_out;
    cb.pAttachments = att;

    VkPipelineDepthStencilStateCreateInfo ds;
    memset(&ds, 0, sizeof(ds));
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (flags & 64) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

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

    VkViewport viewport;
    memset(&viewport, 0, sizeof(viewport));
    viewport.width = 854.0f;
    viewport.height = 480.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor;
    memset(&scissor, 0, sizeof(scissor));
    scissor.extent.width = 854;
    scissor.extent.height = 480;

    VkPipelineViewportStateCreateInfo vp;
    memset(&vp, 0, sizeof(vp));
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;

    VkPipelineVertexInputStateCreateInfo vi;
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia;
    memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineShaderStageCreateInfo stages[2];
    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkRenderPass rp = VK_NULL_HANDLE;
    if (flags & 2) {
        VkAttachmentDescription ad[8];
        memset(ad, 0, sizeof(ad));
        for (int i = 0; i < n_out; i++) {
            ad[i].format = color_fmt;
            ad[i].samples = VK_SAMPLE_COUNT_1_BIT;
            ad[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ad[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ad[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ad[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            ad[i].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ad[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkAttachmentReference aref[8];
        memset(aref, 0, sizeof(aref));
        for (int i = 0; i < n_out; i++)
            aref[i].attachment = (uint32_t)i;
        VkSubpassDescription sd;
        memset(&sd, 0, sizeof(sd));
        sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sd.colorAttachmentCount = n_out;
        sd.pColorAttachments = aref;
        VkRenderPassCreateInfo rpci;
        memset(&rpci, 0, sizeof(rpci));
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = n_out;
        rpci.pAttachments = ad;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sd;
        PFN_vkCreateRenderPass pCRP = (PFN_vkCreateRenderPass)vkGetDeviceProcAddr(dev, "vkCreateRenderPass");
        if (pCRP && pCRP(dev, &rpci, NULL, &rp) != VK_SUCCESS)
            rp = VK_NULL_HANDLE;
    }

    VkGraphicsPipelineCreateInfo gci;
    memset(&gci, 0, sizeof(gci));
    gci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gci.stageCount = 2;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pDepthStencilState = &ds;
    gci.pColorBlendState = &cb;
    gci.layout = layout;
    gci.renderPass = rp;
    if (!rp)
        gci.pNext = &prci;

    PFN_vkCreateGraphicsPipelines pCGP = (PFN_vkCreateGraphicsPipelines)vkGetDeviceProcAddr(dev, "vkCreateGraphicsPipelines");
    VkPipeline pipe = VK_NULL_HANDLE;
    printf("attempt 1 (original shader): ");
    fflush(stdout);
    VkResult r = pCGP(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe);
    printf("-> %d (%p)\n", r, (void *)pipe);
    fflush(stdout);
    if (argc > 9) {
        /* second attempt with a replacement fragment shader */
        size_t fw2;
        uint32_t *fcode2 = read_spv(argv[9], &fw2);
        VkShaderModule fs2 = VK_NULL_HANDLE;
        smci.codeSize = fw2 * 4;
        smci.pCode = fcode2;
        if (pCSM(dev, &smci, NULL, &fs2) != VK_SUCCESS) {
            printf("replacement module failed\n");
            return 1;
        }
        stages[1].module = fs2;
        VkPipeline pipe2 = VK_NULL_HANDLE;
        printf("attempt 2 (replacement shader): ");
        fflush(stdout);
        VkResult r2 = pCGP(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe2);
        printf("-> %d (%p)\n", r2, (void *)pipe2);
        fflush(stdout);
    }
    return 0;
}
