/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Host test of the own player's queue (ui/player/queue.c, queuedb.c) and playback (playback.c) against a real SQLite
 * song.db and an in-process stand-in for the MPD engine (the eng_* and dac_* functions below), so the queue logic, the
 * LIST_SONG tables and the frames to the UI are checked without MPD or the Disc.
 * Run: make -C ui/tests playback_test && ./ui/tests/playback_test */
#include "../player/core.h"
#include "../player/engine.h"
#include "../player/playback.h"
#include "../player/proto.h"
#include "../player/queue.h"
#include "../player/queuedb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

/* ---- stand-in engine: two slots like MPD with our window ---- */
static char e_cur[1024], e_next[1024], e_missing[1024];
static int e_cur_id = -1, e_next_id = -1, e_ids = 1, e_state = ENG_STOP, e_armed, e_loads;
static long e_ms;
int eng_ready(void){ return 1; }
int eng_load(const char *p, long ms, int paused){
    if(e_missing[0] && !strcmp(p, e_missing)) return -1;
    snprintf(e_cur, sizeof e_cur, "%s", p); e_cur_id = e_ids++; e_next[0] = 0; e_next_id = -1; e_ms = ms; e_loads++;
    e_armed = paused; e_state = paused ? ENG_PAUSE : ENG_PLAY;
    return 0;
}
int eng_set_next(const char *p){
    e_next[0] = 0; e_next_id = -1;
    if(!p) return 0;
    if(e_missing[0] && !strcmp(p, e_missing)) return -1;
    snprintf(e_next, sizeof e_next, "%s", p); e_next_id = e_ids++;
    return 0;
}
static int e_advanced_flag;
int eng_advanced(const eng_status_t *s){ (void)s; int a = e_advanced_flag; e_advanced_flag = 0; return a; }
int eng_toggle(void){ e_state = e_state == ENG_PLAY ? ENG_PAUSE : ENG_PLAY; e_armed = 0; return 0; }
int eng_pause(int on){ if(on && e_state == ENG_PLAY) e_state = ENG_PAUSE; return 0; }
int eng_seek_ms(long ms){ e_ms = ms; return 0; }
int eng_status(eng_status_t *s){
    memset(s, 0, sizeof *s);
    s->state = e_state; s->songid = e_cur_id; s->song = e_cur_id >= 0 ? 0 : -1; s->elapsed_ms = e_ms;
    s->rate = e_state == ENG_PLAY ? 44100 : 0; s->bits = 16; s->channels = 2;
    snprintf(s->file, sizeof s->file, "%s", e_cur);
    return 0;
}
/* the current song ends: MPD moves on to the next slot, or stops */
static void e_song_ends(void){
    if(e_next_id >= 0){ memcpy(e_cur, e_next, sizeof e_cur); e_cur_id = e_next_id; e_next[0] = 0; e_next_id = -1; e_advanced_flag = 1; }
    else e_state = ENG_STOP;
    e_ms = 0;
    pb_engine_event();
}
unsigned dac_current_rate(void){ return 44100; }
void dac_attenuation(int v, int g, int b, unsigned char *a, unsigned char *c){ (void)v; (void)g; (void)b; *a = *c = 0; }
int dac_start(unsigned r, unsigned char a, unsigned char c, void (*l)(const char *, ...)){ (void)r; (void)a; (void)c; (void)l; return 0; }

/* ---- frames to the UI ---- */
static char sent[64][2048]; static int nsent;
static void cap_send(void *ctx, const char *f, int len){ (void)ctx; if(nsent < 64){ snprintf(sent[nsent], sizeof sent[0], "%.*s", len, f); nsent++; } }
static void quiet(const char *fmt, ...){ (void)fmt; }

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
/* the queue's paths in LIST_SONG_0 ID order, joined by ',' (file names only) */
static const char *queue_names(sqlite3 *d){
    static char out[1024]; out[0] = 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM LIST_SONG_0 ORDER BY ID;", -1, &st, NULL) != SQLITE_OK) return "?";
    while(sqlite3_step(st) == SQLITE_ROW){
        const char *p = (const char *)sqlite3_column_text(st, 0), *b = strrchr(p, '/');
        snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s", out[0] ? "," : "", b ? b + 1 : p);
    }
    sqlite3_finalize(st);
    return out;
}

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
           "('/s/cue.flac','cue.flac','Cue 1',10,0,'Cue','Cid','Cid','Rock',1,1,1,0,60000,8);");
    sql(d, "INSERT INTO MY_LOVE (PATH,NAME,TITLE,TITLE_CODE,NAME_CODE,ALBUM,ARTIST,TRACK,IS_CUE,IS_ISO,DURATION) VALUES"
           "('/s/c.flac','c.flac','Echo',50,0,'Alpha','Cid',1,0,0,150000),('/s/b1.flac','b1.flac','Gamma',30,0,'Bravo','Bob',0,0,0,150000);");
    /* two playlists; diskOS plays the one with the lowest LIST_ID (seq 0) */
    sql(d, "INSERT INTO CUSTOM_PLAYLIST_INDEX VALUES (5,'Later',''),(2,'Slot','');"
           "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,TITLE,TRACK,IS_CUE,IS_ISO,DURATION) VALUES"
           "(2,'/s/c.flac','Echo',1,0,0,1),(5,'/s/a1.flac','Zulu',1,0,0,1),(2,'/s/a1.flac','Zulu',1,0,0,1);");
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
    CHECK(qd_build(d, QD_ALL, "", 0, err, sizeof err) == 6);
    CHECK(!strcmp(queue_names(d), "a2.flac,b1.flac,b2.flac,c.flac,a1.flac,cue.flac"));
    CHECK(count(d, "SELECT MIN(ID) FROM LIST_SONG_0;") == 1 && count(d, "SELECT COUNT(*) FROM LIST_SONG_0 WHERE SONG_TYPE=3;") == 6);
    CHECK(count(d, "SELECT DURATION FROM LIST_SONG_0 WHERE ID=1;") == 200000);
    /* rebuilt: IDs start at 1 again (DROP + CREATE like the stock player) */
    CHECK(qd_build(d, QD_ALBUM, "Alpha", 0, err, sizeof err) == 3);
    CHECK(count(d, "SELECT MIN(ID) FROM LIST_SONG_0;") == 1);
    CHECK(!strcmp(queue_names(d), "a1.flac,a2.flac,c.flac"));      /* album order: disc, track */
    CHECK(qd_build(d, QD_ARTIST, "Ann", 0, err, sizeof err) == 3);
    CHECK(!strcmp(queue_names(d), "a2.flac,b2.flac,a1.flac"));
    CHECK(qd_build(d, QD_ARTIST, "Various", 1, err, sizeof err) == 2);   /* album artist grouping */
    CHECK(!strcmp(queue_names(d), "b1.flac,b2.flac"));
    CHECK(qd_build(d, QD_GENRE, "Jazz", 0, err, sizeof err) == 2);
    CHECK(qd_build(d, QD_ARTIST_ALBUM, "{\"artist\":\"Various\", \"album\":\"Bravo\"}", 1, err, sizeof err) == 2);
    CHECK(!strcmp(queue_names(d), "b2.flac,b1.flac"));               /* disc 0 after disc 1 */
    CHECK(qd_build(d, QD_ARTIST_ALBUM, "{\"artist\":\"Ann\", \"album\":\"Bravo\"}", 0, err, sizeof err) == 1);
    CHECK(qd_build(d, QD_GENRE_ALBUM, "{\"style\":\"Rock\", \"album\":\"Alpha\"}", 0, err, sizeof err) == 3);
    CHECK(qd_build(d, QD_ARTIST_ALBUM, "{\"album\":\"x\"}", 0, err, sizeof err) == -1 && strstr(err, "payload"));
    CHECK(qd_build(d, QD_FAVOURITES, "", 0, err, sizeof err) == 2);   /* MY_LOVE without the M3U columns */
    CHECK(!strcmp(queue_names(d), "b1.flac,c.flac"));
    CHECK(qd_build(d, QD_PLAYLIST, "", 0, err, sizeof err) == 2);     /* seq 0 = LIST_ID 2, in row order */
    CHECK(!strcmp(queue_names(d), "c.flac,a1.flac"));
    CHECK(qd_build(d, QD_PLAYLIST, "1", 0, err, sizeof err) == 1);
    CHECK(qd_build(d, 4, "", 0, err, sizeof err) == -1);
    CHECK(qd_build(d, QD_ALBUM, "it's", 0, err, sizeof err) == 0);    /* a quote is bound, not interpolated */

    CHECK(qd_build(d, QD_ALL, "", 0, err, sizeof err) == 6);
    int *ids = NULL; CHECK(qd_ids(d, &ids) == 6 && ids[0] == 1 && ids[5] == 6); free(ids);
    int ord[6] = { 4, 1, 2, 3, 6, 5 }, *o = NULL;
    CHECK(qd_shuffle_write(d, ord, 6) == 0 && qd_shuffle_read(d, &o) == 6 && o[0] == 4 && o[5] == 5); free(o);
    CHECK(count(d, "SELECT MIN(ID) FROM LIST_SONG_3;") == 1);
    CHECK(qd_build(d, QD_ALL, "", 0, err, sizeof err) == 6 && count(d, "SELECT COUNT(*) FROM sqlite_master WHERE name='LIST_SONG_3';") == 0);
    CHECK(qd_shuffle_read(d, &o) == 0);

    qd_row_t r;
    CHECK(qd_row(d, 4, &r) == 1 && !strcmp(r.path, "/s/c.flac") && r.song_id == 5 && r.love && r.duration_ms == 150000);
    CHECK(qd_row(d, 1, &r) == 1 && !strcmp(r.title, "Beta") && r.song_id == 1 && !r.love);
    CHECK(qd_row(d, 6, &r) == 1 && r.is_cue);
    CHECK(qd_row(d, 99, &r) == 0);
    sqlite3_close(d);
    CHECK(qd_artist_class(sysp) == 0 && qd_artist_class("/nonexistent/sys.db") == -1);
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

static void test_playback(void){
    pl_core_t c;
    pl_core_init(&c, cap_send, NULL, NULL);
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
    /* shuffle: LIST_SONG_3 written, an exact permutation with the current song first */
    c.play_mode = PQ_SHUFFLE; pb_mode_changed();
    sqlite3 *d = qd_open(dbp);
    CHECK(count(d, "SELECT COUNT(*) FROM LIST_SONG_3;") == 3 && count(d, "SELECT POS_ID FROM LIST_SONG_3 WHERE ID=1;") == 3);
    CHECK(count(d, "SELECT COUNT(DISTINCT POS_ID) FROM LIST_SONG_3 WHERE POS_ID IN (SELECT ID FROM LIST_SONG_0);") == 3);
    CHECK(e_next[0] != 0);
    c.play_mode = PQ_IN_ORDER; pb_mode_changed();
    CHECK(count(d, "SELECT COUNT(*) FROM sqlite_master WHERE name='LIST_SONG_3';") == 0);
    /* a play list in shuffle: the tapped song plays first, LIST_SONG_3 starts with it */
    c.play_mode = PQ_SHUFFLE;
    CHECK(pb_play_list(QD_ALL, 3, "") == 0 && !strcmp(e_cur, "/s/c.flac"));
    CHECK(count(d, "SELECT POS_ID FROM LIST_SONG_3 WHERE ID=1;") == 4 && count(d, "SELECT COUNT(*) FROM LIST_SONG_3;") == 6);
    /* type 0 jumps by LIST_SONG_0 position, also in shuffle */
    CHECK(pb_play_list(QD_RESUME, 0, "") == 0 && !strcmp(e_cur, "/s/a2.flac"));
    c.play_mode = PQ_IN_ORDER; pb_mode_changed();
    /* the CUE row is skipped (not supported yet); a missing file is skipped too */
    CHECK(pb_play_list(QD_ALL, 5, "") == 0 && !strcmp(e_cur, "/s/a2.flac"));   /* wrapped past the CUE row */
    snprintf(e_missing, sizeof e_missing, "/s/b1.flac");
    CHECK(pb_play_list(QD_ALL, 0, "") == 0 && !strcmp(e_next, "/s/b2.flac"));
    CHECK(pb_play_list(QD_ALL, 1, "") == 0 && !strcmp(e_cur, "/s/b2.flac"));
    e_missing[0] = 0;
    /* a fresh player (no queue in memory) takes type 0 from the tables */
    pb_close();
    CHECK(pb_play_list(QD_RESUME, 2, "") == 0 && !strcmp(e_cur, "/s/b2.flac"));
    /* 0202 while a track is loaded: the full a2 with the real state */
    nsent = 0; pb_engine_event(); nsent = 0;
    CHECK(pb_transport(PL_T_STATE, 0) == 0 && nsent == 1 && strstr(sent[0], "\"state\":0"));
    /* the Artist grouping comes from 0648 and is written to SYSCONFIG, as the UI predicts the queue from there */
    c.artist_grouping = 1;
    CHECK(pb_play_list(QD_ARTIST, 0, "Various") == 0 && qd_artist_class(sysp) == 1);
    CHECK(!strcmp(queue_names(d), "b1.flac,b2.flac"));
    /* headphones pulled */
    e_state = ENG_PLAY; pb_engine_event();
    CHECK(pb_transport(PL_T_PAUSE, 0) == 0 && e_state == ENG_PAUSE);
    sqlite3_close(d);
    pb_close();
}

int main(void){
    make_db();
    test_queue();
    test_queuedb();
    test_orders_in_step();
    test_playback();
    unlink(dbp); unlink(sysp);
    if(fails){ fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    printf("playback_test: all checks passed\n");
    return 0;
}
