#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# export-patches.sh - regenerate patches/<component>/ from the commits base..patched of
# src/<component>, so that the workflow is: fix in src/<component>, commit, export.
# Each commit keeps the category directory of the series entry at the same position;
# commits beyond the current series go to --category (default: unsorted).
# Usage: scripts/export-patches.sh [--category DIR] <component>
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/lib.sh"
cat_new=unsorted
[ "$1" = --category ] && { cat_new=$2; shift 2; }
name=$1; [ -n "$name" ] || die "usage: $0 [--category DIR] <component>"
dir=$SRC/$name; pdir=$ROOT/patches/$name
git -C "$dir" rev-parse -q --verify patched >/dev/null || die "src/$name has no 'patched' branch"
tmp=$(mktemp -d)
git -C "$dir" format-patch -q -N --no-signature -o "$tmp" base..patched
old=$([ -f "$pdir/series" ] && series_files "$pdir/series" || true)
mkdir -p "$pdir"
i=0; new_series=
for p in "$tmp"/*.patch; do
    i=$((i + 1))
    entry=$(echo "$old" | sed -n "${i}p")
    cat=${entry%%/*}; [ -n "$entry" ] || cat=$cat_new
    [ "$cat" != "$entry" ] || cat=$cat_new   # entry without a directory
    f=$cat/$(printf '%04d' $i)-$(basename "$p" | sed 's/^[0-9]*-//')
    mkdir -p "$pdir/$cat"
    cp "$p" "$pdir/$f"
    new_series="$new_series$f
"
done
# drop files of the old series that were renumbered or removed
for e in $old; do
    case "$new_series" in *"$e"*) ;; *) rm -f "$pdir/$e";; esac
done
{ echo "# patches/$name: apply order (scripts/patch.sh); regenerate with scripts/export-patches.sh"
  printf '%s' "$new_series"; } > "$pdir/series"
rm -rf "$tmp"
msg "$name: $i patches written to patches/$name/"
