#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * /etc/X11/xdm/Xsession - the session that xdm runs after a login, as the user, with
 * DISPLAY, HOME, USER, PATH and XAUTHORITY set by xdm: the links web browser in graphics
 * mode, its window the size of the screen (as userland/startx.c runs it).  When links
 * ends the session ends, xdm resets the server and shows the greeter again.  A C script,
 * like every script of this system; xdm execs it directly (the kernel's #! line).
 * xdm passes "failsafe" when F1 was held: the same session here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    char mode[32];
    FILE *f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    int w = 0, h = 0;
    if (getenv("HOME"))
        chdir(getenv("HOME"));
    if (f) {
        if (fscanf(f, "%d,%d", &w, &h) == 2 && w > 0 && h > 0)
            snprintf(mode, sizeof mode, "%dx%d", w, h);
        else
            w = 0;
        fclose(f);
    }
    if (w > 0)
        execlp("links", "links", "-g", "-mode", mode, (char *)NULL);
    execlp("links", "links", "-g", (char *)NULL);
    perror("Xsession: links");
    return 1;
}
