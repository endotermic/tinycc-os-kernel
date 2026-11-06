/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tcc-canary.h: given to every kernel compile through KCFLAGS=-include (scripts/build.sh
 * step_kernel, userland/selfhost/build-kernel.c), so that the kernel tree needs no patch
 * for it.
 *
 * include/linux/seqlock.h declares __scoped_seqlock_bug() as an extern canary: the
 * __always_inline __scoped_seqlock_next() calls it in the ss_done arm of its switch, which
 * an optimizing compiler proves unreachable (the scoped_seqlock_read() loop condition
 * excludes ss_done right before the increment), so the reference disappears and the
 * link succeeds.  tcc keeps the state in the struct on the stack across the loop and
 * cannot prove it, so the reference stays and the vmlinux link fails on an undefined
 * symbol.  The kernel itself uses this empty stub for GCC before 9, KASAN and
 * UBSAN_ALIGNMENT ("we don't care about code quality for such builds"); this header
 * gives tcc the same stub.  The later extern declaration in seqlock.h takes the static
 * linkage of this definition (C11 6.2.2p4); a translation unit that never calls it
 * emits nothing.  Only this one symbol: every other canary must still fail the link.
 */
static inline void __scoped_seqlock_bug(void) { }
