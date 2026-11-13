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
# usage [STATUS]: the script's documentation, which is its header comment (line 3 to the first
# line that is not a comment), then exit.  Every script answers -h/--help with it (status 0,
# stdout); with a non-zero status (a usage error) it goes to stderr.
usage() {
    if [ "${1:-0}" = 0 ]; then sed -n '3,/^[^#]/p' "$0" | sed '$d' | cut -c3-
    else sed -n '3,/^[^#]/p' "$0" | sed '$d' | cut -c3- >&2; fi
    exit "${1:-0}"
}

# ---- host prerequisites ----------------------------------------------------------------
# Every script checks what it needs on the host when it starts, before any work, and names
# every missing piece at once with the Debian/Ubuntu packages that provide them:
#   need_reset; need_tool TOOL[:PACKAGE]...; need_dir DIR:PACKAGE...; need_done
# (PACKAGE defaults to the tool's name).  The sets below are what the scripts share.
need_reset() { NEED_MISS=; NEED_PKGS=; }
need_add() {
    case " $NEED_MISS " in *" $1 "*) ;; *) NEED_MISS="$NEED_MISS $1";; esac
    case " $NEED_PKGS " in *" $2 "*) ;; *) NEED_PKGS="$NEED_PKGS $2";; esac
}
need_tool() { for _t; do _n=${_t%%:*}; _p=${_t#*:}; [ "$_p" != "$_t" ] || _p=$_n; have "$_n" || need_add "$_n" "$_p"; done; }
need_dir() { for _t; do _n=${_t%%:*}; [ -d "$_n" ] || need_add "$_n" "${_t#*:}"; done; }
need_done() {
    [ -n "$NEED_MISS" ] || return 0
    printf 'error: missing on this host:%s\n' "$NEED_MISS" >&2
    printf '       install with (Debian/Ubuntu package names):  sudo apt install%s\n' "$NEED_PKGS" >&2
    exit 1
}
# what the build needs: git (sources, version tags), make, gcc (builds the host tcc and
# gen_init_cpio; nothing else), binutils' ar (musl and busybox archives), the kernel's host
# tools flex/bison/bc/rsync, cpio, tar, and for the X libraries python3 (libxcb's C is
# generated from xcb-proto's XML) and cpp (libX11's keysym and compose tables)
need_build_tools() { need_tool git make gcc ar:binutils flex bison bc cpio rsync tar python3 cpp; }
# downloads (fetch.sh: GNU make's release tarball): curl or wget
need_download_tool() { have curl || have wget || need_add "curl (or wget)" curl; }
# the ISO: grub-mkrescue with both GRUB platforms (without i386-pc the ISO is UEFI-only and
# SeaBIOS fails with "Could not read from CDROM"), xorriso, mtools
need_iso_tools() {
    need_tool grub-mkrescue:grub2-common xorriso mformat:mtools
    need_dir /usr/lib/grub/i386-pc:grub-pc-bin /usr/lib/grub/x86_64-efi:grub-efi-amd64-bin
}
# QEMU for the architecture (one package provides both system emulators)
need_qemu() { case $1 in i386) need_tool qemu-system-i386:qemu-system-x86;; *) need_tool qemu-system-x86_64:qemu-system-x86;; esac; }

# sources.lock accessors
lock_names()  { awk '!/^#/ && NF >= 4 { print $1 }' "$LOCK"; }
lock_field()  { awk -v n="$1" -v f="$2" '!/^#/ && $1 == n { print $f; exit }' "$LOCK"; }
lock_commit() { lock_field "$1" 2; }
lock_ref()    { lock_field "$1" 3; }
lock_urls()   { lock_field "$1" 4 | tr ',' ' '; }
# the directory of a component that is checked out alone ("#sparse NAME DIRECTORY", which may
# contain spaces); empty for the components that are checked out whole
lock_sparse() { awk -v n="$1" '$1 == "#sparse" && $2 == n { sub(/^#sparse[ \t]+[^ \t]+[ \t]+/, ""); print; exit }' "$LOCK"; }

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

# ---- the overall progress bar ----------------------------------------------------------
# build.sh (the top level) pins one line to the bottom of the terminal: the whole run's
# progress over its steps, the step it is in, the elapsed time and an estimate of what is left.
# The terminal's scroll region keeps everything else (messages, the per-step bars above) in
# the rows above it; a background drawer redraws it every second.  Each step weighs what it
# took on the last run (TIMES-FILE, written when a run succeeds), or the estimate below for a
# fresh build when it has no record.  Only on a terminal (stdout and stderr) with PROGRESS not
# 0; elsewhere every overall_* function does nothing.
#   overall_begin TIMES-FILE STEP...   the plan, in order; sets the traps that end the bar
#   overall_step STEP                  STEP starts (scripts/build.sh calls it for its steps)
#   overall_end STATUS                 removes the bar; on 0 records the step times
# A parallel run (build.sh --parallel) adds a "P" line to the state file and an "E STEP" mark
# when each step ends: several steps are then running at once.
OVERALL_ESTIMATES='fetch 120 patch 20 host-tcc 40 tcc-test 150 tcc-archive 5 tld 5 libelf 3
kernel 420 musl 45 busybox 60 ncurses 60 nnn 5 libressl 90 zlib 10 libpng 15 xlibs 420
tinyx 30 xdm 30 jwm 15 mlterm 45 xfiles 20 wallpapers 15 links 40 guest-tcc 20 initramfs 20 selfhost 150 iso 30'
overall_tty() { pb_tty && [ -t 1 ] && have stty && stty size </dev/tty >/dev/null 2>&1; }
overall_step() { [ -z "${OVERALL_STATE:-}" ] || echo "S $1 $(date +%s)" >> "$OVERALL_STATE"; }
overall_begin() {
    OVERALL_STATE=; OVERALL_PID=
    overall_tty || { shift; return 0; }
    _times=$1; shift
    OVERALL_TIMES=$_times OVERALL_STATE=$(mktemp); export OVERALL_STATE
    for _s; do
        _w=; [ ! -f "$_times" ] || _w=$(awk -v s="$_s" '$1 == s { w = $2 } END { print w }' "$_times")
        [ -n "$_w" ] || _w=$(echo $OVERALL_ESTIMATES | awk -v s="$_s" '{ for (i = 1; i < NF; i += 2) if ($i == s) print $(i + 1) }')
        _w=${_w:-30}; [ "$_w" -ge 1 ] 2>/dev/null || _w=1; echo "$_s $_w"
    done > "$OVERALL_STATE.plan"
    echo "B - $(date +%s)" > "$OVERALL_STATE"
    trap 'overall_end $?' EXIT; trap 'exit 130' INT; trap 'exit 143' TERM HUP
    # a free bottom row: a newline (scrolls when the cursor is on the last row), then the
    # cursor back up; the drawer sets the scroll region (and again after a resize)
    printf '\n\033[1A' >&2
    overall_draw_loop "$$" &
    OVERALL_PID=$!
}
# overall_line ROWS COLS LASTPCT: the bar's text from the plan and the state file; prints
# "PCT TEXT" (PCT never goes down: LASTPCT is the previous value)
overall_line() {
    awk -v now="$(date +%s)" -v cols="$2" -v lastpct="$3" -v plan="$OVERALL_STATE.plan" '
    function t(s) { s = int(s + 0.5); return s >= 3600 ? sprintf("%dh%02dm", s / 3600, s % 3600 / 60) : sprintf("%dm%02ds", s / 60, s % 60) }
    BEGIN { while ((getline l < plan) > 0) { split(l, p, " "); n++; name[n] = p[1]; w[n] = p[2]; total += p[2]; idx[p[1]] = n } }
    $1 == "B" { t0 = $3 }
    $1 == "S" && ($2 in idx) { cur = idx[$2]; ts = $3 }
    $1 == "P" { par = 1 }
    $1 == "S" && ($2 in idx) && !($2 in st) { st[$2] = $3 }
    $1 == "E" && ($2 in idx) { en[$2] = $3 }
    END {
        if (par) {
            # a parallel run (scripts/parallel.sh): the steps with an E mark are done, those
            # started without one are running; the time left from the rate so far
            donew = 0; part = 0; nd = 0; run = ""
            for (i = 1; i <= n; i++) {
                if (name[i] in en) { donew += w[i]; nd++ }
                else if (name[i] in st) {
                    el = now - st[name[i]]; fr = w[i] > 0 ? el / w[i] : 0; if (fr > 0.95) fr = 0.95
                    part += w[i] * fr; run = run (run == "" ? "" : " ") name[i]
                }
            }
            pct = total > 0 ? int(100 * (donew + part) / total) : 0
            if (pct < lastpct) pct = lastpct; if (pct > 99) pct = 99
            gone = now - t0
            info = sprintf("%3d%%  %d/%d done  %s", pct, nd, n, t(gone))
            if (donew + part > 0 && gone >= 10) info = info sprintf(", ~%s left", t((total - donew - part) * gone / (donew + part)))
            extra = "  " (run == "" ? "waiting" : run)     # after the bar width: the names may be cut
        } else {
        donew = 0; for (i = 1; i < cur; i++) donew += w[i]
        el = cur ? now - ts : 0; fr = cur && w[cur] > 0 ? el / w[cur] : 0; if (fr > 0.95) fr = 0.95
        pct = total > 0 ? int(100 * (donew + (cur ? w[cur] * fr : 0)) / total) : 0
        if (pct < lastpct) pct = lastpct; if (pct > 99) pct = 99
        left = 0; for (i = cur + 1; i <= n; i++) left += w[i]; if (cur) left += (w[cur] > el ? w[cur] - el : w[cur] * 0.05)
        info = sprintf("%3d%%  %d/%d %s  %s", pct, cur, n, cur ? name[cur] : "starting", t(now - t0))
        if (cur) info = info sprintf(", ~%s left", t(left))
        }
        bw = cols - length(info) - 13 - (par ? 20 : 0); if (bw > 40) bw = 40
        s = ""; if (bw >= 10) { f = int(bw * pct / 100); for (i = 0; i < bw; i++) s = s (i < f ? "#" : "."); s = "[" s "] " }
        info = info extra
        line = " overall " s info; if (length(line) > cols - 1) line = substr(line, 1, cols - 1)
        print pct " " line
    }' "$OVERALL_STATE"
}
overall_draw_loop() {
    trap - EXIT INT TERM HUP; _parent=$1 _rows0=0 _pct=0
    while kill -0 "$_parent" 2>/dev/null && [ -f "$OVERALL_STATE" ]; do
        set -- $(stty size </dev/tty 2>/dev/null); _rows=${1:-0} _cols=${2:-80}
        if [ "$_rows" -ge 3 ]; then
            [ "$_rows" = "$_rows0" ] || { printf '\0337\033[1;%dr\0338' $((_rows - 1)) >&2; _rows0=$_rows; }
            _l=$(overall_line "$_rows" "$_cols" "$_pct"); _pct=${_l%% *}
            printf '\0337\033[%d;1H\033[2K\033[7m%s\033[0m\0338' "$_rows" "${_l#* }" >&2
        fi
        sleep 1
    done
}
overall_end() {
    [ -n "${OVERALL_STATE:-}" ] || return 0
    _st=$1
    kill "$OVERALL_PID" 2>/dev/null || true; wait "$OVERALL_PID" 2>/dev/null || true
    set -- $(stty size </dev/tty 2>/dev/null)
    printf '\0337\033[r\0338' >&2
    [ -z "${1:-}" ] || printf '\0337\033[%d;1H\033[2K\0338' "$1" >&2
    _now=$(date +%s)
    # the steps' times: each lasts until the next one starts (the last until now)
    # (a parallel run, P: from each step's first S mark to its E mark, the plan's steps only)
    awk -v now="$_now" -v plan="$OVERALL_STATE.plan" '
        BEGIN { while ((getline l < plan) > 0) { split(l, q, " "); inplan[q[1]] } }
        $1 == "P" { par = 1 }
        $1 == "S" { if (!($2 in st)) st[$2] = $3; if (p != "") d[p] += $3 - t; p = $2; t = $3 }
        $1 == "E" { en[$2] = $3 }
        END {
            if (par) { for (s in st) if (s in inplan) print s, ((s in en) ? en[s] : now) - st[s] }
            else { if (p != "") d[p] += now - t; for (s in d) print s, d[s] }
        }' "$OVERALL_STATE" > "$OVERALL_STATE.times"
    _el=$(awk -v now="$_now" '$1 == "B" { print now - $3 }' "$OVERALL_STATE")
    _cur=$(awk -v plan="$OVERALL_STATE.plan" 'BEGIN { while ((getline l < plan) > 0) { split(l, q, " "); inplan[q[1]] } }
        $1 == "P" { par = 1 } $1 == "S" { s = $2; if ($2 in inplan) st[$2] } $1 == "E" { delete st[$2] }
        END { if (par) { s = ""; for (k in st) s = s (s == "" ? "" : "+") k } print s }' "$OVERALL_STATE")
    if [ "$_st" = 0 ]; then
        mkdir -p "$(dirname "$OVERALL_TIMES")"
        # merge: this run's steps replace their old records, the others stay
        { cat "$OVERALL_STATE.times"; [ ! -f "$OVERALL_TIMES" ] || awk 'NR == FNR { s[$1]; next } !($1 in s)' "$OVERALL_STATE.times" "$OVERALL_TIMES"; } \
            > "$OVERALL_TIMES.new" && mv "$OVERALL_TIMES.new" "$OVERALL_TIMES"
        printf '== overall: %d steps in %dm%02ds\n' "$(wc -l < "$OVERALL_STATE.plan")" $((_el / 60)) $((_el % 60))
    else
        printf '== overall: stopped in step %s after %dm%02ds (exit %s)\n' "${_cur:-?}" $((_el / 60)) $((_el % 60)) "$_st" >&2
    fi
    rm -f "$OVERALL_STATE" "$OVERALL_STATE.plan" "$OVERALL_STATE.times"
    OVERALL_STATE=
}
