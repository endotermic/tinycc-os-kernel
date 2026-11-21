# NetSurf: the web APIs a script-built site needs (plan, target youtube.com)

Plan of 2026-11-21 (phase 0 done the same day). NetSurf 3.11 runs scripts on QuickJS (patch 0006, a Duktape-API layer, so the
nsgenbind bindings are unchanged), redraws DOM changes made after load (0009/0010) and dispatches
events correctly (0012/0013). youtube.com still shows a blank page. This file lists what it needs,
in the order planned. Mark a phase done here when it is committed and verified (host and guest,
x86_64 and i386).

## Why youtube.com is blank

The script errors form a chain:

1. `ReferenceError: 'Image' is not defined`: there is no `new Image()`.
2. `Error: argument 2 (capture) requires a bool`: fixed by 0013 (options objects).
3. `TypeError: cannot read property 'polyfillWrapFlushCallback' of undefined`: YouTube sees no
   native Web Components, loads its own polyfill (webcomponentsjs: Custom Elements + ShadyDOM) and
   the polyfill failed earlier; the app then stops.

Behind those, the app is a single-page app: the HTML is a shell, every list of videos arrives as
JSON from scripts (`/youtubei/v1/...`, POST with JSON bodies and custom headers). NetSurf 3.11 has
none of:

- `XMLHttpRequest` or `fetch`;
- Custom Elements or Shadow DOM;
- `MutationObserver`, `IntersectionObserver`, `requestAnimationFrame`, `localStorage`;
- several small DOM calls the bindings declare in WebIDL but do not implement (survey below).

Video playback is a separate problem (phase 7).

## What exists (survey of 2026-11-21)

- `Window.bnd`: `setTimeout`/`setInterval`/`clear*` on the GUI scheduler (`guit->misc->schedule`),
  `alert`, `document`, `location`, `navigator`, `console`. No `requestAnimationFrame`, storage,
  `performance`, `atob`/`btoa`.
- `Document.bnd`: `getElementById`, `getElementsByTagName`, `createElement(NS)`, `createTextNode`,
  `createDocumentFragment`, `createEvent`, `cookie`, `write`. No `querySelector(All)`,
  `getElementsByClassName`.
- `Element.bnd`: attributes, `id`, `className`, `classList`, `innerHTML`, element siblings.
  `querySelector(All)`, `matches`, `closest`, `getBoundingClientRect`, `dataset` are only in the
  IDL (nsgenbind emits no-op stubs for unbound IDL members).
- `HTMLElement.bnd`: `style` (partial), `on*` handlers. No `getComputedStyle`.
- QuickJS: full ES2023, Promises; `duk_quickjs.c` already runs `JS_ExecutePendingJob` after each
  call into JS, so microtasks work. `Proxy` is used by `generics.js`.
- libdom raises mutation events (`DOMNodeInserted`, `DOMNodeRemoved`, `DOMAttrModified`,
  `DOMCharacterDataModified`): the base for `MutationObserver` and Custom Element reactions.
- Fetching: `hlcache_handle_retrieve`/`llcache_handle_retrieve` take a URL, a referer and
  `llcache_post_data` (urlencoded or multipart), are asynchronous and report
  `LLCACHE_EVENT_HAD_HEADERS`/`HAD_DATA`/`DONE`/`ERROR`. Request headers are fixed in
  `fetchers/curl.c` (Accept-Language, Accept-Charset, cookies); there is no per-request header API
  and no raw (JSON) POST body.
- libcss parses flexbox and NetSurf 3.11 lays it out; CSS custom properties (`var()`) and grid are
  not supported.
- `<script type="module">` is not handled (classic scripts only).

## Ground rules

- Patches go into `src/netsurf` as commits, exported to `patches/netsurf/features/` (new APIs) or
  `upstreamable/` (bug fixes), one patch per phase or API, each with a FIX-LOG.md entry
  (C42 onward) and the licence note in `patches/netsurf/LICENSE` + `REUSE.toml`.
- Native code where the engine owns the state (network, layout, timers, storage, DOM hooks);
  JavaScript (`polyfill.js`, loaded into every heap) where a spec-sized API can sit on top of a
  small native one (`fetch` on XHR, `URL`, `TextEncoder`, selector matching if C is too costly).
- Every API gets a self-checking page in `tests/netsurf/pages/` (checks through `harness.js`;
  `T.todo` for a known gap), run by `tests/netsurf/run.sh` on the host. Guest check (QEMU
  driven over QMP: screendumps, keys, pointer) at the end of each phase. A tcc bug found on the
  way goes into `patches/tinycc` and the tcc fix log as usual.
- A built tree is about 4 min; after an `.idl` edit delete `build/Linux-framebuffer/duktape`
  in the incremental tree (nsgenbind output does not depend on the IDL files).

## Phase 0: test harness — DONE (2026-11-21)

- `tests/netsurf/run.sh [--arch A] [--timeout S] [--keep] [PAGE|URL...]` opens each page of
  `tests/netsurf/pages/` with the built browser (`-f ram`, no window, 1 GB memory cap) and reads
  its log: a page passes when it printed `TEST END`, no check failed and its count of uncaught
  script errors is the one its `<meta name="expect-uncaught">` names. `T.todo` checks list the
  known gaps (`todo:`, and `fixed:` once one passes). Resources and fonts come through
  `$NETSURFRES`; `/etc/ssl` is hidden in a private mount namespace (`unshare -rm`) because the
  browser's LibreSSL aborts on the host's OpenSSL 3 `openssl.cnf`. 7 pages: `es`, `timers`,
  `events-order`, `events-options`, `events-load`, `dom-dynamic`, `stress`; all pass.
- For a URL it reports the uncaught errors with the place each was thrown.
  `tests/netsurf/mirror.py URL DIR` saves a page with its scripts and style sheets for offline
  runs (served by `python3 -m http.server --bind 127.0.0.1`: the browser's curl tries `::1` for
  `localhost`).
- `NETSURF_JS_TRACE=1` (patch features/0014): the stubs nsgenbind generates for unimplemented
  WebIDL members call `dukky_unimplemented("Interface.member")`, which reports each name once on
  stderr and in the log; `run.sh` sets it and lists the names.
- Fixes found on the way: local `.js` files were typed `text/html` and refused as scripts
  (upstreamable/0015, FIX-LOG C43); `removeEventListener` without options missed a listener added
  with `{passive: true}` (listener identity is type, callback, capture: 0013 amended, C42).
- Known gaps the pages found (phase 1): `Element.tagName` is unimplemented; no
  `DOMContentLoaded` is fired; at load only `window.onload`/`<body onload>` run, not
  `addEventListener("load")` listeners on window.

YouTube (start page saved 2026-11-21, NetSurf's user agent): it sends NetSurf its ES5 app build
and its own polyfills (`fetch-polyfill`, `intersection-observer`, `webcomponents-all-noPatch`,
`web-animations-next-lite`, `scheduler`), so phases 2-4 may partly be covered by them. Measured:
4 uncaught errors, each mapping to phase 1:

| Error | Where | Missing |
|---|---|---|
| `'Image' is not defined` | inline `ytPrewarmYtimg` | `Image` constructor |
| `not a function` | web-animations 78:434 | `getContext` on `createElementNS(XHTML, "canvas")`: elements made with createElementNS get no HTML interface |
| ` is not a constructor` | webcomponents 20:665 | `new Event("click")` |
| `cannot read property 'forEach' of undefined` | spf.js 11:238 | `document.querySelectorAll` (stub returns undefined) |

Unimplemented members used: `Document.querySelectorAll()`, `Window.postMessage()`, `Window.self`.

## Phase 1: small DOM and window gaps (medium)

Native (`.bnd`):

- Found by phase 0: `Element.tagName`; fire `DOMContentLoaded` at the document (html.c only
  has the comment) and run `addEventListener("load")` listeners on window (js_fire_event only
  calls `window.onload` or `<body onload>`); `createElementNS` with the XHTML namespace must give
  the HTML interfaces (`HTMLCanvasElement` etc.); `window.self`, `window.postMessage` (a message
  event to the same window, from the scheduler).
- `Image` constructor (`new Image(w, h)` → `createElement("img")`), `Option`; `Event` and
  `CustomEvent` constructors with `bubbles`/`cancelable`/`detail`.
- `querySelector`, `querySelectorAll`, `matches`, `closest` on Document/Element/DocumentFragment:
  parse the selector with libcss (`css__parse_selector`-level API, or a stylesheet with one rule)
  and match through the existing `css_select_handler` that NetSurf already implements for libdom
  nodes (`content/handlers/css/select.c`). Fallback if libcss's internals cannot be reached
  cleanly: a selector matcher in `polyfill.js` (tag, id, class, attribute, descendant/child,
  `:not`, comma lists), which is enough for YouTube's polyfills.
- `getElementsByClassName`, `children`, `dataset` (Proxy over `data-*`), `append`/`prepend`/
  `remove`/`before`/`after`/`replaceWith`, `toggleAttribute`, `insertAdjacentHTML`.
- `getBoundingClientRect`, `offsetWidth/Height/Top/Left`, `clientWidth/Height`, `scrollTop`:
  read the box attached to the node (box tree user data), forcing a layout if the content is
  dirty. Zero rectangle for nodes with no box.
- `getComputedStyle`: a read-only `CSSStyleDeclaration` over the node's `css_computed_style`
  (display, visibility, width/height, position, colours; more on demand).
- `requestAnimationFrame`/`cancelAnimationFrame`: the window scheduler at 16 ms, batched so all
  callbacks of a frame run together before one redraw.
- `localStorage`/`sessionStorage`: per origin, in memory; `localStorage` written to
  `~/.netsurf/storage/<origin>` on change (a small key/value file), size cap 5 MB.
- `performance.now()`, `atob`/`btoa`, `queueMicrotask`, `structuredClone` (JSON-level),
  `matchMedia` (evaluate width/height/prefers-* against the window; listeners never fire),
  `navigator.userAgent/language/languages/cookieEnabled/onLine`, `screen`, `devicePixelRatio`,
  `innerWidth/innerHeight`, `scrollX/scrollY`, `scrollTo`.

JavaScript (`polyfill.js`): `URL`, `URLSearchParams`, `TextEncoder`/`TextDecoder` (UTF-8),
`AbortController`/`AbortSignal`, `DOMException`.

Done when: each has a passing test page; YouTube's first error is no longer `Image`.

## Phase 2: networking (large)

1. Fetch layer: extend `llcache_post_data` with a raw body type (bytes + content type) and add an
   optional extra-headers list to `hlcache_handle_retrieve`/`llcache_handle_retrieve` →
   `fetch_start` → `fetchers/curl.c` (`curl_slist_append`). Methods besides GET/POST (PUT, DELETE,
   HEAD) via `CURLOPT_CUSTOMREQUEST`. Responses of script requests are not cached as contents of
   the page (fetch them with `HLCACHE_RETRIEVE_SNIFF_TYPE` off and a "raw data" content type, or
   straight on llcache).
2. `XMLHttpRequest` in C (`XMLHttpRequest.bnd` + `xhr.idl`): `open`/`send`/`abort`,
   `setRequestHeader`, `getResponseHeader`/`getAllResponseHeaders`, `status`/`statusText`,
   `readyState` + `onreadystatechange`, `responseText`, `responseType` = `""`/`"text"`/`"json"`/
   `"arraybuffer"`, `response`, `withCredentials`, `timeout`, and the `load`/`error`/`abort`/
   `timeout`/`loadend`/`progress` events. Asynchronous only (synchronous XHR throws). The object is
   kept alive (a reference in the heap's stash) while a request is in flight; requests of a page
   are aborted when its heap is destroyed (same place the window scheduler is cleared).
3. `fetch` in `polyfill.js` on top of XHR: `Request`, `Response` (`text`, `json`, `arrayBuffer`,
   `ok`, `status`, `headers`, `clone`), `Headers`, `AbortSignal` support.
4. Policy: same-origin requests send cookies; cross-origin requests go out without cookies unless
   `withCredentials`/`credentials: "include"`, and the response is exposed only if
   `Access-Control-Allow-Origin` allows it (no preflight: a simplification, recorded here).
5. `navigator.sendBeacon` as a fire-and-forget POST (YouTube logs through it).

Done when: test pages (GET text/JSON, POST JSON with headers, abort, timeout, 404, cross-origin
refused/allowed) pass against the local http.server and a small Python test endpoint; YouTube's
local copy issues its first `/youtubei/v1/` request.

Risk: curl in the guest lacks eventfd (built with `ac_cv_func_eventfd=no`); the existing fetcher
loop is used, so no change, but many parallel requests may expose fetcher limits
(`max_fetchers`).

## Phase 3: observers (medium)

- `MutationObserver`: native registration list per heap; libdom mutation event listeners
  (registered once per document by dukky) append `MutationRecord`s (childList, attributes with
  old value, characterData, subtree filtering) and schedule one delivery as a microtask
  (`JS_EnqueueJob`). `takeRecords`, `disconnect`.
- `IntersectionObserver`: after each reflow and on scroll, compute intersection of each observed
  node's box with the viewport (or root) and the thresholds; queue entries when the state changes;
  deliver from the scheduler. First version: root = viewport, `rootMargin` parsed in px only.
- `ResizeObserver`: compare box sizes after reflow, same delivery path.

Done when: test pages pass (including records for script-inserted subtrees and lazy-loading images
via IntersectionObserver).

## Phase 4: Web Components (large; the decision gate of this plan)

Two routes; try A first because YouTube ships it.

A. Make YouTube's own polyfills run (Custom Elements polyfill + ShadyDOM + ShadyCSS). They need:
   phase 1-3 APIs (MutationObserver above all), configurable accessor properties on
   `Node.prototype`/`Element.prototype` (`childNodes`, `firstChild`, `parentNode`, `innerHTML`,
   `textContent`, `appendChild`... must be redefinable with `Object.defineProperty` and callable
   through saved descriptors), `document.createTreeWalker`/`NodeFilter`, `Element.prototype`
   methods reachable from `HTMLElement` subclasses, and `Reflect.construct`-compatible element
   wrappers (a class `extends HTMLElement` whose constructor returns the wrapper of an existing
   libdom node). Work: find each failing polyfill call with the phase 0 trace and fix the binding.
B. Native: a per-document `CustomElementRegistry` (`define`, `get`, `whenDefined`, `upgrade`);
   upgrade on `define`, on `createElement`, on parser insertion and on insertion via the mutation
   hooks; reactions `connectedCallback`/`disconnectedCallback`/`attributeChangedCallback`
   (observedAttributes) delivered as custom-element reaction microtasks. Shadow DOM natively needs
   libdom and the box builder to know about shadow trees and slots: too large; instead
   `attachShadow` returns a ShadyDOM-style "logical" root whose children are rendered as the host's
   light children (what ShadyDOM does), styles scoped by ShadyCSS rewriting.

Gate: if route A gets the YouTube app booting (`ytd-app` upgraded, no fatal errors), stop there
and only do the native Custom Elements of route B if other sites need it.

## Phase 5: CSS needed for the page to look right (medium-large, libcss)

Even with the scripts running, YouTube's layout relies on CSS custom properties (`--yt-*`,
`var()` everywhere, also set from script through `style.setProperty`). libcss has no `var()`.
Plan: parse custom properties as opaque token lists stored per computed style (inherited), and
substitute `var()` at cascade time before the property parser runs (the CSS Variables spec's
"computed-value time" substitution, minus animation). Then `calc()` gaps if any, `gap` for flex.
Grid stays out; whether YouTube's video grid needs it is checked when this phase starts.

Done when: libcss tests for `var()` (fallbacks, inheritance, cycles) pass and a local page with
YouTube-like variable use renders like a desktop browser's screenshot.

## Phase 6: performance and memory (ongoing)

- The 10.9 MB app script compiles in ~5.9 s on the host (tcc-built QuickJS, no optimisation
  flags tcc lacks). Options: cache compiled bytecode per script URL+ETag on disk
  (`JS_WriteObject`/`JS_ReadObject`), raise the interrupt budget so long scripts are not killed,
  run the parse off the redraw path (show the page before scripts finish).
- Guest RAM: the tests run with 640 MB; measure heap peak with the local YouTube copy, add a
  `JS_SetMemoryLimit` per heap so a page cannot take the whole guest.
- Relayout cost: rebox after every DOM change (0009/0010) is coarse; batch changes per frame
  (same tick as `requestAnimationFrame`) before rebuilding the box tree.

## Phase 7: video (out of scope for now)

YouTube streams DASH segments (VP9/AV1/Opus, H.264/AAC) through Media Source Extensions. That needs
`<video>` layout and drawing, `MediaSource`/`SourceBuffer`, a demuxer and decoders (ffmpeg or
libvpx/dav1d/opus built with tcc, each a large build with likely tcc bugs), audio output (ALSA is
in the kernel from hardware group 6) and A/V sync. A cheaper path to "a video plays" is handing the
stream to an external player, which the guest does not have either. Decide after phase 5;
until then the target is browsing (home, search, video page with title, description, comments).

## Milestones

| | Target | Phases |
|---|---|---|
| M1 | YouTube's scripts run without uncaught errors up to their first network request | 0, 1 |
| M2 | the home page (local copy, then live) shows the list of videos with thumbnails | 2, 3, 4 |
| M3 | it looks close to a desktop browser's rendering; search works | 5 |
| M4 | acceptable speed in the guest (page usable in under ~15 s at 640 MB) | 6 |

Side benefit: each phase also helps ordinary sites (lazy-loaded images, `fetch`-driven pages,
consent banners, menus built with `querySelector`), which is worth checking with the BBC page used
for patch 0006 (script errors 52 → 8 then) after every phase.
