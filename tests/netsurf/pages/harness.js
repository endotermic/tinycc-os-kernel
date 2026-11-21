/* SPDX-License-Identifier: BSD-2-Clause
 * tests/netsurf/pages/harness.js: the checks of the self-checking test pages.
 * Each check prints one "TEST PASS name" or "TEST FAIL name (detail)" console line,
 * T.done() prints "TEST END <checks> <failed>"; tests/netsurf/run.sh reads them from
 * NetSurf's log.  T.todo() is a check of a known gap (docs/NETSURF-JS.md): it prints
 * "TEST TODO name" while it fails and "TEST FIXED name" once it passes (then make it a
 * T.eq or T.ok); neither counts as a failure.  Plain ES5, so a page can test newer syntax on its own. */
var T = (function () {
    var checks = 0, failed = 0, ended = false;
    function show(v) {
        try { return JSON.stringify(v); } catch (e) { return String(v); }
    }
    return {
        ok: function (name, cond, detail) {
            checks++;
            if (cond) {
                console.log("TEST PASS " + name);
            } else {
                failed++;
                console.log("TEST FAIL " + name + (detail === undefined ? "" : " (" + detail + ")"));
            }
        },
        eq: function (name, got, want) {
            this.ok(name, got === want, "got " + show(got) + ", want " + show(want));
        },
        todo: function (name, got, want) {
            var cond = arguments.length < 3 ? !!got : got === want;
            console.log((cond ? "TEST FIXED " : "TEST TODO ") + name +
                (cond || arguments.length < 3 ? "" : " (got " + show(got) + ", want " + show(want) + ")"));
        },
        /* runs fn and fails the check if it throws */
        run: function (name, fn) {
            try { fn(); } catch (e) { this.ok(name, false, "threw " + e); }
        },
        done: function () {
            if (ended) return;
            ended = true;
            console.log("TEST END " + checks + " " + failed);
        }
    };
})();
