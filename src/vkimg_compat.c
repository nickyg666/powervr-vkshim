/*
 * vkimg_compat.c - reverse-engineered compatibility shim for the proprietary
 * Imagination Technologies PowerVR B-Series Vulkan ICD (libVK_IMG.so).
 *
 * The driver is closed source and ships only as a binary blob
 * (xserver-xorg-img-bxm / libVK_IMG.so.24.2.6603887).  Its SPIR-V front end
 * rejects some shaders that are perfectly legal per the Vulkan spec, and its
 * internal shader-cache state machine can wedge afterwards, leaving the
 * application stuck inside vkAcquireNextImageKHR.
 *
 * This shim sits between the Vulkan loader and the real driver and
 *
 *   1. captures every shader module's SPIR-V so failures can be analysed,
 *   2. detects failing vkCreateGraphicsPipelines calls and retries them with
 *      rewritten SPIR-V (dead debug info stripped, OpKill downgraded to
 *      OpReturn, gl_FragDepth writes removed) and, as a last resort, with a
 *      synthesised minimal fragment shader,
 *   3. watches the swapchain acquire/present path so a wedged driver does not
 *      hang the render thread unnoticed.
 *
 * It is installed as a drop-in replacement ICD, therefore *every* call the
 * application makes passes through it.  The critical detail is that the loader
 * resolves device level functions through vkGetDeviceProcAddr, so that entry
 * point has to be intercepted as well - otherwise the wrappers are bypassed.
 *
 * Build:  gcc -O2 -fPIC -shared -o libvkimg_compat.so vkimg_compat.c -ldl -lpthread
 *
 * Environment:
 *   VKC_DEBUG=1        verbose logging
 *   VKC_REPAIR=0       disable pipeline repair (diagnostics only)
 *   VKC_DIR=<dir>      where to put logs / SPIR-V dumps (default /tmp/vkcompat)
 */

#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <time.h>

#include "minimal_fs.h"
#include "pvr_spirv_descriptors.h"
#include "pvr_texelbuf.h"
#include "pvr_copysec.h"

/* ------------------------------------------------------------------ */
/* crash handler - tells us exactly which driver call killed us        */
/* ------------------------------------------------------------------ */

static const char *g_last_op = "(none)";
static int g_crashfd = -1;
static struct sigaction g_old_segv, g_old_bus;

/* async signal safe crash reporter - writes straight to a file, because the
   application's stderr is usually swallowed by the launcher */
static void segv_handler(int sig, siginfo_t *si, void *ctx)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "\n*** signal %d in shim, addr=%p, last driver op: %s ***\n", sig,
                     si ? si->si_addr : NULL, g_last_op ? g_last_op : "(none)");
    if (g_crashfd >= 0) {
        ssize_t ignored = write(g_crashfd, buf, (size_t)n);
        (void)ignored;
    }
    void *bt[32];
    int cnt = backtrace(bt, 32);
    char **syms = backtrace_symbols(bt, cnt);
    if (syms) {
        for (int i = 0; i < cnt && i < 24; i++) {
            n = snprintf(buf, sizeof(buf), "    #%d %s\n", i, syms[i]);
            if (g_crashfd >= 0) {
                ssize_t ignored = write(g_crashfd, buf, (size_t)n);
                (void)ignored;
            }
        }
        free(syms);
    }
    /* chain to the previous handler (the JVM installs its own) */
    if (sig == SIGSEGV && g_old_segv.sa_flags & SA_SIGINFO)
        g_old_segv.sa_sigaction(sig, si, ctx);
    else if (sig == SIGSEGV && g_old_segv.sa_handler && g_old_segv.sa_handler != SIG_DFL &&
             g_old_segv.sa_handler != SIG_IGN)
        g_old_segv.sa_handler(sig);
    signal(sig, SIG_DFL);
    raise(sig);
}


/* ------------------------------------------------------------------ */
/* Vulkan layer plumbing                                               */
/*                                                                     */
/* The shim is installed as a Vulkan *layer*, not as a replacement ICD: */
/* the loader builds the device dispatch table from the layer chain,  */
/* so every device level call the application makes passes through us.  */
/* An ICD wrapper cannot do that - the loader resolves the device      */
/* functions from the ICD's own vkGetDeviceProcAddr and never reaches  */
/* the wrapper.                                                         */
/* ------------------------------------------------------------------ */

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static VkPhysicalDevice g_pd = VK_NULL_HANDLE;
/* atlas dump via worker thread (avoids allocating during command recording) */
static VkBuffer     g_atlas_dbg = VK_NULL_HANDLE;
static VkDeviceMemory g_atlas_dbgmem = VK_NULL_HANDLE;
static uint8_t     *g_atlas_ptr = NULL;
static VkImage      g_atlas_img = VK_NULL_HANDLE;
static VkImageLayout g_atlas_lay = VK_IMAGE_LAYOUT_UNDEFINED;
static int          g_atlas_done = 0;
static PFN_vkLayerCreateDevice next_layer_create_device;
static VkInstance layer_instance;

#define LAYER_NEGOTIATE_INTERFACE_STRUCT ((VkStructureType)37)

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface *pVersionStruct)
{
    if (!pVersionStruct || (uint32_t)pVersionStruct->sType != (uint32_t)LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion > 2)
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    next_gipa = pVersionStruct->pfnGetInstanceProcAddr;
    next_gdpa = pVersionStruct->pfnGetDeviceProcAddr;
    return VK_SUCCESS;
}

/* Find a VkLayerInstanceCreateInfo with the given function id in a chain. */
static const VkLayerInstanceCreateInfo *find_instance_info(const void *pNext, VkLayerFunction fn)
{
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO)
            continue;
        const VkLayerInstanceCreateInfo *li = (const VkLayerInstanceCreateInfo *)p;
        if (li->function == fn)
            return li;
    }
    return NULL;
}

/* Find a VkLayerDeviceCreateInfo with the given function id in a chain. */
static const VkLayerDeviceCreateInfo *find_device_info(const void *pNext, VkLayerFunction fn)
{
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
            continue;
        const VkLayerDeviceCreateInfo *ld = (const VkLayerDeviceCreateInfo *)p;
        if (ld->function == fn)
            return ld;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static FILE *g_log;
static pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_in_child;
static int g_debug;
static char g_dir[256] = "/tmp/vkcompat";

static void log_open(void)
{
    if (g_log)
        return;
    const char *env = getenv("VKC_DIR");
    if (env && *env)
        snprintf(g_dir, sizeof(g_dir), "%s", env);
    mkdir(g_dir, 0755);
    char p[512];
    snprintf(p, sizeof(p), "%s/shim.log", g_dir);
    g_log = fopen(p, "a");
    g_debug = getenv("VKC_DEBUG") ? 1 : 0;
}

static void logv(const char *fmt, ...)
{
    if (g_in_child)
        return;
    log_open();
    if (!g_log)
        return;
    va_list ap;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    pthread_mutex_lock(&g_log_mtx);
    fprintf(g_log, "%02d:%02d:%02d.%03d[%d] ", (int)(ts.tv_sec / 3600 % 24),
            (int)(ts.tv_sec / 60 % 60), (int)(ts.tv_sec % 60), (int)(ts.tv_nsec / 1000000),
            (int)getpid());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    pthread_mutex_unlock(&g_log_mtx);
}

#define LOG(...) logv(__VA_ARGS__)
#define DBG(...)                                                                                   \
    do {                                                                                           \
        if (g_debug)                                                                              \
            logv(__VA_ARGS__);                                                                     \
    } while (0)

/* ------------------------------------------------------------------ */
/* real driver handles                                                 */
/* ------------------------------------------------------------------ */

static void *g_drv;
static PFN_vkVoidFunction (*g_drv_gipa)(void *, const char *);
static PFN_vkVoidFunction (*g_drv_gdpa)(VkDevice, const char *);
static PFN_vkVoidFunction (*g_drv_gpdpa)(void *, const char *);
typedef VkResult(VKAPI_PTR *PFN_negver)(uint32_t *pVersion);
static PFN_negver g_drv_negver;

static VkInstance g_inst;
static VkDevice g_dev;

/*
 * The driver's vk_icdGetPhysicalDeviceProcAddr dereferences the instance
 * without a NULL check, so it must never be called with a NULL instance.
 * Device level functions are therefore only looked up once an instance
 * exists; global functions are resolved with a NULL instance.
 */
static PFN_vkVoidFunction dev_lookup(const char *name)
{
    if (!g_inst)
        return NULL;
    if (g_drv_gipa) {
        PFN_vkVoidFunction f = g_drv_gipa(g_inst, name);
        if (f)
            return f;
    }
    if (g_drv_gpdpa) {
        PFN_vkVoidFunction f = g_drv_gpdpa(g_inst, name);
        if (f)
            return f;
    }
    return NULL;
}

static PFN_vkVoidFunction drv_lookup(const char *name)
{
    if (next_gipa && g_inst) {
        PFN_vkVoidFunction f = next_gipa(g_inst, name);
        if (f)
            return f;
    }
    PFN_vkVoidFunction f = dev_lookup(name);
    if (f)
        return f;
    if (g_drv_gipa) {
        f = g_drv_gipa(VK_NULL_HANDLE, name);
        if (f)
            return f;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* real pointers for everything we wrap                                */
/* ------------------------------------------------------------------ */

static PFN_vkCreateInstance real_CreateInstance;
static PFN_vkDestroyInstance real_DestroyInstance;
static PFN_vkCreateDevice real_CreateDevice;
static PFN_vkDestroyDevice real_DestroyDevice;
static PFN_vkCreateShaderModule real_CreateShaderModule;
static PFN_vkCreateDescriptorSetLayout real_CreateDescriptorSetLayout;
static PFN_vkCreatePipelineLayout real_CreatePipelineLayout;
static PFN_vkCreateBufferView real_CreateBufferView;
static PFN_vkCmdPushDescriptorSetKHR real_CmdPushDescriptorSetKHR;
static PFN_vkUpdateDescriptorSets real_UpdateDescriptorSets;
static PFN_vkCreateImage real_CreateImage;
static PFN_vkCmdCopyBufferToImage real_CmdCopyBufferToImage;
static PFN_vkCreateImageView real_CreateImageView;
static PFN_vkCreateSampler real_CreateSampler;
static unsigned g_patched_seq;

/*
 * The application creates a texel buffer view for the cloud data and hands it
 * to the driver as a UNIFORM_TEXEL_BUFFER descriptor.  The driver does not
 * support that, so the descriptor is turned into a storage buffer instead -
 * the buffer, offset and range stay exactly the same, we only need to know
 * them when the descriptor is written.
 */
struct bufview_info {
    VkBufferView handle;
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
};
static struct bufview_info g_bufviews[512];
static int g_nbufviews;
static int g_bufview_next;
static pthread_mutex_t g_bufview_mtx = PTHREAD_MUTEX_INITIALIZER;

static const struct bufview_info *bufview_find(VkBufferView h)
{
    for (int i = 0; i < g_nbufviews; i++)
        if (g_bufviews[i].handle == h)
            return &g_bufviews[i];
    return NULL;
}

/* the application creates a new buffer view every frame, so the table is a
 * ring buffer */
static void bufview_add(VkBufferView h, VkBuffer buffer, VkDeviceSize offset,
                        VkDeviceSize range)
{
    int i = g_bufview_next % (int)(sizeof(g_bufviews) / sizeof(g_bufviews[0]));
    g_bufviews[i].handle = h;
    g_bufviews[i].buffer = buffer;
    g_bufviews[i].offset = offset;
    g_bufviews[i].range = range;
    g_bufview_next++;
    if (g_nbufviews < (int)(sizeof(g_bufviews) / sizeof(g_bufviews[0])))
        g_nbufviews++;
}
static PFN_vkUpdateDescriptorSets real_UpdateDescriptorSets;
static PFN_vkCreateImage real_CreateImage;
static PFN_vkCmdCopyBufferToImage real_CmdCopyBufferToImage;
static PFN_vkCreateImageView real_CreateImageView;
static PFN_vkCreateSampler real_CreateSampler;
static unsigned g_patched_seq;
static PFN_vkDestroyShaderModule real_DestroyShaderModule;
static PFN_vkCreateGraphicsPipelines real_CreateGraphicsPipelines;
static PFN_vkDestroyPipeline real_DestroyPipeline;
static PFN_vkCreateComputePipelines real_CreateComputePipelines;
static PFN_vkAcquireNextImageKHR real_AcquireNextImageKHR;
static PFN_vkQueuePresentKHR real_QueuePresentKHR;
static PFN_vkCreateSwapchainKHR real_CreateSwapchainKHR;
static PFN_vkDestroySwapchainKHR real_DestroySwapchainKHR;

/* global (instance independent) entry points */
static void resolve_global(void)
{
    if (real_CreateInstance)
        return;
    if (!g_drv_gipa)
        return;
#ifdef AS_LAYER
    real_CreateInstance = next_gipa ? (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance") : NULL;
#else
    real_CreateInstance = (PFN_vkCreateInstance)g_drv_gipa(VK_NULL_HANDLE, "vkCreateInstance");
#endif
    real_DestroyInstance = (PFN_vkDestroyInstance)g_drv_gipa(VK_NULL_HANDLE, "vkDestroyInstance");
}

/* device level entry points - requires a valid instance */
static void resolve_device(void)
{
    if (real_CreateShaderModule)
        return;
#ifdef AS_LAYER
    /* the device level functions can only be resolved once the device
     * exists; before that there is nothing to do */
    if (!g_dev)
        return;
    if (next_gdpa) {
        real_CreateShaderModule = (PFN_vkCreateShaderModule)next_gdpa(g_dev, "vkCreateShaderModule");
        real_DestroyShaderModule = (PFN_vkDestroyShaderModule)next_gdpa(g_dev, "vkDestroyShaderModule");
        real_CreateGraphicsPipelines =
            (PFN_vkCreateGraphicsPipelines)next_gdpa(g_dev, "vkCreateGraphicsPipelines");
        real_DestroyPipeline = (PFN_vkDestroyPipeline)next_gdpa(g_dev, "vkDestroyPipeline");
        real_CreateComputePipelines =
            (PFN_vkCreateComputePipelines)next_gdpa(g_dev, "vkCreateComputePipelines");
        real_CreatePipelineLayout = (PFN_vkCreatePipelineLayout)next_gdpa(g_dev, "vkCreatePipelineLayout");
        real_CreateDescriptorSetLayout =
            (PFN_vkCreateDescriptorSetLayout)next_gdpa(g_dev, "vkCreateDescriptorSetLayout");
        real_UpdateDescriptorSets =
            (PFN_vkUpdateDescriptorSets)next_gdpa(g_dev, "vkUpdateDescriptorSets");
        real_CreateBufferView = (PFN_vkCreateBufferView)next_gdpa(g_dev, "vkCreateBufferView");
        real_CmdPushDescriptorSetKHR =
            (PFN_vkCmdPushDescriptorSetKHR)next_gdpa(g_dev, "vkCmdPushDescriptorSetKHR");
        real_CreateImage = (PFN_vkCreateImage)next_gdpa(g_dev, "vkCreateImage");
        real_CmdCopyBufferToImage =
            (PFN_vkCmdCopyBufferToImage)next_gdpa(g_dev, "vkCmdCopyBufferToImage");
        real_CreateImageView = (PFN_vkCreateImageView)next_gdpa(g_dev, "vkCreateImageView");
        real_CreateSampler = (PFN_vkCreateSampler)next_gdpa(g_dev, "vkCreateSampler");
        real_AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)next_gdpa(g_dev, "vkAcquireNextImageKHR");
        real_QueuePresentKHR = (PFN_vkQueuePresentKHR)next_gdpa(g_dev, "vkQueuePresentKHR");
        real_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)next_gdpa(g_dev, "vkCreateSwapchainKHR");
        real_DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)next_gdpa(g_dev, "vkDestroySwapchainKHR");
        return;
    }
#endif
    if (next_gdpa && g_dev) {
        real_CreateShaderModule = (PFN_vkCreateShaderModule)next_gdpa(g_dev, "vkCreateShaderModule");
        real_DestroyShaderModule = (PFN_vkDestroyShaderModule)next_gdpa(g_dev, "vkDestroyShaderModule");
        real_CreateGraphicsPipelines =
            (PFN_vkCreateGraphicsPipelines)next_gdpa(g_dev, "vkCreateGraphicsPipelines");
        real_DestroyPipeline = (PFN_vkDestroyPipeline)next_gdpa(g_dev, "vkDestroyPipeline");
        real_CreateComputePipelines =
            (PFN_vkCreateComputePipelines)next_gdpa(g_dev, "vkCreateComputePipelines");
        real_CreatePipelineLayout = (PFN_vkCreatePipelineLayout)next_gdpa(g_dev, "vkCreatePipelineLayout");
        real_CreateDescriptorSetLayout =
            (PFN_vkCreateDescriptorSetLayout)next_gdpa(g_dev, "vkCreateDescriptorSetLayout");
        real_UpdateDescriptorSets =
            (PFN_vkUpdateDescriptorSets)next_gdpa(g_dev, "vkUpdateDescriptorSets");
        real_CreateBufferView = (PFN_vkCreateBufferView)next_gdpa(g_dev, "vkCreateBufferView");
        real_CmdPushDescriptorSetKHR =
            (PFN_vkCmdPushDescriptorSetKHR)next_gdpa(g_dev, "vkCmdPushDescriptorSetKHR");
        real_CreateImage = (PFN_vkCreateImage)next_gdpa(g_dev, "vkCreateImage");
        real_CmdCopyBufferToImage =
            (PFN_vkCmdCopyBufferToImage)next_gdpa(g_dev, "vkCmdCopyBufferToImage");
        real_CreateImageView = (PFN_vkCreateImageView)next_gdpa(g_dev, "vkCreateImageView");
        real_CreateSampler = (PFN_vkCreateSampler)next_gdpa(g_dev, "vkCreateSampler");
        real_AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)next_gdpa(g_dev, "vkAcquireNextImageKHR");
        real_QueuePresentKHR = (PFN_vkQueuePresentKHR)next_gdpa(g_dev, "vkQueuePresentKHR");
        real_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)next_gdpa(g_dev, "vkCreateSwapchainKHR");
        real_DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)next_gdpa(g_dev, "vkDestroySwapchainKHR");
        return;
    }
#ifdef AS_LAYER
    if (next_gipa)
        real_CreateDevice = (PFN_vkCreateDevice)next_gipa(layer_instance, "vkCreateDevice");
#endif
    if (!real_CreateDevice)
        real_CreateDevice = (PFN_vkCreateDevice)dev_lookup("vkCreateDevice");
    if (!real_CreateDevice)
        real_CreateDevice = (PFN_vkCreateDevice)dev_lookup("vkCreateDevice");
    real_DestroyDevice = (PFN_vkDestroyDevice)dev_lookup("vkDestroyDevice");
    real_CreateShaderModule = (PFN_vkCreateShaderModule)dev_lookup("vkCreateShaderModule");
    real_CreateDescriptorSetLayout = (PFN_vkCreateDescriptorSetLayout)dev_lookup("vkCreateDescriptorSetLayout");
    real_UpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)dev_lookup("vkUpdateDescriptorSets");
    real_CreateBufferView = (PFN_vkCreateBufferView)dev_lookup("vkCreateBufferView");
    real_CreateImage = (PFN_vkCreateImage)dev_lookup("vkCreateImage");
    real_CmdCopyBufferToImage =
        (PFN_vkCmdCopyBufferToImage)dev_lookup("vkCmdCopyBufferToImage");
    real_CreateImageView = (PFN_vkCreateImageView)dev_lookup("vkCreateImageView");
    real_CreateSampler = (PFN_vkCreateSampler)dev_lookup("vkCreateSampler");
    real_CmdPushDescriptorSetKHR =
        (PFN_vkCmdPushDescriptorSetKHR)dev_lookup("vkCmdPushDescriptorSetKHR");
    real_CreatePipelineLayout = (PFN_vkCreatePipelineLayout)dev_lookup("vkCreatePipelineLayout");
    real_DestroyShaderModule = (PFN_vkDestroyShaderModule)dev_lookup("vkDestroyShaderModule");
    real_CreateGraphicsPipelines =
        (PFN_vkCreateGraphicsPipelines)dev_lookup("vkCreateGraphicsPipelines");
    real_DestroyPipeline = (PFN_vkDestroyPipeline)dev_lookup("vkDestroyPipeline");
    real_CreateComputePipelines =
        (PFN_vkCreateComputePipelines)dev_lookup("vkCreateComputePipelines");
    real_AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)dev_lookup("vkAcquireNextImageKHR");
    real_QueuePresentKHR = (PFN_vkQueuePresentKHR)dev_lookup("vkQueuePresentKHR");
    real_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)dev_lookup("vkCreateSwapchainKHR");
    real_DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)dev_lookup("vkDestroySwapchainKHR");
}

/* ------------------------------------------------------------------ */
/* shader module bookkeeping                                           */
/* ------------------------------------------------------------------ */

struct spv_entry {
    struct spv_entry *next;
    VkShaderModule mod;
    uint32_t *code;
    size_t words;
    int stage;
    int oit; /* the shader belongs to Minecraft's order independent transparency pass */
    char file[512];
    char src[192];
    unsigned seq;
};

static struct spv_entry *g_spv_head;
static unsigned g_spv_seq;
static pthread_mutex_t g_spv_mtx = PTHREAD_MUTEX_INITIALIZER;

static const char *stage_name(int s)
{
    switch (s) {
    case 1: return "vertex";
    case 2: return "tess-control";
    case 3: return "tess-eval";
    case 4: return "geometry";
    case 5: return "fragment";
    case 6: return "compute";
    default: return "unknown";
    }
}

static int spv_entry_point_stage(const uint32_t *w, size_t n)
{
    size_t i = 5; /* skip the 5 word SPIR-V header */
    while (i < n) {
        uint32_t word = w[i];
        uint32_t op = word & 0xffffu;
        uint32_t len = word >> 16;
        if (!len || i + len > n)
            break;
        if (op == 15 /* OpEntryPoint */ && len >= 4) {
            switch (w[i + 1]) {
            case 0: return 1; /* Vertex */
            case 1: return 2; /* TessellationControl */
            case 2: return 3; /* TessellationEvaluation */
            case 3: return 4; /* Geometry */
            case 4: return 5; /* Fragment */
            case 5: return 6; /* GLCompute */
            default: return 0;
            }
        }
        i += len;
    }
    return 0;
}

static void spv_dump(struct spv_entry *e)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/spv", g_dir);
    mkdir(dir, 0755);
    snprintf(e->file, sizeof(e->file), "%s/spv/%04u-%s.spv", g_dir, e->seq, stage_name(e->stage));
    FILE *f = fopen(e->file, "wb");
    if (!f)
        return;
    fwrite(e->code, 4, e->words, f);
    fclose(f);
}

/* Extract the GLSL "file name" glslang embeds via OpString, e.g.
   "minecraft:core/clouds" - used to identify which pipeline failed. */
static void spv_source_name(const uint32_t *w, size_t n, char *out, size_t outsz)
{
    out[0] = 0;
    size_t i = 5;
    while (i < n) {
        uint32_t op = w[i] & 0xffffu;
        uint32_t len = w[i] >> 16;
        if (!len || i + len > n)
            break;
        if (op == 7 /* OpString */ && len >= 3) {
            /* word 1 = string literal, word 2 = id */
            const char *p = (const char *)&w[i + 1];
            size_t maxlen = (len - 1) * 4;
            size_t l = 0;
            while (l < maxlen && p[l])
                l++;
            if (l && l < outsz) {
                memcpy(out, p, l);
                out[l] = 0;
            }
            return;
        }
        i += len;
    }
}

/* Minecraft compiles its order independent transparency passes from shaders
 * that include "minecraft:oit.glsl".  The PowerVR driver cannot compile
 * those, so they get patched before the driver ever sees them. */
/* does the shader contain an OpKill (discard)? */
#define OP_KILL 252

static int spv_has_kill(const uint32_t *code, size_t words)
{
    size_t i = 5;
    while (i < words) {
        uint32_t w = code[i];
        uint32_t op = w & 0xffffu;
        uint32_t len = w >> 16;
        if (!len || i + len > words)
            break;
        if (op == OP_KILL)
            return 1;
        i += len;
    }
    return 0;
}

static int spv_is_oit(const uint32_t *code, size_t words)
{
    const char needle[] = "minecraft:oit";
    size_t nlen = sizeof(needle) - 1;
    size_t total = words * 4;
    const char *bytes = (const char *)code;
    for (size_t i = 0; i + nlen <= total; i++) {
        if (bytes[i] == 'm' && memcmp(bytes + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

static struct spv_entry *spv_find(VkShaderModule m)
{
    for (struct spv_entry *e = g_spv_head; e; e = e->next)
        if (e->mod == m)
            return e;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* minimal SPIR-V rewriter                                             */
/* ------------------------------------------------------------------ */

#define SPIRV_MAGIC 0x07230203u

enum {
    OP_SOURCE_CONTINUED = 2,
    OP_SOURCE = 3,
    OP_NAME = 5,
    OP_MEMBER_NAME = 6,
    OP_STRING = 7,
    OP_LINE = 8,
    OP_ENTRY_POINT = 15,
    OP_STORE = 62,
    OP_DECORATE = 71,
    OP_LABEL = 248,
    OP_VARIABLE = 59,
    OP_TYPE_VECTOR = 23,
    OP_TYPE_POINTER = 32,
    OP_CONSTANT = 43,
    OP_CONSTANT_COMPOSITE = 44,
    OP_SELECTION_MERGE = 247,
    SC_OUTPUT = 3,
    OP_BRANCH = 249,
    OP_FUNCTION = 54,
    OP_FUNCTION_END = 56,
    OP_RETURN = 253,
    OP_NO_LINE = 317,
    OP_MODULE_PROCESSED = 330,
};

#define DECORATION_BUILTIN 11
#define BUILTIN_FRAGDEPTH 22

struct spv_stats {
    int kills;
    int fragdepth_stores;
    int debug_instrs;
};

static uint32_t *spv_rewrite(const uint32_t *in, size_t nwords, int strip_debug, int kill_to_ret,
                             int drop_fdepth, struct spv_stats *stats, size_t *out_words)
{
    uint32_t fragdepth_var = 0;

    memset(stats, 0, sizeof(*stats));

    if (drop_fdepth) {
        size_t i = 5; /* skip the 5 word SPIR-V header */
        while (i < nwords) {
            uint32_t word = in[i];
            uint32_t op = word & 0xffffu;
            uint32_t len = word >> 16;
            if (!len || i + len > nwords)
                break;
            if (op == OP_DECORATE && len >= 4 && in[i + 2] == DECORATION_BUILTIN &&
                in[i + 3] == BUILTIN_FRAGDEPTH) {
                fragdepth_var = in[i + 1];
                break;
            }
            i += len;
        }
    }

    /*
     * The driver does not implement OpKill (discard).  Rewriting it to an
     * early OpReturn leaves the fragment output *undefined* (the driver then
     * renders garbage), and branching to the merge block of the surrounding
     * selection makes the driver's shader compiler produce garbage as well.
     *
     * What works: write a fully transparent colour to the fragment output and
     * return.  With alpha blending that is exactly what a discard does.
     *
     * So we need the fragment output variable and a zero constant of its type.
     */
    uint32_t out_var = 0, out_type = 0, out_comp = 0, out_count = 0, zero_comp = 0;
    if (kill_to_ret) {
        size_t i = 5;
        while (i < nwords) {
            uint32_t word = in[i];
            uint32_t op = word & 0xffffu;
            uint32_t len = word >> 16;
            if (!len || i + len > nwords)
                break;
            if (op == OP_VARIABLE && len >= 4 && in[i + 3] == SC_OUTPUT) {
                out_var = in[i + 2];
                /* the variable's type is an OpTypePointer */
                uint32_t ptr = in[i + 1];
                size_t j = 5;
                while (j < nwords) {
                    uint32_t w2 = in[j];
                    uint32_t o2 = w2 & 0xffffu;
                    uint32_t l2 = w2 >> 16;
                    if (!l2 || j + l2 > nwords)
                        break;
                    if (o2 == OP_TYPE_POINTER && l2 >= 4 && in[j + 1] == ptr) {
                        out_type = in[j + 3];
                        break;
                    }
                    j += l2;
                }
                break;
            }
            i += len;
        }
        /* the output type has to be a vector of scalars */
        if (out_type) {
            size_t j = 5;
            while (j < nwords) {
                uint32_t w2 = in[j];
                uint32_t o2 = w2 & 0xffffu;
                uint32_t l2 = w2 >> 16;
                if (!l2 || j + l2 > nwords)
                    break;
                if (o2 == OP_TYPE_VECTOR && l2 >= 4 && in[j + 1] == out_type) {
                    out_comp = in[j + 2];
                    out_count = in[j + 3];
                    break;
                }
                j += l2;
            }
        }
        /* a zero constant of the component type */
        if (out_comp) {
            size_t j = 5;
            while (j < nwords) {
                uint32_t w2 = in[j];
                uint32_t o2 = w2 & 0xffffu;
                uint32_t l2 = w2 >> 16;
                if (!l2 || j + l2 > nwords)
                    break;
                if (o2 == OP_CONSTANT && l2 >= 4 && in[j + 1] == out_comp && in[j + 3] == 0) {
                    zero_comp = in[j + 2];
                    break;
                }
                j += l2;
            }
        }
    }

    uint32_t new_bound = (nwords >= 5) ? in[3] : 1;
    uint32_t zero_vec = 0;
    int have_zero_vec = 0;

    size_t cap = nwords + 64;
    uint32_t *out = malloc(cap * 4);
    if (!out)
        return NULL;
    size_t o = 0;
    if (out_words)
        *out_words = 0;

    /* copy the 5 word SPIR-V header verbatim (with the new id bound) */
    if (nwords >= 5) {
        memcpy(out, in, 5 * 4);
        o = 5;
    }

    size_t i = 5; /* skip the 5 word SPIR-V header */
    int inserted_const = 0;
    while (i < nwords) {
        uint32_t word = in[i];
        uint32_t op = word & 0xffffu;
        uint32_t len = word >> 16;
        if (!len || i + len > nwords) {
            memcpy(out + o, in + i, (nwords - i) * 4);
            o += nwords - i;
            break;
        }

        if (op == OP_KILL)
            stats->kills++;

        int skip = 0;
        if (strip_debug) {
            switch (op) {
            case OP_NAME:
            case OP_MEMBER_NAME:
            case OP_STRING:
            case OP_SOURCE:
            case OP_SOURCE_CONTINUED:
            case OP_LINE:
            case OP_NO_LINE:
            case OP_MODULE_PROCESSED:
                stats->debug_instrs++;
                skip = 1;
                break;
            default:
                break;
            }
        }

        if (!skip && drop_fdepth && op == OP_STORE && len >= 3 && fragdepth_var &&
            in[i + 1] == fragdepth_var) {
            stats->fragdepth_stores++;
            skip = 1;
        }

        /* the zero constant for the fragment output goes into the constants */
        if (!inserted_const && op == OP_FUNCTION && out_var && out_type && out_comp &&
            zero_comp && out_count > 0 && out_count <= 4) {
            if (o + 3 + out_count + 4 > cap) {
                cap = (o + 2 + out_count + 4) * 2;
                uint32_t *tmp = realloc(out, cap * 4);
                if (!tmp) {
                    free(out);
                    return NULL;
                }
                out = tmp;
            }
            zero_vec = new_bound++;
            out[o++] = ((uint32_t)(3 + out_count) << 16) | OP_CONSTANT_COMPOSITE;
            out[o++] = out_type;
            out[o++] = zero_vec;
            for (uint32_t c = 0; c < out_count; c++)
                out[o++] = zero_comp;
            out[3] = new_bound;
            inserted_const = 1;
            have_zero_vec = 1;
        }

        if (skip) {
            i += len;
            continue;
        }

        if (o + len > cap) {
            cap = (o + len) * 2;
            uint32_t *tmp = realloc(out, cap * 4);
            if (!tmp) {
                free(out);
                return NULL;
            }
            out = tmp;
        }
        memcpy(out + o, in + i, len * 4);
        if (kill_to_ret && op == OP_KILL) {
            if (have_zero_vec) {
                /* OpStore %out %zeroVec ; OpReturn */
                if (o + 4 > cap) {
                    cap = (o + 4) * 2;
                    uint32_t *tmp = realloc(out, cap * 4);
                    if (!tmp) {
                        free(out);
                        return NULL;
                    }
                    out = tmp;
                }
                out[o++] = (3u << 16) | OP_STORE;
                out[o++] = out_var;
                out[o++] = zero_vec;
                out[o++] = (1u << 16) | OP_RETURN;
            } else {
                /* no output variable found: fall back to an early return */
                out[o] = OP_RETURN | (len << 16);
                o += len;
            }
            i += len;
            continue;
        }
        o += len;
        i += len;
    }

    if (out_words)
        *out_words = o;
    return out;
}

/* ------------------------------------------------------------------ */
/* pipeline inspection helpers                                         */
/* ------------------------------------------------------------------ */

static const char *ci_stage_name(const VkShaderModuleCreateInfo *ci)
{
    return "shader";
}

static const VkPipelineRenderingCreateInfo *find_rendering(const VkGraphicsPipelineCreateInfo *ci)
{
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)ci->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
            return (const VkPipelineRenderingCreateInfo *)p;
    }
    return NULL;
}

static void dump_pipeline_info(const VkGraphicsPipelineCreateInfo *ci)
{
    if (!ci)
        return;
    LOG("  stageCount=%u flags=0x%x", ci->stageCount, ci->flags);
    for (uint32_t i = 0; i < ci->stageCount; i++) {
        const VkPipelineShaderStageCreateInfo *s = &ci->pStages[i];
        struct spv_entry *e;
        pthread_mutex_lock(&g_spv_mtx);
        e = spv_find(s->module);
        pthread_mutex_unlock(&g_spv_mtx);
        LOG("    stage%u: flags=0x%x stage=0x%x module=%p name=%s src=%s spv=%s", i, s->flags,
            s->stage, (void *)s->module, s->pName ? s->pName : "(null)",
            e ? (e->src[0] ? e->src : "(no src)") : "(unknown)", e ? e->file : "(unknown)");
    }
    if (ci->pColorBlendState) {
        const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;
        LOG("    blend: attachments=%u logicOpEnable=%d logicOp=0x%x", cb->attachmentCount,
            cb->logicOpEnable, cb->logicOp);
        for (uint32_t i = 0; i < cb->attachmentCount && i < 8; i++)
            LOG("      att%u: blendEnable=%d srcColor=0x%x dstColor=0x%x colorOp=0x%x srcA=0x%x dstA=0x%x alphaOp=0x%x mask=0x%x",
                i, cb->pAttachments[i].blendEnable, cb->pAttachments[i].srcColorBlendFactor,
                cb->pAttachments[i].dstColorBlendFactor, cb->pAttachments[i].colorBlendOp,
                cb->pAttachments[i].srcAlphaBlendFactor, cb->pAttachments[i].dstAlphaBlendFactor,
                cb->pAttachments[i].alphaBlendOp, cb->pAttachments[i].colorWriteMask);
    }
    if (ci->pDepthStencilState) {
        const VkPipelineDepthStencilStateCreateInfo *ds = ci->pDepthStencilState;
        LOG("    depth: test=%d write=%d compare=0x%x bounds=%d stencil=%d", ds->depthTestEnable,
            ds->depthWriteEnable, ds->depthCompareOp, ds->depthBoundsTestEnable,
            ds->stencilTestEnable);
    }
    if (ci->pRasterizationState) {
        const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
        LOG("    raster: polygon=0x%x cull=0x%x front=0x%x discard=%d depthClamp=%d lineWidth=%f",
            rs->polygonMode, rs->cullMode, rs->frontFace, rs->rasterizerDiscardEnable,
            rs->depthClampEnable, rs->lineWidth);
    }
    if (ci->pMultisampleState) {
        const VkPipelineMultisampleStateCreateInfo *ms = ci->pMultisampleState;
        LOG("    msaa: samples=0x%x alphaToCoverage=%d alphaToOne=%d", ms->rasterizationSamples,
            ms->alphaToCoverageEnable, ms->alphaToOneEnable);
    }
    if (ci->pVertexInputState) {
        const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
        LOG("    vertexInput: bindings=%u attributes=%u topology=0x%x", vi->vertexBindingDescriptionCount,
            vi->vertexAttributeDescriptionCount,
            ci->pInputAssemblyState ? ci->pInputAssemblyState->topology : 0);
        for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++)
            LOG("      binding%u stride=%u inputRate=%u", vi->pVertexBindingDescriptions[i].binding,
                vi->pVertexBindingDescriptions[i].stride, vi->pVertexBindingDescriptions[i].inputRate);
        for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++)
            LOG("      attr%u loc=%u binding=%u fmt=%d offset=%u", i,
                vi->pVertexAttributeDescriptions[i].location,
                vi->pVertexAttributeDescriptions[i].binding,
                vi->pVertexAttributeDescriptions[i].format,
                vi->pVertexAttributeDescriptions[i].offset);
    }
    if (ci->pViewportState) {
        const VkPipelineViewportStateCreateInfo *vp = ci->pViewportState;
        LOG("    viewport: count=%u scissorCount=%u", vp->viewportCount, vp->scissorCount);
    }
    const VkPipelineRenderingCreateInfo *pr = find_rendering(ci);
    if (pr) {
        LOG("    rendering: colorAttachments=%u depthFormat=0x%x stencilFormat=0x%x viewMask=0x%x",
            pr->colorAttachmentCount, pr->depthAttachmentFormat, pr->stencilAttachmentFormat,
            pr->viewMask);
        for (uint32_t i = 0; i < pr->colorAttachmentCount && i < 8; i++)
            LOG("      colorFmt%u=0x%x", i,
                pr->pColorAttachmentFormats ? pr->pColorAttachmentFormats[i] : 0);
    }
}




/* generic timeout wrapper for the driver calls the repair path makes */
struct call_ctx {
    int (*fn)(void *, void *, void *, void *);
    void *a0;
    void *a1;
    void *a2;
    void *a3;
    int res;
    volatile int done;
};

/* returns 1 when the call finished, 0 on timeout */
/*
 * The driver can wedge (not crash, just hang) after a failed pipeline
 * compilation.  Run every attempt in a helper thread and give up on it after
 * a timeout so a wedged driver cannot freeze the game.
 */
struct pipe_call {
    VkDevice dev;
    VkPipelineCache cache;
    uint32_t count;
    const VkGraphicsPipelineCreateInfo *ci;
    const VkAllocationCallbacks *alloc;
    VkPipeline *out;
    VkResult res;
    volatile int done;
};

/* returns 1 when the call finished (and *res is valid), 0 on timeout */
/* ------------------------------------------------------------------ */
/* wrapped entry points                                                */
/* ------------------------------------------------------------------ */

static int g_repair = 1;
static int g_probe;
static int g_retry = 12;

/*
 * The PowerVR shader/pipeline compiler is not thread safe and Minecraft
 * creates its pipelines from a pool of worker threads.  Serialising the
 * driver's compilation calls avoids crashes inside libVK_IMG.so.
 */
static pthread_mutex_t g_drv_lock;
static pthread_once_t g_drv_lock_once = PTHREAD_ONCE_INIT;

static void drv_lock_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_drv_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void drv_lock(void)
{
    pthread_once(&g_drv_lock_once, drv_lock_init);
    pthread_mutex_lock(&g_drv_lock);
}

static void drv_unlock(void)
{
    pthread_mutex_unlock(&g_drv_lock);
}

/*
 * Repairs that are known to work for a given shader.  The PowerVR pipeline
 * compiler rejects several perfectly legal pipeline configurations, and the
 * exact trigger differs per shader, so the working fix is remembered per
 * shader module.
 */
struct fix_entry {
    struct fix_entry *next;
    VkShaderModule orig_fs;  /* the original fragment shader module */
    VkShaderModule repl_fs;  /* rewritten fragment shader (OpKill -> OpReturn) */
    int no_blend;            /* disable blending on all colour attachments */
    int no_depth;            /* disable depth test / write */
    int discard_raster;      /* rasterizerDiscardEnable */
    int add_blend_op;        /* replace MINIMUM/MAXIMUM blend ops with ADD */
    int fmt16;               /* replace 32 bit float colour formats with 16 bit */
    VkPipelineLayout layout; /* a pipeline layout that matches the shaders */
};

static struct fix_entry *g_fixes;

/* remember the descriptor set layouts and pipeline layouts so a failing
 * pipeline can be attributed to a mismatching descriptor layout */
struct dsl_info {
    struct dsl_info *next;
    VkDescriptorSetLayout h;
    uint32_t count;
    VkDescriptorType types[16];
    uint32_t bindings[16];
};
static struct dsl_info *g_dsls;

struct lay_info {
    struct lay_info *next;
    VkPipelineLayout h;
    uint32_t sets;
    VkDescriptorSetLayout dsl[8];
    uint32_t pushCount;
    VkPushConstantRange push[8];
};
static struct lay_info *g_lays;

/* cache of the corrected pipeline layouts */
struct fix_layout {
    struct fix_layout *next;
    VkPipelineLayout orig;
    VkPipelineLayout fixed;
    uint32_t signature;
};
static void remember_dsl(VkDescriptorSetLayout h, const VkDescriptorSetLayoutCreateInfo *ci)
{
    if (!ci || !ci->pBindings || ci->bindingCount > 16)
        return;
    /* the application creates and destroys layouts, so replace an entry with
     * the same handle instead of adding a second one */
    for (struct dsl_info *d = g_dsls; d; d = d->next) {
        if (d->h != h)
            continue;
        d->count = ci->bindingCount;
        for (uint32_t i = 0; i < ci->bindingCount; i++) {
            d->types[i] = (uint32_t)ci->pBindings[i].descriptorType;
            d->bindings[i] = ci->pBindings[i].binding;
        }
        return;
    }
    struct dsl_info *d = calloc(1, sizeof(*d));
    if (!d)
        return;
    d->h = h;
    d->count = ci->bindingCount;
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        d->types[i] = (uint32_t)ci->pBindings[i].descriptorType;
        d->bindings[i] = ci->pBindings[i].binding;
    }
    d->next = g_dsls;
    g_dsls = d;
}

static void remember_layout(VkPipelineLayout h, const VkPipelineLayoutCreateInfo *ci)
{
    struct lay_info *l = calloc(1, sizeof(*l));
    if (!l)
        return;
    l->h = h;
    l->sets = ci->setLayoutCount > 8 ? 8 : ci->setLayoutCount;
    for (uint32_t i = 0; i < l->sets; i++)
        l->dsl[i] = ci->pSetLayouts[i];
    l->pushCount = ci->pushConstantRangeCount > 8 ? 8 : ci->pushConstantRangeCount;
    for (uint32_t i = 0; i < l->pushCount; i++)
        l->push[i] = ci->pPushConstantRanges[i];
    l->next = g_lays;
    g_lays = l;
}


/* The OIT state fix can be disabled with VKC_OIT_FIX=0 for testing. */
static int oit_fix_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("VKC_OIT_FIX");
        cached = (e && e[0] == '0') ? 0 : 1;
    }
    return cached;
}

/* Does the app's descriptor set layout match what the shaders declare? */
static int layout_mismatches(VkPipelineLayout layout, const struct pvr_desc *desc, int ndesc,
                              char *why, size_t whyn)
{
#define MISMATCH(...) do { if (why) snprintf(why, whyn, __VA_ARGS__); } while (0)
    for (struct lay_info *l = g_lays; l; l = l->next) {
        if (l->h != layout)
            continue;
        for (int i = 0; i < ndesc; i++) {
            if (desc[i].set >= l->sets) {
                MISMATCH("shader uses set %d but the layout has only %d sets", desc[i].set, l->sets);
                return 1;
            }
            struct dsl_info *d = NULL;
            for (struct dsl_info *x = g_dsls; x; x = x->next)
                if (x->h == l->dsl[desc[i].set])
                    d = x;
            if (!d) {
                MISMATCH("the layout has no descriptor set layout for set %d", desc[i].set);
                return 1;
            }
            int found = 0;
            for (uint32_t b = 0; b < d->count; b++) {
                if (d->bindings[b] != desc[i].binding)
                    continue;
                found = 1;
                if (d->types[b] != desc[i].type) {
                    MISMATCH("binding %d: the layout says type %d, the shader says type %d",
                             desc[i].binding, d->types[b], desc[i].type);
                    return 1;
                }
            }
            if (!found) {
                MISMATCH("binding %d is missing from the layout", desc[i].binding);
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

/*
 * Build a descriptor set layout and a pipeline layout that match what the
 * shaders of a pipeline actually declare.  Minecraft's OIT pipelines use a
 * layout whose binding types do not match the shaders, and the PowerVR
 * pipeline compiler rejects those.
 */
static int module_is_oit(VkShaderModule m)
{
    int oit = 0;
    pthread_mutex_lock(&g_spv_mtx);
    struct spv_entry *e = spv_find(m);
    if (e)
        oit = e->oit;
    pthread_mutex_unlock(&g_spv_mtx);
    return oit;
}

static struct fix_entry *fix_find(VkShaderModule fs)
{
    for (struct fix_entry *f = g_fixes; f; f = f->next)
        if (f->orig_fs == fs)
            return f;
    return NULL;
}
static int g_strip_debug = 1;

/*
 * "Vulkan required" mode: the game silently falls back to OpenGL (llvmpipe)
 * when the Vulkan backend cannot be initialised.  With this mode enabled the
 * shim terminates the process instead, so the game can never end up running
 * on a software rasteriser.
 */
static int g_require_vulkan;
static VkResult VKAPI_CALL wrapped_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                  const VkAllocationCallbacks *pAllocator,
                                                  VkInstance *pInstance)
{
#ifdef AS_LAYER
    /*
     * Chain to the next layer.  The loader hands us
     *  - a VK_LAYER_LINK_INFO struct whose u.pLayerInfo points at the next
     *    layer's entry points, and
     *  - a VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK struct with the callback
     *    that has to be used to create the device.
     */
    {
        const VkLayerInstanceCreateInfo *link =
            find_instance_info(pCreateInfo->pNext, VK_LAYER_LINK_INFO);
        if (link && link->u.pLayerInfo) {
            next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
            LOG("layer: next gipa=%p", (void *)next_gipa);
        }
        const VkLayerInstanceCreateInfo *cb = find_instance_info(
            pCreateInfo->pNext, VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK);
        if (cb)
            next_layer_create_device = cb->u.layerDevice.pfnLayerCreateDevice;
    }
#endif
    resolve_global();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_CreateInstance)
        r = real_CreateInstance(pCreateInfo, pAllocator, pInstance);
    if (r == VK_SUCCESS && pInstance) {
        g_inst = *pInstance;
        layer_instance = *pInstance;
        resolve_device();
    }
    LOG("CreateInstance r=%d instance=%p", r, (void *)(pInstance ? *pInstance : NULL));
    return r;
}

static void VKAPI_CALL wrapped_DestroyInstance(VkInstance instance,
                                               const VkAllocationCallbacks *pAllocator)
{
    if (real_DestroyInstance)
        real_DestroyInstance(instance, pAllocator);
    g_inst = VK_NULL_HANDLE;
}

static VkResult VKAPI_CALL wrapped_CreateDevice(VkPhysicalDevice physicalDevice,
                                                const VkDeviceCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator,
                                                VkDevice *pDevice)
{
#ifdef AS_LAYER
    /*
     * The device chain link tells us the entry point of the next layer.  We
     * need it *before* the device is created, because the loader fills the
     * device dispatch table through our own vkGetDeviceProcAddr while the
     * device is being created.
     */
    {
        const VkLayerDeviceCreateInfo *link =
            find_device_info(pCreateInfo->pNext, VK_LAYER_LINK_INFO);
        if (link && link->u.pLayerInfo) {
            next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
            LOG("layer: next device gdpa=%p", (void *)next_gdpa);
        }
    }
    /*
     * The loader provides a callback that creates the device through the
     * layer chain and fills the device dispatch table - using it is what makes
     * the loader route the device level calls through this layer.
     */
    g_pd = physicalDevice;
    if (next_gipa) {
        PFN_vkCreateDevice next_create =
            (PFN_vkCreateDevice)next_gipa(layer_instance ? layer_instance : VK_NULL_HANDLE,
                                           "vkCreateDevice");
        if (next_create) {
            VkResult res = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);
            if (res == VK_SUCCESS && pDevice && *pDevice)
                g_dev = *pDevice;
            resolve_device();
            LOG("CreateDevice r=%d device=%p next_gdpa=%p", res, (void *)(pDevice ? *pDevice : NULL),
                (void *)next_gdpa);
            return res;
        }
    }
    LOG("CreateDevice: no vkCreateDevice in the chain");
    return VK_ERROR_INITIALIZATION_FAILED;
#endif
    resolve_device();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_CreateDevice)
        r = real_CreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (r == VK_SUCCESS && pDevice && *pDevice)
        g_dev = *pDevice;
    LOG("CreateDevice r=%d device=%p", r, (void *)(pDevice ? *pDevice : NULL));
    return r;
}

static int atlas_spawned;
static VkResult VKAPI_CALL wrapped_CreateShaderModule(VkDevice device,
                                                   const VkShaderModuleCreateInfo *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator,
                                                   VkShaderModule *pShaderModule)
{
    resolve_device();
    if (!atlas_spawned && getenv("VKC_DUMP_ATLAS") && g_dev && g_pd) {
        atlas_spawned = 1;
        extern void *atlas_worker(void *);
        pthread_t th;
        pthread_create(&th, NULL, atlas_worker, NULL);
        pthread_detach(th);
    }

    /*
     * The PowerVR pipeline compiler chokes on OpKill (discard) inside
     * Minecraft's order independent transparency shaders and kills the whole
     * process asynchronously.  Rewrite those shaders before the driver sees
     * them.
     */
    const uint32_t *code = pCreateInfo ? pCreateInfo->pCode : NULL;
    size_t words = (pCreateInfo && pCreateInfo->codeSize >= 4) ? pCreateInfo->codeSize / 4 : 0;
    int oit = code && words ? spv_is_oit(code, words) : 0;
    uint32_t *patched = NULL;
    size_t patched_words = 0;
    size_t tb_words = 0;
    /*
     * The driver's pipeline compiler does not implement OpKill (discard).
     * Every shader that uses one has to be rewritten, not just the OIT ones:
     * the GUI text shader uses a discard as well and the driver renders
     * garbage for the discarded fragments.
     */
    const char *nokill = getenv("VKC_NO_KILL_REWRITE");
    const char *killmode = getenv("VKC_KILL_REWRITE");
    if (killmode && killmode[0] == '1') {
        /* testing: rewrite the OpKill */
    } else if (nokill && nokill[0] == '1') {
        /* testing: leave the OpKill in the shader */
    } else if (code && words && spv_has_kill(code, words)) {
        struct spv_stats st1;
        patched = spv_rewrite(code, words, 0, 1, 0, &st1, &patched_words);
        if (patched && st1.kills) {
            LOG("%s shader: rewriting %d OpKill -> discard (%zu -> %zu words)",
                oit ? "OIT" : "plain", st1.kills, words, patched_words);
        } else {
            free(patched);
            patched = NULL;
            patched_words = 0;
        }
    }

    /*
     * The driver refuses to compile a pipeline whose descriptor is a
     * UNIFORM_TEXEL_BUFFER while the shader uses a buffer image.  Minecraft's
     * cloud shader does exactly that, so the buffer is moved to a storage
     * buffer here (the application still provides the same VkBuffer).
     */
    const char *notexel = getenv("VKC_NO_TEXEL");
    if (notexel && notexel[0] == '1') {
        /* testing: leave the texel buffer alone */
    } else if (patched) {
        uint32_t *tb = spv_texelbuf_to_storage(patched, patched_words, &tb_words);
        if (tb) {
            LOG("texel buffer shader: rewritten to a storage buffer (%zu -> %zu words)",
                patched_words, tb_words);
            free(patched);
            patched = tb;
            patched_words = tb_words;
        }
    } else if (code && words && !(notexel && notexel[0] == '1')) {
        uint32_t *tb = spv_texelbuf_to_storage(code, words, &tb_words);
        if (tb) {
            LOG("texel buffer shader: rewritten to a storage buffer (%zu -> %zu words)",
                words, tb_words);
            patched = tb;
            patched_words = tb_words;
        }
    }

    if (patched && getenv("VKC_DUMP_PATCHED")) {
        char fn[512], dir[512];
        snprintf(dir, sizeof(dir), "%s/patched", g_dir);
        mkdir(dir, 0755);
        snprintf(fn, sizeof(fn), "%s/patched/%04u-%s.spv", g_dir, ++g_patched_seq,
                 ci_stage_name(pCreateInfo));
        FILE *f = fopen(fn, "wb");
        if (f) {
            fwrite(patched, 4, patched_words, f);
            fclose(f);
        }
    }

    /*
     * The driver computes the implicit texture LOD wrongly, so every mipmapped
     * texture is sampled from a garbage level.  Force level 0.  This has to
     * happen for every shader, not just the ones that needed another fix.
     */
    if (code && words && getenv("VKC_LOD0") != NULL) {
        const uint32_t *src = patched ? patched : code;
        size_t src_words = patched ? patched_words : words;
        size_t lod_words = 0;
        uint32_t *lod = spv_force_lod0(src, src_words, &lod_words);
        if (lod) {
            LOG("shader: forcing the texture LOD to 0 (%zu -> %zu words)", src_words, lod_words);
            free(patched);
            patched = lod;
            patched_words = lod_words;
        }
    }

    VkShaderModuleCreateInfo ci = *pCreateInfo;
    if (patched) {
        ci.codeSize = patched_words * 4;
        ci.pCode = patched;
    }

    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    drv_lock();
    if (real_CreateShaderModule)
        r = real_CreateShaderModule(device, patched ? &ci : pCreateInfo, pAllocator, pShaderModule);
    drv_unlock();
    free(patched);
    for (int attempt = 0; attempt < g_retry && r != VK_SUCCESS; attempt++) {
        usleep(20000);
        drv_lock();
        if (real_CreateShaderModule)
            r = real_CreateShaderModule(device, patched ? &ci : pCreateInfo, pAllocator, pShaderModule);
        drv_unlock();
        if (r == VK_SUCCESS)
            LOG("CreateShaderModule: succeeded on retry %d", attempt + 1);
    }

    if (r == VK_SUCCESS && pShaderModule && *pShaderModule && pCreateInfo && pCreateInfo->pCode &&
        pCreateInfo->codeSize >= 20) {
        size_t words = pCreateInfo->codeSize / 4;
        struct spv_entry *e = calloc(1, sizeof(*e));
        if (e) {
            e->mod = *pShaderModule;
            e->words = words;
            e->code = malloc(words * 4);
            if (e->code) {
                memcpy(e->code, pCreateInfo->pCode, words * 4);
                e->stage = spv_entry_point_stage(e->code, words);
                e->oit = spv_is_oit(e->code, words);
                spv_source_name(e->code, words, e->src, sizeof(e->src));
                pthread_mutex_lock(&g_spv_mtx);
                e->seq = ++g_spv_seq;
                spv_dump(e);
                e->next = g_spv_head;
                g_spv_head = e;
                pthread_mutex_unlock(&g_spv_mtx);
                DBG("CreateShaderModule -> %p stage=%s spv=%s", (void *)*pShaderModule,
                    stage_name(e->stage), e->file);
            } else {
                free(e);
            }
        }
    }
    if (r != VK_SUCCESS)
        LOG("CreateShaderModule FAILED r=%d", r);
    return r;
}

static void VKAPI_CALL wrapped_DestroyShaderModule(VkDevice device, VkShaderModule m,
                                                    const VkAllocationCallbacks *a)
{
    pthread_mutex_lock(&g_spv_mtx);
    struct spv_entry **pp = &g_spv_head;
    while (*pp) {
        if ((*pp)->mod == m) {
            struct spv_entry *dead = *pp;
            *pp = dead->next;
            free(dead->code);
            free(dead);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_spv_mtx);
    if (real_DestroyShaderModule)
        real_DestroyShaderModule(device, m, a);
}

static VkShaderModule make_module(VkDevice device, const uint32_t *code, size_t words)
{
    if (!real_CreateShaderModule)
        return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = words * 4;
    ci.pCode = code;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = real_CreateShaderModule(device, &ci, NULL, &mod);
    if (r != VK_SUCCESS) {
        LOG("  repair: replacement CreateShaderModule failed r=%d", r);
        return VK_NULL_HANDLE;
    }
    return mod;
}

static VkResult VKAPI_CALL wrapped_CreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pSetLayout)
{
    resolve_device();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;

    /*
     * The driver does not support UNIFORM_TEXEL_BUFFER descriptors when the
     * shader uses a buffer image, so the descriptor becomes a storage buffer.
     * The shader is rewritten accordingly (see wrapped_CreateShaderModule)
     * and the descriptor write is translated in wrapped_CmdPushDescriptorSetKHR.
     */
    VkDescriptorSetLayoutBinding lb[64];
    VkDescriptorSetLayoutCreateInfo lci;
    int patched_layout = 0;
    if (pCreateInfo && pCreateInfo->bindingCount <= 64 && pCreateInfo->pBindings) {
        memcpy(lb, pCreateInfo->pBindings, pCreateInfo->bindingCount * sizeof(lb[0]));
        for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
            const char *notexel2 = getenv("VKC_NO_TEXEL");
            if (!(notexel2 && notexel2[0] == '1') &&
                lb[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
                lb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                patched_layout = 1;
            }
        }
    }
    if (patched_layout) {
        lci = *pCreateInfo;
        lci.pBindings = lb;
        if (real_CreateDescriptorSetLayout)
            r = real_CreateDescriptorSetLayout(device, &lci, pAllocator, pSetLayout);
        LOG("descriptor set layout: texel buffer descriptors turned into storage buffers");
    } else if (real_CreateDescriptorSetLayout) {
        r = real_CreateDescriptorSetLayout(device, pCreateInfo, pAllocator, pSetLayout);
    }
    if (r == VK_SUCCESS && pCreateInfo) {
        if (pSetLayout)
            remember_dsl(*pSetLayout, pCreateInfo);
        LOG("dsl %p bindings=%u:", (void *)(pSetLayout ? *pSetLayout : NULL), pCreateInfo->bindingCount);
        for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
            const VkDescriptorSetLayoutBinding *b = &pCreateInfo->pBindings[i];
            LOG("   binding=%u type=%d count=%u stages=0x%x", b->binding, b->descriptorType,
                b->descriptorCount, b->stageFlags);
        }
    }
    return r;
}

static void log_layout(VkPipelineLayout layout)
{
    if (!layout)
        return;
    LOG("  layout=%p", (void *)layout);
    for (struct lay_info *l = g_lays; l; l = l->next) {
        if (l->h != layout)
            continue;
        LOG("    sets=%u pushRanges=%u", l->sets, l->pushCount);
        for (uint32_t i = 0; i < l->sets; i++) {
            LOG("    set%u dsl=%p:", i, (void *)l->dsl[i]);
            for (struct dsl_info *d = g_dsls; d; d = d->next) {
                if (d->h != l->dsl[i])
                    continue;
                for (uint32_t b = 0; b < d->count; b++)
                    LOG("      binding %u type %d", d->bindings[b], d->types[b]);
            }
        }
        for (uint32_t i = 0; i < l->pushCount; i++)
            LOG("    push%u stages=0x%x off=%u size=%u", i, l->push[i].stageFlags,
                l->push[i].offset, l->push[i].size);
        break;
    }
}

static VkResult VKAPI_CALL wrapped_CreatePipelineLayout(
    VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout)
{
    resolve_device();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_CreatePipelineLayout)
        r = real_CreatePipelineLayout(device, pCreateInfo, pAllocator, pPipelineLayout);
    if (r == VK_SUCCESS && pCreateInfo && pPipelineLayout) {
        remember_layout(*pPipelineLayout, pCreateInfo);
        LOG("CreatePipelineLayout %p sets=%u pushRanges=%u", (void *)*pPipelineLayout,
            pCreateInfo->setLayoutCount, pCreateInfo->pushConstantRangeCount);
        for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; i++)
            LOG("   set%u dsl=%p", i, (void *)pCreateInfo->pSetLayouts[i]);
        for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; i++)
            LOG("   pcRange%u stages=0x%x offset=%u size=%u", i,
                pCreateInfo->pPushConstantRanges[i].stageFlags,
                pCreateInfo->pPushConstantRanges[i].offset, pCreateInfo->pPushConstantRanges[i].size);
    }
    return r;
}


static VkResult VKAPI_CALL wrapped_CreateGraphicsPipelines(VkDevice device,
                                                       VkPipelineCache pipelineCache,
                                                       uint32_t createInfoCount,
                                                       const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                                       const VkAllocationCallbacks *pAllocator,
                                                       VkPipeline *pPipelines)
{
    char why[192];
    resolve_device();
    if (!real_CreateGraphicsPipelines)
        return VK_ERROR_INITIALIZATION_FAILED;

    /*
     * Apply the fixes we already know work for these shaders, so we do not
     * have to fail (and retry) every single pipeline.
     */
    VkGraphicsPipelineCreateInfo *cis = NULL;
    VkPipelineShaderStageCreateInfo (*stage_copies)[8] = NULL;
    uint32_t nfix = 0;
    if (pCreateInfos && createInfoCount) {
        for (uint32_t i = 0; i < createInfoCount; i++) {
            const VkGraphicsPipelineCreateInfo *ci = &pCreateInfos[i];
            for (uint32_t st = 0; st < ci->stageCount; st++) {
                if (ci->pStages[st].stage != VK_SHADER_STAGE_FRAGMENT_BIT)
                    continue;
                if (fix_find(ci->pStages[st].module) || module_is_oit(ci->pStages[st].module))
                    nfix++;
            }
            /*
             * The PowerVR pipeline compiler rejects pipelines whose
             * descriptor set layout does not match the descriptors the
             * shaders declare.  Detect that here and use a corrected layout.
             */
            struct pvr_desc pd_[PVR_MAX_DESC];
            int npd = 0;
            for (uint32_t st = 0; st < ci->stageCount && npd < PVR_MAX_DESC; st++) {
                struct spv_entry *se;
                pthread_mutex_lock(&g_spv_mtx);
                se = spv_find(ci->pStages[st].module);
                pthread_mutex_unlock(&g_spv_mtx);
                if (!se)
                    continue;
                npd += pvr_parse_descriptors(se->code, se->words, &pd_[npd], PVR_MAX_DESC - npd);
            }
            if (npd && ci->layout && layout_mismatches(ci->layout, pd_, npd, why, sizeof(why))) {
                nfix++;
                LOG("pipeline %u: the descriptor layout does not match the shaders (%s)", i, why);
            }
            if (getenv("VKC_DUMP_PIPELINES")) {
                char mods[256] = "";
                for (uint32_t st = 0; st < ci->stageCount; st++) {
                    struct spv_entry *se;
                    pthread_mutex_lock(&g_spv_mtx);
                    se = spv_find(ci->pStages[st].module);
                    pthread_mutex_unlock(&g_spv_mtx);
                    char one[96] = "";
                    if (se)
                        snprintf(one, sizeof(one), " %s=%s", stage_name(se->stage),
                                 strrchr(se->file, '/') ? strrchr(se->file, '/') + 1 : "?");
                    strncat(mods, one, sizeof(mods) - strlen(mods) - 1);
                }
                LOG("pipeline %u: layout=%p shaders:%s", i, (void *)ci->layout, mods);
            }
        }
    }
    if (nfix) {
        cis = calloc(createInfoCount, sizeof(*cis));
        stage_copies = calloc(createInfoCount, sizeof(*stage_copies));
        if (cis && stage_copies) {
            memcpy(cis, pCreateInfos, createInfoCount * sizeof(*cis));
            for (uint32_t i = 0; i < createInfoCount; i++) {
                const VkGraphicsPipelineCreateInfo *ci = &pCreateInfos[i];
                if (ci->stageCount > 8)
                    continue;
                memcpy(stage_copies[i], ci->pStages,
                       sizeof(VkPipelineShaderStageCreateInfo) * ci->stageCount);
                /*
                 * OIT passes use 32 bit float colour attachments with
                 * blending.  The driver has neither the floatBlend feature nor
                 * support for the MINIMUM/MAXIMUM blend operations on them,
                 * so fall back to 16 bit float without blending.
                 */
                VkShaderModule oit_mod = VK_NULL_HANDLE;
                for (uint32_t st2 = 0; st2 < ci->stageCount; st2++)
                    if (ci->pStages[st2].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
                        oit_mod = ci->pStages[st2].module;
                        break;
                    }
                if (oit_mod != VK_NULL_HANDLE && module_is_oit(oit_mod) && oit_fix_enabled()) {
                    LOG("  applying the OIT state fix to pipeline %u", i);
                    if (ci->pColorBlendState && ci->pColorBlendState->attachmentCount <= 8) {
                        static VkPipelineColorBlendAttachmentState att[8];
                        static VkPipelineColorBlendStateCreateInfo cb;
                        cb = *ci->pColorBlendState;
                        memcpy(att, ci->pColorBlendState->pAttachments, sizeof(att));
                        for (uint32_t a = 0; a < cb.attachmentCount; a++) {
                            att[a].blendEnable = VK_FALSE;
                            if (att[a].colorBlendOp == (VkBlendOp)4 ||
                                att[a].colorBlendOp == (VkBlendOp)5)
                                att[a].colorBlendOp = VK_BLEND_OP_ADD;
                            if (att[a].alphaBlendOp == (VkBlendOp)4 ||
                                att[a].alphaBlendOp == (VkBlendOp)5)
                                att[a].alphaBlendOp = VK_BLEND_OP_ADD;
                        }
                        cb.pAttachments = att;
                        cis[i].pColorBlendState = &cb;
                    }
                    const VkPipelineRenderingCreateInfo *pr = find_rendering(ci);
                    if (pr && pr->colorAttachmentCount <= 8) {
                        static VkFormat fmts[8];
                        static VkPipelineRenderingCreateInfo prc;
                        prc = *pr;
                        for (uint32_t a = 0; a < pr->colorAttachmentCount; a++) {
                            fmts[a] = pr->pColorAttachmentFormats ? pr->pColorAttachmentFormats[a]
                                                                   : VK_FORMAT_UNDEFINED;
                            if (fmts[a] == VK_FORMAT_R32G32B32A32_SFLOAT)
                                fmts[a] = VK_FORMAT_R16G16B16A16_SFLOAT;
                            else if (fmts[a] == VK_FORMAT_R32_SFLOAT)
                                fmts[a] = VK_FORMAT_R16_SFLOAT;
                            else if (fmts[a] == VK_FORMAT_R32G32_SFLOAT)
                                fmts[a] = VK_FORMAT_R16G16_SFLOAT;
                        }
                        prc.pColorAttachmentFormats = fmts;
                        cis[i].pNext = &prc;
                    }
                    if (ci->pDepthStencilState) {
                        static VkPipelineDepthStencilStateCreateInfo ds;
                        ds = *ci->pDepthStencilState;
                        ds.depthWriteEnable = VK_FALSE;
                        cis[i].pDepthStencilState = &ds;
                    }
                }

                /* corrected layout when the app's layout does not match */
                struct pvr_desc pd_[PVR_MAX_DESC];
                int npd = 0;
                for (uint32_t st = 0; st < ci->stageCount && npd < PVR_MAX_DESC; st++) {
                    struct spv_entry *se;
                    pthread_mutex_lock(&g_spv_mtx);
                    se = spv_find(ci->pStages[st].module);
                    pthread_mutex_unlock(&g_spv_mtx);
                    if (!se)
                        continue;
                    npd += pvr_parse_descriptors(se->code, se->words, &pd_[npd], PVR_MAX_DESC - npd);
                }
                if (npd && ci->layout && layout_mismatches(ci->layout, pd_, npd, why, sizeof(why)))
                    LOG("  pipeline %u: the descriptor layout does not match the shaders (%s)", i, why);

                struct fix_entry *f = NULL;
                for (uint32_t st = 0; st < ci->stageCount; st++)
                    if (ci->pStages[st].stage == VK_SHADER_STAGE_FRAGMENT_BIT &&
                        (f = fix_find(ci->pStages[st].module)))
                        break;
                if (!f)
                    continue;   /* nothing else to apply for this pipeline */
                if (f->repl_fs)
                    for (uint32_t st = 0; st < ci->stageCount; st++)
                        if (ci->pStages[st].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                            stage_copies[i][st].module = f->repl_fs;
                cis[i].pStages = stage_copies[i];
                if (f->no_blend && ci->pColorBlendState &&
                    ci->pColorBlendState->attachmentCount <= 8) {
                    static VkPipelineColorBlendAttachmentState att[8];
                    static VkPipelineColorBlendStateCreateInfo cb;
                    cb = *ci->pColorBlendState;
                    memcpy(att, ci->pColorBlendState->pAttachments, sizeof(att));
                    for (uint32_t a = 0; a < cb.attachmentCount; a++)
                        att[a].blendEnable = VK_FALSE;
                    cb.pAttachments = att;
                    cis[i].pColorBlendState = &cb;
                }
                if (f->no_depth && ci->pDepthStencilState) {
                    static VkPipelineDepthStencilStateCreateInfo ds;
                    ds = *ci->pDepthStencilState;
                    ds.depthTestEnable = VK_FALSE;
                    ds.depthWriteEnable = VK_FALSE;
                    cis[i].pDepthStencilState = &ds;
                }
                if (f->add_blend_op && ci->pColorBlendState &&
                    ci->pColorBlendState->attachmentCount <= 8) {
                    static VkPipelineColorBlendAttachmentState att[8];
                    static VkPipelineColorBlendStateCreateInfo cb;
                    cb = *ci->pColorBlendState;
                    memcpy(att, ci->pColorBlendState->pAttachments, sizeof(att));
                    for (uint32_t a = 0; a < cb.attachmentCount; a++) {
                        if (att[a].colorBlendOp == (VkBlendOp)4 || att[a].colorBlendOp == (VkBlendOp)5)
                            att[a].colorBlendOp = VK_BLEND_OP_ADD;
                        if (att[a].alphaBlendOp == (VkBlendOp)4 || att[a].alphaBlendOp == (VkBlendOp)5)
                            att[a].alphaBlendOp = VK_BLEND_OP_ADD;
                    }
                    cb.pAttachments = att;
                    cis[i].pColorBlendState = &cb;
                }
                if (f->fmt16) {
                    const VkPipelineRenderingCreateInfo *pr = find_rendering(ci);
                    if (pr && pr->colorAttachmentCount <= 8) {
                        static VkFormat fmts[8];
                        static VkPipelineRenderingCreateInfo prc;
                        prc = *pr;
                        for (uint32_t a = 0; a < pr->colorAttachmentCount; a++) {
                            fmts[a] = pr->pColorAttachmentFormats ? pr->pColorAttachmentFormats[a]
                                                                   : VK_FORMAT_UNDEFINED;
                            if (fmts[a] == VK_FORMAT_R32G32B32A32_SFLOAT)
                                fmts[a] = VK_FORMAT_R16G16B16A16_SFLOAT;
                            else if (fmts[a] == VK_FORMAT_R32_SFLOAT)
                                fmts[a] = VK_FORMAT_R16_SFLOAT;
                            else if (fmts[a] == VK_FORMAT_R32G32_SFLOAT)
                                fmts[a] = VK_FORMAT_R16G16_SFLOAT;
                        }
                        prc.pColorAttachmentFormats = fmts;
                        cis[i].pNext = &prc;
                    }
                }
                if (f->discard_raster && ci->pRasterizationState) {
                    static VkPipelineRasterizationStateCreateInfo rs;
                    rs = *ci->pRasterizationState;
                    rs.rasterizerDiscardEnable = VK_TRUE;
                    cis[i].pRasterizationState = &rs;
                }
            }
        } else {
            free(cis);
            free(stage_copies);
            cis = NULL;
            stage_copies = NULL;
        }
    }

    if (getenv("VKC_LOG_LAYOUT") && pCreateInfos)
        for (uint32_t i = 0; i < createInfoCount; i++)
            LOG("pipeline %u layout=%p", i, (void *)pCreateInfos[i].layout);
    drv_lock();
    VkResult r = real_CreateGraphicsPipelines(device, pipelineCache, createInfoCount,
                                              cis ? cis : pCreateInfos, pAllocator, pPipelines);
    drv_unlock();
    free(cis);
    free(stage_copies);

    /*
     * The PowerVR shader/pipeline compiler is non-deterministic: the very
     * same pipeline sometimes fails with VK_ERROR_UNKNOWN and sometimes
     * succeeds.  Simply trying again is enough to get a valid pipeline.
     */
    for (int attempt = 0; attempt < g_retry && r != VK_SUCCESS; attempt++) {
        usleep(30000);
        drv_lock();
        r = real_CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos,
                                         pAllocator, pPipelines);
        drv_unlock();
        if (r == VK_SUCCESS)
            LOG("vkCreateGraphicsPipelines: succeeded on retry %d", attempt + 1);
    }
    if (r == VK_SUCCESS)
        return VK_SUCCESS;

    LOG("vkCreateGraphicsPipelines FAILED r=%d count=%u", r, createInfoCount);
    if (!g_repair || !pPipelines || !pCreateInfos)
        return r;

    for (uint32_t i = 0; i < createInfoCount; i++) {
        const VkGraphicsPipelineCreateInfo *ci = &pCreateInfos[i];
        LOG("  repairing pipeline %u", i);
        dump_pipeline_info(ci);
        log_layout(ci->layout);

        uint32_t fs_idx = UINT32_MAX;
        for (uint32_t st = 0; st < ci->stageCount; st++)
            if (ci->pStages[st].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                fs_idx = st;
        if (fs_idx == UINT32_MAX)
            continue;

        struct spv_entry *e;
        pthread_mutex_lock(&g_spv_mtx);
        e = spv_find(ci->pStages[fs_idx].module);
        pthread_mutex_unlock(&g_spv_mtx);
        if (!e)
            continue;

        /* the candidate fixes, in the order we try them */
        struct fix_entry cand[4];
        int ncand = 0;

        /* 1: rewrite OpKill -> OpReturn in the fragment shader */
        {
            struct spv_stats st1;
            size_t nw = 0;
            uint32_t *fixed = spv_rewrite(e->code, e->words, 0, 1, 0, &st1, &nw);
            if (fixed && st1.kills) {
                VkShaderModule m = make_module(device, fixed, nw);
                free(fixed);
                if (m) {
                    memset(&cand[ncand], 0, sizeof(cand[ncand]));
                    cand[ncand].orig_fs = ci->pStages[fs_idx].module;
                    cand[ncand].repl_fs = m;
                    ncand++;
                }
            } else {
                free(fixed);
            }
        }

        /*
         * 2: replace the fragment shader with a minimal stub that just
         * writes the colour attachments.  The application's own descriptor
         * layout is kept, so the descriptor sets stay compatible.
         */
        {
            int n_out = 1;
            const VkPipelineRenderingCreateInfo *pr = find_rendering(ci);
            if (pr && pr->colorAttachmentCount >= 1 && pr->colorAttachmentCount <= 8)
                n_out = (int)pr->colorAttachmentCount;
            size_t w = 0;
            const uint32_t *fs = minimal_fs_get(n_out, &w);
            if (fs) {
                VkShaderModule m = make_module(device, fs, w);
                if (m) {
                    memset(&cand[ncand], 0, sizeof(cand[ncand]));
                    cand[ncand].orig_fs = ci->pStages[fs_idx].module;
                    cand[ncand].repl_fs = m;
                    ncand++;
                }
            }
        }

        {
            struct spv_stats st1;
            size_t nw = 0;
            uint32_t *fixed = spv_rewrite(e->code, e->words, 0, 1, 0, &st1, &nw);
            if (fixed && st1.kills) {
                VkShaderModule m = make_module(device, fixed, nw);
                free(fixed);
                if (m) {
                    memset(&cand[ncand], 0, sizeof(cand[ncand]));
                    cand[ncand].orig_fs = ci->pStages[fs_idx].module;
                    cand[ncand].repl_fs = m;
                    ncand++;
                }
            } else {
                free(fixed);
            }
        }

        for (int c = 0; c < ncand; c++) {
            VkPipelineShaderStageCreateInfo stages[8];
            if (ci->stageCount > 8)
                break;
            memcpy(stages, ci->pStages, sizeof(VkPipelineShaderStageCreateInfo) * ci->stageCount);
            if (cand[c].repl_fs)
                stages[fs_idx].module = cand[c].repl_fs;

            VkGraphicsPipelineCreateInfo tmp = *ci;
            tmp.pStages = stages;
            if (cand[c].layout)
                tmp.layout = cand[c].layout;

            /* the state changes need their own copies */
            VkPipelineColorBlendAttachmentState att[8];
            VkPipelineColorBlendStateCreateInfo cb;
            VkPipelineDepthStencilStateCreateInfo ds;
            if (cand[c].no_blend && ci->pColorBlendState &&
                ci->pColorBlendState->attachmentCount <= 8) {
                cb = *ci->pColorBlendState;
                memcpy(att, ci->pColorBlendState->pAttachments, sizeof(att));
                for (uint32_t a = 0; a < cb.attachmentCount; a++)
                    att[a].blendEnable = VK_FALSE;
                cb.pAttachments = att;
                tmp.pColorBlendState = &cb;
            }
            if (cand[c].no_depth && ci->pDepthStencilState) {
                ds = *ci->pDepthStencilState;
                ds.depthTestEnable = VK_FALSE;
                ds.depthWriteEnable = VK_FALSE;
                tmp.pDepthStencilState = &ds;
            }
            if (cand[c].add_blend_op && ci->pColorBlendState &&
                ci->pColorBlendState->attachmentCount <= 8) {
                cb = *ci->pColorBlendState;
                memcpy(att, ci->pColorBlendState->pAttachments, sizeof(att));
                for (uint32_t a = 0; a < cb.attachmentCount; a++) {
                    if (att[a].colorBlendOp == (VkBlendOp)4 /* MINIMUM */ ||
                        att[a].colorBlendOp == (VkBlendOp)5 /* MAXIMUM */)
                        att[a].colorBlendOp = VK_BLEND_OP_ADD;
                    if (att[a].alphaBlendOp == (VkBlendOp)4 || att[a].alphaBlendOp == (VkBlendOp)5)
                        att[a].alphaBlendOp = VK_BLEND_OP_ADD;
                }
                cb.pAttachments = att;
                tmp.pColorBlendState = &cb;
            }
            if (cand[c].fmt16) {
                const VkPipelineRenderingCreateInfo *pr = find_rendering(ci);
                if (pr && pr->colorAttachmentCount <= 8) {
                    static VkFormat fmts[8];
                    static VkPipelineRenderingCreateInfo prc;
                    prc = *pr;
                    for (uint32_t a = 0; a < pr->colorAttachmentCount; a++) {
                        fmts[a] = pr->pColorAttachmentFormats ? pr->pColorAttachmentFormats[a]
                                                               : VK_FORMAT_UNDEFINED;
                        if (fmts[a] == VK_FORMAT_R32G32B32A32_SFLOAT)
                            fmts[a] = VK_FORMAT_R16G16B16A16_SFLOAT;
                        else if (fmts[a] == VK_FORMAT_R32_SFLOAT)
                            fmts[a] = VK_FORMAT_R16_SFLOAT;
                        else if (fmts[a] == VK_FORMAT_R32G32_SFLOAT)
                            fmts[a] = VK_FORMAT_R16G16_SFLOAT;
                    }
                    prc.pColorAttachmentFormats = fmts;
                    tmp.pNext = &prc;
                }
            }
            if (cand[c].discard_raster && ci->pRasterizationState) {
                static VkPipelineRasterizationStateCreateInfo rs;
                rs = *ci->pRasterizationState;
                rs.rasterizerDiscardEnable = VK_TRUE;
                tmp.pRasterizationState = &rs;
            }

            VkPipeline outpipe = VK_NULL_HANDLE;
            VkResult r2 = VK_ERROR_INITIALIZATION_FAILED;
            for (int attempt = 0; attempt < 4; attempt++) {
                drv_lock();
                r2 = real_CreateGraphicsPipelines(device, pipelineCache, 1, &tmp, pAllocator,
                                                 &outpipe);
                drv_unlock();
                if (r2 == VK_SUCCESS)
                    break;
                usleep(30000);
            }
            if (r2 == VK_SUCCESS) {
                LOG("  repair: SUCCESS with candidate %d (kill=%d noBlend=%d noDepth=%d)", c,
                    cand[c].repl_fs ? 1 : 0, cand[c].no_blend, cand[c].no_depth);
                /* remember the fix for this shader */
                struct fix_entry *f = fix_find(ci->pStages[fs_idx].module);
                if (!f) {
                    f = calloc(1, sizeof(*f));
                    if (f) {
                        f->next = g_fixes;
                        g_fixes = f;
                    }
                }
                if (f) {
                    f->orig_fs = ci->pStages[fs_idx].module;
                    f->repl_fs = cand[c].repl_fs;
                    f->no_blend = cand[c].no_blend;
                    f->no_depth = cand[c].no_depth;
                    f->discard_raster = cand[c].discard_raster;
                    f->add_blend_op = cand[c].add_blend_op;
                    f->fmt16 = cand[c].fmt16;
                }
                pPipelines[i] = outpipe;
                return VK_SUCCESS;
            }
            LOG("  repair: candidate %d failed r=%d", c, r2);
        }

        /* clean up the shader modules we created for the candidates */
        for (int c = 0; c < ncand; c++)
            if (cand[c].repl_fs && real_DestroyShaderModule)
                real_DestroyShaderModule(device, cand[c].repl_fs, NULL);
    }
    return r;
}

static VkResult VKAPI_CALL wrapped_CreateImageView(VkDevice device,
                                                   const VkImageViewCreateInfo *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator,
                                                   VkImageView *pView)
{
    resolve_device();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_CreateImageView)
        r = real_CreateImageView(device, pCreateInfo, pAllocator, pView);
    if (r == VK_SUCCESS && pCreateInfo && getenv("VKC_LOG_IMAGES"))
        LOG("image view %p: format=%d type=%d aspect=0x%x baseMip=%u levelCount=%u",
            (void *)(pView ? *pView : NULL), (int)pCreateInfo->format, (int)pCreateInfo->viewType,
            pCreateInfo->subresourceRange.aspectMask, pCreateInfo->subresourceRange.baseMipLevel,
            pCreateInfo->subresourceRange.levelCount);
    return r;
}

static VkResult VKAPI_CALL wrapped_CreateSampler(VkDevice device,
                                                const VkSamplerCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator,
                                                VkSampler *pSampler)
{
    resolve_device();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (getenv("VKC_CLAMP_LOD0")) {
        VkSamplerCreateInfo patched = *pCreateInfo;
        patched.minLod = 0.0f;
        patched.maxLod = 0.0f;
        patched.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        if (real_CreateSampler)
            r = real_CreateSampler(device, &patched, pAllocator, pSampler);
    } else if (real_CreateSampler)
        r = real_CreateSampler(device, pCreateInfo, pAllocator, pSampler);
    if (r == VK_SUCCESS && pCreateInfo && getenv("VKC_LOG_IMAGES"))
        LOG("sampler %p: mag=%d min=%d mipmapMode=%d minLod=%.2f maxLod=%.2f maxAniso=%f",
            (void *)(pSampler ? *pSampler : NULL), (int)pCreateInfo->magFilter,
            (int)pCreateInfo->minFilter, (int)pCreateInfo->mipmapMode,
            pCreateInfo->minLod, pCreateInfo->maxLod, pCreateInfo->maxAnisotropy);
    return r;
}

static void resolve_copyfns(void);

static void VKAPI_CALL wrapped_CmdCopyBufferToImage(VkCommandBuffer commandBuffer,
                                                    VkBuffer srcBuffer,
                                                    VkImage dstImage,
                                                    VkImageLayout dstImageLayout,
                                                    uint32_t regionCount,
                                                    const VkBufferImageCopy *pRegions)
{
    resolve_device();
    if (getenv("VKC_LOG_IMAGES") && pRegions) {
        for (uint32_t i = 0; i < regionCount; i++) {
            LOG("copy to image %p: %ux%u depth=%u srcOffset=%llu rowLength=%u imageHeight=%u "
                "imgOff=%u,%u,%u bufOff=%llu bufRow=%u bufImgH=%u",
                (void *)dstImage, pRegions[i].imageExtent.width, pRegions[i].imageExtent.height,
                pRegions[i].imageExtent.depth,
                (unsigned long long)pRegions[i].bufferOffset, pRegions[i].bufferRowLength,
                pRegions[i].bufferImageHeight,
                pRegions[i].imageOffset.x, pRegions[i].imageOffset.y, pRegions[i].imageOffset.z,
                (unsigned long long)pRegions[i].bufferOffset, pRegions[i].bufferRowLength,
                pRegions[i].bufferImageHeight);
        }
    }
    resolve_copyfns();
    static int atlas_seen;
    if (getenv("VKC_DUMP_ATLAS") && pRegions)
        for (uint32_t ri = 0; ri < regionCount; ri++)
            if (pRegions[ri].imageExtent.width == 1024 &&
                pRegions[ri].imageExtent.height == 1024 &&
                pRegions[ri].imageOffset.x == 0 && pRegions[ri].imageOffset.y == 0)
                atlas_seen++;
    int handled = cs_route_bti(commandBuffer, srcBuffer, dstImage, dstImageLayout,
                     regionCount, pRegions);
    if (handled && getenv("VKC_DUMP_ATLAS") && atlas_seen >= 6 && !g_atlas_done &&
        g_atlas_dbg && g_atlas_ptr) {
        PFN_vkCmdPipelineBarrier rq3 =
            (PFN_vkCmdPipelineBarrier)next_gdpa(g_dev, "vkCmdPipelineBarrier");
        VkImageMemoryBarrier b = {0};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = dstImageLayout;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.image = dstImage;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        rq3(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy rb = {0};
        rb.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rb.imageSubresource.layerCount = 1;
        rb.imageExtent.width = 1024;
        rb.imageExtent.height = 1024;
        rb.imageExtent.depth = 1;
        PFN_vkCmdCopyImageToBuffer rq4 =
            (PFN_vkCmdCopyImageToBuffer)next_gdpa(g_dev, "vkCmdCopyImageToBuffer");
        rq4(commandBuffer, dstImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_atlas_dbg, 1, &rb);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = dstImageLayout;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = 0;
        rq3(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        g_atlas_done = 1;
        LOG("atlas readback recorded");
    }
    if (handled)
        return;
    if (real_CmdCopyBufferToImage)
        real_CmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout,
                                 regionCount, pRegions);
}

/* ------------------------------------------------------------------ */
/* pvr_copysec wiring: all remaining copy/blit families are routed the  */
/* same way (see pvr_copysec.c), plus the command buffer state trackers */
/* ------------------------------------------------------------------ */
static PFN_vkCmdCopyImage real_CmdCopyImage;
static PFN_vkCmdCopyImageToBuffer real_CmdCopyImageToBuffer;
static PFN_vkCmdBlitImage real_CmdBlitImage;
static PFN_vkCmdCopyBufferToImage2 real_CmdCopyBufferToImage2;
static PFN_vkCmdCopyImageToBuffer2 real_CmdCopyImageToBuffer2;
static PFN_vkCmdCopyImage2 real_CmdCopyImage2;
static PFN_vkCmdBlitImage2 real_CmdBlitImage2;
static PFN_vkAllocateCommandBuffers real_AllocateCommandBuffers;
static PFN_vkFreeCommandBuffers real_FreeCommandBuffers;
static PFN_vkBeginCommandBuffer real_BeginCommandBuffer;
static PFN_vkResetCommandBuffer real_ResetCommandBuffer;
static PFN_vkResetCommandPool real_ResetCommandPool;
static PFN_vkCmdBeginRenderPass real_CmdBeginRenderPass;
static PFN_vkCmdEndRenderPass real_CmdEndRenderPass;
static PFN_vkCmdBeginRenderPass2 real_CmdBeginRenderPass2;
static PFN_vkCmdEndRenderPass2 real_CmdEndRenderPass2;

static void resolve_copyfns(void)
{
    if (!g_dev || real_CmdCopyImage)
        return;
#define RQ(name) \
    real_##name = (PFN_vk##name)(next_gdpa ? next_gdpa(g_dev, "vk" #name) : dev_lookup("vk" #name))
    RQ(CmdCopyImage);
    RQ(CmdCopyImageToBuffer);
    RQ(CmdBlitImage);
    RQ(CmdCopyBufferToImage2);
    RQ(CmdCopyImageToBuffer2);
    RQ(CmdCopyImage2);
    RQ(CmdBlitImage2);
    RQ(AllocateCommandBuffers);
    RQ(FreeCommandBuffers);
    RQ(BeginCommandBuffer);
    RQ(ResetCommandBuffer);
    RQ(ResetCommandPool);
    RQ(CmdBeginRenderPass);
    RQ(CmdEndRenderPass);
    RQ(CmdBeginRenderPass2);
    RQ(CmdEndRenderPass2);
#undef RQ
    if (next_gdpa)
        cs_bind(g_dev, next_gdpa);
    LOG("copysec: routing active");
}

static void VKAPI_CALL wrapped_CmdCopyImage(VkCommandBuffer cb, VkImage src,
                                            VkImageLayout sl, VkImage dst,
                                            VkImageLayout dl, uint32_t n,
                                            const VkImageCopy *r)
{
    resolve_device();
    resolve_copyfns();
    if (cs_route_copyimage(cb, src, sl, dst, dl, n, r))
        return;
    if (real_CmdCopyImage)
        real_CmdCopyImage(cb, src, sl, dst, dl, n, r);
}

static void VKAPI_CALL wrapped_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src,
                                                    VkImageLayout sl, VkBuffer dst,
                                                    uint32_t n,
                                                    const VkBufferImageCopy *r)
{
    resolve_device();
    resolve_copyfns();
    if (cs_route_itb(cb, src, sl, dst, n, r))
        return;
    if (real_CmdCopyImageToBuffer)
        real_CmdCopyImageToBuffer(cb, src, sl, dst, n, r);
}

static void VKAPI_CALL wrapped_CmdBlitImage(VkCommandBuffer cb, VkImage src,
                                            VkImageLayout sl, VkImage dst,
                                            VkImageLayout dl, uint32_t n,
                                            const VkImageBlit *r, VkFilter f)
{
    resolve_device();
    resolve_copyfns();
    if (cs_route_blit(cb, src, sl, dst, dl, n, r, f))
        return;
    if (real_CmdBlitImage)
        real_CmdBlitImage(cb, src, sl, dst, dl, n, r, f);
}

static void VKAPI_CALL wrapped_CmdCopyBufferToImage2(VkCommandBuffer cb,
                                                     const VkCopyBufferToImageInfo2 *i)
{
    resolve_device();
    resolve_copyfns();
    if (i && cs_route_bti2(cb, i->srcBuffer, i->dstImage, i->dstImageLayout,
                           i->regionCount, i->pRegions))
        return;
    if (real_CmdCopyBufferToImage2)
        real_CmdCopyBufferToImage2(cb, i);
}

static void VKAPI_CALL wrapped_CmdCopyImageToBuffer2(VkCommandBuffer cb,
                                                     const VkCopyImageToBufferInfo2 *i)
{
    resolve_device();
    resolve_copyfns();
    if (i && cs_route_itb2(cb, i->srcImage, i->srcImageLayout, i->dstBuffer,
                           i->regionCount, i->pRegions))
        return;
    if (real_CmdCopyImageToBuffer2)
        real_CmdCopyImageToBuffer2(cb, i);
}

static void VKAPI_CALL wrapped_CmdCopyImage2(VkCommandBuffer cb,
                                             const VkCopyImageInfo2 *i)
{
    resolve_device();
    resolve_copyfns();
    if (i && cs_route_copyimage2(cb, i->srcImage, i->srcImageLayout,
                                 i->dstImage, i->dstImageLayout,
                                 i->regionCount, i->pRegions))
        return;
    if (real_CmdCopyImage2)
        real_CmdCopyImage2(cb, i);
}

static void VKAPI_CALL wrapped_CmdBlitImage2(VkCommandBuffer cb,
                                             const VkBlitImageInfo2 *i)
{
    resolve_device();
    resolve_copyfns();
    if (i && cs_route_blit2(cb, i->srcImage, i->srcImageLayout, i->dstImage,
                            i->dstImageLayout, i->regionCount, i->pRegions,
                            i->filter))
        return;
    if (real_CmdBlitImage2)
        real_CmdBlitImage2(cb, i);
}

static VkResult VKAPI_CALL wrapped_AllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo,
    VkCommandBuffer *pCommandBuffers)
{
    resolve_device();
    resolve_copyfns();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_AllocateCommandBuffers)
        r = real_AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
    if (r == VK_SUCCESS)
        cs_track_alloc(device, pAllocateInfo, pCommandBuffers);
    return r;
}

static void VKAPI_CALL wrapped_FreeCommandBuffers(VkDevice device,
                                                  VkCommandPool pool,
                                                  uint32_t n,
                                                  const VkCommandBuffer *cbs)
{
    resolve_device();
    resolve_copyfns();
    cs_track_free(device, n, cbs);
    if (real_FreeCommandBuffers)
        real_FreeCommandBuffers(device, pool, n, cbs);
}

static VkResult VKAPI_CALL wrapped_BeginCommandBuffer(
    VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    resolve_device();
    resolve_copyfns();
    cs_track_begin(cb);
    if (g_atlas_done >= 1 && g_atlas_ptr) {
        FILE *f = fopen("/tmp/atlas.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n1024 1024\n255\n");
            for (int y = 1023; y >= 0; y--)
                fwrite(g_atlas_ptr + (size_t)y * 1024 * 4, 1, 1024 * 3, f);
            fclose(f);
        }
        g_atlas_done++;
        if (g_atlas_done == 2)
            LOG("atlas dumped to /tmp/atlas.ppm");
    }
    if (real_BeginCommandBuffer)
        return real_BeginCommandBuffer(cb, bi);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static void VKAPI_CALL wrapped_ResetCommandBuffer(VkCommandBuffer cb,
                                                  VkCommandBufferResetFlags f)
{
    resolve_device();
    resolve_copyfns();
    cs_track_reset(cb);
    if (real_ResetCommandBuffer)
        real_ResetCommandBuffer(cb, f);
}

static void VKAPI_CALL wrapped_ResetCommandPool(VkDevice device,
                                                VkCommandPool pool,
                                                VkCommandPoolResetFlags f)
{
    resolve_device();
    resolve_copyfns();
    cs_track_resetpool(device, pool);
    if (real_ResetCommandPool)
        real_ResetCommandPool(device, pool, f);
}

static void VKAPI_CALL wrapped_CmdBeginRenderPassTracking(
    VkCommandBuffer cb, const VkRenderPassBeginInfo *i, VkSubpassContents c)
{
    resolve_copyfns();
    cs_track_beginpass(cb);
    if (real_CmdBeginRenderPass)
        real_CmdBeginRenderPass(cb, i, c);
}

static void VKAPI_CALL wrapped_CmdEndRenderPassTracking(VkCommandBuffer cb)
{
    resolve_copyfns();
    cs_track_endpass(cb);
    if (real_CmdEndRenderPass)
        real_CmdEndRenderPass(cb);
}

static void VKAPI_CALL wrapped_CmdBeginRenderPass2Tracking(
    VkCommandBuffer cb, const VkRenderPassBeginInfo *i,
    const VkSubpassBeginInfo *i2)
{
    resolve_copyfns();
    cs_track_beginpass(cb);
    if (real_CmdBeginRenderPass2)
        real_CmdBeginRenderPass2(cb, i, i2);
}

static void VKAPI_CALL wrapped_CmdEndRenderPass2Tracking(
    VkCommandBuffer cb, const VkSubpassEndInfo *i)
{
    resolve_copyfns();
    cs_track_endpass(cb);
    if (real_CmdEndRenderPass2)
        real_CmdEndRenderPass2(cb, i);
}

static void VKAPI_CALL wrapped_DestroyDeviceHook(VkDevice device,
                                                 const VkAllocationCallbacks *a)
{
    resolve_device();
    resolve_copyfns();
    cs_destroy();
    if (real_DestroyDevice)
        real_DestroyDevice(device, a);
    g_dev = VK_NULL_HANDLE;
}

static VkResult VKAPI_CALL wrapped_CreateImage(VkDevice device,
                                                const VkImageCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator,
                                                VkImage *pImage)
{
    resolve_device();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    const VkImageCreateInfo *ci = pCreateInfo;
    VkImageCreateInfo patched;
    if (pCreateInfo && pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR &&
        (pCreateInfo->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
        patched = *pCreateInfo;
        patched.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci = &patched;
        LOG("image tiling forced optimal: %dx%d format=%d", pCreateInfo->extent.width,
            pCreateInfo->extent.height, (int)pCreateInfo->format);
    }
    if (real_CreateImage)
        r = real_CreateImage(device, ci, pAllocator, pImage);
    if (r == VK_SUCCESS && pCreateInfo && getenv("VKC_LOG_IMAGES"))
        LOG("image %p: %dx%d format=%d mips=%u layers=%u samples=%d tiling=%d usage=0x%x",
            (void *)(pImage ? *pImage : NULL), pCreateInfo->extent.width,
            pCreateInfo->extent.height, (int)pCreateInfo->format, pCreateInfo->mipLevels,
            pCreateInfo->arrayLayers, (int)pCreateInfo->samples, (int)pCreateInfo->tiling,
            pCreateInfo->usage);
    return r;
}

static VkResult VKAPI_CALL wrapped_CreateBufferView(VkDevice device,
                                                    const VkBufferViewCreateInfo *pCreateInfo,
                                                    const VkAllocationCallbacks *pAllocator,
                                                    VkBufferView *pBufferView)
{
    resolve_device();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_CreateBufferView)
        r = real_CreateBufferView(device, pCreateInfo, pAllocator, pBufferView);
    if (r == VK_SUCCESS && pBufferView && *pBufferView && pCreateInfo) {
        pthread_mutex_lock(&g_bufview_mtx);
        bufview_add(*pBufferView, pCreateInfo->buffer, pCreateInfo->offset,
                    pCreateInfo->range);
        pthread_mutex_unlock(&g_bufview_mtx);
        LOG("buffer view %p: buffer=%p offset=%llu range=%llu format=%d",
            (void *)*pBufferView, (void *)pCreateInfo->buffer,
            (unsigned long long)pCreateInfo->offset, (unsigned long long)pCreateInfo->range,
            (int)pCreateInfo->format);
    }
    return r;
}

/*
 * Rewrite the texel buffer descriptors of a write into storage buffer
 * descriptors.  The application hands us a VkBufferView; we look up the
 * buffer it was created from and describe the very same memory as a storage
 * buffer, which is what the patched shader expects.
 */
static void rewrite_texel_buffers(VkWriteDescriptorSet *w, uint32_t count,
                                  VkDescriptorBufferInfo *infos, VkBufferView *views)
{
    for (uint32_t i = 0; i < count; i++) {
        if (w[i].descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
            continue;
        VkBufferView h = w[i].pTexelBufferView ? *w[i].pTexelBufferView : VK_NULL_HANDLE;
        const struct bufview_info *bi;
        pthread_mutex_lock(&g_bufview_mtx);
        bi = bufview_find(h);
        if (bi) {
            infos[i].buffer = bi->buffer;
            infos[i].offset = bi->offset;
            infos[i].range = bi->range;
            views[i] = h;
        }
        pthread_mutex_unlock(&g_bufview_mtx);
        if (!bi) {
            LOG("push descriptor: unknown buffer view %p - leaving it alone", (void *)h);
            continue;
        }
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pImageInfo = NULL;
        w[i].pTexelBufferView = NULL;
        w[i].pBufferInfo = &infos[i];
        LOG("push descriptor: texel buffer %p -> storage buffer (buffer=%p offset=%llu range=%llu)",
            (void *)h, (void *)bi->buffer, (unsigned long long)bi->offset,
            (unsigned long long)bi->range);
    }
}

static void VKAPI_CALL wrapped_CmdPushDescriptorSetKHR(VkCommandBuffer commandBuffer,
                                                       uint32_t pipelineBindPoint,
                                                       VkPipelineLayout layout,
                                                       uint32_t set,
                                                       uint32_t descriptorWriteCount,
                                                       const VkWriteDescriptorSet *pDescriptorWrites)
{
    resolve_device();
    VkWriteDescriptorSet w[64];
    VkDescriptorBufferInfo infos[64];
    VkBufferView views[64];
    if (descriptorWriteCount <= 64 && pDescriptorWrites) {
        memcpy(w, pDescriptorWrites, descriptorWriteCount * sizeof(w[0]));
        memset(infos, 0, sizeof(infos));
        memset(views, 0, sizeof(views));
        rewrite_texel_buffers(w, descriptorWriteCount, infos, views);
    }
    if (real_CmdPushDescriptorSetKHR)
        real_CmdPushDescriptorSetKHR(commandBuffer, pipelineBindPoint, layout, set,
                                     descriptorWriteCount, w);
}

static void VKAPI_CALL wrapped_UpdateDescriptorSets(VkDevice device,
                                                    uint32_t descriptorWriteCount,
                                                    const VkWriteDescriptorSet *pDescriptorWrites,
                                                    uint32_t descriptorCopyCount,
                                                    const VkCopyDescriptorSet *pDescriptorCopies)
{
    resolve_device();
    for (uint32_t i = 0; i < descriptorWriteCount; i++) {
        const VkWriteDescriptorSet *w = &pDescriptorWrites[i];
        LOG("descriptor write: set=%u binding=%u array=%u count=%u type=%d image=%p sampler=%p bufview=%p",
            w->dstSet, w->dstBinding, w->dstArrayElement, w->descriptorCount,
            (int)w->descriptorType, (void *)w->pImageInfo,
            w->pImageInfo ? (void *)w->pImageInfo->sampler : NULL,
            (void *)w->pBufferInfo);
    }
    VkWriteDescriptorSet w[64];
    VkDescriptorBufferInfo infos[64];
    VkBufferView views[64];
    if (descriptorWriteCount <= 64 && pDescriptorWrites) {
        memcpy(w, pDescriptorWrites, descriptorWriteCount * sizeof(w[0]));
        memset(infos, 0, sizeof(infos));
        memset(views, 0, sizeof(views));
        rewrite_texel_buffers(w, descriptorWriteCount, infos, views);
        if (real_UpdateDescriptorSets)
            real_UpdateDescriptorSets(device, descriptorWriteCount, w, descriptorCopyCount,
                                      pDescriptorCopies);
        return;
    }
    if (real_UpdateDescriptorSets)
        real_UpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites,
                                  descriptorCopyCount, pDescriptorCopies);
}

static uint64_t now_ms(void);

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

static VkResult VKAPI_CALL wrapped_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                        uint64_t timeout, VkSemaphore semaphore,
                                                        VkFence fence, uint32_t *pImageIndex)
{
    resolve_device();
    uint64_t t0 = now_ms();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_AcquireNextImageKHR)
        r = real_AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    uint64_t dt = now_ms() - t0;
    if (r == VK_SUCCESS) {
        if (dt > 50)
            LOG("AcquireNextImageKHR slow: %llu ms", (unsigned long long)dt);
    } else {
        LOG("AcquireNextImageKHR r=%d after %llu ms", r, (unsigned long long)dt);
    }
    return r;
}

static VkResult VKAPI_CALL wrapped_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    resolve_device();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_QueuePresentKHR)
        r = real_QueuePresentKHR(queue, pPresentInfo);
    if (r != VK_SUCCESS)
        LOG("QueuePresentKHR r=%d", r);
    /*
     * Count the presented frames: this is the frame rate the application
     * actually achieves on the GPU (no need to inject an F3 keypress).
     */
    {
        static uint64_t frames;
        static uint64_t t_start;
        static uint64_t t_last;
        frames++;
        uint64_t t = now_ms();
        if (t_start == 0) {
            t_start = t;
            t_last = t;
        } else if (t - t_last >= 2000) {
            LOG("FPS: %.1f (%llu frames in %llu ms)", (double)frames * 1000.0 / (double)(t - t_start),
                (unsigned long long)frames, (unsigned long long)(t - t_start));
            frames = 0;
            t_start = t;
            t_last = t;
        }
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* dispatch interception                                               */
/* ------------------------------------------------------------------ */

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance,
                                                                         const char *name);

static PFN_vkVoidFunction intercept(const char *name)
{
    if (!name)
        return NULL;
    DBG("intercept query: %s", name);
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateInstance;
    if (strcmp(name, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)wrapped_DestroyInstance;
    if (strcmp(name, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateDevice;
    if (strcmp(name, "vkCreateGraphicsPipelines") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateGraphicsPipelines;
    if (strcmp(name, "vkCreatePipelineLayout") == 0)
        return (PFN_vkVoidFunction)wrapped_CreatePipelineLayout;
    if (strcmp(name, "vkCreateDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateDescriptorSetLayout;
    if (strcmp(name, "vkCreateShaderModule") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateShaderModule;
    if (strcmp(name, "vkUpdateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)wrapped_UpdateDescriptorSets;
    if (strcmp(name, "vkCreateBufferView") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateBufferView;
    if (strcmp(name, "vkCreateImage") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateImage;
    if (strcmp(name, "vkCmdCopyBufferToImage") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdCopyBufferToImage;
    if (strcmp(name, "vkCmdCopyImage") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdCopyImage;
    if (strcmp(name, "vkCmdCopyImageToBuffer") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdCopyImageToBuffer;
    if (strcmp(name, "vkCmdBlitImage") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdBlitImage;
    if (strcmp(name, "vkCmdCopyBufferToImage2") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdCopyBufferToImage2;
    if (strcmp(name, "vkCmdCopyImageToBuffer2") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdCopyImageToBuffer2;
    if (strcmp(name, "vkCmdCopyImage2") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdCopyImage2;
    if (strcmp(name, "vkCmdBlitImage2") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdBlitImage2;
    if (strcmp(name, "vkAllocateCommandBuffers") == 0)
        return (PFN_vkVoidFunction)wrapped_AllocateCommandBuffers;
    if (strcmp(name, "vkFreeCommandBuffers") == 0)
        return (PFN_vkVoidFunction)wrapped_FreeCommandBuffers;
    if (strcmp(name, "vkBeginCommandBuffer") == 0)
        return (PFN_vkVoidFunction)wrapped_BeginCommandBuffer;
    if (strcmp(name, "vkResetCommandBuffer") == 0)
        return (PFN_vkVoidFunction)wrapped_ResetCommandBuffer;
    if (strcmp(name, "vkResetCommandPool") == 0)
        return (PFN_vkVoidFunction)wrapped_ResetCommandPool;
    if (strcmp(name, "vkCmdBeginRenderPass") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdBeginRenderPassTracking;
    if (strcmp(name, "vkCmdEndRenderPass") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdEndRenderPassTracking;
    if (strcmp(name, "vkCmdBeginRenderPass2") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdBeginRenderPass2Tracking;
    if (strcmp(name, "vkCmdEndRenderPass2") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdEndRenderPass2Tracking;
    if (strcmp(name, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)wrapped_DestroyDeviceHook;
    if (strcmp(name, "vkCreateImageView") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateImageView;
    if (strcmp(name, "vkCreateSampler") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateSampler;
    if (strcmp(name, "vkCmdPushDescriptorSetKHR") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdPushDescriptorSetKHR;
    if (strcmp(name, "vkDestroyShaderModule") == 0)
        return (PFN_vkVoidFunction)wrapped_DestroyShaderModule;
    if (strcmp(name, "vkAcquireNextImageKHR") == 0)
        return (PFN_vkVoidFunction)wrapped_AcquireNextImageKHR;
    if (strcmp(name, "vkQueuePresentKHR") == 0)
        return (PFN_vkVoidFunction)wrapped_QueuePresentKHR;
    return NULL;
}

#ifdef AS_LAYER
/*
 * Layer entry points.  The Vulkan loader fills the device dispatch table from
 * the *layer chain's* vkGetDeviceProcAddr, so this is the only place where
 * device level calls can be intercepted.
 */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    LOG("GIPA-ENTRY %s", name ? name : "(null)");
    if (!name)
        return NULL;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    PFN_vkVoidFunction f = intercept(name);
    if (f) {
        resolve_device();
        return f;
    }
    if (next_gipa && instance)
        return next_gipa(instance, name);
    return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    LOG("GDPA-ENTRY %s", name ? name : "(null)");
    if (!name)
        return NULL;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    PFN_vkVoidFunction f = intercept(name);
    if (f)
        return f;
    if (next_gdpa && device)
        return next_gdpa(device, name);
    return NULL;
}
#else /* ICD mode */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    (void)instance;
    DBG("gipa entry: %s", name ? name : "(null)");
    if (!name)
        return NULL;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    PFN_vkVoidFunction f = intercept(name);
    if (f) {
        resolve_device();
        return f;
    }
    return drv_lookup(name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    (void)device;
    DBG("gdpa entry: %s", name ? name : "(null)");
    if (!name)
        return NULL;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    PFN_vkVoidFunction f = intercept(name);
    if (f)
        return f;
    PFN_vkVoidFunction r = drv_device_lookup(name);
    if (!r)
        r = drv_lookup(name);
    return r;
}

#endif /* AS_LAYER */

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                    const char *name)
{
    DBG("icd_gipa entry: %s", name ? name : "(null)");
    if (!name)
        return NULL;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    /* The loader prefers this entry point for device level dispatch, so it
       must be our own implementation - otherwise every device call bypasses
       the shim. */
    if (strcmp(name, "vk_icdGetPhysicalDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vk_icdGetPhysicalDeviceProcAddr;
    PFN_vkVoidFunction f = intercept(name);
    if (f)
        return f;
    if (!g_drv_gipa)
        return NULL;
    return g_drv_gipa(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance,
                                                                         const char *name)
{
    (void)instance;
    DBG("gpdpa entry: %s", name ? name : "(null)");
    if (!name)
        return NULL;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    PFN_vkVoidFunction f = intercept(name);
    if (f) {
        DBG("  gpdpa(%s) -> %p (wrapped)", name, (void *)f);
        return f;
    }
    if (g_drv_gpdpa) {
        PFN_vkVoidFunction r = g_drv_gpdpa(instance, name);
        DBG("  gpdpa(%s) -> %p (driver)", name, (void *)r);
        return r;
    }
    PFN_vkVoidFunction r2 = drv_lookup(name);
    DBG("  gpdpa(%s) -> %p (lookup)", name, (void *)r2);
    return r2;
}

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion)
{
    if (g_drv_negver)
        return g_drv_negver(pVersion);
    if (pVersion)
        *pVersion = 5;
    return VK_SUCCESS;
}

/*
 * The Vulkan loader resolves *device* level entry points by dlsym()ing them
 * directly out of the ICD library (loader_icd_init_entries).  If the ICD does
 * not export them the loader ends up with a NULL pointer and calls into
 * address 0.  Exporting them explicitly - and forwarding to the real driver -
 * keeps the whole chain intact.
 */
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device,
                                                      const VkShaderModuleCreateInfo *pCreateInfo,
                                                      const VkAllocationCallbacks *pAllocator,
                                                      VkShaderModule *pShaderModule)
{
    resolve_device();
    if (!real_CreateShaderModule)
        return VK_ERROR_INITIALIZATION_FAILED;
    return wrapped_CreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

__attribute__((constructor)) static void shim_init(void)
{
    log_open();
    /* Optional: capture the application's stdout/stderr into a file.  The
       launcher swallows the game's output, so this is the only way to see a
       JVM crash report. */
    if (getenv("VKC_CAPTURE_STDERR")) {
        char sp[512];
        snprintf(sp, sizeof(sp), "%s/app-stderr.log", g_dir);
        int sfd = open(sp, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (sfd >= 0) {
            dup2(sfd, 1);
            dup2(sfd, 2);
        }
    }
    if (getenv("VKC_SEGV")) {
        char cp[512];
        snprintf(cp, sizeof(cp), "%s/segv.log", g_dir);
        g_crashfd = open(cp, O_WRONLY | O_CREAT | O_APPEND, 0644);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = segv_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, &g_old_segv);
        sigaction(SIGBUS, &sa, &g_old_bus);
    }
    g_repair = getenv("VKC_REPAIR") ? (atoi(getenv("VKC_REPAIR")) ? 1 : 0) : 1;
    g_probe = getenv("VKC_PROBE") ? (atoi(getenv("VKC_PROBE")) ? 1 : 0) : 0;
    g_retry = getenv("VKC_RETRY") ? atoi(getenv("VKC_RETRY")) : 12;
    g_strip_debug = getenv("VKC_STRIP_DEBUG") ? (atoi(getenv("VKC_STRIP_DEBUG")) ? 1 : 0) : 0;
    g_require_vulkan = getenv("VKC_REQUIRE_VULKAN") ? 1 : 0;

    const char *cands[] = {
        "/var/data/vk-img/libVK_IMG.so",
        "/lib/libVK_IMG.so",
        "/usr/lib/libVK_IMG.so",
        "/usr/lib/libVK_IMG.so.24.2.6603887",
    };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
        g_drv = dlopen(cands[i], RTLD_NOW | RTLD_LOCAL);
        if (g_drv)
            break;
    }
    if (!g_drv) {
        LOG("shim_init: FATAL cannot dlopen PowerVR ICD: %s", dlerror());
        return;
    }

    PFN_vkVoidFunction (*icd_gipa)(void *, const char *) =
        (PFN_vkVoidFunction(*)(void *, const char *))dlsym(g_drv, "vk_icdGetInstanceProcAddr");
    g_drv_negver = (PFN_negver)dlsym(g_drv, "vk_icdNegotiateLoaderICDInterfaceVersion");

    if (icd_gipa)
        g_drv_gipa = (PFN_vkVoidFunction(*)(void *, const char *))icd_gipa(
            VK_NULL_HANDLE, "vkGetInstanceProcAddr");
    if (!g_drv_gipa)
        g_drv_gipa =
            (PFN_vkVoidFunction(*)(void *, const char *))dlsym(g_drv, "vkGetInstanceProcAddr");
    if (g_drv_gipa)
        g_drv_gdpa =
            (PFN_vkVoidFunction(*)(VkDevice, const char *))g_drv_gipa(VK_NULL_HANDLE,
                                                                      "vkGetDeviceProcAddr");
    if (!g_drv_gdpa)
        g_drv_gdpa = (PFN_vkVoidFunction(*)(VkDevice, const char *))dlsym(g_drv, "vkGetDeviceProcAddr");
    g_drv_gpdpa = (PFN_vkVoidFunction(*)(void *, const char *))dlsym(
        g_drv, "vk_icdGetPhysicalDeviceProcAddr");
    if (!g_drv_gpdpa && icd_gipa)
        g_drv_gpdpa = (PFN_vkVoidFunction(*)(void *, const char *))icd_gipa(
            VK_NULL_HANDLE, "vk_icdGetPhysicalDeviceProcAddr");

    LOG("shim_init: driver=%p gipa=%p gdpa=%p repair=%d requireVulkan=%d dir=%s", g_drv,
        (void *)g_drv_gipa, (void *)g_drv_gdpa, g_repair, g_require_vulkan, g_dir);
}

/* runs on a detached worker thread: allocate the readback buffer safely */
void *atlas_worker(void *arg)
{
    (void)arg;
    LOG("atlas worker starting");
    if (!g_dev || !g_pd || !next_gdpa)
        return NULL;
    PFN_vkCreateBuffer cb = (PFN_vkCreateBuffer)next_gdpa(g_dev, "vkCreateBuffer");
    PFN_vkAllocateMemory am = (PFN_vkAllocateMemory)next_gdpa(g_dev, "vkAllocateMemory");
    PFN_vkMapMemory mm = (PFN_vkMapMemory)next_gdpa(g_dev, "vkMapMemory");
    if (!cb || !am || !mm)
        return NULL;
    VkBufferCreateInfo bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 1024 * 1024 * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    LOG("atlas: create buffer...");
    cb(g_dev, &bci, NULL, &g_atlas_dbg);
    LOG("atlas: buffer created");
    PFN_vkGetBufferMemoryRequirements rq =
        (PFN_vkGetBufferMemoryRequirements)next_gdpa(g_dev, "vkGetBufferMemoryRequirements");
    VkMemoryRequirements mr = {0};
    LOG("atlas: get requirements...");
    rq(g_dev, g_atlas_dbg, &mr);
    LOG("atlas: got requirements");
    PFN_vkGetPhysicalDeviceMemoryProperties rmp =
        (PFN_vkGetPhysicalDeviceMemoryProperties)(next_gipa ? next_gipa(layer_instance, "vkGetPhysicalDeviceMemoryProperties") : NULL);
    VkPhysicalDeviceMemoryProperties mp = {0};
    LOG("atlas: get phys props...");
    rmp(g_pd, &mp);
    LOG("atlas: got phys props");
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    for (uint32_t ti = 0; ti < mp.memoryTypeCount; ti++)
        if ((mr.memoryTypeBits & (1u << ti)) &&
            (mp.memoryTypes[ti].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mai.memoryTypeIndex = ti;
            break;
        }
    LOG("atlas: allocate...");
    am(g_dev, &mai, NULL, &g_atlas_dbgmem);
    LOG("atlas: allocated");
    PFN_vkBindBufferMemory rq2 =
        (PFN_vkBindBufferMemory)next_gdpa(g_dev, "vkBindBufferMemory");
    rq2(g_dev, g_atlas_dbg, g_atlas_dbgmem, 0);
    LOG("atlas: map...");
    mm(g_dev, g_atlas_dbgmem, 0, VK_WHOLE_SIZE, 0, (void **)&g_atlas_ptr);
    LOG("atlas: mapped");
    LOG("atlas dbg buffer ready (worker)");
    return NULL;
}
