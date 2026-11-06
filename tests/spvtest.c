/*
 * Minimal reproducer that talks to the PowerVR ICD directly (no Vulkan
 * loader in between) so we can see exactly what the driver does with a
 * given SPIR-V module.
 *
 *   spvtest <file.spv> [...]
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef PFN_vkVoidFunction (*PFN_icd_gipa)(void *, const char *);
typedef PFN_vkVoidFunction (*PFN_icd_gpdpa)(void *, const char *);
typedef VkResult(*PFN_negver)(uint32_t *);

static PFN_icd_gipa gipa;
static PFN_icd_gpdpa gpdpa;

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
    if (fread(w, 1, n, f) != (size_t)n) {
        fprintf(stderr, "short read\n");
        exit(1);
    }
    fclose(f);
    *words = n / 4;
    return w;
}

int main(int argc, char **argv)
{
    void *drv = dlopen("/usr/lib/libVK_IMG.so", RTLD_NOW | RTLD_LOCAL);
    if (!drv) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    PFN_icd_gipa icd_gipa = (PFN_icd_gipa)dlsym(drv, "vk_icdGetInstanceProcAddr");
    PFN_negver negver = (PFN_negver)dlsym(drv, "vk_icdNegotiateLoaderICDInterfaceVersion");
    uint32_t iv = 5;
    if (negver)
        negver(&iv);
    printf("ICD interface version %u\n", iv);

    gipa = (PFN_icd_gipa)icd_gipa(VK_NULL_HANDLE, "vkGetInstanceProcAddr");
    gpdpa = (PFN_icd_gpdpa)icd_gipa(VK_NULL_HANDLE, "vk_icdGetPhysicalDeviceProcAddr");
    printf("gipa=%p gpdpa=%p\n", (void *)gipa, (void *)gpdpa);

    PFN_vkCreateInstance pCreateInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;

    VkInstance inst;
    VkResult r = pCreateInstance(&ici, NULL, &inst);
    printf("vkCreateInstance -> %d\n", r);
    if (r != VK_SUCCESS)
        return 1;

    PFN_vkEnumeratePhysicalDevices pEnum = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    uint32_t n = 0;
    pEnum(inst, &n, NULL);
    printf("physical devices: %u\n", n);
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    pEnum(inst, &n, pds);
    VkPhysicalDevice pd = pds[0];
    PFN_vkGetPhysicalDeviceProperties pProps =
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    VkPhysicalDeviceProperties props;
    pProps(pd, &props);
    printf("device: %s\n", props.deviceName);

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
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    PFN_vkCreateDevice pCreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    VkDevice dev;
    r = pCreateDevice(pd, &dci, NULL, &dev);
    printf("vkCreateDevice -> %d\n", r);
    if (r != VK_SUCCESS)
        return 1;

    printf("  gipa(inst,vkCreateShaderModule) = %p\n", (void *)gipa(inst, "vkCreateShaderModule"));
    printf("  gpdpa(inst,vkCreateShaderModule) = %p\n", (void *)(gpdpa ? gpdpa(inst, "vkCreateShaderModule") : NULL));
    printf("  gipa(dev,vkCreateShaderModule)  = %p\n", (void *)gipa(dev, "vkCreateShaderModule"));
    printf("  gpdpa(dev,vkCreateShaderModule) = %p\n", (void *)(gpdpa ? gpdpa(dev, "vkCreateShaderModule") : NULL));
    PFN_vkVoidFunction tmp = gipa(dev, "vkCreateShaderModule");
    if (!tmp && gpdpa)
        tmp = gpdpa(dev, "vkCreateShaderModule");
    PFN_vkCreateShaderModule pCreateShaderModule = (PFN_vkCreateShaderModule)tmp;
    printf("vkCreateShaderModule = %p\n", (void *)pCreateShaderModule);
    if (!pCreateShaderModule)
        return 1;

    for (int i = 1; i < argc; i++) {
        size_t words;
        uint32_t *code = read_spv(argv[i], &words);
        printf("[%d] %s (%zu words)\n", i, argv[i], words);
        fflush(stdout);
        VkShaderModuleCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = words * 4;
        ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        r = pCreateShaderModule(dev, &ci, NULL, &m);
        printf("    -> %d (%p)\n", r, (void *)m);
        fflush(stdout);
        if (m != VK_NULL_HANDLE) {
            PFN_vkVoidFunction t2 = gpdpa(inst, "vkDestroyShaderModule");
            PFN_vkDestroyShaderModule pDestroy = (PFN_vkDestroyShaderModule)t2;
            if (pDestroy)
                pDestroy(dev, m, NULL);
        }
        free(code);
    }

    printf("all done\n");
    return 0;
}
