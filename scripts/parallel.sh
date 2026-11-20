#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# parallel.sh - the distro build as one dependency graph that GNU make -j runs: every component
# is fetched and patched on its own, and each build step starts as soon as its own sources and
# the steps it needs are there: the host tcc, tld, libelf and musl while linux is still
# downloading, the kernel next to the userland, and the ~30 libraries of the X stack each as a
# step of its own (scripts/build.sh xlib-NAME), side by side as their dependencies allow
# (their configure scripts, one process at a time each, are most of their time).
# build.sh --parallel (the default) calls it.  The jobs are the usual scripts (fetch.sh NAME,
# patch.sh NAME, build.sh STEP), run with PROGRESS=0 (milestone lines instead of bars) and
# their output prefixed with the job's name; the overall bar (build.sh) shows the steps running.
# One make jobserver for everything: up to N jobs at once (-j N, default JOBS, i.e. nproc),
# and a step's own make (the kernel's, a library's) takes its parallel compilers from the
# same N (scripts/build.sh jrun), so that the steps together never run more than N processes
# that count: a step running configure holds one, the kernel takes what is free.  When a job
# fails, the others are stopped at once and the failing job's log tail is the last thing printed.
# Usage: scripts/parallel.sh [-j N] [--test] [--selfhost] [--iso]   (ARCH, JOBS from the
#        environment, as for scripts/build.sh)
#        scripts/parallel.sh --list [--test] [--selfhost] [--iso]   the graph's build steps
#        scripts/parallel.sh job NAME CMD...   (internal: one job of the graph)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/lib.sh"
case ${1:-} in -h|--help) usage;; esac
ARCH=${ARCH:-x86_64}
JOBS=${JOBS:-$(nproc)}
PAR_DIR=$BUILD/$ARCH/parallel

mark() { [ -z "${OVERALL_STATE:-}" ] || echo "$1 $2 $(date +%s)" >> "$OVERALL_STATE"; }

# job NAME CMD...: CMD with PROGRESS=0, output prefixed "[NAME] ".  The graph's jobserver
# (make's MAKEFLAGS: "-jN --jobserver-auth=fifo:PATH") goes to CMD as JOBSERVER_FLAGS, for the
# makes the step runs with jrun/jmake; MAKEFLAGS itself is unset, so that the step's other
# makes (configure probes, installs) run one job as they do outside the graph.  XLIB_GRAPH=1:
# the X steps leave the libraries to their own xlib-NAME steps.  On failure: record NAME and
# stop the whole graph (make runs in its own process group, see below: kill 0 reaches every
# job in it).
if [ "${1:-}" = job ]; then
    name=$2; shift 2
    mark S "$name"
    JOBSERVER_FLAGS=$(for f in ${MAKEFLAGS:-}; do case $f in -j[0-9]*|--jobserver-auth=*) printf '%s ' "$f";; esac; done)
    case $JOBSERVER_FLAGS in *--jobserver-auth=*) ;; *) JOBSERVER_FLAGS=;; esac
    export JOBSERVER_FLAGS XLIB_GRAPH=1
    st=$(mktemp)
    { unset MAKEFLAGS MFLAGS MAKELEVEL MAKEOVERRIDES
      PROGRESS=0 "$@" </dev/null 2>&1 && echo 0 > "$st" || echo $? > "$st"; } \
        | sed -u "s|^|[$name] |"      # (-u: each line in one write, jobs do not interleave)
    s=$(cat "$st" 2>/dev/null); rm -f "$st"; s=${s:-1}
    if [ "$s" = 0 ]; then mark E "$name"; exit 0; fi
    echo "$name" >> "$PAR_DIR/failed"
    printf '== %s failed (exit %s): stopping the other jobs\n' "$name" "$s" >&2
    trap '' TERM; kill -TERM 0 2>/dev/null || true
    exit "$s"
fi
if [ "${1:-}" = mark ]; then mark "$2" "$3"; exit 0; fi

par=$JOBS test= selfhost= iso= list=
while [ $# -gt 0 ]; do
    case $1 in
        -j) par=$2; shift 2;; -j*) par=${1#-j}; shift;;
        --test) test=1; shift;; --selfhost) selfhost=1; shift;; --iso) iso=1; shift;;
        --list) list=1; shift;;
        *) die "parallel.sh: unknown option '$1'";;
    esac
done
case $par in ''|*[!0-9]*|0) die "parallel.sh: -j needs a positive number";; esac

# ---- the graph ----
# the components: linux first (the largest download by far), then those the first steps need
names=$(lock_names | awk '{ p = $1 == "linux" ? 0 : $1 == "tinycc" ? 1 : $1 == "musl" ? 2 : 3; print p, NR, $1 }' | sort -n -k1,1 -k2,2 | cut -d' ' -f3)
gate=host-tcc; [ -z "$test" ] || gate=tcc-test
# STEP: what it needs.  A word @NAME is component NAME fetched and patched (patch-NAME).
# "X LIB: LIBS" is the step xlib-LIB of scripts/build.sh, which needs xlib- of each of LIBS
# and its own component.  The dependencies are the real ones (each package's configure and
# link), plus a few that keep a package from finding an optional library its serial order
# never showed it: libXdmcp after libxcb (libxcb would link it), libXpm after libXt and
# libXext (its sxpm).  Every autotools library after libXau waits for it: libXau's configure
# writes the site cache they read (scripts/build.sh xsite_write).  Only the steps that include
# Linux headers (or probe them: links) wait for kernel-headers.
deps="
host-tcc: @tinycc
${test:+tcc-test: host-tcc}
tld: $gate
libelf: $gate
kernel: tld libelf @linux
kernel-headers: @linux
musl: $gate @musl
busybox: musl kernel-headers @busybox
ncurses: musl kernel-headers @ncurses
nnn: ncurses @nnn @musl-fts
libressl: musl kernel-headers @libressl
guest-tcc: musl @tinycc
zlib: musl @zlib
libpng: zlib @libpng
X xorgproto: musl
X xtrans: musl
X libXau: xorgproto
X xcb-proto: libXau
X libxcb: xcb-proto libXau
X libX11: libxcb xtrans
X libXdmcp: libxcb
X libfontenc: libXau zlib
X libXfont: libfontenc xtrans
X libICE: libXau xtrans
X libSM: libICE
X libXext: libX11
X libXt: libSM libX11
X libXmu: libXt libXext
X libXpm: libXt libXext
X libXaw: libXmu libXpm
X libXrender: libX11
X expat: libXau
X freetype: libXau libpng
X fontconfig: freetype expat
X libXft: fontconfig libXrender
X libXinerama: libXext
X jpeg: libXau
X libXfixes: libX11
X libXcursor: libXrender libXfixes
X xprop: libX11
X xrdb: libXmu
X libXcomposite: libXfixes
X libXdamage: libXfixes
X xcompmgr: libXcomposite libXdamage libXrender libXext
X xcb-util: libxcb
X xcb-util-image: xcb-util
X xcb-util-keysyms: libxcb
X xcb-util-wm: libxcb
tinyx: xlib-libXfont xlib-libXdmcp kernel-headers @tinyx
xdm: xlib-libXaw xlib-libXft xlib-libXdmcp @xdm
jwm: xlib-libXft xlib-libXinerama xlib-jpeg xlib-libXpm xlib-libXmu @jwm @dejavu-fonts
mlterm: xlib-libXft kernel-headers @mlterm
xfiles: xlib-libXft xlib-libXinerama xlib-jpeg xlib-libXcursor xlib-libXpm xlib-libXmu xlib-xprop xlib-xrdb xlib-xcompmgr @dmenu @xmenu @xfiles @unifont
xedit: xlib-libXaw @xedit
wallpapers: xlib-libX11 xlib-jpeg libpng @wallpapers
links: xlib-libX11 xlib-jpeg xlib-fontconfig libressl kernel-headers @links @dejavu-fonts @liberation-fonts
curl: libressl zlib kernel-headers @curl
netsurf: curl xlib-xcb-util-image xlib-xcb-util-keysyms xlib-xcb-util-wm xlib-freetype xlib-jpeg libpng @netsurf @quickjs @dejavu-fonts
libnl: musl kernel-headers @libnl
wifi: libnl libressl kernel-headers @wpa_supplicant @hostapd @iw
firmware: kernel @linux-firmware @wireless-regdb @sof-bin
initramfs: kernel busybox nnn libressl tinyx xdm jwm mlterm xfiles xedit wallpapers links curl netsurf wifi firmware guest-tcc
${selfhost:+selfhost: initramfs @make}
${iso:+iso: initramfs ${selfhost:+selfhost}}
"
final=initramfs; [ -z "$selfhost" ] || final=selfhost; [ -z "$iso" ] || final=iso
# the graph's lines as "TARGET: DEPS", the X lines expanded
graph=$(echo "$deps" | awk '
    NF == 0 { next }
    $1 == "X" { t = $2; sub(/:$/, "", t); l = "xlib-" t ": @" t; for (i = 3; i <= NF; i++) l = l " " ($i == "musl" || $i == "zlib" || $i == "libpng" ? $i : "xlib-" $i); print l; next }
    { print }')
if [ -n "$list" ]; then echo "$graph" | sed 's/:.*//'; exit 0; fi

mkdir -p "$PAR_DIR"; rm -f "$PAR_DIR/failed"
mk=$PAR_DIR/graph.mk
J="sh '$ROOT/scripts/parallel.sh'"
{
    echo "# generated by scripts/parallel.sh; run with make -f"
    echo ".PHONY: all fetched sources"
    # the kernel lane first: make starts the prerequisites from the left
    echo "all: kernel $final sources"
    for n in $names; do
        printf 'fetch-%s:\n\t+@%s job fetch-%s "%s/scripts/fetch.sh" %s\n' "$n" "$J" "$n" "$ROOT" "$n"
        printf 'patch-%s: fetch-%s\n\t+@%s job patch-%s "%s/scripts/patch.sh" %s\n' "$n" "$n" "$J" "$n" "$ROOT" "$n"
        echo ".PHONY: fetch-$n patch-$n"
    done
    printf 'fetched:'; for n in $names; do printf ' fetch-%s' "$n"; done
    printf '\n\t@%s mark E fetch\n' "$J"
    printf 'sources: fetched'; for n in $names; do printf ' patch-%s' "$n"; done
    printf '\n\t@%s mark E patch\n' "$J"
    echo "$graph" | while read -r t d; do
        t=${t%:}
        d=$(for w in $d; do case $w in @*) printf ' patch-%s' "${w#@}";; *) printf ' %s' "$w";; esac; done)
        printf '%s:%s\n\t+@%s job %s "%s/scripts/build.sh" %s\n.PHONY: %s\n' "$t" "$d" "$J" "$t" "$ROOT" "$t" "$t"
    done
} > "$mk"
# every @NAME must be a component, every other word a target of the graph
for w in $(echo "$graph" | cut -d: -f2-); do
    case $w in
        @*) echo "$names" | grep -qx "${w#@}" || die "parallel.sh: the graph names component ${w#@}, which sources.lock has not";;
        *) echo "$graph" | sed 's/:.*//' | grep -qx "$w" || die "parallel.sh: the graph names step $w, which it has not";;
    esac
done

msg "parallel: $(echo "$names" | wc -l) components and $(echo "$graph" | wc -l) steps, up to $par jobs at once (one jobserver: the steps' own makes share them)"
mark S fetch; mark S patch
# make in its own session and process group, so that a failing job (kill 0) or an interrupt
# here stops every job and its compilers at once; its stdin is not the terminal
pgf=$PAR_DIR/pgid; rm -f "$pgf"
stop() {
    pg=$(cat "$pgf" 2>/dev/null || true)
    [ -z "$pg" ] || kill -TERM "-$pg" 2>/dev/null || true
    wait 2>/dev/null || true
    exit "$1"
}
trap 'stop 130' INT; trap 'stop 143' TERM HUP
export ARCH JOBS
# (make's own complaints about the jobs a failure stopped are dropped: the failing job said why)
st=$PAR_DIR/status; rm -f "$st"
{ setsid -w sh -c 'echo $$ > "$1"; shift; exec "$@"' sh "$pgf" \
      make --no-print-directory -s -j"$par" --jobserver-style=fifo -f "$mk" all </dev/null 2>&1 && echo 0 > "$st" || echo $? > "$st"; } \
    | grep --line-buffered -v -E '^make: \*\*\* (\[.*\] Terminated|wait: No child processes|Waiting for unfinished jobs)' &
wait $! || true
trap - INT TERM HUP
s=$(cat "$st" 2>/dev/null || echo 1)
if [ -s "$PAR_DIR/failed" ]; then
    die "parallel build failed in: $(sort -u "$PAR_DIR/failed" | tr '\n' ' ')(the step's log is named above)"
fi
[ "$s" = 0 ] || die "parallel build: make exited with $s"
msg "parallel: all jobs done"
