/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "core.h"
#include "proto.h"
#include <stdarg.h>
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
    c->out_aux = 1; c->vol_aux = c->vol_bt = c->volume; c->codec_want = -1;
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
/* the lead output's volume becomes the one shown; the UI hears a change */
static void lead_volume(pl_core_t *c, int tell){
    int v = c->out_bt ? c->vol_bt : c->vol_aux;
    if(v == c->volume) return;
    c->volume = v;
    if(tell) send_volume(c);
}

void pl_core_set_volume(pl_core_t *c, int v){
    v = clampv(c, v);
    int d = v - c->volume;
    if(c->out_aux || !c->out_bt) c->vol_aux = clampv(c, c->vol_aux + d);
    if(c->out_bt) c->vol_bt = clampv(c, c->vol_bt + d);
    c->volume = c->out_bt ? c->vol_bt : c->vol_aux;
    logf_(c, "volume = %d (AUX %d%s, Bluetooth %d%s)", c->volume, c->vol_aux, c->out_aux ? "" : " off",
          c->vol_bt, c->out_bt ? "" : " off");
    send_volume(c);
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

static void plug_changed(pl_core_t *c, int was_plugged, const char *which, int plugged){
    int now = pl_core_plugged(c);
    if(plugged){
        logf_(c, "jack: %s in%s", which, was_plugged ? "" : " (playback stays as it is)");
        if(!was_plugged) set_outputs(c, 1, 0, "plug in");
        return;
    }
    if(now){ logf_(c, "jack: %s out, the other plug is still in", which); return; }
    if(c->out_bt){
        logf_(c, "jack: %s out (Bluetooth plays on)", which);
        set_outputs(c, 0, 1, "plug out");
        return;
    }
    logf_(c, "jack: %s out - pause", which);
    if(c->transport) c->transport(c->ctx, PL_T_PAUSE, 0);
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
        if(v != PL_WORK_LOCAL){ logf_(c, "work mode %d: not supported by the own player yet, ignored", v); return 0; }
        int changed = c->work_mode != v;
        c->work_mode = v;
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
        c->bt_ready = 0; c->bt_auto = 0;                       /* the headphones are gone (or the UI chose the DAC) */
        set_outputs(c, 1, 0, "route to the DAC");
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
            if(c->bt_auto || new_mac){ c->bt_auto = 0; set_outputs(c, 0, 1, "headphones connected"); }
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
