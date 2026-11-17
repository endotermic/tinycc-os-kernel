#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * build-disk - put the self-hosting work directory on a disk instead of in RAM, a C script
 * (/usr/bin/tcc runs it).  build-kernel and build-toolchain call it before they fall back to
 * a tmpfs: the kernel with the GPU drivers needs 1.3 GB (i386) to 1.7 GB (x86_64) for its
 * source and objects, which a tmpfs would hold in RAM on top of what the compilers and the
 * linker use.
 *
 * Usage: build-disk DIR      (exit 0: DIR is mounted on a build disk; 1: there is none)
 *
 * Only a disk marked for it is used, never any other: run.sh --selfhost attaches a fresh
 * sparse image whose first bytes are "TCC-BUILD-DISK" (scripts/selfhost-test.sh too); that
 * disk is formatted ext2 with the label "tcc-build" (busybox mke2fs) and mounted on DIR.  A
 * disk that already has that label (a second command in the same boot, or a disk prepared
 * by hand with mke2fs -L tcc-build) is mounted as it is.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAGIC "TCC-BUILD-DISK"
#define LABEL "tcc-build"

static int run(char *const argv[])
{
    pid_t pid;
    int st;
    fflush(NULL);
    if ((pid = fork()) < 0)
        return 127;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return 127;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* the ext2 volume label of dev, if it has an ext2/3/4 superblock ("" otherwise) */
static const char *ext_label(const char *dev)
{
    static char label[17];
    unsigned char sb[136];
    int fd = open(dev, O_RDONLY);
    label[0] = 0;
    if (fd < 0)
        return label;
    if (pread(fd, sb, sizeof sb, 1024) == (ssize_t)sizeof sb && sb[56] == 0x53 && sb[57] == 0xef) {
        memcpy(label, sb + 120, 16);        /* s_volume_name */
        label[16] = 0;
    }
    close(fd);
    return label;
}

static int has_magic(const char *dev)
{
    char buf[sizeof MAGIC - 1];
    int fd = open(dev, O_RDONLY), r;
    if (fd < 0)
        return 0;
    r = read(fd, buf, sizeof buf) == (ssize_t)sizeof buf && memcmp(buf, MAGIC, sizeof buf) == 0;
    close(fd);
    return r;
}

int main(int argc, char **argv)
{
    DIR *d;
    struct dirent *e;
    char dev[300], found[300] = "";
    int fresh = 0;
    if (argc != 2) {
        fprintf(stderr, "usage: build-disk DIR\n");
        return 2;
    }
    /* whole disks only: SATA/IDE/USB (sd*), virtio (vd*), NVMe (nvme*n*) */
    if (!(d = opendir("/sys/block")))
        return 1;
    while ((e = readdir(d)) && !found[0]) {
        if (strncmp(e->d_name, "sd", 2) && strncmp(e->d_name, "vd", 2) && strncmp(e->d_name, "nvme", 4))
            continue;
        snprintf(dev, sizeof dev, "/dev/%s", e->d_name);
        if (strcmp(ext_label(dev), LABEL) == 0)
            snprintf(found, sizeof found, "%s", dev);
        else if (has_magic(dev))
            snprintf(found, sizeof found, "%s", dev), fresh = 1;
    }
    closedir(d);
    if (!found[0])
        return 1;
    if (fresh) {
        printf("== build disk %s: formatting it (ext2, label %s)\n", found, LABEL);
        if (run((char *[]){ "mke2fs", "-q", "-F", "-L", LABEL, "-m", "0", found, NULL }) != 0) {
            fprintf(stderr, "build-disk: mke2fs %s failed\n", found);
            return 1;
        }
    }
    mkdir(argv[1], 0755);
    /* the ext4 driver mounts ext2 (CONFIG_EXT4_USE_FOR_EXT2) */
    if (mount(found, argv[1], "ext4", MS_NOATIME, NULL) != 0 && mount(found, argv[1], "ext2", MS_NOATIME, NULL) != 0) {
        fprintf(stderr, "build-disk: mount %s on %s: %s\n", found, argv[1], strerror(errno));
        return 1;
    }
    printf("== build disk %s mounted on %s\n", found, argv[1]);
    fflush(stdout);
    return 0;
}
