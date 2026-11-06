/* Query the PowerVR DRM device info directly. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/pvr_drm.h>

int main(void)
{
    const char *nodes[] = {"/dev/dri/renderD128", "/dev/dri/card1", "/dev/dri/card0"};
    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        int fd = open(nodes[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            printf("%s: open failed (%s)\n", nodes[i], strerror(errno));
            continue;
        }
        printf("%s: opened\n", nodes[i]);

        struct drm_pvr_ioctl_dev_query_args args;
        memset(&args, 0, sizeof(args));
        args.type = DRM_PVR_DEV_QUERY_GPU_INFO_GET;
        args.size = sizeof(struct drm_pvr_dev_query_gpu_info);
        struct drm_pvr_dev_query_gpu_info info;
        memset(&info, 0, sizeof(info));
        args.pointer = (uintptr_t)&info;

        int r = ioctl(fd, DRM_IOCTL_PVR_DEV_QUERY, &args);
        if (r != 0) {
            printf("  DEV_QUERY failed: %s\n", strerror(errno));
        } else {
            printf("  gpu_id = 0x%016llx -> B=%u V=%u N=%u C=%u, phantoms=%u\n",
                   (unsigned long long)info.gpu_id, (unsigned)(info.gpu_id >> 48),
                   (unsigned)(info.gpu_id >> 32), (unsigned)(info.gpu_id >> 16),
                   (unsigned)info.gpu_id, info.num_phantoms);
        }

        /* also try the runtime info query */
        struct drm_pvr_ioctl_dev_query_args args2;
        memset(&args2, 0, sizeof(args2));
        args2.type = DRM_PVR_DEV_QUERY_RUNTIME_INFO_GET;
        args2.size = 256;
        unsigned char buf[256];
        memset(buf, 0, sizeof(buf));
        args2.pointer = (uintptr_t)buf;
        r = ioctl(fd, DRM_IOCTL_PVR_DEV_QUERY, &args2);
        printf("  RUNTIME_INFO -> %d (%s)\n", r, r ? strerror(errno) : "ok");
        close(fd);
    }
    return 0;
}
