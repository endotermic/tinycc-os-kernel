#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# install.sh - fetch the pinned sources, apply the patches, build and put the results in
# out/<arch>/.  Without arguments on a terminal it asks what to build, the target and the
# output; the same choices are available as options for unattended use.
#
# Usage: ./install.sh [--build tcc|distro] [--arch ARCH] [--output initrd|iso|all] [--selfhost] [--jobs N] [--run] [--test]
#   --build   tcc: only the patched Tiny C Compiler, installed into a staging tree and packed
#                  as out/<arch>/tcc-<version>-<arch>-linux.tar.gz (extract with tar xzf ... -C /)
#             distro: the kernel and the userland built with that compiler (default)
#   --arch    x86_64 (default; the only working target, see the table below), i386, arm64
#   --output  initrd: kernel + initramfs images (default); iso: also the bootable ISO; all
#   --selfhost  also the self-hosting image (initramfs-selfhost.cpio, and its ISO menu entries):
#             the sources and the toolchain to rebuild the kernel and tcc inside the guest
#             (./run.sh --selfhost; scripts/selfhost-test.sh); downloads GNU make's tarball too
#   --jobs    parallel jobs (default: nproc)
#   --test    run tinycc's test suite after building it
#   --run     boot the result in QEMU when done (./run.sh; distro only)
#
# Target status:
#   x86_64  works: the tcc-built kernel boots the tcc-built busybox and tcc in QEMU
#   i386    not implemented: needs the i386-tcc cross compiler, a 32-bit kernel fragment and
#           32-bit versions of the instruction-table and inline-asm fixes (all x86_64 so far)
#   arm64   not available: tcc's arm64 backend has no inline assembler good enough for the
#           kernel; nothing has been attempted (also needs aarch64 binutils and qemu-system-aarch64)
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
. "$ROOT/scripts/lib.sh"
build= arch= output= run= test= selfhost= jobs=${JOBS:-$(nproc)}
usage() { sed -n '2,/^set -e/p' "$0" | sed '$d' | cut -c3-; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
    case $1 in
        --build) build=$2; shift 2;; --build=*) build=${1#*=}; shift;;
        --arch) arch=$2; shift 2;; --arch=*) arch=${1#*=}; shift;;
        --output) output=$2; shift 2;; --output=*) output=${1#*=}; shift;;
        --jobs) jobs=$2; shift 2;; --jobs=*) jobs=${1#*=}; shift;;
        --run) run=1; shift;; --test) test=1; shift;; --selfhost) selfhost=1; shift;;
        -h|--help) usage;;
        *) echo "unknown option: $1" >&2; usage 2;;
    esac
done
ask() { # ask VAR "prompt" default choices...
    v=$1; p=$2; d=$3; shift 3
    echo "$p"; i=0; for c; do i=$((i + 1)); echo "  $i) $c"; done
    printf 'choice [%s]: ' "$d"; read -r a </dev/tty
    [ -n "$a" ] || a=$d
    case $a in [1-9]) eval "set -- \$@"; eval "a=\${$a}";; esac
    eval "$v=\$a"
}
if [ -t 0 ] && [ -t 1 ]; then
    [ -n "$build" ] || ask build "What to build:" distro "tcc: only the patched Tiny C Compiler, as a tar.gz archive" "distro: the kernel and userland built with it"
    build=${build%%:*}
    if [ "$build" = distro ]; then
        [ -n "$arch" ] || ask arch "Target architecture:" x86_64 "x86_64 (works)" "i386 (not implemented)" "arm64 (not available)"
        arch=${arch%% *}
        [ -n "$output" ] || ask output "Output:" initrd "initrd: kernel + initramfs images (boot with ./run.sh)" "iso: the same plus a bootable BIOS/UEFI ISO"
        output=${output%%:*}
    fi
fi
build=${build:-distro}; arch=${arch:-x86_64}; output=${output:-initrd}
case $build in tcc|distro) ;; *) die "unknown build '$build' (tcc, distro)";; esac
case $arch in
    x86_64) ;;
    i386|arm64) sed -n "/^#   $arch /,/^#   [a-z]\|^set -e/p" "$0" | sed '$d' | cut -c3- >&2; die "arch $arch is not supported yet";;
    *) die "unknown arch '$arch' (x86_64, i386, arm64)";;
esac
case $output in initrd|iso|all) ;; *) die "unknown output '$output' (initrd, iso, all)";; esac
for t in git make gcc; do have $t || die "$t is required"; done
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
for t in flex bison bc cpio rsync; do have $t || die "$t is required"; done
msg "tinycc-os-kernel: distro, arch $arch, output $output${selfhost:+ + self-hosting image}, $jobs jobs"
"$ROOT/scripts/fetch.sh"
"$ROOT/scripts/patch.sh"
"$ROOT/scripts/build.sh" host-tcc
[ -z "$test" ] || "$ROOT/scripts/build.sh" tcc-test
"$ROOT/scripts/build.sh" tld kernel musl busybox guest-tcc initramfs
[ -z "$selfhost" ] || "$ROOT/scripts/build.sh" selfhost
case $output in iso|all) "$ROOT/scripts/build.sh" iso;; esac
echo
msg "done: $OUT/$arch"
ls -l "$OUT/$arch" | awk 'NR > 1 {print "   " $5 "\t" $9}'
echo "   boot:  ./run.sh            (busybox shell with tcc; 'exit' ends QEMU)"
echo "          ./run.sh --nolibc   (libc-free init: banner, reboot)"
case $output in iso|all) echo "          ./run.sh --iso      (the ISO on SeaBIOS; --efi for OVMF)";; esac
[ -z "$selfhost" ] || echo "          ./run.sh --selfhost (1 GB; inside: build-kernel, send-file; or scripts/selfhost-test.sh)"
[ -z "$run" ] || exec "$ROOT/run.sh"
