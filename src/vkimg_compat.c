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
#include <link.h>
#include <setjmp.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
/* view -> image map for render pass attachment tracking */
#define VIEWMAP_MAX 4096
static VkImageView g_viewmap[VIEWMAP_MAX];
static VkImage   g_viewimgs[VIEWMAP_MAX];
static int       g_viewn;
static int       g_view_next;
static pthread_mutex_t g_view_mtx = PTHREAD_MUTEX_INITIALIZER;
static int       g_rt_done;
static int       g_ubo_done;
static int       g_rt_big_done;

void *vimg(void *view)
{
    if (!view)
        return NULL;
    void *res = NULL;
    pthread_mutex_lock(&g_view_mtx);
    for (int i = g_viewn - 1; i >= 0; i--) {
        if (g_viewmap[i] == (VkImageView)view) {
            res = (void *)g_viewimgs[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_view_mtx);
    return res;
}

static void vimg_record(VkImageView view, VkImage image)
{
    if (!view || !image)
        return;
    pthread_mutex_lock(&g_view_mtx);
    for (int i = 0; i < g_viewn; i++) {
        if (g_viewmap[i] == view) {
            g_viewimgs[i] = image;
            pthread_mutex_unlock(&g_view_mtx);
            return;
        }
    }
    int idx = g_view_next % VIEWMAP_MAX;
    g_viewmap[idx] = view;
    g_viewimgs[idx] = image;
    g_view_next++;
    if (g_viewn < VIEWMAP_MAX)
        g_viewn++;
    pthread_mutex_unlock(&g_view_mtx);
}

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
        real_DestroyDevice = (PFN_vkDestroyDevice)next_gdpa(g_dev, "vkDestroyDevice");
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
        if (!real_CmdPushDescriptorSetKHR)
            real_CmdPushDescriptorSetKHR =
                (PFN_vkCmdPushDescriptorSetKHR)next_gdpa(g_dev, "vkCmdPushDescriptorSet");
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
        real_DestroyDevice = (PFN_vkDestroyDevice)next_gdpa(g_dev, "vkDestroyDevice");
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

    if (drop_fdepth || kill_to_ret) {
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
     * The driver does not implement OpKill (discard) in some MRT/OIT shaders.
     * Rewriting it to a bare OpReturn leaves the fragment outputs *undefined*
     * (the driver then renders register garbage — e.g. flashing sky in OIT
     * transmittance passes where the output is `vec4 coeff[2]`).
     *
     * What works: emit OpConstantNull (opcode 46) for every non-FragDepth
     * SC_OUTPUT variable's type (works for OpTypeVector, OpTypeArray, etc.)
     * and OpStore that null constant into each output variable before OpReturn.
     */
    uint32_t out_vars[8], out_types[8], null_ids[8];
    int n_outs = 0;
    if (kill_to_ret) {
        size_t i = 5;
        while (i < nwords && n_outs < 8) {
            uint32_t word = in[i];
            uint32_t op = word & 0xffffu;
            uint32_t len = word >> 16;
            if (!len || i + len > nwords)
                break;
            if (op == OP_VARIABLE && len >= 4 && in[i + 3] == SC_OUTPUT &&
                in[i + 2] != fragdepth_var) {
                uint32_t v = in[i + 2];
                uint32_t ptr = in[i + 1];
                uint32_t otype = 0;
                size_t j = 5;
                while (j < nwords) {
                    uint32_t w2 = in[j];
                    uint32_t o2 = w2 & 0xffffu;
                    uint32_t l2 = w2 >> 16;
                    if (!l2 || j + l2 > nwords)
                        break;
                    if (o2 == OP_TYPE_POINTER && l2 >= 4 && in[j + 1] == ptr) {
                        otype = in[j + 3];
                        break;
                    }
                    j += l2;
                }
                if (otype) {
                    out_vars[n_outs] = v;
                    out_types[n_outs] = otype;
                    null_ids[n_outs] = 0;
                    n_outs++;
                }
            }
            i += len;
        }
    }

    uint32_t new_bound = (nwords >= 5) ? in[3] : 1;

    size_t cap = nwords + 128;
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

        /* emit OpConstantNull (opcode 46) for each fragment output type */
        if (!inserted_const && op == OP_FUNCTION && n_outs > 0) {
            if (o + (size_t)n_outs * 3 + 8 > cap) {
                cap = (o + (size_t)n_outs * 3 + 8) * 2;
                uint32_t *tmp = realloc(out, cap * 4);
                if (!tmp) {
                    free(out);
                    return NULL;
                }
                out = tmp;
            }
            for (int k = 0; k < n_outs; k++) {
                null_ids[k] = new_bound++;
                out[o++] = (3u << 16) | 46u /* OpConstantNull */;
                out[o++] = out_types[k];
                out[o++] = null_ids[k];
            }
            out[3] = new_bound;
            inserted_const = 1;
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
            if (inserted_const && n_outs > 0) {
                /* OpStore %out_k %null_k for each output ; OpReturn */
                if (o + (size_t)n_outs * 3 + 4 > cap) {
                    cap = (o + (size_t)n_outs * 3 + 4) * 2;
                    uint32_t *tmp = realloc(out, cap * 4);
                    if (!tmp) {
                        free(out);
                        return NULL;
                    }
                    out = tmp;
                }
                for (int k = 0; k < n_outs; k++) {
                    out[o++] = (3u << 16) | OP_STORE;
                    out[o++] = out_vars[k];
                    out[o++] = null_ids[k];
                }
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
        if (e && !e->file[0])
            spv_dump(e);
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

static __thread sigjmp_buf g_uf_jmp;
static __thread volatile int g_uf_in_call;
static uintptr_t g_uf_base;

static void caught_uf_out_of_range(const char *msg)
{
    void *bt[16];
    int cnt = backtrace(bt, 16);
    LOG("caught libufwriter std::out_of_range(%s) base=0x%lx",
        msg ? msg : "?", (unsigned long)g_uf_base);
    for (int i = 0; i < cnt && i < 8; i++) {
        uintptr_t a = (uintptr_t)bt[i];
        if (g_uf_base && a >= g_uf_base)
            LOG("  bt[%d]: %p (uf+0x%lx)", i, bt[i], (unsigned long)(a - g_uf_base));
        else
            LOG("  bt[%d]: %p", i, bt[i]);
    }
    if (g_uf_in_call) {
        g_uf_in_call = 0;
        siglongjmp(g_uf_jmp, 1);
    }
    abort();
}

static int uf_phdr_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    (void)data;
    if (!info->dlpi_name || !strstr(info->dlpi_name, "libufwriter.so"))
        return 0;
    g_uf_base = (uintptr_t)info->dlpi_addr;
    uintptr_t got_page = (g_uf_base + 0x2fd6000ULL) & ~4095ULL;
    if (mprotect((void *)got_page, 8192, PROT_READ | PROT_WRITE) == 0) {
        *(void **)(g_uf_base + 0x2fd6470ULL) = (void *)caught_uf_out_of_range;
        *(void **)(g_uf_base + 0x2fd6028ULL) = (void *)caught_uf_out_of_range;
        DBG("hooked libufwriter.so out_of_range GOT at base=0x%lx", (unsigned long)g_uf_base);
    }
    return 1;
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


/* The OIT state fix is opt-in (VKC_OIT_FIX=1); default OFF. */
static int oit_fix_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("VKC_OIT_FIX");
        cached = (e && e[0] == '1') ? 1 : 0;
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
    {
        const VkLayerInstanceCreateInfo *link =
            find_instance_info(pCreateInfo->pNext, VK_LAYER_LINK_INFO);
        if (link && link->u.pLayerInfo) {
            next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
            DBG("layer: next gipa=%p", (void *)next_gipa);
        }
        const VkLayerInstanceCreateInfo *cb = find_instance_info(
            pCreateInfo->pNext, VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK);
        if (cb)
            next_layer_create_device = cb->u.layerDevice.pfnLayerCreateDevice;
    }
#endif
    real_CreateInstance = NULL;
    resolve_global();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_CreateInstance)
        r = real_CreateInstance(pCreateInfo, pAllocator, pInstance);
    if (r == VK_SUCCESS && pInstance) {
        g_inst = *pInstance;
        layer_instance = *pInstance;
#ifdef AS_LAYER
        if (next_gipa)
            real_DestroyInstance = (PFN_vkDestroyInstance)next_gipa(g_inst, "vkDestroyInstance");
#endif
        resolve_device();
    }
    DBG("CreateInstance r=%d instance=%p", r, (void *)(pInstance ? *pInstance : NULL));
    return r;
}

static void VKAPI_CALL wrapped_DestroyInstance(VkInstance instance,
                                               const VkAllocationCallbacks *pAllocator)
{
    if (real_DestroyInstance)
        real_DestroyInstance(instance, pAllocator);
    if (g_inst == instance)
        g_inst = VK_NULL_HANDLE;
}

static void resolve_copyfns(void);

static VkResult VKAPI_CALL wrapped_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char *pLayerName,
    uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
    static const char *const extra_exts[] = {
        "VK_EXT_robustness2",
        "VK_EXT_transform_feedback",
    };
    const uint32_t nextra = 2;

    if (pLayerName && strcmp(pLayerName, "VK_LAYER_powervr_compat") == 0) {
        if (!pPropertyCount)
            return VK_INCOMPLETE;
        if (!pProperties) {
            *pPropertyCount = nextra;
            return VK_SUCCESS;
        }
        uint32_t cap = *pPropertyCount;
        uint32_t n = cap < nextra ? cap : nextra;
        for (uint32_t i = 0; i < n; i++) {
            memset(&pProperties[i], 0, sizeof(pProperties[i]));
            strncpy(pProperties[i].extensionName, extra_exts[i],
                    VK_MAX_EXTENSION_NAME_SIZE - 1);
            pProperties[i].specVersion = 1;
        }
        *pPropertyCount = n;
        return n < nextra ? VK_INCOMPLETE : VK_SUCCESS;
    }

    PFN_vkEnumerateDeviceExtensionProperties fn = NULL;
#ifdef AS_LAYER
    if (next_gipa)
        fn = (PFN_vkEnumerateDeviceExtensionProperties)next_gipa(
            layer_instance ? layer_instance : VK_NULL_HANDLE,
            "vkEnumerateDeviceExtensionProperties");
#endif
    if (!fn && g_drv_gipa)
        fn = (PFN_vkEnumerateDeviceExtensionProperties)g_drv_gipa(
            layer_instance ? layer_instance : VK_NULL_HANDLE,
            "vkEnumerateDeviceExtensionProperties");
    if (!fn)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (!pProperties) {
        VkResult r = fn(physicalDevice, pLayerName, pPropertyCount, NULL);
        if (r == VK_SUCCESS && pPropertyCount)
            (*pPropertyCount) += nextra;
        return r;
    }

    uint32_t cap = *pPropertyCount;
    uint32_t cnt = cap > nextra ? cap - nextra : 0;
    VkResult r = fn(physicalDevice, pLayerName, &cnt, pProperties);
    for (uint32_t e = 0; e < nextra; e++) {
        int found = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            if (strcmp(pProperties[i].extensionName, extra_exts[e]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            if (cnt < cap) {
                memset(&pProperties[cnt], 0, sizeof(pProperties[cnt]));
                strncpy(pProperties[cnt].extensionName, extra_exts[e],
                        VK_MAX_EXTENSION_NAME_SIZE - 1);
                pProperties[cnt].specVersion = 1;
                cnt++;
            } else {
                r = VK_INCOMPLETE;
            }
        }
    }
    *pPropertyCount = cnt;
    return r;
}

typedef struct VkPhysicalDeviceRobustness2FeaturesEXT_local {
    VkStructureType sType;
    void *pNext;
    VkBool32 robustBufferAccess2;
    VkBool32 robustImageAccess2;
    VkBool32 nullDescriptor;
} VkPhysicalDeviceRobustness2FeaturesEXT_local;

typedef struct VkPhysicalDeviceRobustness2PropertiesEXT_local {
    VkStructureType sType;
    void *pNext;
    VkDeviceSize robustStorageBufferAccessSizeAlignment;
    VkDeviceSize robustUniformBufferAccessSizeAlignment;
} VkPhysicalDeviceRobustness2PropertiesEXT_local;

typedef struct VkPhysicalDeviceTransformFeedbackFeaturesEXT_local {
    VkStructureType sType;
    void *pNext;
    VkBool32 transformFeedback;
    VkBool32 geometryStreams;
} VkPhysicalDeviceTransformFeedbackFeaturesEXT_local;

typedef struct VkPhysicalDeviceTransformFeedbackPropertiesEXT_local {
    VkStructureType sType;
    void *pNext;
    uint32_t maxTransformFeedbackStreams;
    uint32_t maxTransformFeedbackBuffers;
    VkDeviceSize maxTransformFeedbackBufferSize;
    uint32_t maxTransformFeedbackStreamDataSize;
    uint32_t maxTransformFeedbackBufferDataSize;
    uint32_t maxTransformFeedbackBufferDataStride;
    VkBool32 transformFeedbackQueries;
    VkBool32 transformFeedbackStreamsLinesTriangles;
    VkBool32 transformFeedbackRasterizationStreamSelect;
    VkBool32 transformFeedbackDraw;
} VkPhysicalDeviceTransformFeedbackPropertiesEXT_local;

#define VKC_STRUCTURE_TYPE_ROBUSTNESS_2_FEATURES_EXT ((VkStructureType)1000286000)
#define VKC_STRUCTURE_TYPE_ROBUSTNESS_2_PROPERTIES_EXT ((VkStructureType)1000286001)
#define VKC_STRUCTURE_TYPE_TRANSFORM_FEEDBACK_FEATURES_EXT ((VkStructureType)1000028000)
#define VKC_STRUCTURE_TYPE_TRANSFORM_FEEDBACK_PROPERTIES_EXT ((VkStructureType)1000028001)

static void VKAPI_CALL wrapped_GetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures)
{
    PFN_vkGetPhysicalDeviceFeatures2 fn = NULL;
#ifdef AS_LAYER
    if (next_gipa) {
        fn = (PFN_vkGetPhysicalDeviceFeatures2)next_gipa(
            layer_instance ? layer_instance : VK_NULL_HANDLE,
            "vkGetPhysicalDeviceFeatures2");
        if (!fn)
            fn = (PFN_vkGetPhysicalDeviceFeatures2)next_gipa(
                layer_instance ? layer_instance : VK_NULL_HANDLE,
                "vkGetPhysicalDeviceFeatures2KHR");
    }
#endif
    if (!fn && g_drv_gipa) {
        fn = (PFN_vkGetPhysicalDeviceFeatures2)g_drv_gipa(
            layer_instance ? layer_instance : VK_NULL_HANDLE,
            "vkGetPhysicalDeviceFeatures2");
        if (!fn)
            fn = (PFN_vkGetPhysicalDeviceFeatures2)g_drv_gipa(
                layer_instance ? layer_instance : VK_NULL_HANDLE,
                "vkGetPhysicalDeviceFeatures2KHR");
    }
    if (fn)
        fn(physicalDevice, pFeatures);
    if (!pFeatures)
        return;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)pFeatures->pNext; p; p = p->pNext) {
        if (p->sType == VKC_STRUCTURE_TYPE_ROBUSTNESS_2_FEATURES_EXT) {
            VkPhysicalDeviceRobustness2FeaturesEXT_local *rf =
                (VkPhysicalDeviceRobustness2FeaturesEXT_local *)p;
            rf->robustBufferAccess2 = VK_TRUE;
            rf->robustImageAccess2 = VK_TRUE;
            rf->nullDescriptor = VK_TRUE;
        } else if (p->sType == VKC_STRUCTURE_TYPE_TRANSFORM_FEEDBACK_FEATURES_EXT) {
            VkPhysicalDeviceTransformFeedbackFeaturesEXT_local *tf =
                (VkPhysicalDeviceTransformFeedbackFeaturesEXT_local *)p;
            tf->transformFeedback = VK_TRUE;
            tf->geometryStreams = VK_TRUE;
        }
    }
}

static void VKAPI_CALL wrapped_GetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties)
{
    PFN_vkGetPhysicalDeviceProperties2 fn = NULL;
#ifdef AS_LAYER
    if (next_gipa) {
        fn = (PFN_vkGetPhysicalDeviceProperties2)next_gipa(
            layer_instance ? layer_instance : VK_NULL_HANDLE,
            "vkGetPhysicalDeviceProperties2");
        if (!fn)
            fn = (PFN_vkGetPhysicalDeviceProperties2)next_gipa(
                layer_instance ? layer_instance : VK_NULL_HANDLE,
                "vkGetPhysicalDeviceProperties2KHR");
    }
#endif
    if (!fn && g_drv_gipa) {
        fn = (PFN_vkGetPhysicalDeviceProperties2)g_drv_gipa(
            layer_instance ? layer_instance : VK_NULL_HANDLE,
            "vkGetPhysicalDeviceProperties2");
        if (!fn)
            fn = (PFN_vkGetPhysicalDeviceProperties2)g_drv_gipa(
                layer_instance ? layer_instance : VK_NULL_HANDLE,
                "vkGetPhysicalDeviceProperties2KHR");
    }
    if (fn)
        fn(physicalDevice, pProperties);
    if (!pProperties)
        return;
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)pProperties->pNext; p; p = p->pNext) {
        if (p->sType == VKC_STRUCTURE_TYPE_ROBUSTNESS_2_PROPERTIES_EXT) {
            VkPhysicalDeviceRobustness2PropertiesEXT_local *rp =
                (VkPhysicalDeviceRobustness2PropertiesEXT_local *)p;
            rp->robustStorageBufferAccessSizeAlignment = 4;
            rp->robustUniformBufferAccessSizeAlignment = 16;
        } else if (p->sType == VKC_STRUCTURE_TYPE_TRANSFORM_FEEDBACK_PROPERTIES_EXT) {
            VkPhysicalDeviceTransformFeedbackPropertiesEXT_local *tp =
                (VkPhysicalDeviceTransformFeedbackPropertiesEXT_local *)p;
            tp->maxTransformFeedbackStreams = 4;
            tp->maxTransformFeedbackBuffers = 4;
            tp->maxTransformFeedbackBufferSize = 0x10000000ULL;
            tp->maxTransformFeedbackStreamDataSize = 512;
            tp->maxTransformFeedbackBufferDataSize = 512;
            tp->maxTransformFeedbackBufferDataStride = 512;
            tp->transformFeedbackQueries = VK_TRUE;
            tp->transformFeedbackStreamsLinesTriangles = VK_TRUE;
            tp->transformFeedbackRasterizationStreamSelect = VK_TRUE;
            tp->transformFeedbackDraw = VK_TRUE;
        }
    }
}

static void VKAPI_CALL noop_CmdBindTransformFeedbackBuffersEXT(
    VkCommandBuffer cb, uint32_t firstBinding, uint32_t bindingCount,
    const VkBuffer *pBuffers, const VkDeviceSize *pOffsets, const VkDeviceSize *pSizes)
{
    (void)cb; (void)firstBinding; (void)bindingCount;
    (void)pBuffers; (void)pOffsets; (void)pSizes;
}

static void VKAPI_CALL noop_CmdBeginTransformFeedbackEXT(
    VkCommandBuffer cb, uint32_t firstCounterBuffer, uint32_t counterBufferCount,
    const VkBuffer *pCounterBuffers, const VkDeviceSize *pCounterBufferOffsets)
{
    (void)cb; (void)firstCounterBuffer; (void)counterBufferCount;
    (void)pCounterBuffers; (void)pCounterBufferOffsets;
}

static void VKAPI_CALL noop_CmdEndTransformFeedbackEXT(
    VkCommandBuffer cb, uint32_t firstCounterBuffer, uint32_t counterBufferCount,
    const VkBuffer *pCounterBuffers, const VkDeviceSize *pCounterBufferOffsets)
{
    (void)cb; (void)firstCounterBuffer; (void)counterBufferCount;
    (void)pCounterBuffers; (void)pCounterBufferOffsets;
}

static void VKAPI_CALL noop_CmdBeginQueryIndexedEXT(
    VkCommandBuffer cb, VkQueryPool queryPool, uint32_t query,
    VkQueryControlFlags flags, uint32_t index)
{
    (void)cb; (void)queryPool; (void)query; (void)flags; (void)index;
}

static void VKAPI_CALL noop_CmdEndQueryIndexedEXT(
    VkCommandBuffer cb, VkQueryPool queryPool, uint32_t query, uint32_t index)
{
    (void)cb; (void)queryPool; (void)query; (void)index;
}

static void VKAPI_CALL noop_CmdDrawIndirectByteCountEXT(
    VkCommandBuffer cb, uint32_t instanceCount, uint32_t firstInstance,
    VkBuffer counterBuffer, VkDeviceSize counterBufferOffset,
    uint32_t counterOffset, uint32_t vertexStride)
{
    (void)cb; (void)instanceCount; (void)firstInstance;
    (void)counterBuffer; (void)counterBufferOffset;
    (void)counterOffset; (void)vertexStride;
}

static VkResult VKAPI_CALL wrapped_CreateDevice(VkPhysicalDevice physicalDevice,
                                                const VkDeviceCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator,
                                                VkDevice *pDevice)
{
    VkDeviceCreateInfo ci_copy;
    const char *filtered_exts[256];
    if (pCreateInfo && pCreateInfo->enabledExtensionCount > 0 &&
        pCreateInfo->enabledExtensionCount <= 256 && pCreateInfo->ppEnabledExtensionNames) {
        ci_copy = *pCreateInfo;
        uint32_t new_cnt = 0;
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            if (pCreateInfo->ppEnabledExtensionNames[i] &&
                (strcmp(pCreateInfo->ppEnabledExtensionNames[i], "VK_EXT_robustness2") == 0 ||
                 strcmp(pCreateInfo->ppEnabledExtensionNames[i], "VK_EXT_transform_feedback") == 0))
                continue;
            filtered_exts[new_cnt++] = pCreateInfo->ppEnabledExtensionNames[i];
        }
        ci_copy.enabledExtensionCount = new_cnt;
        ci_copy.ppEnabledExtensionNames = filtered_exts;
        pCreateInfo = &ci_copy;
    }
    if (pCreateInfo) {
        for (VkBaseOutStructure *p = (VkBaseOutStructure *)pCreateInfo->pNext; p; p = p->pNext) {
            if (p->sType == VKC_STRUCTURE_TYPE_ROBUSTNESS_2_FEATURES_EXT) {
                VkPhysicalDeviceRobustness2FeaturesEXT_local *rf =
                    (VkPhysicalDeviceRobustness2FeaturesEXT_local *)p;
                rf->robustBufferAccess2 = VK_FALSE;
                rf->robustImageAccess2 = VK_FALSE;
                rf->nullDescriptor = VK_FALSE;
            } else if (p->sType == VKC_STRUCTURE_TYPE_TRANSFORM_FEEDBACK_FEATURES_EXT) {
                VkPhysicalDeviceTransformFeedbackFeaturesEXT_local *tf =
                    (VkPhysicalDeviceTransformFeedbackFeaturesEXT_local *)p;
                tf->transformFeedback = VK_FALSE;
                tf->geometryStreams = VK_FALSE;
            }
        }
    }
#ifdef AS_LAYER
    {
        const VkLayerDeviceCreateInfo *link =
            find_device_info(pCreateInfo->pNext, VK_LAYER_LINK_INFO);
        if (link && link->u.pLayerInfo) {
            next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
            DBG("layer: next device gdpa=%p", (void *)next_gdpa);
        }
    }
    g_pd = physicalDevice;
    if (next_gipa) {
        PFN_vkCreateDevice next_create =
            (PFN_vkCreateDevice)next_gipa(layer_instance ? layer_instance : VK_NULL_HANDLE,
                                           "vkCreateDevice");
        if (next_create) {
            VkResult res = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);
            if (res == VK_SUCCESS && pDevice && *pDevice) {
                g_dev = *pDevice;
                real_CreateShaderModule = NULL;
                resolve_device();
                if (!g_uf_base)
                    dl_iterate_phdr(uf_phdr_cb, NULL);
            }
            if (res == VK_SUCCESS && getenv("VKC_DUMP_ATLAS")) {
                extern void *atlas_worker(void *);
                pthread_t th;
                pthread_create(&th, NULL, atlas_worker, NULL);
                pthread_detach(th);
            }
            DBG("CreateDevice r=%d device=%p next_gdpa=%p", res, (void *)(pDevice ? *pDevice : NULL),
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
    DBG("CreateDevice r=%d device=%p", r, (void *)(pDevice ? *pDevice : NULL));
    return r;
}


static VkResult VKAPI_CALL wrapped_CreateShaderModule(VkDevice device,
                                                   const VkShaderModuleCreateInfo *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator,
                                                   VkShaderModule *pShaderModule)
{
    resolve_device();

    const uint32_t *code = pCreateInfo ? pCreateInfo->pCode : NULL;
    size_t words = (pCreateInfo && pCreateInfo->codeSize >= 4) ? pCreateInfo->codeSize / 4 : 0;
    int oit = code && words ? spv_is_oit(code, words) : 0;
    uint32_t *patched = NULL;
    size_t patched_words = 0;
    size_t tb_words = 0;
    /*
     * Leave OpKill in place by default (rewriting plain shaders breaks GUI font
     * rendering); failing OIT/MRT shaders are rewritten on demand in
     * wrapped_CreateGraphicsPipelines. Set VKC_KILL_REWRITE=1 to force early rewrite.
     */
    const char *nokill = getenv("VKC_NO_KILL_REWRITE");
    const char *killmode = getenv("VKC_KILL_REWRITE");
    int do_kill_rewrite = (killmode && killmode[0] == '1') || (nokill && nokill[0] == '0');
    if (do_kill_rewrite && code && words && spv_has_kill(code, words)) {
        struct spv_stats st1;
        patched = spv_rewrite(code, words, 0, 1, 0, &st1, &patched_words);
        if (patched && st1.kills) {
            DBG("%s shader: rewriting %d OpKill -> discard (%zu -> %zu words)",
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
            DBG("texel buffer shader: rewritten to a storage buffer (%zu -> %zu words)",
                patched_words, tb_words);
            free(patched);
            patched = tb;
            patched_words = tb_words;
        }
    } else if (code && words) {
        uint32_t *tb = spv_texelbuf_to_storage(code, words, &tb_words);
        if (tb) {
            DBG("texel buffer shader: rewritten to a storage buffer (%zu -> %zu words)",
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

    if (code && words && getenv("VKC_LOD0") != NULL) {
        const uint32_t *src = patched ? patched : code;
        size_t src_words = patched ? patched_words : words;
        size_t lod_words = 0;
        uint32_t *lod = spv_force_lod0(src, src_words, &lod_words);
        if (lod) {
            DBG("shader: forcing the texture LOD to 0 (%zu -> %zu words)", src_words, lod_words);
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
    if (real_CreateShaderModule) {
        if (sigsetjmp(g_uf_jmp, 1) == 0) {
            g_uf_in_call = 1;
            r = real_CreateShaderModule(device, patched ? &ci : pCreateInfo, pAllocator, pShaderModule);
            g_uf_in_call = 0;
        } else {
            r = VK_ERROR_UNKNOWN;
        }
    }
    drv_unlock();
    for (int attempt = 0; attempt < g_retry && r != VK_SUCCESS; attempt++) {
        usleep(20000);
        drv_lock();
        if (real_CreateShaderModule) {
            if (sigsetjmp(g_uf_jmp, 1) == 0) {
                g_uf_in_call = 1;
                r = real_CreateShaderModule(device, patched ? &ci : pCreateInfo, pAllocator, pShaderModule);
                g_uf_in_call = 0;
            } else {
                r = VK_ERROR_UNKNOWN;
            }
        }
        drv_unlock();
        if (r == VK_SUCCESS)
            DBG("CreateShaderModule: succeeded on retry %d", attempt + 1);
    }

    if (r == VK_SUCCESS && pShaderModule && *pShaderModule && pCreateInfo && pCreateInfo->pCode &&
        pCreateInfo->codeSize >= 20) {
        const uint32_t *save_code = patched ? patched : pCreateInfo->pCode;
        size_t save_words = patched ? patched_words : (pCreateInfo->codeSize / 4);
        struct spv_entry *e = calloc(1, sizeof(*e));
        if (e) {
            e->mod = *pShaderModule;
            e->words = save_words;
            e->code = malloc(save_words * 4);
            if (e->code) {
                memcpy(e->code, save_code, save_words * 4);
                e->stage = spv_entry_point_stage(e->code, save_words);
                e->oit = oit;
                spv_source_name(pCreateInfo->pCode, pCreateInfo->codeSize / 4, e->src, sizeof(e->src));
                pthread_mutex_lock(&g_spv_mtx);
                e->seq = ++g_spv_seq;
                if (g_debug || getenv("VKC_DUMP_SPV"))
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
    free(patched);
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
    drv_lock();
    VkResult r = real_CreateShaderModule(device, &ci, NULL, &mod);
    drv_unlock();
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

    VkDescriptorSetLayoutBinding lb[64];
    VkDescriptorSetLayoutCreateInfo lci;
    int patched_layout = 0;
    if (pCreateInfo && pCreateInfo->bindingCount <= 64 && pCreateInfo->pBindings) {
        const char *notexel2 = getenv("VKC_NO_TEXEL");
        if (!(notexel2 && notexel2[0] == '1')) {
            for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
                if (pCreateInfo->pBindings[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
                    if (!patched_layout) {
                        memcpy(lb, pCreateInfo->pBindings, pCreateInfo->bindingCount * sizeof(lb[0]));
                        patched_layout = 1;
                    }
                    lb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                }
            }
        }
    }
    if (patched_layout) {
        lci = *pCreateInfo;
        lci.pBindings = lb;
        if (real_CreateDescriptorSetLayout)
            r = real_CreateDescriptorSetLayout(device, &lci, pAllocator, pSetLayout);
        DBG("descriptor set layout: texel buffer descriptors turned into storage buffers");
    } else if (real_CreateDescriptorSetLayout) {
        r = real_CreateDescriptorSetLayout(device, pCreateInfo, pAllocator, pSetLayout);
    }
    if (r == VK_SUCCESS && pCreateInfo) {
        if (pSetLayout)
            remember_dsl(*pSetLayout, patched_layout ? &lci : pCreateInfo);
        if (g_debug) {
            DBG("dsl %p bindings=%u:", (void *)(pSetLayout ? *pSetLayout : NULL), pCreateInfo->bindingCount);
            for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
                const VkDescriptorSetLayoutBinding *b = &pCreateInfo->pBindings[i];
                DBG("   binding=%u type=%d count=%u stages=0x%x", b->binding, b->descriptorType,
                    b->descriptorCount, b->stageFlags);
            }
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
        if (g_debug) {
            DBG("CreatePipelineLayout %p sets=%u pushRanges=%u", (void *)*pPipelineLayout,
                pCreateInfo->setLayoutCount, pCreateInfo->pushConstantRangeCount);
            for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; i++)
                DBG("   set%u dsl=%p", i, (void *)pCreateInfo->pSetLayouts[i]);
            for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; i++)
                DBG("   pcRange%u stages=0x%x offset=%u size=%u", i,
                    pCreateInfo->pPushConstantRanges[i].stageFlags,
                    pCreateInfo->pPushConstantRanges[i].offset, pCreateInfo->pPushConstantRanges[i].size);
        }
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
    VkPipelineRasterizationStateCreateInfo *rs_copies = NULL;
    uint32_t nfix = 0;
    if (pCreateInfos && createInfoCount) {
        for (uint32_t i = 0; i < createInfoCount; i++) {
            const VkGraphicsPipelineCreateInfo *ci = &pCreateInfos[i];
            for (uint32_t st = 0; st < ci->stageCount; st++) {
                if (ci->pStages[st].stage != VK_SHADER_STAGE_FRAGMENT_BIT)
                    continue;
                if (fix_find(ci->pStages[st].module) ||
                    (oit_fix_enabled() && module_is_oit(ci->pStages[st].module)))
                    nfix++;
            }
            if (ci->pRasterizationState &&
                ci->pRasterizationState->polygonMode != VK_POLYGON_MODE_FILL)
                nfix++;
            if (g_debug) {
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
                    DBG("pipeline %u: the descriptor layout does not match the shaders (%s)", i, why);
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
        rs_copies = calloc(createInfoCount, sizeof(*rs_copies));
        if (cis && stage_copies && rs_copies) {
            memcpy(cis, pCreateInfos, createInfoCount * sizeof(*cis));
            for (uint32_t i = 0; i < createInfoCount; i++) {
                const VkGraphicsPipelineCreateInfo *ci = &pCreateInfos[i];
                if (ci->pRasterizationState &&
                    ci->pRasterizationState->polygonMode != VK_POLYGON_MODE_FILL) {
                    rs_copies[i] = *ci->pRasterizationState;
                    rs_copies[i].polygonMode = VK_POLYGON_MODE_FILL;
                    cis[i].pRasterizationState = &rs_copies[i];
                }
                if (ci->stageCount > 8)
                    continue;
                memcpy(stage_copies[i], ci->pStages,
                       sizeof(VkPipelineShaderStageCreateInfo) * ci->stageCount);
                VkShaderModule oit_mod = VK_NULL_HANDLE;
                for (uint32_t st2 = 0; st2 < ci->stageCount; st2++)
                    if (ci->pStages[st2].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
                        oit_mod = ci->pStages[st2].module;
                        break;
                    }
                if (oit_mod != VK_NULL_HANDLE && module_is_oit(oit_mod) && oit_fix_enabled()) {
                    DBG("  applying the OIT state fix to pipeline %u", i);
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
                    rs_copies[i] = *ci->pRasterizationState;
                    rs_copies[i].rasterizerDiscardEnable = VK_TRUE;
                    cis[i].pRasterizationState = &rs_copies[i];
                }
            }
        } else {
            free(cis);
            free(stage_copies);
            free(rs_copies);
            cis = NULL;
            stage_copies = NULL;
            rs_copies = NULL;
        }
    }

    if (getenv("VKC_LOG_LAYOUT") && pCreateInfos)
        for (uint32_t i = 0; i < createInfoCount; i++)
            LOG("pipeline %u layout=%p", i, (void *)pCreateInfos[i].layout);
    drv_lock();
    VkResult r = VK_ERROR_UNKNOWN;
    if (sigsetjmp(g_uf_jmp, 1) == 0) {
        g_uf_in_call = 1;
        r = real_CreateGraphicsPipelines(device, pipelineCache, createInfoCount,
                                         cis ? cis : pCreateInfos, pAllocator, pPipelines);
        g_uf_in_call = 0;
    } else {
        LOG("vkCreateGraphicsPipelines threw out_of_range on initial attempt");
        r = VK_ERROR_UNKNOWN;
    }
    drv_unlock();
    free(cis);
    free(stage_copies);
    free(rs_copies);

    /*
     * The PowerVR shader/pipeline compiler is non-deterministic: the very
     * same pipeline sometimes fails with VK_ERROR_UNKNOWN and sometimes
     * succeeds.  Simply trying again is enough to get a valid pipeline.
     */
    for (int attempt = 0; attempt < g_retry && r != VK_SUCCESS; attempt++) {
        usleep(30000);
        drv_lock();
        if (sigsetjmp(g_uf_jmp, 1) == 0) {
            g_uf_in_call = 1;
            r = real_CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos,
                                             pAllocator, pPipelines);
            g_uf_in_call = 0;
        } else {
            r = VK_ERROR_UNKNOWN;
        }
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
                if (sigsetjmp(g_uf_jmp, 1) == 0) {
                    g_uf_in_call = 1;
                    r2 = real_CreateGraphicsPipelines(device, pipelineCache, 1, &tmp, pAllocator,
                                                     &outpipe);
                    g_uf_in_call = 0;
                } else {
                    r2 = VK_ERROR_UNKNOWN;
                }
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
                for (int k = 0; k < ncand; k++)
                    if (k != c && cand[k].repl_fs && real_DestroyShaderModule)
                        real_DestroyShaderModule(device, cand[k].repl_fs, NULL);
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

static VkResult VKAPI_CALL wrapped_CreateComputePipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo *pCreateInfos, const VkAllocationCallbacks *pAllocator,
    VkPipeline *pPipelines)
{
    resolve_device();
    if (!real_CreateComputePipelines)
        return VK_ERROR_INITIALIZATION_FAILED;
    drv_lock();
    VkResult r = VK_ERROR_UNKNOWN;
    if (sigsetjmp(g_uf_jmp, 1) == 0) {
        g_uf_in_call = 1;
        r = real_CreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos,
                                        pAllocator, pPipelines);
        g_uf_in_call = 0;
    }
    drv_unlock();
    for (int attempt = 0; attempt < g_retry && r != VK_SUCCESS; attempt++) {
        usleep(20000);
        drv_lock();
        if (sigsetjmp(g_uf_jmp, 1) == 0) {
            g_uf_in_call = 1;
            r = real_CreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos,
                                            pAllocator, pPipelines);
            g_uf_in_call = 0;
        }
        drv_unlock();
        if (r == VK_SUCCESS)
            DBG("vkCreateComputePipelines: succeeded on retry %d", attempt + 1);
    }
    if (r != VK_SUCCESS)
        LOG("vkCreateComputePipelines FAILED r=%d count=%u", r, createInfoCount);
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
    if (r == VK_SUCCESS && pView && pCreateInfo) {
        vimg_record(*pView, pCreateInfo->image);
    }
    if (r == VK_SUCCESS && pCreateInfo && getenv("VKC_LOG_IMAGES"))
        LOG("image view %p: image=%p format=%d type=%d aspect=0x%x baseMip=%u levelCount=%u baseLayer=%u layerCount=%u",
            (void *)(pView ? *pView : NULL), (void *)pCreateInfo->image,
            (int)pCreateInfo->format, (int)pCreateInfo->viewType,
            pCreateInfo->subresourceRange.aspectMask, pCreateInfo->subresourceRange.baseMipLevel,
            pCreateInfo->subresourceRange.levelCount, pCreateInfo->subresourceRange.baseArrayLayer,
            pCreateInfo->subresourceRange.layerCount);
    return r;
}

static VkResult VKAPI_CALL wrapped_CreateSampler(VkDevice device,
                                                const VkSamplerCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator,
                                                VkSampler *pSampler)
{
    resolve_device();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    static int clamp_lod0 = -1;
    if (clamp_lod0 < 0) {
        const char *e = getenv("VKC_CLAMP_LOD0");
        clamp_lod0 = (e && e[0] == '0') ? 0 : 1; /* default ON */
    }
    if (clamp_lod0 && pCreateInfo) {
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
static PFN_vkCreateBuffer real_CreateBufferH;
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
static PFN_vkCmdBindVertexBuffers real_CmdBindVertexBuffers;

static PFN_vkDestroyCommandPool real_DestroyCommandPoolH;
static PFN_vkCmdBeginRendering real_CmdBeginRendering;
static PFN_vkCmdEndRendering real_CmdEndRendering;
static PFN_vkCmdPipelineBarrier real_CmdPipelineBarrier;
static PFN_vkQueueSubmit2KHR real_QueueSubmit2KHR;
static PFN_vkWaitSemaphores real_WaitSemaphores;
static PFN_vkCmdClearColorImage real_CmdClearColorImage;
static PFN_vkCmdClearDepthStencilImage real_CmdClearDepthStencilImage;
static PFN_vkEndCommandBuffer real_EndCommandBuffer;

#define MAX_PENDING_CLEARS 64
struct pending_clear {
    int used;
    VkCommandBuffer cb;
    VkImage image;
    VkImageLayout layout;
    int is_depth;
    VkClearColorValue color;
    VkClearDepthStencilValue depth;
    VkImageSubresourceRange range;
};
static struct pending_clear g_pclears[MAX_PENDING_CLEARS];
static pthread_mutex_t g_pclear_mtx = PTHREAD_MUTEX_INITIALIZER;

static void pclear_reset_cb(VkCommandBuffer cb)
{
    pthread_mutex_lock(&g_pclear_mtx);
    for (int i = 0; i < MAX_PENDING_CLEARS; i++) {
        if (g_pclears[i].used && g_pclears[i].cb == cb)
            g_pclears[i].used = 0;
    }
    pthread_mutex_unlock(&g_pclear_mtx);
}

static void pclear_reset_all(void)
{
    pthread_mutex_lock(&g_pclear_mtx);
    memset(g_pclears, 0, sizeof(g_pclears));
    pthread_mutex_unlock(&g_pclear_mtx);
}

static void pclear_add_color(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
                             const VkClearColorValue *color, const VkImageSubresourceRange *range)
{
    pthread_mutex_lock(&g_pclear_mtx);
    int slot = -1;
    for (int i = 0; i < MAX_PENDING_CLEARS; i++) {
        if (g_pclears[i].used && g_pclears[i].cb == cb &&
            g_pclears[i].image == image && !g_pclears[i].is_depth) {
            slot = i;
            break;
        }
        if (!g_pclears[i].used && slot < 0)
            slot = i;
    }
    if (slot >= 0) {
        g_pclears[slot].used = 1;
        g_pclears[slot].cb = cb;
        g_pclears[slot].image = image;
        g_pclears[slot].layout = layout;
        g_pclears[slot].is_depth = 0;
        g_pclears[slot].color = *color;
        g_pclears[slot].range = *range;
    }
    pthread_mutex_unlock(&g_pclear_mtx);
}

static void pclear_add_depth(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
                             const VkClearDepthStencilValue *depth, const VkImageSubresourceRange *range)
{
    pthread_mutex_lock(&g_pclear_mtx);
    int slot = -1;
    for (int i = 0; i < MAX_PENDING_CLEARS; i++) {
        if (g_pclears[i].used && g_pclears[i].cb == cb &&
            g_pclears[i].image == image && g_pclears[i].is_depth) {
            slot = i;
            break;
        }
        if (!g_pclears[i].used && slot < 0)
            slot = i;
    }
    if (slot >= 0) {
        g_pclears[slot].used = 1;
        g_pclears[slot].cb = cb;
        g_pclears[slot].image = image;
        g_pclears[slot].layout = layout;
        g_pclears[slot].is_depth = 1;
        g_pclears[slot].depth = *depth;
        g_pclears[slot].range = *range;
    }
    pthread_mutex_unlock(&g_pclear_mtx);
}

static void pclear_flush_image(VkCommandBuffer cb, VkImage image)
{
    struct pending_clear to_flush[2];
    int nflush = 0;
    pthread_mutex_lock(&g_pclear_mtx);
    for (int i = 0; i < MAX_PENDING_CLEARS; i++) {
        if (g_pclears[i].used && g_pclears[i].cb == cb &&
            (image == VK_NULL_HANDLE || g_pclears[i].image == image)) {
            if (nflush < 2)
                to_flush[nflush++] = g_pclears[i];
            g_pclears[i].used = 0;
        }
    }
    pthread_mutex_unlock(&g_pclear_mtx);
    for (int i = 0; i < nflush; i++) {
        if (to_flush[i].is_depth) {
            if (real_CmdClearDepthStencilImage)
                real_CmdClearDepthStencilImage(cb, to_flush[i].image, to_flush[i].layout,
                                               &to_flush[i].depth, 1, &to_flush[i].range);
        } else {
            if (real_CmdClearColorImage)
                real_CmdClearColorImage(cb, to_flush[i].image, to_flush[i].layout,
                                        &to_flush[i].color, 1, &to_flush[i].range);
        }
    }
}

static void resolve_copyfns(void)
{
    static VkDevice bound_dev;
    if (!g_dev || (real_CmdCopyImage && bound_dev == g_dev))
        return;
    bound_dev = g_dev;
#define RQ(name) \
    real_##name = (PFN_vk##name)(next_gdpa ? next_gdpa(g_dev, "vk" #name) : dev_lookup("vk" #name))
    RQ(CmdCopyImage);
    RQ(CmdCopyImageToBuffer);
    RQ(CmdBlitImage);
    RQ(CmdPipelineBarrier);
    RQ(CmdClearColorImage);
    RQ(CmdClearDepthStencilImage);
    RQ(EndCommandBuffer);
    real_QueueSubmit2KHR = (PFN_vkQueueSubmit2KHR)(next_gdpa ? next_gdpa(g_dev, "vkQueueSubmit2KHR") : dev_lookup("vkQueueSubmit2KHR"));
    if (!real_QueueSubmit2KHR)
        real_QueueSubmit2KHR = (PFN_vkQueueSubmit2KHR)(next_gdpa ? next_gdpa(g_dev, "vkQueueSubmit2") : dev_lookup("vkQueueSubmit2"));
    real_WaitSemaphores = (PFN_vkWaitSemaphores)(next_gdpa ? next_gdpa(g_dev, "vkWaitSemaphores") : dev_lookup("vkWaitSemaphores"));
    if (!real_WaitSemaphores)
        real_WaitSemaphores = (PFN_vkWaitSemaphores)(next_gdpa ? next_gdpa(g_dev, "vkWaitSemaphoresKHR") : dev_lookup("vkWaitSemaphoresKHR"));
    RQ(CmdCopyBufferToImage2);
    if (!real_CmdCopyBufferToImage2 && next_gdpa)
        real_CmdCopyBufferToImage2 = (PFN_vkCmdCopyBufferToImage2)next_gdpa(g_dev, "vkCmdCopyBufferToImage2KHR");
    RQ(CmdCopyImageToBuffer2);
    if (!real_CmdCopyImageToBuffer2 && next_gdpa)
        real_CmdCopyImageToBuffer2 = (PFN_vkCmdCopyImageToBuffer2)next_gdpa(g_dev, "vkCmdCopyImageToBuffer2KHR");
    RQ(CmdCopyImage2);
    if (!real_CmdCopyImage2 && next_gdpa)
        real_CmdCopyImage2 = (PFN_vkCmdCopyImage2)next_gdpa(g_dev, "vkCmdCopyImage2KHR");
    RQ(CmdBlitImage2);
    if (!real_CmdBlitImage2 && next_gdpa)
        real_CmdBlitImage2 = (PFN_vkCmdBlitImage2)next_gdpa(g_dev, "vkCmdBlitImage2KHR");
    RQ(AllocateCommandBuffers);
    RQ(FreeCommandBuffers);
    RQ(BeginCommandBuffer);
    RQ(ResetCommandBuffer);
    RQ(ResetCommandPool);
    real_DestroyCommandPoolH = (PFN_vkDestroyCommandPool)(next_gdpa ? next_gdpa(g_dev, "vkDestroyCommandPool") : dev_lookup("vkDestroyCommandPool"));
    RQ(CmdBeginRenderPass);
    RQ(CmdEndRenderPass);
    RQ(CmdBeginRenderPass2);
    if (!real_CmdBeginRenderPass2 && next_gdpa)
        real_CmdBeginRenderPass2 = (PFN_vkCmdBeginRenderPass2)next_gdpa(g_dev, "vkCmdBeginRenderPass2KHR");
    RQ(CmdEndRenderPass2);
    if (!real_CmdEndRenderPass2 && next_gdpa)
        real_CmdEndRenderPass2 = (PFN_vkCmdEndRenderPass2)next_gdpa(g_dev, "vkCmdEndRenderPass2KHR");
    RQ(CmdBeginRendering);
    if (!real_CmdBeginRendering && next_gdpa)
        real_CmdBeginRendering = (PFN_vkCmdBeginRendering)next_gdpa(g_dev, "vkCmdBeginRenderingKHR");
    RQ(CmdEndRendering);
    if (!real_CmdEndRendering && next_gdpa)
        real_CmdEndRendering = (PFN_vkCmdEndRendering)next_gdpa(g_dev, "vkCmdEndRenderingKHR");
    RQ(CmdBindVertexBuffers);
    real_CreateBufferH = (PFN_vkCreateBuffer)(next_gdpa ? next_gdpa(g_dev, "vkCreateBuffer") : dev_lookup("vkCreateBuffer"));
#undef RQ
    if (next_gdpa)
        cs_bind(g_dev, next_gdpa);
    DBG("copysec: routing active");
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

static void emit_blit_barrier(VkCommandBuffer cb)
{
    if (!real_CmdPipelineBarrier || cs_in_pass(cb))
        return;
    VkMemoryBarrier mb = {0};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_TRANSFER_WRITE_BIT |
                       VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                       VK_ACCESS_TRANSFER_WRITE_BIT |
                       VK_ACCESS_MEMORY_READ_BIT;
    real_CmdPipelineBarrier(cb,
                            VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 1, &mb, 0, NULL, 0, NULL);
}

static void VKAPI_CALL wrapped_CmdBlitImage(VkCommandBuffer cb, VkImage src,
                                            VkImageLayout sl, VkImage dst,
                                            VkImageLayout dl, uint32_t n,
                                            const VkImageBlit *r, VkFilter f)
{
    resolve_device();
    resolve_copyfns();
    static int blit_copysec = -1;
    if (blit_copysec < 0) {
        const char *e = getenv("VKC_BLIT_COPYSEC");
        blit_copysec = (e && e[0] == '1') ? 1 : 0;
    }
    if (blit_copysec && cs_route_blit(cb, src, sl, dst, dl, n, r, f))
        return;
    pclear_flush_image(cb, src);
    emit_blit_barrier(cb);
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
    static int blit_copysec = -1;
    if (blit_copysec < 0) {
        const char *e = getenv("VKC_BLIT_COPYSEC");
        blit_copysec = (e && e[0] == '1') ? 1 : 0;
    }
    if (blit_copysec && i && cs_route_blit2(cb, i->srcImage, i->srcImageLayout, i->dstImage,
                            i->dstImageLayout, i->regionCount, i->pRegions,
                            i->filter))
        return;
    emit_blit_barrier(cb);
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
    pclear_reset_cb(cb);
    { static int dbgN; if (++dbgN <= 3) DBG("begin #%d: done=%d ubo=%d ptr=%p", dbgN, g_atlas_done, g_ubo_done, (void *)g_atlas_ptr); }
    if (g_debug && (g_atlas_done >= 1 || g_ubo_done) && g_atlas_ptr) {
        DBG("DUMPING atlas.ppm");
        FILE *f = fopen("/tmp/atlas.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n1024 1024\n255\n");
            for (int y = 1023; y >= 0; y--)
                fwrite(g_atlas_ptr + (size_t)y * 1024 * 4, 1, 1024 * 3, f);
            fclose(f);
        }
        g_atlas_done++;
        if (g_atlas_done == 2)
            DBG("atlas dumped to /tmp/atlas.ppm");
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
    pclear_reset_cb(cb);
    { static int dbgN; if (++dbgN <= 3) DBG("reset #%d: done=%d ubo=%d ptr=%p", dbgN, g_atlas_done, g_ubo_done, (void *)g_atlas_ptr); }
    if (g_debug && (g_atlas_done >= 1 || g_ubo_done) && g_atlas_ptr) {
        FILE *f = fopen("/tmp/atlas.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n1024 1024\n255\n");
            for (int y = 1023; y >= 0; y--)
                fwrite(g_atlas_ptr + (size_t)y * 1024 * 4, 1, 1024 * 3, f);
            fclose(f);
        }
        DBG("DUMPING atlas.ppm (at reset)");
        g_atlas_done = 0;
        g_ubo_done = 0;
    }
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
    pclear_reset_all();
    if (real_ResetCommandPool)
        real_ResetCommandPool(device, pool, f);
}

static void VKAPI_CALL wrapped_DestroyCommandPool(VkDevice device,
                                                  VkCommandPool pool,
                                                  const VkAllocationCallbacks *a)
{
    resolve_device();
    resolve_copyfns();
    cs_track_destroypool(device, pool);
    if (real_DestroyCommandPoolH)
        real_DestroyCommandPoolH(device, pool, a);
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

static void VKAPI_CALL wrapped_CmdClearColorImage(
    VkCommandBuffer cb, VkImage image, VkImageLayout imageLayout,
    const VkClearColorValue *pColor, uint32_t rangeCount,
    const VkImageSubresourceRange *pRanges)
{
    resolve_device();
    resolve_copyfns();
    static int fold_clear = -1;
    if (fold_clear < 0) {
        const char *e = getenv("VKC_NO_FOLD_CLEAR");
        fold_clear = (e && e[0] == '1') ? 0 : 1;
    }
    if (fold_clear && !cs_in_pass(cb) && pColor && rangeCount == 1 && pRanges) {
        pclear_add_color(cb, image, imageLayout, pColor, &pRanges[0]);
        return;
    }
    if (real_CmdClearColorImage)
        real_CmdClearColorImage(cb, image, imageLayout, pColor, rangeCount, pRanges);
}

static void VKAPI_CALL wrapped_CmdClearDepthStencilImage(
    VkCommandBuffer cb, VkImage image, VkImageLayout imageLayout,
    const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount,
    const VkImageSubresourceRange *pRanges)
{
    resolve_device();
    resolve_copyfns();
    static int fold_clear = -1;
    if (fold_clear < 0) {
        const char *e = getenv("VKC_NO_FOLD_CLEAR");
        fold_clear = (e && e[0] == '1') ? 0 : 1;
    }
    if (fold_clear && !cs_in_pass(cb) && pDepthStencil && rangeCount == 1 && pRanges) {
        pclear_add_depth(cb, image, imageLayout, pDepthStencil, &pRanges[0]);
        return;
    }
    if (real_CmdClearDepthStencilImage)
        real_CmdClearDepthStencilImage(cb, image, imageLayout, pDepthStencil, rangeCount, pRanges);
}

static VkResult VKAPI_CALL wrapped_EndCommandBuffer(VkCommandBuffer cb)
{
    resolve_device();
    resolve_copyfns();
    pclear_flush_image(cb, VK_NULL_HANDLE);
    if (real_EndCommandBuffer)
        return real_EndCommandBuffer(cb);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static void VKAPI_CALL wrapped_CmdBeginRenderingTracking(
    VkCommandBuffer cb, const VkRenderingInfo *i)
{
    resolve_copyfns();
    if (!i) {
        cs_track_beginpass(cb);
        if (real_CmdBeginRendering)
            real_CmdBeginRendering(cb, i);
        return;
    }

    VkRenderingInfo ri = *i;
    VkRenderingAttachmentInfo color_atts[8];
    VkRenderingAttachmentInfo depth_att;
    int modified = 0;
    int full_area = (i->renderArea.offset.x == 0 && i->renderArea.offset.y == 0);

    if (i->colorAttachmentCount > 0 && i->colorAttachmentCount <= 8 && i->pColorAttachments) {
        memcpy(color_atts, i->pColorAttachments,
               i->colorAttachmentCount * sizeof(VkRenderingAttachmentInfo));
        for (uint32_t k = 0; k < i->colorAttachmentCount; k++) {
            VkImage img = (VkImage)vimg((void *)color_atts[k].imageView);
            if (!img)
                continue;
            if (!full_area && color_atts[k].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
                pclear_flush_image(cb, img);
                continue;
            }
            pthread_mutex_lock(&g_pclear_mtx);
            for (int p = 0; p < MAX_PENDING_CLEARS; p++) {
                if (g_pclears[p].used && g_pclears[p].cb == cb &&
                    g_pclears[p].image == img && !g_pclears[p].is_depth) {
                    if (color_atts[k].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
                        color_atts[k].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                        color_atts[k].clearValue.color = g_pclears[p].color;
                        modified = 1;
                    }
                    g_pclears[p].used = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&g_pclear_mtx);
        }
        if (modified)
            ri.pColorAttachments = color_atts;
    }

    if (i->pDepthAttachment && i->pDepthAttachment->imageView) {
        depth_att = *i->pDepthAttachment;
        VkImage img = (VkImage)vimg((void *)depth_att.imageView);
        if (img) {
            if (!full_area && depth_att.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
                pclear_flush_image(cb, img);
            } else {
                pthread_mutex_lock(&g_pclear_mtx);
                for (int p = 0; p < MAX_PENDING_CLEARS; p++) {
                    if (g_pclears[p].used && g_pclears[p].cb == cb &&
                        g_pclears[p].image == img && g_pclears[p].is_depth) {
                        if (depth_att.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
                            depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                            depth_att.clearValue.depthStencil = g_pclears[p].depth;
                            ri.pDepthAttachment = &depth_att;
                            modified = 1;
                        }
                        g_pclears[p].used = 0;
                        break;
                    }
                }
                pthread_mutex_unlock(&g_pclear_mtx);
            }
        }
    }

    /* Flush any remaining pending color clears on this command buffer in case
     * the cleared texture is sampled by this render pass. */
    struct pending_clear extra[8];
    int nextra = 0;
    pthread_mutex_lock(&g_pclear_mtx);
    for (int p = 0; p < MAX_PENDING_CLEARS; p++) {
        if (g_pclears[p].used && g_pclears[p].cb == cb && !g_pclears[p].is_depth) {
            if (nextra < 8)
                extra[nextra++] = g_pclears[p];
            g_pclears[p].used = 0;
        }
    }
    pthread_mutex_unlock(&g_pclear_mtx);
    for (int p = 0; p < nextra; p++) {
        if (real_CmdClearColorImage)
            real_CmdClearColorImage(cb, extra[p].image, extra[p].layout,
                                    &extra[p].color, 1, &extra[p].range);
    }

    cs_track_beginpass(cb);
    if (real_CmdBeginRendering)
        real_CmdBeginRendering(cb, modified ? &ri : i);
}

static void VKAPI_CALL wrapped_CmdEndRenderingTracking(VkCommandBuffer cb)
{
    resolve_copyfns();
    cs_track_endpass(cb);
    if (real_CmdEndRendering)
        real_CmdEndRendering(cb);
}

static void VKAPI_CALL wrapped_CmdBindVertexBuffers(VkCommandBuffer cb,
                                                  uint32_t firstBinding,
                                                  uint32_t bindingCount,
                                                  const VkBuffer *pBuffers,
                                                  const VkDeviceSize *pOffsets)
{
    resolve_device();
    resolve_copyfns();
    for (uint32_t i = 0; i < bindingCount; i++)
        if (pOffsets[i])
            DBG("vertex binding %u offset=%llu", firstBinding + i,
                (unsigned long long)pOffsets[i]);
    if (real_CmdBindVertexBuffers)
        real_CmdBindVertexBuffers(cb, firstBinding, bindingCount, pBuffers, pOffsets);
}

static PFN_vkGetImageSubresourceLayout real_GetImageSubresourceLayout;
static VkResult VKAPI_CALL wrapped_GetImageSubresourceLayout(
    VkDevice device, VkImage image, const VkImageSubresource *pSubresource,
    VkSubresourceLayout *pLayout)
{
    resolve_device();
    resolve_copyfns();
    static int dbgN;
    if (++dbgN <= 5)
        DBG("GetImageSubresourceLayout #%d image=%p mip=%u layer=%u",
            dbgN, (void *)image, pSubresource ? pSubresource->mipLevel : 0,
            pSubresource ? pSubresource->arrayLayer : 0);
    if (!real_GetImageSubresourceLayout)
        real_GetImageSubresourceLayout =
            (PFN_vkGetImageSubresourceLayout)(next_gdpa ? next_gdpa(g_dev, "vkGetImageSubresourceLayout") : NULL);
    if (real_GetImageSubresourceLayout)
        real_GetImageSubresourceLayout(device, image, pSubresource, pLayout);
    else if (pLayout)
        memset(pLayout, 0, sizeof(*pLayout));
    return VK_SUCCESS;
}

static void VKAPI_CALL wrapped_DestroyDeviceHook(VkDevice device,
                                                 const VkAllocationCallbacks *a)
{
    resolve_device();
    cs_destroy();
    if (!real_DestroyDevice && next_gdpa && device)
        real_DestroyDevice = (PFN_vkDestroyDevice)next_gdpa(device, "vkDestroyDevice");
    if (real_DestroyDevice)
        real_DestroyDevice(device, a);
    if (g_dev == device) {
        g_dev = VK_NULL_HANDLE;
        real_CreateShaderModule = NULL;
        real_CmdCopyImage = NULL;
    }
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
    const char *linopt = getenv("VKC_LINEAR_OPTIMAL");
    const char *nlin = getenv("VKC_NO_LINEAR_OPTIMAL");
    int want_optimal = (linopt && linopt[0] == '1') && !(nlin && nlin[0] == '1');
    if (pCreateInfo && pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR &&
        (pCreateInfo->usage & VK_IMAGE_USAGE_SAMPLED_BIT) && want_optimal) {
        patched = *pCreateInfo;
        patched.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci = &patched;
        DBG("image tiling forced optimal: %dx%d format=%d", pCreateInfo->extent.width,
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

static VkBufferCreateInfo g_bufs[65536];
static VkBuffer g_bufh[65536];
static int g_nbufs;
static VkResult VKAPI_CALL wrapped_CreateBuffer(VkDevice device,
                                               const VkBufferCreateInfo *pCreateInfo,
                                               const VkAllocationCallbacks *pAllocator,
                                               VkBuffer *pBuffer)
{
    resolve_device();
    resolve_copyfns();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_CreateBufferH)
        r = real_CreateBufferH(device, pCreateInfo, pAllocator, pBuffer);
    if (r == VK_SUCCESS && pBuffer && pCreateInfo && g_nbufs < 65536) {
        g_bufs[g_nbufs] = *pCreateInfo;
        g_bufh[g_nbufs] = *pBuffer;
        g_nbufs++;
    }
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
        DBG("buffer view %p: buffer=%p offset=%llu range=%llu format=%d",
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
            DBG("push descriptor: unknown buffer view %p - leaving it alone", (void *)h);
            continue;
        }
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pImageInfo = NULL;
        w[i].pTexelBufferView = NULL;
        w[i].pBufferInfo = &infos[i];
        DBG("push descriptor: texel buffer %p -> storage buffer (buffer=%p offset=%llu range=%llu)",
            (void *)h, (void *)bi->buffer, (unsigned long long)bi->offset,
            (unsigned long long)bi->range);
    }
}

static int desc_write_is_null(const VkWriteDescriptorSet *w)
{
    if (!w || w->descriptorCount == 0)
        return 1;
    switch (w->descriptorType) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        if (!w->pBufferInfo)
            return 1;
        for (uint32_t i = 0; i < w->descriptorCount; i++)
            if (w->pBufferInfo[i].buffer == VK_NULL_HANDLE)
                return 1;
        return 0;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        if (!w->pTexelBufferView)
            return 1;
        for (uint32_t i = 0; i < w->descriptorCount; i++)
            if (w->pTexelBufferView[i] == VK_NULL_HANDLE)
                return 1;
        return 0;
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        if (!w->pImageInfo)
            return 1;
        for (uint32_t i = 0; i < w->descriptorCount; i++)
            if (w->pImageInfo[i].imageView == VK_NULL_HANDLE)
                return 1;
        return 0;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        if (!w->pImageInfo)
            return 1;
        for (uint32_t i = 0; i < w->descriptorCount; i++)
            if (w->pImageInfo[i].imageView == VK_NULL_HANDLE ||
                w->pImageInfo[i].sampler == VK_NULL_HANDLE)
                return 1;
        return 0;
    case VK_DESCRIPTOR_TYPE_SAMPLER:
        if (!w->pImageInfo)
            return 1;
        for (uint32_t i = 0; i < w->descriptorCount; i++)
            if (w->pImageInfo[i].sampler == VK_NULL_HANDLE)
                return 1;
        return 0;
    default:
        return 0;
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
    int has_texel = 0;
    int has_null = 0;
    if (pDescriptorWrites) {
        for (uint32_t i = 0; i < descriptorWriteCount; i++) {
            if (pDescriptorWrites[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
                has_texel = 1;
            if (desc_write_is_null(&pDescriptorWrites[i]))
                has_null = 1;
        }
    }
    if (!has_texel && !has_null && !g_debug) {
        if (real_CmdPushDescriptorSetKHR)
            real_CmdPushDescriptorSetKHR(commandBuffer, pipelineBindPoint, layout, set,
                                         descriptorWriteCount, pDescriptorWrites);
        return;
    }
    VkWriteDescriptorSet w[64];
    VkDescriptorBufferInfo infos[64];
    VkBufferView views[64];
    if (descriptorWriteCount <= 64 && pDescriptorWrites) {
        uint32_t out_cnt = 0;
        for (uint32_t i = 0; i < descriptorWriteCount; i++) {
            if (desc_write_is_null(&pDescriptorWrites[i]))
                continue;
            w[out_cnt++] = pDescriptorWrites[i];
        }
        if (out_cnt == 0)
            return;
        memset(infos, 0, out_cnt * sizeof(infos[0]));
        memset(views, 0, out_cnt * sizeof(views[0]));
        rewrite_texel_buffers(w, out_cnt, infos, views);
        if (real_CmdPushDescriptorSetKHR)
            real_CmdPushDescriptorSetKHR(commandBuffer, pipelineBindPoint, layout, set,
                                         out_cnt, w);
        return;
    }
    if (real_CmdPushDescriptorSetKHR)
        real_CmdPushDescriptorSetKHR(commandBuffer, pipelineBindPoint, layout, set,
                                     descriptorWriteCount, pDescriptorWrites);
}

static void VKAPI_CALL wrapped_UpdateDescriptorSets(VkDevice device,
                                                    uint32_t descriptorWriteCount,
                                                    const VkWriteDescriptorSet *pDescriptorWrites,
                                                    uint32_t descriptorCopyCount,
                                                    const VkCopyDescriptorSet *pDescriptorCopies)
{
    resolve_device();
    int has_texel = 0;
    int has_null = 0;
    if (pDescriptorWrites) {
        for (uint32_t i = 0; i < descriptorWriteCount; i++) {
            if (pDescriptorWrites[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
                has_texel = 1;
            if (desc_write_is_null(&pDescriptorWrites[i]))
                has_null = 1;
        }
    }
    if (!has_texel && !has_null && !g_debug) {
        if (real_UpdateDescriptorSets)
            real_UpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites,
                                      descriptorCopyCount, pDescriptorCopies);
        return;
    }
    VkWriteDescriptorSet w[64];
    VkDescriptorBufferInfo infos[64];
    VkBufferView views[64];
    if (descriptorWriteCount <= 64 && pDescriptorWrites) {
        uint32_t out_cnt = 0;
        for (uint32_t i = 0; i < descriptorWriteCount; i++) {
            if (desc_write_is_null(&pDescriptorWrites[i]))
                continue;
            w[out_cnt++] = pDescriptorWrites[i];
        }
        if (out_cnt == 0 && descriptorCopyCount == 0)
            return;
        memset(infos, 0, out_cnt * sizeof(infos[0]));
        memset(views, 0, out_cnt * sizeof(views[0]));
        rewrite_texel_buffers(w, out_cnt, infos, views);
        if (real_UpdateDescriptorSets)
            real_UpdateDescriptorSets(device, out_cnt, w, descriptorCopyCount,
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
    if (!g_debug) {
        return real_AcquireNextImageKHR
            ? real_AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex)
            : VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t t0 = now_ms();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_AcquireNextImageKHR)
        r = real_AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    uint64_t dt = now_ms() - t0;
    if (r == VK_SUCCESS) {
        if (dt > 50)
            DBG("AcquireNextImageKHR slow: %llu ms", (unsigned long long)dt);
    } else {
        DBG("AcquireNextImageKHR r=%d after %llu ms", r, (unsigned long long)dt);
    }
    return r;
}

static VkResult VKAPI_CALL wrapped_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    resolve_device();
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (real_QueuePresentKHR)
        r = real_QueuePresentKHR(queue, pPresentInfo);
    if (g_debug) {
        if (r != VK_SUCCESS)
            DBG("QueuePresentKHR r=%d", r);
        static uint64_t frames;
        static uint64_t t_start;
        static uint64_t t_last;
        frames++;
        uint64_t t = now_ms();
        if (t_start == 0) {
            t_start = t;
            t_last = t;
        } else if (t - t_last >= 2000) {
            DBG("FPS: %.1f (%llu frames in %llu ms)", (double)frames * 1000.0 / (double)(t - t_start),
                (unsigned long long)frames, (unsigned long long)(t - t_start));
            frames = 0;
            t_start = t;
            t_last = t;
        }
    }
    return r;
}

static VkResult VKAPI_CALL wrapped_QueueSubmit2KHR(VkQueue queue,
                                                   uint32_t submitCount,
                                                   const VkSubmitInfo2 *pSubmits,
                                                   VkFence fence)
{
    resolve_device();
    resolve_copyfns();
    if (!real_QueueSubmit2KHR)
        return VK_ERROR_INITIALIZATION_FAILED;

    static int sync_submit = -1;
    if (sync_submit < 0) {
        const char *e = getenv("VKC_NO_SYNC_SUBMIT");
        sync_submit = (e && e[0] == '1') ? 0 : 1;
    }

    if (sync_submit && pSubmits && submitCount > 0) {
        /*
         * Wait for the previous frame's timeline semaphore (value - 1) BEFORE
         * submitting the new frame so Frame N does not clear the single shared
         * mainRenderTarget while Frame N-1 is still rendering or blitting it.
         */
        if (real_WaitSemaphores && g_dev) {
            for (uint32_t i = 0; i < submitCount; i++) {
                for (uint32_t j = 0; j < pSubmits[i].signalSemaphoreInfoCount; j++) {
                    const VkSemaphoreSubmitInfo *si = &pSubmits[i].pSignalSemaphoreInfos[j];
                    if (si->semaphore && si->value >= 2) {
                        uint64_t prev_val = si->value - 1;
                        VkSemaphoreWaitInfo wi = {0};
                        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
                        wi.semaphoreCount = 1;
                        wi.pSemaphores = &si->semaphore;
                        wi.pValues = &prev_val;
                        real_WaitSemaphores(g_dev, &wi, 5000000000ULL);
                    }
                }
            }
        }

        /*
         * When VulkanQueue$Submission splits mainRenderCmdBuf (submits[0]) and
         * blitCmdBuf (submits[1]) into separate VkSubmitInfo2 entries without
         * intermediate semaphore signals, merge them into a single VkSubmitInfo2
         * so PowerVR's 3D and Transfer engines serialize them in one submission.
         */
        if (submitCount > 1 && submitCount <= 8) {
            uint32_t nw = 0, nc = 0, ns = 0;
            VkSubmitFlags flags = 0;
            int can_merge = 1;
            for (uint32_t i = 0; i < submitCount; i++) {
                if (pSubmits[i].pNext != NULL)
                    can_merge = 0;
                if (i + 1 < submitCount && pSubmits[i].signalSemaphoreInfoCount > 0)
                    can_merge = 0;
                nw += pSubmits[i].waitSemaphoreInfoCount;
                nc += pSubmits[i].commandBufferInfoCount;
                ns += pSubmits[i].signalSemaphoreInfoCount;
                flags |= pSubmits[i].flags;
            }
            if (can_merge && nw <= 16 && nc <= 32 && ns <= 16) {
                VkSemaphoreSubmitInfo waits[16];
                VkCommandBufferSubmitInfo cbs[32];
                VkSemaphoreSubmitInfo sigs[16];
                uint32_t wi = 0, ci = 0, si = 0;
                for (uint32_t i = 0; i < submitCount; i++) {
                    for (uint32_t j = 0; j < pSubmits[i].waitSemaphoreInfoCount; j++)
                        waits[wi++] = pSubmits[i].pWaitSemaphoreInfos[j];
                    for (uint32_t j = 0; j < pSubmits[i].commandBufferInfoCount; j++)
                        cbs[ci++] = pSubmits[i].pCommandBufferInfos[j];
                    for (uint32_t j = 0; j < pSubmits[i].signalSemaphoreInfoCount; j++)
                        sigs[si++] = pSubmits[i].pSignalSemaphoreInfos[j];
                }
                VkSubmitInfo2 merged = {0};
                merged.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
                merged.flags = flags;
                merged.waitSemaphoreInfoCount = wi;
                merged.pWaitSemaphoreInfos = wi ? waits : NULL;
                merged.commandBufferInfoCount = ci;
                merged.pCommandBufferInfos = ci ? cbs : NULL;
                merged.signalSemaphoreInfoCount = si;
                merged.pSignalSemaphoreInfos = si ? sigs : NULL;
                return real_QueueSubmit2KHR(queue, 1, &merged, fence);
            }
        }
    }

    return real_QueueSubmit2KHR(queue, submitCount, pSubmits, fence);
}

/* ------------------------------------------------------------------ */
/* dispatch interception                                               */
/* ------------------------------------------------------------------ */

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance,
                                                                         const char *name);

static int want_copysec(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *no = getenv("VKC_NO_COPYSEC");
        cached = (no && no[0] == '1') ? 0 : 1; /* default ON */
    }
    return cached;
}

static int want_linear_optimal(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *linopt = getenv("VKC_LINEAR_OPTIMAL");
        const char *nlin = getenv("VKC_NO_LINEAR_OPTIMAL");
        const char *logi = getenv("VKC_LOG_IMAGES");
        cached = (((linopt && linopt[0] == '1') && !(nlin && nlin[0] == '1')) ||
                  (logi && logi[0] == '1')) ? 1 : 0;
    }
    return cached;
}

static PFN_vkVoidFunction intercept(const char *name)
{
    if (!name)
        return NULL;
    DBG("intercept query: %s", name);
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateInstance;
    if (strcmp(name, "vkDestroyInstance") == 0)
        return (PFN_vkVoidFunction)wrapped_DestroyInstance;
    if (strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
        return (PFN_vkVoidFunction)wrapped_EnumerateDeviceExtensionProperties;
    if (strcmp(name, "vkGetPhysicalDeviceFeatures2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return (PFN_vkVoidFunction)wrapped_GetPhysicalDeviceFeatures2;
    if (strcmp(name, "vkGetPhysicalDeviceProperties2") == 0 ||
        strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0)
        return (PFN_vkVoidFunction)wrapped_GetPhysicalDeviceProperties2;
    if (strcmp(name, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateDevice;
    if (strcmp(name, "vkDestroyDevice") == 0)
        return (PFN_vkVoidFunction)wrapped_DestroyDeviceHook;
    if (strcmp(name, "vkCreateGraphicsPipelines") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateGraphicsPipelines;
    if (strcmp(name, "vkCreateComputePipelines") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateComputePipelines;
    if (strcmp(name, "vkCreatePipelineLayout") == 0)
        return (PFN_vkVoidFunction)wrapped_CreatePipelineLayout;
    if (strcmp(name, "vkCreateDescriptorSetLayout") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateDescriptorSetLayout;
    if (strcmp(name, "vkCreateShaderModule") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateShaderModule;
    if (strcmp(name, "vkDestroyShaderModule") == 0)
        return (PFN_vkVoidFunction)wrapped_DestroyShaderModule;
    if (strcmp(name, "vkUpdateDescriptorSets") == 0)
        return (PFN_vkVoidFunction)wrapped_UpdateDescriptorSets;
    if (strcmp(name, "vkCreateBufferView") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateBufferView;
    if (strcmp(name, "vkCreateImageView") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateImageView;
    if (strcmp(name, "vkCreateSampler") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateSampler;
    if (strcmp(name, "vkCmdPushDescriptorSetKHR") == 0 ||
        strcmp(name, "vkCmdPushDescriptorSet") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdPushDescriptorSetKHR;
    if (strcmp(name, "vkQueueSubmit2KHR") == 0 ||
        strcmp(name, "vkQueueSubmit2") == 0)
        return (PFN_vkVoidFunction)wrapped_QueueSubmit2KHR;
    if (strcmp(name, "vkCmdClearColorImage") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdClearColorImage;
    if (strcmp(name, "vkCmdClearDepthStencilImage") == 0)
        return (PFN_vkVoidFunction)wrapped_CmdClearDepthStencilImage;
    if (strcmp(name, "vkEndCommandBuffer") == 0)
        return (PFN_vkVoidFunction)wrapped_EndCommandBuffer;
    if (strcmp(name, "vkCmdBindTransformFeedbackBuffersEXT") == 0)
        return (PFN_vkVoidFunction)noop_CmdBindTransformFeedbackBuffersEXT;
    if (strcmp(name, "vkCmdBeginTransformFeedbackEXT") == 0)
        return (PFN_vkVoidFunction)noop_CmdBeginTransformFeedbackEXT;
    if (strcmp(name, "vkCmdEndTransformFeedbackEXT") == 0)
        return (PFN_vkVoidFunction)noop_CmdEndTransformFeedbackEXT;
    if (strcmp(name, "vkCmdBeginQueryIndexedEXT") == 0)
        return (PFN_vkVoidFunction)noop_CmdBeginQueryIndexedEXT;
    if (strcmp(name, "vkCmdEndQueryIndexedEXT") == 0)
        return (PFN_vkVoidFunction)noop_CmdEndQueryIndexedEXT;
    if (strcmp(name, "vkCmdDrawIndirectByteCountEXT") == 0)
        return (PFN_vkVoidFunction)noop_CmdDrawIndirectByteCountEXT;
    if (want_linear_optimal() && strcmp(name, "vkCreateImage") == 0)
        return (PFN_vkVoidFunction)wrapped_CreateImage;
    if (want_copysec()) {
        if (strcmp(name, "vkCmdCopyBufferToImage") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdCopyBufferToImage;
        if (strcmp(name, "vkCmdCopyImage") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdCopyImage;
        if (strcmp(name, "vkCmdCopyImageToBuffer") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdCopyImageToBuffer;
        if (strcmp(name, "vkCmdBlitImage") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdBlitImage;
        if (strcmp(name, "vkCmdCopyBufferToImage2") == 0 ||
            strcmp(name, "vkCmdCopyBufferToImage2KHR") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdCopyBufferToImage2;
        if (strcmp(name, "vkCmdCopyImageToBuffer2") == 0 ||
            strcmp(name, "vkCmdCopyImageToBuffer2KHR") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdCopyImageToBuffer2;
        if (strcmp(name, "vkCmdCopyImage2") == 0 ||
            strcmp(name, "vkCmdCopyImage2KHR") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdCopyImage2;
        if (strcmp(name, "vkCmdBlitImage2") == 0 ||
            strcmp(name, "vkCmdBlitImage2KHR") == 0)
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
        if (strcmp(name, "vkDestroyCommandPool") == 0)
            return (PFN_vkVoidFunction)wrapped_DestroyCommandPool;
        if (strcmp(name, "vkCmdBeginRenderPass") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdBeginRenderPassTracking;
        if (strcmp(name, "vkCmdEndRenderPass") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdEndRenderPassTracking;
        if (strcmp(name, "vkCmdBeginRenderPass2") == 0 ||
            strcmp(name, "vkCmdBeginRenderPass2KHR") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdBeginRenderPass2Tracking;
        if (strcmp(name, "vkCmdEndRenderPass2") == 0 ||
            strcmp(name, "vkCmdEndRenderPass2KHR") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdEndRenderPass2Tracking;
        if (strcmp(name, "vkCmdBeginRendering") == 0 ||
            strcmp(name, "vkCmdBeginRenderingKHR") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdBeginRenderingTracking;
        if (strcmp(name, "vkCmdEndRendering") == 0 ||
            strcmp(name, "vkCmdEndRenderingKHR") == 0)
            return (PFN_vkVoidFunction)wrapped_CmdEndRenderingTracking;
    }
    if (g_debug) {
        if (strcmp(name, "vkAcquireNextImageKHR") == 0)
            return (PFN_vkVoidFunction)wrapped_AcquireNextImageKHR;
        if (strcmp(name, "vkQueuePresentKHR") == 0)
            return (PFN_vkVoidFunction)wrapped_QueuePresentKHR;
    }
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
    DBG("GIPA-ENTRY %s", name ? name : "(null)");
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
    DBG("GDPA-ENTRY %s", name ? name : "(null)");
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
    setenv("MESA_GL_VERSION_OVERRIDE", "4.5", 0);
    setenv("MESA_GLSL_VERSION_OVERRIDE", "450", 0);
    setenv("MESA_GLES_VERSION_OVERRIDE", "3.2", 0);
    const char *env = getenv("VKC_DIR");
    if (env && *env)
        snprintf(g_dir, sizeof(g_dir), "%s", env);
    g_debug = getenv("VKC_DEBUG") ? 1 : 0;
    if (g_debug)
        log_open();
    if (getenv("VKC_CAPTURE_STDERR")) {
        mkdir(g_dir, 0755);
        char sp[512];
        snprintf(sp, sizeof(sp), "%s/app-stderr.log", g_dir);
        int sfd = open(sp, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (sfd >= 0) {
            dup2(sfd, 1);
            dup2(sfd, 2);
        }
    }
    if (getenv("VKC_SEGV")) {
        mkdir(g_dir, 0755);
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
    g_retry = getenv("VKC_RETRY") ? atoi(getenv("VKC_RETRY")) : 3;
    g_strip_debug = getenv("VKC_STRIP_DEBUG") ? (atoi(getenv("VKC_STRIP_DEBUG")) ? 1 : 0) : 0;
    g_require_vulkan = getenv("VKC_REQUIRE_VULKAN") ? 1 : 0;

    const char *cands[] = {
        "/var/data/vk-img/libVK_IMG.so",
        "/lib/libVK_IMG.so",
        "/usr/lib/libVK_IMG.so",
        "/usr/lib/libVK_IMG.so.24.2.6603887",
        "/usr/lib/aarch64-linux-gnu/GL/default/lib/libVK_IMG.so",
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
    dl_iterate_phdr(uf_phdr_cb, NULL);

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

    DBG("shim_init: driver=%p gipa=%p gdpa=%p repair=%d requireVulkan=%d dir=%s", g_drv,
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
    PFN_vkGetPhysicalDeviceMemoryProperties rmp = NULL;
    if (next_gipa && layer_instance)
        rmp = (PFN_vkGetPhysicalDeviceMemoryProperties)next_gipa(layer_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!rmp)
        rmp = (PFN_vkGetPhysicalDeviceMemoryProperties)(g_drv_gipa ? g_drv_gipa(layer_instance, "vkGetPhysicalDeviceMemoryProperties") : NULL);
    if (!rmp)
        rmp = (PFN_vkGetPhysicalDeviceMemoryProperties)next_gdpa(g_dev, "vkGetPhysicalDeviceMemoryProperties");
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
    for (int i = 0; i < 40; i++) usleep(250000);
    LOG("atlas: allocate (after early-init sleep)...");
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
