/*
 * Rewrite a shader that reads a texel buffer (uniform isamplerBuffer) so that
 * it reads a storage buffer instead.
 *
 * Why: the proprietary PowerVR driver refuses to compile a pipeline whose
 * descriptor is a VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER while the shader
 * uses a buffer image (OpTypeImage with Dim=Buffer).  The failure is
 * deterministic and shows up as VK_ERROR_UNKNOWN from vkCreateGraphicsPipelines.
 * The driver does accept the very same shader when the descriptor is a
 * combined image sampler, and it accepts a storage buffer descriptor as well,
 * so we move the buffer over to a storage buffer:
 *
 *   - the descriptor becomes a StorageBuffer variable holding a runtime array
 *     of int8 (the cloud data is R8_SINT),
 *   - every texelFetch() becomes an access chain + load + sign extension.
 *
 * The data path is unchanged: the application still hands us the same
 * VkBuffer, the compatibility layer only rewrites the descriptor.
 */
#ifndef PVR_TEXELBUF_H
#define PVR_TEXELBUF_H

#include <stddef.h>
#include <stdint.h>

/* returns a malloc'ed, patched copy of the module, or NULL if the module does
 * not use a texel buffer.  *out_words gets the size of the patched module. */
uint32_t *spv_texelbuf_to_storage(const uint32_t *code, size_t words, size_t *out_words);

/* forces the texture LOD to 0 (the driver computes the implicit LOD wrongly) */
uint32_t *spv_force_lod0(const uint32_t *code, size_t words, size_t *out_words);

#endif
