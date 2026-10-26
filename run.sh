#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# run.sh - boot out/<arch> in QEMU on the serial console.  Extra arguments go to QEMU.
# QEMU exits when the guest reboots (-no-reboot): 'exit' in the shell, or Ctrl-A X.
# Usage: ./run.sh [--arch ARCH] [--iso] [--efi] [--vga|--no-vga] [--no-devices] [--nolibc] [--selfhost [--auto]] [qemu args...]
#        ./run.sh --list
#   --arch    x86_64 or i386 (also ARCH in the environment).  Without it, run.sh looks at what
#             is in place in out/<arch>/ (the ISO, or the kernel and the initramfs image a
#             form needs): on a terminal it offers a numbered menu of every bootable form of
#             every architecture (of the given form only, with --nolibc/--selfhost/--iso),
#             otherwise it takes the one architecture that has the form (x86_64 when both do).
#             Whatever is chosen, a missing file is named, with the command that builds it,
#             before QEMU starts.
#   --list    print that table (ok / missing files) and exit
#   --iso     boot the ISO from boot/mkiso.sh instead of -kernel/-initrd; implies --vga when a
#             display is available ($DISPLAY or $WAYLAND_DISPLAY), since the ISO's default menu
#             entry puts the console on the screen
#   --efi     boot with the OVMF UEFI firmware (needs 128M; GRUB's EFI image does not fit in 64M)
#   --vga     open QEMU's display window (standard VGA with EDID) for the VGA/framebuffer
#             console and keyboard; the serial console stays on this terminal
#   --no-vga  headless (-nographic) even with --iso
#   --no-devices  only the serial ports and the VGA: without the emulated devices that the
#             drivers of kernels/linux/drivers.config handle, which are attached by default:
#             an e1000 Ethernet card on QEMU's user-mode network (DHCP, NAT to the host;
#             10.0.2.2 is the host), an Intel HD Audio card with a codec (duplex), an xHCI USB
#             controller with a USB tablet and a USB stick (USBSTICK below), and the standard
#             VGA with an EDID that names RES as the preferred mode, which the bochs-drm
#             driver sets (so the direct boot gets a framebuffer console too); the PS/2 mouse
#             is always there.  The guest's devices-test checks all of them (scripts/devices-test.sh)
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
#   --        end of run.sh's options; everything after it goes to QEMU as it is
#   -h, --help  this text
# Env: ARCH (x86_64, i386; see --arch), OUT, MEM (128M; 1G with --selfhost), KERNEL (a bzImage to boot instead of
# out/<arch>/bzImage, e.g. the one built inside the guest), SMP (virtual CPUs; default: the
# host's, at most 8 = CONFIG_NR_CPUS), KVM (0: no hardware virtualization; default: KVM with the
# host's CPU model when /dev/kvm is writable, else TCG), RES (screen resolution for QEMU's display, default
# 1024x768 = XGA: the preferred mode of the VGA's EDID, which the kernel's bochs-drm driver
# sets, and for the ISO also GRUB's gfxpayload through an SMBIOS OEM string; the tcc kernel
# is built without the real-mode video mode setting (CONFIG_X86_BOOT_VIDEO_MODES,
# kernels/linux/tcc.config), so vga= is not passed; RES= (empty) leaves the EDID and GRUB to their
# defaults), APPEND (extra kernel command line parameters for the direct boot, e.g.
# tcc.devtest), AUDIO (QEMU audio backend of the sound card: none, the default, plays into
# the void; pa, pipewire, alsa, sdl play on the host), USBSTICK (disk image attached as the
# USB stick: out/<arch>/usbstick.img by default, created empty with 16 MB when missing;
# USBSTICK= attaches none), OVMF (firmware path), TIMEOUT (seconds; default none on a terminal, 30 when stdin
# is not a tty; 0 = none)
ROOT=$(cd "$(dirname "$0")" && pwd)
. "$ROOT/scripts/lib.sh"
iso= efi= vga= selfhost= auto= form=shell list= devices=1
while [ $# -gt 0 ]; do
    case $1 in
        --arch) ARCH=$2; shift 2;; --arch=*) ARCH=${1#--arch=}; shift;;
        -h|--help) usage;;
        --list) list=1; shift;;
        --iso) iso=1; form=iso; shift;; --efi) efi=1; shift;; --vga) vga=1; shift;; --no-vga) vga=0; shift;;
        --no-devices) devices=; shift;;
        --nolibc) form=nolibc; shift;;
        --selfhost) selfhost=1; form=selfhost; shift;;
        --auto) auto=auto; shift;; --auto=*) auto=${1#--auto=}; shift;;
        --) shift; break;; *) break;;
    esac
done
[ -z "$auto" ] || [ -n "$selfhost" ] || die "--auto needs --selfhost"
# What a boot needs from out/<arch>/: the ISO, or the kernel (KERNEL overrides) and the
# initramfs image of the form.  The table of what is in place drives --list, the menu and
# the final check, so a missing file is named before QEMU starts.
ARCHES="x86_64 i386"; FORMS="shell nolibc selfhost iso"
needs() { # needs ARCH FORM: the files, one per line
    case $2 in
        iso) echo "$OUT/$1/tcc-linux-$1.iso";;
        *) echo "${KERNEL:-$OUT/$1/bzImage}"
           case $2 in nolibc) echo "$OUT/$1/initramfs-nolibc.cpio";; selfhost) echo "$OUT/$1/initramfs-selfhost.cpio";; *) echo "$OUT/$1/initramfs.cpio";; esac;;
    esac
}
missing() { for f in $(needs "$1" "$2"); do [ -f "$f" ] || echo "$f"; done; }
describe() { # describe ARCH FORM
    case $2 in
        shell) echo "$1: busybox shell with tcc";; nolibc) echo "$1: libc-free smoke test (--nolibc)";;
        selfhost) echo "$1: the self-hosting image, 1 GB (--selfhost)";; iso) echo "$1: the ISO, GRUB menu (--iso)";;
    esac
}
howto() { # howto ARCH FORM: what builds the missing files
    ao=; [ "$1" = x86_64 ] || ao=" --arch $1"
    case $2 in
        iso) echo "./install.sh$ao --output iso  (or scripts/build.sh$ao iso)";;
        selfhost) echo "./install.sh$ao --selfhost  (or scripts/build.sh$ao selfhost)";;
        *) echo "./install.sh$ao  (or scripts/build.sh$ao all)";;
    esac
}
if [ -n "$list" ]; then
    echo "boot targets in $OUT (./run.sh --arch ARCH [--nolibc|--selfhost|--iso]):"
    for a in $ARCHES; do for f in $FORMS; do
        m=$(missing $a $f)
        if [ -z "$m" ]; then printf '  %-50s ok
' "$(describe $a $f)"
        else printf '  %-50s missing: %s
' "$(describe $a $f)" "$(echo "$m" | sed "s|^$OUT/||" | tr '\n' ' ')"; fi
    done; done
    exit 0
fi
# The architecture: --arch or ARCH; else, on a terminal, a menu of every form that is in place
# (of the chosen form when --nolibc/--selfhost/--iso was given); else the one architecture
# that has the form, x86_64 when both do.
if [ -z "$ARCH" ]; then
    case $form in shell) forms=$FORMS;; *) forms=$form;; esac
    n=0; targets=        # "arch:form" entries; "$@" stays what goes to QEMU
    for a in $ARCHES; do for f in $forms; do [ -n "$(missing $a $f)" ] || { n=$((n + 1)); targets="$targets $a:$f"; }; done; done
    [ $n -gt 0 ] || die "nothing of that kind is built in $OUT: $(howto x86_64 $form); ./run.sh --list shows what is there"
    if [ -t 0 ] && [ -t 1 ] && [ $n -gt 1 ]; then
        while :; do
            echo "What to boot (in place in $OUT):"; i=0
            for t in $targets; do i=$((i + 1)); echo "  $i) $(describe "${t%%:*}" "${t#*:}")"; done
            printf '  1-%s [1]: ' $n; read -r r; [ -n "$r" ] || r=1
            case $r in *[!0-9]*|'') ;; *) [ "$r" -lt 1 ] || [ "$r" -gt $n ] || break;; esac
            echo "  '$r' is not one of the choices" >&2
        done
        i=0; for t in $targets; do i=$((i + 1)); [ "$i" != "$r" ] || break; done
        ARCH=${t%%:*}; form=${t#*:}
        case $form in iso) iso=1;; selfhost) selfhost=1;; esac
    else
        t=${targets# }; t=${t%% *}; ARCH=${t%%:*}
        case " $targets " in *" x86_64:"*) ARCH=x86_64;; esac
        [ "$ARCH" = x86_64 ] || echo "   the $ARCH system (the only one built; --arch chooses, --list lists)" >&2
    fi
fi
case $ARCH in x86_64|i386) ;; *) die "--arch $ARCH: x86_64 or i386";; esac
m=$(missing "$ARCH" "$form")
[ -z "$m" ] || die "not in place for '$(describe "$ARCH" "$form")': $(echo "$m" | tr '\n' ' ')-- build it with $(howto "$ARCH" "$form"); ./run.sh --list shows what is there"
O=$OUT/$ARCH
case $form in nolibc) initrd=$O/initramfs-nolibc.cpio;; selfhost) initrd=$O/initramfs-selfhost.cpio;; *) initrd=$O/initramfs.cpio;; esac
case $ARCH in x86_64) QEMU=qemu-system-x86_64;; i386) QEMU=qemu-system-i386;; *) die "ARCH=$ARCH: no QEMU setup";; esac
need_reset; need_qemu "$ARCH"; need_done
if [ -z "$TIMEOUT" ]; then if [ -t 0 ]; then TIMEOUT=0; else TIMEOUT=30; fi; fi
[ "$TIMEOUT" = 0 ] && TIMEOUT= || TIMEOUT="timeout $TIMEOUT"
if [ -n "$efi" ]; then
    # a combined image works with -bios; a split CODE image needs pflash with a VARS copy
    fw=$OVMF
    for f in /usr/share/qemu/OVMF.fd /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd \
             /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd; do
        [ -n "$fw" ] || [ ! -f "$f" ] || fw=$f
    done
    [ -f "$fw" ] || die "no OVMF firmware found: sudo apt install ovmf  (or set OVMF=/path/to/OVMF.fd)"
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
# screen resolution of QEMU's VGA: the preferred mode of the VGA's EDID (bochs-drm sets it,
# with or without --vga: the device is there headless too) and, for the ISO, the GRUB
# gfxpayload (see boot/mkiso.sh: read from SMBIOS OEM string 1; ",," is a literal comma in
# a QEMU option value).  The direct boot has no vga=: the tcc kernel's real-mode setup does
# not set video modes (K12 in docs/FIX-LOG.md).
RES=${RES-1024x768}
# the self-hosting image: RAM for the source tree, the build and the compilers (see
# userland/selfhost/build-kernel), the second UART captured to a file, GRUB's menu entry
# and the guest's automatic run chosen through SMBIOS OEM strings / the command line
cmdline="console=tty0 console=ttyS0${APPEND:+ $APPEND}"
if [ -n "$selfhost" ]; then
    MEM=${MEM:-1G}
    mkdir -p "$O/selfhost"
    : > "$O/selfhost/ttyS1.bin"
    set -- -serial file:"$O/selfhost/ttyS1.bin" "$@"
    [ -z "$auto" ] || cmdline="$cmdline tcc.selfhost=$auto"
fi
KERNEL=${KERNEL:-$O/bzImage}
if [ -n "$iso" ]; then
    set -- -cdrom "$O/tcc-linux-$ARCH.iso" -boot d "$@"
    # one OEM string, space-separated words that grub.cfg parses: the gfxpayload, and with
    # --selfhost the menu entry to boot plus the extra kernel parameters (",," is a literal
    # comma in a QEMU option value; GRUB's "smbios --get-string 4" returns the string whose
    # index is the byte at offset 4, the string count, so a second string would hide the first)
    oem="${RES:+gfxpayload=${RES}x32,,${RES},,auto}${selfhost:+${RES:+ }menu=selfhost${auto:+ tcc.selfhost=$auto}}"
    [ -z "$oem" ] || set -- -smbios "type=11,value=$oem" "$@"
else
    set -- -kernel "$KERNEL" -initrd "$initrd" -append "$cmdline" "$@"
fi
# CPUs and hardware virtualization: the SMP kernel uses every virtual CPU (the self-hosting
# build runs make -j on them); KVM is 10x faster than TCG for compiling inside
# One socket per CPU: without ACPI the kernel enumerates CPUs from SeaBIOS's MP table, and
# SeaBIOS lists only the first logical CPU of each package there (QEMU's default topology
# puts every CPU in one package, which would leave the guest with one CPU).
SMP=${SMP:-$(nproc 2>/dev/null || echo 1)}; [ "$SMP" -le 8 ] || SMP=8
set -- -smp "$SMP,sockets=$SMP,cores=1,threads=1" "$@"
# -cpu host with KVM: the default qemu64 model advertises SVM, which KVM cannot pass through,
# and QEMU warns once per virtual CPU; the host's own model has no such gap
if [ "$KVM" != 0 ] && [ -w /dev/kvm ]; then set -- -accel kvm -cpu host "$@"
elif [ "$KVM" != 0 ]; then echo "   /dev/kvm is not writable: software emulation (TCG), slow for compiling" >&2; fi
# the emulated devices (--no-devices leaves them out; see the header)
if [ -n "$devices" ]; then
    # (the stick first, so that its usb-storage device follows the xHCI controller on the command line)
    if [ -n "${USBSTICK-$O/usbstick.img}" ]; then
        USBSTICK=${USBSTICK-$O/usbstick.img}
        [ -f "$USBSTICK" ] || { mkdir -p "$(dirname "$USBSTICK")"; truncate -s 16M "$USBSTICK" || die "cannot create $USBSTICK"; }
        set -- -drive if=none,id=usbstick,format=raw,file="$USBSTICK" -device usb-storage,drive=usbstick "$@"
    fi
    vgaopts="edid=on${RES:+,xres=${RES%x*},yres=${RES#*x}}"
    set -- -vga none -device "VGA,$vgaopts" \
           -nic user,model=e1000 \
           -audiodev "${AUDIO:-none},id=snd0" -device intel-hda -device hda-duplex,audiodev=snd0 \
           -device qemu-xhci,id=xhci -device usb-tablet "$@"
else
    set -- -nic none "$@"
fi
if [ -n "$iso" ] && [ -z "$vga" ]; then
    # the ISO boots to the screen by default: open the window when there is a display
    if [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; then vga=1; else echo "   no display: headless (--vga to force)" >&2; fi
fi
if [ "$vga" = 1 ]; then
    set -- -serial mon:stdio "$@"               # window + serial on this terminal (Ctrl-A C: monitor)
    [ -n "$devices" ] || set -- -vga std "$@"
else
    set -- -nographic -serial mon:stdio "$@"    # explicit, so that --selfhost's -serial file: is the second port
fi
$TIMEOUT $QEMU -m "${MEM:-128M}" -no-reboot "$@"
