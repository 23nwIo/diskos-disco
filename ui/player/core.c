/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "core.h"
#include "proto.h"
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void logf_(pl_core_t *c, const char *fmt, ...){
    if(!c->log) return;
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    c->log(c->ctx, b);
}
static void send_(pl_core_t *c, const char *f, int len){ if(c->send && len > 0) c->send(c->ctx, f, len); }

void pl_core_init(pl_core_t *c, pl_send_fn send, pl_log_fn log, void *ctx){
    memset(c, 0, sizeof *c);
    c->send = send; c->log = log; c->ctx = ctx;
    /* Defaults until the UI re-sends its settings (ui_reapply_audio does that once the player answers). The volume
     * starts low on purpose: a player that has not been told the volume yet must not be loud. */
    c->volume = 20; c->max_volume = PL_VOL_MAX;
    c->gain = 0; c->dre = 0; c->work_mode = -1; c->route = -1; c->jack = -1; c->bal = -1; c->artist_grouping = -1;
    c->out_aux = 1; c->vol_aux = c->vol_bt = c->volume; c->codec_want = -1; c->auto_out = PL_AUTO_NEWEST;
}

void pl_core_send_state(pl_core_t *c){
    /* No playback yet. The stock player answers 0202 only while a track is loaded; when idle it sends nothing, but it
     * has already sent its start-up frames (settings, serial number), and the UI takes any frame as "the player is
     * up". This player has no start-up frames yet, so it answers with the partial state frame the stock player sends
     * on pause: {"state":1} (0 = playing, 1 = paused; capture 2026-11-15). A partial frame leaves the UI's track as is. */
    (void)c;
    char f[32];
    send_(c, f, pp_fmt_a2(f, sizeof f, "{\"state\":1}"));
}

static void send_volume(pl_core_t *c){ char f[16]; send_(c, f, pp_fmt_a714(f, sizeof f, c->volume)); }

static int clampv(const pl_core_t *c, int v){ return v < 0 ? 0 : v > c->max_volume ? c->max_volume : v; }
/* what plays: Bluetooth when it is on and the headphones are there; the DAC when AUX is on or stands in for Bluetooth
 * that waits for the headphones (playback.c pb_route plays to one output at least) */
static int bt_eff(const pl_core_t *c){ return c->out_bt && c->bt_ready; }
static int aux_eff(const pl_core_t *c){ return c->out_aux || !bt_eff(c); }
/* the lead output's volume becomes the one shown; the UI hears a change */
static void lead_volume(pl_core_t *c, int tell){
    int v = bt_eff(c) ? c->vol_bt : c->vol_aux;
    if(v == c->volume) return;
    c->volume = v;
    if(tell) send_volume(c);
}

void pl_core_set_volume(pl_core_t *c, int v){
    v = clampv(c, v);
    int d = v - c->volume;
    if(aux_eff(c)) c->vol_aux = clampv(c, c->vol_aux + d);
    if(bt_eff(c)) c->vol_bt = clampv(c, c->vol_bt + d);
    c->volume = bt_eff(c) ? c->vol_bt : c->vol_aux;
    logf_(c, "volume = %d (AUX %d%s, Bluetooth %d%s)", c->volume, c->vol_aux, c->out_aux ? "" : " off",
          c->vol_bt, c->out_bt ? "" : " off");
    send_volume(c);
}

void pl_core_set_bt_volume(pl_core_t *c, int v){
    v = clampv(c, v);
    if(v == c->vol_bt) return;
    int d = v - c->vol_bt;
    c->vol_bt = v;
    if(c->hp_vol_all && c->out_aux){                         /* 0d06 1: AUX that plays alongside moves by the same step */
        c->vol_aux = clampv(c, c->vol_aux + d);
        logf_(c, "volume: Bluetooth %d, AUX %d (from the headphones)", v, c->vol_aux);
    } else logf_(c, "volume: Bluetooth %d (from the headphones)", v);
    lead_volume(c, 1);
}

void pl_core_load_volumes(pl_core_t *c, int vol_aux, int vol_bt){
    c->vol_aux = clampv(c, vol_aux); c->vol_bt = clampv(c, vol_bt);
    lead_volume(c, 1);
}

void pl_core_volume_step(pl_core_t *c, int dir){
    int v = c->volume + (dir > 0 ? 1 : -1);
    if(v < 0 || v > c->max_volume){ logf_(c, "volume key: already at %d", c->volume); return; }
    pl_core_set_volume(c, v);
}

/* switch the outputs; why: for the log */
static void set_outputs(pl_core_t *c, int aux, int bt, const char *why){
    if(c->out_aux == aux && c->out_bt == bt) return;
    c->out_aux = aux; c->out_bt = bt; c->route_seq++;
    logf_(c, "outputs: AUX %s, Bluetooth %s (%s)", aux ? "on" : "off", bt ? "on" : "off", why);
    lead_volume(c, 1);
}

int pl_core_plugged(const pl_core_t *c){ return c->jack == 1 || c->bal == 1; }
/* a plug state is known for at least one jack */
static int plug_known(const pl_core_t *c){ return c->jack >= 0 || c->bal >= 0; }
static void notice(pl_core_t *c, int what){
    c->notice = what; c->notice_seq++;
    logf_(c, "outputs: %s connected, but %s is off (auto outputs off) - notice to the UI",
          what == PL_NOTICE_AUX ? "a plug" : "headphones", what == PL_NOTICE_AUX ? "AUX" : "Bluetooth");
}
static void pause_(pl_core_t *c){ if(c->transport) c->transport(c->ctx, PL_T_PAUSE, 0); }
static const char *const AUTO_NAME[] = { "off", "newest", "all" };

static void plug_changed(pl_core_t *c, int was_plugged, const char *which, int plugged){
    int now = pl_core_plugged(c), bt_live = c->out_bt && c->bt_ready;
    if(plugged){
        logf_(c, "jack: %s in%s", which, was_plugged ? "" : " (playback stays as it is)");
        if(was_plugged) return;
        if(c->auto_out == PL_AUTO_NEWEST) set_outputs(c, 1, 0, "plug in");
        else if(c->auto_out == PL_AUTO_ALL) set_outputs(c, 1, c->out_bt, "plug in");
        else if(!c->out_aux) notice(c, PL_NOTICE_AUX);
        return;
    }
    if(now){ logf_(c, "jack: %s out, the other plug is still in", which); return; }
    if(c->bt_ready && c->auto_out != PL_AUTO_OFF){     /* AUX off; the connected headphones are what is left */
        if(bt_live) logf_(c, "jack: %s out (Bluetooth plays on)", which);
        else { logf_(c, "jack: %s out - pause, Bluetooth takes over", which); pause_(c); }
        set_outputs(c, 0, 1, "plug out");
        return;
    }
    if(bt_live && c->out_aux){ logf_(c, "jack: %s out, Bluetooth plays on (auto outputs off: AUX stays on)", which); return; }
    if(bt_live){ logf_(c, "jack: %s out, AUX is off", which); return; }
    /* the DAC plays (AUX on, or standing in while Bluetooth waits for the headphones) */
    logf_(c, "jack: %s out - pause", which);
    pause_(c);
}

/* the headphones are gone: the UI routed away from them or their link went (0666 6) */
static void bt_gone(pl_core_t *c, const char *why){
    int was = c->bt_ready;
    c->bt_ready = 0; c->bt_auto = 0;
    if(!c->out_bt){ if(was) logf_(c, "bluetooth: headphones gone (%s), Bluetooth was off", why); return; }
    if(c->out_aux && c->auto_out != PL_AUTO_OFF){ set_outputs(c, 1, 0, why); return; }   /* AUX plays on */
    if(c->out_aux){ logf_(c, "bluetooth: headphones gone (%s), AUX plays on (auto outputs off: Bluetooth stays on)", why); return; }
    /* Bluetooth was the only output: pause (no sudden sound elsewhere). A plug is in: AUX takes over (auto on). Else
     * the choice waits for the headphones and the DAC plays meanwhile, so the UI's play goes on (playback.c pb_route). */
    logf_(c, "bluetooth: headphones gone (%s) - pause", why);
    pause_(c);
    if(c->auto_out != PL_AUTO_OFF && pl_core_plugged(c)) set_outputs(c, 1, 0, "headphones gone, a plug is in");
    else logf_(c, "outputs: Bluetooth waits for the headphones, the DAC plays meanwhile");
    lead_volume(c, 1);
}

/* 0d05 to all: every connected device on (a plug: AUX, connected headphones: Bluetooth), nothing else */
static void auto_all_now(pl_core_t *c){
    int aux = plug_known(c) ? pl_core_plugged(c) : c->out_aux, bt = c->bt_ready ? 1 : c->out_bt;
    if(!aux && !bt) return;                          /* nothing connected: the choice stays */
    set_outputs(c, aux, bt, "auto outputs: all connected");
}

static void plug(pl_core_t *c, int *slot, const char *which, int plugged){
    if(*slot == plugged) return;
    int first = *slot < 0, was = pl_core_plugged(c);
    *slot = plugged;
    if(first){ logf_(c, "jack: %s %s", which, plugged ? "in" : "out"); return; }
    plug_changed(c, was, which, plugged);
}
void pl_core_jack(pl_core_t *c, int plugged){ plug(c, &c->jack, "3.5 mm headphones", plugged); }
void pl_core_bal(pl_core_t *c, int plugged){ plug(c, &c->bal, "4.4 mm balanced headphones", plugged); }

/* a class 1 setting: store v in *slot if it is within lo..hi */
static int set_(pl_core_t *c, const char *name, int *slot, int v, int lo, int hi){
    if(v < lo || v > hi){ logf_(c, "%s: value %d out of range %d..%d, ignored", name, v, lo, hi); return -1; }
    *slot = v;
    logf_(c, "%s = %d", name, v);
    return 0;
}

int pl_core_handle(pl_core_t *c, const char *buf, int n){
    pp_frame_t f;
    c->frames++;
    if(n < 0 || pp_parse(buf, (size_t)n, &f) != 0){
        c->bad_frames++;
        logf_(c, "malformed frame (%d chars): %.*s", n, n > 64 ? 64 : (n < 0 ? 0 : n), buf);
        return -1;
    }
    int v = pp_class1(&f);
    switch(f.tag){
    case 0x0202:
        if(c->transport && c->transport(c->ctx, PL_T_STATE, 0) == 0) return 0;
        pl_core_send_state(c); return 0;

    /* transport and queue */
    case 0x0100: {
        if(f.dlen < 8){ c->bad_frames++; logf_(c, "play list: frame too short"); return -1; }
        pp_frame_t pos = { 0, f.data, 4 }, type = { 0, f.data + 4, 4 };
        int p = pp_class1(&pos), t = pp_class1(&type);
        if(p < 0 || t < 0){ c->bad_frames++; logf_(c, "play list: bad frame"); return -1; }
        logf_(c, "play list: pos %d type %d name \"%.*s\"", p, t, f.dlen - 8, f.data + 8);
        if(!c->play){ pl_core_send_state(c); return 0; }
        char name[PP_MAX_FRAME];
        memcpy(name, f.data + 8, (size_t)(f.dlen - 8)); name[f.dlen - 8] = 0;
        if(c->play(c->ctx, t, p, name) != 0) logf_(c, "play list: nothing plays");
        return 0;
    }
    case 0x0103:
        if(f.dlen != 8){ c->bad_frames++; logf_(c, "seek: bad frame"); return -1; }
    {
        char h[9]; memcpy(h, f.data, 8); h[8] = 0;
        char *end; unsigned long ms = strtoul(h, &end, 16);
        if(*end){ c->bad_frames++; logf_(c, "seek: bad frame"); return -1; }
        logf_(c, "seek to %lu ms", ms);
        if(c->transport) c->transport(c->ctx, PL_T_SEEK, (long)ms);
        return 0;
    }
    case 0x0201:
        if(v < 0 || v > 2){ logf_(c, "transport %d: unknown, ignored", v); return -1; }
        logf_(c, "transport %d (0 play/pause, 1 next, 2 previous)%s", v, c->transport ? "" : " - no engine");
        if(c->transport) c->transport(c->ctx, v == 0 ? PL_T_TOGGLE : v == 1 ? PL_T_NEXT : PL_T_PREV, 0);
        return 0;
    case 0x0104: logf_(c, "favourite %d (not implemented yet)", v); return 0;
    case 0x0102: {
        int old = c->play_mode;
        if(set_(c, "play mode", &c->play_mode, v, 0, 4)) return -1;
        if(old != v && c->transport) c->transport(c->ctx, PL_T_MODE, v);
        return 0;
    }
    case 0x0648: return set_(c, "artist grouping", &c->artist_grouping, v, 0, 1);

    /* audio settings */
    case 0x0715:
        if(v < 0 || v > PL_VOL_MAX){ logf_(c, "volume: value %d out of range, ignored", v); return -1; }
        pl_core_set_volume(c, v);
        return 0;
    case 0x0711:
        if(set_(c, "max volume", &c->max_volume, v, 0, PL_VOL_MAX)) return -1;
        c->vol_aux = clampv(c, c->vol_aux); c->vol_bt = clampv(c, c->vol_bt);
        lead_volume(c, 1);
        return 0;
    case 0x0713: {
        int n2 = v & 0xff;
        if(v < 0 || (v >> 8) > 1 || n2 > 10){ logf_(c, "balance: value %d out of range, ignored", v); return -1; }
        c->balance = v; logf_(c, "balance = %04x", v); return 0;
    }
    case 0x0645: case 0x0649: return set_(c, "gain", &c->gain, v, 0, 1);
    case 0x0653: return set_(c, "dac filter", &c->dac_filter, v, 0, 5);
    case 0x0812: return set_(c, "dre (0 on, 1 off)", &c->dre, v, 0, 1);
    case 0x0689: return set_(c, "eq preset", &c->eq_preset, v, 0, 20);
    case 0x0718: return set_(c, "replaygain", &c->replaygain, v, 0, 2);

    case 0x0657: {
        if(v < 0){ c->bad_frames++; logf_(c, "work mode: bad frame"); return -1; }
        if(v != PL_WORK_LOCAL && v != PL_WORK_DLNA){ logf_(c, "work mode %d: not supported by the own player yet, ignored", v); return 0; }
        int changed = c->work_mode != v;
        c->work_mode = v;
        if(changed) c->mode_seq++;
        logf_(c, "work mode = %d%s", v, changed ? " (announced)" : "");
        if(changed){ char a[16]; send_(c, a, pp_fmt_a607(a, sizeof a, v)); }   /* the stock player announces a change */
        return 0;
    }
    case 0x0666:
        if(v < 0){ c->bad_frames++; logf_(c, "output route: bad frame"); return -1; }
        if(v != PL_ROUTE_DAC && v != PL_ROUTE_BT){ logf_(c, "output route %d: not supported by the own player yet, ignored", v); return 0; }
        c->route = v; c->route_seq++;
        logf_(c, "output route = %d (%s)", v, v == PL_ROUTE_BT ? "Bluetooth" : "DAC");
        if(v == PL_ROUTE_BT){ c->bt_auto = 1; return 0; }
        bt_gone(c, "route to the DAC");                        /* the headphones are gone (or the UI chose the DAC) */
        return 0;
    case 0x06c1:          /* the stock player's Bluetooth init; the UI brings Bluetooth up itself (ui/bt.c) */
        logf_(c, "bluetooth init: nothing to do");
        return 0;
    case 0x06b3: {        /* codec VALUE1 (4 hex) + the headphones' MAC (17), ui/bt.c bt_codec_frame_value */
        pp_frame_t cv = { 0, f.data, 4 };
        int codec = f.dlen == 4 + 17 ? pp_class1(&cv) : -1;
        int ok = codec >= 0 && codec <= 4;
        for(int i = 0; ok && i < 17; i++){
            char ch = f.data[4 + i];
            ok = (i % 3 == 2) ? ch == ':' : ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'));
        }
        if(!ok){ c->bad_frames++; logf_(c, "bluetooth codec: bad frame (%.*s)", f.dlen > 48 ? 48 : f.dlen, f.data); return -1; }
        int new_mac = strncmp(c->bt_mac, f.data + 4, 17) != 0;
        memcpy(c->bt_mac, f.data + 4, 17); c->bt_mac[17] = 0;
        c->bt_codec = codec; c->route_seq++;
        logf_(c, "bluetooth codec %d for %s", codec, c->bt_mac);
        if(c->route == PL_ROUTE_BT){
            c->bt_ready = 1;
            if(c->bt_auto || new_mac){
                c->bt_auto = 0;
                if(c->auto_out == PL_AUTO_NEWEST) set_outputs(c, 0, 1, "headphones connected");
                else if(c->auto_out == PL_AUTO_ALL)
                    set_outputs(c, plug_known(c) ? pl_core_plugged(c) : c->out_aux, 1, "headphones connected");
                else if(!c->out_bt) notice(c, PL_NOTICE_BT);
                else logf_(c, "outputs: headphones connected, Bluetooth plays (auto outputs off)");
            }
            lead_volume(c, 1);
        }
        return 0;
    }

    /* Outputs (diskOS; only the own player gets these, the UI sends them only when it runs) */
    case 0x0d01: {
        int aux = v >= 0 && (v & 1), bt = v >= 0 && (v & 2);
        if(v < 0 || v > 3){ c->bad_frames++; logf_(c, "outputs: bad value %d", v); return -1; }
        if(!aux && !bt){ logf_(c, "outputs: none asked - one must stay on, ignored"); return -1; }
        if(bt && !c->bt_ready){ logf_(c, "outputs: Bluetooth asked, but no headphones are connected - ignored"); return -1; }
        set_outputs(c, aux, bt, "by hand");
        return 0;
    }
    case 0x0d02: case 0x0d03: {
        int *slot = f.tag == 0x0d02 ? &c->vol_aux : &c->vol_bt;
        if(v < 0 || v > PL_VOL_MAX){ logf_(c, "%s volume: value %d out of range, ignored", f.tag == 0x0d02 ? "AUX" : "Bluetooth", v); return -1; }
        *slot = clampv(c, v);
        logf_(c, "%s volume = %d", f.tag == 0x0d02 ? "AUX" : "Bluetooth", *slot);
        lead_volume(c, 1);
        return 0;
    }
    case 0x0d10: {        /* the Radio app's station: name length (4 hex), the name, the URL; nothing after the length = stop */
        long nl = f.dlen >= 4 ? strtol((char[]){ f.data[0], f.data[1], f.data[2], f.data[3], 0 }, NULL, 16) : -1;
        if(f.dlen < 4 || nl < 0 || 4 + nl > f.dlen){ c->bad_frames++; logf_(c, "radio: bad frame"); return -1; }
        char *name = malloc((size_t)nl + 1), *url = malloc((size_t)(f.dlen - 4 - nl) + 1);
        if(!name || !url){ free(name); free(url); logf_(c, "radio: out of memory"); return -1; }
        memcpy(name, f.data + 4, (size_t)nl); name[nl] = 0;
        memcpy(url, f.data + 4 + nl, (size_t)(f.dlen - 4 - nl)); url[f.dlen - 4 - nl] = 0;
        int r = 0;
        if(c->work_mode != PL_WORK_DLNA){ logf_(c, "radio: not in the radio work mode (0657 0C), ignored"); r = -1; }
        else if(!c->radio) logf_(c, "radio: \"%s\" %s (no playback here)", name, url[0] ? url : "stop");
        else r = c->radio(c->ctx, name, url);
        free(name); free(url);
        return r;
    }
    case 0x0d12: {        /* the station for PL_END_STATION, as 0d10: name length (4 hex), the name, the URL; nothing = none */
        long nl = f.dlen >= 4 ? strtol((char[]){ f.data[0], f.data[1], f.data[2], f.data[3], 0 }, NULL, 16) : -1;
        if(f.dlen < 4 || nl < 0 || 4 + nl > f.dlen){ c->bad_frames++; logf_(c, "end station: bad frame"); return -1; }
        long ul = f.dlen - 4 - nl;
        if(nl >= (long)sizeof c->end_name || ul >= (long)sizeof c->end_url){ c->bad_frames++; logf_(c, "end station: too long"); return -1; }
        memcpy(c->end_name, f.data + 4, (size_t)nl); c->end_name[nl] = 0;
        memcpy(c->end_url, f.data + 4 + nl, (size_t)ul); c->end_url[ul] = 0;
        logf_(c, "end station: \"%s\" %s", c->end_name, c->end_url[0] ? c->end_url : "(none)");
        return 0;
    }
    case 0x0d13: {        /* the UI's queue: ack (8 hex), party (1 char), then one SONG.ID per 8 hex */
        if(f.dlen < 9 || (f.dlen - 9) % 8 || (f.data[8] != '0' && f.data[8] != '1') || (f.dlen - 9) / 8 > PL_UP_MAX){
            c->bad_frames++; logf_(c, "up next: bad frame (%d chars)", f.dlen); return -1;
        }
        int n = (f.dlen - 9) / 8, ids[PL_UP_MAX];
        char h[9]; h[8] = 0;
        memcpy(h, f.data, 8);
        for(int k = 0; k < 8; k++) if(!isxdigit((unsigned char)h[k])){ c->bad_frames++; logf_(c, "up next: bad ack"); return -1; }
        unsigned ack = (unsigned)strtoul(h, NULL, 16);
        for(int i = 0; i < n; i++){
            memcpy(h, f.data + 9 + 8 * i, 8);
            for(int k = 0; k < 8; k++) if(!isxdigit((unsigned char)h[k])){ c->bad_frames++; logf_(c, "up next: bad song id"); return -1; }
            long id = strtol(h, NULL, 16);
            if(id <= 0 || id > 0x7fffffffL){ c->bad_frames++; logf_(c, "up next: bad song id"); return -1; }
            ids[i] = (int)id;
        }
        if(!c->upnext){ logf_(c, "up next: %d songs (no playback here)", n); return 0; }
        return c->upnext(c->ctx, ack, f.data[8] == '1', ids, n);
    }
    case 0x0d14:          /* a stream list (Bandcamp): play OPL_BC_LIST from track v */
        if(v < 0){ c->bad_frames++; logf_(c, "stream list: bad frame"); return -1; }
        if(c->work_mode != PL_WORK_DLNA){ logf_(c, "stream list: not in the radio work mode (0657 0C), ignored"); return -1; }
        if(!c->slist){ logf_(c, "stream list: track %d (no playback here)", v); return 0; }
        return c->slist(c->ctx, v);
    case 0x0d15: {        /* play SONG.ID (8 hex) now, on its own */
        char h[9]; long id = -1;
        if(f.dlen == 8){ memcpy(h, f.data, 8); h[8] = 0; int ok = 1; for(int k = 0; k < 8; k++) ok &= isxdigit((unsigned char)h[k]) != 0; if(ok) id = strtol(h, NULL, 16); }
        if(id <= 0 || id > 0x7fffffffL){ c->bad_frames++; logf_(c, "play song: bad frame"); return -1; }
        if(!c->playsong){ logf_(c, "play song %ld (no playback here)", id); return 0; }
        return c->playsong(c->ctx, (int)id);
    }
    case 0x0d11:
        if(v < PL_END_STOP || v > PL_END_STATION){ c->bad_frames++; logf_(c, "list end: bad value %d", v); return -1; }
        if(v != c->end_mode) logf_(c, "list end = %s", v == PL_END_STOP ? "stop" : v == PL_END_UI ? "the UI picks" : "station");
        c->end_mode = v;
        return 0;
    case 0x0d06:
        if(v != 0 && v != 1){ c->bad_frames++; logf_(c, "headphone volume: bad value %d", v); return -1; }
        if(v == c->hp_vol_all) return 0;
        c->hp_vol_all = v;
        logf_(c, "headphone volume = %s", v ? "all outputs" : "Bluetooth only");
        return 0;
    case 0x0d05:
        if(v < PL_AUTO_OFF || v > PL_AUTO_ALL){ c->bad_frames++; logf_(c, "auto outputs: bad value %d", v); return -1; }
        if(v == c->auto_out) return 0;
        c->auto_out = v;
        logf_(c, "auto outputs = %s", AUTO_NAME[v]);
        if(v == PL_AUTO_ALL) auto_all_now(c);
        return 0;
    case 0x0d04:
        if(v != 0 && v != 1 && v != 2 && v != 5 && v != 6){ logf_(c, "bluetooth codec %d: unknown, ignored", v); return -1; }
        if(!c->bt_ready || !c->out_bt){ logf_(c, "bluetooth codec %d: Bluetooth is not playing, ignored", v); return -1; }
        c->codec_want = v; c->codec_seq++;
        logf_(c, "bluetooth codec %d asked (this connection)", v);
        return 0;

    case 0x0664: return set_(c, "idle power off s", &c->idle_off_s, v, 0, 0xffff);
    case 0x0806:
        if(set_(c, "sleep timer s (power off when it runs out, 0 cancels)", &c->sleep_s, v, 0, 0xffff)) return -1;
        c->sleep_seq++;
        return 0;

    /* remembered only */
    case 0x0647: return set_(c, "gapless", &c->gapless, v, 0, 1);
    case 0x0646: return set_(c, "rotation", &c->rotation, v, 0, 0xffff);
    case 0x064d: return set_(c, "track numbers", &c->track_numbers, v, 0, 1);
    case 0x0684: return set_(c, "resume mode", &c->resume_mode, v, 0, 2);
    case 0x0687: return set_(c, "folder play", &c->folder_play, v, 0, 1);
    case 0x0808: return set_(c, "charge option", &c->charge, v, 0, 1);
    case 0x0820: case 0x0821: case 0x0822:
        return set_(c, "key action", &c->key_action[f.tag - 0x0820], v, 0, 0xffff);

    /* accepted, belong to later steps */
    case 0x0642:
        if(v < 0){ c->bad_frames++; logf_(c, "usb gadget: bad frame"); return -1; }
        if(v != 0 && v != 1){ logf_(c, "usb gadget %d: not supported by the own player yet, ignored", v); return 0; }
        c->usb_gadget = v; c->usb_seq++;
        logf_(c, "usb gadget = %d (%s)", v, v ? "USB storage" : "none");
        return 0;
    case 0x0622: case 0x0818:
        logf_(c, "%04x: not handled by the own player yet (%.*s)", f.tag, f.dlen > 48 ? 48 : f.dlen, f.data);
        return 0;
    }
    c->unknown_tags++;
    logf_(c, "unknown tag %04x (%d chars of data), ignored", f.tag, f.dlen);
    return -1;
}
