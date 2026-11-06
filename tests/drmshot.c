/*
 * Grab the screen from the DRM/KMS scanout buffer.
 *
 * The game runs in exclusive fullscreen, so its pixels are in a KMS plane and
 * not in the X server's frame buffer - an X11 screenshot cannot see it.
 *
 * usage: drmshot <out.ppm> [card]
 */
#define _GNU_SOURCE
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

void *gbm_map(int handle, uint32_t *w, uint32_t *h, uint32_t *stride);

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "/tmp/drmshot.ppm";
    int card = argc > 2 ? atoi(argv[2]) : 0;
    char path[64];
    snprintf(path, sizeof(path), "/dev/dri/card%d", card);
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open dri"); return 1; }

    drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
    if (!pres) { fprintf(stderr, "no plane resources\n"); return 1; }
    printf("planes=%u\n", pres->count_planes);

    uint32_t plane_id = 0, fb_id = 0;
    for (uint32_t i = 0; i < pres->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pres->planes[i]);
        if (!p) continue;
        if (p->fb_id) {
            printf("plane %u: crtc=%u fb=%u formats=%u\n", p->plane_id, p->crtc_id, p->fb_id,
                   p->count_formats);
            if (!fb_id || p->crtc_id) {
                plane_id = p->plane_id;
                fb_id = p->fb_id;
            }
        }
        drmModeFreePlane(p);
    }
    if (!fb_id) { fprintf(stderr, "no plane with a frame buffer\n"); return 1; }

    drmModeFB2 *fb = drmModeGetFB2(fd, fb_id);
    if (!fb) { fprintf(stderr, "drmModeGetFB2 failed\n"); return 1; }
    printf("fb %u %ux%u format=0x%x handles=%u/%u pitch=%u\n", fb->fb_id, fb->width, fb->height,
           fb->pixel_format, fb->handles[0], fb->handles[1], fb->pitches[0]);

    uint32_t width = fb->width, height = fb->height, stride = fb->pitches[0];
    void *ptr = NULL;
    if (fb->handles[1]) {
        ptr = gbm_map((int)fb->handles[1], &width, &height, &stride);
    }
    if (!ptr && fb->handles[0]) {
        uint32_t id = 0;
        if (drmModeAddFB2(fd, fb->width, fb->height, fb->pixel_format, fb->handles, fb->pitches,
                          fb->offsets, &id, 0) == 0 &&
            drmModeMapFramebuffer(fd, id, &stride, &height, &ptr, 0) != 0)
            ptr = NULL;
    }
    if (!ptr) { fprintf(stderr, "cannot map the frame buffer\n"); return 1; }
    printf("stride=%u size=%ux%u\n", stride, width, height);

    FILE *f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (uint32_t y = 0; y < height; y++) {
        unsigned char *row = (unsigned char *)ptr + (size_t)y * stride;
        for (uint32_t x = 0; x < width; x++) {
            fputc(row[x * 4 + 2], f);
            fputc(row[x * 4 + 1], f);
            fputc(row[x * 4 + 0], f);
        }
    }
    fclose(f);
    printf("wrote %s (%ux%u)\n", out, width, height);
    return 0;
}
