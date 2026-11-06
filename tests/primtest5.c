/*
 * Confirm the ExecuteCommands workaround at scale + measure which copy
 * families break in primaries and whether secondaries fix them.
 *
 *   W1: 100 BTI copies in ONE secondary, executed once -> full atlas correct?
 *   W2: 10 BTI copies in 10 secondaries (one copy each), executed one by one
 *   G1: 2x BlitImage in one primary          (expect: broken)
 *   G2: 2x BlitImage in one secondary        (expect: fixed)
 *   I1: 2x CopyImageToBuffer in one primary  (expect: broken)
 *   I2: 2x CopyImageToBuffer in one secondary(expect: fixed?)
 *
 *   DISABLE_VK_LAYER_POWERVR_COMPAT=1 ./primtest5
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define TILE 4
#define NTILE 100

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static VkCommandPool pool;
static uint8_t *src, *rptr;
static VkBuffer sbuf, rbuf;
static VkDeviceMemory smem[1], rmem[1];

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

static void mkimg(uint32_t w, uint32_t h, VkImage *img, VkDeviceMemory *mem)
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
                int w, int h, int bufOffPx)
{
    VkBufferImageCopy r;
    memset(&r, 0, sizeof(r));
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.bufferOffset = (VkDeviceSize)bufOffPx * 4;
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

#define BEGINSEC(var)                                                      \
    VkCommandBufferAllocateInfo scai = {0};                                \
    scai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;           \
    scai.commandPool = pool;                                                \
    scai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;                         \
    scai.commandBufferCount = 1;                                            \
    VkCommandBuffer var;                                                    \
    vkAllocateCommandBuffers(dev, &scai, &var);                             \
    VkCommandBufferBeginInfo _sbi = {0};                                    \
    _sbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;               \
    vkBeginCommandBuffer(var, &_sbi);

#define ENDSUB(cb)                                                          \
    vkEndCommandBuffer(cb);                                                 \
    VkSubmitInfo _si = {0};                                                 \
    _si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;                              \
    _si.commandBufferCount = 1;                                             \
    _si.pCommandBuffers = &cb;                                              \
    vkQueueSubmit(q, 1, &_si, VK_NULL_HANDLE);                              \
    vkQueueWaitIdle(q);

static void back(const char *tag, VkImage img, int grid, int *bad, int *whole)
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
    /* every tile anchor: the (1,1) pixel of tile k must read k */
    int errs = 0, firstbad = -1;
    for (int k = 0; k < (grid ? grid : 1); k++) {
        int tx, ty;
        if (grid) {
            tx = (k % 10) * 6;
            ty = (k / 10) * 6;
        } else {
            tx = 0; ty = 0;
        }
        const uint8_t *p = rptr + ((size_t)(ty + 1) * W + tx + 1) * 4;
        if (!(p[0] == 1 && p[1] == 1 && p[2] == k && p[3] == 255)) {
            errs++;
            if (firstbad < 0) firstbad = k;
        }
    }
    *bad = errs;
    *whole = firstbad;
    printf("%-28s wrong tiles: %d/%d", tag, errs, grid ? grid : 1);
    if (firstbad >= 0)
        printf(" (first: tile %d -> dst(%d,%d), got %02x%02x%02x%02x)",
               firstbad, (firstbad % 10) * 6, (firstbad / 10) * 6,
               rptr[((size_t)((firstbad / 10) * 6 + 1) * W + (firstbad % 10) * 6 + 1) * 4],
               rptr[((size_t)((firstbad / 10) * 6 + 1) * W + (firstbad % 10) * 6 + 1) * 4 + 1],
               rptr[((size_t)((firstbad / 10) * 6 + 1) * W + (firstbad % 10) * 6 + 1) * 4 + 2],
               rptr[((size_t)((firstbad / 10) * 6 + 1) * W + (firstbad % 10) * 6 + 1) * 4 + 3]);
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
    /* 100 distinct 4x4 tiles packed at 6*4=... grid 10x10 uses 6-px pitch,
     * packed buffer tile k at k*4*4 texels */
    for (int k = 0; k < NTILE; k++)
        for (int y = 0; y < TILE; y++)
            for (int x = 0; x < TILE; x++) {
                uint8_t *p = src + ((size_t)k * TILE * TILE + y * TILE + x) * 4;
                p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = (uint8_t)k; p[3] = 255;
            }

    /* ---- W1: 100 copies in ONE secondary, one EC ---- */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        BEGINSEC(sec);
        for (int k = 0; k < NTILE; k++)
            bti(sec, sbuf, imgB, (k % 10) * 6, (k / 10) * 6, TILE, TILE, k * TILE * TILE);
        BEGINCB(c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdExecuteCommands(c, 1, &sec);
        ENDSUB(c);
        int bad, first;
        back("W1 100 copies 1 sec", imgB, NTILE, &bad, &first);
    }

    /* ---- W2: 10 copies in 10 secondaries, one EC each ---- */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        VkCommandBuffer secs[10];
        for (int k = 0; k < 10; k++) {
            BEGINSEC(sec);
            bti(sec, sbuf, imgB, (k % 10) * 6, (k / 10) * 6, TILE, TILE, k * TILE * TILE);
            secs[k] = sec;
        }
        BEGINCB(c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        for (int k = 0; k < 10; k++)
            vkCmdExecuteCommands(c, 1, &secs[k]);
        ENDSUB(c);
        int bad, first;
        back("W2 10 copies 10 secs", imgB, 10, &bad, &first);
    }

    /* ---- G1: 2 blits in one primary ---- */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        BEGINCB(c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit b1 = {0}, b2 = {0};
        for (int i = 0; i < 2; i++) {
            VkImageBlit *b = i ? &b2 : &b1;
            b->srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b->srcSubresource.layerCount = 1;
            b->dstSubresource = b->srcSubresource;
            b->dstOffsets[1] = (VkOffset3D){(i % 10) * 6 + TILE, (i / 10) * 6 + TILE, 1};
            b->dstOffsets[0] = (VkOffset3D){(i % 10) * 6, (i / 10) * 6, 0};
        }
        /* src: use src pattern via fresh src image S built from tiles 0/1 */
        VkImage si;
        VkDeviceMemory sm2[1];
        mkimg(TILE * 2, TILE, &si, sm2);
        bar_img(c, si, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(c, sbuf, si, 0, 0, TILE, TILE, 0);
        bti(c, sbuf, si, TILE, 0, TILE, TILE, TILE * TILE); /* SECOND copy (may break) */
        bar_img(c, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdBlitImage(c, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b1, VK_FILTER_NEAREST);
        vkCmdBlitImage(c, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b2, VK_FILTER_NEAREST);
        ENDSUB(c);
        int bad, first;
        back("G1 2x blit primary", imgB, 2, &bad, &first);
    }

    /* ---- G2: same blits inside ONE secondary ---- */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        VkImage si;
        VkDeviceMemory sm2[1];
        mkimg(TILE * 2, TILE, &si, sm2);
        BEGINSEC(sec);
        bar_img(sec, si, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(sec, sbuf, si, 0, 0, TILE, TILE, 0);
        bti(sec, sbuf, si, TILE, 0, TILE, TILE, TILE * TILE);
        bar_img(sec, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkImageBlit b1 = {0}, b2 = {0};
        b1.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b1.srcSubresource.layerCount = 1;
        b1.dstSubresource = b1.srcSubresource;
        b1.dstOffsets[1] = (VkOffset3D){TILE, TILE, 1};
        b2 = b1;
        b2.dstOffsets[0] = (VkOffset3D){6, 0, 0};
        b2.dstOffsets[1] = (VkOffset3D){6 + TILE, TILE, 1};
        vkCmdBlitImage(sec, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b1, VK_FILTER_NEAREST);
        vkCmdBlitImage(sec, si, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b2, VK_FILTER_NEAREST);
        BEGINCB(c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bar_img(c, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdExecuteCommands(c, 1, &sec);
        ENDSUB(c);
        int bad, first;
        back("G2 2x blit secondary", imgB, 2, &bad, &first);
    }

    /* ---- I1: 2x CopyImageToBuffer in one primary ---- */
    {
        VkImage se;
        VkDeviceMemory sm2[1];
        mkimg(TILE * 2, TILE, &se, sm2);
        BEGINCB(c);
        bar_img(c, se, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(c, sbuf, se, 0, 0, TILE, TILE, 0);
        bti(c, sbuf, se, TILE, 0, TILE, TILE, TILE * TILE);
        bar_img(c, se, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy r1 = {0}, r2 = {0};
        r1.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r1.imageSubresource.layerCount = 1;
        r1.imageExtent.width = TILE; r1.imageExtent.height = TILE; r1.imageExtent.depth = 1;
        r2 = r1;
        r2.imageOffset.x = TILE;
        vkCmdCopyImageToBuffer(c, se, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &r1);
        vkCmdCopyImageToBuffer(c, se, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &r2);
        ENDSUB(c);
        uint8_t *d = rptr;
        printf("I1 2x img->buf: buf[0..3]=%02x%02x%02x%02x (want tile0 (0,0)) buf[16..19]=%02x%02x%02x%02x (want tile1 (0,0))\n",
               d[0], d[1], d[2], d[3], d[16], d[17], d[18], d[19]);
        memset(rptr, 0, W * H * 4);
    }

    /* ---- I2: 2x CopyImageToBuffer in one secondary ---- */
    {
        VkImage se;
        VkDeviceMemory sm2[1];
        mkimg(TILE * 2, TILE, &se, sm2);
        BEGINSEC(sec);
        bar_img(sec, se, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(sec, sbuf, se, 0, 0, TILE, TILE, 0);
        bti(sec, sbuf, se, TILE, 0, TILE, TILE, TILE * TILE);
        bar_img(sec, se, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy r1 = {0}, r2 = {0};
        r1.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r1.imageSubresource.layerCount = 1;
        r1.imageExtent.width = TILE; r1.imageExtent.height = TILE; r1.imageExtent.depth = 1;
        r2 = r1;
        r2.imageOffset.x = TILE;
        vkCmdCopyImageToBuffer(sec, se, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &r1);
        vkCmdCopyImageToBuffer(sec, se, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &r2);
        BEGINCB(c);
        vkCmdExecuteCommands(c, 1, &sec);
        ENDSUB(c);
        uint8_t *d = rptr;
        printf("I2 2x img->buf: buf[0..3]=%02x%02x%02x%02x (want tile0 (0,0)) buf[16..19]=%02x%02x%02x%02x (want tile1 (0,0))\n",
               d[0], d[1], d[2], d[3], d[16], d[17], d[18], d[19]);
        memset(rptr, 0, W * H * 4);
    }
    printf("done\n");
    return 0;
}
