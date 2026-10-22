/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * init.c - a libc-free PID 1 for the minimal kernel.
 *
 * It prints a banner and the running kernel's uname, then asks the kernel
 * to reboot so that QEMU (started with -no-reboot) exits cleanly.
 *
 * Build (no libc, static, x86_64):
 *   tcc -nostdlib -static -o init init.c
 *   gcc -nostdlib -static -o init init.c
 */

typedef unsigned long ul;

static long sys4(long n, long a, long b, long c, long d)
{
    long r;
    __asm__ volatile ("mov %5, %%r10\n\tsyscall"
                      : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(d)
                      : "rcx", "r11", "r10", "memory");
    return r;
}

#define SYS_write   1
#define SYS_uname   63
#define SYS_reboot  169
#define SYS_exit    60

static void print(const char *s)
{
    ul n = 0;
    while (s[n])
        n++;
    sys4(SYS_write, 1, (long)s, n, 0);
}

struct utsname { char f[6][65]; };   /* sysname nodename release version machine domainname */

void _start(void)
{
    struct utsname u;

    print("\n*** hello from minimal init (PID 1), no libc, compiled by tcc ***\n");
    if (sys4(SYS_uname, (long)&u, 0, 0, 0) == 0) {
        print("kernel: "); print(u.f[0]); print(" ");
        print(u.f[2]); print(" ");
        print(u.f[4]); print("\n");
        print("build:  "); print(u.f[3]); print("\n");
    }
    print("*** init done, rebooting ***\n");

    /* reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART, 0) */
    sys4(SYS_reboot, 0xfee1dead, 672274793, 0x01234567, 0);

    for (;;)
        sys4(SYS_exit, 0, 0, 0, 0);
}
