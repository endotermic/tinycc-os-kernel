# kernels/

Per-kernel build inputs. `scripts/build.sh` reads `kernels/<name>/` for the kernel named by
the component; today only `linux/`:

* `minimal.config`: Kconfig fragment applied on top of `make tinyconfig` (x86_64, serial
  console, VGA/framebuffer console with keyboard, initramfs, the few options the busybox
  shell needs). The single source of truth
  for the configuration; `build.sh` reports every line that `olddefconfig` dropped.
* `tcc.config`: fragment applied only for the tcc build (`SECTION_MISMATCH_WARN_ONLY`,
  because tcc does not inline `__init`-only `static inline` helpers).
* `tcc-kcc.c`: the `CC=` wrapper that presents tcc to Kbuild (GCC identity macros,
  `-U__OPTIMIZE__`, `-Wunsupported` under `-Werror`, the `-dumpmachine`/`-Wa,--version`/
  `-print-file-name` answers; nothing goes to gcc). `tcc-hostcc.c` is HOSTCC, `objtool-stub.c`
  OBJTOOL. All three are C programs: `scripts/build.sh kernel` compiles them with the host tcc
  into `build/<arch>/kcc/`, and the self-hosting image carries the sources as `tcc -run`
  scripts (`/usr/bin/tcc-kcc` ...), the only form of script that system has.

Adding a kernel: create `kernels/<name>/` with its configuration and wrapper, a
`patches/<name>/` series, an entry in `sources.lock`, and a `step_kernel` variant in
`scripts/build.sh` selected by the component.
