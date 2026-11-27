/* SPDX-License-Identifier: BSD-2-Clause */
/* tcc-btf.h - included into kernel/bpf/btf.c only (scripts/build.sh kernel-android passes
 * CFLAGS_btf.o=-include this file).  btf_parse_vmlinux() refers to __start_BTF/__stop_BTF,
 * the bounds of the .BTF section, which the linker script emits only with
 * CONFIG_DEBUG_INFO_BTF (pahole); its one caller, bpf_get_btf_vmlinux(), is behind
 * IS_ENABLED(CONFIG_DEBUG_INFO_BTF), but the function itself is global and stays.  Weak
 * stand-ins let the kernel link; the function is never called. */
char __start_BTF[1] __attribute__((weak));
char __stop_BTF[1] __attribute__((weak));
