#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Print gen_init_cpio lines for every directory, file and symlink under a staging
# tree, so that a whole tree can be appended to a cpio manifest.  Files are root's, 0644,
# 0755 when executable, 4755 when setuid in the tree (the X server).
# Usage: userland/dir2list.sh STAGE_DIR    (paths in the cpio are relative to STAGE_DIR)
set -e
case ${1:-} in -h|--help|'') sed -n '3,/^[^#]/p' "$0" | sed '$d' | cut -c3-; [ "${1:-}" ] && exit 0 || exit 2;; esac
root=$(cd "$1" && pwd)
cd "$root"
find . -mindepth 1 -type d | sort | sed 's|^\.||' | while IFS= read -r d; do
    printf 'dir   %s 0755 0 0\n' "$d"
done
find . -type f | sort | sed 's|^\.||' | while IFS= read -r f; do
    if [ -u "$root$f" ]; then m=4755; elif [ -x "$root$f" ]; then m=0755; else m=0644; fi
    printf 'file  %s %s %s 0 0\n' "$f" "$root$f" "$m"
done
find . -type l | sort | sed 's|^\.||' | while IFS= read -r l; do
    printf 'slink %s %s 0777 0 0\n' "$l" "$(readlink "$root$l")"
done
