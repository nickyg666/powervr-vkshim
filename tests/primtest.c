/*
 * Which copy primitives does the PowerVR driver implement correctly?
 *
 *   T1: vkCmdCopyBuffer with srcOffset != 0
 *   T2: vkCmdCopyImage with both src and dst image offsets
 *   T3: vkCmdCopyImage with dst image offset only
 *   T4: vkCmdBlitImage with image offsets
 *   T5: vkCmdCopyBufferToImage2 (KHR path) with imageOffset
 *   T6: vkCmdCopyBufferToImage with explicit bufferRowLength (offset 0)
 *
 * Run with the compat layer disabled to see the raw driver behaviour:
 *   DISABLE_VK_LAYER_POWERVR_COMPAT=1 ./primtest
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static void out(VkCommandBuffer cmd, VkImage img, VkBuffer buf,
                const void **rptr, VkDevice dev, VkQueue q, VkCommandPool pool)
{
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer c;
    vkAllocateCommandBuffers(dev, &ai, &c);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    vkBeginCommandBuffer(c, &bi);
    VkBufferImageCopy r;
    memset(&r, 0, sizeof(r));
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = W;
    r.imageExtent.height = H;
    r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(c, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
    vkEndCommandBuffer(c);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q);
    *rptr = NULL; /* the mapped readback buffer is returned by the caller */
    (void)cmd;
}

static uint32_t pick_mem(VkPhysicalDevice pd, uint32_t bits, uint32_t flags)
{
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof(mp));
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return 0;
}

static void mkbuf(VkDevice dev, VkPhysicalDevice pd, VkDeviceSize size,
                  uint32_t usage, VkBuffer *buf, VkDeviceMemory **mem, void **ptr)
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
    ai.memoryTypeIndex = pick_mem(pd, mr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    *mem = malloc(sizeof(**mem));
    memset(*mem, 0, sizeof(**mem));
    vkAllocateMemory(dev, &ai, NULL, *mem);
    vkBindBufferMemory(dev, *buf, **mem, 0);
    vkMapMemory(dev, **mem, 0, VK_WHOLE_SIZE, 0, ptr);
    memset(*ptr, 0, size);
}

static void mkimg(VkDevice dev, VkPhysicalDevice pd, uint32_t w, uint32_t h,
                  VkImage *img, VkDeviceMemory **mem)
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
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vkCreateImage(dev, &ii, NULL, img);
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *img, &mr);
    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = pick_mem(pd, mr.memoryTypeBits, 0);
    *mem = malloc(sizeof(**mem));
    memset(*mem, 0, sizeof(**mem));
    vkAllocateMemory(dev, &ai, NULL, *mem);
    vkBindImageMemory(dev, *img, **mem, 0);
}

/* run one command buffer: recorder fills it, submit, wait */
#define RUN(...)                                                            \
    do {                                                                    \
        VkCommandBufferAllocateInfo _ai = {0};                              \
        _ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;         \
        _ai.commandPool = pool;                                             \
        _ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;                        \
        _ai.commandBufferCount = 1;                                         \
        VkCommandBuffer _c;                                                 \
        vkAllocateCommandBuffers(dev, &_ai, &_c);                           \
        VkCommandBufferBeginInfo _bi = {0};                                 \
        _bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;            \
        vkBeginCommandBuffer(_c, &_bi);                                     \
        __VA_ARGS__;                                                        \
        vkEndCommandBuffer(_c);                                             \
        VkSubmitInfo _si = {0};                                             \
        _si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;                          \
        _si.commandBufferCount = 1;                                         \
        _si.pCommandBuffers = &_c;                                          \
        vkQueueSubmit(q, 1, &_si, VK_NULL_HANDLE);                          \
        vkQueueWaitIdle(q);                                                 \
    } while (0)

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

static int check_px(const uint8_t *rb, int x, int y, int ex, int ey)
{
    const uint8_t *p = rb + ((size_t)y * W + x) * 4;
    return p[0] == ex && p[1] == ey && p[3] == 255;
}

int main(void)
{
    VkApplicationInfo ai = {0};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS)
        return printf("no instance\n"), 1;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = pds[0];
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS)
        return printf("no device\n"), 1;
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);

    /* the reference pattern: pixel (x,y) -> (x,y,~x,255) */
    uint8_t *src;
    VkBuffer sbuf, rbuf;
    VkDeviceMemory *smem, *rmem;
    void *rptr = NULL;
    mkbuf(dev, pd, W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sbuf, &smem,
          (void **)&src);
    mkbuf(dev, pd, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rbuf, &rmem,
          &rptr);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = src + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = (uint8_t)~x; p[3] = 255;
        }

    VkImage imgA, imgB;
    VkDeviceMemory *memA, *memB;
    mkimg(dev, pd, W, H, &imgA, &memA);
    mkimg(dev, pd, W, H, &imgB, &memB);

    /* fill imgA with the pattern via a tight origin copy (known good) */
    RUN({
        bar_img(_c, imgA, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageExtent.width = W;
        r.imageExtent.height = H;
        r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(_c, sbuf, imgA,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        bar_img(_c, imgA, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    });

    /* ---- T1: vkCmdCopyBuffer srcOffset ---- */
    RUN({
        VkBufferCopy c = {0};
        c.srcOffset = 17 * W * 4; /* pixel (17,5) row */
        c.dstOffset = 0;
        c.size = 16 * 4;
        vkCmdCopyBuffer(_c, sbuf, rbuf, 1, &c);
    });
    {
        uint8_t *rb = rptr;
        int ok = rb[0] == 17 && rb[1] == 5 && rb[2] == (uint8_t)~17;
        printf("T1 CopyBuffer srcOffset      %s (rb[0..2]=%02x%02x%02x want 11/05/%02x)\n",
               ok ? "PASSED" : "FAILED", rb[0], rb[1], rb[2], (uint8_t)~17);
        memset(rptr, 0, W * H * 4);
    }

    /* ---- helper macros for test T2-T4 ---- */
#define LOOKUP(tag, ox, oy, wx, wy)                                        \
    do {                                                                   \
        uint8_t *_rb = rptr;                                               \
        memset(_rb, 0, W * H * 4);                                         \
    } while (0)

    /* ---- T2: CopyImage both offsets ---- */
    memset(rptr, 0, W * H * 4);
    RUN({
        bar_img(_c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy r = {0};
        r.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.srcSubresource.layerCount = 1;
        r.dstSubresource = r.srcSubresource;
        r.srcOffset.x = 16; r.srcOffset.y = 16;
        r.dstOffset.x = 32; r.dstOffset.y = 32;
        r.extent.width = 16; r.extent.height = 16; r.extent.depth = 1;
        vkCmdCopyImage(_c, imgA, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    });
    {
        uint8_t *rb = rptr;
        int ok = check_px(rb, 32 + 5, 32 + 5, 21, 21) &&
                 !check_px(rb, 5, 5, 5, 5);
        printf("T2 CopyImage both offsets    %s (dst(37,37): %02x%02x%02x want %02x%02x%02x)\n",
               ok ? "PASSED" : "FAILED", rb[((size_t)37 * W + 37) * 4 + 0],
               rb[((size_t)37 * W + 37) * 4 + 1], rb[((size_t)37 * W + 37) * 4 + 3],
               21, 21, 255);
    }

    /* ---- T3: CopyImage dst offset only (src copies whole imgA then offset) ---- */
    memset(rptr, 0, W * H * 4);
    RUN({
        bar_img(_c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy r = {0};
        r.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.srcSubresource.layerCount = 1;
        r.dstSubresource = r.srcSubresource;
        r.dstOffset.x = 8; r.dstOffset.y = 8;
        r.extent.width = 2;
        r.extent.height = 2;
        r.extent.depth = 1;
        vkCmdCopyImage(_c, imgA, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    });
    {
        uint8_t *rb = rptr;
        int ok = check_px(rb, 9, 9, 1, 1);
        printf("T3 CopyImage dstOffset only  %s (dst(9,9): %02x%02x%02x want 01/01/ff)\n",
               ok ? "PASSED" : "FAILED", rb[((size_t)9 * W + 9) * 4],
               rb[((size_t)9 * W + 9) * 4 + 1], rb[((size_t)9 * W + 9) * 4 + 3]);
    }

    /* ---- T4: BlitImage offsets ---- */
    memset(rptr, 0, W * H * 4);
    RUN({
        bar_img(_c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit r = {0};
        r.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.srcSubresource.layerCount = 1;
        r.dstSubresource = r.srcSubresource;
        r.srcOffsets[0] = (VkOffset3D){16, 16, 0};
        r.srcOffsets[1] = (VkOffset3D){32, 32, 1};
        r.dstOffsets[0] = (VkOffset3D){32, 32, 0};
        r.dstOffsets[1] = (VkOffset3D){48, 48, 1};
        vkCmdBlitImage(_c, imgA, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                       VK_FILTER_NEAREST);
    });
    {
        uint8_t *rb = rptr;
        int ok = check_px(rb, 40, 40, 24, 24);
        printf("T4 BlitImage offsets         %s (dst(40,40): %02x%02x%02x want 18/18/ff)\n",
               ok ? "PASSED" : "FAILED", rb[((size_t)40 * W + 40) * 4],
               rb[((size_t)40 * W + 40) * 4 + 1], rb[((size_t)40 * W + 40) * 4 + 3]);
    }

    /* ---- T5: CopyBufferToImage2 with imageOffset ---- */
    memset(rptr, 0, W * H * 4);
    RUN({
        bar_img(_c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageOffset.x = 8; r.imageOffset.y = 8;
        r.imageExtent.width = 8;
        r.imageExtent.height = 8;
        r.imageExtent.depth = 1;
        VkCopyBufferToImageInfo2 info = {0};
        info.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
        info.srcBuffer = sbuf;
        info.dstImage = imgB;
        info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        info.regionCount = 1;
        info.pRegions = (const VkBufferImageCopy2 *)&r;
        vkCmdCopyBufferToImage2(_c, &info);
    });
    {
        uint8_t *rb = rptr;
        int ok = check_px(rb, 12, 12, 4, 4);
        printf("T5 CopyBufferToImage2 offset %s (dst(12,12): %02x%02x%02x want 04/04/ff)\n",
               ok ? "PASSED" : "FAILED", rb[((size_t)12 * W + 12) * 4],
               rb[((size_t)12 * W + 12) * 4 + 1], rb[((size_t)12 * W + 12) * 4 + 3]);
    }

    /* ---- T6: CopyBufferToImage with explicit bufferRowLength, offset 0 ---- */
    memset(rptr, 0, W * H * 4);
    RUN({
        bar_img(_c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.bufferRowLength = 8;
        r.bufferImageHeight = 8;
        r.imageExtent.width = 8;
        r.imageExtent.height = 8;
        r.imageExtent.depth = 1;
        /* source data: rows 16..23 of the pattern, 8 px wide */
        void *sp;
        vkMapMemory(dev, *smem, 0, VK_WHOLE_SIZE, 0, &sp); /* already mapped */
        (void)sp;
        /* no offset involved: rowLength==width => identical to tight */
        vkCmdCopyBufferToImage(_c, sbuf, imgB,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    });
    /* expect pixels (0..7, 0..7) == pattern (0..7, 0..7) */
    {
        uint8_t *rb = rptr;
        int ok = check_px(rb, 3, 3, 3, 3);
        printf("T6 rowLength==width, off 0   %s (dst(3,3): %02x%02x%02x want 03/03/ff)\n",
               ok ? "PASSED" : "FAILED", rb[((size_t)3 * W + 3) * 4],
               rb[((size_t)3 * W + 3) * 4 + 1], rb[((size_t)3 * W + 3) * 4 + 3]);
    }

    /* ---- T7: strided bufferRowLength=16 with width=8 ---- */
    memset(rptr, 0, W * H * 4);
    RUN({
        bar_img(_c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.bufferRowLength = 16;
        r.bufferImageHeight = 0;
        r.imageExtent.width = 8;
        r.imageExtent.height = 8;
        r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(_c, sbuf, imgB,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    });
    {
        uint8_t *rb = rptr;
        int ok = check_px(rb, 3, 3, 3, 3);
        printf("T7 rowLength=16 width=8      %s (dst(3,3): %02x%02x%02x want 03/03/ff)\n",
               ok ? "PASSED" : "FAILED", rb[((size_t)3 * W + 3) * 4],
               rb[((size_t)3 * W + 3) * 4 + 1], rb[((size_t)3 * W + 3) * 4 + 3]);
    }
    return 0;
}
