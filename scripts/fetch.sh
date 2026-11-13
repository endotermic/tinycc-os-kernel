#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# fetch.sh - clone every component of sources.lock (or the ones named) at exactly the
# pinned commit into src/<component>, as a shallow checkout on a branch called 'base'.
# A component that already has the commit is left alone (its 'patched' branch too).
# A component pinned as a release tarball (sha256:<digest> in sources.lock; GNU make, whose git
# tree needs autotools and gnulib) is downloaded (curl or wget) into src/.downloads/, verified,
# unpacked into src/<component> and committed there as branch 'base'.
# A component with a "#sparse NAME DIRECTORY" line (the wallpapers: one directory of a 2 GB
# repository) is fetched without file contents (a partial clone, --filter=blob:none, from the
# remote 'origin', which later fetches the contents it needs) and checks out that directory alone.
# Usage: scripts/fetch.sh [component...]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/lib.sh"
case ${1:-} in -h|--help) usage;; esac
need_reset; need_tool git tar sha256sum:coreutils; need_download_tool; need_done
[ $# -gt 0 ] || set -- $(lock_names)
mkdir -p "$SRC"

# fetch_tarball NAME SHA256 VERSION: download NAME-VERSION.tar.gz (.tar.xz or .tar.bz2 when
# the URLs end so: jwm, fontconfig and the DejaVu fonts publish no gzip tarball), check it,
# unpack it into src/NAME and make that a git repository with one commit on branch 'base'
# (patch.sh and build.sh treat every component alike).  The tarball is kept in
# src/.downloads/ (the self-hosting image ships it).  A URL that names no tarball (the
# Unifont PCF file: a single compressed font file) is downloaded and checked the same way
# and committed as the one file src/NAME/<its basename>.
fetch_tarball() {
    name=$1 sum=$2 ver=$3
    single=
    case $(lock_urls "$name") in
        *.tar.xz*) ext=tar.xz; have xz || die "$name: xz is needed to unpack a .tar.xz tarball (package xz-utils)";;
        *.tar.bz2*) ext=tar.bz2; have bzip2 || die "$name: bzip2 is needed to unpack a .tar.bz2 tarball (package bzip2)";;
        *.tar.gz*|*.tgz*) ext=tar.gz;;
        *) single=$(basename "$(lock_urls "$name" | cut -d' ' -f1)");;
    esac
    dir=$SRC/$name; dl=$SRC/.downloads; file=${single:-$name-$ver.$ext}; tb=$dl/$file
    stamp=$dir/.git/tcc-port-tarball
    if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$sum" ] && [ -f "$tb" ]; then
        msg "$name: already unpacked from $file ($(git -C "$dir" rev-parse --abbrev-ref HEAD))"
        return
    fi
    mkdir -p "$dl"
    if ! [ -f "$tb" ] || [ "$(sha256sum "$tb" | cut -d' ' -f1)" != "$sum" ]; then
        msg "$name: downloading $file"
        got=
        for url in $(lock_urls "$name"); do
            rm -f "$tb.part"
            if have curl; then curl -fL $(pb_tty && echo --progress-bar || echo -sS) -o "$tb.part" "$url" 2>&1 | tr '\r' '\n' | tail -1 >&2 || true
            elif have wget; then wget -q -O "$tb.part" "$url" || true
            else die "neither curl nor wget found (needed to download $url)"; fi
            if [ -f "$tb.part" ] && [ "$(sha256sum "$tb.part" | cut -d' ' -f1)" = "$sum" ]; then
                mv "$tb.part" "$tb"; got=$url; break
            fi
            echo "   $url: download failed or checksum mismatch, trying the next URL" >&2
        done
        [ -n "$got" ] || die "$name: could not download a tarball with sha256 $sum from any URL"
        echo "   $(ls -l "$tb" | awk '{print $5}') bytes from $got, sha256 verified"
    fi
    msg "$name: $([ -n "$single" ] && echo copying || echo unpacking) $file into src/$name"
    rm -rf "$dir"; mkdir -p "$dir"
    if [ -n "$single" ]; then cp "$tb" "$dir/$single"; else tar xf "$tb" -C "$dir" --strip-components=1; fi
    git -C "$dir" init -q
    git -C "$dir" add -A
    git -C "$dir" -c user.name=tinycc-os-kernel -c user.email=fetch@tinycc-os-kernel \
        commit -q -m "$name $ver (release $([ -n "$single" ] && echo file || echo tarball), sha256 $sum)"
    git -C "$dir" checkout -q -B base
    echo "$sum" > "$stamp"
    echo "   $(git -C "$dir" ls-files | wc -l) files, committed as branch 'base'"
}
for name; do
    commit=$(lock_commit "$name"); ref=$(lock_ref "$name")
    [ -n "$commit" ] || die "$name: not in sources.lock"
    dir=$SRC/$name
    case $commit in sha256:*) fetch_tarball "$name" "${commit#sha256:}" "$ref"; continue;; esac
    if [ -d "$dir/.git" ] && git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null; then
        msg "$name: already at $commit ($(git -C "$dir" rev-parse --abbrev-ref HEAD))"
        continue
    fi
    sparse=$(lock_sparse "$name")
    msg "$name: fetching $commit ($ref)${sparse:+, the directory \"$sparse\" alone}"
    mkdir -p "$dir"
    [ -d "$dir/.git" ] || git -C "$dir" init -q
    got=
    FETCH_ERR=$dir/.git/fetch-error.log; : >"$FETCH_ERR"
    for url in $(lock_urls "$name"); do
        if [ -n "$sparse" ]; then
            # the commit and its trees, no blobs; checkout fetches the blobs of the directory
            git -C "$dir" remote remove origin 2>/dev/null || true
            git -C "$dir" remote add origin "$url"
            git -C "$dir" config remote.origin.promisor true
            git -C "$dir" config remote.origin.partialclonefilter blob:none
            git -C "$dir" sparse-checkout set --no-cone "/$sparse/"
            if git_progress "$name" git -C "$dir" fetch --depth 1 --filter=blob:none origin "$commit"; then
                got=$url; break
            fi
            echo "   $url: cannot fetch $commit, trying the next URL" >&2
            continue
        fi
        # 1. the commit itself (GitHub, repo.or.cz and cgit >= 1.2 allow this),
        # 2. the tag/branch it came from, accepted only if it still is that commit
        if git_progress "$name" git -C "$dir" fetch --depth 1 "$url" "$commit"; then
            got=$url; break
        fi
        for r in "refs/tags/$ref" "$ref"; do
            if git_progress "$name" git -C "$dir" fetch --depth 1 "$url" "$r" \
               && [ "$(git -C "$dir" rev-parse FETCH_HEAD^{commit})" = "$commit" ]; then
                got=$url; break 2
            fi
        done
        echo "   $url: cannot fetch $commit, trying the next URL" >&2
    done
    [ -n "$got" ] || { tail -n 5 "$FETCH_ERR" >&2; die "$name: could not fetch commit $commit from any URL"; }
    git -C "$dir" -c advice.detachedHead=false checkout -q -B base "$commit"
    echo "   $(git -C "$dir" log -1 --format='%h %ad %s' --date=short) from $got"
done
