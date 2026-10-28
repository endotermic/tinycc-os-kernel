/* SPDX-License-Identifier: BSD-2-Clause */
/* kdrive-config.h for TinyX built by tcc (userland/tinyx/Makefile): the kdrive server with
 * the fbdev card only; Xvesa (vm86, /dev/mem) is not built.  See dix-config.h. */
#ifndef _KDRIVE_CONFIG_H_
#define _KDRIVE_CONFIG_H_

#include <dix-config.h>

#define KDRIVESERVER 1
#define KDRIVEFBDEV 1

#endif /* _KDRIVE_CONFIG_H_ */
