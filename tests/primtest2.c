/*
 * Where does the PowerVR driver actually place copy data?
 * Each test uses a FRESH 64x64 image with a clean UNDEFINED->DST barrier,
 * so no stale-layout interference.  After each copy we report the first
 * non-zero pixels found in the readback (expect: replaced pattern).
 *
 * Run raw:
 *   DISABLE_VK_LAYER_POWERVR_COMPAT=1 ./primtest2
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
static VkImage imgA;
static VkDeviceMemory memA[1];
static uint8_t *src; /* host-mapped pattern buffer */

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
        memset(ptr ? *ptr : NULL, 0, size);
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

/* fresh 64x64 dst image + UNDEFINED->DST barrier stage */
#define FRESH_DST(var, name)                                               \
    VkImage var;                                                            \
    VkDeviceMemory _m_##var[1];                                             \
    mkimg(W, H, &(var), _m_##var);                                          \
    VkDeviceMemory *name = _m_##var

static VkDeviceMemory imgA_mem_unused;

static VkDeviceMemory *memdummy;
static void report(const char *tag, const uint8_t *rb)
{
    int found = 0;
    printf("%-28s first non-zero pixels:", tag);
    for (int y = 0; y < H && found < 4; y++)
        for (int x = 0; x < W && found < 4; x++) {
            const uint8_t *p = rb + ((size_t)y * W + x) * 4;
            if (p[0] || p[1] || p[3] != 0 /* exact value below */) {
                ;
            }
            if (p[0] || p[1] || p[2] || p[3]) {
                printf(" (%d,%d)=%02x%02x%02x%02x", x, y, p[0], p[1], p[2], p[3]);
                found++;
            }
        }
    if (!found)
        printf(" NONE\n");
    else
        printf("\n");
    /* any other distinct pixels? */
    found = 0;
    for (int y = 0, seen = 0; y < H && seen < 3; y++)
        for (int x = 0; x < W && seen < 3; x++) {
            const uint8_t *p = rb + ((size_t)y * W + x) * 4;
            if ((p[0] != 0 || p[1] != 0)) {
                int idx = y * W + x;
                (void)idx;
                seen = 5;
                break;
            }
        }
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

    /* src buffer with the unique per-pixel pattern */
    VkBuffer sbuf, rbuf;
    VkDeviceMemory smem[1], rmem[1];
    void *rptr = NULL;
    mkbuf(4 + W * H * 4 * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sbuf, smem, (void **)&src);
    mkbuf(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rbuf, rmem, &rptr);
    memset(src, 0, 16);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = src + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = (uint8_t)~x; p[3] = 255;
        }

    /* fill imgA = reference copy of the whole pattern (tight origin copy) */
    {
        mkimg(W, H, &imgA, memA);
        VkCommandBufferAllocateInfo cai = {0};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer c;
        vkAllocateCommandBuffers(dev, &cai, &c);
        VkCommandBufferBeginInfo bi = {0};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(c, &bi);
        bar_img(c, imgA, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageExtent.width = W;
        r.imageExtent.height = H;
        r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(c, sbuf, imgA, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        bar_img(c, imgA, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        vkEndCommandBuffer(c);
        VkSubmitInfo si = {0};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &c;
        vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(q);
    }

#define SUBMIT(fn)                                                         \
    do {                                                                   \
        VkCommandBufferAllocateInfo cai = {0};                              \
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;         \
        cai.commandPool = pool;                                             \
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;                        \
        cai.commandBufferCount = 1;                                         \
        VkCommandBuffer c;                                                  \
        vkAllocateCommandBuffers(dev, &cai, &c);                            \
        VkCommandBufferBeginInfo bi = {0};                                  \
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;             \
        vkBeginCommandBuffer(c, &bi);                                       \
        fn;                                                                 \
        vkEndCommandBuffer(c);                                              \
        VkSubmitInfo si = {0};                                              \
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;                           \
        si.commandBufferCount = 1;                                          \
        si.pCommandBuffers = &c;                                            \
        vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);                           \
        vkQueueWaitIdle(q);                                                 \
        memset(rptr, 0, W * H * 4);                                        \
    } while (0)

#define BACK(tag)                                                          \
    do {                                                                   \
        VkCommandBufferAllocateInfo cai = {0};                              \
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;         \
        cai.commandPool = pool;                                             \
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;                        \
        cai.commandBufferCount = 1;                                         \
        VkCommandBuffer c;                                                  \
        vkAllocateCommandBuffers(dev, &cai, &c);                            \
        VkCommandBufferBeginInfo bi = {0};                                  \
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;             \
        vkBeginCommandBuffer(c, &bi);                                       \
        VkBufferImageCopy r = {0};                                          \
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;          \
        r.imageSubresource.layerCount = 1;                                  \
        r.imageExtent.width = W;                                            \
        r.imageExtent.height = H;                                           \
        r.imageExtent.depth = 1;                                            \
        vkCmdCopyImageToBuffer(c, imgB, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, \
                               rbuf, 1, &r);                                \
        vkEndCommandBuffer(c);                                              \
        VkSubmitInfo si = {0};                                              \
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;                           \
        si.commandBufferCount = 1;                                          \
        si.pCommandBuffers = &c;                                            \
        vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);                           \
        vkQueueWaitIdle(q);                                                 \
        report(tag, rptr);                                                  \
        memset(rptr, 0, W * H * 4);                                        \
    } while (0)

    /* ---- control: whole-image tight copy ---- */
    {
    FRESH_DST(imgB, memB);
    SUBMIT({
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageExtent.width = W;
        r.imageExtent.height = H;
        r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(c, sbuf, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    });
    BACK("T0 whole 64x64 transDST->src");
    }

    /* ---- CopyBuffer srcOffset ---- */
    {
        VkBuffer dbuf;
        VkDeviceMemory dmem[1];
        void *dptr = NULL;
        mkbuf(256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dbuf, dmem, &dptr);
        SUBMIT({
            VkBufferCopy bc = {0};
            bc.srcOffset = 17 * W * 4; /* pixel (17,5) */
            bc.size = 16;
            vkCmdCopyBuffer(c, sbuf, dbuf, 1, &bc);
        });
        uint8_t *d = dptr;
        printf("T1 CopyBuffer srcOffset=17*256 : d[0..7]=%02x%02x%02x%02x %02x%02x%02x%02x (want 11 05 ee ff 14 05 ec ff)\n",
               d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        /* control: dstOffset */
        memset(dptr, 0, 256);
        SUBMIT({
            VkBufferCopy bc = {0};
            bc.dstOffset = 128;
            bc.size = 16;
            vkCmdCopyBuffer(c, sbuf, dbuf, 1, &bc);
        });
        d = dptr;
        printf("T1b CopyBuffer dstOffset=128   : d[128]=%02x d[129]=%02x (want %02x %02x)\n",
               d[128], d[129], src[0], src[1]);
    }

    /* ---- CopyImage both offsets (fresh dst) ---- */
    {
    FRESH_DST(imgB, memB);
    SUBMIT({
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy r = {0};
        r.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.srcSubresource.layerCount = 1;
        r.dstSubresource = r.srcSubresource;
        r.srcOffset.x = 16; r.srcOffset.y = 16;
        r.dstOffset.x = 32; r.dstOffset.y = 32;
        r.extent.width = 16; r.extent.height = 16; r.extent.depth = 1;
        vkCmdCopyImage(c, imgA, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    });
    BACK("T2 CopyImage (16,16)->(32,32)");
    }

    /* ---- CopyImage dst offset only (fresh dst) ---- */
    {
    FRESH_DST(imgB, memB);
    SUBMIT({
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy r = {0};
        r.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.srcSubresource.layerCount = 1;
        r.dstSubresource = r.srcSubresource;
        r.dstOffset.x = 8; r.dstOffset.y = 8;
        r.extent.width = 16; r.extent.height = 16; r.extent.depth = 1;
        vkCmdCopyImage(c, imgA, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    });
    BACK("T3 CopyImage (0,0)->(8,8)");
    }

    /* ---- CopyImage src offset only (fresh dst) ---- */
    {
    FRESH_DST(imgB, memB);
    SUBMIT({
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy r = {0};
        r.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.srcSubresource.layerCount = 1;
        r.dstSubresource = r.srcSubresource;
        r.srcOffset.x = 16; r.srcOffset.y = 16;
        r.extent.width = 16; r.extent.height = 16; r.extent.depth = 1;
        vkCmdCopyImage(c, imgA, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    });
    BACK("T3b CopyImage (16,16)->(0,0)");
    }

    /* ---- CopyBufferToImage: imageOffset (32,32) only (fresh dst) ---- */
    {
    FRESH_DST(imgB, memB);
    SUBMIT({
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageOffset.x = 32; r.imageOffset.y = 32;
        r.imageExtent.width = 16; r.imageExtent.height = 16; r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(c, sbuf, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    });
    BACK("T4 BTI imageOffset(32,32)");
    }

    /* ---- CopyBufferToImage: offset (0,16) only (fresh dst) ---- */
    {
    FRESH_DST(imgB, memB);
    SUBMIT({
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageOffset.y = 16;
        r.imageExtent.width = 16; r.imageExtent.height = 16; r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(c, sbuf, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    });
    BACK("T4b BTI imageOffset(0,16)");
    }

    /* ---- CopyBufferToImage: rowLength==width, offset 0 (fresh dst) ---- */
    {
    FRESH_DST(imgB, memB);
    SUBMIT({
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.bufferRowLength = 16; r.bufferImageHeight = 16;
        r.imageExtent.width = 16; r.imageExtent.height = 16; r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(c, sbuf, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    });
    BACK("T5 BTI rowLen=16 off 0");
    }

    /* ---- CopyBufferToImage: 8x8 tight at origin (fresh dst) ---- */
    {
    FRESH_DST(imgB, memB);
    SUBMIT({
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageExtent.width = 8; r.imageExtent.height = 8; r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(c, sbuf, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    });
    BACK("T6 BTI 8x8 tight origin");
    }

    /* ---- CopyBufferToImage: whole image but SRC data at buffer offset (fresh) */
    {
    FRESH_DST(imgB, memB);
    /* view = rows 17.. of the pattern (buffer offset 17*W*4) */
    SUBMIT({
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy r = {0};
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.bufferOffset = 17 * W * 4;
        r.imageExtent.width = W; r.imageExtent.height = H; r.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(c, sbuf, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    });
    BACK("T7 BTI bufOffset=17*W*4");
    }
    printf("done\n");
    return 0;
}
