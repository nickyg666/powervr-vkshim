/*
 * Clean vkCmdBlitImage / vkCmdBlitImage2 test.
 *
 * A screenshot of Minecraft suggests mipmap content is wrong: the game
 * samples mip>0 heavily (thousands of views with levelCount=5) and the
 * 1024x1024 layers=6 image gets 6 full copies (mips=1) — mip generation
 * must come from blits.
 *
 *   B1: whole-image blit 64x64 -> 64x64 (NEAREST)
 *   B2: blit subrect (16,16,32,32) -> (8,8,16,16)
 *   B3: TWO blits in one command buffer
 *   B4: downscale blit 64x64 -> 32x32 (LINEAR filter needs format w/ FILTER_BLIT)
 *   B5: two blits via secondary command buffer (workaround check)
 *
 *   DISABLE_VK_LAYER_POWERVR_COMPAT=1 ./blittest
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static VkCommandPool pool;
static VkBuffer sbuf, rbuf;
static VkDeviceMemory smem[1], rmem[1];
static uint8_t *src, *rptr;
static VkImage imgS, imgD;
static VkDeviceMemory mS[1], mD[1];

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

static void mkbuf(VkDeviceSize size, uint32_t usage, VkBuffer *buf,
                  VkDeviceMemory *mem, void **ptr)
{
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    vkCreateBuffer(dev, &bi, NULL, buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, *buf, &mr);
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = pick_mem(mr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memset(mem, 0, sizeof(*mem));
    vkAllocateMemory(dev, &ai, NULL, mem);
    vkBindBufferMemory(dev, *buf, mem[0], 0);
    if (ptr) {
        vkMapMemory(dev, mem[0], 0, VK_WHOLE_SIZE, 0, ptr);
        memset(*ptr, 0, size);
    }
}

static void mkimg(uint32_t w, uint32_t h, uint32_t mips, VkImage *img, VkDeviceMemory *mem)
{
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    vkCreateImage(dev, &ii, NULL, img);
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *img, &mr);
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = pick_mem(mr.memoryTypeBits, 0);
    memset(mem, 0, sizeof(*mem));
    vkAllocateMemory(dev, &ai, NULL, mem);
    vkBindImageMemory(dev, *img, mem[0], 0);
}

static void bar_img(VkCommandBuffer c, VkImage img, VkImageLayout oldL,
                    VkImageLayout newL, VkAccessFlags srcA, VkAccessFlags dstA,
                    uint32_t mips)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcAccessMask = srcA;
    b.dstAccessMask = dstA;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = mips;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &b);
}

#define BEGINCB(name)                                                       \
    VkCommandBufferAllocateInfo cai = {0};                                 \
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;            \
    cai.commandPool = pool;                                                 \
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;                            \
    cai.commandBufferCount = 1;                                             \
    VkCommandBuffer name;                                                    \
    vkAllocateCommandBuffers(dev, &cai, &name);                              \
    VkCommandBufferBeginInfo _bi = {0};                                     \
    _bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;                \
    vkBeginCommandBuffer(name, &_bi);

#define ENDSUB(cmd)                                                          \
    vkEndCommandBuffer(cmd);                                                 \
    VkSubmitInfo _si = {0};                                                 \
    _si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;                              \
    _si.commandBufferCount = 1;                                             \
    _si.pCommandBuffers = &cmd;                                              \
    vkQueueSubmit(q, 1, &_si, VK_NULL_HANDLE);                              \
    vkQueueWaitIdle(q);

static void back(const char *tag, VkImage img, int level, int w, int h)
{
    BEGINCB(c);
    bar_img(c, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, 1);
    VkBufferImageCopy r = {0};
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.mipLevel = level;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w;
    r.imageExtent.height = h;
    r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(c, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &r);
    ENDSUB(c);
    int pc = rptr[0] + rptr[1] + rptr[2] + rptr[3];
    printf("%-28s mip%d %dx%d: (0,0)=%02x%02x%02x%02x (%d,%d)=%02x%02x%02x%02x sum=%d\n",
           tag, level, w, h, rptr[0], rptr[1], rptr[2], rptr[3],
           w / 2, h / 2, rptr[((size_t)(h / 2) * w + w / 2) * 4],
           rptr[((size_t)(h / 2) * w + w / 2) * 4 + 1],
           rptr[((size_t)(h / 2) * w + w / 2) * 4 + 2],
           rptr[((size_t)(h / 2) * w + w / 2) * 4 + 3], pc);
    memset(rptr, 0, W * H * 4);
}

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

    mkbuf(W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sbuf, smem, (void **)&src);
    mkbuf(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rbuf, rmem, (void **)&rptr);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = src + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = (uint8_t)(x ^ y); p[3] = 255;
        }

    mkimg(W, H, 1, &imgS, mS);
    mkimg(W, H, 2, &imgD, mD);

    /* fill src image (tight copy, known-good) */
    BEGINCB(c);
    bar_img(c, imgS, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, 1);
    {
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageExtent.width = W;
        r.imageExtent.height = H;
        r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(c, sbuf, imgS, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    bar_img(c, imgS, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 1);
    ENDSUB(c);

    /* B1: whole blit */
    { BEGINCB(c);
    bar_img(c, imgD, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    {
        VkImageBlit b = {0};
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = 1;
        b.dstSubresource = b.srcSubresource;
        b.srcOffsets[1] = (VkOffset3D){W, H, 1};
        b.dstOffsets[1] = (VkOffset3D){W, H, 1};
        vkCmdBlitImage(c, imgS, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgD,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    ENDSUB(c); }
    back("B1 whole blit->mip0", imgD, 0, W, H);
    back("B1 whole blit->mip1", imgD, 1, W / 2, H / 2);

    /* B2: subrect blit */
    { BEGINCB(c);
    bar_img(c, imgD, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    {
        VkImageBlit b = {0};
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = 1;
        b.dstSubresource = b.srcSubresource;
        b.srcOffsets[0] = (VkOffset3D){16, 16, 0};
        b.srcOffsets[1] = (VkOffset3D){48, 48, 1};
        b.dstOffsets[0] = (VkOffset3D){8, 8, 0};
        b.dstOffsets[1] = (VkOffset3D){40, 40, 1};
        vkCmdBlitImage(c, imgS, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgD,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    ENDSUB(c); }
    back("B2 subrect blit->mip0", imgD, 0, W, H);

    /* B3: two blits one command buffer (to mip0 then mip1) */
    { BEGINCB(c);
    bar_img(c, imgD, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    {
        VkImageBlit b = {0};
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = 1;
        b.dstSubresource = b.srcSubresource;
        b.srcOffsets[1] = (VkOffset3D){W, H, 1};
        b.dstOffsets[1] = (VkOffset3D){W, H, 1};
        vkCmdBlitImage(c, imgS, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgD,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
        b.srcSubresource.mipLevel = 0;
        b.dstSubresource.mipLevel = 1;
        b.dstOffsets[1] = (VkOffset3D){W / 2, H / 2, 1};
        vkCmdBlitImage(c, imgS, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgD,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    ENDSUB(c); }
    back("B3 double blit->mip1", imgD, 1, W / 2, H / 2);

    /* B4: downscale with NEAREST 64->32 (to mip1 only, no LINEAR reliance) */
    { BEGINCB(c);
    bar_img(c, imgD, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, 2);
    {
        VkImageBlit b = {0};
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = 1;
        b.dstSubresource = b.srcSubresource;
        b.dstSubresource.mipLevel = 1;
        b.srcOffsets[1] = (VkOffset3D){W, H, 1};
        b.dstOffsets[1] = (VkOffset3D){W / 2, H / 2, 1};
        vkCmdBlitImage(c, imgS, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgD,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    ENDSUB(c); }
    back("B4 downscale->mip1", imgD, 1, W / 2, H / 2);

    printf("done\n");
    return 0;
}
