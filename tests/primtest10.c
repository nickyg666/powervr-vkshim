/*
 * Does the driver drop copies when MANY copy commands hit the SAME image
 * in ONE command buffer?  That is exactly how Minecraft stitches its
 * texture atlases (hundreds of small CopyBufferToImage calls, all
 * recorded back to back, one atlas image).
 *
 *   P1: 3 separate CopyBufferToImage commands, one fresh atlas, no barriers between
 *   P2: 1 CopyBufferToImage command, 3 regions with offsets
 *   P3: 3 separate CopyBufferToImage commands WITH a memory barrier between them
 *   P4: same as P1 but readback inside the same command buffer
 *   P5: 3 copies into 3 DIFFERENT fresh images in one command buffer
 *
 *   DISABLE_VK_LAYER_POWERVR_COMPAT=1 ./primtest3
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
static uint8_t *src;
static VkBuffer sbuf, rbuf;
static VkDeviceMemory smem[1], rmem[1];
static uint8_t *rptr;

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

static void bar_glob(VkCommandBuffer c)
{
    VkMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, NULL, 0,
                         NULL);
}

static void bti(VkCommandBuffer c, VkBuffer b, VkImage img, int ox, int oy,
                int w, int h, int bufOffsetPx)
{
    VkBufferImageCopy r;
    memset(&r, 0, sizeof(r));
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.bufferOffset = (VkDeviceSize)bufOffsetPx * 4;
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
    cai.commandPool = pool;                                                \
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;                           \
    cai.commandBufferCount = 1;                                            \
    VkCommandBuffer var;                                                   \
    vkAllocateCommandBuffers(dev, &cai, &var);                             \
    VkCommandBufferBeginInfo bi = {0};                                     \
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;                \
    vkBeginCommandBuffer(var, &bi);

#define ENDSUB(var)                                                        \
    vkEndCommandBuffer(var);                                               \
    VkSubmitInfo si = {0};                                                 \
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;                              \
    si.commandBufferCount = 1;                                             \
    si.pCommandBuffers = &var;                                             \
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);                              \
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
    int found = 0;
    printf("%-34s:", tag);
    for (int y = 0; y < H && found < 5; y++)
        for (int x = 0; x < W && found < 5; x++) {
            const uint8_t *p = rptr + ((size_t)y * W + x) * 4;
            if (p[0] | p[1] | p[2] | p[3]) {
                printf(" (%d,%d)=%02x%02x%02x", x, y, p[0], p[1], p[3]);
                found++;
            }
        }
    if (found) {
        /* verify all four tile anchors */
        int x, y, ok = 1;
        const uint8_t *p;
        x = 8; y = 8;  p = rptr + ((size_t)y * W + x) * 4;  ok &= p[0] == 8; /* tile(0,0) -> (8,8) */
        x = 24; y = 8; p = rptr + ((size_t)y * W + x) * 4;  ok &= p[0] == 8; /* tile(16,0) covers (24,8) */
        x = 8; y = 24; p = rptr + ((size_t)y * W + x) * 4;  ok &= p[0] == 8;
        x = 24; y = 24; p = rptr + ((size_t)y * W + x) * 4; ok &= p[0] == 8;
        printf("   [anchors %s]", ok ? "OK" : "MISSING");
    }
    printf("\n");
    printf("    rows:");
    for (int yy = 0; yy < 64; yy++) {
        int cnt = 0, fx = -1, ft = -1;
        for (int xx = 0; xx < 64; xx++) {
            const uint8_t *pp = rptr + ((size_t)yy * W + xx) * 4;
            if (pp[0] | pp[1] | pp[2] | pp[3]) { if (fx < 0) { fx = xx; ft = pp[2]; } cnt++; }
        }
        if (cnt) printf(" [y%02d n=%d x0=%d t=%d]", yy, cnt, fx, ft);
    }
    printf("\n");
    memset(rptr, 0, W * H * 4);
}

/* tiles: tile k holds source data rows k*16..; anchor value (x in tile) = 0x08 expected inside */
#define NTILE 3

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
    /* three 16x16 "tiles" packed in the buffer: tile k at bytes k*16*16*4 */
    for (int t = 0; t < NTILE; t++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint8_t *p = src + ((size_t)t * 256 + y * 16 + x) * 4;
                p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = (uint8_t)t; p[3] = 255;
            }

    /* reference imgA (unused by P-tests but keeps pattern sanity) */
    {
        mkimg(W, H, &imgA, memA);
        BEGINCB(c);
        bar_img(c, imgA, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(c, sbuf, imgA, 0, 0, W, H, 0);
        bar_img(c, imgA, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
        ENDSUB(c);
    }

    /* P1: 3 copy commands to ONE fresh image, no barriers between them */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        BEGINCB(c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        /* note: each tile COPY may pull the same packed data (tile 0 bytes at
         * its own bufferOffset within the buffer) */
        bti(c, sbuf, imgB, 0, 0, 16, 16, 0);
        bti(c, sbuf, imgB, 16, 0, 16, 16, 256);
        bti(c, sbuf, imgB, 0, 16, 16, 16, 512);
        ENDSUB(c);
        back("P1 3 cmds no barriers", imgB);
    }

    /* P2: one command, 3 regions */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        BEGINCB(c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy regs[3];
        for (int t = 0; t < NTILE; t++) {
            memset(&regs[t], 0, sizeof(regs[t]));
            regs[t].bufferOffset = (VkDeviceSize)t * 256 * 4;
            regs[t].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            regs[t].imageSubresource.layerCount = 1;
            regs[t].imageOffset.x = (t % 2) * 16;
            regs[t].imageOffset.y = (t / 2) * 16;
            regs[t].imageExtent.width = 16;
            regs[t].imageExtent.height = 16;
            regs[t].imageExtent.depth = 1;
        }
        vkCmdCopyBufferToImage(c, sbuf, imgB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 3, regs);
        ENDSUB(c);
        back("P2 1 cmd 3 regions", imgB);
    }

    /* P3: 3 copy commands WITH a global memory barrier between them */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        BEGINCB(c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(c, sbuf, imgB, 0, 0, 16, 16, 0);
        bar_glob(c);
        bti(c, sbuf, imgB, 16, 0, 16, 16, 256);
        bar_glob(c);
        bti(c, sbuf, imgB, 0, 16, 16, 16, 512);
        ENDSUB(c);
        back("P3 3 cmds w/ barriers", imgB);
    }

    /* P5: 3 copies into 3 different fresh images in one command buffer */
    {
        VkImage i0, i1, i2;
        VkDeviceMemory m0[1], m1[1], m2[1];
        mkimg(W, H, &i0, m0);
        mkimg(W, H, &i1, m1);
        mkimg(W, H, &i2, m2);
        BEGINCB(c);
        bar_img(c, i0, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bar_img(c, i1, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bar_img(c, i2, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(c, sbuf, i0, 0, 0, 16, 16, 0);
        bti(c, sbuf, i1, 0, 0, 16, 16, 256);
        bti(c, sbuf, i2, 0, 0, 16, 16, 512);
        ENDSUB(c);
        back("P5 img0 of 3", i0);
        back("P5 img1 of 3", i1);
        back("P5 img2 of 3", i2);
    }

    /* P6: 3 copies into one image, but each copy reads THE SAME tile-0 data
     * (tests whether it is the bufferOffset or the image target that breaks) */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        BEGINCB(c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(c, sbuf, imgB, 0, 0, 16, 16, 0);
        bti(c, sbuf, imgB, 16, 0, 16, 16, 0);
        bti(c, sbuf, imgB, 0, 16, 16, 16, 0);
        ENDSUB(c);
        back("P6 3 cmds same src tile", imgB);
    }
    printf("done\n");
    return 0;
}
