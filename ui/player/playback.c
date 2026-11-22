/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "playback.h"
#include "btout.h"
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
#include <signal.h>
#include <sys/wait.h>

static pl_core_t *C;
static pb_log_fn L;
static char g_song_db[256], g_sys_db[256];
static pq_t Q = { 0, NULL, NULL, -1, 0 };
static int g_next = -1;            /* order index queued in MPD as the next song, -1 none */
static int g_pending = -1;         /* order index to start when the current song ends (another sample rate: the DAC
                                    * needs its start sequence, so no gapless change), -1 none */
/* The UI's queue (0d13, Up next), in RAM only (Jo, 2026-11-22: no song.db writes for it): SONG.IDs that play before
 * the list goes on. g_cur_up: the one playing now (0 = the list's song Q.cur plays); g_next_up / g_pend_up: queued in
 * MPD as the next song / to start when this one ends (another sample rate), 0 none. Q.cur stays on the list's song
 * meanwhile, so the list goes on after them - unless g_up_party: then the list ends (0d11 decides what follows).
 * Every up-next song that starts (or is skipped as unplayable) leaves g_up and is told to the UI (ad13 with a running
 * number); 0d13 carries the number the UI has seen, so a list sent before the UI heard of a start can't bring it back. */
static int g_up[PL_UP_MAX], g_upn, g_up_party, g_cur_up, g_next_up, g_pend_up;
/* g_solo: g_cur_up was started on its own (0d15, Hit&Guess): the list was let go, so nothing of it follows.
 * g_next_rep: MPD's next song is g_cur_up again (Repeat One; Repeat All for a song on its own). */
static int g_solo, g_next_rep;
static unsigned g_up_seq;
static struct { unsigned seq; int id; } g_up_gone[32];
static int g_up_ngone;
static eng_status_t g_es;
static int g_bt;                   /* playing to Bluetooth (pb_route) */
static int g_aux = 1;              /* playing to the DAC (pb_route); off: the DAC and its mixer route are left alone */
static int g_bt_codec = -1;        /* the last codec asked of BlueALSA for g_bt_mac */
static char g_bt_mac[18];
static unsigned g_bt_rate;         /* the link's rate, as last read */
static char g_bt_sel[16], g_bt_avail[64];   /* the link's codec and the codecs the headphones offer, as last read */
/* a codec switch by hand (pb_codec): bluealsa-cli's pid, when it started, whether the music was playing */
static int g_cs_pid = -1, g_cs_play;
static double g_cs_t;
#define PB_CODEC_WAIT_S 10.0
static void codec_tick(double t);
static double g_es_t, g_last_a1;
static int g_es_valid;
static int g_told = -1;            /* the state the UI was last told: ENG_PLAY or ENG_PAUSE, -1 nothing yet */
/* a track start to announce (the full a2), once MPD knows the stream format: 0 none, 1 from a play command (0100,
 * Next, Previous), 2 a natural track change */
static int g_announce;
static double g_announce_t;

/* The resume bookmark (MEMORY_PLAY): the song in it is the one playing (g_mem_ok), its POSITION written every
 * PB_MEM_EVERY_S while playing, on pause and at power off.
 * Off for now (Jo, 2026-11-16): song.db is on the internal NAND and resume is not worth the writes. With PB_RESUME 0
 * the player neither writes MEMORY_PLAY nor resumes at start; todo.md "Resume Playback" has the plan to bring it back
 * without periodic flash writes. The tests build it with -DPB_RESUME=1. */
#ifndef PB_RESUME
#define PB_RESUME 0
#endif
#define PB_MEM_EVERY_S 30.0
static int g_mem_ok;
static double g_mem_t;
/* Resume at start (pb_resume_begin): waiting for the card (g_rs 1), the card first seen mounted, the give-up time */
#define PB_RESUME_WAIT_S   30.0
#define PB_RESUME_STABLE_S 1.0
#define PB_RESUME_OK_S     10.0
#define PB_SL_MIN_S        5.0     /* a stream-list track that stops sooner did not play (expired link, no network) */
static int g_rs;
static double g_rs_until, g_rs_mounted, g_rs_check;
/* Web radio (pb_stream_*): the DLNA work mode (0657 0C), the station the renderer was given, whether it is in MPD, when
 * Play was given, whether its track frame went out, and the local song to come back to when the mode ends. */
static int g_stream, g_st_loaded, g_st_told;
/* The list ended and C->end_mode is PL_END_STATION: the station plays on in the local work mode, as one endless song,
 * until something else is played (list_ended). g_tl_sent: the title the last track frame named. */
static int g_tail;
static char g_tl_url[600], g_tl_name[128], g_tl_sent[256];
static char g_st_uri[1024], g_st_title[256];
static double g_st_t;
static int g_st_local = -1;
static long g_st_local_ms;
static void stream_a2(int state);
/* A stream list (0d14, Bandcamp): the tracks of OPL_BC_LIST, the one playing (g_sl_cur, -1 none). They play one after
 * another in the radio work mode; a track that does not play is told to the UI with ad14 (its links have expired). */
typedef struct { long ms; char *url, *title, *artist, *album; } sl_track_t;
static sl_track_t *g_sl;
static char *g_sl_text;
static int g_sl_n, g_sl_cur = -1;
static void sl_clear(void);
static int sl_play(int idx);
static int (*g_rs_mounted_fn)(void);
static char g_rs_flag[256];
static double g_rs_flag_t;          /* the resumed song started (the flag goes once it has played PB_RESUME_OK_S), 0 none */

static double now_s(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
void pb_attenuation(unsigned char *a, unsigned char *l){
    /* the stock player: the high gain curve only with a plug in, the balanced curve with the 4.4 mm plug */
    dac_attenuation(C->vol_aux, C->gain && pl_core_plugged(C), C->bal == 1, C->balance, a, l);
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
    if(!g_aux) return;
    mix_route_dac(L);                        /* DMA3 -> DAC; without it hw_params fails (mixer.h) */
    if(rate == dac_current_rate()) return;
    unsigned char a, l; pb_attenuation(&a, &l);
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
    g_bt = 0; g_aux = 1; g_bt_codec = -1; g_bt_mac[0] = 0; g_bt_rate = 0; g_bt_sel[0] = g_bt_avail[0] = 0; g_cs_pid = -1;
    g_next = g_pending = -1; g_es_valid = 0; g_told = -1; g_announce = 0; g_mem_ok = 0; g_rs = 0; g_rs_flag_t = 0;
    g_upn = g_cur_up = g_next_up = g_pend_up = g_up_party = g_solo = g_next_rep = 0; g_up_seq = 0; g_up_ngone = 0;
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

/* The song playing now: the up-next song g_cur_up, else the list's row Q.cur. 1, 0 gone, -1 error. */
static int cur_row(sqlite3 *d, qd_row_t *r){ return g_cur_up ? qd_song(d, g_cur_up, r) : qd_row(d, pq_id(&Q, Q.cur), r); }
/* Up-next song id leaves g_up (it starts, or can't be played): the UI hears it (ad13 <number 8h><SONG.ID 8h>). */
static void up_take(int id){
    for(int i = 0; i < g_upn; i++) if(g_up[i] == id){ memmove(&g_up[i], &g_up[i + 1], (size_t)(g_upn - i - 1) * sizeof *g_up); g_upn--; break; }
    g_up_seq++;
    if(g_up_ngone == (int)(sizeof g_up_gone / sizeof *g_up_gone)){ memmove(&g_up_gone[0], &g_up_gone[1], sizeof g_up_gone - sizeof *g_up_gone); g_up_ngone--; }
    g_up_gone[g_up_ngone].seq = g_up_seq; g_up_gone[g_up_ngone].id = id; g_up_ngone++;
    char f[32]; snprintf(f, sizeof f, "ad130018%08X%08X", g_up_seq, (unsigned)id);
    send_lit(f);
}
static int playable_song(sqlite3 *d, int id, qd_row_t *r){
    int x = qd_song(d, id, r);
    if(x != 1){ L("up next: song %d: %s", id, x == 0 ? "not in the library" : sqlite3_errmsg(d)); return 0; }
    if(r->is_iso){ L("up next: song %d is a SACD ISO track, not supported - skipped", id); return 0; }
    return 1;
}

/* Queue the song that follows the current one when it ends (by the play mode); skips rows MPD can't take. The UI's
 * up-next songs come first. */
static void queue_next(sqlite3 *d){
    g_next_up = g_pend_up = 0; g_next_rep = 0;
    if(g_cur_up && (C->play_mode == PQ_REPEAT_ONE || (g_solo && C->play_mode == PQ_REPEAT_ALL))){   /* the same song again */
        qd_row_t *u = malloc(sizeof *u);
        g_next = g_pending = -1;
        if(u && playable_song(d, g_cur_up, u) && eng_set_next(u->path, PART(u), dac_current_rate()) == 0) g_next_rep = 1;
        else eng_set_next(NULL, 0, 0, 0);
        free(u);
        return;
    }
    /* Repeat One (Hit&Guess loops its song) and Single (a book chapter) keep to the song: the up-next songs wait for
     * Next or another play mode */
    if(g_upn > 0 && C->play_mode != PQ_REPEAT_ONE && C->play_mode != PQ_SINGLE){
        qd_row_t *u = malloc(sizeof *u);
        g_next = g_pending = -1;
        while(u && g_upn > 0){
            int id = g_up[0];
            int x = playable_song(d, id, u) ? eng_set_next(u->path, PART(u), dac_current_rate()) : -1;
            if(x == 0){ g_next_up = id; break; }
            if(x == 1){ g_pend_up = id; L("up next: next song has another sample rate, started after this one (no gapless)"); break; }
            up_take(id);                                  /* can't be played: it never will, the UI drops it too */
        }
        free(u);
        if(g_next_up || g_pend_up) return;
        eng_set_next(NULL, 0, 0, 0);
    }
    if(g_cur_up && (g_up_party || g_solo)){ g_next = g_pending = -1; eng_set_next(NULL, 0, 0, 0); return; }   /* party, or no list: it ends */
    int mode = C->play_mode, save = Q.cur;
    if(g_cur_up && mode == PQ_REPEAT_ONE) mode = PQ_REPEAT_ALL;   /* after up-next songs: the list's next song */
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
/* The first up-next song, now (Next, a natural end at another sample rate, the station after the list). 0 or -1. */
static int start_up(sqlite3 *d){
    qd_row_t *r = malloc(sizeof *r);
    if(!r) return -1;
    while(g_upn > 0){
        int id = g_up[0];
        int ok = playable_song(d, id, r) && eng_load(r->path, PART(r), 0, 0) == 0;
        up_take(id);
        if(!ok) continue;
        g_cur_up = id; g_solo = 0;
        L("up next: song %d starts (%s), %d more", id, r->path, g_upn);
        free(r);
        queue_next(d);
        g_announce = 1; g_announce_t = now_s();
        return 0;
    }
    free(r);
    L("up next: nothing in it can be played");
    return -1;
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
    Q.cur = idx; g_cur_up = 0; g_solo = 0;
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
    if(cur_row(d, r) != 1){ L("queue: current row gone, no track frame"); goto out; }
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
    g_mem_ok = 0;
    if(!PB_RESUME) return;                 /* no flash writes (see PB_RESUME) */
    qd_row_t *r = malloc(sizeof *r);
    if(r && cur_row(d, r) == 1){
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
    if(g_aux && s.state == ENG_PLAY && s.rate && s.rate != dac_current_rate()){
        unsigned char a, l; pb_attenuation(&a, &l);
        dac_start(s.rate, a, l, L);
    }
    int was = g_es_valid ? g_es.state : ENG_STOP;
    if(s.state != was || (g_es_valid && strcmp(s.file, g_es.file)))
        L("engine: %s, %ld/%ld ms, %u Hz, %s", s.state == ENG_PLAY ? "play" : s.state == ENG_PAUSE ? "pause" : "stop",
          s.elapsed_ms, s.duration_ms, s.rate, s.file);
    g_es = s; g_es_t = now_s(); g_es_valid = 1;
}

static void stream_file(int on);
static void stream_frame(int state, const char *uri, const char *title, const char *artist, const char *album,
                         const char *work_mode);
static void stream_frame_at(int state, const char *uri, const char *title, const char *artist, const char *album,
                            const char *work_mode, long dur_ms, int num, int total);
/* ---- after the list (0d11 PL_END_*, 0d12 the station) ---- */
static void tail_frame(int force){
    const char *t = g_es_valid && !strcmp(g_es.file, g_tl_url) && g_es.title[0] ? g_es.title : g_tl_name;
    if(!force && !strcmp(t, g_tl_sent)) return;
    snprintf(g_tl_sent, sizeof g_tl_sent, "%s", t);
    stream_frame(g_es_valid && g_es.state == ENG_PLAY ? 0 : 1, g_tl_url, t, g_tl_name, "Web radio", "LOCAL");
    if(strcmp(t, g_tl_name)) L("list end: station \"%s\" now \"%s\"", g_tl_name, t);
}
static void tail_event(void){
    if(g_es.state == ENG_PLAY) tail_frame(0);          /* a new song named by the station: a new track frame */
    if(g_es.state == ENG_STOP) L("list end: the station stopped (%s)", g_tl_url);
    tell_state(g_es.state, 0);
}
static void tail_end(void){ g_tail = 0; g_tl_sent[0] = 0; }
/* The last song of the list ended (no next one by the play mode). 0 when something plays on. */
static int list_ended(void){
    if(C->end_mode == PL_END_UI){
        /* the UI starts the next list itself (ui/radio.c radio_end_list_ended), so its queue knows the list it plays
         * and returns to it after queued songs; this player stops as usual */
        L("list end: the UI picks what follows (ad11)");
        send_lit("ad11000C0000");
        return -1;
    }
    if(C->end_mode == PL_END_STATION){
        if(!C->end_url[0]){ L("list end: no station set (0d12)"); return -1; }
        if(strncmp(C->end_url, "http://", 7) && strncmp(C->end_url, "https://", 8)){ L("list end: not a stream URL: %.80s", C->end_url); return -1; }
        snprintf(g_tl_url, sizeof g_tl_url, "%s", C->end_url);
        snprintf(g_tl_name, sizeof g_tl_name, "%s", C->end_name[0] ? C->end_name : "Web radio");
        if(eng_cert_stale() && eng_restart() != 0) L("list end: MPD did not start again");
        if(eng_load(g_tl_url, 0, 0, 0, 0) != 0){ L("list end: MPD can't play %s", g_tl_url); return -1; }
        g_tail = 1; g_tl_sent[0] = 0; g_next = g_pending = -1; g_announce = 0;
        L("list end: station \"%s\" %s", g_tl_name, g_tl_url);
        update_status();
        tail_frame(1);
        tell_state(g_es_valid ? g_es.state : ENG_PLAY, 1);
        return 0;
    }
    return -1;
}
int pb_tail_active(void){ return g_tail; }
void pb_engine_event(void){
    int was = g_es_valid ? g_es.state : ENG_STOP;
    update_status();
    if(!g_es_valid) return;
    if(g_stream){                                     /* the radio: no queue, no bookmark */
        if(g_st_loaded && g_es.state == ENG_PLAY && !g_st_told){ stream_a2(0); g_st_told = 1; }
        stream_file(1);
        if(g_st_loaded && was == ENG_PLAY && g_es.state == ENG_STOP){
            L("radio: the stream stopped (%s)", g_st_uri);
            if(g_sl_cur >= 0){                       /* a stream list: the track ended, or it didn't play at all */
                double played = now_s() - g_st_t;
                g_st_loaded = 0;
                if(played < PB_SL_MIN_S){
                    L("stream list: track %d stopped after %.1f s - asking the UI for fresh links (ad14)", g_sl_cur, played);
                    char f[24]; snprintf(f, sizeof f, "ad14000C%04X", (unsigned)g_sl_cur & 0xFFFFu); send_lit(f);
                } else if(g_sl_cur + 1 < g_sl_n){ sl_play(g_sl_cur + 1); return; }
                else L("stream list: the last track (%d) ended", g_sl_cur);
            }
        }
        tell_state(g_es.state, 0);
        return;
    }
    if(g_tail){ tail_event(); return; }
    if(g_es.state == ENG_STOP && was == ENG_PLAY && (g_pending >= 0 || g_pend_up)){   /* the song ended; the next needs the DAC restarted */
        int p = g_pending, up = g_pend_up;
        sqlite3 *d = db();
        if(d){ if((up ? start_up(d) : start_at(d, p, +1, 0, 0)) == 0) g_announce = 2; sqlite3_close(d); }
        return;
    }
    if(eng_advanced(&g_es)){                          /* the queued next song has started */
        if(g_next_rep){ g_next_rep = 0; L("song %d again", g_cur_up); }
        else if(g_next_up){ up_take(g_next_up); g_cur_up = g_next_up; g_solo = 0; L("up next: song %d plays, %d more", g_cur_up, g_upn); }
        else { Q.cur = g_next; g_cur_up = 0; }
        sqlite3 *d = db();
        if(d){ queue_next(d); sqlite3_close(d); }
        g_announce = 2; g_announce_t = now_s();
    }
    if(g_announce){
        /* wait for the stream format (song_sample_rate), at most a second (pb_tick) */
        if(g_es.state != ENG_PLAY || g_es.rate) announce(&g_es);
        return;
    }
    if(was == ENG_PLAY && g_es.state == ENG_STOP && g_next < 0 && (Q.n > 0 || g_cur_up) && list_ended() == 0) return;
    if(was == ENG_PLAY && g_es.state != ENG_PLAY) remember_position("paused");
    tell_state(g_es.state, 0);
}

static void resume_tick(double t);
void pb_tick(double t){
    resume_tick(t);
    codec_tick(t);
    if(g_announce && t - g_announce_t >= 1.0){ update_status(); if(g_es_valid) announce(&g_es); }
    if(!g_stream && g_es_valid && g_es.state == ENG_PLAY && g_mem_ok && t - g_mem_t >= PB_MEM_EVERY_S) remember_position("playing");
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
    if(g_stream){ L("play list: ends the radio"); g_stream = g_st_loaded = 0; g_st_local = -1; sl_clear(); }
    if(g_tail){ L("play list: ends the station after the list"); tail_end(); }
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

/* The output route changed (0666, 06b3; ui/main.c ui_route_bt / ui_route_analog). */
static void bt_read(void){
    if(bto_info(g_bt_mac, &g_bt_rate, g_bt_sel, sizeof g_bt_sel, g_bt_avail, sizeof g_bt_avail, L) != 0) g_bt_rate = 0;
}

void pb_route(void){
    int bt = C->out_bt && C->route == PL_ROUTE_BT && C->bt_mac[0];
    int aux = C->out_aux || !bt;                        /* one output always plays */
    if(C->out_bt && !bt) L("route: Bluetooth, waiting for the headphones (0666 2 and 06b3)");
    if(bt && (!g_bt || strcmp(g_bt_mac, C->bt_mac))){
        FILE *f = fopen(ENG_BT_DEV, "we");
        if(!f || fprintf(f, "%s\n", C->bt_mac) < 0){
            L("route: write %s: %s", ENG_BT_DEV, strerror(errno));
            if(f) fclose(f);
            bt = 0; aux = 1;
        } else {
            fclose(f);
            snprintf(g_bt_mac, sizeof g_bt_mac, "%s", C->bt_mac);
            g_bt_codec = -1;
        }
    }
    if(bt){
        if(C->bt_codec > 0 && C->bt_codec != g_bt_codec) bto_codec(g_bt_mac, C->bt_codec, L);   /* 06b3 at the route */
        g_bt_codec = C->bt_codec;
        bt_read();
    }
    if(aux && !g_aux && g_es_valid && g_es.state != ENG_STOP && g_es.rate){
        g_aux = 1;                                      /* the DAC joins a running stream: its start sequence first */
        prepare(g_es.rate);
    }
    g_aux = aux; g_bt = bt;
    L("route: %s%s%s, link at %u Hz", aux ? "DAC" : "", aux && bt ? " + " : "", bt ? g_bt_mac : "", bt ? g_bt_rate : 0);
    if(g_cs_pid < 0) eng_output(aux, bt, g_bt_rate);    /* during a codec switch pb_tick sets the outputs once it is done */
    if(bt) bto_volume(g_bt_mac, C->vol_bt, PL_VOL_MAX, L);
}

void pb_codec(int value){
    if(!g_bt){ L("codec: Bluetooth is not playing"); return; }
    if(g_cs_pid >= 0){ L("codec: a switch is still running, %s not asked", bto_codec_name(value)); return; }
    char want[16]; snprintf(want, sizeof want, "%s", bto_codec_name(value));
    for(char *p = want; *p; p++) if(*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    if(!strcmp(want, g_bt_sel)){ L("codec: already %s", g_bt_sel); return; }
    /* the switch renegotiates the link: pause, close the Bluetooth output, switch, reopen at the new rate, play */
    g_cs_play = g_es_valid && g_es.state == ENG_PLAY;
    if(g_cs_play) eng_pause(1);
    eng_output(g_aux, 0, 0);
    g_cs_pid = bto_codec_start(g_bt_mac, value, L);
    g_cs_t = now_s();
    if(g_cs_pid < 0){ eng_output(g_aux, 1, g_bt_rate); if(g_cs_play) eng_pause(0); }
}

static void codec_tick(double t){
    if(g_cs_pid < 0) return;
    int st = 0;
    pid_t r = waitpid((pid_t)g_cs_pid, &st, WNOHANG);
    if(r == 0 && t - g_cs_t < PB_CODEC_WAIT_S) return;
    if(r == 0){ L("codec: bluealsa-cli still running after %.0f s, going on", PB_CODEC_WAIT_S); kill((pid_t)g_cs_pid, SIGKILL); waitpid((pid_t)g_cs_pid, &st, 0); }
    else L("codec: bluealsa-cli done (%d)", r > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    g_cs_pid = -1;
    if(!g_bt){ eng_output(g_aux, 0, 0); return; }       /* Bluetooth went off meanwhile (pb_route left the outputs) */
    bt_read();
    L("codec: now %s, link at %u Hz", g_bt_sel[0] ? g_bt_sel : "?", g_bt_rate);
    eng_output(g_aux, 1, g_bt_rate);
    bto_volume(g_bt_mac, C->vol_bt, PL_VOL_MAX, L);
    if(g_cs_play) eng_pause(0);
}

int pb_switching(void){ return g_cs_pid >= 0; }
int pb_aux(void){ return g_aux; }
const char *pb_bt_codec(void){ return g_bt ? g_bt_sel : ""; }
const char *pb_bt_codecs(void){ return g_bt ? g_bt_avail : ""; }
int pb_bluetooth(void){ return g_bt; }
const char *pb_bt_mac(void){ return g_bt_mac; }

/* Resume at start, like the stock player's play_memory thread (docs/MQ_PLAYER_REPLACEMENT.md, "Resume"). */
void pb_resume_begin(int (*mounted)(void), const char *flag_path){
    g_rs_mounted_fn = mounted;
    snprintf(g_rs_flag, sizeof g_rs_flag, "%s", flag_path ? flag_path : "");
    if(!PB_RESUME){ L("resume: not built in (PB_RESUME 0, no flash writes)"); g_rs = 0; return; }
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
    if(g_stream && g_sl_cur >= 0) switch(op){        /* a stream list: play/pause, skip, seek */
    case PL_T_TOGGLE: if(!g_st_loaded) return sl_play(g_sl_cur); return eng_toggle();   /* after its end: again */
    case PL_T_PAUSE: pb_pause(); return 0;
    case PL_T_MODE: return 0;
    case PL_T_STATE: if(!g_st_loaded) return -1; update_status(); stream_a2(g_es.state == ENG_PLAY ? 0 : 1); return 0;
    case PL_T_NEXT: if(g_sl_cur + 1 >= g_sl_n){ L("next: the last track of the list"); return -1; } return sl_play(g_sl_cur + 1);
    case PL_T_PREV: {                                  /* as on a CD: back to the start, or the track before near it */
        long ms = g_st_loaded && g_es_valid ? g_es.elapsed_ms + (g_es.state == ENG_PLAY ? (long)((now_s() - g_es_t) * 1000.0) : 0) : 0;
        return sl_play(ms > 3000 || g_sl_cur == 0 ? g_sl_cur : g_sl_cur - 1);
    }
    case PL_T_SEEK: if(!g_st_loaded || eng_seek_ms(arg) != 0) return -1; update_status(); return 0;
    default: return -1;
    }
    if(g_stream) switch(op){                         /* the radio: play/pause and the state; no skip, no seek */
    case PL_T_TOGGLE: if(!g_st_loaded){ L("play/pause: no station"); return -1; } return eng_toggle();
    case PL_T_PAUSE: pb_pause(); return 0;
    case PL_T_MODE: break;
    case PL_T_STATE: if(!g_st_loaded) return -1; update_status(); stream_a2(g_es.state == ENG_PLAY ? 0 : 1); return 0;
    default: L("transport %d: not on the radio", op); return -1;
    }
    if(g_tail) switch(op){                           /* the station after the list: play/pause; Next starts the list again */
    case PL_T_TOGGLE: return eng_toggle();
    case PL_T_PAUSE: pb_pause(); return 0;
    case PL_T_MODE: return 0;
    case PL_T_STATE: update_status(); tail_frame(1); return 0;
    case PL_T_NEXT: case PL_T_PREV: {
        if(op == PL_T_NEXT && g_upn > 0){               /* the UI's queue first */
            tail_end();
            sqlite3 *d = db();
            if(!d) return -1;
            int r = start_up(d);
            sqlite3_close(d);
            return r;
        }
        if(Q.n <= 0) return -1;
        tail_end();
        sqlite3 *d = db();
        if(!d) return -1;
        int idx = pq_from_base(&Q, 0);
        int r = start_at(d, idx >= 0 ? idx : 0, +1, 0, 0);
        sqlite3_close(d);
        L("list end: %s - the list again from its start", op == PL_T_NEXT ? "Next" : "Previous");
        return r;
    }
    default: L("transport %d: not on the station after the list", op); return -1;
    }
    switch(op){
    case PL_T_TOGGLE:
        if(Q.n <= 0 && !g_cur_up && g_upn > 0){ sqlite3 *d = db(); int r = d ? start_up(d) : -1; if(d) sqlite3_close(d); return r; }
        if(Q.n <= 0 && !g_cur_up){ L("play/pause: nothing loaded"); return -1; }
        return eng_toggle();
    case PL_T_PAUSE: pb_pause(); return 0;
    case PL_T_MODE: pb_mode_changed(); return 0;
    case PL_T_STATE: {
        if((Q.n <= 0 && !g_cur_up) || !g_es_valid || g_es.songid < 0) return -1;
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
        if(op == PL_T_NEXT && g_upn > 0){                /* the UI's queue first */
            sqlite3 *d = db();
            if(!d) return -1;
            int r = start_up(d);
            sqlite3_close(d);
            return r;
        }
        if(op == PL_T_NEXT && g_cur_up && g_up_party){   /* party: after the queue the list ends */
            L("next: the last up-next song in party mode - the list ends");
            eng_pause(1);
            list_ended();
            return 0;
        }
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

int pb_play_song(int song_id){
    if(!eng_ready()){ L("play song: no engine"); return -1; }
    if(g_stream){ L("play song: ends the radio"); g_stream = g_st_loaded = 0; g_st_local = -1; sl_clear(); }
    if(g_tail){ L("play song: ends the station after the list"); tail_end(); }
    sqlite3 *d = db();
    if(!d) return -1;
    qd_row_t *r = malloc(sizeof *r);
    int rc = -1;
    if(r && playable_song(d, song_id, r) && eng_load(r->path, PART(r), 0, 0) == 0){
        pq_free(&Q);                                   /* the list is let go: nothing of it follows this song */
        g_cur_up = song_id; g_solo = 1;
        L("play song: %d on its own (%s)", song_id, r->path);
        queue_next(d);
        g_announce = 1; g_announce_t = now_s();
        rc = 0;
    } else L("play song: %d can't be played", song_id);
    free(r);
    sqlite3_close(d);
    return rc;
}

int pb_up_set(unsigned ack, int party, const int *ids, int n){
    int k = 0;
    for(int i = 0; i < g_up_ngone; i++) if((int)(g_up_gone[i].seq - ack) > 0) g_up_gone[k++] = g_up_gone[i];
    g_up_ngone = k;
    int skip = 0;                                      /* started meanwhile: the UI's list still has them at its head */
    for(int i = 0; i < g_up_ngone && skip < n; i++) if(ids[skip] == g_up_gone[i].id) skip++;
    if(n - skip > PL_UP_MAX) n = PL_UP_MAX + skip;
    memcpy(g_up, ids + skip, (size_t)(n - skip) * sizeof *g_up);
    g_upn = n - skip; g_up_party = party;
    L("up next: %d songs%s%s", g_upn, party ? " (party: then the list ends)" : "", skip ? " (the ones already started left out)" : "");
    if(!eng_ready() || g_stream) return 0;
    sqlite3 *d = db();
    if(!d) return -1;
    if(g_tail && g_upn > 0){ L("up next: ends the station after the list"); tail_end(); start_up(d); }
    else if(g_es_valid && g_es.state != ENG_STOP && (g_cur_up || (Q.n > 0 && Q.cur >= 0))) queue_next(d);
    sqlite3_close(d);
    return 0;
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
    if(g_cur_up || (Q.n > 0 && Q.cur >= 0)) queue_next(d);
    sqlite3_close(d);
}

/* ================================================================ web radio (the DLNA work mode) */
/* a station's track frame: the DLNA one for the Radio app, a local one for the station after the list (g_tail) */
static void stream_frame(int state, const char *uri, const char *title, const char *artist, const char *album,
                         const char *work_mode){
    stream_frame_at(state, uri, title, artist, album, work_mode, 0, 1, 1);
}
static void stream_frame_at(int state, const char *uri, const char *title, const char *artist, const char *album,
                            const char *work_mode, long dur_ms, int num, int total){
    char *song = malloc(PP_MAX_FRAME), *json = malloc(PP_MAX_FRAME), *frame = malloc(PP_MAX_FRAME);
    if(!song || !json || !frame){ L("radio: out of memory for the track frame"); goto out; }
    unsigned rate = g_es_valid ? g_es.rate : 0;
    int bits = g_es_valid && g_es.bits ? g_es.bits : 16, ch = g_es_valid && g_es.channels ? g_es.channels : 2;
    jbuf_t j = { song, 0, PP_MAX_FRAME, 1 };
    song[0] = 0;
    jraw(&j, "{\"id\":0,\"pos_id\":%d,\"song_name\":\"", total > 1 || dur_ms > 0 ? num : 0);
    jstr(&j, title);
    jraw(&j, "\",\"song_duration_time\":%ld,\"song_file_path\":\"", dur_ms);
    jstr(&j, uri);
    jraw(&j, "\",\"song_sample_rate\":%u,\"song_encoding_rate\":%d,\"song_bit_rate\":%d,\"song_artist_name\":\"", rate, bits,
         g_es_valid ? g_es.kbps : 0);
    jstr(&j, artist);
    jraw(&j, "\",\"song_album_name\":\"");
    jstr(&j, album);
    jraw(&j, "\",\"song_style_name\":\"(null)\",\"song_track\":0,\"song_channel\":%d,\"is_sacd\":false,"
         "\"is_cue\":false,\"is_dsd\":false,\"is_m3u\":false,\"m3u_file_path\":\"(null)\"}", ch);
    jbuf_t o = { json, 0, PP_MAX_FRAME, j.ok };
    json[0] = 0;
    jraw(&o, "{\"song\":\"");
    jstr(&o, song);
    jraw(&o, "\",\"love\":false,\"playerflag\":3,\"state\":%d,\"playing_num\":\"%d/%d\",\"work_mode\":\"%s\"}", state, num, total,
         work_mode);
    int n = o.ok ? pp_fmt_a2(frame, PP_MAX_FRAME, json) : -1;
    if(n < 0){ L("radio: track frame too long"); goto out; }
    send_(frame, n);
out:
    free(song); free(json); free(frame);
}
static void stream_a2(int state){
    if(g_sl_cur >= 0){
        const sl_track_t *t = &g_sl[g_sl_cur];
        stream_frame_at(state, t->url, t->title, t->artist, t->album, "DLNA", t->ms, g_sl_cur + 1, g_sl_n);
        return;
    }
    stream_frame(state, g_st_uri, g_st_title[0] ? g_st_title : g_st_uri, "Web radio", "", "DLNA");
}
/* OPL_STREAM: the song the station names now, as MPD has it, for ui/radio.c (no second connection to the station).
 * Written when it changes, removed when no station plays. */
static char g_sf_last[1600];
static void clean_line(char *d, size_t cap, const char *v){
    size_t n = 0;
    for(; *v && n + 1 < cap; v++) d[n++] = (*v == '\n' || *v == '\r') ? ' ' : *v;
    d[n] = 0;
}
static void stream_file(int on){
    if(!on || !g_st_loaded){
        if(g_sf_last[0]){ if(unlink(OPL_STREAM) != 0 && errno != ENOENT) L("radio: unlink %s: %s", OPL_STREAM, strerror(errno)); g_sf_last[0] = 0; }
        return;
    }
    char a[160] = "", t[256] = "", b[sizeof g_sf_last];
    if(g_es_valid && !strcmp(g_es.file, g_st_uri)){ clean_line(a, sizeof a, g_es.artist); clean_line(t, sizeof t, g_es.title); }
    snprintf(b, sizeof b, "uri=%s\nartist=%s\ntitle=%s\n", g_st_uri, a, t);
    if(!strcmp(b, g_sf_last)) return;
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.tmp", OPL_STREAM);
    FILE *f = fopen(tmp, "we");
    if(!f || fputs(b, f) < 0 || fclose(f) != 0){ L("radio: write %s: %s", tmp, strerror(errno)); return; }
    if(rename(tmp, OPL_STREAM) != 0){ L("radio: rename %s: %s", tmp, strerror(errno)); return; }
    if(t[0] || a[0]) L("radio: now \"%s%s%s\"", a, a[0] ? " - " : "", t);
    snprintf(g_sf_last, sizeof g_sf_last, "%s", b);
}
void pb_stream_mode(int on){
    if(on && !g_stream){
        if(g_tail){ char r[256]; eng_cmd("stop", r, sizeof r); eng_cmd("clear", r, sizeof r); tail_end(); update_status(); }
        g_stream = 1; g_st_loaded = 0; g_st_uri[0] = g_st_title[0] = 0;
        g_st_local = -1;
        if(Q.n > 0 && Q.cur >= 0 && g_es_valid){             /* the local song, to come back to */
            update_status();
            remember_position("radio");
            g_st_local = Q.cur;
            g_st_local_ms = g_es.elapsed_ms + (g_es.state == ENG_PLAY ? (long)((now_s() - g_es_t) * 1000.0) : 0);
            if(g_es.state == ENG_PLAY) eng_pause(1);
        }
        g_mem_ok = 0; g_announce = 0; g_next = g_pending = -1;
        L("radio: DLNA mode (local song %d at %ld ms kept)", g_st_local, g_st_local >= 0 ? g_st_local_ms : 0L);
    } else if(!on && g_stream){
        g_stream = 0;
        sl_clear();
        if(g_st_loaded){ char r[256]; eng_cmd("stop", r, sizeof r); eng_cmd("clear", r, sizeof r); }
        g_st_loaded = 0;
        stream_file(0);
        L("radio: back to local playback");
        if(g_st_local >= 0 && g_st_local < Q.n){              /* the local song again, paused where it was */
            sqlite3 *d = db();
            if(d){ start_at(d, g_st_local, +1, g_st_local_ms, 1); sqlite3_close(d); }
        }
        g_st_local = -1;
    }
}
int pb_stream_active(void){ return g_stream; }
int pb_stream_set(const char *uri, const char *title){
    if(!g_stream) return -1;
    if(strncmp(uri, "http://", 7) && strncmp(uri, "https://", 8) && uri[0] != '/'){ L("radio: not a stream URL: %.80s", uri); return -1; }   /* '/': a saved Bandcamp track on the card */
    if(strlen(uri) >= sizeof g_st_uri){ L("radio: stream URL too long"); return -1; }
    snprintf(g_st_uri, sizeof g_st_uri, "%s", uri);
    snprintf(g_st_title, sizeof g_st_title, "%s", title ? title : "");
    L("radio: station \"%s\" %s", g_st_title, g_st_uri);
    return 0;
}
int pb_stream_play(void){
    if(!g_stream || !g_st_uri[0]) return -1;
    if(g_st_loaded && g_es_valid && g_es.state == ENG_PAUSE && (!strcmp(g_es.file, g_st_uri) ||
       (g_st_uri[0] == '/' && !strncmp(g_es.file, "file://", 7) && !strcmp(g_es.file + 7, g_st_uri)))){   /* plays on after a pause */
        if(eng_pause(0) != 0) return -1;
        update_status();
        return 0;
    }
    if(eng_cert_stale()){                                /* Radio Certificates changed: MPD reads it only at its start */
        L("radio: certificate check is now %s - restarting MPD", access(OPL_NO_CERT_CHECK, F_OK) == 0 ? "off" : "on");
        if(eng_restart() != 0) L("radio: MPD did not start again");
    }
    if(eng_load(g_st_uri, 0, 0, 0, 0) != 0){ L("radio: MPD can't play %s (an MPD without curl?)", g_st_uri); g_st_loaded = 0; return -1; }
    g_st_loaded = 1; g_st_told = 0; g_st_t = now_s();
    update_status();
    return 0;
}
int pb_stream_pause(void){
    if(!g_stream || !g_st_loaded || eng_pause(1) != 0) return -1;
    update_status();
    return 0;
}
int pb_stream_stop(void){
    if(!g_stream) return -1;
    sl_clear();
    if(g_st_loaded){ char r[256]; eng_cmd("stop", r, sizeof r); eng_cmd("clear", r, sizeof r); g_st_loaded = 0; update_status(); tell_state(ENG_PAUSE, 0); }
    stream_file(0);
    return 0;
}
const char *pb_stream_uri(void){ return g_stream ? g_st_uri : ""; }

/* 0d10: a new station; the one playing stops first */
int pb_stream_start(const char *title, const char *uri){
    if(pb_stream_set(uri, title) != 0) return -1;
    sl_clear();                                          /* a station replaces a stream list */
    if(g_st_loaded){ char x[256]; eng_cmd("stop", x, sizeof x); g_st_loaded = 0; }
    return pb_stream_play();
}

/* ================================================================ a stream list (0d14, Bandcamp) */
static void sl_clear(void){
    free(g_sl); free(g_sl_text); g_sl = NULL; g_sl_text = NULL; g_sl_n = 0; g_sl_cur = -1;
}
/* OPL_BC_LIST's text into tracks (in place); bad lines are skipped. Returns the count. */
static int sl_parse(char *text, sl_track_t *out, int max){
    int n = 0;
    for(char *l = text; l && *l && n < max; ){
        char *e = strchr(l, '\n'); if(e) *e = 0;
        char *f[5] = { l, NULL, NULL, NULL, NULL };
        for(int k = 1; k < 5 && f[k - 1]; k++){ char *t = strchr(f[k - 1], '\t'); if(t){ *t = 0; f[k] = t + 1; } }
        char *end;
        long ms = f[4] ? strtol(f[0], &end, 10) : -1;
        if(f[4] && !*end && ms >= 0 && (!strncmp(f[1], "http://", 7) || !strncmp(f[1], "https://", 8) || f[1][0] == '/')   /* '/': saved on the card */
           && strlen(f[1]) < sizeof g_st_uri){
            out[n].ms = ms; out[n].url = f[1]; out[n].title = f[2]; out[n].artist = f[3]; out[n].album = f[4]; n++;
        } else L("stream list: line %d skipped", n + 1);
        l = e ? e + 1 : NULL;
    }
    return n;
}
static int sl_load(const char *path){
    sl_clear();
    FILE *f = fopen(path, "re");
    if(!f){ L("stream list: open %s: %s", path, strerror(errno)); return -1; }
    size_t cap = 0, len = 0; char *b = NULL; size_t r;
    do {
        if(len + 4096 + 1 > cap){
            if(cap >= (size_t)OPL_SLIST_MAX * 4096){ L("stream list: %s too big", path); free(b); fclose(f); return -1; }
            char *nb = realloc(b, cap += 16384);
            if(!nb){ free(b); fclose(f); L("stream list: out of memory"); return -1; }
            b = nb;
        }
        r = fread(b + len, 1, 4096, f); len += r;
    } while(r > 0);
    fclose(f);
    if(!b) return -1;
    b[len] = 0;
    g_sl = calloc(OPL_SLIST_MAX, sizeof *g_sl);
    if(!g_sl){ free(b); L("stream list: out of memory"); return -1; }
    g_sl_text = b;
    g_sl_n = sl_parse(b, g_sl, OPL_SLIST_MAX);
    if(g_sl_n <= 0){ L("stream list: no tracks in %s", path); sl_clear(); return -1; }
    return 0;
}
static int sl_play(int idx){
    if(!g_stream || idx < 0 || idx >= g_sl_n) return -1;
    const sl_track_t *t = &g_sl[idx];
    g_sl_cur = idx;
    if(pb_stream_set(t->url, t->title) != 0) return -1;
    if(g_st_loaded){ char x[256]; eng_cmd("stop", x, sizeof x); g_st_loaded = 0; }
    L("stream list: track %d of %d, \"%s\" (%ld ms)", idx + 1, g_sl_n, t->title, t->ms);
    if(pb_stream_play() != 0){
        char f[24]; snprintf(f, sizeof f, "ad14000C%04X", (unsigned)idx & 0xFFFFu); send_lit(f);
        return -1;
    }
    return 0;
}
int pb_slist_play(int idx){
    if(!g_stream){ L("stream list: not in the radio work mode"); return -1; }
    if(sl_load(OPL_BC_LIST) != 0) return -1;
    if(idx >= g_sl_n){ L("stream list: track %d, the list has %d", idx, g_sl_n); idx = 0; }
    return sl_play(idx);
}
int pb_slist_track(void){ return g_stream ? g_sl_cur : -1; }
#ifdef PB_RESUME
void pb_test_age_stream(double sec){ g_st_t -= sec; }   /* host test: the track has played sec seconds */
#endif
