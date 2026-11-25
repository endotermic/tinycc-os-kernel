#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# mkiso.sh - hybrid BIOS + UEFI bootable ISO of out/<arch>: GRUB (grub-mkrescue needs
# grub-pc-bin, grub-efi-amd64-bin, xorriso and mtools; grub-efi-ia32-bin adds 32-bit UEFI)
# with the kernel and both initramfs images, plus the self-hosting image and its two menu
# entries when out/<arch>/initramfs-selfhost.cpio exists; GRUB talks on the serial console
# like the kernel.  The default entry boots into the graphical login (tcc.xdm: /init runs
# xdm, whose greeter on the TinyX X server lists the accounts and runs links -g as the
# session), the second into the graphical mode without a login (tcc.startx); the text
# console entries follow.
# Usage: boot/mkiso.sh [out.iso]      (default: out/<arch>/autarkeia-<arch>.iso)
# Env: ARCH (x86_64, i386), OUT, BUILD (the directories, see scripts/lib.sh)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/lib.sh"
case ${1:-} in -h|--help) usage;; -*) usage 2;; esac
ARCH=${ARCH:-x86_64}
O=$OUT/$ARCH; ISO=${1:-$O/autarkeia-$ARCH.iso}
# grub-mkrescue puts a boot image on the ISO for every GRUB platform under /usr/lib/grub; both
# are needed for the hybrid image: without i386-pc the ISO is UEFI-only and SeaBIOS (QEMU's
# default firmware, ./run.sh --iso) fails with "Could not read from CDROM (code 0009)"
need_reset; need_iso_tools; need_done
[ -f "$O/bzImage" ] || die "no $O/bzImage: run scripts/build.sh first"
R=$BUILD/$ARCH/iso-root
rm -rf "$R" && mkdir -p "$R/boot/grub"
cp "$O/bzImage" "$R/boot/bzImage"
cp "$O/initramfs.cpio.gz" "$R/boot/initramfs.cpio.gz"
cp "$O/initramfs-nolibc.cpio" "$R/boot/initramfs-nolibc.cpio"
# the self-hosting image (scripts/build.sh selfhost) when it exists: its own menu entries
selfhost=
[ ! -f "$O/initramfs-selfhost.cpio" ] || { cp "$O/initramfs-selfhost.cpio" "$R/boot/"; selfhost=1; }
# the firmware of the drivers (scripts/build.sh firmware: Wi-Fi, Bluetooth, GPUs, sound,
# Realtek Ethernet) when it exists: the ISO's /firmware directory, every file xz-compressed
# (the kernel's FW_LOADER_COMPRESS_XZ), not an initrd.  GRUB reads initrds through the BIOS or
# UEFI disk calls, ~15 MB/s in QEMU: of the 12.6 s of black screen before the kernel started,
# the 123 MB firmware image took 7.4 s (SeaBIOS; on OVMF minutes).  /init mounts the medium instead (userland/firmware-medium.c), finds it
# by its /firmware/.id, which every entry's tcc.firmware= names, hands the directory to the
# firmware loader and probes again the devices that missed their files; it costs no RAM, so
# the self-hosting entries have it too.  Made once per firmware image (its checksum is the
# ID), in $BUILD/<arch>/firmware-iso; symlinks to files become NAME.xz -> TARGET.xz, as the
# loader asks for NAME.xz.
fwid=
if [ -f "$O/firmware.cpio.gz" ]; then
    need_reset; need_tool xz:xz-utils cpio; need_done
    fwid=$(sha256sum "$O/firmware.cpio.gz" | cut -c1-16)
    FWT=$BUILD/$ARCH/firmware-iso
    if [ "$(cat "$FWT/firmware/.id" 2>/dev/null)" != "$fwid" ]; then
        echo "   firmware: unpacking $O/firmware.cpio.gz into the ISO's /firmware, xz-compressed (once per firmware image)"
        rm -rf "$FWT"; mkdir -p "$FWT/unpack"
        (cd "$FWT/unpack" && gzip -dc "$O/firmware.cpio.gz" | cpio -id --quiet) || die "cannot unpack $O/firmware.cpio.gz"
        F=$FWT/unpack/lib/firmware
        find "$F" -type l | while IFS= read -r l; do
            [ -f "$l" ] || continue     # a link to a directory stays (intel/sof-ace-tplg)
            t=$(readlink "$l"); rm "$l"; ln -s "$t.xz" "$l.xz"
        done
        # -C crc32: the kernel's xz decoder checks CRC32 (or none), not xz's default CRC64;
        # 4 at a time: xz -6 takes ~100 MB each
        find "$F" -type f -print0 | xargs -0 -n 8 -P 4 xz -C crc32 -6 -q || die "xz failed"
        mv "$F" "$FWT/firmware" && rm -rf "$FWT/unpack"
        echo "$fwid" > "$FWT/firmware/.id"
    fi
    cp -al "$FWT/firmware" "$R/firmware"
    echo "   firmware: $(find "$R/firmware" -type f | wc -l) files, $(du -sm "$R/firmware" | cut -f1) MB on the ISO (ID $fwid)"
fi
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
# VirtualBox's and VMware's firmware report no EDID, and "auto" falls back to 640x480 there,
# too small for the login screen: on those machines (the SMBIOS system manufacturer, type 1
# offset 4) ask for a desktop mode first.  Real machines keep their EDID mode.
if smbios --type 1 --get-string 4 --set sysvendor; then
    if regexp '^(innotek GmbH|Oracle Corporation|VMware, Inc\.)$' "$sysvendor"; then
        set gfxpayload=1280x800,1280x1024,1024x768,auto
    fi
fi
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
        set default=5
    fi
fi

# the default: the graphical login.  tcc.xdm makes /init run xdm after the banner: the X
# server (TinyX's Xfbdev on the framebuffer, tty5), its greeter with the helper text that
# lists the accounts and passwords, and the links browser in graphics mode as the session;
# the shell stays on the serial console.  tcc.startx is the same screen without a login.
menuentry "Autarkeia OS: graphical login (xdm on the TinyX X server, links as the session), all compiled by tinycc" {
    linux /boot/bzImage console=ttyS0 console=tty0 rootfstype=ramfs tcc.xdm
    initrd /boot/initramfs.cpio.gz
}
menuentry "graphical mode without a login: the X server and the links browser as the console user" {
    linux /boot/bzImage console=ttyS0 console=tty0 rootfstype=ramfs tcc.startx
    initrd /boot/initramfs.cpio.gz
}
menuentry "text console: Linux, busybox and tcc (screen + serial; startx opens the graphical mode)" {
    linux /boot/bzImage console=ttyS0 console=tty0 rootfstype=ramfs
    initrd /boot/initramfs.cpio.gz
}
menuentry "the same, serial console first (QEMU -nographic, headless machines)" {
    linux /boot/bzImage console=tty0 console=ttyS0 rootfstype=ramfs
    initrd /boot/initramfs.cpio.gz
}
menuentry "smoke test: libc-free init, prints a banner and reboots" {
    linux /boot/bzImage console=tty0 console=ttyS0 rootfstype=ramfs
    initrd /boot/initramfs-nolibc.cpio
}
CFG
[ -z "$selfhost" ] || cat >> "$R/boot/grub/grub.cfg" <<'CFG'
menuentry "self-hosting: the same plus the sources and the toolchain to rebuild the kernel and tcc inside (needs 1 GB)" {
    linux /boot/bzImage console=tty0 console=ttyS0 rootfstype=ramfs $selfhost_args
    initrd /boot/initramfs-selfhost.cpio
}
menuentry "self-hosting, screen first" {
    linux /boot/bzImage console=ttyS0 console=tty0 rootfstype=ramfs $selfhost_args
    initrd /boot/initramfs-selfhost.cpio
}
CFG
# every entry: a line on the screen while GRUB reads the files (nothing else shows there
# until the kernel's console starts), and the firmware's ID for /init
sed -i -E 's#^(    linux /boot/bzImage .*)$#    echo "Loading Linux and the system image ..."\n\1'"${fwid:+ tcc.firmware=$fwid}"'#' "$R/boot/grub/grub.cfg"
# every GRUB platform installed under /usr/lib/grub is included: i386-pc (BIOS), x86_64-efi,
# and i386-efi when grub-efi-ia32-bin is installed (32-bit UEFI: Bay Trail tablets and 2-in-1s)
grub-mkrescue -o "$ISO" "$R" >"$BUILD/$ARCH/logs/iso.log" 2>&1 || { tail -5 "$BUILD/$ARCH/logs/iso.log"; die "grub-mkrescue failed"; }
ls -l "$ISO" | awk '{print "   " $5 "\t" $9}'
