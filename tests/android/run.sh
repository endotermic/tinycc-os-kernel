#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# tests/android/run.sh: boots the opt-in Android kernel (scripts/build.sh kernel-android ->
# out/<arch>/bzImage-android) with tests/android/probe.c as the whole initramfs and reports
# which of the features an Android container needs work.
#
#   tests/android/run.sh [--arch x86_64|i386]
#
# The probe is compiled static by build/<arch>/musl-tcc (scripts/build.sh musl kernel-headers;
# the binder and binderfs UAPI headers come from build/<arch>/kernel-headers), packed as /init
# into build/<arch>/android-test/probe.cpio.gz and booted headless in QEMU (KVM when there is
# one); it prints one "probe: NAME ok|FAIL" line per check and powers off.  The run is capped:
# TIMEOUT seconds (default 120), MEM of guest RAM (default 512M), the QEMU process at 1200 MB
# with a systemd-run scope where one is available, and the console log at 300 kB
# (build/<arch>/android-test/console.log).
# Exit status: the number of failed checks; 1 when the probe did not finish.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$ROOT/scripts/lib.sh"
while [ $# -gt 0 ]; do
    case $1 in
        --arch) ARCH=$2; shift 2;;
        --arch=*) ARCH=${1#--arch=}; shift;;
        -h|--help) sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
        *) die "unknown argument '$1' (see the header of $0)";;
    esac
done
ARCH=${ARCH:-x86_64}
case $ARCH in x86_64) QEMU=qemu-system-x86_64;; i386) QEMU=qemu-system-i386;; *) die "--arch $ARCH: x86_64 or i386";; esac
need_reset; need_qemu "$ARCH"; need_tool cpio gzip; need_done
B=$BUILD/$ARCH; K=$OUT/$ARCH/bzImage-android; D=$B/android-test
[ -f "$K" ] || die "no $K: scripts/build.sh --arch $ARCH kernel-android first"
[ -x "$B/musl-tcc" ] || die "no $B/musl-tcc: scripts/build.sh --arch $ARCH musl kernel-headers first"
mkdir -p "$D/root"
"$B/musl-tcc" -O2 -static -o "$D/root/init" "$ROOT/tests/android/probe.c" || die "probe build failed"
(cd "$D/root" && echo init | cpio -o -H newc -R 0:0 2>/dev/null) | gzip -9 > "$D/probe.cpio.gz"
scope=
if command -v systemd-run >/dev/null 2>&1 && systemd-run --user --scope -q true 2>/dev/null; then
    scope="systemd-run --user --scope -q -p MemoryMax=1200M --"
fi
msg "android-test: $ARCH, $(basename "$K") $(ls -l "$K" | awk '{print $5}') bytes (log: $D/console.log)"
$scope timeout "${TIMEOUT:-120}" $QEMU -name androidprobe -accel kvm -accel tcg -cpu max -smp 2 -m "${MEM:-512M}" \
    -kernel "$K" -initrd "$D/probe.cpio.gz" -append "console=ttyS0 panic=-1 quiet" \
    -nographic -no-reboot </dev/null 2>&1 | tr -d '\r' | head -c 300000 > "$D/console.log"
grep -E '^probe: ' "$D/console.log"
failed=$(sed -n 's/^probe: done: [0-9]* ok, \([0-9]*\) failed.*/\1/p' "$D/console.log")
[ -n "$failed" ] || { echo "error: the probe did not finish (see $D/console.log)" >&2; exit 1; }
[ "$failed" = 0 ] && msg "android-test: every check passed" || echo "error: $failed check(s) failed" >&2
exit "$failed"
