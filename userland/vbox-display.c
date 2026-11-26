#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * /usr/sbin/vbox-display of the busybox image: the guest's screen follows the size of the
 * VirtualBox window, a C script that /init starts in the background when /dev/vboxguest
 * exists (CONFIG_VBOXGUEST, a VirtualBox machine).
 *
 * VirtualBox sends the guest the size of its window (a "display change request") only when
 * the guest says it can change its screen size (VMMDEV_GUEST_SUPPORTS_GRAPHICS), which the
 * Guest Additions' VBoxClient does.  This program does that, on its /dev/vboxguest session,
 * which lives as long as it runs, then waits for the requests:
 *   - VBoxVGA and VBoxSVGA (vboxvideo): the driver receives the size itself and makes it the
 *     preferred mode with a hotplug event; nothing else to do here.
 *   - VMSVGA (vmwgfx, VirtualBox's default for Linux): the driver learns the size only from
 *     user space, DRM_IOCTL_VMW_UPDATE_LAYOUT on its render node, which then makes it the
 *     preferred mode and sends the hotplug event.
 * TinyX's Xfbdev in KMS mode (patches/tinyx/features/0010) takes the new preferred mode on
 * the hotplug event and resizes the X screen; the text console keeps its boot size (the
 * kernel's fbdev emulation cannot grow).
 *
 * The structures are written out here (the image has no kernel headers): include/uapi/
 * linux/vboxguest.h, drivers/virt/vboxguest/vmmdev.h, include/uapi/drm/vmwgfx_drm.h.
 * -v prints each request.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* vboxguest */
struct vbg_ioctl_hdr {
    uint32_t size_in, version, type;
    int32_t rc;
    uint32_t size_out, reserved;
};
#define VBG_IOCTL_HDR_VERSION 0x10001
struct vbg_u32x2 { struct vbg_ioctl_hdr hdr; uint32_t a, b; };    /* filter, caps, wait */
#define VBG_IOCTL_WAIT_FOR_EVENTS            _IOWR('V', 10, struct vbg_u32x2)
#define VBG_IOCTL_CHANGE_FILTER_MASK         _IOWR('V', 12, struct vbg_u32x2)
#define VBG_IOCTL_CHANGE_GUEST_CAPABILITIES  _IOWR('V', 14, struct vbg_u32x2)
#define VBG_IOCTL_VMMDEV_REQUEST(s)          _IOC(_IOC_READ | _IOC_WRITE, 'V', 2, (s))
#define VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST  (1u << 2)
#define VMMDEV_GUEST_SUPPORTS_GRAPHICS       (1u << 2)
#define VMMDEVREQ_GET_DISPLAY_CHANGE_REQ2    54
struct display_change_req2 {
    struct vbg_ioctl_hdr hdr;   /* the same layout as vmmdev_request_header */
    uint32_t xres, yres, bpp, eventack, display;
};

/* DRM */
struct drm_version {
    int major, minor, patchlevel;
    size_t name_len; char *name;
    size_t date_len; char *date;
    size_t desc_len; char *desc;
};
#define DRM_IOCTL_VERSION _IOWR('d', 0x00, struct drm_version)
struct drm_vmw_rect { int32_t x, y; uint32_t w, h; };
struct drm_vmw_update_layout_arg { uint32_t num_outputs, pad64; uint64_t rects; };
#define DRM_IOCTL_VMW_UPDATE_LAYOUT _IOW('d', 0x40 + 20, struct drm_vmw_update_layout_arg)

static int verbose;

/* out: the size of the request's output part; vboxguest wants size_in and size_out exact
   (vbg_ioctl_chk: the header plus the input or output part) */
static int vbg(int fd, unsigned long req, struct vbg_u32x2 *r, uint32_t a, uint32_t b, uint32_t out)
{
    memset(r, 0, sizeof *r);
    r->hdr.size_in = sizeof *r;
    r->hdr.size_out = sizeof r->hdr + out;
    r->hdr.version = VBG_IOCTL_HDR_VERSION;
    r->a = a;
    r->b = b;
    return ioctl(fd, req, r) < 0 ? -1 : r->hdr.rc;
}

/* the requested size of display 0; ack: take the pending request (clears the event) */
static int display_request(int fd, int ack, uint32_t *w, uint32_t *h)
{
    struct display_change_req2 q;

    memset(&q, 0, sizeof q);
    q.hdr.size_in = q.hdr.size_out = sizeof q;
    q.hdr.version = VBG_IOCTL_HDR_VERSION;
    q.hdr.type = VMMDEVREQ_GET_DISPLAY_CHANGE_REQ2;
    q.eventack = ack ? VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST : 0;
    if (ioctl(fd, VBG_IOCTL_VMMDEV_REQUEST(sizeof q), &q) < 0 || q.hdr.rc < 0)
        return -1;
    *w = q.xres;
    *h = q.yres;
    return q.display;
}

/* the render node of vmwgfx, if that is the display's driver */
static int open_vmwgfx(void)
{
    char path[32], name[32];
    struct drm_version v;
    int i, fd;

    for (i = 128; i < 136; i++) {
        snprintf(path, sizeof path, "/dev/dri/renderD%d", i);
        if ((fd = open(path, O_RDWR | O_CLOEXEC)) < 0)
            continue;
        memset(&v, 0, sizeof v);
        memset(name, 0, sizeof name);
        v.name = name;
        v.name_len = sizeof name - 1;
        if (ioctl(fd, DRM_IOCTL_VERSION, &v) == 0 && !strcmp(name, "vmwgfx"))
            return fd;
        close(fd);
    }
    return -1;
}

static void apply(int drm, uint32_t w, uint32_t h)
{
    struct drm_vmw_rect r = { 0, 0, w, h };
    struct drm_vmw_update_layout_arg a = { 1, 0, (uintptr_t)&r };

    if (verbose)
        printf("vbox-display: %ux%u\n", w, h);
    if (drm >= 0 && w && h && ioctl(drm, DRM_IOCTL_VMW_UPDATE_LAYOUT, &a) < 0)
        fprintf(stderr, "vbox-display: DRM_IOCTL_VMW_UPDATE_LAYOUT %ux%u: %s\n", w, h, strerror(errno));
}

int main(int argc, char **argv)
{
    struct vbg_u32x2 r;
    uint32_t w, h;
    int fd, drm;

    verbose = argc > 1 && !strcmp(argv[1], "-v");
    if ((fd = open("/dev/vboxguest", O_RDWR | O_CLOEXEC)) < 0) {
        perror("vbox-display: /dev/vboxguest");
        return 1;
    }
    drm = open_vmwgfx();
    if (vbg(fd, VBG_IOCTL_CHANGE_FILTER_MASK, &r, VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST, 0, 0) < 0 ||
        vbg(fd, VBG_IOCTL_CHANGE_GUEST_CAPABILITIES, &r, VMMDEV_GUEST_SUPPORTS_GRAPHICS, 0, 8) < 0) {
        fprintf(stderr, "vbox-display: vboxguest refused the event filter or the capability\n");
        return 1;
    }
    printf("vbox-display: the screen follows the VirtualBox window (%s)\n",
           drm >= 0 ? "VMSVGA: vmwgfx" : "vboxvideo");
    fflush(stdout);
    /* a size asked for before this program ran */
    if (display_request(fd, 0, &w, &h) >= 0 && w && h)
        apply(drm, w, h);
    for (;;) {
        if (vbg(fd, VBG_IOCTL_WAIT_FOR_EVENTS, &r, 0xffffffffu, VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST, 4) < 0) {
            if (errno == EINTR)
                continue;
            sleep(1);       /* VERR_INTERRUPTED and the like: wait again */
            continue;
        }
        if ((r.a & VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST) && display_request(fd, 1, &w, &h) >= 0)
            apply(drm, w, h);
    }
}
