#include "pvr_texelbuf.h"

#include <stdlib.h>
#include <string.h>

/* SPIR-V opcodes we need */
#define OP_CAPABILITY              17
#define OP_TYPE_VOID               19
#define OP_TYPE_BOOL               20
#define OP_TYPE_INT                21
#define OP_TYPE_FLOAT              22
#define OP_TYPE_VECTOR             23
#define OP_TYPE_MATRIX             24
#define OP_TYPE_IMAGE              25
#define OP_TYPE_SAMPLER            26
#define OP_TYPE_SAMPLED_IMAGE      27
#define OP_TYPE_ARRAY              28
#define OP_TYPE_RUNTIME_ARRAY      29
#define OP_TYPE_STRUCT             30
#define OP_TYPE_POINTER            32
#define OP_TYPE_FUNCTION           33
#define OP_TYPE_FORWARD_POINTER    39
#define OP_CONSTANT                43
#define OP_VARIABLE                59
#define OP_LOAD                    61
#define OP_ACCESS_CHAIN            65
#define OP_DECORATE                71
#define OP_MEMBER_DECORATE         72
#define OP_IMAGE_FETCH             95
#define OP_IMAGE                   100
#define OP_SCONVERT                114
#define OP_COMPOSITE_CONSTRUCT     80

#define SC_UNIFORM_CONSTANT        0
#define SC_STORAGE_BUFFER          12

#define CAP_INT8                   39
#define CAP_STORAGE_BUFFER_8BIT    43

#define DIM_BUFFER                 5
#define DECORATION_BLOCK           2
#define DECORATION_OFFSET          35
#define DECORATION_ARRAY_STRIDE     6

#define SPIRV_MAGIC                0x07230203u

struct insn {
    uint32_t off;      /* word offset in the source module */
    uint16_t op;
    uint16_t wc;
};

static int is_type_op(uint16_t op)
{
    switch (op) {
    case OP_TYPE_VOID: case OP_TYPE_BOOL: case OP_TYPE_INT: case OP_TYPE_FLOAT:
    case OP_TYPE_VECTOR: case OP_TYPE_MATRIX: case OP_TYPE_IMAGE: case OP_TYPE_SAMPLER:
    case OP_TYPE_SAMPLED_IMAGE: case OP_TYPE_ARRAY: case OP_TYPE_RUNTIME_ARRAY:
    case OP_TYPE_STRUCT: case OP_TYPE_POINTER: case OP_TYPE_FUNCTION:
    case OP_TYPE_FORWARD_POINTER:
        return 1;
    default:
        return 0;
    }
}

static int decode(const uint32_t *w, size_t words, struct insn *out, size_t max, size_t *count)
{
    size_t i = 5, n = 0;
    while (i < words) {
        uint32_t h = w[i];
        uint16_t wc = (uint16_t)(h >> 16);
        uint16_t op = (uint16_t)(h & 0xFFFF);
        if (wc == 0 || i + wc > words)
            return -1;
        if (n >= max)
            return -1;
        out[n].off = (uint32_t)i;
        out[n].op = op;
        out[n].wc = wc;
        n++;
        i += wc;
    }
    *count = n;
    return 0;
}

uint32_t *spv_texelbuf_to_storage(const uint32_t *code, size_t words, size_t *out_words)
{
    struct insn *ins;
    size_t nins = 0, i;
    uint32_t image_id = 0, comp_type = 0, sampled_id = 0, ptr_id = 0, var_id = 0;
    uint32_t load_ids[16];
    int nloads = 0;
    uint32_t zero_id = 0;
    uint32_t fetch_vec = 0;
    uint32_t bound;
    uint32_t next_id;
    uint32_t int8_id, rtarr_id, struct_id, sb_ptr_id, elem_ptr_id;
    size_t first_type = 0;
    int have_first_type = 0;
    size_t nfetch = 0;
    uint32_t *out = NULL;
    size_t outn = 0, outcap = 0;

    if (!code || words < 6 || code[0] != SPIRV_MAGIC)
        return NULL;

    ins = malloc(sizeof(*ins) * 4096);
    if (!ins)
        return NULL;
    if (decode(code, words, ins, 4096, &nins) != 0) {
        free(ins);
        return NULL;
    }

    bound = code[3];

    for (i = 0; i < nins; i++) {
        const uint32_t *o = code + ins[i].off;
        switch (ins[i].op) {
        case OP_TYPE_IMAGE:
            if (ins[i].wc >= 8 && o[3] == DIM_BUFFER && !image_id) {
                image_id = o[1];
                comp_type = o[2];
            }
            break;
        case OP_TYPE_SAMPLED_IMAGE:
            if (ins[i].wc >= 3 && o[2] == image_id && !sampled_id)
                sampled_id = o[1];
            break;
        case OP_TYPE_POINTER:
            if (ins[i].wc >= 4 && o[2] == SC_UNIFORM_CONSTANT && o[3] == sampled_id && !ptr_id)
                ptr_id = o[1];
            break;
        case OP_VARIABLE:
            if (ins[i].wc >= 4 && o[1] == ptr_id && o[3] == SC_UNIFORM_CONSTANT && !var_id)
                var_id = o[2];
            break;
        case OP_LOAD:
            if (var_id && ins[i].wc >= 4 && o[3] == var_id && nloads < 16)
                load_ids[nloads++] = o[2];
            break;
        case OP_CONSTANT:
            /* an int 0 constant is needed to index the runtime array member */
            if (ins[i].wc >= 4 && o[1] == comp_type && o[3] == 0 && !zero_id)
                zero_id = o[2];
            break;
        case OP_IMAGE_FETCH:
            if (!fetch_vec)
                fetch_vec = o[1];
            break;
        default:
            break;
        }
    }
    if (!image_id || !sampled_id || !ptr_id || !var_id || !fetch_vec) {
        free(ins);
        return NULL;
    }

    next_id = bound;
#define NEWID() (next_id++)
    int zero_new = 0;
    if (!zero_id) {
        zero_id = NEWID();
        zero_new = 1;
    }
    int8_id = NEWID();
    rtarr_id = NEWID();
    struct_id = NEWID();
    sb_ptr_id = NEWID();
    elem_ptr_id = NEWID();

    /* the new type instructions, in dependency order */
    /*
     * The new instructions, in the order they have to appear:
     *   - the Block/Offset decorations for the storage buffer struct,
     *   - the types.
     */
    uint32_t newins[10][6];
    int nnew = 0;
#define NEWINSN(n) do {                                              \
        newins[nnew][0] = ((uint32_t)(n) << 16);                    \
        newins[nnew][1] = 0; newins[nnew][2] = 0;                   \
        newins[nnew][3] = 0; newins[nnew][4] = 0; newins[nnew][5] = 0; \
    } while (0)
    /* OpDecorate %struct Block */
    NEWINSN(3);
    newins[nnew][0] |= OP_DECORATE;
    newins[nnew][1] = struct_id;
    newins[nnew][2] = DECORATION_BLOCK;
    nnew++;
    /* OpMemberDecorate %struct 0 Offset 0 */
    NEWINSN(5);
    newins[nnew][0] |= OP_MEMBER_DECORATE;
    newins[nnew][1] = struct_id;
    newins[nnew][2] = 0;
    newins[nnew][3] = DECORATION_OFFSET;
    newins[nnew][4] = 0;
    nnew++;
    /* OpDecorate %rtarr ArrayStride 1 */
    NEWINSN(4);
    newins[nnew][0] |= OP_DECORATE;
    newins[nnew][1] = rtarr_id;
    newins[nnew][2] = DECORATION_ARRAY_STRIDE;
    newins[nnew][3] = 1;
    nnew++;
    /* OpTypeInt 8 1 */
    NEWINSN(4);
    newins[nnew][0] |= OP_TYPE_INT;
    newins[nnew][1] = int8_id;
    newins[nnew][2] = 8;
    newins[nnew][3] = 1;
    nnew++;
    /* OpTypeRuntimeArray %int8 */
    NEWINSN(3);
    newins[nnew][0] |= OP_TYPE_RUNTIME_ARRAY;
    newins[nnew][1] = rtarr_id;
    newins[nnew][2] = int8_id;
    nnew++;
    /* OpTypeStruct %rtarr */
    NEWINSN(3);
    newins[nnew][0] |= OP_TYPE_STRUCT;
    newins[nnew][1] = struct_id;
    newins[nnew][2] = rtarr_id;
    nnew++;
    /* OpTypePointer StorageBuffer %struct */
    NEWINSN(4);
    newins[nnew][0] |= OP_TYPE_POINTER;
    newins[nnew][1] = sb_ptr_id;
    newins[nnew][2] = SC_STORAGE_BUFFER;
    newins[nnew][3] = struct_id;
    nnew++;
    /* OpConstant %int 0 (only if the module does not have one) */
    if (zero_new) {
        NEWINSN(4);
        newins[nnew][0] |= OP_CONSTANT;
        newins[nnew][1] = comp_type;
        newins[nnew][2] = zero_id;
        newins[nnew][3] = 0;
        nnew++;
    }
    /* OpTypePointer StorageBuffer %int8 */
    NEWINSN(4);
    newins[nnew][0] |= OP_TYPE_POINTER;
    newins[nnew][1] = elem_ptr_id;
    newins[nnew][2] = SC_STORAGE_BUFFER;
    newins[nnew][3] = int8_id;
    nnew++;
#undef NEWINSN

    outcap = words + 256 + nins * 6;
    out = malloc(outcap * sizeof(uint32_t));
    if (!out) {
        free(ins);
        return NULL;
    }
    outn = 0;

    /* header */
    for (i = 0; i < 5; i++)
        out[outn++] = code[i];
    out[3] = next_id;

    /* capabilities */
    out[outn++] = (2u << 16) | OP_CAPABILITY;
    out[outn++] = CAP_INT8;
    out[outn++] = (2u << 16) | OP_CAPABILITY;
    out[outn++] = CAP_STORAGE_BUFFER_8BIT;

    /* find where the type section starts so the new types can be inserted */
    for (i = 0; i < nins; i++) {
        if (is_type_op(ins[i].op)) {
            first_type = i;
            have_first_type = 1;
            break;
        }
    }
    if (!have_first_type) {
        for (i = 0; i < nins; i++) {
            if (ins[i].op == OP_VARIABLE) {
                first_type = i;
                have_first_type = 1;
                break;
            }
        }
    }
    if (!have_first_type) {
        free(out);
        free(ins);
        return NULL;
    }

#define PUSH(w)                                        \
    do {                                               \
        if (outn + (w) > outcap) {                     \
            uint32_t *no = realloc(out, (outcap * 2 + (w)) * sizeof(uint32_t)); \
            if (!no) { free(out); free(ins); return NULL; } \
            out = no;                                  \
            outcap = outcap * 2 + (w);                 \
        }                                              \
    } while (0)

    for (i = 0; i < nins; i++) {
        const uint32_t *o = code + ins[i].off;
        uint16_t op = ins[i].op;
        uint16_t wc = ins[i].wc;

        if (i == first_type) {
            int k;
            for (k = 0; k < nnew; k++) {
                uint32_t wc = newins[k][0] >> 16;
                PUSH((int)wc);
                for (uint32_t q = 0; q < wc; q++)
                    out[outn++] = newins[k][q];
            }
        }

        if (op == OP_VARIABLE && wc >= 4 && o[2] == var_id) {
            PUSH(4);
            out[outn++] = (4u << 16) | OP_VARIABLE;
            out[outn++] = sb_ptr_id;
            out[outn++] = var_id;
            out[outn++] = SC_STORAGE_BUFFER;
            continue;
        }
        /* the load of the image and the OpImage that turns it into a sampled
         * image are not needed any more */
        if (op == OP_LOAD && wc >= 4 && o[3] == var_id)
            continue;
        if (op == OP_IMAGE && wc >= 4) {
            int k;
            for (k = 0; k < nloads; k++)
                if (o[3] == load_ids[k])
                    break;
            if (k < nloads)
                continue;
        }
        if (op == OP_IMAGE_FETCH && wc >= 5) {
            uint32_t coord = o[4];
            uint32_t acc = NEWID();
            uint32_t v8 = NEWID();
            uint32_t v32 = NEWID();
            PUSH(6 + 4 + 4 + 7);
            out[outn++] = (6u << 16) | OP_ACCESS_CHAIN;
            out[outn++] = elem_ptr_id;
            out[outn++] = acc;
            out[outn++] = var_id;
            out[outn++] = zero_id;   /* the runtime array member */
            out[outn++] = coord;     /* the index into it */
            out[outn++] = (4u << 16) | OP_LOAD;
            out[outn++] = int8_id;
            out[outn++] = v8;
            out[outn++] = acc;
            out[outn++] = (4u << 16) | OP_SCONVERT;
            out[outn++] = comp_type;
            out[outn++] = v32;
            out[outn++] = v8;
            out[outn++] = (7u << 16) | OP_COMPOSITE_CONSTRUCT;
            out[outn++] = fetch_vec;
            out[outn++] = o[2];   /* the original result id */
            out[outn++] = v32;
            out[outn++] = v32;
            out[outn++] = v32;
            out[outn++] = v32;
            nfetch++;
            continue;
        }
        PUSH(wc);
        for (uint16_t k = 0; k < wc; k++)
            out[outn++] = o[k];
    }
#undef PUSH

    if (!nfetch) {
        free(out);
        free(ins);
        return NULL;
    }
    /* the id bound grew while patching */
    out[3] = next_id;

    free(ins);
    *out_words = outn;
    return out;
}
