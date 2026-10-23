#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# selfhost-test.sh - exercise the self-hosting image (scripts/build.sh selfhost).
# Usage: scripts/selfhost-test.sh host|qemu|boot|all
#   host   unpack the pruned kernel tree from the image's staging area and build it on the host
#          with the host tcc and the image's own tld, make, wrappers and flex/bison stand-ins
#          (userland/selfhost/build-kernel with WORK=build/<arch>/selfhost-test): proves the
#          pruned tree and the scripts in a minute, before the slow run inside the guest
#   qemu   boot the image with ./run.sh --selfhost --auto (the guest runs build-kernel and
#          send-file, then shuts down) and receive the bzImage into out/<arch>/selfhost/
#          (scripts/recv-file.sh); the console goes to out/<arch>/selfhost/console.log too
#   boot   boot the received bzImage (./run.sh --nolibc) and show its build line: "#2" is the
#          second generation, a kernel built inside the first
#   all    qemu, then boot
# Env: ARCH (x86_64, i386), JOBS, AUTO (auto: build-kernel only, the default; all: build-toolchain first
#      and build the kernel with the guest-built tcc and tld), TIMEOUT (seconds for the guest run,
#      default 7200), MEM/SMP as in run.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/lib.sh"
ARCH=${ARCH:-x86_64}; B=$BUILD/$ARCH; O=$OUT/$ARCH; export ARCH    # run.sh reads ARCH too
stage=$B/selfhost/stage
mode=${1:-all}
test_host() {
    [ -d "$stage/usr/src" ] || die "no $stage: run scripts/build.sh selfhost first"
    [ -x "$SRC/tinycc/tcc" ] && [ -x "$SRC/tinycc/i386-tcc" ] || die "no host tcc/i386-tcc: scripts/build.sh host-tcc"
    W=$B/selfhost-test
    msg "host check: build-kernel on the pruned tree with the image's toolchain (host tcc, ARCH=$ARCH) -> $W"
    rm -rf "$W"
    # the i386 kernel is compiled by the host's i386-tcc cross compiler (KCC_TCC, as in
    # scripts/build.sh); inside the i386 guest tcc itself is that compiler
    kcc_tcc=; [ "$ARCH" != i386 ] || kcc_tcc=$SRC/tinycc/i386-tcc
    env WORK="$W" NO_TMPFS=1 ARCH="$ARCH" TCC="$SRC/tinycc/tcc" ${kcc_tcc:+KCC_TCC="$kcc_tcc"} SELFHOST_SRC="$stage/usr/src" \
        SELFHOST_SHARE="$stage/usr/share/tinycc-os-kernel" SELFHOST_BIN="$stage/usr/bin" \
        GEN=1 KBUILD_BUILD_HOST=selfhost-test JOBS="${JOBS:-$(nproc)}" \
        sh "$ROOT/userland/selfhost/build-kernel" || die "the host check failed"
    echo "   reference: $(ls -l "$O/bzImage" | awk '{print $5}') bytes ($O/bzImage), pruned tree: $(ls -l "$W/linux/arch/x86/boot/bzImage" | awk '{print $5}') bytes"
}
test_qemu() {
    [ -f "$O/initramfs-selfhost.cpio" ] || die "no $O/initramfs-selfhost.cpio: scripts/build.sh selfhost"
    mkdir -p "$O/selfhost"
    msg "guest run: ./run.sh --selfhost --auto=${AUTO:-auto} (console: $O/selfhost/console.log)"
    t0=$(date +%s)
    TIMEOUT=${TIMEOUT:-7200} "$ROOT/run.sh" --selfhost --auto="${AUTO:-auto}" 2>&1 | tee "$O/selfhost/console.log" | grep -E '^\*\*\*|^== |^   |^send-file|Kernel panic|Out of memory|Killed' || true
    echo "   guest run took $(( $(date +%s) - t0 )) s"
    grep -q 'self-hosting run finished with status 0' "$O/selfhost/console.log" || die "the guest did not finish successfully (see $O/selfhost/console.log)"
    "$ROOT/scripts/recv-file.sh" "$O/selfhost/ttyS1.bin" "$O/selfhost"
    [ -f "$O/selfhost/bzImage" ] || die "no bzImage received"
}
test_boot() {
    [ -f "$O/selfhost/bzImage" ] || die "no $O/selfhost/bzImage: run the qemu step first"
    msg "booting the guest-built kernel: KERNEL=$O/selfhost/bzImage ./run.sh --nolibc"
    out=$(KERNEL="$O/selfhost/bzImage" TIMEOUT=60 "$ROOT/run.sh" --nolibc 2>&1 | tr -d '\r')
    echo "$out" | grep -E '^Linux version|^kernel:|^build:' | sed 's/^/   /'
    gen=$(echo "$out" | sed -n 's/^build:  *#\([0-9]*\).*/\1/p')
    [ -n "$gen" ] || die "the guest-built kernel did not boot to the banner"
    [ "$gen" -ge 2 ] || die "the kernel booted but is generation $gen, not a guest build"
    echo "   generation $gen: a kernel built inside the guest boots"
}
case $mode in
    host) test_host;;
    qemu) test_qemu;;
    boot) test_boot;;
    all) test_qemu; test_boot;;
    *) die "usage: $0 host|qemu|boot|all";;
esac
