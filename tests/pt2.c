/*
 * Focused pipeline reproducer for the PowerVR driver.
 *
 * usage: pt2 <vs.spv> <fs.spv> <layout> <color_fmt> <depth_fmt> <blend> <colorop>
 *
 *   <layout>  comma separated "binding:type" pairs, e.g. "0:6,1:6,2:6,3:6,4:1"
 *             VkDescriptorType: 0=SAMPLER 1=COMBINED_IMAGE_SAMPLER 2=SAMPLED_IMAGE
 *                                4=UNIFORM_TEXEL_BUFFER 6=UNIFORM_BUFFER
 *   <blend>   0 = no blending, 1 = blending
 *   <colorop> 0 = ADD, 4 = MINIMUM
 */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GI(inst, name) ((PFN_vkVoidFunction)vkGetInstanceProcAddr((inst), (name)))
#define GD(dev, name) ((PFN_vkVoidFunction)vkGetDeviceProcAddr((dev), (name)))

static uint32_t *read_spv(const char *path, size_t *words)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc(n);
    if (fread(w, 1, n, f) != (size_t)n) exit(1);
    fclose(f);
    *words = n / 4;
    return w;
}

int main(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr, "usage: %s vs.spv fs.spv layout color_fmt depth_fmt blend colorop\n", argv[0]);
        return 2;
    }
    const char *vs_path = argv[1], *fs_path = argv[2], *layout_spec = argv[3];
    VkFormat color_fmt = (VkFormat)strtoul(argv[4], NULL, 0);
    VkFormat depth_fmt = (VkFormat)strtoul(argv[5], NULL, 0);
    int blend = atoi(argv[6]);
    int colorop = atoi(argv[7]);

    PFN_vkGetInstanceProcAddr gipa = vkGetInstanceProcAddr;

    VkApplicationInfo ai; memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici; memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = iext;
    VkInstance inst;
    if (((PFN_vkCreateInstance)GI(NULL, "vkCreateInstance"))(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "instance failed\n"); return 1; }

    PFN_vkEnumeratePhysicalDevices pEnum = (PFN_vkEnumeratePhysicalDevices)GI(inst, "vkEnumeratePhysicalDevices");
    uint32_t n = 0; pEnum(inst, &n, NULL);
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds)); pEnum(inst, &n, pds);
    VkPhysicalDevice pd = pds[0];

    PFN_vkGetPhysicalDeviceQueueFamilyProperties pQFam = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)GI(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t qn = 0; pQFam(pd, &qn, NULL);
    VkQueueFamilyProperties *q = calloc(qn, sizeof(*q)); pQFam(pd, &qn, q);
    uint32_t qfi = 0;
    for (uint32_t i = 0; i < qn; i++)
        if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci; memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                           VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, "VK_KHR_synchronization2",
                           "VK_EXT_calibrated_timestamps", "VK_EXT_vertex_attribute_divisor"};
    VkPhysicalDeviceVulkan12Features v12f; memset(&v12f, 0, sizeof(v12f));
    v12f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    v12f.uniformBufferStandardLayout = VK_TRUE;
    VkPhysicalDeviceVulkan11Features v11f; memset(&v11f, 0, sizeof(v11f));
    v11f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    v11f.pNext = &v12f;
    VkPhysicalDeviceFeatures2 feats2; memset(&feats2, 0, sizeof(feats2));
    feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats2.pNext = NULL;
    VkDeviceCreateInfo dci; memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &feats2;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 6; dci.ppEnabledExtensionNames = dext;
    VkDevice dev;
    if (((PFN_vkCreateDevice)GI(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev) != VK_SUCCESS) { fprintf(stderr, "device failed\n"); return 1; }

    /* ---- descriptor set layout ---- */
    VkDescriptorSetLayoutBinding b[16];
    memset(b, 0, sizeof(b));
    int nb = 0;
    { char tmp[256]; strncpy(tmp, layout_spec, sizeof(tmp) - 1); tmp[sizeof(tmp)-1] = 0;
      for (char *tok = strtok(tmp, ","); tok && nb < 16; tok = strtok(NULL, ",")) {
          int bind = 0, type = 0;
          sscanf(tok, "%d:%d", &bind, &type);
          b[nb].binding = (uint32_t)bind;
          b[nb].descriptorType = (VkDescriptorType)type;
          b[nb].descriptorCount = 1;
          b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
          nb++;
      } }
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorSetLayoutCreateInfo dslci; memset(&dslci, 0, sizeof(dslci));
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = (uint32_t)nb; dslci.pBindings = b;
    ((PFN_vkCreateDescriptorSetLayout)GD(dev, "vkCreateDescriptorSetLayout"))(dev, &dslci, NULL, &dsl);

    VkPushConstantRange pcr; memset(&pcr, 0, sizeof(pcr));
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkPipelineLayoutCreateInfo plci; memset(&plci, 0, sizeof(plci));
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    ((PFN_vkCreatePipelineLayout)GD(dev, "vkCreatePipelineLayout"))(dev, &plci, NULL, &playout);

    /* ---- shader modules ---- */
    size_t vw, fw;
    uint32_t *vcode = read_spv(vs_path, &vw);
    uint32_t *fcode = read_spv(fs_path, &fw);
    VkShaderModuleCreateInfo smci; memset(&smci, 0, sizeof(smci));
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    smci.codeSize = vw * 4; smci.pCode = vcode;
    printf("creating vertex module...\n"); fflush(stdout);
    if (((PFN_vkCreateShaderModule)GD(dev, "vkCreateShaderModule"))(dev, &smci, NULL, &vs) != VK_SUCCESS) { fprintf(stderr, "vertex module failed\n"); return 1; }
    smci.codeSize = fw * 4; smci.pCode = fcode;
    printf("creating fragment module...\n"); fflush(stdout);
    if (((PFN_vkCreateShaderModule)GD(dev, "vkCreateShaderModule"))(dev, &smci, NULL, &fs) != VK_SUCCESS) { fprintf(stderr, "fragment module failed\n"); return 1; }

    /* ---- pipeline ---- */
    VkPipelineShaderStageCreateInfo stages[2];
    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi; memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = {0, 0, 16, 16, 0, 1};
    VkRect2D sc = {{0, 0}, {16, 16}};
    VkPipelineViewportStateCreateInfo vps; memset(&vps, 0, sizeof(vps));
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof(rs));
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds; memset(&ds, 0, sizeof(ds));
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkPipelineColorBlendAttachmentState att; memset(&att, 0, sizeof(att));
    att.blendEnable = blend ? VK_TRUE : VK_FALSE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.colorBlendOp = (VkBlendOp)colorop;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    att.alphaBlendOp = (VkBlendOp)colorop;
    att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb; memset(&cb, 0, sizeof(cb));
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &att;
    VkFormat colorfmts[1] = {color_fmt};
    VkPipelineRenderingCreateInfo pr; memset(&pr, 0, sizeof(pr));
    pr.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pr.colorAttachmentCount = 1; pr.pColorAttachmentFormats = colorfmts;
    pr.depthAttachmentFormat = depth_fmt;
    VkGraphicsPipelineCreateInfo gci; memset(&gci, 0, sizeof(gci));
    gci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gci.pNext = &pr;
    gci.stageCount = 2; gci.pStages = stages;
    gci.pVertexInputState = &vi; gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vps; gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms; gci.pDepthStencilState = &ds;
    gci.pColorBlendState = &cb; gci.layout = playout;

    printf("creating pipeline...\n"); fflush(stdout);
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = ((PFN_vkCreateGraphicsPipelines)GD(dev, "vkCreateGraphicsPipelines"))(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe);
    printf("-> %d (pipe=%p)\n", (int)r, (void *)pipe);
    fflush(stdout);
    /* the driver crashes during teardown after a failed pipeline creation,
     * so leave immediately without cleaning up */
    _exit(r == VK_SUCCESS ? 0 : 1);
}
