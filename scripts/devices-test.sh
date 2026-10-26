#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# devices-test.sh - boot the busybox image with the emulated devices and run the guest's
# device checks (userland/devices-test.c: PCI, screen, Ethernet with DHCP, ICMP and HTTP to
# the host, IPv6, Wi-Fi, Bluetooth, sound, USB, USB storage, mouse) unattended.
# Usage: scripts/devices-test.sh [--arch ARCH]
#   The host side: a web server on 127.0.0.1 (python3 http.server) that the guest reaches as
#   10.0.2.2 through QEMU's user-mode network, and mouse motion sent through QMP while the
#   guest waits for it.  The console goes to build/<arch>/devtest/console.log; the "devtest:"
#   lines are printed, the exit status is the number of failed checks (or 1 when the guest
#   did not finish).
# Env: ARCH (x86_64, i386; --arch does the same), RES (as run.sh; the screen check must show
#      it), HTTP_PORT (18080), TIMEOUT (seconds for the guest, default 240), MEM/SMP as in run.sh
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/lib.sh"
while [ $# -gt 0 ]; do case $1 in --arch) ARCH=$2; shift 2;; --arch=*) ARCH=${1#--arch=}; shift;; -h|--help) usage;; *) usage 2;; esac; done
ARCH=${ARCH:-x86_64}
case $ARCH in x86_64|i386) ;; *) die "--arch $ARCH: x86_64 or i386";; esac
need_reset; need_qemu "$ARCH"; need_tool python3 socat; need_done
O=$OUT/$ARCH; D=$BUILD/$ARCH/devtest; mkdir -p "$D/www"
[ -f "$O/bzImage" ] && [ -f "$O/initramfs.cpio" ] || die "no $O/bzImage + initramfs.cpio: ./install.sh --arch $ARCH first"
RES=${RES-1024x768}; HTTP_PORT=${HTTP_PORT:-18080}
echo "tinycc-os-kernel devices-test" > "$D/www/index.txt"
# the web server the guest fetches from (10.0.2.2:PORT reaches the host's 127.0.0.1:PORT)
python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1 --directory "$D/www" >"$D/http.log" 2>&1 &
http_pid=$!
qmp=$D/qmp.sock; rm -f "$qmp"
cleanup() { kill $http_pid $mouse_pid 2>/dev/null; rm -f "$qmp"; }
trap cleanup EXIT INT TERM
# mouse motion through QMP every half second, as long as QEMU has the socket open
( sleep 5
  while [ -S "$qmp" ]; do
      printf '{"execute":"qmp_capabilities"}\n{"execute":"input-send-event","arguments":{"events":[{"type":"rel","data":{"axis":"x","value":5}},{"type":"rel","data":{"axis":"y","value":-3}}]}}\n' \
          | socat -t 1 - UNIX-CONNECT:"$qmp" >/dev/null 2>&1
      sleep 0.5
  done ) &
mouse_pid=$!
msg "devices-test: $ARCH, RES=$RES, HTTP on 127.0.0.1:$HTTP_PORT (log: $D/console.log)"
# the guest: a fresh USB stick image, the test mode on the command line, QMP for the mouse
rm -f "$D/usbstick.img"
APPEND="tcc.devtest=http:$HTTP_PORT" USBSTICK=$D/usbstick.img TIMEOUT=${TIMEOUT:-240} RES=$RES \
    "$ROOT/run.sh" --arch "$ARCH" --no-vga -- -qmp unix:"$qmp",server,nowait </dev/null 2>&1 | tr -d '\r' > "$D/console.log"
grep -E '^devtest:|^\*\*\* devices-test' "$D/console.log" | grep -v 'waiting up to'
failed=$(sed -n 's/^devtest: done: .* checks, \([0-9]*\) failed.*/\1/p' "$D/console.log")
[ -n "$failed" ] || { echo "error: the guest did not finish the checks (see $D/console.log)" >&2; exit 1; }
if ! grep -q "^devtest: video: ok framebuffer $RES " "$D/console.log"; then
    echo "error: the screen is not at RES=$RES" >&2; failed=$((failed + 1))
fi
[ "$failed" = 0 ] && msg "devices-test: every check passed" || echo "error: $failed check(s) failed" >&2
exit "$failed"
