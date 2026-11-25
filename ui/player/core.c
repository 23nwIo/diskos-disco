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
    c->out_aux = 1; c->mem_aux = 1; c->codec_want = -1; c->auto_out = PL_AUTO_NEWEST;
    for(int o = 0; o < PL_O_N; o++){ c->off[o] = o == PL_O_AUX || o == PL_O_DIG ? 0 : -PL_VOL_MAX; c->cap[o] = PL_VOL_MAX; c->fade[o] = 1; }
    pl_core_recalc(c);
    c->app_aux = c->vol_aux; c->app_bt = c->vol_bt; c->app_dig = c->vol_dig; c->app_usb = c->vol_usb;
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
static int clampo(int v){ return v < -PL_VOL_MAX ? -PL_VOL_MAX : v > PL_VOL_MAX ? PL_VOL_MAX : v; }
static int clampc(int v){ return v < 0 ? 0 : v > PL_VOL_MAX ? PL_VOL_MAX : v; }
static const char *const ONAME[PL_O_N] = { "AUX", "Bluetooth", "digital", "USB" };

/* ---- the device table (core.h PL_DEV_MAX) ---- */
static pl_dev_t *dev_find(pl_core_t *c, const char *key){
    for(int i = 0; i < PL_DEV_MAX; i++) if(c->devs[i].key[0] && !strcmp(c->devs[i].key, key)) return &c->devs[i];
    return NULL;
}
/* the entry for key, made when missing (a new device: silent, fade on); the oldest goes when the table is full */
static pl_dev_t *dev_get(pl_core_t *c, const char *key){
    pl_dev_t *d = dev_find(c, key);
    if(!d){
        int k = 0;
        for(int i = 0; i < PL_DEV_MAX; i++){
            if(!c->devs[i].key[0]){ k = i; break; }
            if(c->devs[i].used < c->devs[k].used) k = i;
        }
        d = &c->devs[k];
        if(d->key[0]) logf_(c, "volumes: device table full, %s forgotten", d->key);
        memset(d, 0, sizeof *d);
        snprintf(d->key, sizeof d->key, "%s", key);
        /* AUX analog and digital are the Disc's own: they start at the main volume; anything else starts silent */
        d->off = !strcmp(key, "aux") || !strcmp(key, "dig") ? 0 : -PL_VOL_MAX; d->cap = PL_VOL_MAX; d->fade = 1;
        c->vol_seq++;
    }
    d->used = ++c->dev_clock;
    return d;
}
/* the stored entry behind output o; NULL for Bluetooth without headphones and for a USB card without a serial number
 * (its values live in c->off[] etc. only, until it is pulled) */
static pl_dev_t *out_dev(pl_core_t *c, int o){
    char k[120];
    switch(o){
    case PL_O_AUX: return dev_get(c, "aux");
    case PL_O_DIG: return dev_get(c, "dig");
    case PL_O_BT: if(!c->bt_mac[0]) return NULL; snprintf(k, sizeof k, "bt:%s", c->bt_mac); return dev_get(c, k);
    default: return c->usb_ready && c->usb_stored ? dev_get(c, c->usb_key) : NULL;
    }
}
/* output o takes its device's offset, cap and fade */
static void out_load(pl_core_t *c, int o){
    pl_dev_t *d = out_dev(c, o);
    if(d){ c->off[o] = d->off; c->cap[o] = d->cap; c->fade[o] = d->fade; }
}
/* Bluetooth: the headphones in bt_mac take their own values; the first ones after a volumes file from before
 * offsets take its Bluetooth volume (bt_legacy), any other new ones start silent */
static void bt_load(pl_core_t *c){
    char k[120];
    if(!c->bt_mac[0]) return;
    snprintf(k, sizeof k, "bt:%s", c->bt_mac);
    if(!dev_find(c, k) && c->bt_legacy > 0){
        pl_dev_t *d = dev_get(c, k);
        d->off = clampo(c->bt_legacy - PL_VOL_MAX - 1);
        c->bt_legacy = 0; c->vol_seq++;
        logf_(c, "volumes: %s takes the Bluetooth volume from before (offset %+d)", k, d->off);
    }
    out_load(c, PL_O_BT);
    logf_(c, "volumes: %s: offset %+d, cap %d", k, c->off[PL_O_BT], c->cap[PL_O_BT]);
}
/* output o's offset, cap and fade go back to its device */
static void out_store(pl_core_t *c, int o){
    pl_dev_t *d = out_dev(c, o);
    if(d && (d->off != c->off[o] || d->cap != c->cap[o] || d->fade != c->fade[o])){
        d->off = c->off[o]; d->cap = c->cap[o]; d->fade = c->fade[o]; c->vol_seq++;
    }
}

static int eff(const pl_core_t *c, int o){
    int lim = c->cap[o] < c->max_volume ? c->cap[o] : c->max_volume;
    int v = c->volume + c->off[o];
    return v < 0 ? 0 : v > lim ? lim : v;
}
void pl_core_recalc(pl_core_t *c){
    c->vol_aux = eff(c, PL_O_AUX); c->vol_bt = eff(c, PL_O_BT); c->vol_dig = eff(c, PL_O_DIG); c->vol_usb = eff(c, PL_O_USB);
}

void pl_core_set_volume(pl_core_t *c, int v){
    v = clampv(c, v);
    if(v != c->volume) c->vol_seq++;
    c->volume = v;
    pl_core_recalc(c);
    logf_(c, "volume = %d (AUX %d%s, digital %d%s, Bluetooth %d%s, USB %d%s)", c->volume, c->vol_aux, c->out_aux ? "" : " off",
          c->vol_dig, c->out_dig ? "" : " off", c->vol_bt, c->out_bt ? "" : " off", c->vol_usb, c->out_usb ? "" : " off");
    send_volume(c);
}

/* output o plays at v from now on (within its cap): its offset to the main volume becomes v - main */
static void set_out_volume(pl_core_t *c, int o, int v){
    c->off[o] = clampo(clampv(c, v) - c->volume);
    out_store(c, o);
    pl_core_recalc(c);
}

void pl_core_set_bt_volume(pl_core_t *c, int v){
    v = clampv(c, v);
    if(v == c->vol_bt) return;
    if(c->hp_vol_all){                                       /* 0d06 1: the main volume moves, every output follows */
        int m = v - c->off[PL_O_BT];
        /* main can't follow below 0 or above max: there Bluetooth's offset takes the rest, so the headphones get v */
        if(m < 0 || m > c->max_volume){ m = m < 0 ? 0 : c->max_volume; c->off[PL_O_BT] = clampo(v - m); out_store(c, PL_O_BT); }
        pl_core_set_volume(c, m);
        logf_(c, "volume: from the headphones: main %d, Bluetooth %d (offset %+d)", c->volume, c->vol_bt, c->off[PL_O_BT]);
        return;
    }
    set_out_volume(c, PL_O_BT, v);
    logf_(c, "volume: Bluetooth %d (from the headphones; offset %+d)", c->vol_bt, c->off[PL_O_BT]);
}

/* the volumes file: "main=N" and "dev KEY off=N cap=N fade=N"; from before offsets "vol_aux=N vol_bt=N vol_dig=N" */
int pl_core_vol_parse(pl_core_t *c, const char *t){
    int main_v = -1, va = -1, vb = -1, vd = -1, seen = 0;
    for(const char *l = t; l && *l; ){
        const char *e = strchr(l, '\n');
        size_t n = e ? (size_t)(e - l) : strlen(l);
        char line[200];
        if(n >= sizeof line) n = sizeof line - 1;
        memcpy(line, l, n); line[n] = 0;
        l = e ? e + 1 : NULL;
        if(!strncmp(line, "main=", 5)){ main_v = atoi(line + 5); seen = 1; }
        else if(!strncmp(line, "vol_aux=", 8)){ va = atoi(line + 8); seen = 1; }
        else if(!strncmp(line, "vol_bt=", 7)) vb = atoi(line + 7);
        else if(!strncmp(line, "vol_dig=", 8)) vd = atoi(line + 8);
        else if(!strncmp(line, "bt_legacy=", 10)) c->bt_legacy = atoi(line + 10);
        else if(!strncmp(line, "dev ", 4)){
            char key[100]; int off = -PL_VOL_MAX, cap = PL_VOL_MAX, fade = 1;
            char *p = line + 4, *sp = strchr(p, ' ');
            if(!sp || sp == p || (size_t)(sp - p) >= sizeof key) continue;
            memcpy(key, p, (size_t)(sp - p)); key[sp - p] = 0;
            for(char *q = sp; q && *q; q = strchr(q + 1, ' ')){
                if(!strncmp(q, " off=", 5)) off = atoi(q + 5);
                else if(!strncmp(q, " cap=", 5)) cap = atoi(q + 5);
                else if(!strncmp(q, " fade=", 6)) fade = atoi(q + 6) ? 1 : 0;
            }
            pl_dev_t *d = dev_get(c, key);
            d->off = clampo(off); d->cap = clampc(cap); d->fade = fade;
            seen = 1;
        }
    }
    if(!seen) return -1;
    if(main_v < 0 && va >= 0){                               /* from before offsets: AUX's volume is the main one */
        main_v = va;
        pl_dev_t *a = dev_get(c, "aux"), *d = dev_get(c, "dig");
        a->off = 0; d->off = vd >= 0 ? clampo(vd - va) : 0;
        if(vb >= 0) c->bt_legacy = clampo(vb - va) + PL_VOL_MAX + 1;   /* shifted: > 0 means there is one */
        logf_(c, "volumes: from the old file: main %d, digital %+d%s", main_v, d->off, vb >= 0 ? ", Bluetooth kept for the first headphones" : "");
    }
    if(main_v >= 0) c->volume = clampv(c, main_v);
    for(int o = 0; o < PL_O_N; o++) out_load(c, o);
    pl_core_recalc(c);
    send_volume(c);
    return 0;
}
int pl_core_vol_format(const pl_core_t *c, char *buf, unsigned cap){
    int n = snprintf(buf, cap, "main=%d\n", c->volume);
    if(c->bt_legacy > 0 && n >= 0 && (unsigned)n < cap) n += snprintf(buf + n, cap - (unsigned)n, "bt_legacy=%d\n", c->bt_legacy);
    for(int i = 0; i < PL_DEV_MAX && n >= 0 && (unsigned)n < cap; i++){
        const pl_dev_t *d = &c->devs[i];
        if(d->key[0]) n += snprintf(buf + n, cap - (unsigned)n, "dev %s off=%d cap=%d fade=%d\n", d->key, d->off, d->cap, d->fade);
    }
    return n >= 0 && (unsigned)n < cap ? 0 : -1;
}

void pl_core_volume_step(pl_core_t *c, int dir){
    int v = c->volume + (dir > 0 ? 1 : -1);
    if(v < 0 || v > c->max_volume){ logf_(c, "volume key: already at %d", c->volume); return; }
    pl_core_set_volume(c, v);
}
/* switch the outputs; why: for the log */
static void set_outputs4(pl_core_t *c, int aux, int bt, int dig, int usb, const char *why){
    if(c->out_aux == aux && c->out_bt == bt && c->out_dig == dig && c->out_usb == usb) return;
    c->out_aux = aux; c->out_bt = bt; c->out_dig = dig; c->out_usb = usb; c->route_seq++;
    if(aux || dig){ c->mem_aux = aux; c->mem_dig = dig; }   /* what a plug going in brings back */
    logf_(c, "outputs: AUX %s, digital %s, Bluetooth %s, USB %s (%s)", aux ? "on" : "off", dig ? "on" : "off", bt ? "on" : "off",
          usb ? "on" : "off", why);
}
/* AUX analog, Bluetooth, AUX digital; USB stays as it is */
static void set_outputs(pl_core_t *c, int aux, int bt, int dig, const char *why){ set_outputs4(c, aux, bt, dig, c->out_usb, why); }

int pl_core_plugged(const pl_core_t *c){ return c->jack == 1 || c->bal == 1; }
/* a plug state is known for at least one jack */
static int plug_known(const pl_core_t *c){ return c->jack >= 0 || c->bal >= 0; }
static void notice(pl_core_t *c, int what){
    c->notice = what; c->notice_seq++;
    if(what == PL_NOTICE_NONE) logf_(c, "outputs: play asked, but no output is on - notice to the UI");
    else if(what == PL_NOTICE_USB) logf_(c, "outputs: USB sound card %s connected, USB audio is off - notice to the UI", c->usb_name);
    else logf_(c, "outputs: %s connected, but %s is off (auto outputs off) - notice to the UI",
               what == PL_NOTICE_AUX ? "a plug" : "headphones", what == PL_NOTICE_AUX ? "AUX" : "Bluetooth");
}
void pl_core_notice(pl_core_t *c, int what){ notice(c, what); }
static void pause_(pl_core_t *c){ if(c->transport) c->transport(c->ctx, PL_T_PAUSE, 0); }
static const char *const AUTO_NAME[] = { "off", "newest", "all" };

static void plug_changed(pl_core_t *c, int was_plugged, const char *which, int plugged){
    int now = pl_core_plugged(c), bt_live = c->out_bt && c->bt_ready, jack_out = c->out_aux || c->out_dig;
    if(plugged){
        logf_(c, "jack: %s in%s", which, was_plugged ? "" : " (playback stays as it is)");
        if(was_plugged) return;
        /* the last AUX choice comes back: analog, digital or both (the Disc can't tell what the plug is) */
        if(c->auto_out == PL_AUTO_NEWEST) set_outputs4(c, c->mem_aux, 0, c->mem_dig, 0, "plug in");
        else if(c->auto_out == PL_AUTO_ALL) set_outputs(c, c->mem_aux, c->out_bt, c->mem_dig, "plug in");
        else if(!c->out_aux && !c->out_dig) notice(c, PL_NOTICE_AUX);
        return;
    }
    if(now){ logf_(c, "jack: %s out, the other plug is still in", which); return; }
    if(c->out_usb && c->usb_ready){                    /* a USB sound card plays: it goes on, AUX goes off unless auto off */
        if(jack_out && c->auto_out != PL_AUTO_OFF) set_outputs(c, 0, c->out_bt, 0, "plug out, USB audio plays on");
        else logf_(c, "jack: %s out, USB audio plays on", which);
        return;
    }
    /* both AUX outputs are on the jack: analog and digital are treated alike */
    if(c->bt_ready && c->auto_out != PL_AUTO_OFF){     /* AUX off; the connected headphones are what is left */
        if(bt_live) logf_(c, "jack: %s out (Bluetooth plays on)", which);
        else { logf_(c, "jack: %s out - pause, Bluetooth takes over", which); pause_(c); }
        set_outputs(c, 0, 1, 0, "plug out");
        return;
    }
    if(bt_live && jack_out){ logf_(c, "jack: %s out, Bluetooth plays on (auto outputs off: AUX stays on)", which); return; }
    if(bt_live){ logf_(c, "jack: %s out, AUX is off", which); return; }
    if(!jack_out && !c->out_bt){ logf_(c, "jack: %s out, no output is on", which); return; }
    /* AUX plays (analog, digital, or the DAC standing in while Bluetooth waits for the headphones) */
    logf_(c, "jack: %s out - pause", which);
    pause_(c);
}

/* the headphones are gone: the UI routed away from them or their link went (0666 6) */
static void bt_gone(pl_core_t *c, const char *why){
    int was = c->bt_ready;
    c->bt_ready = 0; c->bt_auto = 0;
    if(!c->out_bt){ if(was) logf_(c, "bluetooth: headphones gone (%s), Bluetooth was off", why); return; }
    int aux_on = c->out_aux || c->out_dig || (c->out_usb && c->usb_ready);
    if(aux_on && c->auto_out != PL_AUTO_OFF){ set_outputs(c, c->out_aux, 0, c->out_dig, why); return; }   /* AUX / USB play on */
    if(aux_on){ logf_(c, "bluetooth: headphones gone (%s), AUX plays on (auto outputs off: Bluetooth stays on)", why); return; }
    /* Bluetooth was the only output: pause (no sudden sound elsewhere). A plug is in: AUX takes over (auto on). Else
     * the choice waits for the headphones and the DAC plays meanwhile, so the UI's play goes on (playback.c pb_route). */
    logf_(c, "bluetooth: headphones gone (%s) - pause", why);
    pause_(c);
    if(c->auto_out != PL_AUTO_OFF && pl_core_plugged(c)) set_outputs(c, c->mem_aux, 0, c->mem_dig, "headphones gone, a plug is in");
    else logf_(c, "outputs: Bluetooth waits for the headphones, the DAC plays meanwhile");
}

/* 0d05 to all: every connected device on (a plug: AUX, connected headphones: Bluetooth), nothing else */
static void auto_all_now(pl_core_t *c){
    int in = plug_known(c) ? pl_core_plugged(c) : c->out_aux || c->out_dig, bt = c->bt_ready ? 1 : c->out_bt;
    /* a USB card stays as it is: one that was never switched on (new) must not come on by itself */
    if(!in && !bt) return;                           /* nothing connected: the choice stays */
    set_outputs(c, in ? c->mem_aux : 0, bt, in ? c->mem_dig : 0, "auto outputs: all connected");
}

void pl_core_usb(pl_core_t *c, int present, const char *key, int stored, const char *name){
    if(present){
        int known = stored && dev_find(c, key) != NULL;
        if(c->usb_ready && !strcmp(c->usb_key, key)) return;
        c->usb_ready = 1; c->usb_stored = stored;
        snprintf(c->usb_key, sizeof c->usb_key, "%s", key);
        snprintf(c->usb_name, sizeof c->usb_name, "%s", name && *name ? name : key);
        /* new (or no serial): silent and off; known: its own offset, cap, fade */
        c->off[PL_O_USB] = -PL_VOL_MAX; c->cap[PL_O_USB] = PL_VOL_MAX; c->fade[PL_O_USB] = 1;
        if(stored) out_load(c, PL_O_USB);            /* makes the entry for a new one: stored from now on */
        pl_core_recalc(c);
        logf_(c, "usb audio: %s (%s) connected, %s: offset %+d, cap %d", c->usb_name, key,
              known ? "known" : stored ? "new" : "no serial number, treated as new", c->off[PL_O_USB], c->cap[PL_O_USB]);
        c->route_seq++;
        if(!known){ if(c->out_usb) set_outputs4(c, c->out_aux, c->out_bt, c->out_dig, 0, "new USB sound card"); notice(c, PL_NOTICE_USB); return; }
        if(c->auto_out == PL_AUTO_NEWEST) set_outputs4(c, 0, 0, 0, 1, "known USB sound card connected");
        else if(c->auto_out == PL_AUTO_ALL) set_outputs4(c, c->out_aux, c->out_bt, c->out_dig, 1, "known USB sound card connected");
        else if(!c->out_usb) notice(c, PL_NOTICE_USB);
        return;
    }
    if(!c->usb_ready) return;
    logf_(c, "usb audio: %s gone", c->usb_name);
    c->usb_ready = 0; c->usb_stored = 0; c->route_seq++;
    if(!c->out_usb) return;
    int other = c->out_aux || c->out_dig || (c->out_bt && c->bt_ready);
    if(other){ set_outputs4(c, c->out_aux, c->out_bt, c->out_dig, 0, "USB sound card gone, the others play on"); return; }
    logf_(c, "usb audio: it was the only output - pause");
    pause_(c);
    if(c->auto_out != PL_AUTO_OFF && pl_core_plugged(c)) set_outputs4(c, c->mem_aux, 0, c->mem_dig, 0, "USB sound card gone, a plug is in");
    else if(c->auto_out != PL_AUTO_OFF && c->bt_ready) set_outputs4(c, 0, 1, 0, 0, "USB sound card gone, headphones are connected");
    else set_outputs4(c, c->out_aux, c->out_bt, c->out_dig, 0, "USB sound card gone");
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
    case 0x0104:
        if(v < 0 || v > 1){ logf_(c, "favourite %d: unknown, ignored", v); return -1; }
        logf_(c, "favourite %d%s", v, c->transport ? "" : " - no engine");
        if(c->transport && c->transport(c->ctx, PL_T_LOVE, v) != 0) logf_(c, "favourite %d: not done", v);
        return 0;
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
        if(c->volume > c->max_volume){ c->volume = c->max_volume; send_volume(c); }
        pl_core_recalc(c);
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
        if(v != PL_ROUTE_DAC && v != PL_ROUTE_BT && v != PL_ROUTE_SPDIF){ logf_(c, "output route %d: not supported by the own player yet, ignored", v); return 0; }
        if(v == PL_ROUTE_SPDIF){   /* the stock form: AUX digital instead of analog; Bluetooth and the route stay */
            set_outputs(c, 0, c->out_bt, 1, "output route 4 (S/PDIF)");
            return 0;
        }
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
        if(new_mac){ bt_load(c); pl_core_recalc(c); }
        logf_(c, "bluetooth codec %d for %s", codec, c->bt_mac);
        if(c->route == PL_ROUTE_BT){
            c->bt_ready = 1;
            if(c->bt_auto || new_mac){
                c->bt_auto = 0;
                if(c->auto_out == PL_AUTO_NEWEST) set_outputs4(c, 0, 1, 0, 0, "headphones connected");
                else if(c->auto_out == PL_AUTO_ALL)
                    set_outputs(c, plug_known(c) ? pl_core_plugged(c) && c->mem_aux : c->out_aux, 1,
                                plug_known(c) ? pl_core_plugged(c) && c->mem_dig : c->out_dig, "headphones connected");
                else if(!c->out_bt) notice(c, PL_NOTICE_BT);
                else logf_(c, "outputs: headphones connected, Bluetooth plays (auto outputs off)");
            }
        }
        return 0;
    }

    /* Outputs (diskOS; only the own player gets these, the UI sends them only when it runs) */
    case 0x0d01: {
        int aux = v >= 0 && (v & PL_OUT_AUX), bt = v >= 0 && (v & PL_OUT_BT), dig = v >= 0 && (v & PL_OUT_DIG), usb = v >= 0 && (v & PL_OUT_USB);
        if(v < 0 || v > 15){ c->bad_frames++; logf_(c, "outputs: bad value %d", v); return -1; }
        if(bt && !c->out_bt && !c->bt_ready){ logf_(c, "outputs: Bluetooth asked, but no headphones are connected - ignored"); return -1; }
        if(usb && !c->out_usb && !c->usb_ready){ logf_(c, "outputs: USB audio asked, but no USB sound card is there - ignored"); return -1; }
        set_outputs4(c, aux, bt, dig, usb, "by hand");    /* none at all is fine: playback waits (engine.c eng_output) */
        return 0;
    }
    case 0x0d02: case 0x0d03: case 0x0d07: case 0x0d08: {   /* an output's volume: stored as its offset to main */
        int o = f.tag == 0x0d02 ? PL_O_AUX : f.tag == 0x0d03 ? PL_O_BT : f.tag == 0x0d07 ? PL_O_DIG : PL_O_USB;
        if(v < 0 || v > PL_VOL_MAX){ logf_(c, "%s volume: value %d out of range, ignored", ONAME[o], v); return -1; }
        set_out_volume(c, o, v);
        logf_(c, "%s volume = %d (offset %+d to main %d, cap %d)", ONAME[o], eff(c, o), c->off[o], c->volume, c->cap[o]);
        return 0;
    }
    case 0x0d09: case 0x0d0a: {   /* an output's cap (0..120) / fade in (0/1): output index (high byte) and value */
        int o = v >> 8, x = v & 0xff, cap = f.tag == 0x0d09;
        if(v < 0 || o >= PL_O_N || (cap ? x > PL_VOL_MAX : x > 1)){ c->bad_frames++; logf_(c, "%s: bad value %d", cap ? "cap" : "fade", v); return -1; }
        if(cap) c->cap[o] = x; else c->fade[o] = x;
        out_store(c, o);
        pl_core_recalc(c);
        logf_(c, "%s %s = %d", ONAME[o], cap ? "cap" : "fade in", x);
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
        if(v == 5){ logf_(c, "usb gadget 5 (USB host): the own player switches the host role by itself (usbaudio.c)"); return 0; }
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
