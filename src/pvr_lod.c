/*
 * Force the texture LOD to 0 in every fragment shader.
 *
 * The proprietary driver computes the implicit LOD (the screen space
 * derivatives of the texture coordinates) wrongly, so every texture that has
 * mip levels is sampled from a garbage mip: the world turns into a psychedelic
 * mess while textures without mips (the font atlas, the widget atlas) are
 * fine.  Sampling level 0 always is correct, just aliased in the distance.
 */
#include "pvr_texelbuf.h"

#include <stdlib.h>
#include <string.h>

#define OP_TYPE_FLOAT      22
#define OP_CONSTANT        43
#define OP_FUNCTION        54
#define OP_IMAGE_SAMPLE_IMPLICIT 87
#define OP_IMAGE_SAMPLE_EXPLICIT 88

uint32_t *spv_force_lod0(const uint32_t *in, size_t nwords, size_t *out_words)
{
    size_t i = 5, nimg = 0;
    uint32_t float_type = 0, zero_id = 0;
    uint32_t new_bound;

    if (!in || nwords < 6)
        return NULL;
    new_bound = in[3];

    /* the float type and a float constant 0 */
    for (i = 5; i < nwords;) {
        uint32_t w = in[i];
        uint32_t op = w & 0xffffu;
        uint32_t len = w >> 16;
        if (!len || i + len > nwords)
            break;
        if (op == OP_TYPE_FLOAT && len >= 2 && !float_type)
            float_type = in[i + 1];
        if (op == OP_CONSTANT && len >= 4 && float_type && in[i + 1] == float_type &&
            in[i + 3] == 0 && !zero_id)
            zero_id = in[i + 2];
        i += len;
    }
    if (!float_type)
        return NULL;

    /* count the implicit samples */
    for (i = 5; i < nwords;) {
        uint32_t w = in[i];
        uint32_t op = w & 0xffffu;
        uint32_t len = w >> 16;
        if (!len || i + len > nwords)
            break;
        if (op == OP_IMAGE_SAMPLE_IMPLICIT)
            nimg++;
        i += len;
    }
    if (!nimg)
        return NULL;

    int need_zero = (zero_id == 0);
    if (need_zero) {
        zero_id = new_bound++;
        if (new_bound <= in[3])
            return NULL;
    }

    size_t cap = nwords + 64 + nimg;
    uint32_t *out = malloc(cap * 4);
    if (!out)
        return NULL;
    size_t o = 0;
    memcpy(out, in, 5 * 4);
    out[3] = new_bound;
    o = 5;

    int inserted_zero = 0;
    for (i = 5; i < nwords;) {
        uint32_t w = in[i];
        uint32_t op = w & 0xffffu;
        uint32_t len = w >> 16;
        if (!len || i + len > nwords) {
            memcpy(out + o, in + i, (nwords - i) * 4);
            o += nwords - i;
            break;
        }

        /* the zero constant has to live in the constants section */
        if (!inserted_zero && op == OP_FUNCTION) {
            if (o + 4 > cap) {
                cap = (o + 4) * 2;
                uint32_t *tmp = realloc(out, cap * 4);
                if (!tmp) {
                    free(out);
                    return NULL;
                }
                out = tmp;
            }
            out[o++] = (4u << 16) | OP_CONSTANT;
            out[o++] = float_type;
            out[o++] = zero_id;
            out[o++] = 0;
            inserted_zero = 1;
        }

        if (op == OP_IMAGE_SAMPLE_IMPLICIT && len >= 5) {
            if (o + 7 > cap) {
                cap = (o + 7) * 2;
                uint32_t *tmp = realloc(out, cap * 4);
                if (!tmp) {
                    free(out);
                    return NULL;
                }
                out = tmp;
            }
            /*
             * OpImageSampleExplicitLod %type %id %image %coord Lod %lod
             * The image operand mask 0x2 selects "Lod", its value follows.
             */
            out[o++] = (7u << 16) | OP_IMAGE_SAMPLE_EXPLICIT;
            out[o++] = in[i + 1];
            out[o++] = in[i + 2];
            out[o++] = in[i + 3];
            out[o++] = in[i + 4];
            out[o++] = 0x2;   /* image operand mask: Lod */
            out[o++] = zero_id;
            i += len;
            continue;
        }

        if (o + len > cap) {
            cap = (o + len) * 2;
            uint32_t *tmp = realloc(out, cap * 4);
            if (!tmp) {
                free(out);
                return NULL;
            }
            out = tmp;
        }
        memcpy(out + o, in + i, len * 4);
        o += len;
        i += len;
    }

    *out_words = o;
    return out;
}
