/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* diskos-player: the open-source replacement for /usr/bin/mq_player (docs/MQ_PLAYER_REPLACEMENT.md), skeleton stage.
 *
 * Started by the launcher instead of the stock player when /usr/data/diskos-own-player exists (ui/own_player.h), as
 * "mq_player" so fiio_init's watchdog loop finds it. What it does so far:
 *   - arms the hardware watchdog like the stock player (start 10000 ms, feed every second), via ui/watchdog;
 *   - creates the /player queue, opens (or creates) /ui, and answers the UI as docs/PLAYER_PROTOCOL.md says (core.c);
 *   - deletes /usr/data/diskos-own-player.unstable after OPL_STABLE_S seconds, which tells the launcher that this
 *     player did not crash early;
 *   - owns the keys (/dev/input/event0, grabbed like the stock player): Power toggles the screen, a held Power powers
 *     off, Volume up/down step the volume and tell the UI (a714);
 *   - mounts the SD card when it is inserted and unmounts it when it is pulled (sdcard.c; the cold-boot mount is the
 *     UI's);
 *   - runs the stock power off routine when the sleep timer (0806) runs out (hw.c);
 *   - logs every command to /tmp/diskos-player.log (RAM).
 * No sound yet.
 *
 *   diskos-player [-n] [-l <log>] [-q <prefix>]
 *     -n           do not touch the hardware watchdog, power off only as a logged dry run (host runs)
 *     -l <log>     log file instead of /tmp/diskos-player.log ("-" = stderr)
 *     -q <prefix>  queue names <prefix>player and <prefix>ui instead of /player and /ui (host runs, tests)
 *     -k <path>    key events from <path> instead of /dev/input/event0 (tests feed a FIFO) */
#include "core.h"
#include "proto.h"
#include "hw.h"
#include "sdcard.h"
#include "../own_player.h"
#include "../watchdog/watchdog.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <mqueue.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WD_TIMEOUT_MS 10000        /* as the stock player: cmd_watchdog start 10000 */
#define WD_FEED_MS    1000         /* as the stock player: cmd_watchdog feed once a second */
#define QUEUE_MAXMSG  10
#define LOG_DEFAULT   "/tmp/diskos-player.log"

static volatile sig_atomic_t g_stop;
static FILE *g_log;
static mqd_t g_ui = (mqd_t)-1;
static char g_ui_name[64] = "/ui";

static double now_s(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void logmsg(const char *fmt, ...){
    if(!g_log) return;
    fprintf(g_log, "%.3f ", now_s());
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}
static void core_log(void *ctx, const char *msg){ (void)ctx; logmsg("%s", msg); }

static int ui_open(void){
    if(g_ui != (mqd_t)-1) return 0;
    struct mq_attr a; memset(&a, 0, sizeof a);
    a.mq_maxmsg = QUEUE_MAXMSG; a.mq_msgsize = PP_MAX_FRAME;
    g_ui = mq_open(g_ui_name, O_WRONLY | O_NONBLOCK | O_CREAT | O_CLOEXEC, 0666, &a);
    if(g_ui == (mqd_t)-1){ logmsg("mq_open %s: %s", g_ui_name, strerror(errno)); return -1; }
    return 0;
}
static void core_send(void *ctx, const char *frame, int len){
    (void)ctx;
    if(ui_open() != 0) return;
    if(mq_send(g_ui, frame, (size_t)len, 0) != 0){
        /* EAGAIN: the UI is not reading (not started yet, or busy); the frame is dropped, as a later one replaces it */
        logmsg("send %.4s to %s failed: %s", frame, g_ui_name, strerror(errno));
        if(errno == EBADF){ mq_close(g_ui); g_ui = (mqd_t)-1; }
        return;
    }
    logmsg("> %.*s", len > 120 ? 120 : len, frame);
}

static void on_signal(int s){ (void)s; g_stop = 1; }

int main(int argc, char **argv){
    int use_wd = 1; const char *logpath = LOG_DEFAULT, *qprefix = "/", *keypath = KEY_DEV;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "-n")) use_wd = 0;
        else if(!strcmp(argv[i], "-l") && i + 1 < argc) logpath = argv[++i];
        else if(!strcmp(argv[i], "-q") && i + 1 < argc) qprefix = argv[++i];
        else if(!strcmp(argv[i], "-k") && i + 1 < argc) keypath = argv[++i];
        else { fprintf(stderr, "usage: diskos-player [-n] [-l <log>] [-q <prefix>] [-k <keys>]\n"); return 2; }
    }
    g_log = !strcmp(logpath, "-") ? stderr : fopen(logpath, "ae");
    if(!g_log) g_log = stderr;
    char pl_name[64];
    snprintf(pl_name, sizeof pl_name, "%splayer", qprefix);
    snprintf(g_ui_name, sizeof g_ui_name, "%sui", qprefix);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    logmsg("=== diskos-player start, pid %d, watchdog %s", (int)getpid(), use_wd ? "on" : "off");

    /* Watchdog first: from here on a hang reboots the Disc, exactly as with the stock player. A running watchdog
     * (an earlier player armed it and was killed) refuses start with EPERM; it is then simply fed. */
    int wd = -1;
    if(use_wd){
        wd = wd_open();
        if(wd < 0) logmsg("watchdog: open failed: %s", strerror(-wd));
        else {
            int r = wd_start(wd, WD_TIMEOUT_MS);
            if(r == -EPERM){ logmsg("watchdog: already running, feeding it"); wd_feed(wd); }
            else if(r < 0) logmsg("watchdog: start failed: %s", strerror(-r));
            else logmsg("watchdog: armed, %d ms", WD_TIMEOUT_MS);
        }
    }

    /* A fresh /player: frames left over from an earlier player are not ours to replay. The UI notices the new queue
     * by its inode and reattaches (ipc_health_check). */
    mq_unlink(pl_name);
    struct mq_attr a; memset(&a, 0, sizeof a);
    a.mq_maxmsg = QUEUE_MAXMSG; a.mq_msgsize = PP_MAX_FRAME;
    mqd_t pl = mq_open(pl_name, O_RDONLY | O_CREAT | O_CLOEXEC, 0666, &a);
    if(pl == (mqd_t)-1){
        logmsg("mq_open %s: %s - exiting (the launcher falls back to the stock player after a few tries)",
               pl_name, strerror(errno));
        return 1;    /* the watchdog stays armed on purpose: without a working queue the Disc has no player */
    }
    struct mq_attr got;
    long msgsize = (mq_getattr(pl, &got) == 0 && got.mq_msgsize > 0) ? got.mq_msgsize : PP_MAX_FRAME;
    char *buf = malloc((size_t)msgsize + 1);
    if(!buf){ logmsg("out of memory"); return 1; }
    ui_open();

    pl_core_t core;
    pl_core_init(&core, core_send, core_log, NULL);

    /* The keys. The stock player grabs them, so the UI never sees them (ui/main.c reads the Power level from the GPIO
     * instead); we do the same. Without the device the player still runs, only without keys. */
    int keys = open(keypath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(keys < 0) logmsg("keys: open %s: %s - no keys", keypath, strerror(errno));
    else if(ioctl(keys, EVIOCGRAB, 1) != 0) logmsg("keys: grab %s: %s (keys still read)", keypath, strerror(errno));
    else logmsg("keys: %s grabbed", keypath);

    int uev = use_wd ? sd_uevent_open() : -1;           /* host runs (-n) never mount anything */
    if(use_wd && uev < 0) logmsg("sd: uevent socket: %s - no card hot-plug", strerror(errno));

    double t0 = now_s(), last_feed = t0, sleep_at = 0, power_at = 0;
    int stable = 0; unsigned sleep_seq = 0;
    while(!g_stop){
        /* an mqd_t is a descriptor on Linux; a negative fd is skipped by poll */
        struct pollfd pf[3] = { { (int)pl, POLLIN, 0 }, { keys, POLLIN, 0 }, { uev, POLLIN, 0 } };
        int pr = poll(pf, 3, 250);
        if(pr > 0 && uev >= 0 && (pf[2].revents & POLLIN)){
            char ub[2048];
            ssize_t un;
            while((un = recv(uev, ub, sizeof ub - 1, 0)) > 0){
                ub[un] = 0;
                int k = sd_uevent_kind(ub, (size_t)un);
                if(k == SDEV_ADD){ logmsg("sd: card inserted"); sd_mount(logmsg); }
                else if(k == SDEV_REMOVE){ logmsg("sd: card pulled"); sd_unmount(logmsg); }
            }
        }
        if(pr < 0 && errno != EINTR){ logmsg("poll: %s", strerror(errno)); usleep(100000); }
        if(pr > 0 && (pf[0].revents & POLLIN)){
            ssize_t n = mq_receive(pl, buf, (size_t)msgsize, NULL);
            if(n >= 0){
                buf[n] = 0;
                logmsg("< %.*s", n > 200 ? 200 : (int)n, buf);
                pl_core_handle(&core, buf, (int)n);
            } else if(errno != EAGAIN && errno != EINTR){
                logmsg("mq_receive: %s", strerror(errno));
                usleep(100000);
            }
        }
        if(pr > 0 && keys >= 0 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))){
            struct input_event ev[16];
            ssize_t r = read(keys, ev, sizeof ev);
            if(r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)){
                logmsg("keys: read: %s - no keys from now on", r == 0 ? "end of file" : strerror(errno));
                close(keys); keys = -1;
            }
            for(ssize_t k = 0; r > 0 && k < r / (ssize_t)sizeof ev[0]; k++){
                if(ev[k].type != EV_KEY || ev[k].value != 1) continue;     /* presses only; releases follow at once */
                switch(ev[k].code){
                case KEYC_POWER: hw_screen_toggle(!use_wd, logmsg); break;
                case KEYC_POWER_LONG:
                    /* Give the UI a moment: it sees the held key on the GPIO and flushes before the player's power off
                     * (ui/main.c, "power key held"). The stock player's delay is not measured; 2 s is a guess. */
                    logmsg("power key held: power off in 2 s");
                    power_at = now_s() + 2.0;
                    break;
                case KEYC_VOL_UP:   pl_core_volume_step(&core, +1); break;
                case KEYC_VOL_DOWN: pl_core_volume_step(&core, -1); break;
                case KEYC_PLAY:     logmsg("play key (playback not implemented yet)"); break;
                default:            logmsg("key %u pressed, not used", (unsigned)ev[k].code); break;
                }
            }
        }
        double t = now_s();
        if(wd >= 0 && (t - last_feed) * 1000.0 >= WD_FEED_MS){
            int r = wd_feed(wd);
            if(r < 0) logmsg("watchdog: feed failed: %s", strerror(-r));
            last_feed = t;
        }
        if(core.sleep_seq != sleep_seq){                 /* 0806: the UI's "Shut down" sends 1 s */
            sleep_seq = core.sleep_seq;
            sleep_at = core.sleep_s > 0 ? t + core.sleep_s : 0;
        }
        if(power_at > 0 && t >= power_at){ power_at = 0; sleep_at = t; }   /* the same routine as the sleep timer */
        if(sleep_at > 0 && t >= sleep_at){
            sleep_at = 0;
            logmsg("power off (sleep timer or held Power key)%s", use_wd ? "" : " (dry run, -n)");
            hw_power_off(wd, !use_wd, logmsg);
            if(use_wd) logmsg("power off did not happen");
        }
        if(!stable && t - t0 >= OPL_STABLE_S){
            stable = 1;
            if(unlink(OPL_UNSTABLE) == 0) logmsg("running %d s: removed %s", OPL_STABLE_S, OPL_UNSTABLE);
            else if(errno != ENOENT) logmsg("unlink %s: %s", OPL_UNSTABLE, strerror(errno));
        }
    }

    /* SIGTERM/SIGINT: an orderly stop, so disarm. SIGKILL (fiio_init's loop) leaves it armed, as with the stock
     * player; the next player start feeds it. */
    logmsg("=== diskos-player stop: %u frames, %u malformed, %u unknown", core.frames, core.bad_frames,
           core.unknown_tags);
    if(wd >= 0){ wd_stop(wd); wd_close(wd); }
    if(keys >= 0) close(keys);
    if(uev >= 0) close(uev);
    mq_close(pl);
    if(g_ui != (mqd_t)-1) mq_close(g_ui);
    free(buf);
    return 0;
}
