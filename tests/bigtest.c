/*
 * Large-copy test: does the driver fully write a 1024x1024 copy with a
 * 20 MB buffer offset (Minecraft shape)?
 *
 *   DISABLE_VK_LAYER_POWERVR_COMPAT=1 ./bigtest
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1024
#define H 1024

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

    /* staging buffer: 24 MB, tiles at 0, 8M, 16M ... encoded (x,y)~per pixel + tag */
    VkDeviceSize SS = 24ull * 1024 * 1024;
    VkBufferCreateInfo bci = {0};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = SS;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer sbuf;
    vkCreateBuffer(dev, &bci, NULL, &sbuf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, sbuf, &mr);
    VkMemoryAllocateInfo mai = {0};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = pick_mem(mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory smem;
    if (vkAllocateMemory(dev, &mai, NULL, &smem) != VK_SUCCESS) {
        printf("staging alloc failed\n");
        return 1;
    }
    vkBindBufferMemory(dev, sbuf, smem, 0);
    void *pp;
    vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, &pp);
    memset(pp, 0, SS);
    for (int t = 0; t < 3; t++) {
        uint8_t *base = pp + (size_t)t * 8 * 1024 * 1024;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint8_t *p = base + ((size_t)y * W + x) * 4;
                p[0] = (uint8_t)(x & 0xff);
                p[1] = (uint8_t)(y & 0xff);
                p[2] = (uint8_t)(x >> 8) + t * 40;
                p[3] = 255;
            }
    }
    vkUnmapMemory(dev, smem);

    /* readback buffer */
    VkBufferCreateInfo rbci = bci;
    rbci.size = W * H * 4;
    rbci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer rbuf;
    vkCreateBuffer(dev, &rbci, NULL, &rbuf);
    VkMemoryRequirements rmr;
    vkGetBufferMemoryRequirements(dev, rbuf, &rmr);
    VkMemoryAllocateInfo rbmai = mai;
    rbmai.allocationSize = rmr.size;
    VkDeviceMemory rmem;
    vkAllocateMemory(dev, &rbmai, NULL, &rmem);
    vkBindBufferMemory(dev, rbuf, rmem, 0);
    void *rptr;
    vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, &rptr);

    /* image */
    VkImage img;
    VkDeviceMemory imem;
    {
        VkImageCreateInfo ii = {0};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent.width = W;
        ii.extent.height = H;
        ii.extent.depth = 1;
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_LINEAR;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT;
        vkCreateImage(dev, &ii, NULL, &img);
    }
    {
        VkMemoryRequirements imr;
        vkGetImageMemoryRequirements(dev, img, &imr);
        VkMemoryAllocateInfo imai = mai;
        imai.allocationSize = imr.size;
        imai.memoryTypeIndex = pick_mem(imr.memoryTypeBits, 0);
        vkAllocateMemory(dev, &imai, NULL, &imem);
        vkBindImageMemory(dev, img, imem, 0);
        printf("image mem: %llu bytes, type %u\n", (unsigned long long)imr.size, imai.memoryTypeIndex);
    }

    /* copy from bufferOffset 16 MB */
    {
        BEGINCB(c);
        VkImageMemoryBarrier b = {0};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.image = img;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                             1, &b);
        VkBufferImageCopy r = {0};
        r.bufferOffset = 16ull * 1024 * 1024;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageExtent.width = W;
        r.imageExtent.height = H;
        r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(c, sbuf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &b);
        r.bufferOffset = 0;
        vkCmdCopyImageToBuffer(c, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &r);
        ENDSUB(c);
    }
    uint8_t *rb = rptr;
    int checks[5][2] = {{0, 0}, {10, 10}, {512, 512}, {1023, 0}, {0, 1023}};
    for (int i = 0; i < 5; i++) {
        int x = checks[i][0], y = checks[i][1];
        uint8_t *p = rb + ((size_t)y * W + x) * 4;
        printf("px(%4d,%4d)=%02x%02x%02x%02x want %02x%02x%02x (tile2)\n",
               x, y, p[0], p[1], p[2], p[3], x & 0xff, y & 0xff, (x >> 8) + 80);
    }
    printf("done\n");
    return 0;
}
