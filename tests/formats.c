/* check the format support of the PowerVR driver for the formats Minecraft uses */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void show(VkPhysicalDevice pd, VkFormat f, const char *name)
{
    VkFormatProperties p;
    memset(&p, 0, sizeof(p));
    vkGetPhysicalDeviceFormatProperties(pd, f, &p);
    printf("%-24s %-14s optimal=0x%08x buffer=0x%08x depthstencil=0x%08x\n", name,
           f == VK_FORMAT_R8G8B8A8_UNORM ? "RGBA8" :
           f == VK_FORMAT_R8G8B8A8_SRGB ? "RGBA8_SRGB" :
           f == VK_FORMAT_B8G8R8A8_UNORM ? "BGRA8" :
           f == VK_FORMAT_R16G16B16A16_SFLOAT ? "RGBA16F" :
           f == VK_FORMAT_R32G32B32A32_SFLOAT ? "RGBA32F" :
           f == VK_FORMAT_R8_UNORM ? "R8" :
           f == VK_FORMAT_R16_UNORM ? "R16" :
           f == VK_FORMAT_D32_SFLOAT ? "D32F" :
           f == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" :
           f == VK_FORMAT_D16_UNORM ? "D16" :
           f == VK_FORMAT_R8G8B8A8_UINT ? "RGBA8UI" : "?",
           p.optimalTilingFeatures, p.bufferFeatures, 0);
}

int main(void)
{
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    VkInstance inst;
    if (vkCreateInstance(&ci, 0, &inst) != VK_SUCCESS) return 1;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, 0);
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = pds[0];

    printf("SAMPLED=0x1 LINEAR=0x2 STORAGE=0x8 COLOR_ATTACH=0x10 DEPTH_ATTACH=0x20\n");
    show(pd, VK_FORMAT_R8G8B8A8_UNORM, "block atlas");
    show(pd, VK_FORMAT_R8G8B8A8_SRGB, "srgb target");
    show(pd, VK_FORMAT_B8G8R8A8_UNORM, "bgra8");
    show(pd, VK_FORMAT_R16G16B16A16_SFLOAT, "hdr16f");
    show(pd, VK_FORMAT_R32G32B32A32_SFLOAT, "hdr32f");
    show(pd, VK_FORMAT_R8_UNORM, "r8");
    show(pd, VK_FORMAT_R16_UNORM, "r16");
    show(pd, VK_FORMAT_R8G8B8A8_UINT, "rgba8ui");
    show(pd, VK_FORMAT_D32_SFLOAT, "depth D32F");
    show(pd, VK_FORMAT_D24_UNORM_S8_UINT, "depth D24S8");
    show(pd, VK_FORMAT_D16_UNORM, "depth D16");
    show(pd, VK_FORMAT_S8_UINT, "stencil s8");

    VkPhysicalDeviceFeatures f;
    memset(&f, 0, sizeof(f));
    vkGetPhysicalDeviceFeatures(pd, &f);
    printf("features: depthClamp=%d geometryShader=%d fragmentStoresAndAtomics=%d\n",
           f.depthClamp, f.geometryShader, f.fragmentStoresAndAtomics);
    return 0;
}
