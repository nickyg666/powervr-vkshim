/*
 * LINEAR-tiling image copies on the PowerVR driver.
 *
 * Minecraft uploads its texture atlases into LINEAR-tiling images
 * (tiling=0, usage SRC|DST|SAMPLED) with many small strided copies.
 * Everything before used OPTIMAL tiling, where the driver behaved.
 *
 *   L1: 1 tight 64x64 copy into a LINEAR image, read back from LINEAR
 *   L2: 4 offset 16x16 copies into one LINEAR image
 *   L3: 1 copy with non-trivial bufferOffset + rowLength + imageOffset
 *   L4: same as L1 but the image is ALSO used as a sampled image
 *       (image view + sampler + compute or draw) — skip if no cmd
 *
 *   DISABLE_VK_LAYER_POWERVR_COMPAT=1 ./lintest
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

static VkSubresourceLayout layout_of(VkImage img)
{
    VkSubresourceLayout sl;
    memset(&sl, 0, sizeof(sl));
    VkImageSubresource sub = {0};
    sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sub.mipLevel = 0;
    sub.arrayLayer = 0;
    vkGetImageSubresourceLayout(dev, img, &sub, &sl);
    return sl;
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

static void mkimg(uint32_t w, uint32_t h, VkImageTiling tiling, VkImage *img, VkDeviceMemory *mem)
{
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = tiling;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    VkResult r = vkCreateImage(dev, &ii, NULL, img);
    if (r != VK_SUCCESS) { printf("  (create image tiling=%d failed r=%d)\n", tiling, r); *img = 0; return; }
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
                    VkImageLayout newL, VkAccessFlags srcA, VkAccessFlags dstA)
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
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &b);
}

static void bti(VkCommandBuffer c, VkBuffer b, VkImage img, int ox, int oy,
                int w, int h, int bufOffPx, int rowLen, int imgH)
{
    VkBufferImageCopy r;
    memset(&r, 0, sizeof(r));
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.bufferOffset = (VkDeviceSize)bufOffPx * 4;
    r.bufferRowLength = rowLen;
    r.bufferImageHeight = imgH;
    r.imageOffset.x = ox;
    r.imageOffset.y = oy;
    r.imageExtent.width = w;
    r.imageExtent.height = h;
    r.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(c, b, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
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

static void back(const char *tag, VkImage img)
{
    BEGINCB(c);
    bar_img(c, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy r = {0};
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = W;
    r.imageExtent.height = H;
    r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(c, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &r);
    ENDSUB(c);
    int first = 0;
    printf("%-30s first pixels:", tag);
    for (int y = 0; y < 4 && first < 4; y++)
        for (int x = 0; x < 8 && first < 4; x++) {
            const uint8_t *p = rptr + ((size_t)y * W + x) * 4;
            printf(" (%d,%d)=%02x%02x%02x", x, y, p[0], p[1], p[2]);
            first++;
        }
    printf("\n");
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

    mkbuf(4 + W * H * 4 * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sbuf, smem, (void **)&src);
    mkbuf(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rbuf, rmem, (void **)&rptr);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = src + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = (uint8_t)(x + y); p[3] = 255;
        }

    /* L0 control: OPTIMAL whole copy (known good) */
    {
        VkImage imgO;
        VkDeviceMemory mo[1];
        mkimg(W, H, VK_IMAGE_TILING_OPTIMAL, &imgO, mo);
        BEGINCB(c);
        bar_img(c, imgO, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(c, sbuf, imgO, 0, 0, W, H, 0, 0, 0);
        ENDSUB(c);
        back("L0 OPTIMAL full", imgO);
    }

    /* L1: LINEAR whole copy */
    {
        VkImage imgL;
        VkDeviceMemory ml[1];
        mkimg(W, H, VK_IMAGE_TILING_LINEAR, &imgL, ml);
        if (!imgL) return 0;
        VkSubresourceLayout sl = layout_of(imgL);
        printf("%-30s subresource: offset=%llu rowPitch=%llu\n", "L1 LINEAR layout",
               (unsigned long long)sl.offset, (unsigned long long)sl.rowPitch);
        BEGINCB(c);
        bar_img(c, imgL, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(c, sbuf, imgL, 0, 0, W, H, 0, 0, 0);
        ENDSUB(c);
        back("L1 LINEAR full", imgL);
    }

    /* L2: LINEAR, 4 offset copies */
    {
        VkImage imgL;
        VkDeviceMemory ml[1];
        mkimg(W, H, VK_IMAGE_TILING_LINEAR, &imgL, ml);
        if (!imgL) return 0;
        BEGINCB(c);
        bar_img(c, imgL, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT);
        for (int t = 0; t < 4; t++)
            bti(c, sbuf, imgL, (t % 2) * 32, (t / 2) * 32, 32, 32,
                ((t / 2) * 32) * W + (t % 2) * 32, 0, 0);
        ENDSUB(c);
        back("L2 LINEAR 4 offsets", imgL);
    }

    /* L3: LINEAR, Minecraft-style strided sub-rect copies */
    {
        VkImage imgL;
        VkDeviceMemory ml[1];
        mkimg(W, H, VK_IMAGE_TILING_LINEAR, &imgL, ml);
        if (!imgL) return 0;
        BEGINCB(c);
        bar_img(c, imgL, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT);
        /* sub-rect: 16x16 from (8,9) in the source, rowLength 16 */
        bti(c, sbuf, imgL, 8, 9, 16, 16, 9 * W + 8, 16, 16);
        bti(c, sbuf, imgL, 32, 9, 16, 16, 9 * W + 40, 16, 16);
        ENDSUB(c);
        back("L3 LINEAR strided", imgL);
        {
            const uint8_t *p = rptr + ((size_t)17 * W + 16) * 4;
            printf("  expect src(14,17)=0e11, got %02x%02x\n", p[0], p[1]);
        }
    }
    printf("done\n");
    return 0;
}
