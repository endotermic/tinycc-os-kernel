#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# pkg-config.sh - the pkg-config that the configure scripts of the X libraries run
# (scripts/build.sh exports it as PKG_CONFIG).  The host's pkg-config, when it exists at all,
# knows the host's libraries, which the guest must never link; this one reads only the .pc
# files of the directories in PKG_CONFIG_LIBDIR (colon-separated), i.e. the tcc-built
# libraries under build/<arch>/.  Everything here is linked statically, so Requires.private
# and Libs.private always count, as with --static.
# Usage: PKG_CONFIG_LIBDIR=DIR[:DIR...] scripts/pkg-config.sh [options] [package [op version]...]
#   --exists, --atleast-version=V, --exact-version=V, --max-version=V, --modversion,
#   --cflags, --cflags-only-I, --cflags-only-other, --libs, --libs-only-l, --libs-only-L,
#   --libs-only-other, --variable=NAME, --define-variable=NAME=VALUE, --static, --list-all,
#   --print-errors, --short-errors, --silence-errors, --version, --atleast-pkgconfig-version=V
#   -h, --help  this text
# Package operators (>=, =, <=, >, <, !=) are read from the arguments or from inside one
# argument ("xproto >= 7.0.25 xextproto").  Exit status 1 when a package or a version
# requirement is missing, as pkg-config does.
set -e
case ${1:-} in -h|--help) sed -n '3,/^[^#]/p' "$0" | sed '$d' | cut -c3-; exit 0;; esac
want=; atleast=; exact=; maxv=; variable=; defines=; errors=; quiet=
while [ $# -gt 0 ]; do
    case $1 in
        --exists) want="$want exists";;
        --modversion) want="$want modversion";;
        --cflags) want="$want cflags";;
        --cflags-only-I) want="$want cflags-I";;
        --cflags-only-other) want="$want cflags-other";;
        --libs) want="$want libs";;
        --libs-only-l) want="$want libs-l";;
        --libs-only-L) want="$want libs-L";;
        --libs-only-other) want="$want libs-other";;
        --atleast-version=*) atleast=${1#*=};;
        --atleast-version) atleast=$2; shift;;
        --exact-version=*) exact=${1#*=};;
        --exact-version) exact=$2; shift;;
        --max-version=*) maxv=${1#*=};;
        --max-version) maxv=$2; shift;;
        --variable=*) variable=${1#*=}; want="$want variable";;
        --variable) variable=$2; want="$want variable"; shift;;
        --define-variable=*) defines="$defines
${1#*=}";;
        --define-variable) defines="$defines
$2"; shift;;
        --static|--uninstalled|--print-requires|--print-requires-private|--print-provides|--validate) ;;
        --print-errors) errors=1;;
        --short-errors) ;;
        --silence-errors) quiet=1;;
        --version) echo 0.29.2; exit 0;;
        --atleast-pkgconfig-version=*|--atleast-pkgconfig-version) [ "${1#*=}" != "$1" ] || shift; exit 0;;
        --list-all) want="$want list";;
        --errors-to-stdout) ;;
        -*) echo "pkg-config.sh: unknown option $1" >&2; exit 1;;
        *) break;;
    esac
    shift
done
# the remaining arguments, joined, are the package list
pkgs=$*
[ -n "$PKG_CONFIG_LIBDIR" ] || { [ -n "$quiet" ] || echo "pkg-config.sh: PKG_CONFIG_LIBDIR is not set" >&2; exit 1; }
dirs=$(echo "${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$PKG_CONFIG_LIBDIR" | tr ':' ' ')
# find NAME: the .pc file of a package
find_pc() { for d in $dirs; do [ ! -f "$d/$1.pc" ] || { echo "$d/$1.pc"; return 0; }; done; return 1; }
case " $want " in *" list "*)
    for d in $dirs; do for f in "$d"/*.pc; do [ -f "$f" ] && basename "$f" .pc; done; done | sort -u; exit 0;;
esac
# field FILE KEY: a keyword of a .pc file with its variables expanded (${name}); the
# variables of the file, plus --define-variable ones, in order of definition
field() {
    awk -v key="$2" -v defs="$defines" '
    function expand(s,   out, i, j, name) {
        out = ""
        while ((i = index(s, "${")) > 0) {
            out = out substr(s, 1, i - 1); s = substr(s, i + 2)
            j = index(s, "}"); name = substr(s, 1, j - 1); s = substr(s, j + 1)
            out = out vars[name]
        }
        return out s
    }
    BEGIN { n = split(defs, d, "\n"); for (i = 1; i <= n; i++) if (d[i] != "") { split(d[i], kv, "="); fixed[kv[1]] = substr(d[i], length(kv[1]) + 2) } }
    /^[A-Za-z_][A-Za-z0-9_.]*=/ { eq = index($0, "="); name = substr($0, 1, eq - 1); val = substr($0, eq + 1)
        vars[name] = (name in fixed) ? fixed[name] : expand(val); next }
    /^[A-Za-z_.]+:/ { c = index($0, ":"); k = substr($0, 1, c - 1); if (k == key) { print expand(substr($0, c + 1)); found = 1 } }
    END { if (!found && (key in vars)) print vars[key] }' "$1"
}
# version comparison (as pkg-config: dot-separated numbers, missing parts are 0)
vcmp() { # vcmp A B: prints -1, 0 or 1
    awk -v a="$1" -v b="$2" 'BEGIN { na = split(a, x, "."); nb = split(b, y, "."); n = na > nb ? na : nb
        for (i = 1; i <= n; i++) { p = (i <= na) ? x[i] + 0 : 0; q = (i <= nb) ? y[i] + 0 : 0
            if (p < q) { print -1; exit } if (p > q) { print 1; exit } } print 0 }'
}
vok() { # vok VERSION OP WANTED
    r=$(vcmp "$1" "$3")
    case $2 in '>=') [ "$r" -ge 0 ];; '<=') [ "$r" -le 0 ];; '=') [ "$r" -eq 0 ];; '!=') [ "$r" -ne 0 ];; '>') [ "$r" -gt 0 ];; '<') [ "$r" -lt 0 ];; esac
}
missing() { [ -n "$quiet" ] || echo "pkg-config.sh: $*" >&2; exit 1; }
# the packages asked for, with their constraints, checked; the list of their names in $top
top=; set -- $pkgs
while [ $# -gt 0 ]; do
    name=$1; shift
    pc=$(find_pc "$name") || missing "package '$name' not found in $dirs"
    ver=$(field "$pc" Version)
    case ${1:-} in '>='|'<='|'='|'!='|'>'|'<')
        vok "$ver" "$1" "$2" || missing "$name version $ver does not satisfy $name $1 $2"; shift 2;;
    esac
    [ -z "$atleast" ] || vok "$ver" '>=' "$atleast" || exit 1
    [ -z "$exact" ] || vok "$ver" '=' "$exact" || exit 1
    [ -z "$maxv" ] || vok "$ver" '<=' "$maxv" || exit 1
    top="$top $name"
done
# closure NAME...: the names, then their Requires and Requires.private, depth first, each once
seen=
closure() {
    for n; do
        case " $seen " in *" $n "*) continue;; esac
        seen="$seen $n"
        pc=$(find_pc "$n") || missing "package '$n' (required by another) not found in $dirs"
        reqs=$(field "$pc" Requires; field "$pc" Requires.private)
        # drop the version constraints of the Requires lines
        reqs=$(echo "$reqs" | tr ',' ' ' | awk '{ for (i = 1; i <= NF; i++) { if ($i ~ /^[<>=!]/) { i++; continue } print $i } }')
        [ -z "$reqs" ] || closure $reqs
    done
}
closure $top
for w in $want; do
    case $w in
        exists) ;;
        modversion) for n in $top; do field "$(find_pc "$n")" Version; done;;
        variable) for n in $top; do field "$(find_pc "$n")" "$variable"; done;;
        cflags*|libs*)
            out=
            for n in $seen; do
                pc=$(find_pc "$n")
                case $w in
                    cflags*) out="$out $(field "$pc" Cflags)";;
                    libs*) out="$out $(field "$pc" Libs) $(field "$pc" Libs.private)";;
                esac
            done
            # -l flags: keep the last occurrence (static link order), the others the first
            echo "$out" | tr ' ' '\n' | awk -v w="$w" '
                $0 == "" { next }
                { keep = 1
                  if (w == "cflags-I") keep = ($0 ~ /^-I/); else if (w == "cflags-other") keep = ($0 !~ /^-I/)
                  else if (w == "libs-l") keep = ($0 ~ /^-l/); else if (w == "libs-L") keep = ($0 ~ /^-L/)
                  else if (w == "libs-other") keep = ($0 !~ /^-[lL]/)
                  if (!keep) next
                  if ($0 ~ /^-l/) { if ($0 in pos) { delete order[pos[$0]] } pos[$0] = ++k; order[k] = $0 }
                  else if (!($0 in pos)) { pos[$0] = ++k; order[k] = $0 } }
                END { s = ""; for (i = 1; i <= k; i++) if (i in order) s = s (s == "" ? "" : " ") order[i]; print s }';;
    esac
done
exit 0
