#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# tests/netsurf/run.sh: runs NetSurf's self-checking test pages on the host and reports.
#
#   tests/netsurf/run.sh [--arch x86_64|i386] [--binary PATH] [--timeout S] [--keep] [--probe JS] [PAGE...]
#
# Each page (tests/netsurf/pages/*.html, or the PAGEs given: names there, paths or URLs)
# is opened by the built browser (build/<arch>/netsurf/stage/usr/bin/netsurf-fb) with the
# "ram" surface (-f ram: no window), its log read until the page prints
# "TEST END <checks> <failed>" (pages/harness.js) or the time is up.  A page passes when it
# ended, no check failed, and the number of uncaught script errors is the one its
# <meta name="expect-uncaught" content="N"> names (0 without it).  Checks of known gaps
# (T.todo) are listed as "todo:" while they fail and "fixed:" once they pass.
# The resources and the DejaVu fonts come from the build stages through $NETSURFRES (on NetSurf's resource path),
# HOME is an empty directory, so the host's ~/.netsurf is never read.  The LibreSSL in the
# browser reads /etc/ssl/openssl.cnf (a fixed path) and aborts on the host's OpenSSL 3 file,
# so the browser runs in a private mount namespace (unshare -rm, no root) where /etc/ssl is
# a directory with only cert.pem, a copy of the host's CA bundle (for https URLs).
# NETSURF_JS_TRACE=1 is set: each unimplemented WebIDL member a page uses is listed
# ("unimplemented: ...").  Logs of failed pages are kept in
# build/<arch>/netsurf/tests/<page>.log (of every page with --keep).
# A URL (http://...) is run the same way and only reports errors and the trace; this is
# how a saved copy of a real site is measured (served by python3 -m http.server).
# Pages with <meta name="test-server"> are opened from tests/netsurf/server.py, started on
# 127.0.0.1 ports 18537 and 18538 (NETSURF_TEST_PORT sets the first).
# --probe JS runs the script file JS in every page after NetSurf's web APIs
# (NETSURF_JS_PROBE): a look into a real site, e.g. a setTimeout that throws an Error
# carrying the state it found, which the URL report then lists.
# Memory is capped with a systemd-run scope where one is available (1 GB per page);
# NETSURF_TEST_CPU=25% also caps the CPU, to run the browser as slowly as in the guest.
# A crash (signal) or error exit of the browser is reported, and fails a page.
# Exit status: 0 when every page passed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
arch=x86_64
binary=
timeout=30
keep=0
probe=
pages=()
while [ $# -gt 0 ]; do
    case $1 in
        --arch) arch=$2; shift 2 ;;
        --binary) binary=$2; shift 2 ;;
        --timeout) timeout=$2; shift 2 ;;
        --keep) keep=1; shift ;;
        --probe) probe=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); shift 2 ;;
        -h|--help) sed -n '3,31p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "run.sh: unknown option $1" >&2; exit 2 ;;
        *) pages+=("$1"); shift ;;
    esac
done
stage=$repo/build/$arch/netsurf/stage
[ -n "$binary" ] || binary=$stage/usr/bin/netsurf-fb
fonts=$repo/build/$arch/jwm/stage/usr/share/fonts/TTF
for need in "$binary" "$stage/usr/share/netsurf/Choices" "$fonts/DejaVuSans.ttf"; do
    if [ ! -e "$need" ]; then
        echo "run.sh: $need is missing (scripts/build.sh --arch $arch jwm netsurf)" >&2
        exit 2
    fi
done
if [ ${#pages[@]} -eq 0 ]; then
    for p in "$here"/pages/*.html; do pages+=("$p"); done
fi

out=$repo/build/$arch/netsurf/tests
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
res=$tmp/res
mkdir -p "$res" "$tmp/home"
ln -s "$stage"/usr/share/netsurf/* "$res"/
ln -s "$fonts"/*.ttf "$res"/

ssl=()
if [ -e /etc/ssl/openssl.cnf ]; then
    mkdir -p "$tmp/ssl"
    for ca in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem; do
        if [ -f "$ca" ]; then cp "$ca" "$tmp/ssl/cert.pem"; break; fi
    done
    if ! unshare -rm sh -c "mount --bind '$tmp/ssl' /etc/ssl" 2>/dev/null; then
        echo "run.sh: unshare -rm failed (unprivileged user namespaces off?); LibreSSL needs /etc/ssl without the host's openssl.cnf" >&2
        exit 2
    fi
    ssl=(unshare -rm sh -c 'mount --bind "$0" /etc/ssl && exec "$@"' "$tmp/ssl")
fi
cap=()
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
    cap=(systemd-run --user --scope -q -p MemoryMax=1G -p MemorySwapMax=0 ${NETSURF_TEST_CPU:+-p CPUQuota=$NETSURF_TEST_CPU})
fi

# the test server (server.py), for pages with <meta name="test-server">: they are opened
# from it (http://127.0.0.1:PORT/NAME.html?other=PORT2, the second port a second origin)
port=${NETSURF_TEST_PORT:-18537}
port2=$((port + 1))
server=
for page in "${pages[@]}"; do
    f=$page
    case $page in *://*) continue ;; */*) ;; *) f=$here/pages/$page; [ -e "$f" ] || f=$f.html ;; esac
    if [ -e "$f" ] && grep -q '<meta name="test-server"' "$f"; then
        python3 -I "$here/server.py" "$here/pages" $port $port2 > "$tmp/server.log" 2>&1 &
        server=$!
        trap 'kill $server 2>/dev/null; rm -rf "$tmp"' EXIT
        for i in $(seq 50); do grep -q ready "$tmp/server.log" && break; sleep 0.1; done
        grep -q ready "$tmp/server.log" || { echo "run.sh: the test server did not start:" >&2; cat "$tmp/server.log" >&2; exit 2; }
        break
    fi
done

total=0 passed=0
for page in "${pages[@]}"; do
    case $page in
        *://*) url=$page; name=$(printf '%s' "$page" | sed 's|^[a-z]*://||; s|[^A-Za-z0-9._-]|_|g'); file= ;;
        */*) file=$(cd "$(dirname "$page")" && pwd)/$(basename "$page"); url=file://$file; name=$(basename "$page" .html) ;;
        *) file=$here/pages/$page; [ -e "$file" ] || file=$here/pages/$page.html
           url=file://$file; name=$(basename "$file" .html) ;;
    esac
    if [ -n "$file" ] && grep -q '<meta name="test-server"' "$file"; then
        url=http://127.0.0.1:$port/$(basename "$file")?other=$port2
    fi
    log=$out/$name.log
    : > "$log"
    start=$(date +%s.%N)
    NETSURFRES=$res HOME=$tmp/home NETSURF_JS_TRACE=1 NETSURF_JS_PROBE=$probe \
        "${cap[@]}" "${ssl[@]}" timeout -k 2 "$timeout" "$binary" -v -f ram "$url" > "$log" 2>&1 &
    pid=$!
    # wait for the end line (or the browser's exit), then stop it
    while kill -0 $pid 2>/dev/null; do
        if [ -n "$file" ] && grep -q 'TEST END' "$log"; then
            sleep 0.3
            kill $pid 2>/dev/null
            break
        fi
        sleep 0.2
    done
    wait $pid 2>/dev/null
    status=$?
    # the browser's own end: a crash (128 + signal) or an error exit; not timeout's 124 or our kill
    exited=
    case $status in 0|124|137|143) ;; *) exited="browser exited with status $status$([ $status -gt 128 ] && echo " (signal $((status - 128)))")" ;; esac
    secs=$(echo "$(date +%s.%N) - $start" | bc | sed 's/^\./0./; s/\(\.[0-9]\).*/\1/')
    uncaught=$(grep -c 'Uncaught error in JS' "$log")
    trace=$(grep -o 'NETSURF_JS_TRACE unimplemented .*' "$log" | sed 's/.* unimplemented //' | sort -u | tr '\n' ' ')
    if [ -z "$file" ]; then
        echo "---- $url (${secs}s): $uncaught uncaught script errors"
        # each error with the place it was thrown (the first stack line), in order
        awk '/Uncaught error in JS: /{sub(/.*Uncaught error in JS: /,""); e=$0; getline;
             if ($1 == "at") { sub(/^ *at /,""); sub(/\(http:\/\/[^\/]*\//,"("); e=e "  at " $0 }
             print "   error: " e}' "$log" | cut -c1-220 | head -30
        [ -n "$trace" ] && echo "   unimplemented: $trace"
        [ -n "$exited" ] && echo "   $exited"
        continue
    fi
    total=$((total + 1))
    want=$(grep -o '<meta name="expect-uncaught" content="[0-9]*"' "$file" | grep -o '[0-9]*"$' | tr -d '"')
    want=${want:-0}
    end=$(grep -o 'TEST END [0-9]* [0-9]*' "$log" | tail -1)
    fails=$(grep -o 'TEST FAIL .*' "$log")
    problems=
    [ -n "$end" ] || problems="no TEST END within ${timeout}s"
    [ -z "$fails" ] || problems="${problems:+$problems; }$(echo "$fails" | wc -l) failed checks"
    [ "$uncaught" = "$want" ] || problems="${problems:+$problems; }$uncaught uncaught script errors, expected $want"
    [ -z "$exited" ] || problems="${problems:+$problems; }$exited"
    if [ -z "$problems" ]; then
        passed=$((passed + 1))
        set -- $end
        echo "PASS $name ($3 checks, ${secs}s)"
    else
        echo "FAIL $name: $problems (${secs}s, log $log)"
        [ -n "$fails" ] && echo "$fails" | sed 's/^TEST /   /'
        [ "$uncaught" = "$want" ] || grep -o 'Uncaught error in JS: .*' "$log" | cut -c1-200 | sed 's/^/   /'
    fi
    grep -o 'TEST TODO .*' "$log" | sed 's/^TEST TODO /   todo: /'
    grep -o 'TEST FIXED .*' "$log" | sed 's/^TEST FIXED /   fixed (make it a check): /'
    [ -n "$trace" ] && echo "   unimplemented: $trace"
    [ $keep = 1 ] || [ -n "$problems" ] || rm -f "$log"
done
[ $total -eq 0 ] && exit 0
echo "$passed/$total pages passed"
[ $passed -eq $total ]
