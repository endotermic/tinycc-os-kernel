#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# recv-file.sh - cut the files that the guest's "send-file" wrote over ttyS1 out of QEMU's
# serial capture (run.sh --selfhost: out/<arch>/selfhost/ttyS1.bin) and verify their md5 sums.
# Every file in the capture is preceded by a line ">>> send-file NAME SIZE MD5".
# Usage: scripts/recv-file.sh [CAPTURE] [OUTDIR]   (defaults: out/<arch>/selfhost/ttyS1.bin and its directory)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/lib.sh"
ARCH=${ARCH:-x86_64}
cap=${1:-$OUT/$ARCH/selfhost/ttyS1.bin}
dir=${2:-$(dirname "$cap")}
[ -s "$cap" ] || die "no capture at $cap (boot with ./run.sh --selfhost and run send-file in the guest)"
mkdir -p "$dir"
n=0
# byte offsets of the headers (grep -b counts bytes with LC_ALL=C); not anchored to a line start:
# the header follows the previous file's last byte, which need not be a newline
LC_ALL=C grep -aboE '>>> send-file [^ ]+ [0-9]+ [0-9a-f]{32}$' "$cap" | while IFS=: read -r off line; do
    set -- $line; name=$3 size=$4 sum=$5
    hdr=$(( ${#line} + 1 ))
    start=$(( off + hdr ))
    out=$dir/$name
    tail -c +"$((start + 1))" "$cap" | head -c "$size" > "$out"
    got=$(md5sum "$out" | cut -d' ' -f1)
    if [ "$got" = "$sum" ] && [ "$(wc -c < "$out")" = "$size" ]; then
        echo "   $out: $size bytes, md5 ok"
    else
        echo "   $out: BAD ($(wc -c < "$out") of $size bytes, md5 $got, expected $sum)" >&2; exit 1
    fi
done
