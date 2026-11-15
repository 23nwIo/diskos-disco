/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "playback.h"
#include "dac.h"
#include "proto.h"
#include "queue.h"
#include "queuedb.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pl_core_t *C;
static pb_log_fn L;
static char g_song_db[256], g_sys_db[256];
static pq_t Q = { 0, NULL, NULL, -1, 0 };
static int g_next = -1;            /* order index queued in MPD as the next song, -1 none */
static eng_status_t g_es;
static double g_es_t, g_last_a1;
static int g_es_valid;
static int g_told = -1;            /* the state the UI was last told: ENG_PLAY or ENG_PAUSE, -1 nothing yet */
/* a track start to announce (the full a2), once MPD knows the stream format: 0 none, 1 from a play command (0100,
 * Next, Previous), 2 a natural track change */
static int g_announce;
static double g_announce_t;

static double now_s(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void send_(const char *f, int n){ if(C && C->send && n > 0) C->send(C->ctx, f, n); }
static void send_lit(const char *f){ send_(f, (int)strlen(f)); }
static void tell_state(int state, int force){
    int s = state == ENG_PLAY ? ENG_PLAY : ENG_PAUSE;
    if(!force && (s == g_told || (g_told < 0 && s != ENG_PLAY))) return;   /* nothing to say before the first play */
    /* the stock player sends the partial state frame twice (capture 2026-11-15) */
    char f[32]; int n = pp_fmt_a2(f, sizeof f, s == ENG_PLAY ? "{\"state\":0}" : "{\"state\":1}");
    send_(f, n); send_(f, n);
    g_told = s;
}
static unsigned rnd(unsigned k){ return k ? (unsigned)(random() % k) : 0; }

void pb_init(pl_core_t *c, const char *song_db, const char *sysconfig_db, pb_log_fn log){
    C = c; L = log;
    snprintf(g_song_db, sizeof g_song_db, "%s", song_db);
    snprintf(g_sys_db, sizeof g_sys_db, "%s", sysconfig_db);
    srandom((unsigned)time(NULL) ^ (unsigned)(now_s() * 1000.0));
    pq_free(&Q);
}
void pb_close(void){ pq_free(&Q); g_next = -1; }

static sqlite3 *db(void){
    sqlite3 *d = qd_open(g_song_db);
    if(!d) L("queue: can't open %s", g_song_db);
    return d;
}

/* the Artist grouping for types 2 and 7: the UI predicts the queue from SYSCONFIG.ARTIST_CLASS_TYPE, which the stock
 * player keeps in step with 0648; do the same, so both sides use one value */
static int artist_class(void){
    int cls = qd_artist_class(g_sys_db);
    if(C->artist_grouping >= 0 && C->artist_grouping != cls){
        if(qd_sys_set(g_sys_db, "ARTIST_CLASS_TYPE", C->artist_grouping) != 0) L("queue: can't store the Artist grouping in %s", g_sys_db);
        cls = C->artist_grouping;
    }
    return cls < 0 ? 0 : cls;
}

/* A row MPD can play as a whole file. CUE and ISO tracks (a part of a bigger file) are not supported yet. */
static int playable(sqlite3 *d, int idx, qd_row_t *r){
    int id = pq_id(&Q, idx);
    int x = qd_row(d, id, r);
    if(x != 1){ L("queue: row %d: %s", id, x == 0 ? "gone" : sqlite3_errmsg(d)); return 0; }
    if(r->is_cue || r->is_iso){ L("queue: row %d is a %s track, not supported yet - skipped", id, r->is_cue ? "CUE" : "ISO"); return 0; }
    return 1;
}

/* Queue the song that follows the current one when it ends (by the play mode); skips rows MPD can't take. */
static void queue_next(sqlite3 *d){
    int mode = C->play_mode, save = Q.cur;
    int ni = pq_next(&Q, mode, 1);
    qd_row_t *r = malloc(sizeof *r);
    g_next = -1;
    for(int tries = 0; r && ni >= 0 && tries < Q.n; tries++){
        if(playable(d, ni, r) && eng_set_next(r->path) == 0){ g_next = ni; break; }
        Q.cur = ni;                               /* look past it; repeat one moves on like repeat all */
        ni = pq_next(&Q, mode == PQ_REPEAT_ONE ? PQ_REPEAT_ALL : mode, 1);
        Q.cur = save;
        if(ni == save && mode != PQ_REPEAT_ONE) ni = -1;
    }
    Q.cur = save;
    free(r);
    if(g_next < 0) eng_set_next(NULL);
}

/* Start order index idx (dir +1/-1: where to look when it can't be played). 0 or -1. */
static int start_at(sqlite3 *d, int idx, int dir, long ms, int paused){
    qd_row_t *r = malloc(sizeof *r);
    if(!r) return -1;
    int ok = -1;
    for(int tries = 0; idx >= 0 && tries < Q.n; tries++){
        if(playable(d, idx, r) && eng_load(r->path, ms, paused) == 0){ ok = 0; break; }
        idx = (idx + dir + Q.n) % Q.n;
        ms = 0;
    }
    free(r);
    if(ok){ L("queue: nothing in the queue can be played"); return -1; }
    Q.cur = idx;
    queue_next(d);
    g_announce = 1; g_announce_t = now_s();
    return 0;
}

/* A growing JSON text; ok drops to 0 when something did not fit (the frame is then not sent). */
typedef struct { char *b; size_t len, cap; int ok; } jbuf_t;
static void jraw(jbuf_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jraw(jbuf_t *j, const char *fmt, ...){
    if(!j->ok) return;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(j->b + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if(w < 0 || j->len + (size_t)w >= j->cap){ j->ok = 0; return; }
    j->len += (size_t)w;
}
static void jstr(jbuf_t *j, const char *s){
    if(!j->ok) return;
    int w = pp_json_escape(j->b, j->cap, j->len, s);
    if(w < 0){ j->ok = 0; return; }
    j->len = (size_t)w;
}

/* the full a2 frame for the current song (docs/PLAYER_PROTOCOL.md: the fields in the stock order) */
static void send_full_a2(sqlite3 *d, const eng_status_t *s, int state){
    qd_row_t *r = malloc(sizeof *r);
    char *song = malloc(PP_MAX_FRAME), *json = malloc(PP_MAX_FRAME), *frame = malloc(PP_MAX_FRAME);
    if(!r || !song || !json || !frame){ L("queue: out of memory for the track frame"); goto out; }
    if(qd_row(d, pq_id(&Q, Q.cur), r) != 1){ L("queue: current row gone, no track frame"); goto out; }
    unsigned rate = s->rate; int bits = s->bits ? s->bits : 16, ch = s->channels ? s->channels : 2;
    long dur = r->duration_ms > 0 ? (long)r->duration_ms : s->duration_ms;
    jbuf_t j = { song, 0, PP_MAX_FRAME, 1 };
    song[0] = 0;
    jraw(&j, "{\"id\":%d,\"pos_id\":%d,\"song_name\":\"", r->song_id, r->id);
    jstr(&j, r->title[0] ? r->title : r->path);
    jraw(&j, "\",\"song_duration_time\":%ld,\"song_file_path\":\"", dur);
    jstr(&j, r->path);
    /* song_bit_rate: the stock player reports the PCM rate (1411 for 44.1 kHz/16 bit stereo, also for an MP3) */
    jraw(&j, "\",\"song_sample_rate\":%u,\"song_encoding_rate\":%d,\"song_bit_rate\":%u,\"song_artist_name\":\"",
         rate, bits, (unsigned)((unsigned long)rate * (unsigned)bits * (unsigned)ch / 1000u));
    jstr(&j, r->artist);
    jraw(&j, "\",\"song_album_name\":\"");
    jstr(&j, r->album);
    jraw(&j, "\",\"song_style_name\":\"");
    jstr(&j, r->genre[0] ? r->genre : "(null)");
    jraw(&j, "\",\"song_track\":%d,\"song_channel\":%d,\"is_sacd\":%s,\"is_cue\":%s,\"is_dsd\":false,\"is_m3u\":%s,"
         "\"m3u_file_path\":\"", r->track, ch, r->is_iso ? "true" : "false", r->is_cue ? "true" : "false",
         r->is_m3u ? "true" : "false");
    jstr(&j, r->m3u_path[0] ? r->m3u_path : "(null)");
    jraw(&j, "\"}");
    jbuf_t o = { json, 0, PP_MAX_FRAME, j.ok };
    json[0] = 0;
    jraw(&o, "{\"song\":\"");
    jstr(&o, song);                        /* the song JSON as a string, as the stock player sends it */
    jraw(&o, "\",\"love\":%s,\"playerflag\":3,\"state\":%d,\"playing_num\":\"%d/%d\",\"work_mode\":\"LOCAL\"}",
         r->love ? "true" : "false", state, pq_number(&Q), Q.n);
    int n = o.ok ? pp_fmt_a2(frame, PP_MAX_FRAME, json) : -1;
    if(n < 0){ L("queue: track frame too long for %s", r->path); goto out; }
    send_(frame, n);
    L("track %d/%d: %s (pos_id %d, song %d)", pq_number(&Q), Q.n, r->path, r->id, r->song_id);
out:
    free(r); free(song); free(json); free(frame);
}

/* The stock sequence around a track start (protocol capture 2026-11-15). */
static void announce(const eng_status_t *s){
    sqlite3 *d = db();
    if(!d) return;
    char f[16];
    if(g_announce == 1){ snprintf(f, sizeof f, "a102000C%04X", C->play_mode & 0xffff); send_lit(f); }
    else tell_state(ENG_PAUSE, 1);
    send_lit("aa1c000C0000");
    send_full_a2(d, s, 2);                 /* 2 = a new track is being loaded */
    send_lit("aa05000C0001");
    send_lit("aa1c000C0000");
    tell_state(s->state, 1);
    sqlite3_close(d);
    g_announce = 0;
}

static void update_status(void){
    eng_status_t s;
    if(eng_status(&s) != 0) return;
    if(s.state == ENG_PLAY && s.rate && s.rate != dac_current_rate()){
        unsigned char a, l; dac_attenuation(C->volume, C->gain, C->balance, &a, &l);
        dac_start(s.rate, a, l, L);
    }
    int was = g_es_valid ? g_es.state : ENG_STOP;
    if(s.state != was || (g_es_valid && strcmp(s.file, g_es.file)))
        L("engine: %s, %ld/%ld ms, %u Hz, %s", s.state == ENG_PLAY ? "play" : s.state == ENG_PAUSE ? "pause" : "stop",
          s.elapsed_ms, s.duration_ms, s.rate, s.file);
    g_es = s; g_es_t = now_s(); g_es_valid = 1;
}

void pb_engine_event(void){
    update_status();
    if(!g_es_valid) return;
    if(eng_advanced(&g_es)){                          /* the queued next song has started */
        Q.cur = g_next;
        sqlite3 *d = db();
        if(d){ queue_next(d); sqlite3_close(d); }
        g_announce = 2; g_announce_t = now_s();
    }
    if(g_announce){
        /* wait for the stream format (song_sample_rate), at most a second (pb_tick) */
        if(g_es.state != ENG_PLAY || g_es.rate) announce(&g_es);
        return;
    }
    tell_state(g_es.state, 0);
}

void pb_tick(double t){
    if(g_announce && t - g_announce_t >= 1.0){ update_status(); if(g_es_valid) announce(&g_es); }
    if(g_es_valid && g_es.state == ENG_PLAY && t - g_last_a1 >= 1.0){   /* a1 once a second while playing */
        g_last_a1 = t;
        long ms = g_es.elapsed_ms + (long)((t - g_es_t) * 1000.0);
        if(g_es.duration_ms > 0 && ms > g_es.duration_ms) ms = g_es.duration_ms;
        char f[32]; send_(f, pp_fmt_a1(f, sizeof f, (unsigned long)ms));
    }
}

const eng_status_t *pb_status(double *when){
    if(!g_es_valid) return NULL;
    if(when) *when = g_es_t;
    return &g_es;
}

/* Load the queue the tables hold (after a restart of this player): LIST_SONG_0, and LIST_SONG_3 in shuffle. */
static int load_tables(sqlite3 *d, int pos){
    int *ids = NULL, n = qd_ids(d, &ids);
    if(n <= 0){ free(ids); L("queue: no queue in %s", g_song_db); return -1; }
    if(pq_set(&Q, ids, n, pos) != 0) return -1;
    if(C->play_mode == PQ_SHUFFLE){
        int *o = NULL, m = qd_shuffle_read(d, &o);
        if(m <= 0 || pq_adopt_order(&Q, o, m) != 0){
            if(m > 0) L("queue: LIST_SONG_3 is not a permutation of LIST_SONG_0, shuffled again");
            if(pq_shuffle(&Q, 1, rnd) == 0) qd_shuffle_write(d, Q.order, Q.n);
        }
    }
    L("queue: %d songs from the tables%s", Q.n, Q.shuffled ? " (shuffled)" : "");
    return 0;
}

int pb_play_list(int type, int pos, const char *name){
    if(!eng_ready()){ L("play list: no engine"); return -1; }
    sqlite3 *d = db();
    if(!d) return -1;
    int rc = -1;
    if(type == QD_RESUME){
        if(Q.n <= 0 && load_tables(d, pos) != 0) goto out;
        int idx = pq_from_base(&Q, pos);
        if(idx < 0){ L("play list: position %d outside the queue (%d songs)", pos, Q.n); goto out; }
        rc = start_at(d, idx, +1, 0, 0);
        goto out;
    }
    char err[200];
    double t0 = now_s();
    int n = qd_build(d, type, name, artist_class(), err, sizeof err);
    if(n < 0){ L("play list: type %d \"%s\": %s", type, name, err); goto out; }
    L("play list: type %d \"%s\": %d songs in %.0f ms", type, name, n, (now_s() - t0) * 1000.0);
    int *ids = NULL, m = qd_ids(d, &ids);
    if(m <= 0){ free(ids); pq_free(&Q); L("play list: the list is empty"); goto out; }
    if(pq_set(&Q, ids, m, pos) != 0){ L("play list: out of memory"); goto out; }
    if(C->play_mode == PQ_SHUFFLE){
        if(pq_shuffle(&Q, 1, rnd) != 0 || qd_shuffle_write(d, Q.order, Q.n) != 0) L("play list: shuffle order not written");
    }
    rc = start_at(d, Q.cur, +1, 0, 0);
out:
    sqlite3_close(d);
    return rc;
}

int pb_transport(int op, long arg){
    if(!eng_ready() && op != PL_T_MODE){ L("transport %d: no engine", op); return -1; }
    switch(op){
    case PL_T_TOGGLE:
        if(Q.n <= 0){ L("play/pause: nothing loaded"); return -1; }   /* resume from MEMORY_PLAY comes later */
        return eng_toggle();
    case PL_T_PAUSE: pb_pause(); return 0;
    case PL_T_MODE: pb_mode_changed(); return 0;
    case PL_T_STATE: {
        if(Q.n <= 0 || !g_es_valid || g_es.songid < 0) return -1;
        sqlite3 *d = db();
        if(!d) return -1;
        update_status();
        send_full_a2(d, &g_es, g_es.state == ENG_PLAY ? 0 : 1);
        sqlite3_close(d);
        return 0;
    }
    case PL_T_SEEK: {
        int r = eng_seek_ms(arg);
        if(r == 0){ update_status(); tell_state(g_es.state, 1); }   /* the stock player answers a seek with the state */
        return r;
    }
    case PL_T_NEXT: case PL_T_PREV: {
        if(Q.n <= 0){ L("next/previous: nothing loaded"); return -1; }
        int idx = op == PL_T_NEXT ? pq_next(&Q, C->play_mode, 0) : pq_prev(&Q);
        sqlite3 *d = db();
        if(!d) return -1;
        int r = start_at(d, idx, op == PL_T_NEXT ? +1 : -1, 0, 0);
        sqlite3_close(d);
        return r;
    }
    }
    return -1;
}

void pb_pause(void){
    if(g_es_valid && g_es.state == ENG_PLAY) eng_pause(1);
}

void pb_mode_changed(void){
    int on = C->play_mode == PQ_SHUFFLE;
    sqlite3 *d = db();
    if(!d) return;
    if(Q.n > 0 && on != Q.shuffled){
        if(pq_shuffle(&Q, on, rnd) != 0) L("play mode: out of memory");
        else if(on ? qd_shuffle_write(d, Q.order, Q.n) : qd_shuffle_drop(d)) L("play mode: LIST_SONG_3: %s", sqlite3_errmsg(d));
    } else if(!on) qd_shuffle_drop(d);
    if(Q.n > 0 && Q.cur >= 0) queue_next(d);
    sqlite3_close(d);
}
