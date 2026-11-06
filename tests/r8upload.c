/*
 * Does the PowerVR driver upload R8_UNORM textures correctly?
 *
 * The game renders its text from an R8_UNORM font atlas; if the upload path
 * is broken the atlas contains garbage and the text comes out as a solid
 * block of colour.
 *
 * usage: r8upload
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "instance failed\n"); return 1; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof(mp));
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t ntypes = mp.memoryTypeCount;

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
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
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { fprintf(stderr, "device failed\n"); return 1; }
    VkQueue queue;
    vkGetDeviceQueue(dev, qfi, 0, &queue);

    const uint32_t W = 64, H = 64;
    uint8_t src[W * H];
    for (uint32_t i = 0; i < W * H; i++)
        src[i] = (uint8_t)(i * 3);

    /* a host visible staging buffer */
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = sizeof(src);
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    for (uint32_t i = 0; i < ntypes; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mai.memoryTypeIndex = i; break; }
    VkDeviceMemory mem;
    vkAllocateMemory(dev, &mai, NULL, &mem);
    vkBindBufferMemory(dev, buf, mem, 0);
    void *ptr = NULL;
    vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr);
    memcpy(ptr, src, sizeof(src));
    vkUnmapMemory(dev, mem);

    /* the R8 image */
    VkImageCreateInfo imgci;
    memset(&imgci, 0, sizeof(imgci));
    imgci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgci.imageType = VK_IMAGE_TYPE_2D;
    imgci.format = VK_FORMAT_R8_UNORM;
    imgci.extent.width = W; imgci.extent.height = H; imgci.extent.depth = 1;
    imgci.mipLevels = 1; imgci.arrayLayers = 1;
    imgci.samples = VK_SAMPLE_COUNT_1_BIT;
    imgci.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage img;
    VkResult r = vkCreateImage(dev, &imgci, NULL, &img);
    printf("vkCreateImage(R8_UNORM) r=%d\n", (int)r);
    if (r != VK_SUCCESS) return 1;
    VkMemoryRequirements imr;
    vkGetImageMemoryRequirements(dev, img, &imr);
    VkMemoryAllocateInfo imai = mai;
    imai.allocationSize = imr.size;
    for (uint32_t i = 0; i < ntypes; i++)
        if (imr.memoryTypeBits & (1u << i)) { imai.memoryTypeIndex = i; break; }
    VkDeviceMemory imem;
    vkAllocateMemory(dev, &imai, NULL, &imem);
    vkBindImageMemory(dev, img, imem, 0);

    /* read back buffer */
    VkBufferCreateInfo rbci = bci;
    rbci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer rbuf;
    vkCreateBuffer(dev, &rbci, NULL, &rbuf);
    VkMemoryRequirements rmr;
    vkGetBufferMemoryRequirements(dev, rbuf, &rmr);
    VkMemoryAllocateInfo rbmai = mai;
    rbmai.allocationSize = rmr.size;
    for (uint32_t i = 0; i < ntypes; i++)
        if ((rmr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { rbmai.memoryTypeIndex = i; break; }
    VkDeviceMemory rbmem;
    vkAllocateMemory(dev, &rbmai, NULL, &rbmem);
    vkBindBufferMemory(dev, rbuf, rbmem, 0);
    void *rptr = NULL;
    vkMapMemory(dev, rbmem, 0, VK_WHOLE_SIZE, 0, &rptr);

    VkCommandPoolCreateInfo cpci;
    memset(&cpci, 0, sizeof(cpci));
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = qfi;
    VkCommandPool pool;
    vkCreateCommandPool(dev, &cpci, NULL, &pool);
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
    VkBufferImageCopy bic;
    memset(&bic, 0, sizeof(bic));
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = W; bic.imageExtent.height = H; bic.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &bic);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);

    uint8_t *out = (uint8_t *)rptr;
    int bad = 0;
    for (uint32_t i = 0; i < W * H; i++)
        if (out[i] != src[i]) { bad++; }
    printf("R8 upload readback: %d of %d bytes differ\n", bad, W * H);
    printf("  first 8 bytes: ");
    for (int i = 0; i < 8; i++) printf("%02x ", out[i]);
    printf("\n  expected      : ");
    for (int i = 0; i < 8; i++) printf("%02x ", src[i]);
    printf("\n");
    return 0;
}
