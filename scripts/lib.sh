# SPDX-License-Identifier: BSD-2-Clause
# lib.sh - shared helpers; callers set ROOT (the repository root) before sourcing.
# Directory layout (all overridable from the environment):
#   src/<component>   pinned upstream checkouts (branch 'base' = the lock commit,
#                     branch 'patched' = base + patches/<component>/series via git am)
#   build/<arch>/     build trees and logs
#   out/<arch>/       bzImage, initramfs images, ISO
[ -n "$ROOT" ] || { echo "lib.sh: ROOT not set" >&2; exit 1; }
SRC=${SRC:-$ROOT/src}
BUILD=${BUILD:-$ROOT/build}
OUT=${OUT:-$ROOT/out}
LOCK=$ROOT/sources.lock

msg() { printf '== %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# sources.lock accessors
lock_names()  { awk '!/^#/ && NF >= 4 { print $1 }' "$LOCK"; }
lock_field()  { awk -v n="$1" -v f="$2" '!/^#/ && $1 == n { print $f; exit }' "$LOCK"; }
lock_commit() { lock_field "$1" 2; }
lock_ref()    { lock_field "$1" 3; }
lock_urls()   { lock_field "$1" 4 | tr ',' ' '; }

# number of non-comment entries of a series file
series_files() { sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$1"; }

# ---- progress bars -------------------------------------------------------------------
# Drawn on stderr when it is a terminal; plain milestone lines otherwise (logs, CI).
# PROGRESS=0 turns them off completely (the commands then run with their output in the logs).
pb_tty() { [ "${PROGRESS:-1}" != 0 ] && [ -t 2 ]; }
# mawk reads a pipe in large blocks unless told to be interactive; gawk reads line by line
if awk -W version 2>&1 | grep -q mawk; then PB_AWK_CMD="awk -W interactive"; else PB_AWK_CMD=awk; fi
# the awk code shared by the filters below: bar(n, total, info) and finish(info)
PB_AWK='
function bar(n, total, info,    w, f, i, s, pct) {
    if (!tty) {
        if (total > 0) {
            pct = int(n * 100 / total); if (pct > 100) pct = 100
            if (pct - last >= 10) { last = pct; printf("   %s: %3d%% %s\n", label, pct, info) > "/dev/stderr"; fflush("/dev/stderr") }
        } else if (n - last >= 200) { last = n; printf("   %s: %d steps %s\n", label, n, info) > "/dev/stderr"; fflush("/dev/stderr") }
        return
    }
    w = 30; s = ""
    if (total > 0) { pct = int(n * 100 / total); if (pct > 99) pct = 99; f = int(w * pct / 100) }
    else { pct = -1; f = n % (w + 1) }     # unknown total: a travelling block
    for (i = 0; i < w; i++) s = s ((pct >= 0 ? i < f : i == f || i == f - 1) ? "#" : ".")
    if (pct >= 0) printf("\r\033[K   %s [%s] %3d%% %s", label, s, pct, info) > "/dev/stderr"
    else printf("\r\033[K   %s [%s] %d %s", label, s, n, info) > "/dev/stderr"
    fflush("/dev/stderr")
}
function elapsed(   e) { e = systime() - t0; return sprintf("%dm%02ds", e / 60, e % 60) }
function finish(info) {
    if (tty) printf("\r\033[K") > "/dev/stderr"
    printf("   %s: %s\n", label, info) > "/dev/stderr"; fflush("/dev/stderr")
}
BEGIN { t0 = systime(); last = 0; status = "1" }
'

# with_ticker CMD...: CMD's stdout+stderr, then "__status=<exit code>"; a "__tick" line every
# second while it runs, so a filter can redraw during silent phases (a server preparing a pack,
# a long link step)
with_ticker() {
    { "$@" 2>&1; echo "__status=$?"; } &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do echo "__tick"; sleep 1; done
    wait "$pid"
}

# run_progress LABEL TOTAL LOG CMD...: run CMD with its stdout+stderr appended to LOG and draw a
# bar, one step per output line matching $PROGRESS_STEP_RE (default: Kbuild's "  CC  file" lines
# or a compiler command with -c/-o).  TOTAL 0 = unknown (a counter is shown).  When
# $PROGRESS_COUNT_FILE is set the final step count is written there (to learn a total for the
# next run).  Returns CMD's exit status.
run_progress() {
    label=$1 total=$2 log=$3; shift 3
    st=$(mktemp)
    with_ticker "$@" | $PB_AWK_CMD -v label="$label" -v total="$total" -v logf="$log" \
        -v tty="$(pb_tty && echo 1 || echo 0)" -v re="${PROGRESS_STEP_RE:-^  [A-Z]+ | -c | -o }" \
        -v st="$st" -v countfile="${PROGRESS_COUNT_FILE:-}" "$PB_AWK"'
    /^__status=/ { status = substr($0, 10); next }
    /^__tick$/ { if (tty) bar(n, total, elapsed()); next }
    { print >> logf; if ($0 ~ re) { n++; bar(n, total, elapsed()) } }
    END {
        if (status == "0") finish(sprintf("%d steps, %s", n, elapsed()))
        else if (tty) printf("\r\033[K") > "/dev/stderr"
        if (countfile != "") print n > countfile
        print status > st
    }'
    s=$(cat "$st"); rm -f "$st"
    return "${s:-1}"
}

# git_progress LABEL GIT-FETCH-ARGS...: run "git ... --progress" and draw git's own percentages
# (remote compression, receiving objects with the transfer size, resolving deltas).  git's
# messages go to $FETCH_ERR when set.  Returns git's exit status.
git_progress() {
    label=$1; shift
    st=$(mktemp)
    with_ticker "$@" --progress | stdbuf -oL tr '\r' '\n' | $PB_AWK_CMD -v label="$label" \
        -v tty="$(pb_tty && echo 1 || echo 0)" -v st="$st" -v err="${FETCH_ERR:-/dev/null}" "$PB_AWK"'
    function pct(   p) { p = 0; if (match($0, /[0-9]+%/)) p = substr($0, RSTART, RLENGTH - 1) + 0; return p }
    function show() { if (gph != lastph) { last = -10; lastph = gph }; bar(gp, 100, gph ", " elapsed()) }
    BEGIN { gp = 0; gph = "waiting for the server" }
    /^__status=/ { status = substr($0, 10); next }
    /^__tick$/ { if (tty) show(); next }
    /Receiving objects:/ { info = ""; if (match($0, /\), [^,]*/)) info = substr($0, RSTART + 3, RLENGTH - 3)
                           gp = pct(); gph = "receiving " info; show(); next }
    /Resolving deltas:/  { gp = pct(); gph = "resolving deltas"; show(); next }
    /remote: (Counting|Enumerating)/ { gp = pct(); gph = "remote counting objects"; show(); next }
    /remote: Compressing/ { gp = pct(); gph = "remote compressing"; show(); next }
    /^$/ { next }
    { print > err }
    END {
        if (status == "0") finish(sprintf("fetched in %s", elapsed()))
        else if (tty) printf("\r\033[K") > "/dev/stderr"
        print status > st
    }'
    s=$(cat "$st"); rm -f "$st"
    return "${s:-1}"
}

# make_total MAKE-ARGS...: the number of steps "make -n" would run (for the bar's total); 0 if
# make -n cannot tell (Kbuild and busybox do not descend into directories under -n)
make_total() { make -n "$@" 2>/dev/null | grep -cE "${PROGRESS_STEP_RE:-^  [A-Z]+ | -c | -o }" || true; }
