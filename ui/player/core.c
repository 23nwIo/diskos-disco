/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "core.h"
#include "proto.h"
#include <stdarg.h>
#include <stdio.h>
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
    c->gain = 0; c->dre = 0; c->work_mode = -1; c->route = -1;
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

void pl_core_volume_step(pl_core_t *c, int dir){
    int v = c->volume + (dir > 0 ? 1 : -1);
    if(v < 0) v = 0;
    if(v > c->max_volume) v = c->max_volume;
    if(v == c->volume){ logf_(c, "volume key: already at %d", v); return; }
    c->volume = v;
    logf_(c, "volume key: volume = %d", v);
    send_volume(c);
}

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
    case 0x0202: pl_core_send_state(c); return 0;

    /* transport and queue: parsed, not played yet */
    case 0x0100: {
        if(f.dlen < 8){ c->bad_frames++; logf_(c, "play list: frame too short"); return -1; }
        pp_frame_t pos = { 0, f.data, 4 }, type = { 0, f.data + 4, 4 };
        logf_(c, "play list: pos %d type %d name \"%.*s\" (playback not implemented yet)",
              pp_class1(&pos), pp_class1(&type), f.dlen - 8, f.data + 8);
        pl_core_send_state(c);
        return 0;
    }
    case 0x0103:
        if(f.dlen != 8){ c->bad_frames++; logf_(c, "seek: bad frame"); return -1; }
        logf_(c, "seek to %.*s ms (hex, playback not implemented yet)", 8, f.data);
        return 0;
    case 0x0201: logf_(c, "transport %d (0 play/pause, 1 next, 2 previous; playback not implemented yet)", v); return 0;
    case 0x0104: logf_(c, "favourite %d (not implemented yet)", v); return 0;
    case 0x0102: return set_(c, "play mode", &c->play_mode, v, 0, 4);
    case 0x0648: return set_(c, "artist grouping", &c->artist_grouping, v, 0, 1);

    /* audio settings */
    case 0x0715:
        if(v < 0 || v > PL_VOL_MAX){ logf_(c, "volume: value %d out of range, ignored", v); return -1; }
        c->volume = v > c->max_volume ? c->max_volume : v;
        logf_(c, "volume = %d", c->volume);
        send_volume(c);
        return 0;
    case 0x0711:
        if(set_(c, "max volume", &c->max_volume, v, 0, PL_VOL_MAX)) return -1;
        if(c->volume > c->max_volume){ c->volume = c->max_volume; send_volume(c); }
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
        if(v != PL_ROUTE_DAC){ logf_(c, "output route %d: not supported by the own player yet, ignored", v); return 0; }
        c->route = v; logf_(c, "output route = %d", v); return 0;

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
    case 0x0622: case 0x0642: case 0x06b3: case 0x06c1: case 0x0818:
        logf_(c, "%04x: not handled by the own player yet (%.*s)", f.tag, f.dlen > 48 ? 48 : f.dlen, f.data);
        return 0;
    }
    c->unknown_tags++;
    logf_(c, "unknown tag %04x (%d chars of data), ignored", f.tag, f.dlen);
    return -1;
}
