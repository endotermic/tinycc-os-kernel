#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# patch.sh - apply patches/<component>/series to src/<component> with git am, on a branch
# 'patched' created from 'base'.  Idempotent: a component whose 'patched' branch was made
# from the current series files is left alone.  --reset re-creates 'patched' from 'base'.
# Usage: scripts/patch.sh [--reset] [component...]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/lib.sh"
case ${1:-} in -h|--help) usage;; esac
reset=
[ "$1" = --reset ] && { reset=1; shift; }
[ $# -gt 0 ] || set -- $(lock_names)
for name; do
    series=$ROOT/patches/$name/series
    [ -f "$series" ] || { msg "$name: no patches"; continue; }
    dir=$SRC/$name
    [ -d "$dir/.git" ] || die "src/$name: not fetched (scripts/fetch.sh $name)"
    files=$(series_files "$series")
    # fingerprint of the series without the "From <commit>" lines (all zeros since --zero-commit, but older exports had real hashes)
    sum=$(cd "$ROOT/patches/$name" && cat $files | grep -v '^From [0-9a-f]\{40\} ' | sha256sum | cut -c1-16)
    stamp=$dir/.git/tcc-port-series
    if [ -z "$reset" ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$sum" ] \
       && git -C "$dir" rev-parse -q --verify patched >/dev/null; then
        git -C "$dir" checkout -q patched
        msg "$name: $(echo "$files" | wc -l) patches already applied"
        continue
    fi
    git -C "$dir" diff --quiet && git -C "$dir" diff --cached --quiet \
        || die "src/$name has uncommitted changes; commit, stash or discard them first"
    msg "$name: applying $(echo "$files" | wc -l) patches"
    rm -f "$stamp"
    git -C "$dir" checkout -q -B patched base
    if ! (cd "$ROOT/patches/$name" && git -C "$dir" am -q --whitespace=nowarn $(for f in $files; do printf '%s/%s ' "$ROOT/patches/$name" "$f"; done)); then
        echo "   failed patch:" >&2
        git -C "$dir" am --show-current-patch=raw 2>/dev/null | sed -n '1,/^$/p' | grep -E '^Subject' >&2 || true
        git -C "$dir" am --abort 2>/dev/null || true
        die "src/$name: git am failed (tree left on 'patched' at the last good patch)"
    fi
    echo "$sum" > "$stamp"
    echo "   $(git -C "$dir" rev-list --count base..patched) commits on 'patched'"
done
