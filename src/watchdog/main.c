/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 diskOS contributors */
/* diskos-watchdog: drop-in replacement for the stock /usr/bin/cmd_watchdog, plus a feeding mode for headless use.
 *
 *   diskos-watchdog start <ms>              arm the watchdog
 *   diskos-watchdog stop                    disarm it
 *   diskos-watchdog feed                    restart the countdown
 *   diskos-watchdog reset                   stock "reset" request (expected to reboot the Disc)
 *   diskos-watchdog run <ms> [<every_ms>]   arm, then feed every <every_ms> (default <ms>/4) until SIGTERM/SIGINT,
 *                                           then disarm and exit. SIGKILL or a hang leaves it armed: that is the point.
 *   diskos-watchdog -h | --help
 *
 * Exit status: 0 success, 1 device error, 2 usage error. The stock tool exits with 255 for both kinds of failure;
 * its only caller, the stock mq_player, runs it through popen()/pclose() and ignores the status. */
#include "watchdog.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *g_prog = "diskos-watchdog";
static volatile sig_atomic_t g_quit;

static int usage(int status)
{
    FILE *o = status ? stderr : stdout;
    fprintf(o,
            "Usage: %s start <ms>\n"
            "       %s stop\n"
            "       %s feed\n"
            "       %s reset\n"
            "       %s run <ms> [<every_ms>]\n"
            "       %s -h | --help\n",
            g_prog, g_prog, g_prog, g_prog, g_prog, g_prog);
    return status;
}

/* Strict decimal milliseconds, 1..WD_MAX_TIMEOUT_MS. */
static int parse_ms(const char *s, int *out)
{
    char *end;
    unsigned long v;
    if (!s || *s < '0' || *s > '9')
        return -1;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || *end || v == 0 || v > WD_MAX_TIMEOUT_MS)
        return -1;
    *out = (int)v;
    return 0;
}

static int fail(const char *what, int err)
{
    fprintf(stderr, "%s: %s failed: %s\n", g_prog, what, strerror(-err));
    return 1;
}

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);   /* EINTR from SIGTERM ends the wait early, which is what we want */
}

static int run(int fd, int timeout_ms, int every_ms)
{
    struct sigaction sa;
    int r;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if ((r = wd_start(fd, timeout_ms)) < 0)
        return fail("start", r);
    while (!g_quit) {
        if ((r = wd_feed(fd)) < 0)
            fprintf(stderr, "%s: feed failed: %s\n", g_prog, strerror(-r));   /* keep trying until the deadline */
        sleep_ms(every_ms);
    }
    if ((r = wd_stop(fd)) < 0)
        return fail("stop", r);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd;
    int fd, r, ms = 0, every = 0;

    if (argc > 0 && argv[0] && argv[0][0])
        g_prog = argv[0];
    if (argc < 2)
        return usage(2);
    cmd = argv[1];

    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help"))
        return argc == 2 ? usage(0) : usage(2);

    if (!strcmp(cmd, "start")) {
        if (argc != 3 || parse_ms(argv[2], &ms))
            return usage(2);
    } else if (!strcmp(cmd, "run")) {
        if (argc < 3 || argc > 4 || parse_ms(argv[2], &ms))
            return usage(2);
        if (argc == 4) {
            if (parse_ms(argv[3], &every) || every >= ms)
                return usage(2);
        } else {
            every = ms / 4 > 0 ? ms / 4 : 1;
        }
    } else if (!strcmp(cmd, "stop") || !strcmp(cmd, "feed") || !strcmp(cmd, "reset")) {
        if (argc != 2)
            return usage(2);
    } else {
        return usage(2);
    }

    fd = wd_open();
    if (fd < 0)
        return fail("open " WD_DEV_PATH, fd);

    if (!strcmp(cmd, "run")) {
        r = run(fd, ms, every);
    } else {
        if (!strcmp(cmd, "start"))
            r = wd_start(fd, ms);
        else if (!strcmp(cmd, "stop"))
            r = wd_stop(fd);
        else if (!strcmp(cmd, "feed"))
            r = wd_feed(fd);
        else
            r = wd_reset(fd);
        r = r < 0 ? fail(cmd, r) : 0;
    }

    wd_close(fd);
    return r;
}
