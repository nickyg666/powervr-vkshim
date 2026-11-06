/* print the PowerVR device features we care about */
#define _GNU_SOURCE
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void)
{
    VkApplicationInfo ai; memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci; memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo = &ai;
    VkInstance inst;
    if (vkCreateInstance(&ci, 0, &inst) != VK_SUCCESS) { printf("instance failed\n"); return 1; }
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, 0);
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds)); vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceFeatures f; memset(&f, 0, sizeof(f));
    vkGetPhysicalDeviceFeatures(pd, &f);
    printf("shaderInt64=%d vertexPipelineStoresAndAtomics=%d\n", f.shaderInt64, f.vertexPipelineStoresAndAtomics);

    VkPhysicalDeviceVulkan13Features v13;
    memset(&v13, 0, sizeof(v13));
    v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features v12;
    memset(&v12, 0, sizeof(v12));
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan11Features v11;
    memset(&v11, 0, sizeof(v11));
    v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceFeatures2 f2; memset(&f2, 0, sizeof(f2));
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &v11; v11.pNext = &v12; v12.pNext = &v13;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    printf("v1.1: storageBuffer16Bit=%d uniformAndStorageBuffer16Bit=%d\n", v11.storageBuffer16BitAccess, v11.uniformAndStorageBuffer16BitAccess);
    printf("v1.2: storageBuffer8Bit=%d uniformAndStorageBuffer8Bit=%d\n",
           v12.storageBuffer8BitAccess, v12.uniformAndStorageBuffer8BitAccess);
    printf("v1.3: dynamicRendering=%d\n", v13.dynamicRendering);

    /* extensions */
    uint32_t en = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &en, NULL);
    VkExtensionProperties *ep = calloc(en, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &en, ep);
    for (uint32_t i = 0; i < en; i++)
        if (strstr(ep[i].extensionName, "8Bit") || strstr(ep[i].extensionName, "texel") ||
            strstr(ep[i].extensionName, "maintenance"))
            printf("ext: %s\n", ep[i].extensionName);

    /* format properties for R8_SINT as a uniform/texel buffer */
    VkFormatProperties fp; memset(&fp, 0, sizeof(fp));
    vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8_SINT, &fp);
    printf("R8_SINT: optimalTiling=%x buffer=%x\n", fp.optimalTilingFeatures, fp.bufferFeatures);
    return 0;
}
