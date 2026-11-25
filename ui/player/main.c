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
 *   - holds the battery at 80 % while plugged in when the Charging Limit (0808) is on, like the stock player (hw.c);
 *   - logs every command to /tmp/diskos-player.log (RAM).
 *   - plays through MPD as its audio core (engine.c, docs/MPD_CORE.md) when /usr/data/diskos-own/mpd exists: owns
 *     the DAC (dac.c: start sequence per stream rate, volume/balance/gain as attenuation bytes), turns 0201/0103 into
 *     MPD commands and reports a1 (position, once a second while playing) and the state frames;
 *   - registers with BlueZ as the headphones' player, so their buttons and absolute volume reach it (btavrcp.c);
 *   - reads the list for 0100 from song.db like the stock player, but into RAM only (SONG.IDs; queuedb.c qd_list),
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
#include "btavrcp.h"
#include "engine.h"
#include "mpdcli.h"
#include "playback.h"
#include "usbaudio.h"
#include "queuedb.h"
#include "../own_player.h"
#include "../watchdog/watchdog.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
static int core_upnext(void *ctx, unsigned ack, int party, const int *ids, int n){ (void)ctx; return pb_up_set(ack, party, ids, n); }
static int core_playsong(void *ctx, int song_id){ (void)ctx; return pb_play_song(song_id); }
/* 0d10 (the core checked the work mode): the radio mode may still be on its way from the main loop, so it is taken here */
static int core_radio(void *ctx, const char *name, const char *url){
    (void)ctx;
    pb_stream_mode(1);
    if(!url[0]) return pb_stream_stop();
    if(pb_stream_start(name, url) != 0){ logmsg("radio: \"%s\" %s does not play", name, url); return -1; }
    return 0;
}

/* 0d14: a stream list (Bandcamp); the radio mode may still be on its way, as with 0d10 */
static int core_slist(void *ctx, int idx){ (void)ctx; pb_stream_mode(1); return pb_slist_play(idx); }

/* ---- Fade in (docs/USB_AUDIO.md, Jo 2026-11-25): an output whose device has fade on goes from 0 to its volume over
 * g_fade_s when it is switched on and when playback starts after a pause; volume changes are not faded. core vol_*
 * is the target, app_* what the hardware gets. Duration: "fade_s=N" in OPL_AUDIO_CONF (default PL_FADE_MS). ---- */
#define OPL_AUDIO_CONF OPL_DIR "/audio.conf"
#define FADE_BT_STEP_S 0.25        /* Bluetooth: bluealsa-cli per step, so not more often */
static double g_fade_s = PL_FADE_MS / 1000.0;
static double g_fade_t0[PL_O_N];          /* when the fade of each output began, 0 none */
static int fade_busy(int o){ return g_fade_t0[o] > 0; }
static void load_audio_conf(void){
    FILE *f = fopen(OPL_AUDIO_CONF, "re");
    if(!f){ if(errno != ENOENT) logmsg("audio: open %s: %s", OPL_AUDIO_CONF, strerror(errno)); }
    else {
        char l[80];
        while(fgets(l, sizeof l, f)) if(!strncmp(l, "fade_s=", 7)){
            double v = atof(l + 7);
            g_fade_s = v < 0 ? 0 : v > 30 ? 30 : v;
        }
        fclose(f);
    }
    logmsg("audio: fade in %.1f s (%s%s)", g_fade_s, OPL_AUDIO_CONF, f ? "" : ", not there: the default");
}
static void fade_start(const pl_core_t *c, int o, double t){
    if(!c->fade[o] || g_fade_s <= 0) return;
    g_fade_t0[o] = t;
}
/* app_* from vol_* and the fades running at t */
static void fade_apply(pl_core_t *c, double t){
    int *app[PL_O_N] = { &c->app_aux, &c->app_bt, &c->app_dig, &c->app_usb };
    const int vol[PL_O_N] = { c->vol_aux, c->vol_bt, c->vol_dig, c->vol_usb };
    for(int o = 0; o < PL_O_N; o++){
        double r = g_fade_t0[o] > 0 && c->fade[o] && g_fade_s > 0 ? (t - g_fade_t0[o]) / g_fade_s : 1;
        if(r >= 1 || r < 0){ g_fade_t0[o] = 0; *app[o] = vol[o]; }
        else *app[o] = (int)(vol[o] * r);
    }
}

/* ---- the USB sound card (usbaudio.c; docs/USB_AUDIO.md) ---- */
static ua_card_t g_ua;                     /* the card in use (the newest plugged) */
static int g_ua_have;
static ua_mixer_t g_ua_mix;                /* its volume control: its own, or ours by softvol (g_ua_soft) */
static int g_ua_soft;
static ua_card_t g_ua_seen[UA_MAX_CARDS];  /* the last scan, to tell which card is new */
static int g_ua_nseen;

/* ---- the volumes to the hardware: the DAC's attenuation, S/PDIF's software volume (MPD's "setvol", which no other
 * output uses), the USB card's volume control (its own or our softvol one), the headphones (BlueALSA). Only what
 * changed. ---- */
static int g_att_vol = -1, g_att_gain = -1, g_att_bal = -1, g_att_curve = -1, g_bt_last = -1, g_soft_vol = -1, g_usb_vol = -1;
static double g_bt_at;
static void apply_volumes(pl_core_t *c, double t){
    int gain = c->gain && pl_core_plugged(c), curve = c->bal == 1;
    if(pb_aux() && dac_current_rate() && (c->app_aux != g_att_vol || gain != g_att_gain || c->balance != g_att_bal || curve != g_att_curve)){
        unsigned char a, l;
        pb_attenuation(&a, &l);
        dac_set_attenuation(a, l, logmsg);
        g_att_vol = c->app_aux; g_att_gain = gain; g_att_bal = c->balance; g_att_curve = curve;
    }
    if(c->app_dig != g_soft_vol && eng_spdif_volume((unsigned)(c->app_dig * 100 / PL_VOL_MAX)) == 0) g_soft_vol = c->app_dig;
    if(g_ua_have && c->app_usb != g_usb_vol){
        ua_mixer_set(g_ua.card, &g_ua_mix, c->app_usb, PL_VOL_MAX, logmsg);   /* a failure is logged, not retried every turn */
        g_usb_vol = c->app_usb;
    }
    if(pb_bluetooth() && !pb_switching() && c->app_bt != g_bt_last && (!fade_busy(PL_O_BT) || c->app_bt == 0 || t - g_bt_at >= FADE_BT_STEP_S)){
        g_bt_last = c->app_bt; g_bt_at = t;        /* the headphones' own volume (BlueALSA) */
        bto_volume(pb_bt_mac(), c->app_bt, PL_VOL_MAX, logmsg);
    }
}
/* engine.c, right before MPD starts or resumes: a fade in on every output that plays, unless it plays already */
static pl_core_t *g_fcore;
static void before_play(void){
    const eng_status_t *es = pb_status(NULL);
    if(!g_fcore || (es && es->state == ENG_PLAY)) return;
    double t = now_s();
    const int on[PL_O_N] = { pb_aux(), pb_bluetooth(), pb_digital(), pb_usb() };
    int any = 0;
    for(int o = 0; o < PL_O_N; o++) if(on[o] && g_fcore->fade[o] && g_fade_s > 0){ fade_start(g_fcore, o, t); any = 1; }
    if(!any) return;
    fade_apply(g_fcore, t);
    apply_volumes(g_fcore, t);
}

/* Once a second: the cards there now; the newest one is used. A card that came, went or was replaced goes to core
 * (pl_core_usb: new ones silent and off), its control is set before anything can play to it, and MPD's "diskos_usb"
 * points at it. */
static void usb_tick(pl_core_t *c, double t){
    ua_card_t now[UA_MAX_CARDS];
    int n = ua_scan(now, UA_MAX_CARDS), pick = -1, still = 0;
    for(int i = 0; i < n; i++){
        int seen = 0;
        for(int k = 0; k < g_ua_nseen; k++) if(g_ua_seen[k].card == now[i].card && !strcmp(g_ua_seen[k].key, now[i].key)) seen = 1;
        if(!seen) pick = i;                        /* the last new one */
        if(g_ua_have && now[i].card == g_ua.card && !strcmp(now[i].key, g_ua.key)) still = 1;
    }
    memcpy(g_ua_seen, now, sizeof now[0] * (unsigned)n); g_ua_nseen = n;
    if(pick < 0 && still) return;                  /* nothing new, the card in use is there */
    if(pick < 0 && n > 0) pick = n - 1;            /* the card in use went, another one is still there */
    if(pick < 0 && !g_ua_have) return;
    if(g_ua_have){
        logmsg("usb audio: card %d %s %s", g_ua.card, g_ua.key, still ? "replaced by a newer one" : "gone");
        g_ua_have = 0;
        pb_usb_card(0);
        pl_core_usb(c, 0, NULL, 0, NULL);
    }
    if(pick < 0){
        ua_alsa_conf_write(ENG_USB_CONF, -1, 0, 0, logmsg);
        return;
    }
    g_ua = now[pick]; g_ua_have = 1; g_usb_vol = -1;
    logmsg("usb audio: card %d (%s, \"%s\", id %s) pcm %d%s", g_ua.card, g_ua.key, g_ua.name, g_ua.id, g_ua.pcm,
           g_ua.serial[0] ? "" : ", no serial number");
    int soft = 0;
    if(ua_mixer_find(g_ua.card, &g_ua_mix, logmsg) != 0 || !g_ua_mix.ok){
        /* none of its own: one of ours (softvol), made at silence; without it the card is not used */
        soft = 1;
        if(ua_softvol_prepare(g_ua.card, logmsg) != 0 || ua_mixer_find(g_ua.card, &g_ua_mix, logmsg) != 0 || !g_ua_mix.ok){
            logmsg("usb audio: card %d: its level can't be controlled - not used", g_ua.card);
            g_ua_have = 0;
            ua_alsa_conf_write(ENG_USB_CONF, -1, 0, 0, logmsg);
            return;
        }
    }
    pl_core_usb(c, 1, g_ua.key, g_ua.serial[0] != 0, g_ua.name);
    if(c->out_usb) fade_start(c, PL_O_USB, t);
    fade_apply(c, t);
    ua_mixer_set(g_ua.card, &g_ua_mix, c->app_usb, PL_VOL_MAX, logmsg);   /* now, before any stream opens (new: silent) */
    g_usb_vol = c->app_usb;
    g_ua_soft = soft;
    ua_alsa_conf_write(ENG_USB_CONF, g_ua.card, g_ua.pcm, soft, logmsg);
    pb_usb_card(1);
}

/* ---- the headphones' buttons and volume (btavrcp.c, AVRCP through BlueZ) ---- */
static pl_core_t *g_core;
static int *g_bt_vol;                      /* main's bt_vol: the volume the headphones were last given */
static char g_abs_mac[18];                 /* headphones whose own volume is the Bluetooth volume (native BlueALSA) */
static char soft_mac[18];                  /* headphones whose PCM was set to soft volume on this route */
static void hp_transport(int op){ pb_transport(op, 0); }
static void hp_volume(const char *mac, int v, int first){
    pl_core_t *c = g_core;
    if(!c || !pb_bluetooth() || strcasecmp(mac, pb_bt_mac())){
        logmsg("avrcp: volume %d from %s, not the headphones that play - left alone", v, mac);
        return;
    }
    if(first || strcasecmp(g_abs_mac, mac)){
        /* The headphones have absolute volume. Until now BlueALSA scaled the samples (SoftVolume) and the headphones
         * kept their own level; from here the Bluetooth volume is their level. The Disc's volume is given to them at
         * once, on the same connection after the switch, so the loudness does not jump to whatever they were set to. */
        if(access(BTAV_NO_ABS_VOLUME, F_OK) == 0){
            logmsg("avrcp: %s has absolute volume, not used (%s)", mac, BTAV_NO_ABS_VOLUME);
            return;
        }
        snprintf(g_abs_mac, sizeof g_abs_mac, "%s", mac);
        if(btav_soft_volume(mac, 0) != 0 || btav_pcm_volume(mac, btav_to127(c->app_bt, PL_VOL_MAX)) != 0){
            logmsg("avrcp: could not switch %s to absolute volume", mac);
            g_abs_mac[0] = 0;
            return;
        }
        if(g_bt_vol) *g_bt_vol = c->app_bt;
        logmsg("avrcp: %s: absolute volume, the Bluetooth volume %d now sets the headphones' level", mac, c->app_bt);
        return;
    }
    if(btav_to127(c->vol_bt, PL_VOL_MAX) == v) return;     /* our own change coming back */
    if(g_bt_vol && *g_bt_vol >= 0 && btav_to127(*g_bt_vol, PL_VOL_MAX) == v) return;   /* a fade step coming back */
    if(fade_busy(PL_O_BT)){ logmsg("avrcp: volume %d from %s during the fade in - left alone", v, mac); return; }
    pl_core_set_bt_volume(c, btav_from127(v, PL_VOL_MAX));
    /* the headphones have it already, so it is not sent back; above the volume limit (0716) the limit goes to them */
    if(g_bt_vol) *g_bt_vol = btav_to127(c->vol_bt, PL_VOL_MAX) == v ? c->vol_bt : -1;
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
#define VOL_FILE_MAX 8192          /* PL_DEV_MAX lines of at most ~140 bytes */
static void load_volumes(pl_core_t *c){
    FILE *f = fopen(OPL_VOLUMES, "re");
    if(!f){ if(errno != ENOENT) logmsg("outputs: open %s: %s", OPL_VOLUMES, strerror(errno)); return; }
    static char b[VOL_FILE_MAX];
    size_t n = fread(b, 1, sizeof b - 1, f);
    b[n] = 0;
    fclose(f);
    if(pl_core_vol_parse(c, b) != 0){ logmsg("outputs: %s has no volumes - the defaults", OPL_VOLUMES); return; }
    logmsg("outputs: main volume %d; AUX %d, digital %d, Bluetooth %d (%s)", c->volume, c->vol_aux, c->vol_dig, c->vol_bt, OPL_VOLUMES);
}
static void save_volumes(const pl_core_t *c){
    static char b[VOL_FILE_MAX];
    if(pl_core_vol_format(c, b, sizeof b) != 0){ logmsg("outputs: volumes too long for the file - not stored"); return; }
    char tmp[sizeof OPL_VOLUMES + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", OPL_VOLUMES);
    FILE *f = fopen(tmp, "we");
    if(!f || fputs(b, f) < 0 || fflush(f) != 0 || fsync(fileno(f)) != 0){
        logmsg("outputs: write %s: %s", tmp, strerror(errno));
        if(f) fclose(f);
        return;
    }
    fclose(f);
    if(rename(tmp, OPL_VOLUMES) != 0) logmsg("outputs: rename %s: %s", tmp, strerror(errno));
}
static void write_outputs(const pl_core_t *c, const char *path){
    static char last[1024];
    char b[1024];
    snprintf(b, sizeof b, "aux=%d\nbt=%d\ndig=%d\nvol_aux=%d\nvol_bt=%d\nvol_dig=%d\nbt_ready=%d\njack=%d\nbal=%d\ncodec=%s\n"
             "codecs=%s\nswitching=%d\nauto=%d\nnotice=%u %d\nusb=%d\nvol_usb=%d\nusb_ready=%d\nusb_name=%s\nusb_soft=%d\n"
             "main=%d\ncap=%d %d %d %d\nfade=%d %d %d %d\noff=%d %d %d %d\n",
             pb_aux(), pb_bluetooth(), pb_digital(), c->vol_aux, c->vol_bt, c->vol_dig, c->bt_ready, c->jack, c->bal, pb_bt_codec(), pb_bt_codecs(),
             pb_switching(), c->auto_out, c->notice_seq, c->notice, pb_usb(), c->vol_usb, c->usb_ready, c->usb_ready ? c->usb_name : "",
             c->usb_ready && g_ua_have && g_ua_soft, c->volume, c->cap[PL_O_AUX], c->cap[PL_O_BT], c->cap[PL_O_DIG], c->cap[PL_O_USB],
             c->fade[PL_O_AUX], c->fade[PL_O_BT], c->fade[PL_O_DIG], c->fade[PL_O_USB],
             c->off[PL_O_AUX], c->off[PL_O_BT], c->off[PL_O_DIG], c->off[PL_O_USB]);
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
    static int last[5] = { -1, -1, -1, -1, -1 };
    if(c->volume == last[0] && c->vol_aux == last[1] && c->vol_bt == last[2] && c->vol_dig == last[3] && c->vol_usb == last[4]) return;
    last[0] = c->volume; last[1] = c->vol_aux; last[2] = c->vol_bt; last[3] = c->vol_dig; last[4] = c->vol_usb;
    FILE *f = fopen(OPL_VOLUME_LOG, "ae");
    if(!f){ logmsg("volume log: open %s: %s", OPL_VOLUME_LOG, strerror(errno)); return; }
    time_t now = time(NULL); struct tm tm; char ts[32] = "";
    if(localtime_r(&now, &tm)) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S ", &tm);
    fprintf(f, "%sVolume: Main %d, AUX %d%s, Digital %d%s, BT %d%s, USB %d%s\n", ts, c->volume, c->vol_aux, pb_aux() ? "" : " (off)",
            c->vol_dig, pb_digital() ? "" : " (off)", c->vol_bt, pb_bluetooth() ? "" : " (off)", c->vol_usb, pb_usb() ? "" : " (off)");
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
    core.upnext = core_upnext;
    core.slist = core_slist;
    core.playsong = core_playsong;
    /* tell the UI that the own player runs this boot, so both use the card's library when there is one (ui/libdb.h) */
    {
        char tmp[64]; snprintf(tmp, sizeof tmp, "%s.%ld", LIBDB_OWN_PID, (long)getpid());
        FILE *pf = fopen(tmp, "w");
        if(!pf || fprintf(pf, "%ld\n", (long)getpid()) < 0 || fclose(pf) != 0 || rename(tmp, LIBDB_OWN_PID) != 0)
            logmsg("libdb: %s: %s", LIBDB_OWN_PID, strerror(errno));
    }
    pb_init(&core, "", QD_SYSCONFIG_DB, logmsg);   /* "": follow libdb_path() on every open */
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
    /* Charging Limit (0808), kept in SYSCONFIG.CHARGE_PROTECT like the stock player keeps it: the UI reads it back from
     * there at every start (ui/power.c power_startup_mirror) and sends 0808 only when it is changed */
    int cv = use_wd ? qd_sys_get(QD_SYSCONFIG_DB, "CHARGE_PROTECT") : -1;
    core.charge = cv > 0 ? 1 : 0;
    int charge_saved = core.charge;
    hw_charge_t chg; hw_charge_init(&chg);
    logmsg("charge: Charging Limit %s (SYSCONFIG %d)", core.charge ? "on" : "off", cv);

    if(use_wd) load_volumes(&core);                            /* host runs (-n) keep the defaults */
    if(use_wd) load_audio_conf();
    core.app_aux = core.vol_aux; core.app_bt = core.vol_bt; core.app_dig = core.vol_dig; core.app_usb = core.vol_usb;
    g_fcore = &core;
    eng_set_before_play(before_play);
    if(use_wd && (mkdir(ENG_RUN_DIR, 0755) == 0 || errno == EEXIST))
        ua_alsa_conf_write(ENG_USB_CONF, -1, 0, 0, logmsg);       /* no card until usb_tick finds one (not one of a run before) */
    if(use_wd && dac_open(0, logmsg) != 0) logmsg("dac: not available - no sound");
    if(use_wd) mix_route_dac(logmsg);                          /* the stock player sets its route at start too */
    if(eng_start(0, logmsg) == 0) pb_engine_event();           /* MPD is not hardware: host runs (-n) use it too when present */
    if(use_wd) pb_resume_begin(sd_is_mounted, RESUME_FLAG);    /* host runs (-n) have no library */
    g_core = &core; g_bt_vol = &g_bt_last;
    btav_cb hp = { logmsg, hp_transport, hp_volume };
    if(use_wd || getenv("DISKOS_PLAYER_BUS"))                /* host runs (-n) only on a bus they name */
        btav_init(&hp, getenv("DISKOS_PLAYER_BUS"));        /* NULL: the system bus */
    unsigned route_seq = core.route_seq, codec_seq = core.codec_seq;
    unsigned saved_seq = core.vol_seq;
    double usb_at = 0, role_at = 0;
    unsigned blocked_seq = eng_blocked_seq();
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
        struct pollfd pf[5] = { { (int)pl, POLLIN, 0 }, { keys, POLLIN, 0 }, { uev, POLLIN, 0 }, { eng_idle_fd(), POLLIN, 0 },
                                { btav_fd(), POLLIN, 0 } };
        int fading = fade_busy(0) || fade_busy(1) || fade_busy(2) || fade_busy(3);
        int pr = poll(pf, 5, fading ? 50 : 250);      /* a fade in steps every 50 ms */
        if(pr > 0 && pf[4].fd >= 0 && pf[4].revents) btav_io();
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
        if(use_wd && t - role_at >= 2.0){              /* the host role for USB devices; not while the card is exported */
            role_at = t;
            ua_role_tick(us_active(), logmsg);
        }
        if(use_wd && t - usb_at >= 1.0){ usb_at = t; usb_tick(&core, t); }
        if(core.route_seq != route_seq){
            route_seq = core.route_seq;
            /* outputs that come on fade in: silent before they open */
            const int was[PL_O_N] = { pb_aux(), pb_bluetooth(), pb_digital(), pb_usb() };
            const int will[PL_O_N] = { core.out_aux, core.out_bt, core.out_dig, core.out_usb && core.usb_ready };
            for(int o = 0; o < PL_O_N; o++) if(will[o] && !was[o]) fade_start(&core, o, t);
            fade_apply(&core, t);
            apply_volumes(&core, t);
            pb_route();
            g_bt_last = core.app_bt;               /* pb_route gave it to the headphones */
        }
        if(core.codec_seq != codec_seq){ codec_seq = core.codec_seq; pb_codec(core.codec_want); }
        if(g_abs_mac[0] && (!pb_bluetooth() || strcasecmp(g_abs_mac, pb_bt_mac()))) g_abs_mac[0] = 0;   /* they left */
        /* Soft volume unless the link has absolute volume now. BlueALSA keeps a PCM's SoftVolume across BlueZ's transport
         * coming and going, but absolute volume lives in the transport: when it went (avrcp "link gone") and the next one
         * reports no Volume, BlueZ refuses the PCM volume ("No such property 'Volume'" in /tmp/bluealsa.log) and the
         * Bluetooth volume did nothing (Jo 2026-11-24, XQ-50 PRO2 receiver). So: on every new Bluetooth route, and when
         * the absolute-volume link is gone, the PCM goes back to soft volume and gets the Bluetooth volume again; a link
         * that reports its Volume switches it off again (hp_volume). */
        if(pb_bluetooth() && !pb_switching() && btav_fd() >= 0){
            const char *bm = pb_bt_mac();
            int abs = btav_volume_of(bm) >= 0;
            if(g_abs_mac[0] && !abs){
                logmsg("avrcp: %s: absolute volume gone with its link, back to soft volume", bm);
                g_abs_mac[0] = 0; soft_mac[0] = 0;
            }
            if(!abs && !g_abs_mac[0] && strcasecmp(soft_mac, bm) && btav_soft_volume(bm, 1) == 0){
                snprintf(soft_mac, sizeof soft_mac, "%s", bm);
                g_bt_last = -1;                                   /* the Bluetooth volume again, now scaled by BlueALSA */
            }
        }
        if(!pb_bluetooth() || (soft_mac[0] && strcasecmp(soft_mac, pb_bt_mac()))) soft_mac[0] = 0;
        fade_apply(&core, t);
        apply_volumes(&core, t);
        if(eng_blocked_seq() != blocked_seq){             /* a play was held back: no output is on */
            blocked_seq = eng_blocked_seq();
            pl_core_notice(&core, PL_NOTICE_NONE);
        }
        if(core.vol_seq != saved_seq){
            if(!vol_changed) vol_changed = t;
            if(t - vol_changed >= VOL_SAVE_DELAY_S){
                saved_seq = core.vol_seq; vol_changed = 0;
                if(use_wd) save_volumes(&core);
            }
        }
        pb_tick(t);
        btav_tick(t);
        {
            const eng_status_t *es = pb_status(NULL);
            btav_status(es ? es->state : ENG_STOP, es ? es->title : "", es ? es->artist : "", es ? es->duration_ms : 0);
        }
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
        if(core.charge != charge_saved){
            charge_saved = core.charge;
            if(use_wd && qd_sys_set(QD_SYSCONFIG_DB, "CHARGE_PROTECT", core.charge) != 0)
                logmsg("charge: can't store CHARGE_PROTECT in %s", QD_SYSCONFIG_DB);
            chg.polled_at = -1e9;                        /* decide now, not at the next poll */
        }
        hw_charge_tick(&chg, core.charge, t, !use_wd, logmsg);
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
            if(use_wd && core.vol_seq != saved_seq) save_volumes(&core);
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
    hw_charge_release(&chg, !use_wd, logmsg);
    btav_close();
    if(use_wd && core.vol_seq != saved_seq) save_volumes(&core);
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
