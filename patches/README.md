# patches/

One directory per upstream component named as in `../sources.lock`; inside, one
subdirectory per category and a `series` file with the apply order. `scripts/patch.sh`
applies the series with `git am`, `scripts/export-patches.sh` regenerates it from the
commits of `src/<component>`.

Categories:

* `tinycc/bugfixes`, `tinycc/compatibility` (with GCC and GNU as), `tinycc/features`
* `linux/workarounds` (guarded by `__TINYC__`/`__SIZEOF_INT128__`), `linux/upstreamable`
* `ncurses/upstreamable`, `tinyx/upstreamable`, `libXfont/upstreamable`, `xdm/upstreamable`: one
  patch each for the guest's programs (the terminfo entry of the Linux console; the X server
  built against xorgproto's headers alone; libXfont's weak stubs under tcc; xdm's greeter
  linked in instead of dlopened, for a static build)
* `jwm/upstreamable` (Xft text without Pango, `__builtin_expect` under tcc), `xfiles/features`
  (Control+A selects all, Control+H toggles the dot files, both reachable through the
  `_CONTROL_GOTO` property) and `xfiles/upstreamable` (fontconfig patterns destroyed once),
  `xmenu/upstreamable` (a build without Imlib2, the same pattern fix, keys without XKB, Down/Up
  with nothing selected, ARGB32 layers whatever the visual),
  `dmenu/compatibility` (bitmap fonts as glyph fallbacks),
  `links/upstreamable` (an `<li>`/`<p>` hidden with display:none no longer hides its siblings),
  `tinyx/features` (the server out of the OOM killer's choice; a shadow framebuffer in RAM when
  reading the frame buffer is slow, as on real video memory) and
  `links/features` (a subset of CSS: stylesheets, simple selectors, and the properties that map
  onto what Links renders: widths, alignment, colours, bold/italic/underline, display:none; and
  CSS fonts: font-family through fontconfig, font-size, FreeType default faces)
* `netsurf/upstreamable` (the OpenSSL 1.1 API with LibreSSL 2.7 and later; talloc's va_copy
  under compilers that report `__GNUC__` 2; `-Wl,--trace` only for a linker that has it;
  nsgenbind's and libnslog's lexer headers generated before the parsers are compiled, for
  parallel builds; libnsfb's X window closing on WM_DELETE_WINDOW and titled with the page, resizable and maximizable,
  with the system's pointer; event listeners: options objects, several per event, each run once,
  libdom's target phase and non-bubbling events; local `.js` files typed as scripts in the
  framebuffer front end; WebIDL members writable and configurable in nsgenbind; DOMContentLoaded
  and the window's listeners; list indexes, HTMLCollection, fragments; request methods other
  than GET and POST in the fetch layer; libdom's mutation events of removeChild, replaceChild
  and nodeValue; CharacterData's data and editing methods; the framebuffer front end's
  scheduler running due callbacks in time order; libdom's event targets initialised and
  referenced; DOMContentLoaded and the target of events at the Window, uncaught errors in
  listeners and promises reported, timer ids from 1, currentScript, createComment; innerHTML of
  markup starting with a comment, Attr as a Node, textContent "", list proxies that a replaced
  item() cannot loop, DOMContentLoaded once, room for deep recursion; the script_timeout option
  honoured; libdom's class list following a changed class attribute; load and error events of
  images; boxes with opacity 0 not drawn) and `netsurf/features` (the JavaScript bindings' Duktape C API implemented on
  QuickJS: ES2023 instead of ES5.1; DOM changes made after load drawn by rebuilding the box tree;
  NETSURF_JS_TRACE, which names the unimplemented WebIDL members a page uses; webapi.js, the web
  APIs written in JavaScript, with native helpers in Window.bnd; XMLHttpRequest and fetch;
  MutationObserver, IntersectionObserver and ResizeObserver; the DOM of Web Components
  polyfills: traversal, interface objects, template content, MessageChannel, link URLs;
  NETSURF_JS_PROBE; template cloning, handleEvent listener objects; style on SVG elements; document.all,
  image state, interface names, IntersectionObserver fixes; CSS custom properties and var() in
  libcss)

A future kernel gets its own directory, e.g. `openbsd/workarounds/`, and patches to tcc
that it needs go to the tinycc categories like the Linux ones.

A patch is licensed like the code it modifies (`<component>/LICENSE`, texts in `../LICENSES/`),
not like the rest of the repository; `../REUSE.toml` records this per directory.

The number in a file name is the position in the series, not the category; see
`../docs/FIX-LOG.md` for what each patch fixes and how it was verified.
