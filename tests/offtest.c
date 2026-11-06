/*
 * Which parameter of vkCmdCopyBufferToImage does the PowerVR driver get
 * wrong?  Minecraft stitches its texture atlases together from many small
 * copies that use all of them at once.
 *
 *   variant 0: imageOffset only
 *   variant 1: bufferRowLength / bufferImageHeight only
 *   variant 2: both
 *
 * usage: offtest [variant]
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 64
#define H 64

int main(int argc, char **argv)
{
    int variant = argc > 1 ? atoi(argv[1]) : 0;

    VkApplicationInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    if (vkCreateInstance(&ici, 0, &inst) != VK_SUCCESS) { printf("no instance\n"); return 1; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, 0);
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof(mp));
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t ntypes = mp.memoryTypeCount;

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, 0);
    VkQueueFamilyProperties *q = calloc(qn, sizeof(*q));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, q);
    uint32_t qfi = 0;
    for (uint32_t i = 0; i < qn; i++)
        if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, 0, &dev) != VK_SUCCESS) { printf("no device\n"); return 1; }
    VkQueue queue;
    vkGetDeviceQueue(dev, qfi, 0, &queue);

    /* four 16x16 tiles, each with its own value */
    uint8_t src[W * H];
    memset(src, 0, sizeof(src));
    for (int t = 0; t < 4; t++) {
        uint8_t v = (uint8_t)(0x40 + t * 0x40);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                src[(t / 2) * 16 * W + (t % 2) * 16 + y * W + x] = v;
    }

    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(src);
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, 0, &buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    for (uint32_t i = 0; i < ntypes; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mai.memoryTypeIndex = i; break; }
    VkDeviceMemory mem;
    vkAllocateMemory(dev, &mai, 0, &mem);
    vkBindBufferMemory(dev, buf, mem, 0);
    void *ptr = 0;
    vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr);
    memcpy(ptr, src, sizeof(src));
    vkUnmapMemory(dev, mem);

    VkImageCreateInfo imgci;
    memset(&imgci, 0, sizeof(imgci));
    imgci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgci.imageType = VK_IMAGE_TYPE_2D;
    imgci.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgci.extent.width = W; imgci.extent.height = H; imgci.extent.depth = 1;
    imgci.mipLevels = 1; imgci.arrayLayers = 1;
    imgci.samples = VK_SAMPLE_COUNT_1_BIT;
    imgci.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage img;
    if (vkCreateImage(dev, &imgci, 0, &img) != VK_SUCCESS) { printf("no image\n"); return 1; }
    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(dev, img, &imr);
    VkMemoryAllocateInfo imai = mai;
    imai.allocationSize = imr.size;
    for (uint32_t i = 0; i < ntypes; i++)
        if (imr.memoryTypeBits & (1u << i)) { imai.memoryTypeIndex = i; break; }
    VkDeviceMemory imem;
    vkAllocateMemory(dev, &imai, 0, &imem);
    vkBindImageMemory(dev, img, imem, 0);

    VkBufferCreateInfo rbci = bci;
    rbci.size = W * H * 4;
    rbci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer rbuf;
    vkCreateBuffer(dev, &rbci, 0, &rbuf);
    VkMemoryRequirements rmr;
    vkGetBufferMemoryRequirements(dev, rbuf, &rmr);
    VkMemoryAllocateInfo rbmai = mai;
    rbmai.allocationSize = rmr.size;
    for (uint32_t i = 0; i < ntypes; i++)
        if ((rmr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { rbmai.memoryTypeIndex = i; break; }
    VkDeviceMemory rbmem;
    vkAllocateMemory(dev, &rbmai, 0, &rbmem);
    vkBindBufferMemory(dev, rbuf, rbmem, 0);
    void *rptr = 0;
    vkMapMemory(dev, rbmem, 0, VK_WHOLE_SIZE, 0, &rptr);

    VkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = qfi;
    VkCommandPool pool;
    vkCreateCommandPool(dev, &cpci, 0, &pool);
    VkCommandBufferAllocateInfo cbai;
    memset(&cbai, 0, sizeof(cbai));
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &cbai, &cmd);
    VkCommandBufferBeginInfo cbbi;
    memset(&cbbi, 0, sizeof(cbbi));
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &cbbi);

    VkImageMemoryBarrier bar;
    memset(&bar, 0, sizeof(bar));
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.image = img;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1;
    bar.subresourceRange.layerCount = 1;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);

    VkBufferImageCopy regs[4];
    int nreg = 4;
    if (variant == 3) {
        /* a single copy with an image offset */
        nreg = 1;
        memset(&regs[0], 0, sizeof(regs[0]));
        regs[0].bufferOffset = 0;
        regs[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regs[0].imageSubresource.layerCount = 1;
        regs[0].imageOffset.x = 32;
        regs[0].imageOffset.y = 32;
        regs[0].imageExtent.width = 16;
        regs[0].imageExtent.height = 16;
        regs[0].imageExtent.depth = 1;
    } else if (variant == 4) {
        /* four separate copies, each with its own image offset */
        nreg = 1;
    }
    for (int t = 0; t < 4; t++) {
        memset(&regs[t], 0, sizeof(regs[t]));
        /* the source data of every tile lives at its own place in the buffer */
        regs[t].bufferOffset = (uint64_t)(t % 2) * 16 * W + (uint64_t)(t / 2) * 16 * W;
        if (variant == 1 || variant == 2) {
            regs[t].bufferRowLength = 16;
            regs[t].bufferImageHeight = 16;
        }
        regs[t].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regs[t].imageSubresource.layerCount = 1;
        if (variant == 0 || variant == 2) {
            regs[t].imageOffset.x = (t % 2) * 16;
            regs[t].imageOffset.y = (t / 2) * 16;
        }
        regs[t].imageExtent.width = 16;
        regs[t].imageExtent.height = 16;
        regs[t].imageExtent.depth = 1;
    }
    if (variant == 4) {
        for (int t = 0; t < 4; t++) {
            VkBufferImageCopy r;
            memset(&r, 0, sizeof(r));
            r.bufferOffset = (uint64_t)(t % 2) * 16 * W + (uint64_t)(t / 2) * 16 * W;
            r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r.imageSubresource.layerCount = 1;
            r.imageOffset.x = (t % 2) * 16;
            r.imageOffset.y = (t / 2) * 16;
            r.imageExtent.width = 16;
            r.imageExtent.height = 16;
            r.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(cmd, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        }
    } else {
        vkCmdCopyBufferToImage(cmd, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nreg, regs);
    }

    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    VkBufferImageCopy back;
    memset(&back, 0, sizeof(back));
    back.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    back.imageSubresource.layerCount = 1;
    back.imageExtent.width = W; back.imageExtent.height = H; back.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &back);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(dev, &fci, 0, &fence);
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);

    uint8_t *out = (uint8_t *)rptr;
    printf("variant %d: expecting the four values in the four corners\n", variant);
    const char *names[4] = { "top-left    ", "top-right   ", "bottom-left ", "bottom-right" };
    for (int t = 0; t < 4; t++) {
        int x = (t % 2) * 16 + 8;
        int y = (t / 2) * 16 + 8;
        uint8_t *p = out + (y * W + x) * 4;
        printf("  %s -> %02x %02x %02x %02x\n", names[t], p[0], p[1], p[2], p[3]);
    }
    /* where did the data actually end up? */
    printf("  non-zero 4 byte values found in the image:\n");
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = out + (y * W + x) * 4;
            if (p[0] || p[1] || p[2] || p[3]) {
                printf("    at %2d,%2d -> %02x %02x %02x %02x\n", x, y, p[0], p[1], p[2], p[3]);
                y = H; break;
            }
        }
    return 0;
}
