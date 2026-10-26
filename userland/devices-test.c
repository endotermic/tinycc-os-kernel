#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * devices-test: exercise the device drivers of kernels/linux/drivers.config inside the guest
 * and print one "devtest: <device>: ok|FAIL <details>" line per device class, then a summary.
 * A C script run by tcc -run (every script of this system is one).  /init runs it when the
 * kernel command line has tcc.devtest (run.sh APPEND=..., scripts/devices-test.sh drives the
 * whole thing: it serves HTTP on the host and moves the mouse through QMP); it can be run by
 * hand from the shell as well, then the mouse check waits for the mouse to be moved.
 *
 * Usage: devices-test [http:PORT]   (PORT: a web server on the host, reached as 10.0.2.2:PORT
 *        through QEMU's user-mode network; the default comes from tcc.devtest=http:PORT)
 * Exit status: the number of failed checks.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <time.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int failures, checks;

static void result(const char *what, int ok, const char *fmt, ...)
{
    va_list ap;
    checks++;
    if (!ok)
        failures++;
    printf("devtest: %s: %s ", what, ok ? "ok" : "FAIL");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* the first line of a file, without the newline ("" if unreadable); a static buffer */
static char *first_line(const char *path)
{
    static char buf[256];
    FILE *f = fopen(path, "r");
    buf[0] = 0;
    if (f) {
        if (fgets(buf, sizeof buf, f))
            buf[strcspn(buf, "\n")] = 0;
        fclose(f);
    }
    return buf;
}

/* the name of the driver bound to a sysfs device ("" if none) */
static char *driver_of(const char *devpath)
{
    static char link[256], path[300];
    ssize_t n;
    snprintf(path, sizeof path, "%s/driver", devpath);
    n = readlink(path, link, sizeof link - 1);
    if (n <= 0)
        return "";
    link[n] = 0;
    return strrchr(link, '/') ? strrchr(link, '/') + 1 : link;
}

static int run(char *const argv[], int quiet)
{
    pid_t pid;
    int st;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        return 127;
    if (pid == 0) {
        if (quiet) {
            int null = open("/dev/null", O_RDWR);
            dup2(null, 1);
            dup2(null, 2);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return 127;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* count the entries of a directory whose name starts with prefix (all if NULL) */
static int count_dir(const char *dir, const char *prefix, char *names, size_t nlen)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (names)
        names[0] = 0;
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || (prefix && strncmp(e->d_name, prefix, strlen(prefix)) != 0))
            continue;
        n++;
        if (names && strlen(names) + strlen(e->d_name) + 2 < nlen) {
            if (names[0])
                strcat(names, " ");
            strcat(names, e->d_name);
        }
    }
    closedir(d);
    return n;
}

static void test_pci(void)
{
    int n = count_dir("/sys/bus/pci/devices", NULL, NULL, 0);
    result("pci", n > 0, "%d devices on the bus", n);
}

static void test_ethernet(const char *http_port)
{
    struct ifreq ifr;
    int fd, ok = 0, rc;
    char addr[32] = "no address", *drv, path[128], cmd_url[64];
    drv = driver_of("/sys/class/net/eth0/device");
    if (!exists("/sys/class/net/eth0")) {
        result("ethernet", 0, "no eth0 (/sys/class/net: %s)", first_line("/dev/null"));
        return;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&ifr, 0, sizeof ifr);
    strcpy(ifr.ifr_name, "eth0");
    if (fd >= 0 && ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, addr, sizeof addr);
        ok = 1;
    }
    if (fd >= 0)
        close(fd);
    snprintf(path, sizeof path, "/sys/class/net/eth0/carrier");
    result("ethernet", ok, "eth0 %s (%s), carrier %s, DHCP from QEMU's user network", addr, *drv ? drv : "?",
           first_line(path)[0] == '1' ? "on" : "off");
    /* TCP/IP: ICMP to the gateway, then HTTP to the host through the gateway */
    rc = run((char *[]){ "ping", "-c", "1", "-W", "3", "10.0.2.2", NULL }, 1);
    result("ping", rc == 0, "ICMP echo to 10.0.2.2 (QEMU's gateway) %s", rc == 0 ? "answered" : "not answered");
    if (http_port) {
        char *got;
        snprintf(cmd_url, sizeof cmd_url, "http://10.0.2.2:%s/index.txt", http_port);
        unlink("/tmp/devtest.http");
        rc = run((char *[]){ "wget", "-q", "-T", "5", "-O", "/tmp/devtest.http", cmd_url, NULL }, 1);
        got = first_line("/tmp/devtest.http");
        result("http", rc == 0 && strcmp(got, "tinycc-os-kernel devices-test") == 0,
               "wget %s: %s", cmd_url, rc == 0 ? got : "failed");
    }
    /* IPv6: the link-local address the kernel assigned */
    {
        FILE *f = fopen("/proc/net/if_inet6", "r");
        char line[128];
        ok = 0;
        while (f && fgets(line, sizeof line, f))
            if (strstr(line, " eth0"))
                ok = 1;
        if (f)
            fclose(f);
        result("ipv6", ok, "%s link-local address on eth0", ok ? "a" : "no");
    }
}

static void test_wifi(void)
{
    char phys[128], wlans[128];
    int nphy = count_dir("/sys/class/ieee80211", NULL, phys, sizeof phys);
    int nwlan = count_dir("/sys/class/net", "wlan", wlans, sizeof wlans), rc = -1;
    if (nwlan > 0)
        rc = run((char *[]){ "ifconfig", "wlan0", "up", NULL }, 1);
    result("wifi", nphy > 0 && nwlan > 0 && rc == 0, "%d radios (%s: %s), interfaces %s, wlan0 up %s", nphy,
           phys, nwlan ? driver_of("/sys/class/net/wlan0/device") : "", wlans, rc == 0 ? "ok" : "failed");
}

static void test_bluetooth(void)
{
    /* the virtual controller: opening /dev/vhci registers hci0 (after a second, when no vendor
       packet is written); the HCI socket family must exist as well */
    int fd = open("/dev/vhci", O_RDWR), i, sk, have_hci = 0;
    char name[64] = "";
    for (i = 0; fd >= 0 && i < 40 && !(have_hci = exists("/sys/class/bluetooth/hci0")); i++)
        usleep(100000);
    sk = socket(31 /* AF_BLUETOOTH */, SOCK_RAW | SOCK_CLOEXEC, 1 /* BTPROTO_HCI */);
    if (have_hci)
        snprintf(name, sizeof name, ", bus %s", first_line("/sys/class/bluetooth/hci0/device/name")[0] ? "" : "virtual");
    result("bluetooth", fd >= 0 && have_hci && sk >= 0, "/dev/vhci %s, hci0 %s%s, HCI socket %s",
           fd >= 0 ? "opened" : "missing", have_hci ? "registered" : "missing", name, sk >= 0 ? "ok" : "failed");
    if (sk >= 0)
        close(sk);
    if (fd >= 0)
        close(fd);
}

/* OSS ioctls (sound/oss/soundcard.h): _SIOWR('P', n, int) = 0xC004500n, _SIO('P', 1) = 0x5001 */
#define SNDCTL_DSP_SYNC     0x5001
#define SNDCTL_DSP_SPEED    0xC0045002
#define SNDCTL_DSP_SETFMT   0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define AFMT_S16_LE 0x10

static void test_sound(void)
{
    char *card = first_line("/proc/asound/cards"), cardname[80] = "no card";
    int fd, fmt = AFMT_S16_LE, ch = 2, rate = 44100, ok = 0, i, mixer;
    short buf[44100];   /* half a second of stereo 16-bit at 44.1 kHz */
    ssize_t n = 0;
    struct timespec t0, t1;
    double secs = 0;
    if (card[0]) {
        char *p = strchr(card, ':');
        snprintf(cardname, sizeof cardname, "card%s", card + 1);
        if (p) {
            while (*++p == ' ')
                ;
            snprintf(cardname, sizeof cardname, "%s", p);
        }
    }
    fd = open("/dev/dsp", O_WRONLY);
    if (fd >= 0 && ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) == 0 && ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) == 0
        && ioctl(fd, SNDCTL_DSP_SPEED, &rate) == 0) {
        for (i = 0; i < 22050; i++)   /* a 440 Hz tone, both channels */
            buf[2 * i] = buf[2 * i + 1] = (short)(8000 * sin(2 * 3.14159265 * 440 * i / rate));
        clock_gettime(CLOCK_MONOTONIC, &t0);
        n = write(fd, buf, sizeof buf);
        ioctl(fd, SNDCTL_DSP_SYNC, 0);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        ok = n == (ssize_t)sizeof buf;
    }
    mixer = open("/dev/mixer", O_RDONLY);
    result("sound", card[0] && ok, "%s; /dev/dsp %s: 0.5 s of a 440 Hz tone at %d Hz stereo 16-bit written (%zd bytes) and played in %.1f s; /dev/mixer %s",
           cardname, fd < 0 ? "missing" : ok ? "ok" : "ioctl/write failed", rate, n, secs, mixer >= 0 ? "ok" : "missing");
    if (mixer >= 0)
        close(mixer);
    if (fd >= 0)
        close(fd);
}

/* every input device by name, "inputN name; ..." */
static void input_names(char *out, size_t len)
{
    DIR *d = opendir("/sys/class/input");
    struct dirent *e;
    char path[300];
    out[0] = 0;
    if (!d)
        return;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "input", 5) != 0)
            continue;
        snprintf(path, sizeof path, "/sys/class/input/%s/name", e->d_name);
        if (strlen(out) + strlen(first_line(path)) + 3 < len) {
            if (out[0])
                strcat(out, "; ");
            strcat(out, first_line(path));
        }
    }
    closedir(d);
}

static void test_mouse(int wait_secs)
{
    char names[512], mice[128];
    int fd, nmice = count_dir("/sys/class/input", "mouse", mice, sizeof mice), bytes = 0, packets = 0;
    struct pollfd pfd;
    struct timespec t0, t;
    input_names(names, sizeof names);
    fd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        unsigned char b[64];
        ssize_t n;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        printf("devtest: mouse: waiting up to %d s for motion on /dev/input/mice (move the mouse)...\n", wait_secs);
        fflush(stdout);
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &t);
            if (t.tv_sec - t0.tv_sec >= wait_secs || packets >= 8)
                break;
            pfd.fd = fd;
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 500) <= 0)
                continue;
            while ((n = read(fd, b, sizeof b)) > 0) {
                bytes += n;
                packets += n / 3;   /* PS/2 protocol: 3 bytes per movement */
            }
        }
        close(fd);
    }
    result("mouse", nmice > 0 && fd >= 0 && packets > 0, "%d mouse device(s) (%s), /dev/input/mice %s, %d motion packets (%d bytes) read; input devices: %s",
           nmice, mice, fd >= 0 ? "opened" : "missing", packets, bytes, names);
    result("evdev", count_dir("/dev/input", "event", NULL, 0) > 0, "%d /dev/input/event* devices", count_dir("/dev/input", "event", NULL, 0));
}

static void test_usb(void)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    char path[300], list[512] = "", hcs[128];
    int ndev = 0, nhc = count_dir("/sys/bus/usb/devices", "usb", hcs, sizeof hcs);
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':') || strncmp(e->d_name, "usb", 3) == 0)
            continue;   /* interfaces (1-1:1.0) and root hubs (usb1) are not devices to list */
        snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/product", e->d_name);
        ndev++;
        if (strlen(list) + strlen(first_line(path)) + 3 < sizeof list) {
            if (list[0])
                strcat(list, "; ");
            strcat(list, first_line(path));
        }
    }
    if (d)
        closedir(d);
    result("usb", nhc > 0 && ndev > 0, "%d host controller(s) (%s), %d device(s): %s", nhc, hcs, ndev, list);
}

static void test_usb_storage(void)
{
    char disks[64], dev[32], *vendor, *model;
    int n = count_dir("/sys/block", "sd", disks, sizeof disks), rc = -1, ok = 0;
    char path[128];
    if (n == 0) {
        result("usb-storage", 0, "no SCSI disk (/sys/block/sd*)");
        return;
    }
    snprintf(dev, sizeof dev, "/dev/%.8s", disks);
    snprintf(path, sizeof path, "/sys/block/%.8s/device/vendor", disks);
    vendor = strdup(first_line(path));
    snprintf(path, sizeof path, "/sys/block/%.8s/device/model", disks);
    model = strdup(first_line(path));
    /* a FAT filesystem on the stick: made, mounted, written, remounted, read back */
    rc = run((char *[]){ "mkfs.vfat", dev, NULL }, 1);
    if (rc == 0 && mount(dev, "/mnt", "vfat", 0, NULL) == 0) {
        FILE *f = fopen("/mnt/devtest.txt", "w");
        if (f) {
            fprintf(f, "written by devices-test\n");
            fclose(f);
        }
        umount("/mnt");
        if (mount(dev, "/mnt", "vfat", MS_RDONLY, NULL) == 0) {
            ok = strcmp(first_line("/mnt/devtest.txt"), "written by devices-test") == 0;
            umount("/mnt");
        }
    }
    result("usb-storage", ok, "%s (%s %s): mkfs.vfat %s, a file written, remounted and read back %s", dev, vendor, model,
           rc == 0 ? "ok" : "failed", ok ? "ok" : "failed");
}

static void test_video(void)
{
    char size[32], *name, cards[64], conns[128] = "", modes[128] = "", path[300];
    DIR *d;
    struct dirent *e;
    int fb = exists("/sys/class/graphics/fb0");
    char *p;
    snprintf(size, sizeof size, "%s", first_line("/sys/class/graphics/fb0/virtual_size"));
    if ((p = strchr(size, ',')))
        *p = 'x';
    name = strdup(first_line("/sys/class/graphics/fb0/name"));
    count_dir("/sys/class/drm", "card", cards, sizeof cards);
    d = opendir("/sys/class/drm");
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "card", 4) != 0 || !strchr(e->d_name, '-'))
            continue;
        snprintf(path, sizeof path, "/sys/class/drm/%s/status", e->d_name);
        if (strcmp(first_line(path), "connected") != 0)
            continue;
        if (strlen(conns) + strlen(e->d_name) + 2 < sizeof conns) {
            if (conns[0])
                strcat(conns, " ");
            strcat(conns, e->d_name);
        }
        snprintf(path, sizeof path, "/sys/class/drm/%s/modes", e->d_name);
        if (!modes[0])
            snprintf(modes, sizeof modes, "%s", first_line(path));
    }
    if (d)
        closedir(d);
    result("video", fb && size[0] && strcmp(size, "0x0") != 0, "framebuffer %s (%s), drm %s, connected %s, preferred mode %s",
           fb ? size : "none", fb ? name : "-", cards[0] ? cards : "none", conns[0] ? conns : "none", modes[0] ? modes : "?");
}

int main(int argc, char **argv)
{
    char *http_port = NULL, *cmdline = strdup(first_line("/proc/cmdline")), *p;
    if (argc > 1 && strncmp(argv[1], "http:", 5) == 0)
        http_port = argv[1] + 5;
    else if ((p = strstr(cmdline, "tcc.devtest=http:")) && (p == cmdline || p[-1] == ' ')) {
        http_port = p + strlen("tcc.devtest=http:");
        if ((p = strchr(http_port, ' ')))
            *p = 0;
    }
    printf("devtest: start: %s\n", first_line("/proc/version"));
    test_pci();
    test_video();
    test_ethernet(http_port);
    test_wifi();
    test_bluetooth();
    test_sound();
    test_usb();
    test_usb_storage();
    test_mouse(8);
    printf("devtest: done: %d checks, %d failed\n", checks, failures);
    fflush(stdout);
    return failures;
}
