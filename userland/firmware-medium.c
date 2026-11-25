#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * /usr/sbin/firmware-medium of the busybox image: the firmware files from the boot medium, a
 * C script that /init runs before it brings the network up.
 *
 * The ISO (boot/mkiso.sh) keeps the firmware of the drivers in its /firmware directory,
 * every file xz-compressed (the kernel's CONFIG_FW_LOADER_COMPRESS_XZ unpacks it), instead of
 * a second initrd: GRUB reads initrds through the BIOS or UEFI disk calls, and the 123 MB of
 * the firmware image took 7.4 s of the 12.6 s of black screen in QEMU (SeaBIOS; minutes on
 * OVMF).  GRUB puts
 * "tcc.firmware=ID" on the kernel command line, and /firmware/.id on the medium holds the
 * same ID (the checksum of out/<arch>/firmware.cpio.gz the tree was made from).
 *
 * 1. Find the medium: every CD (sr*) and SCSI/USB disk (sd*) is tried as iso9660, read-only,
 *    on /run/boot-medium, until one has the ID; USB sticks appear a few seconds into the
 *    boot, so for up to 20 s.
 * 2. Hand the directory to the firmware loader (/sys/module/firmware_class/parameters/path:
 *    searched before /lib/firmware, which holds what the initramfs carries itself: i915's
 *    files and regulatory.db, see scripts/build.sh firmware).
 * 3. Probe again the devices that asked for firmware before the medium was there.  The
 *    drivers are built in and probe before /init runs, so a driver whose probe needs the
 *    files (iwlwifi, amdgpu, xe, mt76, rtw88/89) failed and left its device unbound, and
 *    one that loads them after the probe (Bluetooth, SOF audio, ath10k/11k/12k) stays bound
 *    without them.  So: every unbound PCI device and USB interface is probed again (no
 *    driver for it: nothing happens), and every device of the kernel log that complained
 *    about firmware (a warning or worse that names a device: /dev/kmsg's DEVICE= line; the
 *    firmware loader's "Direct firmware load for X failed") is unbound from its driver and
 *    probed again.  Not the ones whose unbinding takes something else away: storage and USB
 *    controllers, bridges, hubs, USB disks (the medium itself may hang off them), and the
 *    display drivers that already drive the screen (i915 gets its files from the
 *    initramfs; nouveau and radeon run without acceleration then, which nothing here uses).
 *
 * Usage: firmware-medium [ID]   (without an ID: the tcc.firmware= of /proc/cmdline, and
 *        nothing to do when there is none: a direct boot, run.sh without --iso)
 * Run again later (as root) it finds the medium mounted and only probes again.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>

#define MNT "/run/boot-medium"
#define FWDIR MNT "/firmware"
#define FWPATH_PARAM "/sys/module/firmware_class/parameters/path"
#define WAIT_SECONDS 20

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* the first line of a file, without the newline, in buf ("" if unreadable) */
static char *first_line(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    buf[0] = 0;
    if (f) {
        if (!fgets(buf, len, f))
            buf[0] = 0;
        fclose(f);
    }
    buf[strcspn(buf, "\n")] = 0;
    return buf;
}

static int write_str(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY), rc = -1;
    if (fd >= 0) {
        rc = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -1;
        close(fd);
    }
    return rc;
}

/* the ID of tcc.firmware= on the kernel command line, in buf ("" if there is none) */
static char *cmdline_id(char *buf, size_t len)
{
    char line[4096], *p, *e;
    first_line("/proc/cmdline", line, sizeof line);
    buf[0] = 0;
    for (p = line; (p = strstr(p, "tcc.firmware=")); p++)
        if (p == line || p[-1] == ' ') {
            p += strlen("tcc.firmware=");
            e = p + strcspn(p, " ");
            snprintf(buf, len, "%.*s", (int)(e - p), p);
            break;
        }
    return buf;
}

/* is the ISO with this ID mounted on MNT? */
static int medium_mounted(const char *id)
{
    char got[128];
    return strcmp(first_line(FWDIR "/.id", got, sizeof got), id) == 0;
}

/* try every CD and SCSI/USB disk as the medium; 1 when it is mounted on MNT, its device
 * in dev */
static int find_medium(const char *id, char *dev, size_t devlen)
{
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    char path[300], size[32];
    int found = 0;
    if (!d)
        return 0;
    while (!found && (e = readdir(d))) {
        if (strncmp(e->d_name, "sr", 2) != 0 && strncmp(e->d_name, "sd", 2) != 0)
            continue;
        snprintf(path, sizeof path, "/sys/block/%s/size", e->d_name);
        if (atol(first_line(path, size, sizeof size)) == 0)
            continue;   /* an empty CD drive, a card reader without a card */
        snprintf(dev, devlen, "/dev/%s", e->d_name);
        if (mount(dev, MNT, "iso9660", MS_RDONLY, NULL) != 0)
            continue;
        if (medium_mounted(id))
            found = 1;
        else
            umount(MNT);
    }
    closedir(d);
    return found;
}

/* ---- the devices to probe again ---- */

#define MAXDEV 64
static struct cand { char path[PATH_MAX]; char why[160]; } cands[MAXDEV];
static int ncands;

/* the bus a sysfs device sits on ("" if none: a class device such as hci0) */
static const char *bus_of(const char *devpath)
{
    static char link[PATH_MAX];
    char p[PATH_MAX + 16], *s;
    ssize_t n;
    snprintf(p, sizeof p, "%s/subsystem", devpath);
    n = readlink(p, link, sizeof link - 1);
    if (n <= 0)
        return "";
    link[n] = 0;
    if (!strstr(link, "/bus/"))
        return "";
    s = strrchr(link, '/');
    return s ? s + 1 : link;
}

static const char *driver_of(const char *devpath)
{
    static char link[PATH_MAX];
    char p[PATH_MAX + 16], *s;
    ssize_t n;
    snprintf(p, sizeof p, "%s/driver", devpath);
    n = readlink(p, link, sizeof link - 1);
    if (n <= 0)
        return "";
    link[n] = 0;
    s = strrchr(link, '/');
    return s ? s + 1 : link;
}

static long hex_attr(const char *devpath, const char *attr)
{
    char p[PATH_MAX + 32], v[32];
    snprintf(p, sizeof p, "%s/%s", devpath, attr);
    return strtol(first_line(p, v, sizeof v), NULL, 16);
}

/* May this device be unbound and probed again?  A PCI device of a class whose unbinding
 * takes nothing else away (network, multimedia, wireless, communication; display only when
 * nothing drives it), a USB interface that is no hub and no disk, an I2C, SPI or SDIO
 * device (the amplifiers of laptop sound, SDIO Wi-Fi). */
static int rebindable(const char *devpath, const char *bus, int bound)
{
    const char *name = strrchr(devpath, '/') + 1;
    if (strcmp(bus, "pci") == 0) {
        long cls = hex_attr(devpath, "class") >> 16;
        return cls == 0x02 || cls == 0x04 || cls == 0x07 || cls == 0x0d || (cls == 0x03 && !bound);
    }
    if (strcmp(bus, "usb") == 0) {
        long cls;
        if (!strchr(name, ':'))
            return 0;   /* a USB device, not an interface: the drivers bind the interfaces */
        cls = hex_attr(devpath, "bInterfaceClass");
        return cls != 0x08 && cls != 0x09;
    }
    return strcmp(bus, "i2c") == 0 || strcmp(bus, "spi") == 0 || strcmp(bus, "sdio") == 0;
}

static void add_cand(const char *devpath, const char *why)
{
    int i;
    for (i = 0; i < ncands; i++)
        if (strcmp(cands[i].path, devpath) == 0)
            return;
    if (ncands < MAXDEV) {
        snprintf(cands[ncands].path, sizeof cands[ncands].path, "%s", devpath);
        snprintf(cands[ncands].why, sizeof cands[ncands].why, "%s", why);
        ncands++;
    }
}

/* the bus device behind a kmsg DEVICE= value ("+pci:0000:00:14.3", "+bluetooth:hci0"):
 * that device, or its nearest ancestor on a bus; 0 if none */
static int bus_device(const char *devval, char *out)
{
    char sub[64], p[PATH_MAX];
    const char *colon;
    const char *name;
    if (devval[0] != '+' || !(colon = strchr(devval + 1, ':')) || colon - devval - 1 >= (int)sizeof sub)
        return 0;
    snprintf(sub, sizeof sub, "%.*s", (int)(colon - devval - 1), devval + 1);
    name = colon + 1;
    snprintf(p, sizeof p, "/sys/bus/%s/devices/%s", sub, name);
    if (!exists(p))
        snprintf(p, sizeof p, "/sys/class/%s/%s", sub, name);
    if (!realpath(p, out))
        return 0;
    while (strncmp(out, "/sys/devices/", 13) == 0) {
        if (*bus_of(out))
            return 1;
        *strrchr(out, '/') = 0;
    }
    return 0;
}

/* the kernel log so far: every warning (or worse) about firmware that names a device */
static void scan_kmsg(void)
{
    char rec[8192], *msg, *dev, *nl;
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    ssize_t n;
    if (fd < 0)
        return;
    for (;;) {
        n = read(fd, rec, sizeof rec - 1);
        if (n < 0) {
            if (errno == EPIPE)
                continue;   /* records overwritten meanwhile: go on with the next */
            break;          /* EAGAIN: the end */
        }
        rec[n] = 0;
        /* "prio,seq,usec,flags;text\n KEY=VALUE\n..." with prio = facility * 8 + level */
        if (!(msg = strchr(rec, ';')) || (atoi(rec) & 7) > 4)
            continue;
        msg++;
        if (!(dev = strstr(msg, "\n DEVICE=")))
            continue;
        *dev = 0;
        dev += strlen("\n DEVICE=");
        if ((nl = strchr(dev, '\n')))
            *nl = 0;
        if (!strcasestr(msg, "firmware") || strstr(msg, "[Firmware"))
            continue;   /* "[Firmware Bug]: ..." is about the BIOS */
        {
            char path[PATH_MAX];
            if (bus_device(dev, path))
                add_cand(path, msg);
        }
    }
    close(fd);
}

/* every unbound PCI device and USB interface */
static void scan_unbound(const char *bus)
{
    char dir[64], p[PATH_MAX], real[PATH_MAX];
    DIR *d;
    struct dirent *e;
    snprintf(dir, sizeof dir, "/sys/bus/%s/devices", bus);
    if (!(d = opendir(dir)))
        return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!realpath(p, real) || *driver_of(real))
            continue;
        add_cand(real, "");
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    char id[128], dev[300] = "", path[PATH_MAX + 32], probed[1024] = "", kept[1024] = "";
    time_t start = time(NULL);
    int i, said_wait = 0;
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("usage: firmware-medium [ID]: mount the boot medium whose /firmware/.id is ID (default: tcc.firmware= of the kernel command line), use its firmware, probe again the devices that missed it\n");
        return 0;
    }
    if (argc > 1)
        snprintf(id, sizeof id, "%s", argv[1]);
    else
        cmdline_id(id, sizeof id);
    if (!id[0])
        return 0;
    mkdir("/run", 0755);
    mkdir(MNT, 0755);
    if (!medium_mounted(id)) {
        while (!find_medium(id, dev, sizeof dev)) {
            if (time(NULL) - start >= WAIT_SECONDS) {
                printf("*** firmware: no boot medium with /firmware/.id %s after %d s (CD or USB stick: tcc.firmware= on the command line): the drivers run without their firmware files\n", id, WAIT_SECONDS);
                return 1;
            }
            if (!said_wait && time(NULL) - start >= 2) {
                printf("*** firmware: waiting for the boot medium (up to %d s)\n", WAIT_SECONDS);
                fflush(stdout);
                said_wait = 1;
            }
            usleep(250000);
        }
    }
    if (write_str(FWPATH_PARAM, FWDIR) != 0) {
        printf("*** firmware: cannot set %s: %s\n", FWPATH_PARAM, strerror(errno));
        return 1;
    }
    scan_kmsg();
    scan_unbound("pci");
    scan_unbound("usb");
    for (i = 0; i < ncands; i++) {
        char *p = cands[i].path, *name = strrchr(p, '/') + 1, drv[128], bus[64], item[300];
        int bound;
        snprintf(bus, sizeof bus, "%s", bus_of(p));
        snprintf(drv, sizeof drv, "%s", driver_of(p));
        bound = drv[0] != 0;
        if (!rebindable(p, bus, bound)) {
            /* a complaint from a device we leave alone: say so (an unbound device without
             * a driver for it is no news) */
            if (cands[i].why[0] && strlen(kept) + strlen(name) + strlen(drv) + 8 < sizeof kept)
                snprintf(kept + strlen(kept), sizeof kept - strlen(kept), "%s%s %s", kept[0] ? ", " : "", drv[0] ? drv : bus, name);
            continue;
        }
        if (bound) {
            snprintf(path, sizeof path, "%s/driver/unbind", p);
            write_str(path, name);
        }
        snprintf(path, sizeof path, "/sys/bus/%s/drivers_probe", bus);
        write_str(path, name);   /* fails when no driver takes it: nothing to report */
        if (*driver_of(p)) {
            snprintf(item, sizeof item, "%s%s %s", probed[0] ? ", " : "", driver_of(p), name);
            if (strlen(probed) + strlen(item) < sizeof probed)
                strcat(probed, item);
        }
    }
    printf("*** firmware: from %s%s (%s)%s%s%s%s\n", dev[0] ? dev : "the boot medium", dev[0] ? "" : ", mounted already", FWDIR,
           probed[0] ? "; probed again: " : "", probed,
           kept[0] ? "; left without it: " : "", kept);
    return 0;
}
