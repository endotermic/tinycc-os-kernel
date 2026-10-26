#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# mkiso.sh - hybrid BIOS + UEFI bootable ISO of out/<arch>: GRUB (grub-mkrescue needs
# grub-pc-bin, grub-efi-amd64-bin, xorriso and mtools) with the kernel and both initramfs
# images; GRUB talks on the serial console like the kernel.
# Usage: boot/mkiso.sh [out.iso]   Env: ARCH (x86_64), OUT, BUILD
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/lib.sh"
ARCH=${ARCH:-x86_64}
O=$OUT/$ARCH; ISO=${1:-$O/tcc-linux-$ARCH.iso}
# grub-mkrescue puts a boot image on the ISO for every GRUB platform under /usr/lib/grub; both
# are needed for the hybrid image: without i386-pc the ISO is UEFI-only and SeaBIOS (QEMU's
# default firmware, ./run.sh --iso) fails with "Could not read from CDROM (code 0009)"
need_reset; need_iso_tools; need_done
[ -f "$O/bzImage" ] || die "no $O/bzImage: run scripts/build.sh first"
R=$BUILD/$ARCH/iso-root
rm -rf "$R" && mkdir -p "$R/boot/grub"
cp "$O/bzImage" "$R/boot/bzImage"
cp "$O/initramfs.cpio" "$R/boot/initramfs.cpio"
cp "$O/initramfs-nolibc.cpio" "$R/boot/initramfs-nolibc.cpio"
# the self-hosting image (scripts/build.sh selfhost) when it exists: its own menu entries
selfhost=
[ ! -f "$O/initramfs-selfhost.cpio" ] || { cp "$O/initramfs-selfhost.cpio" "$R/boot/"; selfhost=1; }
cat > "$R/boot/grub/grub.cfg" <<'CFG'
serial --unit=0 --speed=115200
terminal_input serial console
terminal_output serial console
set default=0
set timeout=3
# Set a graphics mode before booting and hand its linear framebuffer to the kernel
# (simplefb -> fbcon on tty0).  "auto" takes the monitor's preferred mode from EDID when the
# firmware reports one (VBE/DDC on BIOS, GOP on UEFI), else a sane default.
insmod all_video
set gfxpayload=auto
# A virtual machine may ask for a mode through SMBIOS OEM string 1 ("gfxpayload=<modes>");
# run.sh does that with -smbios type=11, since QEMU's display has no monitor to read EDID from.
# The same string may carry, space-separated, "menu=selfhost" (run.sh --iso --selfhost: boot
# the self-hosting entry) and extra kernel parameters after it (tcc.selfhost=auto).
set selfhost_args=""
if smbios --type 11 --get-string 4 --set oem; then
    regexp --set 1:gfxpayload '(^| )gfxpayload=([^ ]*)' "$oem"
    regexp --set 2:gfxpayload '(^| )gfxpayload=([^ ]*)' "$oem"
    if regexp --set 1:selfhost_args '(^| )menu=selfhost ?(.*)$' "$oem"; then
        regexp --set 2:selfhost_args '(^| )menu=selfhost ?(.*)$' "$oem"
        set default=3
    fi
fi

menuentry "tinycc-os-kernel: Linux, busybox and tcc, all compiled by tinycc (screen + serial)" {
    linux /boot/bzImage console=ttyS0 console=tty0
    initrd /boot/initramfs.cpio
}
menuentry "the same, serial console first (QEMU -nographic, headless machines)" {
    linux /boot/bzImage console=tty0 console=ttyS0
    initrd /boot/initramfs.cpio
}
menuentry "smoke test: libc-free init, prints a banner and reboots" {
    linux /boot/bzImage console=tty0 console=ttyS0
    initrd /boot/initramfs-nolibc.cpio
}
CFG
[ -z "$selfhost" ] || cat >> "$R/boot/grub/grub.cfg" <<'CFG'
menuentry "self-hosting: the same plus the sources and the toolchain to rebuild the kernel and tcc inside (needs 1 GB)" {
    linux /boot/bzImage console=tty0 console=ttyS0 $selfhost_args
    initrd /boot/initramfs-selfhost.cpio
}
menuentry "self-hosting, screen first" {
    linux /boot/bzImage console=ttyS0 console=tty0 $selfhost_args
    initrd /boot/initramfs-selfhost.cpio
}
CFG
# every GRUB platform installed under /usr/lib/grub is included (i386-pc + x86_64-efi)
grub-mkrescue -o "$ISO" "$R" >"$BUILD/$ARCH/logs/iso.log" 2>&1 || { tail -5 "$BUILD/$ARCH/logs/iso.log"; die "grub-mkrescue failed"; }
ls -l "$ISO" | awk '{print "   " $5 "\t" $9}'
