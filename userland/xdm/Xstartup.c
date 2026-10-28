#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * /etc/X11/xdm/Xstartup - run by xdm as root right after a successful login, before the
 * session (DisplayManager*startup): takes the helper text window down (Xsetup_0 started
 * it, its pid is in /var/run/xdm/help.pid).  A non-zero status would refuse the login;
 * this script always succeeds.
 */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

int main(void)
{
    FILE *f = fopen("/var/run/xdm/help.pid", "r");
    int pid;
    if (f) {
        if (fscanf(f, "%d", &pid) == 1 && pid > 1)
            kill(pid, SIGTERM);
        fclose(f);
        unlink("/var/run/xdm/help.pid");
    }
    return 0;
}
