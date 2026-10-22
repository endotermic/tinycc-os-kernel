#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Print gen_init_cpio lines for every directory, file and symlink under a staging
# tree, so that a whole tree can be appended to a cpio manifest.
# Usage: ./dir2list.sh STAGE_DIR    (paths in the cpio are relative to STAGE_DIR)
set -e
root=$(cd "$1" && pwd)
cd "$root"
find . -mindepth 1 -type d | sort | sed 's|^\.||' | while IFS= read -r d; do
    printf 'dir   %s 0755 0 0\n' "$d"
done
find . -type f | sort | sed 's|^\.||' | while IFS= read -r f; do
    if [ -x "$root$f" ]; then m=0755; else m=0644; fi
    printf 'file  %s %s %s 0 0\n' "$f" "$root$f" "$m"
done
find . -type l | sort | sed 's|^\.||' | while IFS= read -r l; do
    printf 'slink %s %s 0777 0 0\n' "$l" "$(readlink "$root$l")"
done
