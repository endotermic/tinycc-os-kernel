# patches/

One directory per upstream component named as in `../sources.lock`; inside, one
subdirectory per category and a `series` file with the apply order. `scripts/patch.sh`
applies the series with `git am`, `scripts/export-patches.sh` regenerates it from the
commits of `src/<component>`.

Categories:

* `tinycc/bugfixes`, `tinycc/compatibility` (with GCC and GNU as), `tinycc/features`
* `linux/workarounds` (guarded by `__TINYC__`/`__SIZEOF_INT128__`), `linux/upstreamable`

A future kernel gets its own directory, e.g. `openbsd/workarounds/`, and patches to tcc
that it needs go to the tinycc categories like the Linux ones.

A patch is licensed like the code it modifies (`<component>/LICENSE`, texts in `../LICENSES/`),
not like the rest of the repository; `../REUSE.toml` records this per directory.

The number in a file name is the position in the series, not the category; see
`../docs/FIX-LOG.md` for what each patch fixes and how it was verified.
