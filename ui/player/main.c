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
 *   - mounts the SD card when it is inserted and unmounts it when it is pulled (sdcard.c, kernel uevents; the
 *     cold-boot mount is the UI's);
 *   - reads the headphone jack like the stock player (hw.c) and pauses when it is unplugged (core.c);
 *   - USB storage mode (0642 1): hands the card to a USB host as a mass storage gadget, and back on 0642 0
 *     (usbstorage.c);
 *   - runs the stock power off routine when the sleep timer (0806) runs out (hw.c);
 *   - logs every command to /tmp/diskos-player.log (RAM).
 *   - plays through MPD as its audio core (engine.c, docs/MPD_CORE.md) when /usr/data/diskos-own/mpd exists: owns
 *     the DAC (dac.c: start sequence per stream rate, volume/balance/gain as attenuation bytes), turns 0201/0103 into
 *     MPD commands and reports a1 (position, once a second while playing) and the state frames. Building the queue
 *     from song.db for 0100 comes next.
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
#include "usbstorage.h"
#include "dac.h"
#include "engine.h"
#include "mpdcli.h"
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

/* ---- audio engine glue ---- */
static eng_status_t g_es;          /* last status read after an MPD event */
static double g_es_t;              /* when it was read (CLOCK_MONOTONIC) */
static int g_es_valid;

static int core_transport(void *ctx, int op, long arg){
    (void)ctx;
    if(!eng_ready()){ logmsg("transport %d: no engine", op); return -1; }
    switch(op){
    case PL_T_TOGGLE: return eng_toggle();
    case PL_T_NEXT:   return eng_next();
    case PL_T_PREV:   return eng_prev();
    case PL_T_SEEK:   return eng_seek_ms(arg);
    }
    return -1;
}

static void send_state(pl_core_t *c, int state){
    /* the stock player sends the partial state frame twice (capture 2026-11-15) */
    char f[32]; int n = pp_fmt_a2(f, sizeof f, state == ENG_PLAY ? "{\"state\":0}" : "{\"state\":1}");
    core_send(c, f, n); core_send(c, f, n);
}

/* After an MPD event: read the status, start the DAC for a new stream rate, tell the UI about play/pause. */
static void engine_update(pl_core_t *c){
    eng_status_t s;
    if(eng_status(&s) != 0) return;
    int was = g_es_valid ? g_es.state : ENG_STOP;
    if(s.state == ENG_PLAY && s.rate && s.rate != dac_current_rate()){
        unsigned char a, l; dac_attenuation(c->volume, c->gain, c->balance, &a, &l);
        dac_start(s.rate, a, l, logmsg);
    }
    if(s.state != was || (g_es_valid && strcmp(s.file, g_es.file)))
        logmsg("engine: %s, song %d/%d, %ld/%ld ms, %u Hz, %s", s.state == ENG_PLAY ? "play" : s.state == ENG_PAUSE ? "pause" : "stop",
               s.song + 1, s.songs, s.elapsed_ms, s.duration_ms, s.rate, s.file);
    if(s.state != was && (s.state == ENG_PLAY || was == ENG_PLAY)) send_state(c, s.state);
    g_es = s; g_es_t = now_s(); g_es_valid = 1;
}


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
    core.transport = core_transport;

    if(use_wd && dac_open(0, logmsg) != 0) logmsg("dac: not available - no sound");
    if(eng_start(0, logmsg) == 0) engine_update(&core);         /* MPD is not hardware: host runs (-n) use it too when present */
    int att_vol = -1, att_gain = -1, att_bal = -1;
    double last_a1 = 0;

    /* The keys. The stock player grabs them, so the UI never sees them (ui/main.c reads the Power level from the GPIO
     * instead); we do the same. Without the device the player still runs, only without keys. */
    int keys = open(keypath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(keys < 0) logmsg("keys: open %s: %s - no keys", keypath, strerror(errno));
    else if(ioctl(keys, EVIOCGRAB, 1) != 0) logmsg("keys: grab %s: %s (keys still read)", keypath, strerror(errno));
    else logmsg("keys: %s grabbed", keypath);

    int uev = use_wd ? sd_uevent_open() : -1;           /* host runs (-n) never mount anything */
    if(use_wd && uev < 0) logmsg("sd: uevent socket: %s - no card hot-plug", strerror(errno));
    else if(use_wd) logmsg("sd: listening for card events (fd %d)", uev);

    int jack = use_wd ? hw_jack_open() : -1;           /* host runs have no /dev/gpio */
    if(use_wd && jack < 0) logmsg("jack: open %s: %s - no jack detection", JACK_DEV, strerror(errno));

    double t0 = now_s(), last_feed = t0, sleep_at = 0, power_at = 0, last_jack = 0;
    int stable = 0; unsigned sleep_seq = 0, usb_seq = 0;
    while(!g_stop){
        /* an mqd_t is a descriptor on Linux; a negative fd is skipped by poll */
        struct pollfd pf[4] = { { (int)pl, POLLIN, 0 }, { keys, POLLIN, 0 }, { uev, POLLIN, 0 }, { eng_idle_fd(), POLLIN, 0 } };
        int pr = poll(pf, 4, 250);
        if(pr > 0 && pf[3].fd >= 0 && (pf[3].revents & (POLLIN | POLLHUP | POLLERR))){
            int ev = eng_idle_event();
            if(ev > 0) engine_update(&core);
        }
        if(pr > 0 && uev >= 0 && (pf[2].revents & (POLLIN | POLLERR))){
            char ub[2048];
            ssize_t un;
            while((un = recv(uev, ub, sizeof ub - 1, 0)) > 0){
                ub[un] = 0;
                if(strstr(ub, "mmc")) logmsg("sd: uevent %s (%zd bytes)", ub, un);   /* the header; diagnosis */
                int k = sd_uevent_kind(ub, (size_t)un);
                if(k != SDEV_NONE && us_active()) logmsg("sd: card %s while exported over USB, left alone",
                                                         k == SDEV_ADD ? "inserted" : "pulled");
                else if(k == SDEV_ADD){ logmsg("sd: card inserted"); sd_mount(logmsg); }
                else if(k == SDEV_REMOVE){ logmsg("sd: card pulled"); sd_unmount(logmsg); }
            }
            if(un < 0 && errno != EAGAIN && errno != EINTR) logmsg("sd: uevent recv: %s", strerror(errno));
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
                case KEYC_POWER: case KEYC_POWER_ALT:
                    logmsg("power key (%u)", (unsigned)ev[k].code);
                    hw_screen_toggle(!use_wd, logmsg);
                    break;
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
        if(jack >= 0 && (t - last_jack) * 1000.0 >= JACK_POLL_MS){
            last_jack = t;
            int lv = hw_jack_read(jack);
            if(lv < 0){ logmsg("jack: read: %s - no jack detection from now on", strerror(errno)); close(jack); jack = -1; }
            else pl_core_jack(&core, lv);
        }
        eng_check();
        if(dac_current_rate() && (core.volume != att_vol || core.gain != att_gain || core.balance != att_bal)){
            unsigned char a, l;
            dac_attenuation(core.volume, core.gain, core.balance, &a, &l);
            dac_set_attenuation(a, l, logmsg);
            att_vol = core.volume; att_gain = core.gain; att_bal = core.balance;
        }
        if(g_es_valid && g_es.state == ENG_PLAY && t - last_a1 >= 1.0){   /* a1 once a second while playing */
            last_a1 = t;
            long ms = g_es.elapsed_ms + (long)((t - g_es_t) * 1000.0);
            if(g_es.duration_ms > 0 && ms > g_es.duration_ms) ms = g_es.duration_ms;
            char f[32]; int n = pp_fmt_a1(f, sizeof f, (unsigned long)ms);
            core_send(NULL, f, n);
        }
        if(core.usb_seq != usb_seq){                     /* 0642: the UI's "USB storage" source */
            usb_seq = core.usb_seq;
            if(core.usb_gadget == 1) us_start(!use_wd, logmsg);
            else us_stop(!use_wd, logmsg);
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
    eng_stop();
    dac_close();
    if(wd >= 0){ wd_stop(wd); wd_close(wd); }
    if(keys >= 0) close(keys);
    if(jack >= 0) close(jack);
    if(uev >= 0) close(uev);
    mq_close(pl);
    if(g_ui != (mqd_t)-1) mq_close(g_ui);
    free(buf);
    return 0;
}
