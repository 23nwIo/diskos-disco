/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "playback.h"
#include "dac.h"
#include "mixer.h"
#include "proto.h"
#include "queue.h"
#include "queuedb.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

static pl_core_t *C;
static pb_log_fn L;
static char g_song_db[256], g_sys_db[256];
static pq_t Q = { 0, NULL, NULL, -1, 0 };
static int g_next = -1;            /* order index queued in MPD as the next song, -1 none */
static int g_pending = -1;         /* order index to start when the current song ends (another sample rate: the DAC
                                    * needs its start sequence, so no gapless change), -1 none */
static eng_status_t g_es;
static double g_es_t, g_last_a1;
static int g_es_valid;
static int g_told = -1;            /* the state the UI was last told: ENG_PLAY or ENG_PAUSE, -1 nothing yet */
/* a track start to announce (the full a2), once MPD knows the stream format: 0 none, 1 from a play command (0100,
 * Next, Previous), 2 a natural track change */
static int g_announce;
static double g_announce_t;

/* The resume bookmark (MEMORY_PLAY): the song in it is the one playing (g_mem_ok), its POSITION written every
 * PB_MEM_EVERY_S while playing, on pause and at power off. */
#define PB_MEM_EVERY_S 30.0
static int g_mem_ok;
static double g_mem_t;
/* Resume at start (pb_resume_begin): waiting for the card (g_rs 1), the card first seen mounted, the give-up time */
#define PB_RESUME_WAIT_S   30.0
#define PB_RESUME_STABLE_S 1.0
#define PB_RESUME_OK_S     10.0
static int g_rs;
static double g_rs_until, g_rs_mounted, g_rs_check;
static int (*g_rs_mounted_fn)(void);
static char g_rs_flag[256];
static double g_rs_flag_t;          /* the resumed song started (the flag goes once it has played PB_RESUME_OK_S), 0 none */

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
/* before MPD opens the PCM: the DAC start sequence for this rate (eng_set_prepare) */
static void prepare(unsigned rate){
    mix_route_dac(L);                        /* DMA3 -> DAC; without it hw_params fails (mixer.h) */
    if(rate == dac_current_rate()) return;
    unsigned char a, l; dac_attenuation(C->volume, C->gain, C->balance, &a, &l);
    dac_start(rate, a, l, L);
}
static unsigned rnd(unsigned k){ return k ? (unsigned)(random() % k) : 0; }

void pb_init(pl_core_t *c, const char *song_db, const char *sysconfig_db, pb_log_fn log){
    C = c; L = log;
    snprintf(g_song_db, sizeof g_song_db, "%s", song_db);
    snprintf(g_sys_db, sizeof g_sys_db, "%s", sysconfig_db);
    srandom((unsigned)time(NULL) ^ (unsigned)(now_s() * 1000.0));
    eng_set_prepare(prepare);
    pq_free(&Q);
    g_next = g_pending = -1; g_es_valid = 0; g_told = -1; g_announce = 0; g_mem_ok = 0; g_rs = 0; g_rs_flag_t = 0;
}
void pb_close(void){ pq_free(&Q); g_next = g_pending = -1; }

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

/* A row MPD can play: a file, or a CUE track (the part of the file from OFFSET for DURATION, both ms, as the scanner
 * writes them; ui/scanner.c "CUE sheets"). SACD ISO tracks are not supported: MPD has no decoder for disc images. */
static int playable(sqlite3 *d, int idx, qd_row_t *r){
    int id = pq_id(&Q, idx);
    int x = qd_row(d, id, r);
    if(x != 1){ L("queue: row %d: %s", id, x == 0 ? "gone" : sqlite3_errmsg(d)); return 0; }
    if(r->is_iso){ L("queue: row %d is a SACD ISO track, not supported - skipped", id); return 0; }
    return 1;
}
#define PART(r) ((r)->is_cue ? (long)(r)->offset : 0L), ((r)->is_cue ? (long)(r)->duration_ms : 0L)

/* Queue the song that follows the current one when it ends (by the play mode); skips rows MPD can't take. */
static void queue_next(sqlite3 *d){
    int mode = C->play_mode, save = Q.cur;
    int ni = pq_next(&Q, mode, 1);
    qd_row_t *r = malloc(sizeof *r);
    g_next = g_pending = -1;
    for(int tries = 0; r && ni >= 0 && tries < Q.n; tries++){
        int x = playable(d, ni, r) ? eng_set_next(r->path, PART(r), dac_current_rate()) : -1;
        if(x == 0){ g_next = ni; break; }
        if(x == 1){ g_pending = ni; L("queue: next song has another sample rate, started after this one (no gapless)"); break; }
        Q.cur = ni;                               /* look past it; repeat one moves on like repeat all */
        ni = pq_next(&Q, mode == PQ_REPEAT_ONE ? PQ_REPEAT_ALL : mode, 1);
        Q.cur = save;
        if(ni == save && mode != PQ_REPEAT_ONE) ni = -1;
    }
    Q.cur = save;
    free(r);
    if(g_next < 0) eng_set_next(NULL, 0, 0, 0);
}

/* Start order index idx (dir +1/-1: where to look when it can't be played). 0 or -1. */
static int start_at(sqlite3 *d, int idx, int dir, long ms, int paused){
    qd_row_t *r = malloc(sizeof *r);
    if(!r) return -1;
    int ok = -1;
    for(int tries = 0; idx >= 0 && tries < Q.n; tries++){
        if(playable(d, idx, r) && eng_load(r->path, PART(r), ms, paused) == 0){ ok = 0; break; }
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

/* The song that starts goes into the resume bookmark, with where it starts. Songs outside the library (no SONG.ID)
 * leave the bookmark as it is: MUSIC_ID has to name a SONG row (the UI reads it too, ui/musicdb.c mdb_current_play). */
static void remember_song(sqlite3 *d, const eng_status_t *s){
    qd_row_t *r = malloc(sizeof *r);
    g_mem_ok = 0;
    if(r && qd_row(d, pq_id(&Q, Q.cur), r) == 1){
        if(r->song_id <= 0) L("resume: %s is not in the library, not remembered", r->path);
        else if(qd_memory_set(d, r->song_id, s->elapsed_ms, r->is_cue, r->is_iso, r->track) != 0)
            L("resume: MEMORY_PLAY: %s", sqlite3_errmsg(d));
        else { g_mem_ok = 1; g_mem_t = now_s(); }
    }
    free(r);
}
static void remember_position(const char *why){
    if(!g_mem_ok || !g_es_valid) return;
    long ms = g_es.elapsed_ms;
    if(g_es.state == ENG_PLAY) ms += (long)((now_s() - g_es_t) * 1000.0);
    if(g_es.duration_ms > 0 && ms > g_es.duration_ms) ms = g_es.duration_ms;
    sqlite3 *d = db();
    if(!d) return;
    if(qd_memory_pos(d, ms) != 0) L("resume: position not stored (%s): %s", why, sqlite3_errmsg(d));
    sqlite3_close(d);
    g_mem_t = now_s();
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
    remember_song(d, s);
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
    int was = g_es_valid ? g_es.state : ENG_STOP;
    update_status();
    if(!g_es_valid) return;
    if(g_es.state == ENG_STOP && was == ENG_PLAY && g_pending >= 0){   /* the song ended; the next needs the DAC restarted */
        int p = g_pending;
        sqlite3 *d = db();
        if(d){ if(start_at(d, p, +1, 0, 0) == 0) g_announce = 2; sqlite3_close(d); }
        return;
    }
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
    if(was == ENG_PLAY && g_es.state != ENG_PLAY) remember_position("paused");
    tell_state(g_es.state, 0);
}

static void resume_tick(double t);
void pb_tick(double t){
    resume_tick(t);
    if(g_announce && t - g_announce_t >= 1.0){ update_status(); if(g_es_valid) announce(&g_es); }
    if(g_es_valid && g_es.state == ENG_PLAY && g_mem_ok && t - g_mem_t >= PB_MEM_EVERY_S) remember_position("playing");
    if(g_rs_flag_t > 0 && g_es_valid && g_es.state == ENG_PLAY && t - g_rs_flag_t >= PB_RESUME_OK_S){
        if(unlink(g_rs_flag) != 0) L("resume: unlink %s: %s", g_rs_flag, strerror(errno));
        g_rs_flag_t = 0;
    }
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

/* Load the queue the tables hold (after a restart of this player): LIST_SONG_0, and LIST_SONG_3 in shuffle. The current
 * song is base position pos, or the row with LIST_SONG_0.ID row when row > 0. */
static int load_tables(sqlite3 *d, int pos, int row){
    int *ids = NULL, n = qd_ids(d, &ids);
    if(n <= 0){ free(ids); L("queue: no queue in %s", g_song_db); return -1; }
    for(int i = 0; row > 0 && i < n; i++) if(ids[i] == row){ pos = i; break; }
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
        if(Q.n <= 0 && load_tables(d, pos, 0) != 0) goto out;
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

/* Resume at start, like the stock player's play_memory thread (docs/MQ_PLAYER_REPLACEMENT.md, "Resume"). */
void pb_resume_begin(int (*mounted)(void), const char *flag_path){
    g_rs_mounted_fn = mounted;
    snprintf(g_rs_flag, sizeof g_rs_flag, "%s", flag_path ? flag_path : "");
    g_rs = 1; g_rs_until = now_s() + PB_RESUME_WAIT_S; g_rs_mounted = g_rs_check = 0;
}

static void resume_now(void){
    sqlite3 *d = db();
    if(!d) return;
    int music_id = 0, track = 0; long pos = 0;
    int x = qd_memory_get(d, &music_id, &pos, &track);
    if(x < 0){ L("resume: MEMORY_PLAY: %s", sqlite3_errmsg(d)); goto out; }
    if(x == 0 || music_id <= 0){ L("resume: nothing remembered"); goto out; }
    L("resume: song %d at %ld ms (mode %d: %s)", music_id, pos, C->resume_mode, C->resume_mode == 1 ? "position" : "song");
    /* the last resume crashed or hung the player before its song had played a few seconds: don't loop into it again;
     * forget the bookmark as the stock player does (it drops MEMORY_PLAY) */
    if(g_rs_flag[0] && access(g_rs_flag, F_OK) == 0){
        L("resume: the last resume did not play %.0f s (%s is there) - skipped, bookmark cleared", PB_RESUME_OK_S, g_rs_flag);
        if(unlink(g_rs_flag) != 0) L("resume: unlink %s: %s", g_rs_flag, strerror(errno));
        if(qd_memory_set(d, 0, 0, 0, 0, 0) != 0) L("resume: MEMORY_PLAY: %s", sqlite3_errmsg(d));
        goto out;
    }
    int row = qd_list_row_of_song(d, music_id);
    if(row <= 0){ L("resume: song %d is not in the queue (LIST_SONG_0)%s", music_id, row < 0 ? sqlite3_errmsg(d) : ""); goto out; }
    if(load_tables(d, 0, row) != 0) goto out;
    int idx = pq_find(&Q, row);
    if(idx < 0){ L("resume: queue row %d not found", row); goto out; }
    if(g_rs_flag[0]){
        FILE *f = fopen(g_rs_flag, "w");
        if(!f) L("resume: create %s: %s", g_rs_flag, strerror(errno));
        else fclose(f);
    }
    if(start_at(d, idx, +1, C->resume_mode == 1 ? pos : 0, 0) == 0) g_rs_flag_t = now_s();
    else if(g_rs_flag[0]) unlink(g_rs_flag);
out:
    sqlite3_close(d);
}

static void resume_tick(double t){
    if(g_rs != 1 || t < g_rs_check) return;
    g_rs_check = t + 0.25;
    if(C->resume_mode == 0){ L("resume: off"); g_rs = 0; return; }
    if(Q.n > 0 || g_told >= 0){ L("resume: something plays already, no resume"); g_rs = 0; return; }
    if(t > g_rs_until){ L("resume: no card mounted within %.0f s, no resume", PB_RESUME_WAIT_S); g_rs = 0; return; }
    if(!eng_ready()) return;
    if(g_rs_mounted_fn && !g_rs_mounted_fn()){ g_rs_mounted = 0; return; }
    if(g_rs_mounted == 0){ g_rs_mounted = t; return; }
    if(t - g_rs_mounted < PB_RESUME_STABLE_S) return;     /* the stock player waits for "mount stable" too */
    g_rs = 0;
    resume_now();
}

void pb_shutdown(void){
    remember_position("power off");
    if(g_rs_flag[0] && g_rs_flag_t > 0) unlink(g_rs_flag);   /* an orderly end is no crash (the stock player does the same) */
    g_rs_flag_t = 0;
}

int pb_transport(int op, long arg){
    if(!eng_ready() && op != PL_T_MODE){ L("transport %d: no engine", op); return -1; }
    switch(op){
    case PL_T_TOGGLE:
        if(Q.n <= 0){ L("play/pause: nothing loaded"); return -1; }
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
