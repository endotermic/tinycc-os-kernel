#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# /init of the busybox image (userland/initramfs.list): mount the API filesystems (only /bin/busybox and
# /bin/sh exist in the cpio, so call applets through busybox), create the
# applet links, then run an interactive shell on PID 1's console.  The shell is
# a child, not an exec: PID 1 exiting would panic the kernel.  When the shell
# ends ('exit' or Ctrl-D) we reboot, and QEMU (-no-reboot) exits.
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox mount -t proc     proc     /proc
/bin/busybox mount -t sysfs    sysfs    /sys
/bin/busybox --install -s
# Which busybox is this?  tcc names its read-only data section .data.ro (the name
# stays in the stripped binary); a static glibc carries its tunables' names.
if grep -q -a -F .data.ro /bin/busybox; then
    bb_build="static, musl -- compiled and linked by tinycc as well"
elif grep -q -a -F GLIBC_TUNABLES /bin/busybox; then
    bb_build="static, glibc -- the gcc reference build, not tinycc"
else
    bb_build="static, unknown toolchain"
fi
banner() {
    echo
    echo "*** tinycc-os-kernel: $(uname -s) $(uname -r) $(uname -m), $(uname -v)"
    echo "*** every file of this kernel was compiled, assembled and linked by tinycc (tcc, tld)"
    echo "*** $(busybox | head -1 | cut -d' ' -f1-2) ($bb_build)"
    if [ -x /usr/bin/tcc ]; then
        echo "*** $(tcc -v) is installed, static, musl libc:  try  tcc -run hello.c"
    fi
    # the build number counts the generations: #1 was built on the host (scripts/build.sh),
    # #2 inside a guest running #1, and so on (userland/selfhost/build-kernel sets
    # KBUILD_BUILD_VERSION and KBUILD_BUILD_HOST=tinycc-os-guest)
    gen=$(uname -v | sed -n 's/^#\([0-9][0-9]*\).*/\1/p')
    builder=$(sed -n 's/^[^(]*(\([^)]*\)).*/\1/p' /proc/version)
    case $builder in
        *@tinycc-os-guest) echo "*** generation ${gen:-?}: this kernel was built inside a tinycc-os-kernel guest ($builder), with the guest's own tcc, tld and make";;
    esac
    if [ -x /usr/bin/build-kernel ]; then
        echo "*** self-hosting: build-kernel rebuilds this kernel from /usr/src, build-toolchain rebuilds tcc and tld,"
        echo "***   send-file copies results to the host; $(nproc) CPUs, $(awk '/MemTotal/{printf "%d MB", $2/1024}' /proc/meminfo) RAM; see /usr/share/tinycc-os-kernel/README"
    fi
    if [ -e /sys/class/graphics/fb0 ]; then
        echo "*** screen: framebuffer $(cat /sys/class/graphics/fb0/virtual_size | tr , x) ($(cat /sys/class/graphics/fb0/name)), tty1-tty6"
    elif [ -e /dev/tty1 ]; then
        echo "*** screen: VGA text console, tty1-tty6"
    fi
    echo "*** 'exit' (or Ctrl-D) in the console's shell shuts the machine down"
    echo
}
cd /root
# A shell on the screen (tty1, keyboard) too, when there is one and /dev/console is another
# device (the last "console=" of the command line, listed last in console/active).  Not via
# cttyhack, which always picks that last console device: the new session opens /dev/tty1
# itself, which makes it its controlling terminal.
primary=$(awk '{print $NF}' /sys/class/tty/console/active 2>/dev/null)
if [ -e /dev/tty1 ] && [ "$primary" != tty0 ]; then
    banner > /tmp/banner
    setsid sh -c 'exec </dev/tty1 >/dev/tty1 2>&1; cat /tmp/banner; exec sh' &
fi
# the shell on /dev/console's device (cttyhack finds it: ttyS0, or tty0 = the active VT)
banner
# tcc.selfhost=auto (run.sh --selfhost --auto): rebuild the kernel (auto=all: the toolchain
# first, then the kernel with it), send the result to the host and shut down, no shell
case " $(cat /proc/cmdline) " in
    *" tcc.selfhost="*)
        mode=$(sed -n 's/.*tcc.selfhost=\([^ ]*\).*/\1/p' /proc/cmdline)
        echo "*** tcc.selfhost=$mode: automatic self-hosting run"
        rc=0
        if [ "$mode" = all ]; then build-toolchain || rc=$?; fi
        [ $rc != 0 ] || build-kernel || rc=$?
        if [ $rc = 0 ]; then send-file /build/linux/arch/x86/boot/bzImage /build/build-kernel.log || rc=$?
        else for f in /build/build-kernel.log /build/build-toolchain.log; do [ ! -f $f ] || send-file $f; done; fi
        echo "*** self-hosting run finished with status $rc, shutting down"
        exec reboot -f;;
esac
setsid cttyhack sh
echo "*** shell exited, rebooting"
exec reboot -f
