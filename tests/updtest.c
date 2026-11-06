/*
 * vkCmdUpdateBuffer with dstOffset — does the driver honour it?
 * Minecraft writes many small dynamic uniforms with vkCmdUpdateBuffer at
 * different offsets; if offsets are ignored, every write lands at byte 0
 * and all shader constants become the last write (fog colour cycling,
 * global lighting garbage).
 *
 *   U1: two UpdateBuffers at offsets 0 and 64, read back whole buffer
 *   U2: UpdateBuffer at offset 128 only
 *
 *   DISABLE_VK_LAYER_POWERVR_COMPAT=1 ./updtest
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 256

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

#define BEGINCB(var)                                                       \
    VkCommandBufferAllocateInfo cai = {0};                                 \
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;            \
    cai.commandPool = pool;                                                 \
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;                            \
    cai.commandBufferCount = 1;                                             \
    VkCommandBuffer var;                                                    \
    vkAllocateCommandBuffers(dev, &cai, &var);                              \
    VkCommandBufferBeginInfo _bi = {0};                                     \
    _bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;                \
    vkBeginCommandBuffer(var, &_bi);

#define ENDSUB(cb)                                                          \
    vkEndCommandBuffer(cb);                                                 \
    VkSubmitInfo _si = {0};                                                 \
    _si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;                              \
    _si.commandBufferCount = 1;                                             \
    _si.pCommandBuffers = &cb;                                              \
    vkQueueSubmit(q, 1, &_si, VK_NULL_HANDLE);                              \
    vkQueueWaitIdle(q);

int main(void)
{
    /* buffer: 256 bytes, device-local with host-visible; we read back CPU side */
    VkApplicationInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS)
        return 1;
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
        return 1;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    vkCreateCommandPool(dev, &pci, NULL, &pool);

    VkBufferCreateInfo bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = SIZE;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = pick_mem(mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    /* try device-local first; if that fails use host */
    VkDeviceMemory mem;
    if (vkAllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS) {
        VkPhysicalDeviceMemoryProperties mp;
        memset(&mp, 0, sizeof(mp));
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if (mr.memoryTypeBits & (1u << i)) { mai.memoryTypeIndex = i; break; }
        if (vkAllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS) {
            printf("alloc failed\n");
            return 1;
        }
        printf("note: fell back to memtype %u\n", mai.memoryTypeIndex);
    }
    vkBindBufferMemory(dev, buf, mem, 0);
    void *ptr = NULL;
    VkResult mapr = vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr);
    printf("host-visible: %s\n", mapr == VK_SUCCESS ? "yes" : "no");

    uint8_t d0[8], d64[8], d128[8];
    memset(d0, 0xA0, 8);
    memset(d64, 0xB0, 8);
    memset(d128, 0xC0, 8);

    {
        BEGINCB(c);
        vkCmdUpdateBuffer(c, buf, 0, 8, d0);
        vkCmdUpdateBuffer(c, buf, 64, 8, d64);
        vkCmdUpdateBuffer(c, buf, 128, 8, d128);
        ENDSUB(c);
    }
    if (mapr == VK_SUCCESS) {
        uint8_t *b = ptr;
        printf("byte[0..3]=%02x%02x%02x%02x (want a0..)  byte[64..67]=%02x%02x%02x%02x (want b0..)  byte[128..131]=%02x%02x%02x%02x (want c0..)\n",
               b[0], b[1], b[2], b[3], b[64], b[65], b[66], b[67],
               b[128], b[129], b[130], b[131]);
    }
    printf("done\n");
    return 0;
}
