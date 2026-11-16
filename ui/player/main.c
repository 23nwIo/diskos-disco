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
 *     MPD commands and reports a1 (position, once a second while playing) and the state frames;
 *   - builds the queue for 0100 from song.db like the stock player (LIST_SONG_0, LIST_SONG_3 in shuffle; queuedb.c),
 *     moves through it by the play mode and announces every track start with the full a2 frame (playback.c).
 *
 *   diskos-player [-n] [-l <log>] [-q <prefix>]
 *     -n           do not touch the hardware watchdog, power off only as a logged dry run (host runs)
 *     -l <log>     log file instead of /tmp/diskos-player.log ("-" = stderr)
 *     -q <prefix>  queue names <prefix>player and <prefix>ui instead of /player and /ui (host runs, tests)
 *     -k <path>    key events from <path> instead of /dev/input/event0 (tests feed a FIFO)
 *     -o <path>    the outputs state file instead of OPL_OUTPUTS (host runs) */
#include "core.h"
#include "proto.h"
#include "hw.h"
#include "sdcard.h"
#include "usbstorage.h"
#include "dac.h"
#include "mixer.h"
#include "btout.h"
#include "engine.h"
#include "mpdcli.h"
#include "playback.h"
#include "queuedb.h"
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

/* ---- playback glue (playback.c) ---- */
static int core_transport(void *ctx, int op, long arg){ (void)ctx; return pb_transport(op, arg); }
static int core_play(void *ctx, int type, int pos, const char *name){ (void)ctx; return pb_play_list(type, pos, name); }
/* 0d10 (the core checked the work mode): the radio mode may still be on its way from the main loop, so it is taken here */
static int core_radio(void *ctx, const char *name, const char *url){
    (void)ctx;
    pb_stream_mode(1);
    if(!url[0]) return pb_stream_stop();
    if(pb_stream_start(name, url) != 0){ logmsg("radio: \"%s\" %s does not play", name, url); return -1; }
    return 0;
}

/* ---- volume keys: click, double click and long press each change the volume or switch track, as stored by the stock
 * player in SYSCONFIG (set by 0820-0822, ui/controls.h): 1 = volume, anything else = track (up = next, down =
 * previous). While a key is held the driver repeats the long-press code every 40 ms (value 2, after 250 ms; device
 * log 2026-11-15), so a long press that changes the volume steps on the repeats, at most one step per RAMP_STEP_S. ---- */
enum { KA_CLICK = 0, KA_DOUBLE = 1, KA_LONG = 2 };
/* there while a resumed song has not played 10 s yet (playback.h pb_resume_begin); NAND, so it survives a crash */
#define RESUME_FLAG OPL_DIR "/resume-pending"
static const char *const KEY_COL[3] = { "KEY_SINGLE_CLICK_SLE", "KEY_DOUBLE_CLICK_SLE", "KEY_LONG_PRESS_SLE" };
#define RAMP_STEP_S 0.12           /* at most one volume step this often while held (the stock rate is not measured) */
static double g_ramp_last;
static void vol_key(pl_core_t *c, int which, int dir, double t){
    if(c->key_action[which] != 1){
        logmsg("volume key: %s track", dir > 0 ? "next" : "previous");
        pb_transport(dir > 0 ? PL_T_NEXT : PL_T_PREV, 0);
        return;
    }
    pl_core_volume_step(c, dir);
    if(which == KA_LONG) g_ramp_last = t;
}

/* ---- Outputs (core.h): the two volumes are kept in OPL_VOLUMES (NAND, written a few seconds after the last change),
 * the state goes to OPL_OUTPUTS (tmpfs) for the UI's Settings > Audio > Outputs whenever it changes. ---- */
#define VOL_SAVE_DELAY_S 3.0
static void load_volumes(pl_core_t *c){
    FILE *f = fopen(OPL_VOLUMES, "re");
    if(!f){ if(errno != ENOENT) logmsg("outputs: open %s: %s", OPL_VOLUMES, strerror(errno)); return; }
    int va = c->vol_aux, vb = c->vol_bt;
    char l[64];
    while(fgets(l, sizeof l, f)){
        if(!strncmp(l, "vol_aux=", 8)) va = atoi(l + 8);
        else if(!strncmp(l, "vol_bt=", 7)) vb = atoi(l + 7);
    }
    fclose(f);
    pl_core_load_volumes(c, va, vb);
    logmsg("outputs: volumes AUX %d, Bluetooth %d (%s)", c->vol_aux, c->vol_bt, OPL_VOLUMES);
}
static void save_volumes(const pl_core_t *c){
    char tmp[sizeof OPL_VOLUMES + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", OPL_VOLUMES);
    FILE *f = fopen(tmp, "we");
    if(!f || fprintf(f, "vol_aux=%d\nvol_bt=%d\n", c->vol_aux, c->vol_bt) < 0 || fflush(f) != 0 || fsync(fileno(f)) != 0){
        logmsg("outputs: write %s: %s", tmp, strerror(errno));
        if(f) fclose(f);
        return;
    }
    fclose(f);
    if(rename(tmp, OPL_VOLUMES) != 0) logmsg("outputs: rename %s: %s", tmp, strerror(errno));
}
static void write_outputs(const pl_core_t *c, const char *path){
    static char last[512];
    char b[512];
    snprintf(b, sizeof b, "aux=%d\nbt=%d\nvol_aux=%d\nvol_bt=%d\nbt_ready=%d\njack=%d\nbal=%d\ncodec=%s\ncodecs=%s\nswitching=%d\n"
             "auto=%d\nnotice=%u %d\n",
             pb_aux(), pb_bluetooth(), c->vol_aux, c->vol_bt, c->bt_ready, c->jack, c->bal, pb_bt_codec(), pb_bt_codecs(),
             pb_switching(), c->auto_out, c->notice_seq, c->notice);
    if(!strcmp(b, last)) return;
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if(!f || fputs(b, f) < 0 || fclose(f) != 0){ logmsg("outputs: write %s: %s", tmp, strerror(errno)); return; }
    if(rename(tmp, path) != 0){ logmsg("outputs: rename %s: %s", tmp, strerror(errno)); return; }
    snprintf(last, sizeof last, "%s", b);
}

/* "Volume: Main 42, AUX 23, BT 5" on every change, for `tail -f` (Jo 2026-11-16); RAM only (tmpfs), so no flash
 * writes. Main is the volume the UI shows (the lead output's). The UI's log upkeep keeps it small (ui/main.c). */
static void volume_log(const pl_core_t *c){
    static int last[3] = { -1, -1, -1 };
    if(c->volume == last[0] && c->vol_aux == last[1] && c->vol_bt == last[2]) return;
    last[0] = c->volume; last[1] = c->vol_aux; last[2] = c->vol_bt;
    FILE *f = fopen(OPL_VOLUME_LOG, "ae");
    if(!f){ logmsg("volume log: open %s: %s", OPL_VOLUME_LOG, strerror(errno)); return; }
    time_t now = time(NULL); struct tm tm; char ts[32] = "";
    if(localtime_r(&now, &tm)) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S ", &tm);
    fprintf(f, "%sVolume: Main %d, AUX %d%s, BT %d%s\n", ts, c->volume, c->vol_aux, pb_aux() ? "" : " (off)",
            c->vol_bt, pb_bluetooth() ? "" : " (off)");
    fclose(f);
}

int main(int argc, char **argv){
    int use_wd = 1; const char *logpath = LOG_DEFAULT, *qprefix = "/", *keypath = KEY_DEV, *outpath = OPL_OUTPUTS;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "-n")) use_wd = 0;
        else if(!strcmp(argv[i], "-l") && i + 1 < argc) logpath = argv[++i];
        else if(!strcmp(argv[i], "-q") && i + 1 < argc) qprefix = argv[++i];
        else if(!strcmp(argv[i], "-k") && i + 1 < argc) keypath = argv[++i];
        else if(!strcmp(argv[i], "-o") && i + 1 < argc) outpath = argv[++i];
        else { fprintf(stderr, "usage: diskos-player [-n] [-l <log>] [-q <prefix>] [-k <keys>] [-o <outputs>]\n"); return 2; }
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
    core.play = core_play;
    core.radio = core_radio;
    pb_init(&core, QD_SONG_DB, QD_SYSCONFIG_DB, logmsg);
    /* the key actions are kept in SYSCONFIG like the stock player keeps them; the UI sends them only when changed.
     * Not stored: every press changes the volume. */
    int key_saved[3];
    for(int i = 0; i < 3; i++){
        int v = use_wd ? qd_sys_get(QD_SYSCONFIG_DB, KEY_COL[i]) : -1;
        core.key_action[i] = v < 0 ? 1 : v;
        key_saved[i] = core.key_action[i];
    }
    logmsg("keys: click %d, double click %d, long press %d (1 volume, else track)",
           core.key_action[0], core.key_action[1], core.key_action[2]);
    /* resume mode (0684), kept in SYSCONFIG.MEMORY_PLAY: the UI sends it a moment after the start, the resume needs it
     * before */
    int rv = use_wd ? qd_sys_get(QD_SYSCONFIG_DB, "MEMORY_PLAY") : -1;
    core.resume_mode = rv >= 0 && rv <= 2 ? rv : 0;
    int resume_saved = core.resume_mode;

    if(use_wd) load_volumes(&core);                            /* host runs (-n) keep the defaults */
    if(use_wd && dac_open(0, logmsg) != 0) logmsg("dac: not available - no sound");
    if(use_wd) mix_route_dac(logmsg);                          /* the stock player sets its route at start too */
    if(eng_start(0, logmsg) == 0) pb_engine_event();           /* MPD is not hardware: host runs (-n) use it too when present */
    if(use_wd) pb_resume_begin(sd_is_mounted, RESUME_FLAG);    /* host runs (-n) have no library */
    int att_vol = -1, att_gain = -1, att_bal = -1, att_curve = -1, bt_vol = -1;
    unsigned route_seq = core.route_seq, codec_seq = core.codec_seq;
    int saved_aux = core.vol_aux, saved_bt = core.vol_bt;
    double vol_changed = 0;

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
    int bal = use_wd ? hw_bal_open() : -1;             /* the 4.4 mm plug (hw.h) */
    if(use_wd && bal < 0) logmsg("jack: open %s: %s - no 4.4 mm detection", BAL_DEV, strerror(errno));
    int bal_state = -1, bal_count = 0, bal_logged = 0;

    unsigned mode_seq = core.mode_seq;
    double t0 = now_s(), last_feed = t0, sleep_at = 0, power_at = 0, last_jack = 0;
    int stable = 0; unsigned sleep_seq = 0, usb_seq = 0;
    while(!g_stop){
        /* an mqd_t is a descriptor on Linux; a negative fd is skipped by poll */
        struct pollfd pf[4] = { { (int)pl, POLLIN, 0 }, { keys, POLLIN, 0 }, { uev, POLLIN, 0 }, { eng_idle_fd(), POLLIN, 0 } };
        int pr = poll(pf, 4, 250);
        if(pr > 0 && pf[3].fd >= 0 && (pf[3].revents & (POLLIN | POLLHUP | POLLERR))){
            int ev = eng_idle_event();
            if(ev > 0) pb_engine_event();
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
                if(ev[k].type != EV_KEY) continue;
                double kt = now_s();
                if(ev[k].value == 2){                                     /* autorepeat of a held key: every 40 ms */
                    if((ev[k].code == KEYC_VOL_UP_LONG || ev[k].code == KEYC_VOL_DOWN_LONG) && core.key_action[KA_LONG] == 1
                       && kt - g_ramp_last >= RAMP_STEP_S){
                        g_ramp_last = kt;
                        pl_core_volume_step(&core, ev[k].code == KEYC_VOL_UP_LONG ? +1 : -1);
                    }
                    continue;
                }
                if(ev[k].value != 1) continue;
                logmsg("key %u", (unsigned)ev[k].code);
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
                case KEYC_VOL_UP:          vol_key(&core, KA_CLICK, +1, kt); break;
                case KEYC_VOL_DOWN:        vol_key(&core, KA_CLICK, -1, kt); break;
                case KEYC_VOL_UP_DOUBLE:   vol_key(&core, KA_DOUBLE, +1, kt); break;
                case KEYC_VOL_DOWN_DOUBLE: vol_key(&core, KA_DOUBLE, -1, kt); break;
                case KEYC_VOL_UP_LONG:     vol_key(&core, KA_LONG, +1, kt); break;
                case KEYC_VOL_DOWN_LONG:   vol_key(&core, KA_LONG, -1, kt); break;
                case KEYC_PLAY_DOUBLE:     break;  /* ignored like in the stock player (docs/MQ_PLAYER_TRACE.md) */
                case KEYC_PLAY_LONG:       break;  /* the same: a held Play does nothing on the stock player */
                case KEYC_PLAY:     logmsg("play key"); pb_transport(PL_T_TOGGLE, 0); break;
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
        if((jack >= 0 || bal >= 0) && (t - last_jack) * 1000.0 >= JACK_POLL_MS){
            last_jack = t;
            int lv = jack >= 0 ? hw_jack_read(jack) : 0;
            if(lv < 0){ logmsg("jack: read: %s - no jack detection from now on", strerror(errno)); close(jack); jack = -1; }
            else if(jack >= 0) pl_core_jack(&core, lv);
            if(bal >= 0){
                int bv = hw_bal_read(bal);
                if(bv < 0){ logmsg("jack: read %s: %s - no 4.4 mm detection from now on", BAL_DEV, strerror(errno)); close(bal); bal = -1; }
                else {
                    if(bal_logged < 3){ bal_logged++; logmsg("jack: %s reads %d", BAL_DEV, bv); }   /* first values, for the device check */
                    if(hw_debounce(&bal_state, &bal_count, hw_bal_level(bv))){
                        logmsg("jack: %s = %d -> 4.4 mm plug %s", BAL_DEV, bv, bal_state ? "in" : "out");
                        pl_core_bal(&core, bal_state);
                    }
                }
            }
        }
        if(core.mode_seq != mode_seq){                   /* 0657: the Radio app's mode (0C), or back to local playback */
            mode_seq = core.mode_seq;
            pb_stream_mode(core.work_mode == PL_WORK_DLNA);
        }
        eng_check();
        if(core.route_seq != route_seq){ route_seq = core.route_seq; pb_route(); bt_vol = core.vol_bt; }
        if(core.codec_seq != codec_seq){ codec_seq = core.codec_seq; pb_codec(core.codec_want); }
        if(pb_bluetooth() && !pb_switching() && core.vol_bt != bt_vol){   /* the headphones' own volume (BlueALSA) */
            bt_vol = core.vol_bt;
            bto_volume(pb_bt_mac(), core.vol_bt, PL_VOL_MAX, logmsg);
        }
        int gain = core.gain && pl_core_plugged(&core), curve = core.bal == 1;
        if(pb_aux() && dac_current_rate() && (core.vol_aux != att_vol || gain != att_gain || core.balance != att_bal || curve != att_curve)){
            unsigned char a, l;
            pb_attenuation(&a, &l);
            dac_set_attenuation(a, l, logmsg);
            att_vol = core.vol_aux; att_gain = gain; att_bal = core.balance; att_curve = curve;
        }
        if(core.vol_aux != saved_aux || core.vol_bt != saved_bt){
            if(!vol_changed) vol_changed = t;
            if(t - vol_changed >= VOL_SAVE_DELAY_S){
                saved_aux = core.vol_aux; saved_bt = core.vol_bt; vol_changed = 0;
                if(use_wd) save_volumes(&core);
            }
        }
        pb_tick(t);
        write_outputs(&core, outpath);
        if(use_wd) volume_log(&core);
        for(int i = 0; i < 3; i++) if(core.key_action[i] != key_saved[i]){   /* 0820-0822: kept like the stock player */
            key_saved[i] = core.key_action[i];
            if(use_wd && qd_sys_set(QD_SYSCONFIG_DB, KEY_COL[i], core.key_action[i]) != 0)
                logmsg("keys: can't store %s in %s", KEY_COL[i], QD_SYSCONFIG_DB);
        }
        if(core.resume_mode != resume_saved){
            resume_saved = core.resume_mode;
            if(use_wd && qd_sys_set(QD_SYSCONFIG_DB, "MEMORY_PLAY", core.resume_mode) != 0)
                logmsg("resume: can't store MEMORY_PLAY in %s", QD_SYSCONFIG_DB);
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
            pb_shutdown();
            if(use_wd && (core.vol_aux != saved_aux || core.vol_bt != saved_bt)) save_volumes(&core);
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
    pb_shutdown();
    if(use_wd && (core.vol_aux != saved_aux || core.vol_bt != saved_bt)) save_volumes(&core);
    eng_stop();
    pb_close();
    dac_close();
    if(wd >= 0){ wd_stop(wd); wd_close(wd); }
    if(keys >= 0) close(keys);
    if(jack >= 0) close(jack);
    if(bal >= 0) close(bal);
    if(uev >= 0) close(uev);
    mq_close(pl);
    if(g_ui != (mqd_t)-1) mq_close(g_ui);
    free(buf);
    return 0;
}
