#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR

typedef void *VkInstance_t;
typedef void *VkDevice_t;
typedef int32_t VkResult_t;

typedef PFN_vkVoidFunction (*PFN_gipa)(VkInstance_t, const char *);
typedef VkResult (*PFN_negver)(uint32_t *);
typedef PFN_vkVoidFunction (*PFN_gpdpa)(VkInstance_t, const char *);

static void *g_real;
static PFN_gipa g_real_gipa;
static PFN_negver g_real_negver;
static PFN_gpdpa g_real_gpdpa;

typedef VkResult (VKAPI_PTR *PFN_CreateGraphicsPipelines)(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
typedef VkResult (VKAPI_PTR *PFN_CreateComputePipelines)(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
typedef VkResult (VKAPI_PTR *PFN_CreateShaderModule)(VkDevice, const VkShaderModuleCreateInfo *, const VkAllocationCallbacks *, VkShaderModule *);
typedef VkResult (VKAPI_PTR *PFN_CreatePipelineLayout)(VkDevice, const VkPipelineLayoutCreateInfo *, const VkAllocationCallbacks *, VkPipelineLayout *);
typedef VkResult (VKAPI_PTR *PFN_CreateDescriptorSetLayout)(VkDevice, const VkDescriptorSetLayoutCreateInfo *, const VkAllocationCallbacks *, VkDescriptorSetLayout *);
typedef VkResult (VKAPI_PTR *PFN_CreateSampler)(VkDevice, const VkSamplerCreateInfo *, const VkAllocationCallbacks *, VkSampler *);

static PFN_CreateGraphicsPipelines g_real_create_graphics_pipelines;
static PFN_CreateComputePipelines g_real_create_compute_pipelines;
static PFN_CreateShaderModule g_real_create_shader_module;
static PFN_CreatePipelineLayout g_real_create_pipeline_layout;
static PFN_CreateDescriptorSetLayout g_real_create_descriptor_set_layout;
static PFN_CreateSampler g_real_create_sampler;

static PFN_vkGetInstanceProcAddr g_real_get_instance_proc_addr;

static pthread_mutex_t g_lock;

static FILE *g_log;

static void log_line(const char *msg) {
    if (!g_log)
        return;
    fprintf(g_log, "%s\n", msg);
    fflush(g_log);
}

static void logf2(const char *fmt, long a, long b) {
    if (!g_log)
        return;
    fprintf(g_log, fmt, a, b);
    fflush(g_log);
}


static void resolve_all(VkInstance_t instance) {
    if (!g_real_gipa)
        return;
    if (!g_real_create_graphics_pipelines)
        g_real_create_graphics_pipelines = (PFN_CreateGraphicsPipelines)g_real_gipa(instance, "vkCreateGraphicsPipelines");
    if (!g_real_create_compute_pipelines)
        g_real_create_compute_pipelines = (PFN_CreateComputePipelines)g_real_gipa(instance, "vkCreateComputePipelines");
    if (!g_real_create_shader_module)
        g_real_create_shader_module = (PFN_CreateShaderModule)g_real_gipa(instance, "vkCreateShaderModule");
    if (!g_real_create_pipeline_layout)
        g_real_create_pipeline_layout = (PFN_CreatePipelineLayout)g_real_gipa(instance, "vkCreatePipelineLayout");
    if (!g_real_create_descriptor_set_layout)
        g_real_create_descriptor_set_layout = (PFN_CreateDescriptorSetLayout)g_real_gipa(instance, "vkCreateDescriptorSetLayout");
    if (!g_real_create_sampler)
        g_real_create_sampler = (PFN_CreateSampler)g_real_gipa(instance, "vkCreateSampler");
}

static VkResult VKAPI_CALL wrap_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n,
                                                          const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    pthread_mutex_lock(&g_lock);
    VkResult_t r = g_real_create_graphics_pipelines(d, c, n, ci, a, out);
    if (getenv("VKSER_DEBUG") && g_log) {
        fprintf(g_log, "CGP called n=%u r=%d pNext=%p\n", n, r, (void *)(ci ? ci->pNext : 0));
        fflush(g_log);
    }
    if (r != 0 && g_log) {
        const VkGraphicsPipelineCreateInfo *p = (const VkGraphicsPipelineCreateInfo *)ci;
        fprintf(g_log,
                "CGP FAILED r=%d n=%u stageCount=%u vi=%p ia=%p rs=%p ds=%p cb=%p vp=%p ms=%p dyn=%p layout=%p pNext=%p flags=%x\n",
                r, n, p ? p->stageCount : 0,
                p ? (void *)p->pVertexInputState : 0, p ? (void *)p->pInputAssemblyState : 0,
                p ? (void *)p->pRasterizationState : 0, p ? (void *)p->pDepthStencilState : 0,
                p ? (void *)p->pColorBlendState : 0, p ? (void *)p->pViewportState : 0,
                p ? (void *)p->pMultisampleState : 0, p ? (void *)p->pDynamicState : 0,
                p ? (void *)p->layout : 0, p ? (void *)p->pNext : 0, p ? p->flags : 0);
        if (p && p->pColorBlendState) {
            const VkPipelineColorBlendStateCreateInfo *cb = (const VkPipelineColorBlendStateCreateInfo *)p->pColorBlendState;
            fprintf(g_log, "  blend attachCount=%u logicOp=%x consts=%f,%f,%f,%f\n",
                    cb->attachmentCount, cb->logicOp, cb->blendConstants[0], cb->blendConstants[1],
                    cb->blendConstants[2], cb->blendConstants[3]);
            for (uint32_t i = 0; i < cb->attachmentCount && i < 4; i++)
                fprintf(g_log, "  att%u blendEnable=%d colorWriteMask=%x srcColor=%x dstColor=%x op=%x srcAlpha=%x dstAlpha=%x aop=%x\n",
                        i, cb->pAttachments[i].blendEnable, cb->pAttachments[i].colorWriteMask,
                        cb->pAttachments[i].srcColorBlendFactor, cb->pAttachments[i].dstColorBlendFactor,
                        cb->pAttachments[i].colorBlendOp, cb->pAttachments[i].srcAlphaBlendFactor,
                        cb->pAttachments[i].dstAlphaBlendFactor, cb->pAttachments[i].alphaBlendOp);
        }
        if (p && p->pNext) {
            const VkPipelineRenderingCreateInfoKHR *pr = (const VkPipelineRenderingCreateInfoKHR *)p->pNext;
            fprintf(g_log, "  rendering colorAttachCount=%u depthFmt=%x stencilFmt=%x viewMask=%x\n",
                    pr->colorAttachmentCount, pr->depthAttachmentFormat, pr->stencilAttachmentFormat, pr->viewMask);
            for (uint32_t i = 0; i < pr->colorAttachmentCount && i < 8; i++)
                fprintf(g_log, "  fmt%u=%x\n", i, pr->pColorAttachmentFormats ? pr->pColorAttachmentFormats[i] : 0);
        }
        if (p && p->pDepthStencilState) {
            const VkPipelineDepthStencilStateCreateInfo *ds = (const VkPipelineDepthStencilStateCreateInfo *)p->pDepthStencilState;
            fprintf(g_log, "  depthTest=%d depthWrite=%d depthCompare=%x\n",
                    ds->depthTestEnable, ds->depthWriteEnable, ds->depthCompareOp);
        }
        if (p && p->pRasterizationState) {
            const VkPipelineRasterizationStateCreateInfo *rs = (const VkPipelineRasterizationStateCreateInfo *)p->pRasterizationState;
            fprintf(g_log, "  raster polygonMode=%x cull=%x front=%x rasterDiscard=%d lineWidth=%f\n",
                    rs->polygonMode, rs->cullMode, rs->frontFace, rs->rasterizerDiscardEnable, rs->lineWidth);
        }
        if (p && p->pStages) {
            for (uint32_t i = 0; i < p->stageCount; i++)
                fprintf(g_log, "  stage%u stage=%x module=%p name=%p\n", i, p->pStages[i].stage,
                        (void *)p->pStages[i].module, (void *)p->pStages[i].pName);
        }
        fflush(g_log);
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static VkResult VKAPI_CALL wrap_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n,
                                                         const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    pthread_mutex_lock(&g_lock);
    VkResult_t r = g_real_create_compute_pipelines(d, c, n, ci, a, out);
    pthread_mutex_unlock(&g_lock);
    return r;
}

static VkResult VKAPI_CALL wrap_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    pthread_mutex_lock(&g_lock);
    VkResult_t r = g_real_create_shader_module(d, ci, a, out);
    pthread_mutex_unlock(&g_lock);
    return r;
}

static VkResult VKAPI_CALL wrap_CreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    pthread_mutex_lock(&g_lock);
    VkResult_t r = g_real_create_pipeline_layout(d, ci, a, out);
    pthread_mutex_unlock(&g_lock);
    return r;
}

static VkResult VKAPI_CALL wrap_CreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    pthread_mutex_lock(&g_lock);
    VkResult_t r = g_real_create_descriptor_set_layout(d, ci, a, out);
    pthread_mutex_unlock(&g_lock);
    return r;
}

static VkResult VKAPI_CALL wrap_CreateSampler(VkDevice d, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out) {
    pthread_mutex_lock(&g_lock);
    VkResult_t r = g_real_create_sampler(d, ci, a, out);
    pthread_mutex_unlock(&g_lock);
    return r;
}

static void resolve_all(VkInstance_t instance);

__attribute__((constructor))
static void init_wrapper(void) {
    if (getenv("VKSER_DEBUG"))
        g_log = fopen("/tmp/vkser.log", "a");
    log_line("init_wrapper");
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &attr);
    pthread_mutexattr_destroy(&attr);

    g_real = dlopen("/var/data/vk-img/libVK_IMG.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_real)
        g_real = dlopen("/lib/libVK_IMG.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_real)
        g_real = dlopen("/usr/lib/libVK_IMG.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_real) {
        log_line("dlopen failed");
        return;
    }
    g_real_gipa = (PFN_gipa)dlsym(g_real, "vk_icdGetInstanceProcAddr");
    g_real_negver = (PFN_negver)dlsym(g_real, "vk_icdNegotiateLoaderICDInterfaceVersion");
    g_real_gpdpa = (PFN_gpdpa)dlsym(g_real, "vk_icdGetPhysicalDeviceProcAddr");
    if (g_real_gipa)
        g_real_get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)g_real_gipa(0, "vkGetInstanceProcAddr");
    logf2("real gipa=%p negver=%p", (long)(uintptr_t)g_real_gipa, (long)(uintptr_t)g_real_negver);
}

static PFN_vkVoidFunction wrap_device_function(const char *name);
static PFN_vkVoidFunction VKAPI_PTR wrap_GetInstanceProcAddr(VkInstance instance, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance_t instance, const char *name);

/* The loader dlsym()s this symbol directly from the ICD library, so exporting it
   is the only way to intercept device-level function resolution. */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    if (name) {
        PFN_vkVoidFunction mine = wrap_device_function(name);
        if (mine) {
            if (getenv("VKSER_DEBUG") && g_log) {
                fprintf(g_log, "exported gipa inst=%p %s -> WRAPPED\n", (void *)instance, name);
                fflush(g_log);
            }
            return mine;
        }
    }
    if (g_real_get_instance_proc_addr)
        return g_real_get_instance_proc_addr(instance, name);
    return 0;
}

VKAPI_ATTR VkResult_t VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    if (g_real_negver)
        return g_real_negver(pVersion);
    if (pVersion)
        *pVersion = 2;
    return 0;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance_t instance, const char *name) {
    if (!name)
        return 0;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
        if (g_real_get_instance_proc_addr)
            return (PFN_vkVoidFunction)wrap_GetInstanceProcAddr;
    }
    /* In loader-ICD interface v5 the loader caches the physical-device entry
       point it obtains here and calls it directly for every subsequent lookup,
       so we must hand back our own wrapper instead of the driver's. */
    if (strcmp(name, "vk_icdGetPhysicalDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vk_icdGetPhysicalDeviceProcAddr;
    if (strcmp(name, "vkCreateGraphicsPipelines") == 0) {
        if (!g_real_create_graphics_pipelines) resolve_all(instance);
        return (PFN_vkVoidFunction)wrap_CreateGraphicsPipelines;
    }
    if (strcmp(name, "vkCreateComputePipelines") == 0) {
        if (!g_real_create_compute_pipelines) resolve_all(instance);
        return (PFN_vkVoidFunction)wrap_CreateComputePipelines;
    }
    if (strcmp(name, "vkCreateShaderModule") == 0) {
        if (!g_real_create_shader_module) resolve_all(instance);
        return (PFN_vkVoidFunction)wrap_CreateShaderModule;
    }
    if (strcmp(name, "vkCreatePipelineLayout") == 0) {
        if (!g_real_create_pipeline_layout) resolve_all(instance);
        return (PFN_vkVoidFunction)wrap_CreatePipelineLayout;
    }
    if (strcmp(name, "vkCreateDescriptorSetLayout") == 0) {
        if (!g_real_create_descriptor_set_layout) resolve_all(instance);
        return (PFN_vkVoidFunction)wrap_CreateDescriptorSetLayout;
    }
    if (strcmp(name, "vkCreateSampler") == 0) {
        if (!g_real_create_sampler) resolve_all(instance);
        return (PFN_vkVoidFunction)wrap_CreateSampler;
    }
    if (!g_real_gipa)
        return 0;
    PFN_vkVoidFunction out = g_real_gipa(instance, name);
    if (getenv("VKSER_DEBUG") && g_log) {
        fprintf(g_log, "gipa inst=%p %s -> %p\n", (void *)instance, name, (void *)out);
        fflush(g_log);
    }
    return out;
}

/* The Vulkan loader obtains the ICD's vkGetInstanceProcAddr via
   vk_icdGetInstanceProcAddr(NULL, "vkGetInstanceProcAddr") and then uses *that*
   pointer for every subsequent function lookup (instance and device level).
   To intercept device-level pipeline creation we therefore have to wrap this
   function, otherwise our serialization is silently bypassed. */
static PFN_vkVoidFunction VKAPI_PTR wrap_GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (name) {
        PFN_vkVoidFunction mine = wrap_device_function(name);
        if (mine) {
            if (getenv("VKSER_DEBUG") && g_log) {
                fprintf(g_log, "gipa2 inst=%p %s -> WRAPPED\n", (void *)instance, name);
                fflush(g_log);
            }
            return mine;
        }
    }
    if (g_real_get_instance_proc_addr)
        return g_real_get_instance_proc_addr(instance, name);
    return 0;
}

/* The modern loader-ICD interface (v3+) resolves device-level functions through
   vk_icdGetPhysicalDeviceProcAddr, so we must intercept the pipeline-creation
   entry points here as well, otherwise our serialization is bypassed. */
static PFN_vkVoidFunction wrap_device_function(const char *name) {
    if (strcmp(name, "vkCreateGraphicsPipelines") == 0) {
        if (!g_real_create_graphics_pipelines)
            g_real_create_graphics_pipelines = (PFN_CreateGraphicsPipelines)g_real_gipa(0, "vkCreateGraphicsPipelines");
        return (PFN_vkVoidFunction)wrap_CreateGraphicsPipelines;
    }
    if (strcmp(name, "vkCreateComputePipelines") == 0) {
        if (!g_real_create_compute_pipelines)
            g_real_create_compute_pipelines = (PFN_CreateComputePipelines)g_real_gipa(0, "vkCreateComputePipelines");
        return (PFN_vkVoidFunction)wrap_CreateComputePipelines;
    }
    if (strcmp(name, "vkCreateShaderModule") == 0) {
        if (!g_real_create_shader_module)
            g_real_create_shader_module = (PFN_CreateShaderModule)g_real_gipa(0, "vkCreateShaderModule");
        return (PFN_vkVoidFunction)wrap_CreateShaderModule;
    }
    if (strcmp(name, "vkCreatePipelineLayout") == 0) {
        if (!g_real_create_pipeline_layout)
            g_real_create_pipeline_layout = (PFN_CreatePipelineLayout)g_real_gipa(0, "vkCreatePipelineLayout");
        return (PFN_vkVoidFunction)wrap_CreatePipelineLayout;
    }
    if (strcmp(name, "vkCreateDescriptorSetLayout") == 0) {
        if (!g_real_create_descriptor_set_layout)
            g_real_create_descriptor_set_layout = (PFN_CreateDescriptorSetLayout)g_real_gipa(0, "vkCreateDescriptorSetLayout");
        return (PFN_vkVoidFunction)wrap_CreateDescriptorSetLayout;
    }
    if (strcmp(name, "vkCreateSampler") == 0) {
        if (!g_real_create_sampler)
            g_real_create_sampler = (PFN_CreateSampler)g_real_gipa(0, "vkCreateSampler");
        return (PFN_vkVoidFunction)wrap_CreateSampler;
    }
    return 0;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance_t instance, const char *name) {
    if (!g_real_gpdpa)
        return 0;
    /* Interface v6: the loader obtains vkGetInstanceProcAddr through this entry
       point too, so wrap it here as well to stay in the dispatch chain. */
    if (name && strcmp(name, "vkGetInstanceProcAddr") == 0 && g_real_get_instance_proc_addr) {
        if (getenv("VKSER_DEBUG") && g_log) {
            fprintf(g_log, "gpdpa inst=%p vkGetInstanceProcAddr -> WRAPPED\n", (void *)instance);
            fflush(g_log);
        }
        return (PFN_vkVoidFunction)wrap_GetInstanceProcAddr;
    }
    PFN_vkVoidFunction mine = name ? wrap_device_function(name) : 0;
    if (mine) {
        if (getenv("VKSER_DEBUG") && g_log) {
            fprintf(g_log, "gpdpa inst=%p %s -> WRAPPED\n", (void *)instance, name);
            fflush(g_log);
        }
        return mine;
    }
    PFN_vkVoidFunction out = g_real_gpdpa(instance, name);
    if (getenv("VKSER_DEBUG") && g_log) {
        fprintf(g_log, "gpdpa inst=%p %s -> %p\n", (void *)instance, name, (void *)out);
        fflush(g_log);
    }
    return out;
}
