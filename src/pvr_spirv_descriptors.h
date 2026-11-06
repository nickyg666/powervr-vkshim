/*
 * Extract the descriptor declarations of a SPIR-V module so that a pipeline
 * layout can be built that actually matches the shader.
 *
 * The PowerVR pipeline compiler rejects (flakily, and sometimes fatally)
 * pipelines whose descriptor set layout does not match the descriptor types
 * the shader declares, which is exactly what happens with Minecraft's order
 * independent transparency passes.
 */
#ifndef PVR_SPIRV_DESCRIPTORS_H
#define PVR_SPIRV_DESCRIPTORS_H

#include <stdint.h>
#include <string.h>
#ifdef PVR_DESC_DEBUG
#include <stdio.h>
#endif

#include <vulkan/vulkan_core.h>

#define PVR_MAX_DESC 32

struct pvr_desc {
    uint32_t set;
    uint32_t binding;
    VkDescriptorType type;
};

/* SPIR-V opcodes we need */
#define PVR_OP_TYPE_VOID 19
#define PVR_OP_TYPE_INT 21
#define PVR_OP_TYPE_FLOAT 22
#define PVR_OP_TYPE_VECTOR 23
#define PVR_OP_TYPE_IMAGE 25
#define PVR_OP_TYPE_SAMPLER 26
#define PVR_OP_TYPE_SAMPLED_IMAGE 27
#define PVR_OP_TYPE_ARRAY 28
#define PVR_OP_TYPE_RUNTIME_ARRAY 29
#define PVR_OP_TYPE_STRUCT 30
#define PVR_OP_TYPE_POINTER 32
#define PVR_OP_TYPE_FUNCTION 33
#define PVR_OP_VARIABLE 59
#define PVR_OP_DECORATE 71
#define PVR_OP_MEMBER_DECORATE 72
#define PVR_OP_TYPE_SAMPLER_CAPABILITY 44

#define PVR_DECORATION_BLOCK 2
#define PVR_DECORATION_BUFFER_BLOCK 3
#define PVR_DECORATION_BINDING 33
#define PVR_DECORATION_DESCRIPTOR_SET 34

#define PVR_STORAGE_UNIFORM_CONSTANT 0
#define PVR_STORAGE_UNIFORM 2
#define PVR_STORAGE_STORAGE_BUFFER 12

struct pvr_spv_types {
    uint32_t storage[256];  /* id -> storage class */
    uint32_t pointee[256];  /* id -> type id */
    uint32_t kind[256];     /* id -> PVR kind */
    uint32_t sampled[256];  /* id -> sampled flag for images */
    uint32_t member_block[256];
    uint32_t binding[256];
    uint32_t set[256];
    uint32_t has_binding[256];
    uint32_t has_set[256];
    uint32_t block[256];
};

static uint32_t pvr_kind_of(const struct pvr_spv_types *t, uint32_t id)
{
    uint32_t k = t->kind[id];
    if (k == PVR_OP_TYPE_POINTER)
        return pvr_kind_of(t, t->pointee[id]);
    if (k == PVR_OP_TYPE_ARRAY)
        return pvr_kind_of(t, t->pointee[id]);
    return k;
}

static uint32_t pvr_pointee_of(const struct pvr_spv_types *t, uint32_t id)
{
    uint32_t k = t->kind[id];
    if (k == PVR_OP_TYPE_POINTER)
        return t->pointee[id];
    if (k == PVR_OP_TYPE_ARRAY)
        return t->pointee[id];
    return 0;
}

/* Parse the descriptor declarations of a module.  Returns the number found. */
static int pvr_parse_descriptors(const uint32_t *w, size_t words, struct pvr_desc *out, int max)
{
    struct pvr_spv_types t;
    memset(&t, 0, sizeof(t));

    size_t i = 5;
    while (i < words) {
        uint32_t op = w[i] & 0xffffu;
        uint32_t len = w[i] >> 16;
        if (!len || i + len > words)
            break;
        switch (op) {
        case PVR_OP_TYPE_POINTER:
            if (len >= 4 && w[i + 1] < 256) {
                t.storage[w[i + 1]] = w[i + 2];
                t.pointee[w[i + 1]] = w[i + 3];
                t.kind[w[i + 1]] = PVR_OP_TYPE_POINTER;
            }
            break;
        case PVR_OP_TYPE_SAMPLER:
            if (len >= 2 && w[i + 1] < 256)
                t.kind[w[i + 1]] = PVR_OP_TYPE_SAMPLER;
            break;
        case PVR_OP_TYPE_SAMPLED_IMAGE:
            if (len >= 2 && w[i + 1] < 256)
                t.kind[w[i + 1]] = PVR_OP_TYPE_SAMPLED_IMAGE;
            break;
        case PVR_OP_TYPE_IMAGE:
            /* OpTypeImage: result, sampledType, dim, depth, arrayed, ms, sampled, format */
            if (len >= 8 && w[i + 1] < 256) {
                t.kind[w[i + 1]] = PVR_OP_TYPE_IMAGE;
                t.sampled[w[i + 1]] = w[i + 7];
            }
            break;
        case PVR_OP_TYPE_ARRAY:
        case PVR_OP_TYPE_RUNTIME_ARRAY:
            if (len >= 4 && w[i + 1] < 256) {
                t.kind[w[i + 1]] = op;
                t.pointee[w[i + 1]] = w[i + 3];
            }
            break;
        case PVR_OP_TYPE_STRUCT:
            if (len >= 2 && w[i + 1] < 256)
                t.kind[w[i + 1]] = PVR_OP_TYPE_STRUCT;
            break;
        case PVR_OP_VARIABLE:
            /* OpVariable: result type, result id, storage class, [initializer] */
            if (len >= 4 && w[i + 2] < 256) {
                t.storage[w[i + 2]] = w[i + 3];
                t.pointee[w[i + 2]] = t.pointee[w[i + 1]]; /* the resource type */
                t.kind[w[i + 2]] = 1000;                   /* variable */
            }
            break;
        case PVR_OP_DECORATE:
            if (len >= 3 && w[i + 1] < 256) {
                uint32_t dec = w[i + 2];
                uint32_t val = (len >= 4) ? w[i + 3] : 0;
                if (dec == PVR_DECORATION_BINDING) {
                    t.binding[w[i + 1]] = val;
                    t.has_binding[w[i + 1]] = 1;
                } else if (dec == PVR_DECORATION_DESCRIPTOR_SET) {
                    t.set[w[i + 1]] = val;
                    t.has_set[w[i + 1]] = 1;
                } else if (dec == PVR_DECORATION_BLOCK || dec == PVR_DECORATION_BUFFER_BLOCK) {
                    t.block[w[i + 1]] = 1;
                }
            }
            break;
        default:
            break;
        }
        i += len;
    }

    int n = 0;
    for (uint32_t id = 0; id < 256; id++) {
        if (t.kind[id] != 1000 || !t.has_binding[id])
            continue;
        uint32_t storage = t.storage[id];
        uint32_t res = t.pointee[id]; /* the resource type */
#ifdef PVR_DESC_DEBUG
        fprintf(stderr, "var %u storage=%u res=%u kind=%u block=%u\n", id, storage, res,
                t.kind[res], t.block[res]);
#endif
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;

        if (storage == PVR_STORAGE_UNIFORM_CONSTANT) {
            uint32_t k = pvr_kind_of(&t, res);
            if (k == PVR_OP_TYPE_SAMPLER)
                type = VK_DESCRIPTOR_TYPE_SAMPLER;
            else if (k == PVR_OP_TYPE_SAMPLED_IMAGE)
                type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            else if (k == PVR_OP_TYPE_IMAGE)
                type = t.sampled[res] ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        } else if (storage == PVR_STORAGE_UNIFORM) {
            uint32_t k = pvr_kind_of(&t, res);
            if (k == PVR_OP_TYPE_STRUCT)
                type = t.block[res] ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                    : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            else
                type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        } else if (storage == PVR_STORAGE_STORAGE_BUFFER) {
            type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        if (type == VK_DESCRIPTOR_TYPE_MAX_ENUM)
            continue;
        if (n >= max)
            break;
        out[n].set = t.has_set[id] ? t.set[id] : 0;
        out[n].binding = t.binding[id];
        out[n].type = type;
        n++;
    }
    return n;
}

#endif /* PVR_SPIRV_DESCRIPTORS_H */
