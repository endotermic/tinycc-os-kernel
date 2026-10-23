#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# run.sh - boot out/<arch> in QEMU on the serial console.  Extra arguments go to QEMU.
# QEMU exits when the guest reboots (-no-reboot): 'exit' in the shell, or Ctrl-A X.
# Usage: ./run.sh [--iso] [--efi] [--vga|--no-vga] [--nolibc] [--selfhost [--auto]] [qemu args...]
#   --iso     boot the ISO from boot/mkiso.sh instead of -kernel/-initrd; implies --vga when a
#             display is available ($DISPLAY or $WAYLAND_DISPLAY), since the ISO's default menu
#             entry puts the console on the screen
#   --efi     boot with the OVMF UEFI firmware (needs 128M; GRUB's EFI image does not fit in 64M)
#   --vga     open QEMU's display window (standard VGA with EDID) for the VGA/framebuffer
#             console and keyboard; the serial console stays on this terminal
#   --no-vga  headless (-nographic) even with --iso
#   --nolibc  use the libc-free smoke-test image (banner, reboot, exit in ~2 s)
#   --selfhost  the self-hosting image (initramfs-selfhost.cpio from scripts/build.sh selfhost:
#             the busybox image plus the kernel, tcc, tld and make sources and the toolchain to
#             rebuild them inside, see userland/selfhost/README): 1 GB of RAM, and the second
#             serial port (ttyS1) captured to out/<arch>/selfhost/ttyS1.bin, where the guest's
#             "send-file" puts files (scripts/recv-file.sh cuts them out).  With --iso the ISO's
#             self-hosting menu entry is selected.
#   --auto    with --selfhost: the guest runs build-kernel and send-file by itself and shuts
#             down (kernel command line tcc.selfhost=auto; --auto=all also rebuilds the
#             toolchain first and uses it; --auto=test rebuilds the toolchain and runs tcc's
#             test suite on it, no kernel: the logs come back); scripts/selfhost-test.sh
#             drives the first two
# Env: ARCH (x86_64, i386), OUT, MEM (64M; 1G with --selfhost), KERNEL (a bzImage to boot instead of
# out/<arch>/bzImage, e.g. the one built inside the guest), SMP (virtual CPUs; default: the
# host's, at most 8 = CONFIG_NR_CPUS), KVM (0: no hardware virtualization; default: KVM when
# /dev/kvm is writable, else TCG), RES (screen resolution for QEMU's display, default
# 1024x768 = XGA: the ISO gets it through an SMBIOS OEM string that GRUB reads for gfxpayload;
# the direct boot stays on the VGA text console since the tcc kernel is built without the
# real-mode video mode setting (CONFIG_X86_BOOT_VIDEO_MODES, kernels/linux/tcc.config), so
# vga= is not passed; RES= (empty) leaves GRUB to its
# defaults), OVMF (firmware path), TIMEOUT (seconds; default none on a terminal, 30 when stdin
# is not a tty; 0 = none)
ROOT=$(cd "$(dirname "$0")" && pwd)
. "$ROOT/scripts/lib.sh"
ARCH=${ARCH:-x86_64}; O=$OUT/$ARCH
iso= efi= vga= selfhost= auto= initrd=$O/initramfs.cpio
while [ $# -gt 0 ]; do
    case $1 in
        --iso) iso=1; shift;; --efi) efi=1; shift;; --vga) vga=1; shift;; --no-vga) vga=0; shift;;
        --nolibc) initrd=$O/initramfs-nolibc.cpio; shift;;
        --selfhost) selfhost=1; initrd=$O/initramfs-selfhost.cpio; shift;;
        --auto) auto=auto; shift;; --auto=*) auto=${1#--auto=}; shift;;
        --) shift; break;; *) break;;
    esac
done
[ -z "$auto" ] || [ -n "$selfhost" ] || die "--auto needs --selfhost"
case $ARCH in x86_64) QEMU=qemu-system-x86_64;; i386) QEMU=qemu-system-i386;; *) die "ARCH=$ARCH: no QEMU setup";; esac
have $QEMU || die "$QEMU not found"
if [ -z "$TIMEOUT" ]; then if [ -t 0 ]; then TIMEOUT=0; else TIMEOUT=30; fi; fi
[ "$TIMEOUT" = 0 ] && TIMEOUT= || TIMEOUT="timeout $TIMEOUT"
if [ -n "$efi" ]; then
    # a combined image works with -bios; a split CODE image needs pflash with a VARS copy
    fw=$OVMF
    for f in /usr/share/qemu/OVMF.fd /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd \
             /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd; do
        [ -n "$fw" ] || [ ! -f "$f" ] || fw=$f
    done
    [ -f "$fw" ] || die "no OVMF firmware found (set OVMF=/path/to/OVMF.fd)"
    case $fw in
        *CODE*)
            vars=$(echo "$fw" | sed 's/CODE/VARS/'); tmpvars=${TMPDIR:-/tmp}/tcc-linux-ovmf-vars.$$.fd
            [ -f "$vars" ] || die "no VARS image next to $fw"
            cp "$vars" "$tmpvars"; trap 'rm -f "$tmpvars"' EXIT
            set -- -drive if=pflash,format=raw,readonly=on,file="$fw" -drive if=pflash,format=raw,file="$tmpvars" "$@";;
        *)  set -- -bios "$fw" "$@";;
    esac
    MEM=${MEM:-128M}
fi
# screen resolution of QEMU's VGA for the ISO: the GRUB gfxpayload (see boot/mkiso.sh: read
# from SMBIOS OEM string 1; ",," is a literal comma in a QEMU option value).  The direct boot
# has no vga= any more: the tcc kernel's real-mode setup does not set video modes (K12 in
# docs/FIX-LOG.md), the screen is the VGA text console there.
RES=${RES-1024x768}
# the self-hosting image: RAM for the source tree, the build and the compilers (see
# userland/selfhost/build-kernel), the second UART captured to a file, GRUB's menu entry
# and the guest's automatic run chosen through SMBIOS OEM strings / the command line
cmdline="console=tty0 console=ttyS0"
if [ -n "$selfhost" ]; then
    MEM=${MEM:-1G}
    mkdir -p "$O/selfhost"
    : > "$O/selfhost/ttyS1.bin"
    set -- -serial file:"$O/selfhost/ttyS1.bin" "$@"
    [ -z "$auto" ] || cmdline="$cmdline tcc.selfhost=$auto"
fi
KERNEL=${KERNEL:-$O/bzImage}
if [ -n "$iso" ]; then
    [ -f "$O/tcc-linux-$ARCH.iso" ] || die "no $O/tcc-linux-$ARCH.iso: scripts/build.sh iso"
    set -- -cdrom "$O/tcc-linux-$ARCH.iso" -boot d "$@"
    # one OEM string, space-separated words that grub.cfg parses: the gfxpayload, and with
    # --selfhost the menu entry to boot plus the extra kernel parameters (",," is a literal
    # comma in a QEMU option value; GRUB's "smbios --get-string 4" returns the string whose
    # index is the byte at offset 4, the string count, so a second string would hide the first)
    oem="${RES:+gfxpayload=${RES}x32,,${RES},,auto}${selfhost:+${RES:+ }menu=selfhost${auto:+ tcc.selfhost=$auto}}"
    [ -z "$oem" ] || set -- -smbios "type=11,value=$oem" "$@"
else
    [ -f "$KERNEL" ] && [ -f "$initrd" ] || die "no kernel ($KERNEL) or initramfs ($initrd): run ./install.sh or scripts/build.sh${selfhost:+ selfhost}"
    set -- -kernel "$KERNEL" -initrd "$initrd" -append "$cmdline" "$@"
fi
# CPUs and hardware virtualization: the SMP kernel uses every virtual CPU (the self-hosting
# build runs make -j on them); KVM is 10x faster than TCG for compiling inside
# One socket per CPU: without ACPI the kernel enumerates CPUs from SeaBIOS's MP table, and
# SeaBIOS lists only the first logical CPU of each package there (QEMU's default topology
# puts every CPU in one package, which would leave the guest with one CPU).
SMP=${SMP:-$(nproc 2>/dev/null || echo 1)}; [ "$SMP" -le 8 ] || SMP=8
set -- -smp "$SMP,sockets=$SMP,cores=1,threads=1" "$@"
if [ "$KVM" != 0 ] && [ -w /dev/kvm ]; then set -- -accel kvm "$@"
elif [ "$KVM" != 0 ]; then echo "   /dev/kvm is not writable: software emulation (TCG), slow for compiling" >&2; fi
if [ -n "$iso" ] && [ -z "$vga" ]; then
    # the ISO boots to the screen by default: open the window when there is a display
    if [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; then vga=1; else echo "   no display: headless (--vga to force)" >&2; fi
fi
if [ "$vga" = 1 ]; then
    set -- -vga std -serial mon:stdio "$@"      # window + serial on this terminal (Ctrl-A C: monitor)
else
    set -- -nographic -serial mon:stdio "$@"    # explicit, so that --selfhost's -serial file: is the second port
fi
$TIMEOUT $QEMU -m "${MEM:-64M}" -no-reboot "$@"
