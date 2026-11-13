#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# install.sh - write a built ISO (out/<arch>/*.iso) to a USB flash drive.  The whole drive is
# overwritten byte for byte by dd (the ISO is a hybrid image: its own partition table, BIOS and
# UEFI boot), with a progress bar drawn from dd's status=progress, then read back and compared.
# Without options on a terminal it asks for the ISO and the drive; the same choices are options
# for unattended use.
#
# Usage: ./install.sh [--iso PATH|--arch ARCH] [--device /dev/sdX] [--bs SIZE] [--yes] [--no-verify]
#        ./install.sh --list
#   --iso     the image to write (default: a menu of every out/*/*.iso)
#   --arch    shorthand for --iso out/ARCH/tcc-linux-ARCH.iso (x86_64, i386)
#   --device  the whole drive to write, e.g. /dev/sdb (not a partition such as /dev/sdb1).
#             The menu offers only removable/USB drives; a disk that holds a mounted system
#             filesystem (/, /boot, /home, /usr, swap) is refused in every case
#   --bs      write in blocks of SIZE (e.g. 256K, 1M; a multiple of 512, K/M = 1024-based) and
#             have the drive flush each block before the next (dd oflag=sync): slower, for
#             drives that lose data under long fast writes (the check then reports a stretch
#             that differs).  Default: 4M blocks, one flush at the end
#   --yes     do not ask for the final confirmation (needed when stdin is not a terminal)
#   --no-verify  skip reading the drive back and comparing it with the ISO
#   --list    print the ISOs and the drives that can be chosen, and exit
#   -h, --help  this text
# Writing needs root: the script runs the write through sudo (asked once, before the bar starts).
# Mounted partitions of the chosen drive are unmounted first (after asking).
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
. "$ROOT/scripts/lib.sh"
iso= arch= dev= bs= yes= verify=1 list=
while [ $# -gt 0 ]; do
    case $1 in
        --iso) iso=$2; shift 2;; --iso=*) iso=${1#*=}; shift;;
        --arch) arch=$2; shift 2;; --arch=*) arch=${1#*=}; shift;;
        --device) dev=$2; shift 2;; --device=*) dev=${1#*=}; shift;;
        --bs) bs=$2; shift 2;; --bs=*) bs=${1#*=}; shift;;
        --yes|-y) yes=1; shift;; --no-verify) verify=; shift;; --list) list=1; shift;;
        -h|--help) usage;;
        *) echo "unknown option: $1" >&2; usage 2;;
    esac
done
need_reset; need_tool dd:coreutils stdbuf:coreutils lsblk:util-linux findmnt:util-linux numfmt:coreutils cmp:diffutils
[ "$(id -u)" = 0 ] || need_tool sudo
need_done
SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
tty=; [ -t 0 ] && [ -t 1 ] && tty=1
# the write's dd block size and flags: --bs SIZE adds a flush after every block
wflags=direct wbs=4M
if [ -n "$bs" ]; then
    case ${bs%[KM]} in ''|*[!0-9]*) die "--bs: '$bs' is not a size (whole numbers, e.g. 256K, 1M)";; esac
    bsb=$(numfmt --from=iec "$bs") || die "--bs: '$bs' is not a size"
    [ "$bsb" -ge 512 ] && [ $((bsb % 512)) = 0 ] && [ "$bsb" -le 67108864 ] ||
        die "--bs: $bs is not a multiple of 512 between 512 and 64M"
    wflags=direct,sync wbs=$bsb
fi

human() { numfmt --to=iec-i --suffix=B --format=%.1f "$1"; }
fsize() { stat -c %s "$1"; }

# ---- the ISOs ------------------------------------------------------------------------
isos() { find "$OUT" -mindepth 2 -maxdepth 2 -name '*.iso' -type f 2>/dev/null | sort; }
iso_line() { printf '%s  (%s, built %s)' "${1#$ROOT/}" "$(human "$(fsize "$1")")" "$(date -r "$1" '+%Y-%m-%d %H:%M')"; }

# ---- the drives ----------------------------------------------------------------------
# system_disks: the whole disks under the mounted system filesystems and the active swap
system_disks() {
    { findmnt -rn -o SOURCE -T / ; for m in /boot /boot/efi /home /usr /var; do findmnt -rn -o SOURCE "$m" || true; done
      awk 'NR > 1 { print $1 }' /proc/swaps; } 2>/dev/null | sed 's/\[.*//' | grep '^/dev/' | while read -r s; do
        # walk up to the disk (partitions, LVM, dm-crypt: every ancestor of type disk)
        lsblk -s -n -r -o NAME,TYPE "$s" 2>/dev/null | awk '$2 == "disk" { print "/dev/" $1 }'
    done | sort -u
}
SYSTEM_DISKS=$(system_disks)
is_system() { printf '%s\n' "$SYSTEM_DISKS" | grep -qx "$1"; }
# disk_info DEV: sets NAME TRAN RM HOTPLUG SIZE (bytes) VENDOR MODEL SERIAL TYPE
disk_info() {
    eval "$(lsblk -d -n -b -P -o NAME,TRAN,RM,HOTPLUG,SIZE,VENDOR,MODEL,SERIAL,TYPE "$1")"
}
# removable_disks: the USB / removable whole disks with media, not a system disk
removable_disks() {
    lsblk -d -n -r -o NAME,TYPE | awk '$2 == "disk" { print "/dev/" $1 }' | while read -r d; do
        disk_info "$d"
        [ "$TYPE" = disk ] && [ "${SIZE:-0}" -gt 0 ] || continue
        [ "$TRAN" = usb ] || [ "$RM" = 1 ] || [ "$HOTPLUG" = 1 ] || continue
        is_system "$d" || echo "$d"
    done
}
# disk_line DEV: "sdb  SanDisk Ultra (usb, 28.7GiB, serial 4C53...)"
disk_line() {
    disk_info "$1"
    hw=$(echo "$VENDOR $MODEL" | sed 's/  */ /g; s/^ //; s/ $//'); [ -n "$hw" ] || hw="unknown model"
    printf '%s  %s  (%s, %s%s)' "$1" "$hw" "${TRAN:-${RM:+removable}}" "$(human "$SIZE")" "${SERIAL:+, serial $SERIAL}"
}
# disk_parts DEV: one indented line per partition: name, size, file system, label, mount point
disk_parts() {
    n=0
    lsblk -n -b -P -o NAME,TYPE,SIZE,FSTYPE,LABEL,PARTLABEL,MOUNTPOINT "$1" | while read -r l; do
        NAME= TYPE= SIZE= FSTYPE= LABEL= PARTLABEL= MOUNTPOINT=; eval "$l"
        [ "$TYPE" != disk ] || continue
        lab=${LABEL:-$PARTLABEL}
        printf '        %-10s %9s  %-6s %s%s\n' "$NAME" "$(human "$SIZE")" "${FSTYPE:--}" \
            "${lab:+label \"$lab\"}" "${MOUNTPOINT:+  mounted on $MOUNTPOINT}"
    done
    # a disk with a file system and no partition table (the ISO's own label shows on the disk)
    eval "$(lsblk -d -n -P -o FSTYPE,LABEL "$1" | sed 's/^/D/; s/ LABEL/ DLABEL/')"
    [ -z "$DFSTYPE" ] || printf '        (whole disk) %s%s\n' "$DFSTYPE" "${DLABEL:+  label \"$DLABEL\"}"
    lsblk -n -r -o TYPE "$1" | grep -qv '^disk$' || [ -n "$DFSTYPE" ] || echo "        (no partitions)"
}

# pick VAR PROMPT ITEM...: a numbered menu of the items (each shown by the function $PICK_SHOW,
# with $PICK_MORE printing extra lines under it); the answer is a number, Enter = 1 when there
# is only one item
pick() {
    v=$1; p=$2; shift 2
    while :; do
        echo "$p"; i=0
        for c; do i=$((i + 1)); printf '  %s) %s\n' "$i" "$($PICK_SHOW "$c")"; [ -z "$PICK_MORE" ] || $PICK_MORE "$c"; done
        if [ $# = 1 ]; then printf '  1 [1]: '; else printf '  1-%s: ' "$#"; fi
        read -r a </dev/tty || exit 1
        [ -n "$a" ] || [ $# != 1 ] || a=1
        case $a in
            ''|*[!0-9]*) ;;
            *) if [ "$a" -ge 1 ] && [ "$a" -le $# ]; then eval "$v=\${$a}"; return 0; fi;;
        esac
        echo "  '$a' is not one of the choices" >&2
    done
}

if [ -n "$list" ]; then
    echo "ISO images:"; l=$(isos)
    if [ -n "$l" ]; then for f in $l; do echo "  $(iso_line "$f")"; done; else echo "  none (./build.sh --output iso)"; fi
    echo "USB / removable drives:"; l=$(removable_disks)
    if [ -n "$l" ]; then for d in $l; do echo "  $(disk_line "$d")"; disk_parts "$d"; done; else echo "  none found"; fi
    exit 0
fi

# ---- choose the ISO ------------------------------------------------------------------
[ -z "$arch" ] || [ -z "$iso" ] || die "--iso and --arch exclude each other"
[ -z "$arch" ] || iso=$OUT/$arch/tcc-linux-$arch.iso
if [ -z "$iso" ]; then
    l=$(isos); [ -n "$l" ] || die "no ISO in $OUT/*/ - build one with: ./build.sh --output iso"
    [ -n "$tty" ] || die "no terminal to ask on: pass --iso PATH or --arch ARCH"
    PICK_SHOW=iso_line PICK_MORE= pick iso "ISO image to write:" $l
fi
[ -f "$iso" ] || die "no such ISO: $iso${arch:+ (build it with: ./build.sh --arch $arch --output iso)}"
isz=$(fsize "$iso")

# ---- choose the drive ----------------------------------------------------------------
if [ -z "$dev" ]; then
    [ -n "$tty" ] || die "no terminal to ask on: pass --device /dev/sdX"
    while l=$(removable_disks); [ -z "$l" ]; do
        printf 'No USB / removable drive found. Plug one in and press Enter (Ctrl-C to quit): '
        read -r _ </dev/tty || exit 1; sleep 2
    done
    echo
    PICK_SHOW=disk_line PICK_MORE=disk_parts pick dev "USB drive to overwrite (all of it):" $l
fi
dev=$(readlink -f "$dev")
[ -b "$dev" ] || die "$dev is not a block device"
disk_info "$dev"
case $TYPE in part) TYPE=partition;; loop) TYPE="loop device";; esac
[ "$TYPE" = disk ] || die "$dev is a $TYPE, not a whole drive (give the disk, e.g. /dev/sdb, not /dev/sdb1)"
is_system "$dev" && die "$dev holds a mounted system file system (/, /boot, /home, /usr or swap) - refusing"
[ "$TRAN" = usb ] || [ "$RM" = 1 ] || [ "$HOTPLUG" = 1 ] ||
    echo "warning: $dev is not a USB or removable drive (transport '${TRAN:-?}')" >&2
dsz=$SIZE
[ "$dsz" -ge "$isz" ] || die "$dev ($(human "$dsz")) is smaller than the ISO ($(human "$isz"))"

# ---- confirm -------------------------------------------------------------------------
echo
echo "About to write"
echo "  $(iso_line "$iso")"
echo "to the whole drive"
echo "  $(disk_line "$dev")"
disk_parts "$dev"
echo "Everything on $dev will be lost."
if [ -z "$yes" ]; then
    [ -n "$tty" ] || die "no terminal to confirm on: pass --yes"
    printf 'Type the device name (%s) to go on: ' "${dev#/dev/}"
    read -r a </dev/tty || exit 1
    [ "$a" = "${dev#/dev/}" ] || [ "$a" = "$dev" ] || die "not confirmed, nothing written"
fi

# ---- unmount, write, verify ----------------------------------------------------------
[ -z "$SUDO" ] || $SUDO -v || die "sudo failed, nothing written"
mounted=$(lsblk -n -r -o NAME,MOUNTPOINT "$dev" | awk 'NF >= 2 { print "/dev/" $1 }')
if [ -n "$mounted" ]; then
    echo "Mounted now: $(echo $mounted)"
    if [ -z "$yes" ]; then
        printf 'Unmount them? [Y/n] '; read -r a </dev/tty || exit 1
        case $a in [nN]*) die "drive in use, nothing written";; esac
    fi
    for p in $mounted; do $SUDO umount "$p" || die "cannot unmount $p, nothing written"; done
fi

# dd_bar LABEL TOTAL: draw a bar from dd's status=progress lines on stdin (its "\r"-separated
# "<bytes> bytes (...) copied, <time> s, <rate>"), then a "__status=N" line; dd's other messages
# (errors) pass to stderr.  Returns N.  Plain 10% milestone lines when stderr is no terminal.
dd_bar() {
    st=$(mktemp)
    stdbuf -oL tr '\r' '\n' | $PB_AWK_CMD -v label="$1" -v total="$2" -v st="$st" \
        -v tty="$(pb_tty && echo 1 || echo 0)" "$PB_AWK"'
    function iec(b,   u, i) { split("B KiB MiB GiB TiB", u, " "); for (i = 1; b >= 1024 && i < 5; i++) b /= 1024
                              return sprintf(i == 1 ? "%d%s" : "%.1f%s", b, u[i]) }
    /^__status=/ { status = substr($0, 10); next }
    /^[0-9]+ bytes/ { n = $1; rate = $(NF - 1) " " $NF
                      bar(n, total, sprintf("%s of %s, %s, %s", iec(n), iec(total), rate, elapsed())); next }
    / records (in|out)$/ || /^$/ { next }
    { print > "/dev/stderr" }
    END {
        if (status == "0") finish(sprintf("%s in %s (%s)", iec(n), elapsed(), rate))
        else if (tty) printf("\r\033[K") > "/dev/stderr"
        print status > st
    }'
    s=$(cat "$st"); rm -f "$st"
    return "${s:-1}"
}

# diff_report: the stretches where the drive differs from the ISO (differing bytes less than
# 4 KiB apart are one stretch), at most 8, with their offsets and lengths
diff_report() {
    $SUDO dd if="$dev" bs=4M iflag=direct,count_bytes count="$isz" status=none | cmp -l - "$iso" 2>/dev/null |
        awk 'function iec(b,   u, i) { split("B KiB MiB GiB", u, " "); for (i = 1; b >= 1024 && i < 4; i++) b /= 1024
                                       return sprintf(i == 1 ? "%d%s" : "%.1f%s", b, u[i]) }
             function out() { if (++k <= 8) printf("   differs at byte %d (%s), %s long\n", s, iec(s), iec(e - s)) }
             { o = $1 - 1; if (!n || o - e >= 4096) { if (n) out(); s = o }; e = o + 1; n++ }
             END { if (n) out(); if (k > 8) printf("   ... %d stretches in all\n", k)
                   printf("   %d bytes differ\n", n) }'
}

# the write and the read-back check; when the check fails, the report says where, and a
# terminal user may write again (a fault at the same place again points at the drive)
while :; do
    msg "writing $(human "$isz") to $dev${bs:+ in $bs blocks, each flushed}"
    # the old contents' cached pages go first (the file system that was mounted)
    $SUDO blockdev --flushbufs "$dev" 2>/dev/null || true
    # oflag=direct: the bar follows what reaches the drive, not the page cache; conv=fsync: dd
    # returns only when the drive has everything
    { $SUDO dd if="$iso" of="$dev" bs="$wbs" oflag="$wflags" conv=fsync status=progress 2>&1 >/dev/null
      echo "__status=$?"; } | dd_bar write "$isz" || die "writing $dev failed"
    sync
    [ -n "$verify" ] || break
    msg "verifying $dev"
    $SUDO blockdev --flushbufs "$dev" 2>/dev/null || true
    # dd's progress goes to the bar through fd 3, the data to cmp
    if { { $SUDO dd if="$dev" bs=4M iflag=direct,count_bytes count="$isz" status=progress 2>&3 |
           cmp -s - "$iso"; echo "__status=$?"; } 3>&1; } | dd_bar verify "$isz"; then
        echo "   the drive matches the ISO"; break
    fi
    echo "error: verification failed: $dev differs from $iso" >&2
    diff_report >&2
    [ -n "$tty" ] && [ -z "$yes" ] || exit 1
    printf 'Write again? [Y/n] '; read -r a </dev/tty || exit 1
    case $a in [nN]*) exit 1;; esac
done
# let the kernel read the new partition table (the ISO's) so the desktop shows it
$SUDO blockdev --rereadpt "$dev" 2>/dev/null || true
msg "done: $dev can be removed (or: udisksctl power-off -b $dev)"
