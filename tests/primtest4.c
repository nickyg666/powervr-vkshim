/*
 * Which repeated-copy primitive works?  The driver executes only ONE
 * CopyBufferToImage per command buffer (later copies write only their
 * first row or nothing).  Find a workaround primitive:
 *
 *   A) 2x CopyBuffer (buffer dst) in one cmdbuf           -> repeat ok?
 *   B) 2x CopyImage (image dst) in one cmdbuf             -> repeat ok?
 *   C) BTI then CopyImage in one cmdbuf
 *   D) 3x BTI in secondaries executed by one primary
 *   E) BTI inside secondary where each secondary holds ONE region
 *   F) 2x BTI with a render pass begin/end between them
 *
 *   DISABLE_VK_LAYER_POWERVR_COMPAT=1 ./primtest4
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

static void bar_glob(VkCommandBuffer c)
{
    VkMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, NULL, 0,
                         NULL);
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

#define BEGINLV(cblvl, name)                                               \
    VkCommandBufferAllocateInfo cai = {0};                                  \
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;            \
    cai.commandPool = pool;                                                 \
    cai.level = cblvl;                                                      \
    cai.commandBufferCount = 1;                                             \
    VkCommandBuffer name;                                                   \
    vkAllocateCommandBuffers(dev, &cai, &name);                             \
    VkCommandBufferBeginInfo _bi = {0};                                     \
    _bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;                \
    vkBeginCommandBuffer(name, &_bi);

#define ENDSUB(cb)                                                          \
    vkEndCommandBuffer(cb);                                                 \
    VkSubmitInfo si = {0};                                                  \
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;                               \
    si.commandBufferCount = 1;                                              \
    si.pCommandBuffers = &cb;                                               \
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);                               \
    vkQueueWaitIdle(q);

/* readback with per-row count dump */
static void back(const char *tag, VkImage img, int dumprows)
{
    BEGINLV(VK_COMMAND_BUFFER_LEVEL_PRIMARY, c);
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
    printf("%-30s", tag);
    for (int y = 0; y < H; y++) {
        int cnt = 0, firstx = -1, t1 = -1;
        for (int x = 0; x < W; x++) {
            const uint8_t *p = rptr + ((size_t)y * W + x) * 4;
            if (p[0] | p[1] | p[2] | p[3]) {
                if (firstx < 0) { firstx = x; t1 = p[2]; }
                cnt++;
            }
        }
        if (cnt && dumprows)
            printf("  [y%02d n=%02d x0=%d t=%d]", y, cnt, firstx, t1);
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
    for (int t = 0; t < 4; t++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                uint8_t *p = src + ((size_t)t * 256 + y * 16 + x) * 4;
                p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = (uint8_t)t; p[3] = 255;
            }

    /* reference source image imgA (full pattern) */
    VkImage imgA;
    VkDeviceMemory mA[1];
    mkimg(W, H, &imgA, mA);
    BEGINLV(VK_COMMAND_BUFFER_LEVEL_PRIMARY, c);
    bar_img(c, imgA, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    bti(c, sbuf, imgA, 0, 0, W, H, 0);
    bar_img(c, imgA, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    ENDSUB(c);

    /* ---- A) two CopyBuffer in one cmdbuf ---- */
    {
        VkBuffer db;
        VkDeviceMemory dm[1];
        void *dptr = NULL;
        mkbuf(512, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &db, dm, &dptr);
        BEGINLV(VK_COMMAND_BUFFER_LEVEL_PRIMARY, c);
        VkBufferCopy bc1 = {0}, bc2 = {0};
        bc1.srcOffset = 0;      bc1.dstOffset = 0;   bc1.size = 64;
        bc2.srcOffset = 256 * 4; bc2.dstOffset = 128; bc2.size = 64;
        vkCmdCopyBuffer(c, sbuf, db, 1, &bc1);
        vkCmdCopyBuffer(c, sbuf, db, 1, &bc2);
        ENDSUB(c);
        uint8_t *d = dptr;
        printf("A 2x CopyBuffer: d[0]=%02x(p0,t0) d[128]=%02x(p? t1=%d) d[129]=%02x %s\n",
               d[0], d[128], d[130], d[129],
               (d[0] == 0 && d[128] == 0 && d[130] == 1) ? "BOTH LANDED" : "BROKEN");
    }

    /* ---- B) two CopyImage into one fresh image ---- */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        BEGINLV(VK_COMMAND_BUFFER_LEVEL_PRIMARY, c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy r1 = {0}, r2 = {0};
        r1.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r1.srcSubresource.layerCount = 1;
        r1.dstSubresource = r1.srcSubresource;
        r1.dstOffset.x = 0;  r1.dstOffset.y = 0;
        r1.extent.width = 16; r1.extent.height = 16; r1.extent.depth = 1;
        r2 = r1;
        r2.srcOffset.x = 0; r2.srcOffset.y = 16; /* borrow row16 strip (t?) */
        r2.dstOffset.x = 16; r2.dstOffset.y = 16;
        vkCmdCopyImage(c, imgA, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r1);
        vkCmdCopyImage(c, imgA, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r2);
        ENDSUB(c);
        back("B 2x CopyImage", imgB, 1);
    }

    /* ---- C) BTI then CopyImage in one cmdbuf ---- */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        BEGINLV(VK_COMMAND_BUFFER_LEVEL_PRIMARY, c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(c, sbuf, imgB, 0, 0, 16, 16, 0);
        VkImageCopy r2 = {0};
        r2.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r2.srcSubresource.layerCount = 1;
        r2.dstSubresource = r2.srcSubresource;
        r2.srcOffset.x = 16;
        r2.dstOffset.x = 16; r2.dstOffset.y = 16;
        r2.extent.width = 16; r2.extent.height = 16; r2.extent.depth = 1;
        vkCmdCopyImage(c, imgA, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgB,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r2);
        ENDSUB(c);
        back("C BTI then CopyImage", imgB, 1);
    }

    /* ---- D) 3 BTIs in 3 SECONDARIES, one primary executes all ---- */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        VkCommandBuffer sec[3];
        for (int k = 0; k < 3; k++) {
            VkCommandBufferAllocateInfo s = {0};
            s.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            s.commandPool = pool;
            s.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
            s.commandBufferCount = 1;
            vkAllocateCommandBuffers(dev, &s, &sec[k]);
            VkCommandBufferBeginInfo bi = {0};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(sec[k], &bi);
            bti(sec[k], sbuf, imgB, (k % 2) * 16, (k / 2) * 16, 16, 16, k * 256);
            vkEndCommandBuffer(sec[k]);
        }
        BEGINLV(VK_COMMAND_BUFFER_LEVEL_PRIMARY, c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdExecuteCommands(c, 3, sec);
        ENDSUB(c);
        back("D 3 BTI in secondaries", imgB, 1);
    }

    /* ---- E) secondary holding 2 BTI regions executed in primary ---- */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        VkCommandBufferAllocateInfo s = {0};
        s.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        s.commandPool = pool;
        s.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        s.commandBufferCount = 1;
        VkCommandBuffer sb;
        vkAllocateCommandBuffers(dev, &s, &sb);
        VkCommandBufferBeginInfo bi = {0};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(sb, &bi);
        bti(sb, sbuf, imgB, 0, 0, 16, 16, 0);
        bti(sb, sbuf, imgB, 16, 0, 16, 16, 256);
        bti(sb, sbuf, imgB, 0, 16, 16, 16, 512);
        vkEndCommandBuffer(sb);
        BEGINLV(VK_COMMAND_BUFFER_LEVEL_PRIMARY, c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdExecuteCommands(c, 1, &sb);
        ENDSUB(c);
        back("E 3 BTI in one secondary", imgB, 1);
    }

    /* ---- F) 2x BTI with RenderPass between them ---- */
    {
        VkImage imgB;
        VkDeviceMemory mB[1];
        mkimg(W, H, &imgB, mB);
        VkRenderPass rpass;
        VkAttachmentDescription ad = {0};
        ad.format = VK_FORMAT_R8G8B8A8_UNORM;
        ad.samples = VK_SAMPLE_COUNT_1_BIT;
        ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ar = {0};
        ar.attachment = 0;
        ar.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription sp = {0};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ar;
        VkRenderPassCreateInfo rpi = {0};
        rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 1;
        rpi.pAttachments = &ad;
        rpi.subpassCount = 1;
        rpi.pSubpasses = &sp;
        vkCreateRenderPass(dev, &rpi, NULL, &rpass);
        VkFramebufferCreateInfo fbi = {0};
        fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbi.renderPass = rpass;
        fbi.attachmentCount = 1;
        fbi.pAttachments = (const VkImageView *)&imgB;
        fbi.width = W;
        fbi.height = H;
        fbi.layers = 1;
        VkFramebuffer fb;
        vkCreateFramebuffer(dev, &fbi, NULL, &fb);
        VkClearValue cv = {0};
        BEGINLV(VK_COMMAND_BUFFER_LEVEL_PRIMARY, c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        bti(c, sbuf, imgB, 0, 0, 16, 16, 0);  /* layout is COLOR_ATACH here! */
        VkRenderPassBeginInfo rbi = {0};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = rpass;
        rbi.framebuffer = fb;
        rbi.renderArea.extent.width = W;
        rbi.renderArea.extent.height = H;
        rbi.clearValueCount = 1;
        rbi.pClearValues = &cv;
        vkCmdBeginRenderPass(c, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(c);
        bar_img(c, imgB, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        bti(c, sbuf, imgB, 16, 0, 16, 16, 256);
        ENDSUB(c);
        back("F BTI, pass, BTI", imgB, 1);
    }
    printf("done\n");
    return 0;
}
