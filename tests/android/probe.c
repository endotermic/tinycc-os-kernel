/* SPDX-License-Identifier: BSD-2-Clause */
/* tests/android/probe.c - the /init of a one-file initramfs that checks the kernel features an
 * Android container needs (kernels/linux/android.config), then powers off.  Built static by
 * build/<arch>/musl-tcc and booted by tests/android/run.sh.  One line per check:
 *   probe: NAME ok|FAIL detail
 * and at the end "probe: done: N ok, M failed".
 * binder: a context manager (this process) and a client (a child) on binderfs exchange one
 * transaction and its reply; the rest: epoll/eventfd/timerfd/signalfd, memfd seals, cgroup2,
 * PSI, a BPF map and a cgroup_skb program attached, a seccomp filter, overlayfs copy-up, the
 * device nodes (loop, fuse, uinput, udmabuf, dma_heap, tun), and unshare() of every namespace. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/bpf.h>
#include <linux/loop.h>
#include <linux/memfd.h>

static int fails, oks;
static void res(const char *name, int ok, const char *fmt, ...)
{
    printf("probe: %-14s %s ", name, ok ? "ok" : "FAIL");
    if (fmt) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
    if (!ok) printf(" (errno %d %s)", errno, strerror(errno));
    printf("\n"); fflush(stdout);
    if (ok) oks++; else fails++;
}


static int readfile(const char *p, char *buf, int n)
{
    int fd = open(p, O_RDONLY), r;
    if (fd < 0) return -1;
    r = read(fd, buf, n - 1); close(fd);
    if (r < 0) return -1;
    buf[r] = 0; return r;
}
static int writefile(const char *p, const char *s)
{
    int fd = open(p, O_WRONLY), r;
    if (fd < 0) return -1;
    r = write(fd, s, strlen(s)); close(fd);
    return r < 0 ? -1 : 0;
}

static void t_events(void)
{
    int ep = epoll_create1(EPOLL_CLOEXEC), ev = eventfd(0, EFD_NONBLOCK);
    int tf = timerfd_create(CLOCK_MONOTONIC, 0);
    struct epoll_event e = { .events = EPOLLIN }, out[4];
    struct itimerspec its = { .it_value = { 0, 20000000 } };
    uint64_t v = 7, got = 0; int n, seen_ev = 0, seen_tf = 0;
    res("epoll", ep >= 0, "fd %d", ep);
    res("eventfd", ev >= 0, "fd %d", ev);
    res("timerfd", tf >= 0, "fd %d", tf);
    if (ep < 0 || ev < 0 || tf < 0) return;
    e.data.fd = ev; epoll_ctl(ep, EPOLL_CTL_ADD, ev, &e);
    e.data.fd = tf; epoll_ctl(ep, EPOLL_CTL_ADD, tf, &e);
    write(ev, &v, 8);
    timerfd_settime(tf, 0, &its, 0);
    for (int i = 0; i < 4 && !(seen_ev && seen_tf); i++) {
        n = epoll_wait(ep, out, 4, 1000);
        for (int j = 0; j < n; j++) {
            if (out[j].data.fd == ev) { read(ev, &got, 8); seen_ev = got == 7; }
            if (out[j].data.fd == tf) { read(tf, &got, 8); seen_tf = 1; }
        }
    }
    res("epoll_wait", seen_ev && seen_tf, "eventfd=%d timerfd=%d", seen_ev, seen_tf);
    sigset_t m; sigemptyset(&m); sigaddset(&m, SIGUSR1); sigprocmask(SIG_BLOCK, &m, 0);
    int sf = signalfd(-1, &m, 0); struct signalfd_siginfo si = {0};
    if (sf >= 0) { kill(getpid(), SIGUSR1); read(sf, &si, sizeof si); }
    res("signalfd", sf >= 0 && si.ssi_signo == SIGUSR1, "signo %u", si.ssi_signo);
}

static void t_memfd(void)
{
    int fd = syscall(SYS_memfd_create, "ashmem", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    char *p = MAP_FAILED;
    if (fd >= 0 && ftruncate(fd, 65536) == 0)
        p = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p != MAP_FAILED) strcpy(p, "hi");
    res("memfd", p != MAP_FAILED && fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0, 0);
}

/* binder: a context manager (parent) and a client (child) doing one transaction + reply */
struct bwr_buf { uint32_t cmd; struct binder_transaction_data tr; } __attribute__((packed));
static int binder_open(const char *dev, void **map)
{
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    *map = mmap(0, 128 * 1024, PROT_READ, MAP_PRIVATE, fd, 0);
    if (*map == MAP_FAILED) { close(fd); return -1; }
    return fd;
}
static int binder_rw(int fd, void *w, size_t wn, void *r, size_t rn, size_t *got)
{
    struct binder_write_read bwr = { .write_size = wn, .write_buffer = (uintptr_t)w,
                                     .read_size = rn, .read_buffer = (uintptr_t)r };
    int ret = ioctl(fd, BINDER_WRITE_READ, &bwr);
    if (got) *got = bwr.read_consumed;
    return ret;
}
/* walk the read buffer for one BR_ code; returns pointer to its payload */
static void *find_br(void *buf, size_t n, uint32_t want)
{
    char *p = buf, *end = p + n;
    while (p + 4 <= end) {
        uint32_t c = *(uint32_t *)p; p += 4;
        if (c == want) return p;
        p += _IOC_SIZE(c);
    }
    return 0;
}

static void t_binder(void)
{
    struct binderfs_device dev = { .name = "probe" };
    struct binder_version ver = { 0 };
    void *map; char rbuf[512]; size_t got; int fd, ctl;
    mkdir("/dev/binderfs", 0755);
    res("binderfs", mount("binder", "/dev/binderfs", "binder", 0, 0) == 0, 0);
    int all = access("/dev/binderfs/binder", F_OK) == 0 && access("/dev/binderfs/hwbinder", F_OK) == 0
           && access("/dev/binderfs/vndbinder", F_OK) == 0;
    res("binder-devs", all, "binder hwbinder vndbinder");
    ctl = open("/dev/binderfs/binder-control", O_RDWR);
    res("binder-ctl", ctl >= 0 && ioctl(ctl, BINDER_CTL_ADD, &dev) == 0
        && access("/dev/binderfs/probe", F_OK) == 0, "added %d:%d", dev.major, dev.minor);
    fd = binder_open("/dev/binderfs/binder", &map);
    res("binder-open", fd >= 0, 0);
    if (fd < 0) return;
    res("binder-ver", ioctl(fd, BINDER_VERSION, &ver) == 0 && ver.protocol_version == BINDER_CURRENT_PROTOCOL_VERSION,
        "protocol %d", ver.protocol_version);
    res("binder-mgr", ioctl(fd, BINDER_SET_CONTEXT_MGR, 0) == 0, 0);
    uint32_t enter = BC_ENTER_LOOPER;
    binder_rw(fd, &enter, 4, 0, 0, 0);

    pid_t pid = fork();
    if (pid == 0) {                       /* client: transaction to handle 0, wait for reply */
        void *cm; char cr[512]; size_t cg = 0; int ok = 0;
        int cfd = binder_open("/dev/binderfs/binder", &cm);
        static char data[] = "hello-binder";
        struct bwr_buf b = { BC_TRANSACTION };
        b.tr.target.handle = 0; b.tr.code = 42;
        b.tr.data_size = sizeof data; b.tr.data.ptr.buffer = (uintptr_t)data;
        if (cfd >= 0 && binder_rw(cfd, &b, sizeof b, 0, 0, 0) == 0) {
            for (int i = 0; i < 5 && !ok; i++) {
                if (binder_rw(cfd, 0, 0, cr, sizeof cr, &cg) < 0) break;
                struct binder_transaction_data *t = find_br(cr, cg, BR_REPLY);
                if (t && t->data_size >= 6 && memcmp((void *)(uintptr_t)t->data.ptr.buffer, "pong:", 5) == 0) ok = 1;
            }
        }
        _exit(ok ? 0 : 1);
    }
    int served = 0; uint32_t code = 0;
    for (int i = 0; i < 5 && !served; i++) {
        if (binder_rw(fd, 0, 0, rbuf, sizeof rbuf, &got) < 0) break;
        struct binder_transaction_data *t = find_br(rbuf, got, BR_TRANSACTION);
        if (!t) continue;
        code = t->code;
        served = t->data_size == 13 && strcmp((char *)(uintptr_t)t->data.ptr.buffer, "hello-binder") == 0;
        struct { uint32_t c1; binder_uintptr_t buf; uint32_t c2; struct binder_transaction_data tr; } __attribute__((packed)) w;
        static char reply[] = "pong:42";
        memset(&w, 0, sizeof w);
        w.c1 = BC_FREE_BUFFER; w.buf = t->data.ptr.buffer;
        w.c2 = BC_REPLY; w.tr.data_size = sizeof reply; w.tr.data.ptr.buffer = (uintptr_t)reply;
        binder_rw(fd, &w, sizeof w, 0, 0, 0);
    }
    int st = 1; waitpid(pid, &st, 0);
    res("binder-txn", served, "manager got code %u", code);
    res("binder-reply", WIFEXITED(st) && WEXITSTATUS(st) == 0, "client got pong");
}

/* run in a child (unshare changes the caller); reports through its exit status */
static int t_ns(void)
{
    int fl = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWUSER;
    if (unshare(fl) != 0) return 2;
    pid_t p = fork();
    if (p == 0) _exit(getpid() == 1 && sethostname("android", 7) == 0 ? 0 : 1);
    int st = 1; waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 3;
}

static void t_cgroup(void)
{
    char b[256];
    mkdir("/sys/fs/cgroup", 0755);
    res("cgroup2", mount("cgroup2", "/sys/fs/cgroup", "cgroup2", 0, 0) == 0, 0);
    readfile("/sys/fs/cgroup/cgroup.controllers", b, sizeof b);
    b[strcspn(b, "\n")] = 0;
    res("cg-ctrls", strstr(b, "memory") && strstr(b, "cpuset") && strstr(b, "pids"), "%s", b);
    writefile("/sys/fs/cgroup/cgroup.subtree_control", "+memory +pids +cpuset");
    mkdir("/sys/fs/cgroup/android", 0755);
    snprintf(b, sizeof b, "%d", getpid());
    int ok = writefile("/sys/fs/cgroup/android/cgroup.procs", b) == 0
          && access("/sys/fs/cgroup/android/memory.max", F_OK) == 0
          && access("/sys/fs/cgroup/android/cgroup.freeze", F_OK) == 0;
    res("cg-join", ok, "memory.max cgroup.freeze");
    writefile("/sys/fs/cgroup/cgroup.procs", b);
    int n = readfile("/proc/pressure/memory", b, sizeof b);
    res("psi", n > 0 && strncmp(b, "some", 4) == 0, 0);
}

static void t_seccomp(void)
{
    pid_t p = fork();
    if (p == 0) {
        struct sock_filter f[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 77),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog pr = { 4, f };
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &pr)) _exit(2);
        _exit(syscall(SYS_getppid) == -1 && errno == 77 ? 0 : 1);
    }
    int st = 1; waitpid(p, &st, 0);
    res("seccomp", WIFEXITED(st) && WEXITSTATUS(st) == 0, "exit %d", WEXITSTATUS(st));
}

static void t_bpf(void)
{
    union bpf_attr a; memset(&a, 0, sizeof a);
    a.map_type = BPF_MAP_TYPE_ARRAY; a.key_size = 4; a.value_size = 8; a.max_entries = 4;
    int m = syscall(SYS_bpf, BPF_MAP_CREATE, &a, sizeof a);
    res("bpf-map", m >= 0, "fd %d", m);
    struct bpf_insn prog[] = {
        { .code = BPF_ALU64 | BPF_MOV | BPF_K, .dst_reg = 0, .imm = 1 },
        { .code = BPF_JMP | BPF_EXIT },
    };
    char log[256] = "";
    memset(&a, 0, sizeof a);
    a.prog_type = BPF_PROG_TYPE_CGROUP_SKB; a.insn_cnt = 2; a.insns = (uintptr_t)prog;
    a.license = (uintptr_t)"GPL"; a.log_buf = (uintptr_t)log; a.log_size = sizeof log; a.log_level = 1;
    int pf = syscall(SYS_bpf, BPF_PROG_LOAD, &a, sizeof a);
    res("bpf-prog", pf >= 0, "cgroup_skb fd %d %s", pf, pf < 0 ? log : "");
    if (pf >= 0) {
        int cg = open("/sys/fs/cgroup", O_RDONLY | O_DIRECTORY);
        memset(&a, 0, sizeof a);
        a.target_fd = cg; a.attach_bpf_fd = pf; a.attach_type = BPF_CGROUP_INET_EGRESS;
        res("bpf-attach", syscall(SYS_bpf, BPF_PROG_ATTACH, &a, sizeof a) == 0, "cgroup inet egress");
    }
}

static void t_fs(void)
{
    char fsl[4096] = "", miss[256] = "";
    const char *want[] = { "ext4", "erofs", "squashfs", "overlay", "fuse", "binder", "cgroup2", "tmpfs", 0 };
    readfile("/proc/filesystems", fsl, sizeof fsl);
    for (int i = 0; want[i]; i++) {
        char pat[32]; snprintf(pat, sizeof pat, "\t%s\n", want[i]);
        if (!strstr(fsl, pat)) { strcat(miss, " "); strcat(miss, want[i]); }
    }
    errno = 0;
    res("filesystems", !miss[0], "%s%s", miss[0] ? "missing:" : "ext4 erofs squashfs overlay fuse binder cgroup2", miss);

    mkdir("/ov", 0755);
    mount("tmpfs", "/ov", "tmpfs", 0, 0);
    mkdir("/ov/lo", 0755); mkdir("/ov/up", 0755); mkdir("/ov/wk", 0755); mkdir("/ov/m", 0755);
    writefile("/ov/lo/f", ""); int fd = open("/ov/lo/f", O_WRONLY | O_CREAT, 0644); write(fd, "low", 3); close(fd);
    int ok = mount("overlay", "/ov/m", "overlay", 0, "lowerdir=/ov/lo,upperdir=/ov/up,workdir=/ov/wk") == 0;
    if (ok) { fd = open("/ov/m/f", O_WRONLY | O_APPEND); ok = fd >= 0 && write(fd, "+up", 3) == 3; close(fd); }
    char b[16] = ""; readfile("/ov/up/f", b, sizeof b);
    res("overlayfs", ok && strcmp(b, "low+up") == 0, "copy-up %s", b);

    fd = open("/dev/loop-control", O_RDWR);
    int n = fd >= 0 ? ioctl(fd, LOOP_CTL_GET_FREE) : -1;
    res("loop", n >= 0, "free loop%d", n);
    res("fuse", (fd = open("/dev/fuse", O_RDWR)) >= 0, "/dev/fuse"); close(fd);
    res("uinput", (fd = open("/dev/uinput", O_RDWR)) >= 0, "/dev/uinput"); close(fd);
    res("udmabuf", (fd = open("/dev/udmabuf", O_RDWR)) >= 0, "/dev/udmabuf"); close(fd);
    res("dma_heap", (fd = open("/dev/dma_heap/system", O_RDONLY)) >= 0, "/dev/dma_heap/system"); close(fd);
    res("tun", (fd = open("/dev/net/tun", O_RDWR)) >= 0, "/dev/net/tun"); close(fd);
}

int main(void)
{
    mkdir("/proc", 0755); mkdir("/sys", 0755); mkdir("/dev", 0755);
    mount("proc", "/proc", "proc", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);
    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    char u[256] = ""; readfile("/proc/version", u, sizeof u);
    printf("probe: start: %s", u); fflush(stdout);
    t_events(); t_memfd(); t_binder(); t_cgroup(); t_bpf(); t_seccomp(); t_fs();
    pid_t p = fork();
    if (p == 0) _exit(t_ns());
    int st = 1; waitpid(p, &st, 0);
    errno = 0;
    res("namespaces", WIFEXITED(st) && WEXITSTATUS(st) == 0, "mnt pid uts ipc net user, child is pid 1 (status %d)",
        WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    printf("probe: done: %d ok, %d failed\n", oks, fails); fflush(stdout);
    sync(); reboot(RB_POWER_OFF);
    return 0;
}
