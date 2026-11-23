# NetSurf: the web APIs a script-built site needs (plan, target youtube.com)

Plan of 2026-11-21 (phases 0 to 4 done the same day). NetSurf 3.11 runs scripts on QuickJS (patch 0006, a Duktape-API layer, so the
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

## Phase 1: small DOM and window gaps — DONE (2026-11-21)

Patches upstreamable/0016-0018 and features/0019; FIX-LOG C44-C46. 14 test pages, 194 checks,
all pass on the host and (style, window-api, dom-select looked at) in the x86_64 guest.

- nsgenbind (0016): WebIDL operations are writable and configurable, attributes configurable (they
  were fixed, so nothing could replace a stub); needed by everything below and by phase 4.
- Events (0017): `DOMContentLoaded` is fired; `addEventListener` listeners on the window run for
  `load`; `window.dispatchEvent` works (it returned at once for a non-node).
- Bindings (0018): list proxies took numeric keys only, which never reach a Proxy (`childNodes[0]`
  was undefined); `HTMLCollection` length/item/namedItem; fragments get the DocumentFragment
  prototype; `Element.namespaceURI`, `Document.readyState`.
- `webapi.js` (0019), loaded after polyfill.js in every page: selectors (`querySelector(All)`,
  `matches`, `closest`, `getElementsByClassName`; a selector engine in JavaScript: tag/id/class,
  attribute operators with `i`, all combinators, `:not :is :where :has`, structural and form
  pseudo-classes, `nth-*` with `of S`; libcss's matcher is not reachable cleanly), ParentNode and
  ChildNode methods, `children`, `dataset`, `toggleAttribute`, `insertAdjacent*`, an `innerHTML`
  serializer (the getter returned "") and `outerHTML`, `element.style` over the style attribute
  (it returned a fresh empty object; it answers only for properties NetSurf's CSS knows, so
  feature tests do not claim transitions), `getComputedStyle` (display, position, visibility,
  opacity, colours, font size, z-index, width, height from the layout; defaults for the rest),
  `getBoundingClientRect`/offset*/client*, event constructors (Event, CustomEvent, MouseEvent,
  KeyboardEvent and 20 more; `createEvent` by interface with the `init*Event` methods), a
  constructible `EventTarget`, `AbortController`, `Image`, `Option`, `Text`, `createElementNS` for
  HTML, `window.self/top/parent`, `postMessage`, `requestAnimationFrame` (callbacks of a frame
  together, 16 ms), `requestIdleCallback`, `queueMicrotask`, `performance`, `atob`/`btoa`,
  `structuredClone`, `localStorage` (saved to `~/.netsurf/storage/<origin>`, 5 MB) and
  `sessionStorage`, `history` state (pushState does not change the address), `matchMedia`, `screen`,
  `innerWidth` and the like, `navigator.language(s)/onLine`, `document.title/visibilityState/...`,
  `URL`, `URLSearchParams`, `TextEncoder`/`TextDecoder`, `DOMException`, `CSS.escape`.
- `window.__netsurf_native` (Window.bnd): box geometry, viewport and scroll, computed values, the
  storage file.

YouTube (same saved copy): 4 uncaught errors before, 1 after: `new MutationObserver` in the Custom
Elements polyfill (phase 3); no unimplemented WebIDL member is used any more.

Left out or approximate: `scrollTo` and element scrolling do nothing; geometry and computed values
are those of the last layout (a rebuild after a DOM change runs later); `getComputedStyle` without a
box says `display: none`; `CSS.supports` is always false; `history.pushState` keeps state only.

The plan as written before the work:

### Phase 1 plan (2026-11-21)

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

## Phase 2: networking — DONE (2026-11-21)

Patches upstreamable/0020 and features/0021. Test pages `net-xhr` (29 checks) and `net-fetch` (24),
served by `tests/netsurf/server.py` (started by run.sh for pages with
`<meta name="test-server">`, two ports = two origins); all 16 pages pass on the host; net-xhr and net-fetch also in the x86_64 guest (server on the host, reached as 10.0.2.2).

- Fetch layer (0020): `fetch_start` takes the method as the pseudo header `FETCH_METHOD_HEADER`
  ("X-NetSurf-Method: PUT"); the curl fetcher turns it into CURLOPT_CUSTOMREQUEST (CURLOPT_NOBODY
  for HEAD) and resets it on reused handles. The script's request headers went through
  `fetch_start`'s existing headers argument, the text body through its URL-encoded body (curl
  takes the given Content-Type), so the planned llcache changes were not needed: script requests
  bypass the cache and go to the fetch layer directly.
- `request()` (0021, Window.bnd): one request through the fetch layer, redirects followed (at most
  20; 303, and 301/302 after POST, become GET), one callback from the fetch poll with status,
  status text, headers, final URL and the body as an ArrayBuffer, or an error; `abortRequest()`;
  a page's requests end with its script thread (closedown and fini).
- `webapi.js`: `XMLHttpRequest` (asynchronous only: synchronous `open` throws
  InvalidAccessError; readyState 1-4 events, load/error/abort/timeout/loadend and progress events,
  `timeout`, `abort`, `responseType` text/json/arraybuffer/blob, the charset of Content-Type,
  forbidden request headers dropped), `fetch` with `Headers`, `Request`, `Response` (also
  constructed by scripts, `Response.json`), `AbortSignal`, `Blob`, `File`, `FormData` and
  `navigator.sendBeacon`.
- Policy: cookies are stored and sent as for the page's own fetches (also cross-origin, whatever
  `withCredentials`/`credentials` say); a cross-origin response is exposed only when
  Access-Control-Allow-Origin is `*` (without credentials) or the page's origin; no preflight
  requests; pages from `file:` read any response (local development).

Not done: progress while a response arrives (all readyState steps come at the end), binary request
bodies (sent as UTF-8 text) and multipart FormData (sent URL-encoded), streaming bodies
(`response.body` is a placeholder), `responseXML`. YouTube's saved copy still stops at
`new MutationObserver` before its first data request, so the milestone of this phase moves to
phase 3. The BBC start page: 3 uncaught errors (8 after patch 0006).

The plan as written before the work:

### Phase 2 plan (2026-11-21)

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

## Phase 3: observers — DONE (2026-11-21)

Patches upstreamable/0022 (libdom), 0023 (CharacterData) and 0025 (scheduler), features/0024;
FIX-LOG C47-C49. Test pages `observe-mutation` (57 checks) and `observe-geometry` (45); all 18
pages pass on the host and both observer pages in the x86_64 guest.

- `MutationObserver` (0024): `__netsurf_native.observeMutations(node, hook)` (Window.bnd) adds
  capturing libdom listeners for DOMNodeInserted, DOMNodeRemoved, DOMAttrModified and
  DOMCharacterDataModified to each observed node; the C listener calls one JavaScript hook with
  the event's target, related node, attribute name, old and new values and whether a script is
  running (`duk_netsurf_call_depth`, the QuickJS layer's call nesting). `webapi.js` keeps the
  registrations per page and queues the records as the spec does (childList, attributes,
  characterData, subtree, old values, attributeFilter; one record per observer when it observes a
  node and its ancestor; `takeRecords`, `disconnect`, re-`observe` replaces the options). Delivery
  is a microtask when a script made the change and a 0 ms timer for the parser's insertions
  (outside any script). innerHTML, textContent, outerHTML, append/prepend/replaceChildren,
  before/after/replaceWith, insertAdjacentHTML and the insertion of a fragment make one record per
  parent, as in browsers; plain DOM calls make one each.
- libdom (0022): `removeChild` reported every removal twice, `replaceChild` reported nothing,
  `nodeValue` of text changed it silently (C47). CharacterData's `data`, `length` and editing
  methods were empty stubs (0023, C48).
- `IntersectionObserver` and `ResizeObserver` (0024, `webapi.js`): computed from the last layout's
  boxes (`box()` now also gives the content box and the padding) soon after `observe()` and then
  every 100 ms while something is observed, never before the first layout; NetSurf tells scripts
  of no reflow and no scrolling, so they poll. Intersection: root = viewport or an element (its
  border box), `rootMargin` in px or %, thresholds, the first entry for every target; clipping by
  scrolling ancestors is not looked at. Resize: content-box and border-box sizes, no entry for an
  element that stays 0x0.
- The framebuffer scheduler ran timeouts due in the same main-loop run newest first (0025, C49):
  seen in the guest only, where the X surface polls less often than the ram surface of the tests.

Not done: transient registrations (changes inside a subtree removed from an observed node are not
reported until it is observed again), records for the children taken out of an inserted fragment,
`attributeNamespace` (always null). YouTube's saved copy: the Custom Elements polyfill now gets past
`new MutationObserver` and stops at `document.createTreeWalker(document, NodeFilter.SHOW_ALL, ...)`
(no `NodeFilter` constants, no TreeWalker; phase 4 lists both). The milestone "first
`/youtubei/v1/` request" moves to phase 4, which starts with that polyfill. The BBC start page:
3 uncaught errors (unchanged).

The plan as written before the work:

### Phase 3 plan (2026-11-21)

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

## Phase 4: Web Components — DONE, route A (2026-11-21)

Patches upstreamable/0026 (libdom) and 0027, features/0028; FIX-LOG C50-C51. Test pages
`dom-traversal` (30 checks), `dom-interfaces` (45), `events-window` (22), `errors` (3 expected
uncaught errors); all 22 pages pass on the host, and dom-traversal, dom-interfaces and
events-window in the x86_64 guest.

Gate reached with route A (YouTube's own polyfills): on the live site `ytd-app` is upgraded and
has its (ShadyDOM) shadow root, there is no uncaught script error, and the app requests
`/youtubei/v1/player`. Nothing of the app is stamped into the document yet (no masthead, no
`ytd-page-manager`, no video list; in one run before importNode existed the masthead appeared):
finding out why is the first step after this phase. Native Custom Elements (route B) are not
needed for YouTube.

What the polyfills needed, in the order they failed:

- `TreeWalker`, `NodeIterator`, `NodeFilter` (webapi.js; the spec's algorithms).
- `window.Comment`/`CDATASection`/`ProcessingInstruction`, `document.createComment` (a stub),
  constructor links on every interface prototype, the WebIDL constants on the interface objects
  (`Node.ELEMENT_NODE` was undefined: ShadyDOM's own tree walk matched nothing), `HTMLDocument`.
- `EventTarget.prototype` is the prototype nodes and the Window inherit (it was a separate one
  for `new EventTarget()`): ShadyDOM copies `addEventListener` from it and calls it on the window.
- ShadyDOM (patching mode) replaces `firstChild`, `appendChild` and friends on `Node.prototype`
  with its logical tree, which it implements with `TreeWalker`: webapi.js takes the bindings' own
  accessors and methods before page scripts run (kept on `Node.prototype` under a symbol, for the
  later pages of the heap) and walks the real tree with them (selectors, traversal, serializing,
  MutationObserver).
- The Custom Elements polyfill upgrades parsed elements from a `DOMContentLoaded`/`load` listener
  on the window: `DOMContentLoaded` now reaches the window, and events at the window have a target
  (C51).
- `template.content` (and template elements had no `HTMLTemplateElement` prototype),
  `importNode`, `adoptNode`, `MessageChannel`, the URL members of `<a>`/`<area>`, `script.src`,
  `document.currentScript` from the script NetSurf is running, timer ids from 1, the load and
  error events of script elements (scripts added by a script ran, but their `onload` never
  came), element `onerror`.
- Crashes: event targets in libdom (C50), `classList.value` without a class attribute, text of a
  detached node (C50).

Tools: `run.sh --probe JS` (NETSURF_JS_PROBE) runs a script in every page after the web APIs, for
looking into a live site (a timer that throws an Error carrying the state); uncaught errors in
listeners and unhandled promise rejections are now counted (BBC: 2 left, both in a third-party
script). `mirror.py` keeps `=` in paths (YouTube checks its script URL for `/k=`) and gives saved
scripts and style sheets a `.js`/`.css` name: the saved copy used before served YouTube's app
script as `application/octet-stream`, so NetSurf never ran it and the phase 1-3 YouTube numbers
were for the polyfills only. The saved copy cannot follow the app's later module loads; YouTube is
measured live now.

Not done: `ShadowRoot`/`attachShadow` natively (ShadyDOM provides them), `HTMLSlotElement`,
customized built-ins, `NodeIterator`'s adjustment when its reference node is removed, the parsed
`<template>`'s children are rendered until a script reads `content`, `HTMLIFrameElement.contentWindow`
and `document.all` (both used by YouTube, both still stubs).

After phase 4 (2026-11-23): patches upstreamable/0029 and features/0030, FIX-LOG C52. Why the
app stamped nothing, found with `--probe`: its templates were empty (innerHTML of markup starting
with a comment gave no nodes), then Polymer's template parser read `attr.name` (attribute nodes
were bare Nodes), then the stack overflowed (QuickJS's 1 MB; now the process's stack less a
margin), then an endless loop through the list proxies (ShadyDOM replaces `list.item`), and
`textContent`, `DOMContentLoaded` twice, template cloning, `handleEvent` objects and non-callable
handlers. Now the home page builds `ytd-app` with 287 YouTube elements, `ytd-page-manager`,
`ytd-browse` and its rich grid, and calls `/youtubei/v1/guide`, `log_event` and `player`; a search
results page builds 20 `ytd-video-renderer` with their titles, with no uncaught error. For a
signed-out visitor the home page holds only a "Your YouTube history is off" notice, and that
notice is not rendered yet (the rich grid makes no section). Whether all this looks right on
screen is phase 5.

In the guest the search page crashed NetSurf after 5 s: there the app script ran longer than the
fixed 10 s limit (the `script_timeout` option was ignored) and was stopped half-way (0031, C53;
the system `Choices` now allows 120 s). `run.sh` honours `NETSURF_TEST_CPU` (e.g. 25%) to run
the browser slowed down like the guest. With that, the search page loads in the guest (1200 MB,
"Done" after 26 s) without a crash; the window shows only parts of the masthead: the results are
in the document but not drawn (phase 5: YouTube's layout CSS).

The plan as written before the work:

### Phase 4 plan (2026-11-21)

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

## Phase 5: CSS needed for the page to look right — IN PROGRESS

Started 2026-11-23. The page was not drawn for reasons other than CSS first; found with
`run.sh --probe` (layout probes: display and box of the main containers) and fixed: the results
container carried hidden="zClosurez" (document.all was undefined, so YouTube's sanitizer treated
undefined values as document.all; C55), the thumbnails got no src (YouTube's IntersectionObserver
polyfill replaced ours, then its zero-area first entries were ignored), images got no load event,
and a class added later never restyled (libdom's cached class list, C54). Patches upstreamable/0033
and 0034, features/0035; new page `images`. Now YouTube's search page in the guest shows its
results with thumbnails, titles, channels and descriptions; the masthead (logo, sign-in button)
and the duration badges are still wrong. Next: CSS custom properties (YouTube's base style sheet
uses var() about 9,900 times; libcss drops those declarations).

CSS custom properties and var() (features/0037, libcss): the lexer reads `--name` as an identifier;
custom properties and declarations using var() are kept as text and resolved when a node's style
is selected (select/custom.c): the node's custom properties are its parent's plus its own (taken
from the parent's partial style, substituted, cycles and missing names undefined, inherit /
unset / initial), kept on the computed style; var() declarations are substituted, parsed as
inline styles (cached per selection context) and cascaded with their own origin, importance and
specificity (an equal later declaration still wins; a failing one is unset). Shorthands,
fallbacks, nested var(), !important and `style.setProperty("--x")` work. Test page `css-vars`
(21 checks). A box with opacity 0 is no longer drawn (0036, C56: YouTube's hover layers turned
black once their colours resolved). YouTube's search page in the guest: duration badges, grey
secondary text, about as fast as before (29 s); the masthead is still wrong. Not done: calc()
around var() is only as good as libcss's calc(), `@property`, var() in @media or selectors,
custom properties on pseudo-elements (they use the element's), animation of custom properties.

The plan as written before the work:

### Phase 5 plan (2026-11-21)

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
