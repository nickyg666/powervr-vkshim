/*
 * pvr_copysec.c — execute copy/blit commands through secondary buffers.
 *
 * The PowerVR proprietary driver loses every copy that arrives after a
 * first copy inside one command buffer: a second vkCmdCopyBufferToImage /
 * vkCmdCopyImage / vkCmdBlitImage recorded directly into the same primary
 * command buffer is dropped or truncated (primtest3: 3 copies in a
 * primary -> only the first lands; primtest5 W1: the same copies executed
 * through a secondary all land — 100-copy atlas stitch, 0/100 tiles wrong).
 *
 * Workaround: when the game records a copy into a primary command buffer,
 * record it into our own secondary command buffer and execute it
 * immediately with vkCmdExecuteCommands on the primary.  All copies then
 * run correctly and the position in the command sequence is unchanged.
 *
 * Secondaries are recycled when the app re-records the primary
 * (vkBegin/vkFree/vkReset on the same handle) — the app's own sync point,
 * where the primary can no longer be pending execution.
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdarg.h>

#include "pvr_copysec.h"

static pthread_mutex_t cs_lck = PTHREAD_MUTEX_INITIALIZER;

static void cs_log(const char *fmt, ...)
{
    static long long count;
    long long i = ++count;
    if (i > 40)
        return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    FILE *f = fopen("/tmp/vkcompat/shim.log", "a");
    if (f) {
        fprintf(f, "[copysec %lld] %s\n", i, msg);
        fclose(f);
    }
    fprintf(stderr, "vkcompat[copysec %lld]: %s\n", i, msg);
}

/* ------------------------------------------------------------------ */
/* command buffer tracking table                                       */
/* ------------------------------------------------------------------ */
#define TRACKN 256
struct trec {
    VkCommandBuffer cb;
    unsigned char in_pass;      /* recording inside a render pass */
    unsigned char is_secondary; /* the app allocated this as a secondary */
    unsigned char used;
};
static struct trec table[TRACKN];

static struct trec *t_find(VkCommandBuffer cb)
{
    for (int i = 0; i < TRACKN; i++)
        if (table[i].used && table[i].cb == cb)
            return &table[i];
    return NULL;
}

static struct trec *t_alloc(VkCommandBuffer cb)
{
    for (int i = 0; i < TRACKN; i++)
        if (!table[i].used) {
            memset(&table[i], 0, sizeof(table[i]));
            table[i].cb = cb;
            table[i].used = 1;
            return &table[i];
        }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* next-layer state                                                    */
/* ------------------------------------------------------------------ */
#define BATCH 64
#define MAXBATCH 32
#define OWNERMAX 64
#define SECPERPRIMMAX 8192

struct owner {
    VkCommandBuffer prim;
    VkCommandBuffer *secs;
    int cnt;
    int cap;
};

struct cs {
    VkDevice dev;
    gdpfunc gd;
    int resolved;
    VkCommandPool pool;

    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdExecuteCommands CmdExecuteCommands;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCmdCopyBufferToImage2 CmdCopyBufferToImage2;
    PFN_vkCmdCopyImageToBuffer2 CmdCopyImageToBuffer2;
    PFN_vkCmdCopyImage2 CmdCopyImage2;
    PFN_vkCmdBlitImage2 CmdBlitImage2;

    VkCommandBuffer batches[MAXBATCH][BATCH];
    int nbatches;
    VkCommandBuffer freelist[MAXBATCH * BATCH];
    int freecnt;

    struct owner owners[OWNERMAX];

    long long nroutes;
};
static struct cs CS;

static void cs_reclaim_owner(struct cs *s, struct owner *o);

static int cs_envoff(void)
{
    const char *e = getenv("VKC_NO_COPYSEC");
    return e && e[0] == '1';
}

void cs_bind(VkDevice dev, gdpfunc gd)
{
    if (!dev || !gd)
        return;
    pthread_mutex_lock(&cs_lck);
    if (CS.dev == dev && CS.resolved) {
        pthread_mutex_unlock(&cs_lck);
        return;
    }
    if (CS.dev != dev) {
        for (int i = 0; i < OWNERMAX; i++) {
            free(CS.owners[i].secs);
            memset(&CS.owners[i], 0, sizeof(CS.owners[i]));
        }
        CS.nbatches = 0;
        CS.freecnt = 0;
        CS.pool = VK_NULL_HANDLE;
        memset(CS.batches, 0, sizeof(CS.batches));
        memset(CS.freelist, 0, sizeof(CS.freelist));
    }
    CS.dev = dev;
    CS.gd = gd;
#define Q(n) CS.n = (PFN_vk##n)CS.gd(CS.dev, "vk" #n)
    Q(CreateCommandPool);
    Q(DestroyCommandPool);
    Q(AllocateCommandBuffers);
    Q(FreeCommandBuffers);
    Q(ResetCommandBuffer);
    Q(BeginCommandBuffer);
    Q(EndCommandBuffer);
    Q(CmdExecuteCommands);
    Q(CmdCopyBufferToImage);
    Q(CmdCopyImageToBuffer);
    Q(CmdCopyImage);
    Q(CmdBlitImage);
    Q(CmdCopyBufferToImage2);
    Q(CmdCopyImageToBuffer2);
    Q(CmdCopyImage2);
    Q(CmdBlitImage2);
#undef Q
    if (!CS.pool && CS.CreateCommandPool) {
        VkCommandPoolCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (CS.CreateCommandPool(CS.dev, &ci, NULL, &CS.pool) != VK_SUCCESS) {
            cs_log("secondary command pool create failed; copy workaround off");
            CS.pool = VK_NULL_HANDLE;
        }
    }
    CS.resolved = 1;
    pthread_mutex_unlock(&cs_lck);
}

static VkCommandBuffer cs_sec_get(struct cs *s)
{
    if (s->freecnt > 0)
        return s->freelist[--s->freecnt];
    if (s->nbatches >= MAXBATCH)
        return VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = s->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    ai.commandBufferCount = BATCH;
    int b = s->nbatches++;
    if (s->AllocateCommandBuffers(s->dev, &ai, s->batches[b]) != VK_SUCCESS) {
        s->nbatches--;
        cs_log("secondary allocate failed; copy workaround limited");
        return VK_NULL_HANDLE;
    }
    for (int k = 1; k < BATCH; k++)
        s->freelist[s->freecnt++] = s->batches[b][k];
    return s->batches[b][0];
}

static void cs_sec_relist(struct cs *s, VkCommandBuffer sec)
{
    if (s->freecnt < MAXBATCH * BATCH)
        s->freelist[s->freecnt++] = sec;
}

static struct owner *cs_owner_of(struct cs *s, VkCommandBuffer prim)
{
    struct owner *free_slot = NULL;
    for (int i = 0; i < OWNERMAX; i++) {
        if (s->owners[i].secs && s->owners[i].prim == prim)
            return &s->owners[i];
        if (!s->owners[i].secs && !free_slot)
            free_slot = &s->owners[i];
    }
    struct owner *o = free_slot;
    if (!o) {
        o = &s->owners[0];
        for (int i = 0; i < OWNERMAX; i++)
            if (s->owners[i].secs && s->owners[i].cap < o->cap)
                o = &s->owners[i];
        cs_reclaim_owner(s, o);
        cs_log("owner table full; recycled a primary slot");
    }
    o->prim = prim;
    if (!o->secs) {
        o->cap = 16;
        o->secs = malloc((size_t)o->cap * sizeof(VkCommandBuffer));
        o->cnt = 0;
    }
    if (!o->secs)
        return NULL;
    return o;
}

static void cs_reclaim_owner(struct cs *s, struct owner *o)
{
    if (!o->secs)
        return;
    for (int i = 0; i < o->cnt; i++) {
        if (s->pool)
            s->ResetCommandBuffer(o->secs[i], 0);
        cs_sec_relist(s, o->secs[i]);
    }
    o->cnt = 0;
    o->prim = VK_NULL_HANDLE;
    free(o->secs);
    o->secs = NULL;
    o->cap = 0;
}

void cs_track_begin(VkCommandBuffer cb)
{
    if (!CS.resolved)
        return;
    pthread_mutex_lock(&cs_lck);
    for (int i = 0; i < OWNERMAX; i++)
        if (CS.owners[i].secs && CS.owners[i].prim == cb) {
            cs_reclaim_owner(&CS, &CS.owners[i]);
            break;
        }
    struct trec *t = t_find(cb);
    if (t)
        t->in_pass = 0;
    pthread_mutex_unlock(&cs_lck);
}

void cs_track_alloc(VkDevice dev, const VkCommandBufferAllocateInfo *ai,
                    const VkCommandBuffer *cbs)
{
    if (!dev || dev != CS.dev || !ai || !cbs)
        return;
    pthread_mutex_lock(&cs_lck);
    for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
        struct trec *t = t_alloc(cbs[i]);
        if (t)
            t->is_secondary = (ai->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    }
    pthread_mutex_unlock(&cs_lck);
}

void cs_track_free(VkDevice dev, uint32_t n, const VkCommandBuffer *cbs)
{
    if (!dev || dev != CS.dev || !cbs)
        return;
    pthread_mutex_lock(&cs_lck);
    for (uint32_t i = 0; i < n; i++) {
        for (int j = 0; j < OWNERMAX; j++)
            if (CS.owners[j].secs && CS.owners[j].prim == cbs[i]) {
                cs_reclaim_owner(&CS, &CS.owners[j]);
                break;
            }
        struct trec *t = t_find(cbs[i]);
        if (t) {
            t->used = 0;
            t->in_pass = 0;
        }
    }
    pthread_mutex_unlock(&cs_lck);
}

void cs_track_reset(VkCommandBuffer cb)
{
    if (!CS.resolved)
        return;
    pthread_mutex_lock(&cs_lck);
    for (int i = 0; i < OWNERMAX; i++)
        if (CS.owners[i].secs && CS.owners[i].prim == cb) {
            cs_reclaim_owner(&CS, &CS.owners[i]);
            break;
        }
    pthread_mutex_unlock(&cs_lck);
}

void cs_track_resetpool(VkDevice dev, VkCommandPool pool)
{
    if (!dev || dev != CS.dev || !pool)
        return;
    /* a pool reset invalidates every cmdbuf recorded from it */
    pthread_mutex_lock(&cs_lck);
    for (int i = 0; i < OWNERMAX; i++)
        if (CS.owners[i].secs)
            cs_reclaim_owner(&CS, &CS.owners[i]);
    pthread_mutex_unlock(&cs_lck);
}

int cs_in_pass(VkCommandBuffer cb)
{
    struct trec *t = t_find(cb);
    return t && t->in_pass;
}

static void cs_track_pass(VkCommandBuffer cb, int set)
{
    pthread_mutex_lock(&cs_lck);
    struct trec *t = t_find(cb);
    if (set && !t)
        t = t_alloc(cb);
    if (t)
        t->in_pass = set ? 1 : 0;
    pthread_mutex_unlock(&cs_lck);
}

void cs_track_beginpass(VkCommandBuffer cb)
{
    cs_track_pass(cb, 1);
}

void cs_track_endpass(VkCommandBuffer cb)
{
    cs_track_pass(cb, 0);
}

void cs_destroy(void)
{
    pthread_mutex_lock(&cs_lck);
    if (CS.pool) {
        for (int b = 0; b < CS.nbatches; b++)
            CS.FreeCommandBuffers(CS.dev, CS.pool, BATCH, CS.batches[b]);
        CS.DestroyCommandPool(CS.dev, CS.pool, NULL);
    }
    memset(&CS, 0, sizeof(CS));
    memset(table, 0, sizeof(table));
    pthread_mutex_unlock(&cs_lck);
}

/* ---------------- routing ---------------- */

static int route_ok(VkCommandBuffer prim)
{
    if (cs_envoff()) { cs_log("reject: VKC_NO_COPYSEC"); return 0; }
    if (!CS.resolved || !CS.pool || !CS.CmdExecuteCommands) {
        cs_log("reject: resolved=%d pool=%p exec=%p dev=%p", CS.resolved,
               (void*)CS.pool, (void*)CS.CmdExecuteCommands, (void*)CS.dev);
        return 0;
    }
    struct trec *t = t_find(prim);
    if (t && t->is_secondary)
        return 0; /* vkCmdExecuteCommands is invalid inside a secondary */
    if (t && t->in_pass)
        return 0; /* secondary execution inside a pass needs inheritance */
    return 1;
}

static int route_generic(VkCommandBuffer prim, uint32_t n,
                         void (*record)(VkCommandBuffer sec, void *ud), void *ud)
{
    struct owner *o;
    VkCommandBuffer sec;
    VkCommandBufferBeginInfo bi;
    int handled = 0;
    if (n == 0 || !route_ok(prim))
        return 0;
    pthread_mutex_lock(&cs_lck);
    o = cs_owner_of(&CS, prim);
    cs_log("route attempt prim=%p owner=%p", (void*)prim, (void*)o);
    if (o) {
        sec = cs_sec_get(&CS);
        if (sec && o->cnt < SECPERPRIMMAX) {
            if (o->cap == o->cnt) {
                int nc = o->cap * 2;
                VkCommandBuffer *ns =
                    realloc(o->secs, (size_t)nc * sizeof(VkCommandBuffer));
                if (ns) {
                    o->secs = ns;
                    o->cap = nc;
                }
            }
            if (o->cap != o->cnt) {
                memset(&bi, 0, sizeof(bi));
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                o->secs[o->cnt++] = sec;
                CS.BeginCommandBuffer(sec, &bi);
                record(sec, ud);
                CS.EndCommandBuffer(sec);
                CS.CmdExecuteCommands(prim, 1, &sec);
                CS.nroutes++;
                handled = 1;
            }
        }
    }
    pthread_mutex_unlock(&cs_lck);
    return handled;
}

/* per family record callbacks + public routes */
struct cb_bti {
    VkBuffer src;
    VkImage dst;
    VkImageLayout lay;
    uint32_t n;
    const VkBufferImageCopy *rgs;
};

static void rec_bti(VkCommandBuffer sec, void *p)
{
    struct cb_bti *a = p;
    for (uint32_t i = 0; i < a->n; i++) {
        VkBufferImageCopy r = a->rgs[i];
        CS.CmdCopyBufferToImage(sec, a->src, a->dst, a->lay, 1, &r);
    }
}

int cs_route_bti(VkCommandBuffer prim, VkBuffer src, VkImage dst,
                 VkImageLayout layout, uint32_t n, const VkBufferImageCopy *rgs)
{
    struct cb_bti a = {src, dst, layout, n, rgs};
    return route_generic(prim, n, rec_bti, &a);
}

struct cb_bti2 {
    VkBuffer src;
    VkImage dst;
    VkImageLayout lay;
    uint32_t n;
    const VkBufferImageCopy2 *rgs;
};

static void rec_bti2(VkCommandBuffer sec, void *p)
{
    struct cb_bti2 *a = p;
    VkCopyBufferToImageInfo2 info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
    info.srcBuffer = a->src;
    info.dstImage = a->dst;
    info.dstImageLayout = a->lay;
    for (uint32_t i = 0; i < a->n; i++) {
        info.regionCount = 1;
        info.pRegions = &a->rgs[i];
        CS.CmdCopyBufferToImage2(sec, &info);
    }
}

int cs_route_bti2(VkCommandBuffer prim, VkBuffer src, VkImage dst,
                  VkImageLayout layout, uint32_t n, const VkBufferImageCopy2 *rgs)
{
    struct cb_bti2 a = {src, dst, layout, n, rgs};
    return route_generic(prim, n, rec_bti2, &a);
}

struct cb_itb {
    VkImage src;
    VkImageLayout lay;
    VkBuffer dst;
    uint32_t n;
    const VkBufferImageCopy *rgs;
};

static void rec_itb(VkCommandBuffer sec, void *p)
{
    struct cb_itb *a = p;
    for (uint32_t i = 0; i < a->n; i++) {
        VkBufferImageCopy r = a->rgs[i];
        CS.CmdCopyImageToBuffer(sec, a->src, a->lay, a->dst, 1, &r);
    }
}

int cs_route_itb(VkCommandBuffer prim, VkImage src, VkImageLayout layout,
                 VkBuffer dst, uint32_t n, const VkBufferImageCopy *rgs)
{
    struct cb_itb a = {src, layout, dst, n, rgs};
    return route_generic(prim, n, rec_itb, &a);
}

struct cb_itb2 {
    VkImage src;
    VkImageLayout lay;
    VkBuffer dst;
    uint32_t n;
    const VkBufferImageCopy2 *rgs;
};

static void rec_itb2(VkCommandBuffer sec, void *p)
{
    struct cb_itb2 *a = p;
    VkCopyImageToBufferInfo2 info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
    info.srcImage = a->src;
    info.srcImageLayout = a->lay;
    info.dstBuffer = a->dst;
    for (uint32_t i = 0; i < a->n; i++) {
        info.regionCount = 1;
        info.pRegions = &a->rgs[i];
        CS.CmdCopyImageToBuffer2(sec, &info);
    }
}

int cs_route_itb2(VkCommandBuffer prim, VkImage src, VkImageLayout layout,
                  VkBuffer dst, uint32_t n, const VkBufferImageCopy2 *rgs)
{
    struct cb_itb2 a = {src, layout, dst, n, rgs};
    return route_generic(prim, n, rec_itb2, &a);
}

struct cb_ci {
    VkImage src, dst;
    VkImageLayout sl, dl;
    uint32_t n;
    const VkImageCopy *rgs;
};

static void rec_ci(VkCommandBuffer sec, void *p)
{
    struct cb_ci *a = p;
    for (uint32_t i = 0; i < a->n; i++) {
        VkImageCopy r = a->rgs[i];
        CS.CmdCopyImage(sec, a->src, a->sl, a->dst, a->dl, 1, &r);
    }
}

int cs_route_copyimage(VkCommandBuffer prim, VkImage src, VkImageLayout sl,
                       VkImage dst, VkImageLayout dl, uint32_t n,
                       const VkImageCopy *rgs)
{
    struct cb_ci a = {src, dst, sl, dl, n, rgs};
    return route_generic(prim, n, rec_ci, &a);
}

struct cb_ci2 {
    VkImage src, dst;
    VkImageLayout sl, dl;
    uint32_t n;
    const VkImageCopy2 *rgs;
};

static void rec_ci2(VkCommandBuffer sec, void *p)
{
    struct cb_ci2 *a = p;
    VkCopyImageInfo2 info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2;
    info.srcImage = a->src;
    info.srcImageLayout = a->sl;
    info.dstImage = a->dst;
    info.dstImageLayout = a->dl;
    for (uint32_t i = 0; i < a->n; i++) {
        info.regionCount = 1;
        info.pRegions = &a->rgs[i];
        CS.CmdCopyImage2(sec, &info);
    }
}

int cs_route_copyimage2(VkCommandBuffer prim, VkImage src, VkImageLayout sl,
                        VkImage dst, VkImageLayout dl, uint32_t n,
                        const VkImageCopy2 *rgs)
{
    struct cb_ci2 a = {src, dst, sl, dl, n, rgs};
    return route_generic(prim, n, rec_ci2, &a);
}

struct cb_bi {
    VkImage src, dst;
    VkImageLayout sl, dl;
    uint32_t n;
    const VkImageBlit *rgs;
    VkFilter filter;
};

static void rec_bi(VkCommandBuffer sec, void *p)
{
    struct cb_bi *a = p;
    for (uint32_t i = 0; i < a->n; i++) {
        VkImageBlit r = a->rgs[i];
        CS.CmdBlitImage(sec, a->src, a->sl, a->dst, a->dl, 1, &r, a->filter);
    }
}

int cs_route_blit(VkCommandBuffer prim, VkImage src, VkImageLayout sl,
                  VkImage dst, VkImageLayout dl, uint32_t n,
                  const VkImageBlit *rgs, VkFilter filter)
{
    struct cb_bi a = {src, dst, sl, dl, n, rgs, filter};
    return route_generic(prim, n, rec_bi, &a);
}

int cs_route_blit2(VkCommandBuffer prim, VkImage src, VkImageLayout sl,
                   VkImage dst, VkImageLayout dl, uint32_t n,
                   const VkImageBlit2 *rgs, VkFilter filter)
{
    struct owner *o;
    VkCommandBuffer sec;
    VkCommandBufferBeginInfo bi;
    int handled = 0;
    if (n == 0 || !route_ok(prim))
        return 0;
    pthread_mutex_lock(&cs_lck);
    o = cs_owner_of(&CS, prim);
    cs_log("route attempt prim=%p owner=%p", (void*)prim, (void*)o);
    if (o) {
        sec = cs_sec_get(&CS);
        if (sec && o->cnt < SECPERPRIMMAX) {
            if (o->cap == o->cnt) {
                int nc = o->cap * 2;
                VkCommandBuffer *ns =
                    realloc(o->secs, (size_t)nc * sizeof(VkCommandBuffer));
                if (ns) {
                    o->secs = ns;
                    o->cap = nc;
                }
            }
            if (o->cap != o->cnt) {
                memset(&bi, 0, sizeof(bi));
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                o->secs[o->cnt++] = sec;
                CS.BeginCommandBuffer(sec, &bi);
                VkBlitImageInfo2 info;
                memset(&info, 0, sizeof(info));
                info.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
                info.srcImage = src;
                info.srcImageLayout = sl;
                info.dstImage = dst;
                info.dstImageLayout = dl;
                info.filter = filter;
                for (uint32_t i2 = 0; i2 < n; i2++) {
                    info.regionCount = 1;
                    info.pRegions = &rgs[i2];
                    CS.CmdBlitImage2(sec, &info);
                }
                CS.EndCommandBuffer(sec);
                CS.CmdExecuteCommands(prim, 1, &sec);
                CS.nroutes++;
                handled = 1;
            }
        }
    }
    pthread_mutex_unlock(&cs_lck);
    return handled;
}
