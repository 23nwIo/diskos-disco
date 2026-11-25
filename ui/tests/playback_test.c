/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Host test of the own player's queue (ui/player/queue.c, queuedb.c) and playback (playback.c) against a real SQLite
 * song.db and an in-process stand-in for the MPD engine (the eng_* and dac_* functions below), so the queue logic, the
 * list in RAM (published to OPL_LIST, never to LIST_SONG_0) and the frames to the UI are checked without MPD or the Disc.
 * Run: make -C ui/tests playback_test && ./ui/tests/playback_test */
#include "../player/btout.h"
#include "../player/core.h"
#include "../player/engine.h"
#include "../player/playback.h"
#include "../player/proto.h"
#include "../player/queue.h"
#include "../player/queuedb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

/* ---- stand-in engine: two slots like MPD with our window ---- */
static char e_cur[1024], e_next[1024], e_missing[1024];
static int e_cur_id = -1, e_next_id = -1, e_ids = 1, e_state = ENG_STOP, e_armed, e_loads;
static long e_ms, e_off, e_len, e_next_off, e_next_len;
static unsigned rate_of(const char *p){ return strstr(p, "96k") ? 96000 : 44100; }
static eng_prepare_fn e_prepare;
void eng_set_prepare(eng_prepare_fn fn){ e_prepare = fn; }
int eng_ready(void){ return 1; }
int eng_load(const char *p, long off, long len, long ms, int paused){
    if(e_missing[0] && !strcmp(p, e_missing)) return -1;
    snprintf(e_cur, sizeof e_cur, "%s", p); e_cur_id = e_ids++; e_next[0] = 0; e_next_id = -1; e_ms = ms; e_loads++;
    e_off = off; e_len = len;
    e_armed = paused; e_state = paused ? ENG_PAUSE : ENG_PLAY;
    if(!paused && e_prepare) e_prepare(rate_of(p));          /* as engine.c: the DAC before MPD opens the PCM */
    return 0;
}
int eng_set_next(const char *p, long off, long len, unsigned must_rate){
    e_next[0] = 0; e_next_id = -1; e_next_off = off; e_next_len = len;
    if(!p) return 0;
    if(e_missing[0] && !strcmp(p, e_missing)) return -1;
    if(must_rate && rate_of(p) != must_rate) return 1;
    snprintf(e_next, sizeof e_next, "%s", p); e_next_id = e_ids++;
    return 0;
}
static int e_advanced_flag;
int eng_advanced(const eng_status_t *s){ (void)s; int a = e_advanced_flag; e_advanced_flag = 0; return a; }
int eng_toggle(void){ e_state = e_state == ENG_PLAY ? ENG_PAUSE : ENG_PLAY; e_armed = 0; return 0; }
int eng_pause(int on){ if(on && e_state == ENG_PLAY) e_state = ENG_PAUSE; else if(!on && e_state == ENG_PAUSE) e_state = ENG_PLAY; return 0; }
int eng_seek_ms(long ms){ e_ms = ms; return 0; }
static int e_cert_stale, e_restarts;
static char e_title[64];
int eng_cert_stale(void){ return e_cert_stale; }
int eng_restart(void){ e_restarts++; e_cert_stale = 0; e_cur[0] = 0; e_cur_id = -1; e_state = ENG_STOP; return 0; }
int eng_status(eng_status_t *s){
    memset(s, 0, sizeof *s);
    snprintf(s->title, sizeof s->title, "%s", e_title);
    s->state = e_state; s->songid = e_cur_id; s->song = e_cur_id >= 0 ? 0 : -1; s->elapsed_ms = e_ms;
    s->rate = e_state == ENG_PLAY ? rate_of(e_cur) : 0; s->bits = 16; s->channels = 2;
    snprintf(s->file, sizeof s->file, "%s", e_cur);
    return 0;
}
/* the current song ends: MPD moves on to the next slot, or stops */
static void e_song_ends(void){
    if(e_next_id >= 0){ memcpy(e_cur, e_next, sizeof e_cur); e_cur_id = e_next_id; e_off = e_next_off; e_len = e_next_len; e_next[0] = 0; e_next_id = -1; e_advanced_flag = 1; }
    else e_state = ENG_STOP;
    e_ms = 0;
    pb_engine_event();
}
int eng_cmd(const char *c, char *out, unsigned cap){            /* "stop" and "clear", as the radio uses them */
    if(cap) out[0] = 0;
    if(!strcmp(c, "stop") || !strcmp(c, "clear")){ e_state = ENG_STOP; e_armed = 0; }
    if(!strcmp(c, "clear")){ e_cur[0] = e_next[0] = 0; e_cur_id = e_next_id = -1; }
    return 0;
}
static int e_out_bt = 0, e_out_aux = 1, e_outputs_calls; static unsigned e_out_rate;
int eng_output(int aux, int bt, unsigned rate){ e_out_aux = aux; e_out_bt = bt; e_out_rate = rate; e_outputs_calls++; return 0; }
static int e_spdif;
void eng_set_spdif(int on){ e_spdif = on; }
static unsigned bt_link_rate = 48000; static int bt_vol_set = -1; static char bt_codec_asked[8], bt_sel[16] = "SBC";
static int bt_codec_started = -1;
int bto_info(const char *mac, unsigned *rate, char *sel, size_t selcap, char *avail, size_t availcap, bto_log_fn log){
    (void)mac; (void)log;
    if(rate) *rate = bt_link_rate;
    if(sel) snprintf(sel, selcap, "%s", bt_sel);
    if(avail) snprintf(avail, availcap, "SBC AAC LDAC");
    return 0;
}
const char *bto_codec_name(int v){ return v == 1 ? "AAC" : v == 2 ? "LDAC" : "SBC"; }
int bto_codec_start(const char *mac, int value, bto_log_fn log){   /* a child that is done at once, like a quick switch */
    (void)mac; (void)log;
    bt_codec_started = value;
    pid_t p = fork();
    if(p == 0) _exit(0);
    return (int)p;
}
int bto_volume(const char *mac, int v, int max, bto_log_fn log){ (void)mac; (void)max; (void)log; bt_vol_set = v; return 0; }
void bto_codec(const char *mac, int value, bto_log_fn log){ (void)mac; (void)log; snprintf(bt_codec_asked, sizeof bt_codec_asked, "%d", value); }
static char mix_last[32]; static unsigned mix_last_v;
int mix_set_enum(const char *dev, const char *name, unsigned value, void (*log)(const char *, ...)){
    (void)dev; (void)log; snprintf(mix_last, sizeof mix_last, "%s", name); mix_last_v = value; return 0;
}
static unsigned d_rate; static int d_starts;
unsigned dac_current_rate(void){ return d_rate; }
void dac_attenuation(int v, int g, int bl, int b, unsigned char *a, unsigned char *c){ (void)v; (void)g; (void)bl; (void)b; *a = *c = 0; }
int dac_start(unsigned r, unsigned char a, unsigned char c, void (*l)(const char *, ...)){ (void)a; (void)c; (void)l; d_rate = r; d_starts++; return 0; }

static double mono_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }

/* ---- frames to the UI ---- */
static char sent[64][2048]; static int nsent;
static void cap_send(void *ctx, const char *f, int len){ (void)ctx; if(nsent < 64){ snprintf(sent[nsent], sizeof sent[0], "%.*s", len, f); nsent++; } }
static void quiet(const char *fmt, ...){ (void)fmt; }

static int sent_count(const char *s){ int n = 0; for(int i = 0; i < nsent; i++) if(strstr(sent[i], s)) n++; return n; }
static char dbp[256], sysp[256];
static void sql(sqlite3 *d, const char *q){
    char *e = NULL;
    if(sqlite3_exec(d, q, NULL, NULL, &e) != SQLITE_OK){ fprintf(stderr, "sql: %s: %s\n", q, e); fails++; }
    sqlite3_free(e);
}
static int count(sqlite3 *d, const char *q){
    sqlite3_stmt *st; int n = -1;
    if(sqlite3_prepare_v2(d, q, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}
/* the songs' file names (SONG.IDs in list order), joined by ',' */
static const char *names_of(sqlite3 *d, const int *ids, int n){
    static char out[1024]; out[0] = 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM SONG WHERE ID=?1;", -1, &st, NULL) != SQLITE_OK) return "?";
    for(int i = 0; i < n; i++){
        sqlite3_bind_int(st, 1, ids[i]);
        if(sqlite3_step(st) == SQLITE_ROW){
            const char *p = (const char *)sqlite3_column_text(st, 0), *b = strrchr(p, '/');
            snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s", out[0] ? "," : "", b ? b + 1 : p);
        }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return out;
}
/* the list the player published (OPL_LIST): n, shuffled, the SONG.IDs and the play order; -1 when unreadable */
static int pub_n, pub_sh, pub_songs[64], pub_order[64];
static int read_pub(void){
    FILE *f = fopen(OPL_LIST, "r");
    if(!f) return -1;
    int ok = fscanf(f, "diskos-list 1 %d %d", &pub_n, &pub_sh) == 2 && pub_n >= 0 && pub_n <= 64;
    for(int i = 0; ok && i < pub_n; i++) ok = fscanf(f, "%d", &pub_songs[i]) == 1;
    for(int i = 0; ok && pub_sh && i < pub_n; i++) ok = fscanf(f, "%d", &pub_order[i]) == 1;
    fclose(f);
    return ok ? 0 : -1;
}
static int *L_ids; static int L_n;
static int list_of(sqlite3 *d, int type, const char *name, int cls, char *err, int errcap){
    free(L_ids); L_ids = NULL;
    L_n = qd_list(d, type, name, cls, &L_ids, NULL, err, errcap);
    return L_n;
}
#define queue_names(d) names_of(d, L_ids, L_n)

static void make_db(void){
    snprintf(dbp, sizeof dbp, "/tmp/pbtest_%d_song.db", (int)getpid());
    snprintf(sysp, sizeof sysp, "/tmp/pbtest_%d_sys.db", (int)getpid());
    unlink(dbp); unlink(sysp);
    sqlite3 *d; CHECK(sqlite3_open(dbp, &d) == SQLITE_OK);
    /* SONG as ui/scanner.c creates it */
    sql(d, "CREATE TABLE SONG (ID INTEGER PRIMARY KEY autoincrement, PATH TEXT, NAME TEXT,TITLE TEXT, ALBUM TEXT, ARTIST TEXT, "
           "GENRE TEXT, DISC INT, TRACK INT, IS_CUE INT, IS_ISO INT,IS_DSD INT, OFFSET BIGINT, DURATION BIGINT, NAME_CODE INT, "
           "TITLE_CODE INT, ALBUM_CODE INT,ARTIST_CODE INT, GENRE_CODE INT, ADD_TIME INT8, SAMPLE_RATE INT, BIT_PER_SAMPLE INT, "
           "CHANNELS INT,BIT_RATE INT, SONG_MIMETYPE TEXT, SONG_PRODUCTION_YEAR TEXT, IS_SELECT INT, ALBUM_ARTIST TEXT,"
           "ALBUM_ARTIST_CODE INT, IS_M3U INT DEFAULT 0, M3U_PATH TEXT DEFAULT '', ACCENT INTEGER DEFAULT 0);");
    /* MY_LOVE of an older firmware: no IS_M3U / M3U_PATH */
    sql(d, "CREATE TABLE MY_LOVE (ID INTEGER PRIMARY KEY autoincrement, PATH TEXT, NAME TEXT, TITLE TEXT, ALBUM TEXT, ARTIST TEXT, "
           "GENRE TEXT, DISC INT, TRACK INT, IS_CUE INT, IS_ISO INT, IS_DSD INT, OFFSET BIGINT, DURATION BIGINT, NAME_CODE INT, "
           "TITLE_CODE INT, ALBUM_CODE INT, ARTIST_CODE INT, GENRE_CODE INT, ADD_TIME INT8, ALBUM_ARTIST TEXT, UNIQUE(PATH,TRACK));");
    sql(d, "CREATE TABLE CUSTOM_PLAYLIST_INDEX (LIST_ID INTEGER PRIMARY KEY, LIST_NAME TEXT, M3U_PATH TEXT);");
    sql(d, "CREATE TABLE CUSTOM_PLAYLIST (ID INTEGER PRIMARY KEY autoincrement, PLAYLIST_ID INT, PATH TEXT, NAME TEXT, TITLE TEXT, "
           "ALBUM TEXT, ARTIST TEXT, GENRE TEXT, DISC INT, TRACK INT, IS_CUE INT, IS_ISO INT, IS_DSD INT, OFFSET BIGINT, "
           "DURATION BIGINT, ALBUM_ARTIST TEXT, IS_M3U INT, M3U_PATH TEXT);");
    /* PATH, TITLE, TITLE_CODE, ALBUM, ARTIST, ALBUM_ARTIST, GENRE, DISC, TRACK */
    sql(d, "INSERT INTO SONG (PATH,NAME,TITLE,TITLE_CODE,NAME_CODE,ALBUM,ARTIST,ALBUM_ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,DURATION,ADD_TIME) VALUES"
           "('/s/a2.flac','a2.flac','Beta',  20,0,'Alpha','Ann','Ann','Rock',1,2,0,0,200000,5),"
           "('/s/a1.flac','a1.flac','Zulu',  90,0,'Alpha','Ann','Ann','Rock',1,1,0,0,100000,5),"
           "('/s/b1.flac','b1.flac','Gamma', 30,0,'Bravo','Bob','Various','Jazz',0,0,0,0,150000,6),"
           "('/s/b2.flac','b2.flac','Delta', 40,0,'Bravo','Ann','Various','Jazz',1,3,0,0,150000,6),"
           "('/s/c.flac', 'c.flac', 'Echo',  50,0,'Alpha','Cid','Cid','Rock',2,1,0,0,150000,7),"
           "('/s/cue.flac','cue.flac','Cue 1',10,0,'Cue','Cid','Cid','Rock',1,1,1,0,60000,8),"
           "('/s/m1.flac','m1.flac','M1',60,0,'Mixed','Mo','Mo','Pop',1,1,0,0,1000,9),"
           "('/s/m2-96k.flac','m2-96k.flac','M2',61,0,'Mixed','Mo','Mo','Pop',1,2,0,0,1000,9),"
           "('/s/m3-96k.flac','m3-96k.flac','M3',62,0,'Mixed','Mo','Mo','Pop',1,3,0,0,1000,9);");
    sql(d, "INSERT INTO MY_LOVE (PATH,NAME,TITLE,TITLE_CODE,NAME_CODE,ALBUM,ARTIST,TRACK,IS_CUE,IS_ISO,DURATION) VALUES"
           "('/s/c.flac','c.flac','Echo',50,0,'Alpha','Cid',1,0,0,150000),('/s/b1.flac','b1.flac','Gamma',30,0,'Bravo','Bob',0,0,0,150000);");
    /* two playlists; diskOS plays the one with the lowest LIST_ID (seq 0) */
    sql(d, "INSERT INTO CUSTOM_PLAYLIST_INDEX VALUES (5,'Later',''),(2,'Slot','');"
           "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,TITLE,TRACK,IS_CUE,IS_ISO,DURATION) VALUES"
           "(2,'/s/c.flac','Echo',1,0,0,1),(5,'/s/a1.flac','Zulu',1,0,0,1),(2,'/s/a1.flac','Zulu',1,0,0,1);");
    /* MEMORY_PLAY's columns as on a Disc (docs/PLAYER_PROTOCOL.md); empty, the player adds the stock default row */
    sql(d, "CREATE TABLE MEMORY_PLAY (ID INTEGER PRIMARY KEY autoincrement, MUSIC_ID INT, IS_PLAYING INT, POSITION INT, "
           "IS_CUE INT, IS_ISO INT, TRACK INT, IS_NAS INT, IS_M3U INT);");
    sqlite3_close(d);
    CHECK(sqlite3_open(sysp, &d) == SQLITE_OK);
    sql(d, "CREATE TABLE SYSCONFIG (ID INTEGER PRIMARY KEY, ARTIST_CLASS_TYPE INT); INSERT INTO SYSCONFIG VALUES (1, 0);");
    sqlite3_close(d);
}

static unsigned rnd_fixed(unsigned k){ return k - 1; }

static void test_queue(void){
    pq_t q = { 0, NULL, NULL, -1, 0 };
    int *b = malloc(5 * sizeof *b); for(int i = 0; i < 5; i++) b[i] = i + 1;
    CHECK(pq_set(&q, b, 5, 2) == 0 && q.cur == 2 && pq_id(&q, q.cur) == 3);
    CHECK(pq_next(&q, PQ_IN_ORDER, 1) == 3 && pq_next(&q, PQ_REPEAT_ONE, 1) == 2 && pq_next(&q, PQ_SINGLE, 1) == -1);
    q.cur = 4;
    CHECK(pq_next(&q, PQ_IN_ORDER, 1) == -1 && pq_next(&q, PQ_REPEAT_ALL, 1) == 0 && pq_next(&q, PQ_IN_ORDER, 0) == 0);
    CHECK(pq_next(&q, PQ_REPEAT_ONE, 0) == 0);                   /* the Next key moves on also in repeat one */
    q.cur = 0; CHECK(pq_prev(&q) == 4);
    q.cur = 2;
    CHECK(pq_shuffle(&q, 1, rnd_fixed) == 0 && q.shuffled && q.cur == 0 && q.order[0] == 3);
    int seen[6] = { 0 }; for(int i = 0; i < 5; i++) seen[q.order[i]]++;
    CHECK(seen[1] == 1 && seen[2] == 1 && seen[3] == 1 && seen[4] == 1 && seen[5] == 1);
    CHECK(pq_from_base(&q, 2) == 0 && pq_find(&q, 3) == 0);
    q.cur = 3; int id = pq_id(&q, 3);
    CHECK(pq_shuffle(&q, 0, rnd_fixed) == 0 && !q.shuffled && pq_id(&q, q.cur) == id);   /* off: same song stays */
    int *o = malloc(5 * sizeof *o); o[0] = 5; o[1] = 4; o[2] = 3; o[3] = 2; o[4] = 1;
    CHECK(pq_adopt_order(&q, o, 5) == 0 && q.shuffled && pq_id(&q, q.cur) == id);
    int *bad = malloc(5 * sizeof *bad); bad[0] = 1; bad[1] = 1; bad[2] = 3; bad[3] = 4; bad[4] = 5;
    CHECK(pq_adopt_order(&q, bad, 5) == -1 && q.order[0] == 5);  /* not a permutation: rejected */
    pq_free(&q);
    CHECK(q.n == 0 && q.cur == -1);
}

static void test_queuedb(void){
    sqlite3 *d = qd_open(dbp);
    CHECK(d != NULL);
    char err[200];
    /* all songs: title code order, the CUE row last (as ui/musicdb.c mdb_play_pos counts) */
    CHECK(list_of(d, QD_ALL, "", 0, err, sizeof err) == 9);
    CHECK(!strcmp(queue_names(d), "a2.flac,b1.flac,b2.flac,c.flac,m1.flac,m2-96k.flac,m3-96k.flac,a1.flac,cue.flac"));
    CHECK(list_of(d, QD_ALBUM, "Alpha", 0, err, sizeof err) == 3);
    CHECK(!strcmp(queue_names(d), "a1.flac,a2.flac,c.flac"));      /* album order: disc, track */
    CHECK(list_of(d, QD_ARTIST, "Ann", 0, err, sizeof err) == 3);
    CHECK(!strcmp(queue_names(d), "a2.flac,b2.flac,a1.flac"));
    CHECK(list_of(d, QD_ARTIST, "Various", 1, err, sizeof err) == 2);   /* album artist grouping */
    CHECK(!strcmp(queue_names(d), "b1.flac,b2.flac"));
    CHECK(list_of(d, QD_GENRE, "Jazz", 0, err, sizeof err) == 2);
    CHECK(list_of(d, QD_ARTIST_ALBUM, "{\"artist\":\"Various\", \"album\":\"Bravo\"}", 1, err, sizeof err) == 2);
    CHECK(!strcmp(queue_names(d), "b2.flac,b1.flac"));               /* disc 0 after disc 1 */
    CHECK(list_of(d, QD_ARTIST_ALBUM, "{\"artist\":\"Ann\", \"album\":\"Bravo\"}", 0, err, sizeof err) == 1);
    CHECK(list_of(d, QD_GENRE_ALBUM, "{\"style\":\"Rock\", \"album\":\"Alpha\"}", 0, err, sizeof err) == 3);
    CHECK(list_of(d, QD_ARTIST_ALBUM, "{\"album\":\"x\"}", 0, err, sizeof err) == -1 && strstr(err, "payload"));
    CHECK(list_of(d, QD_FAVOURITES, "", 0, err, sizeof err) == 2);   /* MY_LOVE without the M3U columns */
    CHECK(!strcmp(queue_names(d), "b1.flac,c.flac"));
    CHECK(list_of(d, QD_PLAYLIST, "", 0, err, sizeof err) == 2);     /* seq 0 = LIST_ID 2, in row order */
    CHECK(!strcmp(queue_names(d), "c.flac,a1.flac"));
    CHECK(list_of(d, QD_PLAYLIST, "1", 0, err, sizeof err) == 1);
    CHECK(list_of(d, 4, "", 0, err, sizeof err) == -1);
    CHECK(list_of(d, QD_ALBUM, "it's", 0, err, sizeof err) == 0);    /* a quote is bound, not interpolated */

    /* nothing is written: no list tables (the stock player's LIST_SONG_0/3) */
    CHECK(count(d, "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'LIST_SONG_%';") == 0);
    /* a favourite whose song left the library is left out */
    sql(d, "INSERT INTO MY_LOVE (PATH, TITLE, TITLE_CODE, NAME_CODE) VALUES ('/s/gone.flac', 'Gone', 'Gone', 'Gone');");
    int dropped = 0; free(L_ids); L_ids = NULL;
    CHECK((L_n = qd_list(d, QD_FAVOURITES, "", 0, &L_ids, &dropped, err, sizeof err)) == 2 && dropped == 1);
    sql(d, "DELETE FROM MY_LOVE WHERE PATH='/s/gone.flac';");
    free(L_ids); L_ids = NULL; L_n = 0;

    qd_row_t r;
    CHECK(qd_song(d, 5, &r) == 1 && !strcmp(r.path, "/s/c.flac") && r.song_id == 5 && r.love && r.duration_ms == 150000 && r.id == 0);
    CHECK(qd_song(d, 1, &r) == 1 && !strcmp(r.title, "Beta") && r.song_id == 1 && !r.love);
    CHECK(qd_song(d, 999, &r) == 0);
    sqlite3_close(d);
    CHECK(qd_artist_class(sysp) == 0 && qd_artist_class("/nonexistent/sys.db") == -1);
}

static int love_rows(sqlite3 *d, const char *path){
    sqlite3_stmt *st; int n = -1;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM MY_LOVE WHERE PATH=?1 AND TITLE IS NOT NULL;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    if(sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* The orderings are copies of ui/musicdb.c's: fail when the two drift apart. */
static void squash(const char *in, char *out, size_t cap){
    size_t o = 0;
    for(; *in && o + 1 < cap; in++) if(*in != ' ' && *in != '"' && *in != '\n' && *in != '\\') out[o++] = *in;
    out[o] = 0;
}
static void test_orders_in_step(void){
    FILE *f = fopen("../musicdb.c", "r");
    CHECK(f != NULL);
    if(!f) return;
    static char src[1 << 22], flat[1 << 22], want[2048];
    size_t n = fread(src, 1, sizeof src - 1, f); src[n] = 0; fclose(f);
    squash(src, flat, sizeof flat);
    squash("CASE WHEN IS_CUE=0 AND IS_ISO=0 THEN 1 WHEN IS_CUE=1 OR IS_ISO=1 THEN 2 END,"
           "CASE WHEN (IS_CUE=0 AND IS_ISO=0) THEN CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END END,"
           "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN NAME_CODE END,CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN ID END,"
           "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN TRACK END", want, sizeof want);
    CHECK(strstr(flat, want) != NULL);
    squash("CASE WHEN DISC=0 THEN 1 ELSE 0 END,DISC,CASE WHEN TRACK=0 THEN 1 ELSE 0 END,TRACK,"
           "CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END", want, sizeof want);
    CHECK(strstr(flat, want) != NULL);
}

static int core_transport(void *ctx, int op, long arg){ (void)ctx; return pb_transport(op, arg); }   /* as main.c */
static void test_playback(void){
    pl_core_t c;
    pl_core_init(&c, cap_send, NULL, NULL);
    c.transport = core_transport;
    pb_init(&c, dbp, sysp, quiet);
    /* album Alpha from its 2nd song (0-based position 1) */
    nsent = 0;
    CHECK(pb_play_list(QD_ALBUM, 1, "Alpha") == 0);
    CHECK(!strcmp(e_cur, "/s/a2.flac") && !strcmp(e_next, "/s/c.flac") && e_state == ENG_PLAY);
    pb_engine_event();                                        /* MPD: player started */
    if(getenv("PB_DUMP")) puts(sent[2]);
    CHECK(nsent == 7 && !strcmp(sent[0], "a102000C0000") && !strcmp(sent[1], "aa1c000C0000"));
    CHECK(!strncmp(sent[2], "a202", 4) && strstr(sent[2], "\\\"pos_id\\\":2,") && strstr(sent[2], "\\\"id\\\":1,"));
    CHECK(strstr(sent[2], "\\\"song_file_path\\\":\\\"/s/a2.flac\\\"") && strstr(sent[2], "\"playing_num\":\"2/3\""));
    CHECK(strstr(sent[2], "\\\"song_bit_rate\\\":1411,") && strstr(sent[2], "\"state\":2"));
    CHECK(!strcmp(sent[3], "aa05000C0001") && !strcmp(sent[5], "a2020013{\"state\":0}") && !strcmp(sent[6], sent[5]));
    /* natural track change: pos_id 3, then the end of the album in order mode: nothing queued */
    nsent = 0;
    e_song_ends();
    CHECK(!strcmp(e_cur, "/s/c.flac") && e_next[0] == 0);
    CHECK(nsent == 8 && !strcmp(sent[0], "a2020013{\"state\":1}") && strstr(sent[3], "\\\"pos_id\\\":3,") && strstr(sent[3], "\"playing_num\":\"3/3\""));
    nsent = 0;
    e_song_ends();                                            /* the last song ends: stopped, the UI hears "paused" */
    CHECK(e_state == ENG_STOP && nsent == 2 && !strcmp(sent[0], "a2020013{\"state\":1}"));
    /* Next wraps around; repeat all queues the first song after the last */
    c.play_mode = PQ_REPEAT_ALL; pb_mode_changed();
    CHECK(pb_transport(PL_T_NEXT, 0) == 0 && !strcmp(e_cur, "/s/a1.flac") && !strcmp(e_next, "/s/a2.flac"));
    CHECK(pb_transport(PL_T_PREV, 0) == 0 && !strcmp(e_cur, "/s/c.flac") && !strcmp(e_next, "/s/a1.flac"));
    c.play_mode = PQ_REPEAT_ONE; pb_mode_changed();
    CHECK(!strcmp(e_next, "/s/c.flac"));                      /* the same song again */
    c.play_mode = PQ_SINGLE; pb_mode_changed();
    CHECK(e_next[0] == 0);
    /* the list is published to OPL_LIST (RAM), never written to song.db: album Alpha = SONG.IDs 4 (a1), 1 (a2), 5 (c) */
    sqlite3 *d = qd_open(dbp);
    CHECK(read_pub() == 0 && pub_n == 3 && !pub_sh && !strcmp(names_of(d, pub_songs, pub_n), "a1.flac,a2.flac,c.flac"));
    CHECK(count(d, "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'LIST_SONG_%';") == 0);
    /* shuffle: the play order is published, an exact permutation of the positions with the current song first */
    c.play_mode = PQ_SHUFFLE; pb_mode_changed();
    CHECK(read_pub() == 0 && pub_n == 3 && pub_sh && pub_order[0] == 3 && pub_order[0] + pub_order[1] + pub_order[2] == 6);
    CHECK(e_next[0] != 0);
    c.play_mode = PQ_IN_ORDER; pb_mode_changed();
    CHECK(read_pub() == 0 && pub_n == 3 && !pub_sh);
    /* a play list in shuffle: the tapped song plays first, the play order starts with it */
    c.play_mode = PQ_SHUFFLE;
    CHECK(pb_play_list(QD_ALL, 3, "") == 0 && !strcmp(e_cur, "/s/c.flac"));
    CHECK(read_pub() == 0 && pub_n == 9 && pub_sh && pub_order[0] == 4);
    CHECK(count(d, "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'LIST_SONG_%';") == 0);
    /* type 0 jumps by list position, also in shuffle */
    CHECK(pb_play_list(QD_RESUME, 0, "") == 0 && !strcmp(e_cur, "/s/a2.flac"));
    c.play_mode = PQ_IN_ORDER; pb_mode_changed();
    /* a CUE track plays the part of its file from OFFSET for DURATION; a whole file has no part */
    sql(d, "UPDATE SONG SET OFFSET=30000 WHERE PATH='/s/cue.flac';");
    CHECK(pb_play_list(QD_ALL, 8, "") == 0 && !strcmp(e_cur, "/s/cue.flac") && e_off == 30000 && e_len == 60000);
    CHECK(e_next[0] == 0);                                    /* the last song, in order: nothing after it */
    CHECK(pb_play_list(QD_ALL, 7, "") == 0 && e_off == 0 && e_len == 0 && !strcmp(e_next, "/s/cue.flac") && e_next_off == 30000);
    /* a SACD ISO track is skipped (MPD has no decoder for disc images); a missing file is skipped too */
    sql(d, "UPDATE SONG SET IS_CUE=0, IS_ISO=1 WHERE PATH='/s/cue.flac';");
    CHECK(pb_play_list(QD_ALL, 8, "") == 0 && !strcmp(e_cur, "/s/a2.flac"));   /* wrapped past the ISO row */
    sql(d, "UPDATE SONG SET IS_CUE=1, IS_ISO=0 WHERE PATH='/s/cue.flac';");
    snprintf(e_missing, sizeof e_missing, "/s/b1.flac");
    CHECK(pb_play_list(QD_ALL, 0, "") == 0 && !strcmp(e_next, "/s/b2.flac"));
    CHECK(pb_play_list(QD_ALL, 1, "") == 0 && !strcmp(e_cur, "/s/b2.flac"));
    e_missing[0] = 0;
    /* a fresh player has no list (RAM only): type 0 has nothing to jump in; the published list is empty */
    pb_close();
    CHECK(pb_play_list(QD_RESUME, 2, "") == -1);
    pb_init(&c, dbp, sysp, quiet);
    CHECK(read_pub() == 0 && pub_n == 0);
    CHECK(pb_play_list(QD_ALL, 1, "") == 0 && !strcmp(e_cur, "/s/b1.flac"));
    /* 0202 while a track is loaded: the full a2 with the real state */
    nsent = 0; pb_engine_event(); nsent = 0;
    CHECK(pb_transport(PL_T_STATE, 0) == 0 && nsent == 1 && strstr(sent[0], "\"state\":0"));
    /* 0104, the heart: into MY_LOVE (copied from SONG), the a2 says so; twice is one row; out again */
    nsent = 0;
    CHECK(pl_core_handle(&c, "0104000C0001", 12) == 0 && nsent == 1 && strstr(sent[0], "\"love\":true"));
    CHECK(pb_transport(PL_T_LOVE, 1) == 0 && love_rows(d, "/s/b1.flac") == 1);
    nsent = 0;
    CHECK(pl_core_handle(&c, "0104000C0000", 12) == 0 && nsent == 1 && strstr(sent[0], "\"love\":false") && love_rows(d, "/s/b1.flac") == 0);
    CHECK(pl_core_handle(&c, "0104000C0002", 12) == -1);
    /* the Artist grouping comes from 0648 and is written to SYSCONFIG, as the UI predicts the queue from there */
    c.artist_grouping = 1;
    CHECK(pb_play_list(QD_ARTIST, 0, "Various") == 0 && qd_artist_class(sysp) == 1);
    CHECK(read_pub() == 0 && !strcmp(names_of(d, pub_songs, pub_n), "b1.flac,b2.flac"));
    /* a song at another sample rate is not queued in MPD: it starts after the current one ends, with the DAC
     * started for its rate before MPD opens the PCM; the same rate queues gaplessly again */
    c.play_mode = PQ_IN_ORDER; pb_mode_changed();
    int starts = d_starts;
    CHECK(pb_play_list(QD_ALBUM, 0, "Mixed") == 0 && !strcmp(e_cur, "/s/m1.flac") && e_next[0] == 0 && d_rate == 44100);
    pb_engine_event();
    nsent = 0;
    e_song_ends();
    CHECK(!strcmp(e_cur, "/s/m2-96k.flac") && d_rate == 96000 && d_starts > starts && !strcmp(e_next, "/s/m3-96k.flac"));
    pb_engine_event();
    CHECK(sent_count("{\"state\":1}") >= 1 && sent_count("\\\"pos_id\\\":2,") == 1);
    /* Bluetooth: 0666 2 waits for the headphones' address (06b3); then MPD plays to the pipe output for the link's
     * rate, the DAC is left alone, and the volume goes to the headphones; back to the DAC on 0666 6 */
    mkdir(ENG_RUN_DIR, 0755);
    c.route = PL_ROUTE_BT; c.out_bt = 1; c.out_aux = 0; c.bt_mac[0] = 0; pb_route();
    CHECK(!pb_bluetooth() && e_out_bt == 0 && e_out_aux == 1);
    snprintf(c.bt_mac, sizeof c.bt_mac, "AA:BB:CC:01:02:03"); c.bt_codec = 1; c.vol_bt = 60; pb_route();
    CHECK(pb_bluetooth() && !pb_aux() && e_out_bt == 1 && e_out_aux == 0 && e_out_rate == 48000 && bt_vol_set == 60
          && !strcmp(bt_codec_asked, "1") && !strcmp(pb_bt_codecs(), "SBC AAC LDAC"));
    FILE *bf = fopen(ENG_BT_DEV, "r"); char bl[32] = ""; if(bf){ if(!fgets(bl, sizeof bl, bf)) bl[0] = 0; fclose(bf); }
    CHECK(!strcmp(bl, "AA:BB:CC:01:02:03\n"));
    starts = d_starts;
    CHECK(pb_play_list(QD_ALBUM, 1, "Mixed") == 0 && d_starts == starts);   /* a 96 kHz song: no DAC start */
    bt_codec_asked[0] = 0; pb_route();
    CHECK(bt_codec_asked[0] == 0);                            /* the same codec is not asked again */
    /* both outputs: the DAC joins the running stream with its start sequence (the 96 kHz song plays) */
    e_state = ENG_PLAY; pb_engine_event();
    starts = d_starts; d_rate = 0;
    c.out_aux = 1; pb_route();
    CHECK(pb_aux() && pb_bluetooth() && e_out_aux == 1 && e_out_bt == 1 && d_rate == 96000 && d_starts == starts + 1);
    /* a codec by hand: paused, the Bluetooth output closed, BlueALSA asked; then reopened at the new rate, playing */
    pb_codec(1);
    CHECK(bt_codec_started == 1 && pb_switching() && e_state == ENG_PAUSE && e_out_bt == 0 && e_out_aux == 1);
    bt_link_rate = 44100; snprintf(bt_sel, sizeof bt_sel, "AAC");
    for(int i = 0; i < 200 && pb_switching(); i++){ usleep(10000); pb_tick(mono_now()); }
    CHECK(!pb_switching() && e_out_bt == 1 && e_out_rate == 44100 && e_state == ENG_PLAY && !strcmp(pb_bt_codec(), "AAC"));
    bt_codec_started = -1; pb_codec(1);
    CHECK(bt_codec_started == -1);                            /* already AAC: nothing to do */
    c.route = PL_ROUTE_DAC; c.out_bt = 0; pb_route();
    CHECK(!pb_bluetooth() && e_out_bt == 0 && e_out_aux == 1);
    /* S/PDIF (0666 4): AUX plays on hw:0,4 with LO2_MUX = 13, the DAC is not started */
    CHECK(pl_core_handle(&c, "0666000C0004", 12) == 0 && c.spdif == 1);
    starts = d_starts; pb_route();
    CHECK(e_spdif == 1 && e_out_aux == 1 && !strcmp(mix_last, "LO2_MUX") && mix_last_v == 13 && d_starts == starts);
    CHECK(pb_play_list(QD_ALBUM, 1, "Alpha") == 0 && d_starts == starts && !strcmp(mix_last, "LO2_MUX"));
    CHECK(pl_core_handle(&c, "0666000C0006", 12) == 0 && c.spdif == 0);
    c.route = PL_ROUTE_DAC; c.out_bt = 0; pb_route();
    CHECK(e_spdif == 0 && e_out_aux == 1);
    CHECK(pb_play_list(QD_ALBUM, 1, "Alpha") == 0 && !strcmp(mix_last, "LO3_MUX") && mix_last_v == 12);
    /* headphones pulled */
    e_state = ENG_PLAY; pb_engine_event();
    CHECK(pb_transport(PL_T_PAUSE, 0) == 0 && e_state == ENG_PAUSE);
    sqlite3_close(d);
    pb_close();
}

/* ---- resume (MEMORY_PLAY) ---- */
static int mounted_now;
static int is_mounted(void){ return mounted_now; }
static double mono(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
static char flagp[256];
/* a new player start with resume mode m; the card is mounted from the start; ticks until the resume is due */
static void restart_and_resume(pl_core_t *c, int m){
    pb_close();
    e_state = ENG_STOP; e_cur[0] = 0; e_cur_id = -1; e_ms = -1;
    pl_core_init(c, cap_send, NULL, NULL);
    c->resume_mode = m;
    pb_init(c, dbp, sysp, quiet);
    mounted_now = 1;
    pb_resume_begin(is_mounted, flagp);
    double t = mono();
    pb_tick(t); pb_tick(t + 0.5); pb_tick(t + 1.2);
}
static void test_resume(void){
    snprintf(flagp, sizeof flagp, "/tmp/pbtest_%d_flag", (int)getpid());
    unlink(flagp);
    pl_core_t c;
    pl_core_init(&c, cap_send, NULL, NULL);
    pb_init(&c, dbp, sysp, quiet);
    sqlite3 *d = qd_open(dbp);
    /* every track start writes the bookmark (MUSIC_ID = SONG.ID, as the UI reads it); a pause stores the position */
    CHECK(pb_play_list(QD_ALBUM, 1, "Mixed") == 0 && !strcmp(e_cur, "/s/m2-96k.flac"));
    pb_engine_event();
    CHECK(count(d, "SELECT MUSIC_ID FROM MEMORY_PLAY WHERE ID=1;") == 8 && count(d, "SELECT POSITION FROM MEMORY_PLAY;") == 0);
    CHECK(count(d, "SELECT TRACK FROM MEMORY_PLAY;") == 2 && count(d, "SELECT IS_PLAYING FROM MEMORY_PLAY;") == 1);
    e_ms = 42000; e_state = ENG_PAUSE; pb_engine_event();
    CHECK(count(d, "SELECT POSITION FROM MEMORY_PLAY;") == 42000);
    /* mode 1 (position): the same song from the same spot, on its own (the list is RAM only), playing, guarded by the
     * flag until it has played 10 s */
    restart_and_resume(&c, 1);
    CHECK(!strcmp(e_cur, "/s/m2-96k.flac") && e_ms == 42000 && e_state == ENG_PLAY && e_next[0] == 0);   /* on its own: no list over a restart */
    CHECK(access(flagp, F_OK) == 0);
    pb_engine_event();
    pb_tick(mono() + 11.0);
    CHECK(access(flagp, F_OK) != 0);
    /* power off: the position goes into the bookmark */
    e_ms = 50000; pb_engine_event(); pb_shutdown();
    CHECK(count(d, "SELECT POSITION FROM MEMORY_PLAY;") >= 50000);
    /* mode 2 (song): from its start; mode 0: nothing */
    restart_and_resume(&c, 2);
    CHECK(!strcmp(e_cur, "/s/m2-96k.flac") && e_ms == 0);
    unlink(flagp);
    restart_and_resume(&c, 0);
    CHECK(e_cur[0] == 0);
    /* no card within the wait: the resume waits (here it never comes) */
    pb_close(); e_cur[0] = 0; pl_core_init(&c, cap_send, NULL, NULL); c.resume_mode = 1; pb_init(&c, dbp, sysp, quiet);
    mounted_now = 0; pb_resume_begin(is_mounted, flagp);
    double t = mono(); pb_tick(t); pb_tick(t + 2.0);
    CHECK(e_cur[0] == 0);
    mounted_now = 1; pb_tick(t + 2.5); pb_tick(t + 3.6);       /* the card comes: resumed a second later */
    CHECK(!strcmp(e_cur, "/s/m2-96k.flac"));
    unlink(flagp);
    /* a song played before the card was there: no resume */
    pb_close(); pl_core_init(&c, cap_send, NULL, NULL); c.resume_mode = 1; pb_init(&c, dbp, sysp, quiet);
    mounted_now = 0; pb_resume_begin(is_mounted, flagp);
    CHECK(pb_play_list(QD_ALBUM, 0, "Bravo") == 0 && !strcmp(e_cur, "/s/b2.flac"));   /* album order: disc 0 (unknown) last */
    mounted_now = 1; t = mono(); pb_tick(t); pb_tick(t + 1.2);
    CHECK(!strcmp(e_cur, "/s/b2.flac") && access(flagp, F_OK) != 0);
    /* the last resume crashed (the flag is still there): skipped, the bookmark is cleared */
    sql(d, "UPDATE MEMORY_PLAY SET MUSIC_ID=5, POSITION=1000;");
    FILE *f = fopen(flagp, "w"); CHECK(f != NULL); if(f) fclose(f);
    restart_and_resume(&c, 1);
    CHECK(e_cur[0] == 0 && access(flagp, F_OK) != 0 && count(d, "SELECT MUSIC_ID FROM MEMORY_PLAY;") == 0);
    /* any library song resumes, on its own (there is no list after a restart) */
    sql(d, "UPDATE MEMORY_PLAY SET MUSIC_ID=5, POSITION=1000;");   /* c.flac */
    restart_and_resume(&c, 1);
    CHECK(!strcmp(e_cur, "/s/c.flac") && e_ms == 1000);
    unlink(flagp);
    sqlite3_close(d);
    pb_close();
    unlink(flagp);
}

/* web radio: the Radio app's work mode (0657 0C) and its station (0d10, playback.c pb_stream_*) */
static int radio_cb(void *ctx, const char *name, const char *url){   /* as player/main.c core_radio */
    (void)ctx;
    pb_stream_mode(1);
    return url[0] ? pb_stream_start(name, url) : pb_stream_stop();
}
static int send_station(pl_core_t *c, const char *name, const char *url){
    char f[2048];
    int n = snprintf(f, sizeof f, "0d10%04X%04X%s%s", (unsigned)(12 + strlen(name) + strlen(url)), (unsigned)strlen(name), name, url);
    return pl_core_handle(c, f, n);
}
static void test_radio(void){
    pl_core_t c;
    pl_core_init(&c, cap_send, NULL, NULL);
    c.radio = radio_cb;
    pb_init(&c, dbp, sysp, quiet);
    CHECK(pb_play_list(QD_ALBUM, 0, "Alpha") == 0);           /* a local song plays */
    pb_engine_event();
    e_ms = 42000; pb_engine_event();
    /* not in the radio work mode: the station is refused */
    CHECK(send_station(&c, "Jazz FM", "http://radio.example/live") == -1 && !pb_stream_active());
    CHECK(pl_core_handle(&c, "0d10000A00", 10) == -1);        /* too short */
    CHECK(pl_core_handle(&c, "0d10000E0009abcdef", 18) == -1);   /* the name is longer than the frame */
    /* 0657 0C: the mode is taken and announced; the local song pauses */
    nsent = 0;
    CHECK(pl_core_handle(&c, "0657000C000C", 12) == 0 && c.work_mode == PL_WORK_DLNA && sent_count("a607000C000C") == 1);
    pb_stream_mode(1);
    CHECK(e_state == ENG_PAUSE && pb_stream_active());
    pb_engine_event();                                        /* MPD: paused */
    /* the Radio app's station: name and URL in one frame (ui/radio_core.c rc_station_frame) */
    nsent = 0;
    CHECK(send_station(&c, "Jazz & More", "http://radio.example/live?a=1&b=2") == 0);
    CHECK(!strcmp(pb_stream_uri(), "http://radio.example/live?a=1&b=2") && !strcmp(e_cur, "http://radio.example/live?a=1&b=2") && e_state == ENG_PLAY);
    CHECK(send_station(&c, "Bad", "ftp://x/y") == -1 && e_state == ENG_PLAY);   /* not a stream: the station plays on */
    pb_engine_event();                                        /* MPD plays the stream: its track frame and the state */
    if(getenv("PB_DUMP")) for(int i = 0; i < nsent; i++) puts(sent[i]);
    CHECK(sent_count("Jazz & More") == 1 && sent_count("\"work_mode\":\"DLNA\"") == 1 && sent_count("a2020013{\"state\":0}") >= 1);
    /* the song the station names (MPD's Title, ICY "Artist - Title") goes to OPL_STREAM for ui/radio.c */
    {
        char b[2048] = "", u[256], ar[64], ti[64];
        snprintf(e_title, sizeof e_title, "Some Band - Some\nSong"); pb_engine_event();
        FILE *f = fopen(OPL_STREAM, "r"); size_t n = f ? fread(b, 1, sizeof b - 1, f) : 0; if(f) fclose(f); b[n] = 0;
        CHECK(opl_stream_parse(b, u, sizeof u, ar, sizeof ar, ti, sizeof ti) == 0 && !strcmp(u, "http://radio.example/live?a=1&b=2")
              && ar[0] == 0 && !strcmp(ti, "Some Band - Some Song"));
        e_title[0] = 0;
    }
    /* the Play key pauses and plays on; skip and seek do nothing */
    CHECK(pb_transport(PL_T_TOGGLE, 0) == 0 && e_state == ENG_PAUSE);
    pb_engine_event();
    CHECK(pb_transport(PL_T_TOGGLE, 0) == 0 && e_state == ENG_PLAY && !strcmp(e_cur, "http://radio.example/live?a=1&b=2"));
    pb_engine_event();
    CHECK(pb_transport(PL_T_NEXT, 0) == -1 && pb_transport(PL_T_SEEK, 1000) == -1);
    /* another station replaces it */
    CHECK(send_station(&c, "Rock", "https://radio.example/rock") == 0 && !strcmp(e_cur, "https://radio.example/rock") && e_state == ENG_PLAY);
    /* stop: a frame with no URL */
    CHECK(pl_core_handle(&c, "0d10000C0000", 12) == 0 && e_state == ENG_STOP);
    CHECK(access(OPL_STREAM, F_OK) != 0);                     /* stopped: no song named */
    e_cert_stale = 1;                                         /* Radio Certificates changed: MPD restarts before the station */
    CHECK(send_station(&c, "Rock", "https://radio.example/rock") == 0 && e_state == ENG_PLAY && e_restarts == 1);
    /* 0657 08: the station stops, the local song comes back paused where it was */
    CHECK(pl_core_handle(&c, "0657000C0008", 12) == 0 && c.work_mode == PL_WORK_LOCAL);
    pb_stream_mode(0);
    if(getenv("PB_DUMP")) printf("back: %s armed %d ms %ld\n", e_cur, e_armed, e_ms);
    CHECK(!pb_stream_active() && !strcmp(e_cur, "/s/a1.flac") && e_armed && e_ms >= 42000 && e_ms < 42500);
    CHECK(send_station(&c, "Rock", "https://radio.example/rock") == -1);
    pb_close();
}


/* a stream list (0d14, Bandcamp): OPL_BC_LIST's tracks one after another in the radio work mode */
static int slist_cb(void *ctx, int idx){ (void)ctx; pb_stream_mode(1); return pb_slist_play(idx); }   /* as main.c core_slist */
static void test_slist(void){
    pl_core_t c;
    pl_core_init(&c, cap_send, NULL, NULL);
    c.radio = radio_cb; c.slist = slist_cb;
    pb_init(&c, dbp, sysp, quiet);
    CHECK(pb_play_list(QD_ALBUM, 0, "Alpha") == 0);
    pb_engine_event();
    FILE *f = fopen(OPL_BC_LIST, "w");
    CHECK(f != NULL);
    if(!f) return;
    fputs("181000\thttps://t4.example/stream/1?a=1&b=2\tFirst \"Song\"\tUnicorn Partisans\tRepression\n"
          "bad line\n"
          "200000\tftp://nope\tBad\tX\tY\n"
          "95000\thttps://t4.example/stream/2\tSecond\tUnicorn Partisans\tRepression\n"
          "60000\thttps://t4.example/stream/3\tThird\tUnicorn Partisans\tRepression\n", f);
    fclose(f);
    CHECK(pl_core_handle(&c, "0d14000C0000", 12) == -1);         /* not in the radio work mode */
    CHECK(pl_core_handle(&c, "0657000C000C", 12) == 0);
    pb_stream_mode(1);
    nsent = 0;
    CHECK(pl_core_handle(&c, "0d14000C0001", 12) == 0 && pb_slist_track() == 1 && !strcmp(e_cur, "https://t4.example/stream/2") && e_state == ENG_PLAY);
    pb_engine_event();
    if(getenv("PB_DUMP")){ printf("slist %d sent\n", nsent); for(int i = 0; i < nsent; i++) puts(sent[i]); }
    CHECK(sent_count("Second") == 1 && sent_count("song_duration_time\\\":95000") == 1 && sent_count("Unicorn Partisans") == 1
          && sent_count("\"playing_num\":\"2/3\"") == 1 && sent_count("Repression") == 1);
    /* Next, Previous (near the start: the track before), seek */
    CHECK(pb_transport(PL_T_NEXT, 0) == 0 && pb_slist_track() == 2 && !strcmp(e_cur, "https://t4.example/stream/3"));
    pb_engine_event();
    CHECK(pb_transport(PL_T_NEXT, 0) == -1 && pb_slist_track() == 2);   /* the last one */
    CHECK(pb_transport(PL_T_PREV, 0) == 0 && pb_slist_track() == 1);
    pb_engine_event();
    CHECK(pb_transport(PL_T_SEEK, 30000) == 0 && e_ms == 30000);
    /* far into the track, Previous starts it again */
    e_ms = 50000; pb_engine_event();
    CHECK(pb_transport(PL_T_PREV, 0) == 0 && pb_slist_track() == 1 && e_ms == 0);
    pb_engine_event();
    /* a track that stops at once did not play: ad14 with its index, nothing follows */
    nsent = 0;
    e_song_ends();
    CHECK(sent_count("ad14000C0001") == 1 && pb_slist_track() == 1 && e_state == ENG_STOP);
    /* the UI wrote fresh links and starts it again; once it has played a while, its end starts the next */
    CHECK(pl_core_handle(&c, "0d14000C0001", 12) == 0 && e_state == ENG_PLAY);
    pb_engine_event();
    extern void pb_test_age_stream(double s);
    pb_test_age_stream(100.0);
    nsent = 0;
    e_song_ends();
    CHECK(pb_slist_track() == 2 && !strcmp(e_cur, "https://t4.example/stream/3") && e_state == ENG_PLAY && sent_count("ad14") == 0);
    pb_engine_event();
    pb_test_age_stream(100.0);
    e_song_ends();                                                /* the last track ended: the list stops */
    CHECK(e_state == ENG_STOP && pb_slist_track() == 2);
    CHECK(pb_transport(PL_T_TOGGLE, 0) == 0 && e_state == ENG_PLAY && !strcmp(e_cur, "https://t4.example/stream/3"));   /* Play: again */
    /* the UI rewrote the list (track 3 saved on the card meanwhile): the next start reads it again */
    { usleep(20000); FILE *g = fopen(OPL_BC_LIST, "w");
      if(g){ fputs("181000\thttps://t4.example/stream/1?a=1&b=2\tFirst \"Song\"\tUnicorn Partisans\tRepression\n"
                   "95000\thttps://t4.example/stream/2\tSecond\tUnicorn Partisans\tRepression\n"
                   "60000\t/tmp/sdcard/BandcampDownloads/U/R/03 Third.mp3\tThird\tUnicorn Partisans\tRepression\n", g); fclose(g); }
      CHECK(pb_transport(PL_T_PREV, 0) == 0 && pb_slist_track() == 1 && pb_transport(PL_T_NEXT, 0) == 0 && pb_slist_track() == 2
            && !strcmp(e_cur, "/tmp/sdcard/BandcampDownloads/U/R/03 Third.mp3")); }
    /* a station replaces the list */
    CHECK(send_station(&c, "Rock", "https://radio.example/rock") == 0 && pb_slist_track() == -1);
    CHECK(pb_transport(PL_T_NEXT, 0) == -1);
    /* a track past the end starts the list at its first; leaving the mode ends the list */
    CHECK(pl_core_handle(&c, "0d14000C0009", 12) == 0 && pb_slist_track() == 0 && !strcmp(e_cur, "https://t4.example/stream/1?a=1&b=2"));
    CHECK(pl_core_handle(&c, "0657000C0008", 12) == 0);
    pb_stream_mode(0);
    CHECK(pb_slist_track() == -1 && !strcmp(e_cur, "/s/a1.flac"));
    unlink(OPL_BC_LIST);
    pb_close();
}

/* ---- after the list (0d11 / 0d12) ---- */
static void test_list_end(void){
    pl_core_t c;
    pl_core_init(&c, cap_send, NULL, NULL);
    pb_close(); e_cur[0] = 0; e_state = ENG_STOP; e_title[0] = 0;
    pb_init(&c, dbp, sysp, quiet);
    /* the frames */
    CHECK(pl_core_handle(&c, "0d11000C0002", 12) == 0 && c.end_mode == PL_END_STATION);
    CHECK(pl_core_handle(&c, "0d11000C0003", 12) == -1 && c.end_mode == PL_END_STATION);
    char st[128]; snprintf(st, sizeof st, "0d12%04X0003FM4https://fm4.example/live", (unsigned)(12 + 3 + 24));
    CHECK(pl_core_handle(&c, st, (int)strlen(st)) == 0 && !strcmp(c.end_name, "FM4") && !strcmp(c.end_url, "https://fm4.example/live"));
    /* Stop (the default): the last song ends, nothing more */
    c.end_mode = PL_END_STOP;
    CHECK(pb_play_list(QD_ALBUM, 2, "Alpha") == 0); pb_engine_event();
    e_song_ends();
    CHECK(e_state == ENG_STOP && !pb_tail_active());
    /* Station: the station plays on as a local song, named by the station; a new title is a new track frame */
    c.end_mode = PL_END_STATION;
    CHECK(pb_play_list(QD_ALBUM, 2, "Alpha") == 0); pb_engine_event();
    nsent = 0;
    e_song_ends();
    CHECK(pb_tail_active() && !strcmp(e_cur, "https://fm4.example/live") && e_state == ENG_PLAY);
    CHECK(sent_count("\"work_mode\":\"LOCAL\"") == 1 && sent_count("\\\"song_name\\\":\\\"FM4\\\"") == 1);
    nsent = 0;
    snprintf(e_title, sizeof e_title, "Some Band - Song 1"); pb_engine_event();
    CHECK(nsent >= 1 && sent_count("Some Band - Song 1") == 1);
    nsent = 0; pb_engine_event();
    CHECK(sent_count("Some Band - Song 1") == 0);              /* the same title: no new frame */
    CHECK(pb_transport(PL_T_TOGGLE, 0) == 0 && e_state == ENG_PAUSE);
    CHECK(pb_transport(PL_T_TOGGLE, 0) == 0 && e_state == ENG_PLAY);
    CHECK(pb_transport(PL_T_SEEK, 1000) == -1);
    /* Next: the list again from its start */
    CHECK(pb_transport(PL_T_NEXT, 0) == 0 && !pb_tail_active() && !strcmp(e_cur, "/s/a1.flac"));
    /* a new list (the UI's queue) ends the station too */
    e_song_ends(); e_song_ends(); e_song_ends();
    CHECK(pb_tail_active());
    CHECK(pb_play_list(QD_ALBUM, 0, "Alpha") == 0 && !pb_tail_active() && !strcmp(e_cur, "/s/a1.flac"));
    e_title[0] = 0;
    /* no station set: stops as before */
    c.end_url[0] = 0;
    CHECK(pb_play_list(QD_ALBUM, 2, "Alpha") == 0); pb_engine_event();
    e_song_ends();
    CHECK(!pb_tail_active() && e_state == ENG_STOP);
    /* the UI picks (Shuffle All, a playlist): the player stops and tells the UI (ad11) */
    c.end_mode = PL_END_UI;
    CHECK(pb_play_list(QD_ALBUM, 2, "Alpha") == 0); pb_engine_event();
    nsent = 0;
    e_song_ends();
    CHECK(!pb_tail_active() && e_state == ENG_STOP && sent_count("ad11000C0000") == 1);
    CHECK(pl_core_handle(&c, "0d11000C0000", 12) == 0 && c.end_mode == PL_END_STOP);
}

/* The UI's queue in RAM (0d13 / ad13, Jo 2026-11-22): up-next songs play before the list goes on, nothing is written
 * to song.db for them, a start the UI has not heard of yet never comes back, party mode ends the list after them. */
static int up_cb(void *ctx, unsigned ack, int party, const int *ids, int n){ (void)ctx; return pb_up_set(ack, party, ids, n); }
static int up_frame(pl_core_t *c, unsigned ack, int party, const int *ids, int n){
    char f[512]; int k = snprintf(f, sizeof f, "0d13%04X%08X%c", 8 + 9 + 8 * n, ack, party ? '1' : '0');
    for(int i = 0; i < n; i++) k += snprintf(f + k, sizeof f - (size_t)k, "%08X", (unsigned)ids[i]);
    return pl_core_handle(c, f, k);
}
static void test_upnext(void){
    pl_core_t c;
    pl_core_init(&c, cap_send, NULL, NULL);
    c.upnext = up_cb;
    pb_close(); e_cur[0] = 0; e_state = ENG_STOP; e_title[0] = 0;
    pb_init(&c, dbp, sysp, quiet);
    c.end_mode = PL_END_STOP;
    /* SONG.IDs: 1 a2, 2 a1, 3 b1, 4 b2, 5 c, 8 m2-96k; album Alpha = a1, a2, c */
    CHECK(pb_play_list(QD_ALBUM, 0, "Alpha") == 0); pb_engine_event();
    sqlite3 *d = qd_open(dbp);
    int changes0 = count(d, "SELECT COUNT(*) FROM CUSTOM_PLAYLIST;");
    char names0[1024]; snprintf(names0, sizeof names0, "%s", queue_names(d));
    { int ids[] = { 3, 4 }; CHECK(up_frame(&c, 0, 0, ids, 2) == 0); }
    CHECK(!strcmp(e_cur, "/s/a1.flac") && !strcmp(e_next, "/s/b1.flac"));     /* the playing song is never cut */
    nsent = 0;
    e_song_ends();
    CHECK(!strcmp(e_cur, "/s/b1.flac") && sent_count("ad13001800000001" "00000003") == 1 && !strcmp(e_next, "/s/b2.flac"));
    pb_engine_event();
    CHECK(sent_count("\\\"pos_id\\\":0") == 1);                               /* an up-next song has no list row */
    { int ids[] = { 3, 4 }; CHECK(up_frame(&c, 0, 0, ids, 2) == 0); }         /* sent before the UI heard of b1 */
    CHECK(!strcmp(e_next, "/s/b2.flac"));                                        /* b1 does not come back */
    e_song_ends();
    CHECK(!strcmp(e_cur, "/s/b2.flac") && sent_count("ad13001800000002" "00000004") == 1);
    CHECK(up_frame(&c, 2, 0, NULL, 0) == 0);
    CHECK(!strcmp(e_next, "/s/a2.flac"));                                        /* then the list goes on after a1 */
    e_song_ends();
    CHECK(!strcmp(e_cur, "/s/a2.flac"));
    CHECK(count(d, "SELECT COUNT(*) FROM CUSTOM_PLAYLIST;") == changes0 && !strcmp(queue_names(d), names0));
    /* party: after the up-next songs the list ends (here: the UI picks, ad11) */
    { int ids[] = { 3 }; CHECK(up_frame(&c, 2, 1, ids, 1) == 0); }
    e_song_ends();
    CHECK(!strcmp(e_cur, "/s/b1.flac") && e_next[0] == 0);
    c.end_mode = PL_END_UI; nsent = 0;
    e_song_ends();
    CHECK(e_state == ENG_STOP && sent_count("ad11000C0000") == 1);
    /* another sample rate: started when the song ends (no gapless) */
    c.end_mode = PL_END_STOP;
    CHECK(pb_play_list(QD_ALBUM, 0, "Alpha") == 0); pb_engine_event();
    { int ids[] = { 8 }; CHECK(up_frame(&c, 3, 0, ids, 1) == 0); }
    CHECK(e_next[0] == 0);
    e_song_ends();
    CHECK(!strcmp(e_cur, "/s/m2-96k.flac") && e_state == ENG_PLAY);
    /* Next plays the first up-next song at once; with nothing loaded Play does */
    { int ids[] = { 4 }; CHECK(up_frame(&c, 4, 0, ids, 1) == 0); }
    CHECK(pb_transport(PL_T_NEXT, 0) == 0 && !strcmp(e_cur, "/s/b2.flac"));
    pb_close(); e_cur[0] = 0; e_state = ENG_STOP;
    pb_init(&c, dbp, sysp, quiet);
    { int ids[] = { 5 }; CHECK(up_frame(&c, 0, 0, ids, 1) == 0); }
    CHECK(pb_transport(PL_T_TOGGLE, 0) == 0 && !strcmp(e_cur, "/s/c.flac"));
    /* the station after the list gives way to the up-next songs */
    c.end_mode = PL_END_STATION; snprintf(c.end_url, sizeof c.end_url, "https://fm4.example/live"); snprintf(c.end_name, sizeof c.end_name, "FM4");
    CHECK(pb_play_list(QD_ALBUM, 2, "Alpha") == 0); pb_engine_event();
    e_song_ends();
    CHECK(pb_tail_active());
    { int ids[] = { 4 }; CHECK(up_frame(&c, 1, 0, ids, 1) == 0); }
    CHECK(!pb_tail_active() && !strcmp(e_cur, "/s/b2.flac"));
    c.end_mode = PL_END_STOP; c.end_url[0] = 0;
    CHECK(pl_core_handle(&c, "0d130010000000", 14) == -1);                     /* malformed */
    /* Repeat One (Hit&Guess) keeps its song: up-next songs wait for Next */
    CHECK(pb_play_list(QD_ALBUM, 0, "Alpha") == 0); pb_engine_event();
    c.play_mode = PQ_REPEAT_ONE;
    { int ids[] = { 3 }; CHECK(up_frame(&c, 9, 0, ids, 1) == 0); }
    CHECK(!strcmp(e_next, "/s/a1.flac"));
    CHECK(pb_transport(PL_T_NEXT, 0) == 0 && !strcmp(e_cur, "/s/b1.flac"));
    c.play_mode = PQ_IN_ORDER;
    sqlite3_close(d);
}

/* 0d15 (Hit&Guess, Jo 2026-11-22): a library song on its own, without a list and without a song.db write; Repeat One
 * plays it again and again; without a repeat it ends the list; up-next songs follow it; a new list ends it */
static int song_cb(void *ctx, int id){ (void)ctx; return pb_play_song(id); }
static void test_playsong(void){
    pl_core_t c;
    pl_core_init(&c, cap_send, NULL, NULL);
    c.upnext = up_cb; c.playsong = song_cb;
    pb_close(); e_cur[0] = 0; e_state = ENG_STOP; e_title[0] = 0;
    pb_init(&c, dbp, sysp, quiet);
    c.end_mode = PL_END_UI;
    CHECK(pb_play_list(QD_ALL, 0, "") == 0); pb_engine_event();
    sqlite3 *d = qd_open(dbp);
    char names0[1024]; snprintf(names0, sizeof names0, "%s", queue_names(d));
    c.play_mode = PQ_REPEAT_ONE;
    CHECK(pl_core_handle(&c, "0d15001000000005", 16) == 0 && !strcmp(e_cur, "/s/c.flac") && !strcmp(e_next, "/s/c.flac"));
    nsent = 0; pb_engine_event();
    CHECK(sent_count("\\\"pos_id\\\":0") == 1);
    e_song_ends();
    CHECK(!strcmp(e_cur, "/s/c.flac") && !strcmp(e_next, "/s/c.flac"));          /* again and again */
    CHECK(!strcmp(queue_names(d), names0));                                       /* nothing written */
    c.play_mode = PQ_IN_ORDER; pb_mode_changed();
    CHECK(e_next[0] == 0);
    nsent = 0; e_song_ends();
    CHECK(e_state == ENG_STOP && sent_count("ad11000C0000") == 1);                /* no list: it ends */
    CHECK(pl_core_handle(&c, "0d15001000000005", 16) == 0); pb_engine_event();
    { int ids[] = { 3 }; CHECK(up_frame(&c, 0, 0, ids, 1) == 0); }
    CHECK(!strcmp(e_next, "/s/b1.flac"));
    e_song_ends();
    CHECK(!strcmp(e_cur, "/s/b1.flac") && e_next[0] == 0);                         /* then nothing of the old list */
    CHECK(pb_play_list(QD_ALBUM, 0, "Alpha") == 0 && !strcmp(e_cur, "/s/a1.flac") && !strcmp(e_next, "/s/a2.flac"));
    CHECK(pl_core_handle(&c, "0d14001000000000", 16) == -1 && pl_core_handle(&c, "0d14001000000999", 16) == -1);
    c.end_mode = PL_END_STOP;
    sqlite3_close(d);
}
int main(void){
    make_db();
    test_queue();
    test_queuedb();
    test_orders_in_step();
    test_playback();
    test_resume();
    test_radio();
    test_slist();
    test_list_end();
    test_upnext();
    test_playsong();
    unlink(dbp); unlink(sysp);
    if(fails){ fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    printf("playback_test: all checks passed\n");
    return 0;
}
