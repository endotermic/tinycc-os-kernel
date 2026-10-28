#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * /etc/X11/xdm/Xsetup_0 - run by xdm as root on display :0 before its greeter appears
 * (DisplayManager._0.setup): starts xdm-help, the helper text window, detached, and
 * records its pid in /var/run/xdm/help.pid so that Xstartup can end it when a session
 * starts.  xdm waits for this script, so the helper is a grandchild in its own session.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

int main(void)
{
    pid_t pid;
    FILE *f = fopen("/var/run/xdm/help.pid", "r");
    if (f) {        /* a helper of an earlier greeter still around (it should have died with the server reset) */
        int old;
        if (fscanf(f, "%d", &old) == 1 && old > 1)
            kill(old, SIGTERM);
        fclose(f);
    }
    pid = fork();
    if (pid == 0) {
        setsid();
        pid = fork();
        if (pid == 0) {
            execlp("xdm-help", "xdm-help", (char *)NULL);
            _exit(127);
        }
        f = fopen("/var/run/xdm/help.pid", "w");
        if (f) {
            fprintf(f, "%d\n", (int)pid);
            fclose(f);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
    return 0;
}
