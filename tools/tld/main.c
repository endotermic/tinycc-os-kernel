/* SPDX-License-Identifier: BSD-2-Clause */
#include "tld.h"

static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

int main(int argc, char **argv)
{
    const char *tool = base(argv[0]);
    if (argc > 1 && !strncmp(argv[1], "--tool=", 7)) { tool = argv[1] + 7; argv[1] = argv[0]; argv++; argc--; }
    /* allow names like "x86_64-linux-gnu-nm" or "tld-nm" */
    { const char *d = strrchr(tool, '-'); if (d) tool = d + 1; }
    progname = tool;
    if (!strcmp(tool, "nm")) return nm_main(argc, argv);
    if (!strcmp(tool, "objcopy") || !strcmp(tool, "strip")) return objcopy_main(argc, argv);
    if (!strcmp(tool, "readelf")) return readelf_main(argc, argv);
    if (!strcmp(tool, "ar")) return ar_main(argc, argv);
    progname = "ld";
    return ld_main(argc, argv);
}
