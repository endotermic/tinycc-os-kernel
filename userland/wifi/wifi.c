#!/usr/bin/tcc -run
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * wifi: join a wireless network from the shell, a C script (every script of this system is C
 * run by tcc -run).  It drives iw, wpa_supplicant, wpa_cli and udhcpc (scripts/build.sh wifi).
 *
 *   wifi                       the wireless interfaces and the state of the connection
 *   wifi scan                  the networks in range: signal, security, name
 *   wifi connect NAME [PASS]   join NAME (WPA2/WPA3 with PASS, an open network without), then
 *                              get an address by DHCP; the settings stay in
 *                              /etc/wpa_supplicant/IFACE.conf
 *   wifi off                   leave the network and take the interface down
 *   -i IFACE (before the command): another interface than the first wlan*
 *
 * Needs root (su): it configures the interface.  The firmware of the card comes from the
 * ISO's firmware image (/lib/firmware); dmesg names a missing file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define CTRL_DIR "/var/run/wpa_supplicant"
#define CONF_DIR "/etc/wpa_supplicant"

static char iface[64];

/* run argv; output to /dev/null when quiet; the exit status */
static int run(char *const argv[], int quiet)
{
    pid_t pid;
    int st;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        return 127;
    if (pid == 0) {
        if (quiet) {
            int null = open("/dev/null", O_RDWR);
            dup2(null, 1);
            dup2(null, 2);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "wifi: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return 127;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* the output of argv (at most size-1 bytes, NUL-terminated); the exit status */
static int capture(char *const argv[], char *buf, size_t size)
{
    int fd[2], st;
    size_t n = 0;
    ssize_t r;
    pid_t pid;
    buf[0] = 0;
    if (pipe(fd) < 0)
        return 127;
    fflush(NULL);
    pid = fork();
    if (pid < 0)
        return 127;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(fd[1], 1);
        dup2(null, 2);
        close(fd[0]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);
    while ((r = read(fd[0], buf + n, size - 1 - n)) > 0 || (r < 0 && errno == EINTR))
        if (r > 0 && (n += r) == size - 1)
            break;
    buf[n] = 0;
    close(fd[0]);
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return 127;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* the value of "key=value" lines in text ("" if absent); a static buffer */
static char *field(const char *text, const char *key)
{
    static char val[256];
    size_t k = strlen(key);
    const char *p = text;
    val[0] = 0;
    while (p && *p) {
        if (strncmp(p, key, k) == 0 && p[k] == '=') {
            size_t n = strcspn(p + k + 1, "\n");
            if (n >= sizeof val)
                n = sizeof val - 1;
            memcpy(val, p + k + 1, n);
            val[n] = 0;
            break;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return val;
}

static int first_wlan(char *out, size_t size)
{
    DIR *d = opendir("/sys/class/net");
    struct dirent *e;
    char best[64] = "";
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (strncmp(e->d_name, "wlan", 4) == 0 && (!best[0] || strcmp(e->d_name, best) < 0))
            snprintf(best, sizeof best, "%s", e->d_name);
    closedir(d);
    snprintf(out, size, "%s", best);
    return best[0] != 0;
}

static int supplicant_running(void)
{
    char path[128];
    struct stat st;
    snprintf(path, sizeof path, CTRL_DIR "/%s", iface);
    return stat(path, &st) == 0 && run((char *[]){ "wpa_cli", "-p", CTRL_DIR, "-i", iface, "ping", NULL }, 1) == 0;
}

static char *status_text(void)
{
    static char buf[4096];
    if (!supplicant_running() || capture((char *[]){ "wpa_cli", "-p", CTRL_DIR, "-i", iface, "status", NULL }, buf, sizeof buf) != 0)
        buf[0] = 0;
    return buf;
}

static int need_root(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "wifi: configuring %s needs root: su -c 'wifi ...'\n", iface);
        return 0;
    }
    return 1;
}

static int cmd_status(void)
{
    char *s, path[128], drv[128] = "?";
    ssize_t n;
    snprintf(path, sizeof path, "/sys/class/net/%s/device/driver", iface);
    if ((n = readlink(path, drv, sizeof drv - 1)) > 0) {
        drv[n] = 0;
        if (strrchr(drv, '/'))
            memmove(drv, strrchr(drv, '/') + 1, strlen(strrchr(drv, '/')));
    }
    s = status_text();
    if (!s[0]) {
        printf("%s (%s): not connected\n", iface, drv);
        return 0;
    }
    printf("%s (%s): %s", iface, drv, field(s, "wpa_state"));
    if (strcmp(field(s, "wpa_state"), "COMPLETED") == 0) {
        printf(", network \"%s\"", field(s, "ssid"));
        printf(", %s", field(s, "key_mgmt"));
        printf(", %s MHz", field(s, "freq"));
        if (field(s, "ip_address")[0])
            printf(", address %s", field(s, "ip_address"));
    }
    printf("\n");
    return 0;
}

static int iface_up(void)
{
    if (run((char *[]){ "ifconfig", iface, "up", NULL }, 0) != 0) {
        fprintf(stderr, "wifi: cannot bring %s up (rfkill, or no firmware: see dmesg)\n", iface);
        return 0;
    }
    return 1;
}

/* iw's scan: one line per network, strongest first */
static int cmd_scan(void)
{
    static char out[1 << 18];
    struct net { char ssid[64], sec[16]; double sig; int freq; } nets[128];
    int n = 0, i, j, tries;
    char *p, *line;
    if (!need_root() || !iface_up())
        return 1;
    for (tries = 0; tries < 5; tries++) {      /* "Device or resource busy" while a scan runs */
        if (capture((char *[]){ "iw", "dev", iface, "scan", NULL }, out, sizeof out) == 0)
            break;
        sleep(1);
    }
    if (tries == 5) {
        fprintf(stderr, "wifi: the scan on %s failed\n", iface);
        return 1;
    }
    for (p = out; (line = strsep(&p, "\n")); ) {
        while (*line == '\t' || *line == ' ')
            line++;
        if (strncmp(line, "BSS ", 4) == 0 && n < 128) {
            memset(&nets[n], 0, sizeof nets[n]);
            strcpy(nets[n].sec, "open");
            n++;
        } else if (n == 0) {
            continue;
        } else if (strncmp(line, "signal: ", 8) == 0) {
            nets[n - 1].sig = atof(line + 8);
        } else if (strncmp(line, "freq: ", 6) == 0) {
            nets[n - 1].freq = atoi(line + 6);
        } else if (strncmp(line, "SSID: ", 6) == 0) {
            snprintf(nets[n - 1].ssid, sizeof nets[n - 1].ssid, "%s", line + 6);
        } else if (strncmp(line, "RSN:", 4) == 0 || strncmp(line, "WPA:", 4) == 0) {
            if (strcmp(nets[n - 1].sec, "WPA3") != 0)
                strcpy(nets[n - 1].sec, line[0] == 'R' ? "WPA2" : "WPA");
        } else if (strstr(line, "Authentication suites:") && strstr(line, "SAE")) {
            strcpy(nets[n - 1].sec, strstr(line, "PSK") ? "WPA2/WPA3" : "WPA3");
        }
    }
    for (i = 1; i < n; i++)                     /* strongest first */
        for (j = i; j > 0 && nets[j].sig > nets[j - 1].sig; j--) {
            struct net t = nets[j];
            nets[j] = nets[j - 1];
            nets[j - 1] = t;
        }
    printf("%-9s %-6s %-10s %s\n", "SIGNAL", "MHZ", "SECURITY", "NAME");
    for (i = 0; i < n; i++)
        printf("%5.0f dBm %-6d %-10s %s\n", nets[i].sig, nets[i].freq, nets[i].sec,
               nets[i].ssid[0] ? nets[i].ssid : "(hidden)");
    if (n == 0)
        printf("(no networks found)\n");
    return 0;
}

static int cmd_off(void)
{
    if (!need_root())
        return 1;
    if (supplicant_running())
        run((char *[]){ "wpa_cli", "-p", CTRL_DIR, "-i", iface, "terminate", NULL }, 1);
    run((char *[]){ "ifconfig", iface, "0.0.0.0", NULL }, 1);
    run((char *[]){ "ifconfig", iface, "down", NULL }, 1);
    printf("%s: off\n", iface);
    return 0;
}

static int cmd_connect(const char *ssid, const char *pass, int dhcp)
{
    char conf[128], *s = "";
    FILE *f;
    size_t i;
    int t;
    if (!need_root())
        return 1;
    if (pass && (strlen(pass) < 8 || strlen(pass) > 63 || strpbrk(pass, "\"\n"))) {
        fprintf(stderr, "wifi: the password must have 8 to 63 characters, without '\"'\n");
        return 2;
    }
    if (strlen(ssid) == 0 || strlen(ssid) > 32) {
        fprintf(stderr, "wifi: a network name has 1 to 32 bytes\n");
        return 2;
    }
    mkdir(CONF_DIR, 0755);
    mkdir("/var/run", 0755);
    snprintf(conf, sizeof conf, CONF_DIR "/%s.conf", iface);
    if (!(f = fopen(conf, "w"))) {
        fprintf(stderr, "wifi: %s: %s\n", conf, strerror(errno));
        return 1;
    }
    chmod(conf, 0600);
    fprintf(f, "# written by wifi connect\nctrl_interface=%s\nupdate_config=0\n", CTRL_DIR);
    /* the name in hex: any byte is allowed in an SSID.  With a password: WPA2-Personal and
       WPA3-Personal (SAE), whichever the network offers, with protected management frames
       when both sides can */
    fprintf(f, "network={\n\tssid=");
    for (i = 0; i < strlen(ssid); i++)
        fprintf(f, "%02x", (unsigned char)ssid[i]);
    fprintf(f, "\n\tscan_ssid=1\n");
    if (pass)
        fprintf(f, "\tkey_mgmt=WPA-PSK WPA-PSK-SHA256 SAE\n\tpsk=\"%s\"\n\tieee80211w=1\n", pass);
    else
        fprintf(f, "\tkey_mgmt=NONE\n");
    fprintf(f, "}\n");
    fclose(f);
    if (supplicant_running())
        run((char *[]){ "wpa_cli", "-p", CTRL_DIR, "-i", iface, "terminate", NULL }, 1);
    for (t = 0; t < 20 && supplicant_running(); t++)
        usleep(100000);
    if (!iface_up())
        return 1;
    if (run((char *[]){ "wpa_supplicant", "-B", "-q", "-D", "nl80211", "-i", iface, "-c", conf, NULL }, 0) != 0) {
        fprintf(stderr, "wifi: wpa_supplicant did not start on %s\n", iface);
        return 1;
    }
    printf("%s: joining \"%s\"", iface, ssid);
    fflush(stdout);
    for (t = 0; t < 30; t++) {                   /* up to 30 s for the association */
        s = status_text();
        if (strcmp(field(s, "wpa_state"), "COMPLETED") == 0)
            break;
        sleep(1);
        putchar('.');
        fflush(stdout);
    }
    if (strcmp(field(s, "wpa_state"), "COMPLETED") != 0) {
        printf(" failed (%s): wrong password, or out of range?\n", s[0] ? field(s, "wpa_state") : "no wpa_supplicant");
        return 1;
    }
    printf(" connected (%s, %s MHz)\n", field(s, "key_mgmt"), field(s, "freq"));
    if (!dhcp)
        return 0;
    /* an address: udhcpc's default.script configures it, the route and the DNS servers */
    if (run((char *[]){ "udhcpc", "-i", (char *)iface, "-n", "-q", "-t", "5", NULL }, 0) != 0) {
        printf("%s: no address from DHCP (connected without one)\n", iface);
        return 0;
    }
    return cmd_status();
}

static int usage(int st)
{
    fprintf(st ? stderr : stdout,
            "usage: wifi [-i IFACE] [status | scan | connect NAME [PASSWORD] | off]\n"
            "  status   the interface and the connection (the default)\n"
            "  scan     the networks in range\n"
            "  connect  join NAME (WPA2/WPA3 with PASSWORD, open without), then DHCP\n"
            "           (--no-dhcp before NAME: only associate)\n"
            "  off      leave the network\n");
    return st;
}

int main(int argc, char **argv)
{
    int a = 1, dhcp = 1;
    const char *cmd;
    if (a + 1 < argc && strcmp(argv[a], "-i") == 0) {
        snprintf(iface, sizeof iface, "%s", argv[a + 1]);
        a += 2;
    } else if (!first_wlan(iface, sizeof iface)) {
        fprintf(stderr, "wifi: no wireless interface (no driver for the card, or no firmware: see dmesg)\n");
        return 1;
    }
    cmd = a < argc ? argv[a++] : "status";
    if (strcmp(cmd, "status") == 0)
        return cmd_status();
    if (strcmp(cmd, "scan") == 0)
        return cmd_scan();
    if (strcmp(cmd, "off") == 0)
        return cmd_off();
    if (strcmp(cmd, "connect") == 0) {
        if (a < argc && strcmp(argv[a], "--no-dhcp") == 0)
            dhcp = 0, a++;
        if (a >= argc || a + 2 < argc)
            return usage(2);
        return cmd_connect(argv[a], a + 1 < argc ? argv[a + 1] : NULL, dhcp);
    }
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0)
        return usage(0);
    return usage(2);
}
