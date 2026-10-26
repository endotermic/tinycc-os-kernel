#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * /usr/share/udhcpc/default.script of the busybox image: the program udhcpc runs on every
 * DHCP event, a C script (every script of this system is C run by tcc -run).  udhcpc passes
 * the event as the argument (deconfig, bound, renew, nak, leasefail) and the lease in the
 * environment: interface, ip, subnet (the netmask), router (space-separated), dns, domain.
 * It configures the interface with busybox's ifconfig and route and writes /etc/resolv.conf.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static int run(char *const argv[])
{
    pid_t pid;
    int st;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        return 127;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return 127;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

int main(int argc, char **argv)
{
    const char *event = argc > 1 ? argv[1] : "", *ifc = getenv("interface"),
               *ip = getenv("ip"), *mask = getenv("subnet"), *routers = getenv("router"),
               *dns = getenv("dns"), *domain = getenv("domain");
    char *s, *tok;
    if (!ifc)
        return 1;
    if (strcmp(event, "deconfig") == 0) {
        /* the interface up with no address, so that udhcpc can send on it */
        run((char *[]){ "ifconfig", (char *)ifc, "0.0.0.0", "up", NULL });
        return 0;
    }
    if (strcmp(event, "bound") != 0 && strcmp(event, "renew") != 0)
        return 0;   /* nak, leasefail: nothing to configure */
    if (!ip)
        return 1;
    run((char *[]){ "ifconfig", (char *)ifc, (char *)ip, "netmask", (char *)(mask ? mask : "255.255.255.0"), "up", NULL });
    if (routers && *routers) {
        /* the old default route goes, the first router becomes the new one */
        while (run((char *[]){ "route", "del", "default", "dev", (char *)ifc, NULL }) == 0)
            ;
        s = strdup(routers);
        if ((tok = strtok(s, " ")))
            run((char *[]){ "route", "add", "default", "gw", tok, "dev", (char *)ifc, NULL });
        free(s);
    }
    if (dns && *dns) {
        FILE *f = fopen("/etc/resolv.conf", "w");
        if (f) {
            if (domain && *domain)
                fprintf(f, "search %s\n", domain);
            s = strdup(dns);
            for (tok = strtok(s, " "); tok; tok = strtok(NULL, " "))
                fprintf(f, "nameserver %s\n", tok);
            free(s);
            fclose(f);
        }
    }
    return 0;
}
