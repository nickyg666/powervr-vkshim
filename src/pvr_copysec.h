/*
 * pvr_copysec.h — route copy/blit commands through secondary command
 * buffers to defeat the PowerVR "one copy per command buffer" bug.
 */
#ifndef PVR_COPYSEC_H
#define PVR_COPYSEC_H

#include <vulkan/vulkan.h>

typedef PFN_vkGetDeviceProcAddr gdpfunc;

/* device init; may be called repeatedly */
void cs_bind(VkDevice dev, gdpfunc gd);

/* lifecycle tracking called from wrapped loader entry points */
void cs_track_alloc(VkDevice dev, const VkCommandBufferAllocateInfo *ai,
                    const VkCommandBuffer *cbs);
void cs_track_free(VkDevice dev, uint32_t n, const VkCommandBuffer *cbs);
void cs_track_begin(VkCommandBuffer cb);   /* app re-recording the primary */
void cs_track_reset(VkCommandBuffer cb);   /* vkResetCommandBuffer */
void cs_track_resetpool(VkDevice dev, VkCommandPool pool);
int cs_in_pass(VkCommandBuffer cb);
void cs_track_beginpass(VkCommandBuffer cb);
void cs_track_endpass(VkCommandBuffer cb);
void cs_destroy(void);

/* copy routing.  each returns 1 when the call was executed through a
 * secondary (caller must not also call the real function) */
int cs_route_bti(VkCommandBuffer prim, VkBuffer src, VkImage dst,
                 VkImageLayout layout, uint32_t n, const VkBufferImageCopy *rgs);
int cs_route_bti2(VkCommandBuffer prim, VkBuffer src, VkImage dst,
                  VkImageLayout layout, uint32_t n, const VkBufferImageCopy2 *rgs);
int cs_route_itb(VkCommandBuffer prim, VkImage src, VkImageLayout layout,
                 VkBuffer dst, uint32_t n, const VkBufferImageCopy *rgs);
int cs_route_itb2(VkCommandBuffer prim, VkImage src, VkImageLayout layout,
                  VkBuffer dst, uint32_t n, const VkBufferImageCopy2 *rgs);
int cs_route_copyimage(VkCommandBuffer prim, VkImage src, VkImageLayout sl,
                       VkImage dst, VkImageLayout dl, uint32_t n,
                       const VkImageCopy *rgs);
int cs_route_copyimage2(VkCommandBuffer prim, VkImage src, VkImageLayout sl,
                        VkImage dst, VkImageLayout dl, uint32_t n,
                        const VkImageCopy2 *rgs);
int cs_route_blit(VkCommandBuffer prim, VkImage src, VkImageLayout sl,
                  VkImage dst, VkImageLayout dl, uint32_t n,
                  const VkImageBlit *rgs, VkFilter filter);
int cs_route_blit2(VkCommandBuffer prim, VkImage src, VkImageLayout sl,
                   VkImage dst, VkImageLayout dl, uint32_t n,
                   const VkImageBlit2 *rgs, VkFilter filter);

#endif
