#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# install.sh - fetch the pinned sources, apply the patches, build and put the results in
# out/<arch>/.  Without arguments on a terminal it asks what to build, the target and the
# output; the same choices are available as options for unattended use.
#
# Usage: ./install.sh [--build tcc|distro] [--arch ARCH] [--output initrd|iso|all] [--selfhost] [--jobs N] [--run] [--test]
#        ./install.sh --clean [--arch ARCH] [--dry-run]  |  ./install.sh --clean-all [--dry-run]
#   --build   tcc: only the patched Tiny C Compiler, installed into a staging tree and packed
#                  as out/<arch>/tcc-<version>-<arch>-linux.tar.gz (extract with tar xzf ... -C /)
#             distro: the kernel and the userland built with that compiler (default)
#   --arch    x86_64 (default), i386 (the 32-bit x86 kernel), arm64 (not available, see the table below)
#   --output  initrd: kernel + initramfs images (default); iso: the same plus the bootable ISO;
#             all: the same as iso
#   --selfhost  also the self-hosting image (initramfs-selfhost.cpio, and its ISO menu entries):
#             the sources and the toolchain to rebuild the kernel and tcc inside the guest
#             (./run.sh --selfhost; scripts/selfhost-test.sh); downloads GNU make's tarball too
#   --jobs    parallel jobs (default: JOBS from the environment, else nproc)
#   --test    run tinycc's test suite after building it
#   --run     boot the result in QEMU when done (./run.sh; distro only)
#   --clean   remove the build trees, logs and results of --arch (build/<arch>/, out/<arch>/) and
#             exit; --clean-all: those of every architecture (build/, out/).  Both print what goes
#             with its size; --dry-run (-n) prints that and removes nothing.  The fetched sources (src/,
#             with the host tcc built in src/tinycc) stay: scripts/build.sh clean-src removes them
#             (only when they have no unexported work)
#   -h, --help  this text
#
# Target status:
#   x86_64  works: the tcc-built kernel boots the tcc-built busybox and tcc in QEMU
#   i386    works: the 32-bit kernel (ARCH=i386, kernels/linux/i386.config) compiled by the
#           i386-tcc cross compiler, musl/busybox/tcc for i386, boots in qemu-system-i386
#   arm64   not available: tcc's arm64 backend has no inline assembler good enough for the
#           kernel; nothing has been attempted (also needs aarch64 binutils and qemu-system-aarch64)
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
. "$ROOT/scripts/lib.sh"
build= arch= output= run= test= selfhost= clean= dry= jobs=${JOBS:-$(nproc)}
while [ $# -gt 0 ]; do
    case $1 in
        --build) build=$2; shift 2;; --build=*) build=${1#*=}; shift;;
        --arch) arch=$2; shift 2;; --arch=*) arch=${1#*=}; shift;;
        --output) output=$2; shift 2;; --output=*) output=${1#*=}; shift;;
        --jobs) jobs=$2; shift 2;; --jobs=*) jobs=${1#*=}; shift;;
        --run) run=1; shift;; --test) test=1; shift;; --selfhost) selfhost=1; shift;;
        --clean) clean=clean; shift;; --clean-all) clean=clean-all; shift;; --dry-run|-n) dry=1; shift;;
        -h|--help) usage;;
        *) echo "unknown option: $1" >&2; usage 2;;
    esac
done
# ask VAR "prompt" default choices...: a numbered menu; each choice is "key: description" or
# "key (note)", the key is what VAR gets.  The answer is a number (Enter = the default's), or a
# key; anything else is asked again.
key() { k=${1%%:*}; echo "${k%% *}"; }
ask() {
    v=$1; p=$2; d=$3; shift 3
    while :; do
        echo "$p"; i=0; dn=
        for c; do i=$((i + 1)); [ "$(key "$c")" != "$d" ] || { dn=$i; c="$c  (default)"; }; echo "  $i) $c"; done
        printf '  1-%s [%s]: ' "$i" "$dn"; read -r a </dev/tty
        [ -n "$a" ] || a=$dn
        case $a in
            *[!0-9]*|'') for c; do [ "$(key "$c")" != "$a" ] || { eval "$v=\$a"; return 0; }; done;;
            *) if [ "$a" -ge 1 ] && [ "$a" -le $# ]; then eval "a=\${$a}"; eval "$v=\$(key \"\$a\")"; return 0; fi;;
        esac
        echo "  '$a' is not one of the choices" >&2
    done
}
[ -z "$dry" ] || [ -n "$clean" ] || die "--dry-run goes with --clean or --clean-all"
if [ -t 0 ] && [ -t 1 ] && [ -z "$clean" ]; then
    [ -n "$build" ] || ask build "What to build:" distro "tcc: only the patched Tiny C Compiler, as a tar.gz archive" "distro: the kernel and userland built with it"
    if [ "$build" = distro ]; then
        [ -n "$arch" ] || ask arch "Target architecture:" x86_64 "x86_64 (works)" "i386 (works: the 32-bit kernel)" "arm64 (not available)"
        [ -n "$output" ] || ask output "Output:" initrd "initrd: kernel + initramfs images (boot with ./run.sh)" "iso: the same plus a bootable BIOS/UEFI ISO" "all: both"
    fi
fi
build=${build:-distro}; arch=${arch:-x86_64}; output=${output:-initrd}
case $build in tcc|distro) ;; *) die "unknown build '$build' (tcc, distro)";; esac
case $arch in
    x86_64|i386) ;;
    arm64) sed -n "/^#   $arch /,/^#   [a-z]\|^set -e/p" "$0" | sed '$d' | cut -c3- >&2; die "arch $arch is not supported yet";;
    *) die "unknown arch '$arch' (x86_64, i386, arm64)";;
esac
case $output in initrd|iso|all) ;; *) die "unknown output '$output' (initrd, iso, all)";; esac
if [ -n "$clean" ]; then
    DRY_RUN=${dry:-0} ARCH=$arch exec "$ROOT/scripts/build.sh" "$clean"
fi
# the host check: everything the chosen build needs, named at once (scripts/lib.sh)
need_reset
need_tool git make gcc tar
if [ "$build" != tcc ]; then
    need_build_tools
    [ -z "$selfhost" ] || need_download_tool
    case $output in iso|all) need_iso_tools;; esac
    [ -z "$run" ] || need_qemu "$arch"
fi
need_done
export ARCH=$arch JOBS=$jobs
if [ "$build" = tcc ]; then
    msg "tinycc-os-kernel: the patched tcc only ($arch), $jobs jobs"
    "$ROOT/scripts/fetch.sh" tinycc
    "$ROOT/scripts/patch.sh" tinycc
    "$ROOT/scripts/build.sh" host-tcc
    [ -z "$test" ] || "$ROOT/scripts/build.sh" tcc-test
    "$ROOT/scripts/build.sh" tcc-archive
    echo
    msg "done: $OUT/$arch"
    ls -l "$OUT/$arch"/*.tar.gz | awk '{print "   " $5 "\t" $9}'
    echo "   install: sudo tar xzf $OUT/$arch/tcc-*.tar.gz -C /      (puts tcc under /usr/local)"
    exit 0
fi
msg "tinycc-os-kernel: distro, arch $arch, output $output${selfhost:+ + self-hosting image}, $jobs jobs"
"$ROOT/scripts/fetch.sh"
"$ROOT/scripts/patch.sh"
"$ROOT/scripts/build.sh" host-tcc
[ -z "$test" ] || "$ROOT/scripts/build.sh" tcc-test
"$ROOT/scripts/build.sh" tld kernel musl busybox ncurses nnn libressl zlib libpng xlibs tinyx links guest-tcc initramfs
[ -z "$selfhost" ] || "$ROOT/scripts/build.sh" selfhost
case $output in iso|all) "$ROOT/scripts/build.sh" iso;; esac
echo
msg "done: $OUT/$arch"
ls -l "$OUT/$arch" | awk 'NR > 1 {print "   " $5 "\t" $9}'
# how to boot it: run.sh alone offers a menu of everything built; the direct lines name the
# architecture, so that they can be pasted as they are whatever else is built
hint() { printf '   %-6s %-34s %s\n' "$1" "$2" "$3"; }
hint "boot:" "./run.sh" "a menu of everything built (--list prints it), or directly:"
hint "" "./run.sh --arch $arch" "the busybox shell with tcc; /init is a C script tcc runs ('exit' ends QEMU)"
hint "" "./run.sh --arch $arch --nolibc" "the libc-free init, a static binary: banner, reboot"
case $output in iso|all) hint "" "./run.sh --arch $arch --iso" "the ISO on SeaBIOS (--efi: on OVMF); its default entry is the graphical mode";; esac
hint "" "./run.sh --arch $arch --startx" "the graphical mode: TinyX's X server with the links browser (startx in the shell does the same)"
[ -z "$selfhost" ] || hint "" "./run.sh --arch $arch --selfhost" "the self-hosting image, 1 GB (inside: build-kernel, send-file: C scripts tcc runs)"
[ -z "$run" ] || exec "$ROOT/run.sh" --arch "$arch"
