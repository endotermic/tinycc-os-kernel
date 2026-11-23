#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# tests/netsurf/browse.sh: opens the built NetSurf on the host, in an X window, for trying pages by hand.
#
#   tests/netsurf/browse.sh [--arch x86_64|i386] [--binary PATH] [-w W -h H] [URL]
#
# The browser is build/<arch>/netsurf/stage/usr/bin/netsurf-fb (scripts/build.sh --arch <arch>
# jwm netsurf), the framebuffer front end on libnsfb's X surface (-f x): on a Wayland desktop it
# runs through XWayland ($DISPLAY must be set).  Without a URL it opens the system start page
# (DuckDuckGo's HTML search, userland/netsurf/Choices).
# As in tests/netsurf/run.sh, the resources and the DejaVu fonts come from the build stages
# through $NETSURFRES, and the LibreSSL in the browser, which reads /etc/ssl/openssl.cnf and aborts
# on the host's OpenSSL 3 file, runs in a private mount namespace (unshare -rm, no root) where
# /etc/ssl holds only a copy of the host's CA bundle.
# HOME is build/<arch>/netsurf/browse/profile: the profile (~/.netsurf: cookies, history, the
# Choices the browser saves) is kept there between runs and the host's own is never read.
# The browser's log goes to build/<arch>/netsurf/browse/netsurf.log; the script returns when the
# window is closed (or on Ctrl-C).
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
arch=x86_64
binary=
width=1200
height=850
url=
while [ $# -gt 0 ]; do
    case $1 in
        --arch) arch=$2; shift 2 ;;
        --binary) binary=$2; shift 2 ;;
        -w) width=$2; shift 2 ;;
        -h) height=$2; shift 2 ;;
        --help) sed -n '3,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "browse.sh: unknown option $1" >&2; exit 2 ;;
        *) url=$1; shift ;;
    esac
done
stage=$repo/build/$arch/netsurf/stage
[ -n "$binary" ] || binary=$stage/usr/bin/netsurf-fb
fonts=$repo/build/$arch/jwm/stage/usr/share/fonts/TTF
for need in "$binary" "$stage/usr/share/netsurf/Choices" "$fonts/DejaVuSans.ttf"; do
    if [ ! -e "$need" ]; then
        echo "browse.sh: $need is missing (scripts/build.sh --arch $arch jwm netsurf)" >&2
        exit 2
    fi
done
if [ -z "${DISPLAY:-}" ]; then
    echo "browse.sh: no X display (\$DISPLAY is not set)" >&2
    exit 2
fi

dir=$repo/build/$arch/netsurf/browse
res=$dir/res
mkdir -p "$res" "$dir/profile" "$dir/ssl"
rm -f "$res"/*
ln -s "$stage"/usr/share/netsurf/* "$res"/
ln -s "$fonts"/*.ttf "$res"/

ssl=()
if [ -e /etc/ssl/openssl.cnf ]; then
    for ca in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem; do
        if [ -f "$ca" ]; then cp "$ca" "$dir/ssl/cert.pem"; break; fi
    done
    if ! unshare -rm sh -c "mount --bind '$dir/ssl' /etc/ssl" 2>/dev/null; then
        echo "browse.sh: unshare -rm failed (unprivileged user namespaces off?); LibreSSL needs /etc/ssl without the host's openssl.cnf" >&2
        exit 2
    fi
    ssl=(unshare -rm sh -c 'mount --bind "$0" /etc/ssl && exec "$@"' "$dir/ssl")
fi

echo "NetSurf ($arch, $binary) ${url:-(start page)}"
echo "log: $dir/netsurf.log"
# XAUTHORITY usually names the X cookie file; without it Xlib looks in HOME, which is changed
if [ -z "${XAUTHORITY:-}" ] && [ -f "$HOME/.Xauthority" ]; then
    export XAUTHORITY=$HOME/.Xauthority
fi
NETSURFRES=$res HOME=$dir/profile "${ssl[@]}" "$binary" -f x -w "$width" -h "$height" \
    ${url:+"$url"} > "$dir/netsurf.log" 2>&1
status=$?
[ $status -eq 0 ] || echo "browse.sh: NetSurf exited with status $status (log $dir/netsurf.log)" >&2
exit $status
