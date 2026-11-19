/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Hit&Guess (Jo, 2026-11-18): a music quiz for the phones of everybody in the room, on the guests' link while it is
 * in game mode (System > Party: "The guests' link" = Game). The rules are hitguess_core.c; this file adds:
 *  - the setup, kept on the SD card in .diskos/state/hitguess.conf (written only when the host saves it);
 *  - the songs: the whole library, a folder with its subfolders, or a playlist, read when the game starts;
 *  - the playing: a chosen song plays in the all-songs list (as Songs does); hitguess_tick() pauses it at its end so
 *    the next song in that list never gives anything away, or after the part (a random place, HG_S_SNIP seconds);
 *  - each phone's view (api/game): a song's title, artist, album, date and cover only for whom hg_knows() allows -
 *    no secret beyond that (Jo: whoever peeks spoils the game, nothing more);
 *  - the log of every game in /tmp/hitguess/ (RAM; the newest HG_LOGS games, each at most HG_LOG_MAX bytes).
 * Players and points are in memory only: a restart of the Disc ends a game. */
#include "hitguess.h"
#include "hitguess_core.h"
#include "party.h"
#include "remote.h"
#include "screens.h"
#include "musicdb.h"
#include "sdstate.h"
#include "folderbrowser.h"
#include "theme.h"
#include "fork_theme.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HG_FILE    "hitguess.conf"
#define HG_LOGS    20
#define HG_LOG_MAX (1024 * 1024)

static hg_game_t G;
static int g_ready;                                      /* G holds the card's setup */
static char g_dir[PATH_MAX];                             /* the pool's folder, relative to the card */
static long g_pl;                                        /* the pool's playlist */

/* ---------------------------------------------------------------- the log (/tmp/hitguess/) */
static FILE *g_log; static long g_log_n;
static int name_cmp(const void *a, const void *b){ return strcmp(*(char *const *)a, *(char *const *)b); }
static void log_prune(void){                             /* keep the newest HG_LOGS - 1 before a new one */
    DIR *d = opendir(HITGUESS_LOG_DIR); if(!d) return;
    char *names[256]; int n = 0; struct dirent *e;
    while((e = readdir(d)) && n < 256) if(!strncmp(e->d_name, "game-", 5)) names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, name_cmp);
    for(int i = 0; i < n; i++){
        if(names[i] && i < n - (HG_LOGS - 1)){
            char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", HITGUESS_LOG_DIR, names[i]);
            if(unlink(p)) perror(p);
        }
        free(names[i]);
    }
}
static void hlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void hlog(const char *fmt, ...){
    if(!g_log || g_log_n > HG_LOG_MAX) return;
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char line[1024]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    int w = fprintf(g_log, "%02d:%02d:%02d %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
    if(w > 0) g_log_n += w;
    if(g_log_n > HG_LOG_MAX) fprintf(g_log, "(log full - the rest of this game is not logged)\n");
    fflush(g_log);
}
static void log_open(void){
    if(g_log){ fclose(g_log); g_log = NULL; }
    if(mkdir(HITGUESS_LOG_DIR, 0755) && errno != EEXIST){ perror("hitguess: " HITGUESS_LOG_DIR); return; }
    log_prune();
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char p[PATH_MAX]; struct stat st;
    for(int k = 1; k < 100; k++){                         /* two games in one second: -2, -3, ... */
        snprintf(p, sizeof p, "%s/game-%04d%02d%02d-%02d%02d%02d%s%.0d.log", HITGUESS_LOG_DIR, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, k > 1 ? "-" : "", k > 1 ? k : 0);
        if(stat(p, &st)) break;
    }
    g_log = fopen(p, "w"); g_log_n = 0;
    if(!g_log) perror(p);
}

/* ---------------------------------------------------------------- the setup (SD card) */
static void setup_load(void){
    int set[HG_S_N]; for(int k = 0; k < HG_S_N; k++) set[k] = HG_KEYS[k].def;
    g_dir[0] = 0; g_pl = 0;
    char *t = NULL; size_t n = 0; int r = 0;
    if(sds_begin()){ r = sds_read(HG_FILE, &t, &n, 8192); sds_end(); }
    if(r < 0) fprintf(stderr, "hitguess: %s on the SD card could not be read - the default setup applies\n", HG_FILE);
    for(const char *l = r == 1 ? t : NULL; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL){
        char key[24], val[PATH_MAX];
        if(sscanf(l, "%23[a-z_]=%4095[^\n]", key, val) != 2) continue;
        if(!strcmp(key, "dir")){ if(rmt_relpath_ok(val)) snprintf(g_dir, sizeof g_dir, "%s", val); continue; }
        char *e; long v = strtol(val, &e, 10);
        if(*e) continue;
        if(!strcmp(key, "pl")){ if(v >= 0) g_pl = v; continue; }
        for(int k = 0; k < HG_S_N; k++) if(!strcmp(key, HG_KEYS[k].key) && v >= HG_KEYS[k].min && v <= HG_KEYS[k].max) set[k] = (int)v;
    }
    free(t);
    if(G.phase == HG_LOBBY && !G.np) hg_reset(&G, set); else memcpy(G.set, set, sizeof set);
}
static int setup_save(void){
    char text[PATH_MAX + 1024]; int o = 0;
    o += snprintf(text + o, sizeof text - (size_t)o, "# diskOS Hit&Guess (Remote Control: the game page)\ndir=%s\npl=%ld\n", g_dir, g_pl);
    for(int k = 0; k < HG_S_N && o < (int)sizeof text; k++) o += snprintf(text + o, sizeof text - (size_t)o, "%s=%d\n", HG_KEYS[k].key, G.set[k]);
    if(o >= (int)sizeof text) return -1;
    int r = -1;
    if(sds_begin()){ r = sds_mkdir() == 0 ? sds_write(HG_FILE, text, (size_t)o) : -1; sds_end(); }
    return r;
}
static void ready(void){ if(!g_ready){ hg_reset(&G, NULL); setup_load(); g_ready = 1; } }

/* ---------------------------------------------------------------- corrections (Jo, 2026-11-18/19)
 * Many songs carry a wrong year, artist or title, and some songs are no good for the game. The player whose turn it
 * is (once the song is revealed) or the host corrects them; they go to <card>/.diskos/state/song-fixes.tsv, one line
 * per song (a newer one replaces the older), to be written into the tags later. Until then the game uses them: a song
 * with a fix shows and counts the right artist, title and year, and a song left out never comes up again.
 * Columns (tab-separated): path on the card, title, artist, the library's date (these four as the library has them),
 * the right artist, title, year (empty: unchanged), leave out (1), who, when (local time).
 * year-fixes.tsv (the first version: only years, 295418b) is still read, never written. */
#define HG_FIXES "song-fixes.tsv"
#define HG_FIXES_OLD "year-fixes.tsv"
#define HG_FIXES_MAX (2 * 1024 * 1024)
static const char HG_FIXES_HEAD[] = "# diskOS Hit&Guess corrections for the tags: path on the SD card, title, artist, date in the library, "
                                    "right artist, right title, right year (empty: unchanged), leave out of the game (1), by, when\n";
typedef struct { char artist[HG_TXT_MAX], title[HG_TXT_MAX]; int year, out; } fix_t;
static const char *card_rel(const char *p){              /* /tmp/sdcard/Music/x.flac -> Music/x.flac */
    size_t n = strlen(FOLDERBROWSER_ROOT);
    return !strncmp(p, FOLDERBROWSER_ROOT, n) && p[n] == '/' ? p + n + 1 : p;
}
static void tsv_field(char *out, size_t cap, const char *s){   /* no tabs or line breaks inside a field */
    size_t o = 0;
    for(; s && *s && o + 1 < cap; s++) out[o++] = *s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s;
    out[o] = 0;
}
static int fix_key(char *key, size_t cap, const char *path, const char *title){   /* "path\ttitle\t" as in the file */
    char p[PATH_MAX], t[HG_TXT_MAX];
    tsv_field(p, sizeof p, card_rel(path)); tsv_field(t, sizeof t, title);
    int kn = snprintf(key, cap, "%s\t%s\t", p, t);
    return kn > 0 && kn < (int)cap ? kn : -1;
}
/* the line for key in text: its start, or NULL; *end its end (after the line break) */
static char *fix_line(char *text, const char *key, int kn, char **end){
    for(char *l = text; l && *l; ){
        char *nl = strchr(l, '\n'); char *e = nl ? nl + 1 : l + strlen(l);
        if(!strncmp(l, key, (size_t)kn)){ *end = e; return l; }
        l = e;
    }
    return NULL;
}
/* field i (0-based) of the line from l to e into out ("" when missing) */
static void fix_field(const char *l, const char *e, int i, char *out, size_t cap){
    for(int k = 0; k < i && l && l < e; k++){ l = memchr(l, '\t', (size_t)(e - l)); if(l) l++; }
    size_t o = 0;
    if(l) for(; l < e && *l != '\t' && *l != '\n' && *l != '\r' && o + 1 < cap; l++) out[o++] = *l;
    out[o] = 0;
}
static int fix_read(const char *name, char **t){            /* inside a lease: 1 read, 0 none, -1 error */
    size_t n = 0; *t = NULL;
    return sds_read(name, t, &n, HG_FIXES_MAX);
}
/* the corrections for a song: 1 with *f, 0 none */
static int fix_get(const char *path, const char *title, fix_t *f){
    memset(f, 0, sizeof *f);
    char key[PATH_MAX + HG_TXT_MAX + 4]; int kn = fix_key(key, sizeof key, path, title);
    if(kn < 0 || !sds_begin()) return 0;
    char *t = NULL, *o = NULL, *e, *l; int any = 0;
    if(fix_read(HG_FIXES, &t) == 1 && (l = fix_line(t, key, kn, &e))){
        char y[16], x[8];
        fix_field(l, e, 4, f->artist, sizeof f->artist); fix_field(l, e, 5, f->title, sizeof f->title);
        fix_field(l, e, 6, y, sizeof y); fix_field(l, e, 7, x, sizeof x);
        f->year = atoi(y); f->out = atoi(x) == 1; any = 1;
    } else if(fix_read(HG_FIXES_OLD, &o) == 1 && (l = fix_line(o, key, kn, &e))){
        char y[16]; fix_field(l, e, 4, y, sizeof y); f->year = atoi(y); any = 1;
    }
    sds_end(); free(t); free(o);
    if(f->year < 1000 || f->year > 2999) f->year = 0;
    return any && (f->artist[0] || f->title[0] || f->year || f->out);
}
/* the songs left out, as sorted keys, for loading the pool */
typedef struct { char *buf; char **keys; int n; } outs_t;
static int key_cmp(const void *a, const void *b){ return strcmp(*(char *const *)a, *(char *const *)b); }
static void outs_load(outs_t *o){
    memset(o, 0, sizeof *o);
    if(!sds_begin()) return;
    int r = fix_read(HG_FIXES, &o->buf);
    sds_end();
    if(r != 1 || !o->buf) return;
    int cap = 0;
    for(char *l = o->buf; *l; ){
        char *nl = strchr(l, '\n'); char *e = nl ? nl + 1 : l + strlen(l);
        char x[8]; fix_field(l, e, 7, x, sizeof x);
        if(l[0] != '#' && atoi(x) == 1){
            char *t1 = memchr(l, '\t', (size_t)(e - l)), *t2 = t1 ? memchr(t1 + 1, '\t', (size_t)(e - t1 - 1)) : NULL;
            if(t2){
                if(o->n == cap){ int nc = cap ? cap * 2 : 64; char **nk = realloc(o->keys, sizeof *nk * (size_t)nc); if(!nk) break; o->keys = nk; cap = nc; }
                t2[1] = 0; o->keys[o->n++] = l;              /* "path\ttitle\t" (the line is cut after it) */
            }
        }
        if(!nl) break;
        l = e;
    }
    if(o->n > 1) qsort(o->keys, (size_t)o->n, sizeof *o->keys, key_cmp);
}
static int outs_has(const outs_t *o, const char *path, const char *title){
    if(!o->n) return 0;
    char key[PATH_MAX + HG_TXT_MAX + 4], *k = key;
    if(fix_key(key, sizeof key, path, title) < 0) return 0;
    return bsearch(&k, o->keys, (size_t)o->n, sizeof *o->keys, key_cmp) != NULL;
}
static void outs_free(outs_t *o){ free(o->keys); free(o->buf); memset(o, 0, sizeof *o); }
/* ---------------------------------------------------------------- the songs of the game */
static int *g_pool, g_npool, g_nused, g_nout; static unsigned char *g_used;   /* g_used[i]: 1 played, 2 left out (a correction) */
static int id_cmp(const void *a, const void *b){ int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
typedef struct { int *ids; int n, cap; const outs_t *out; } idlist_t;
static int collect(void *ud, const mdb_row_t *r){
    idlist_t *l = ud;
    if(l->out && r->path && outs_has(l->out, r->path, r->title && r->title[0] ? r->title : r->name ? r->name : "")) return 0;   /* left out */
    if(l->n == l->cap){
        int nc = l->cap ? l->cap * 2 : 1024; int *ni = realloc(l->ids, sizeof *ni * (size_t)nc);
        if(!ni) return 1;
        l->ids = ni; l->cap = nc;
    }
    l->ids[l->n++] = r->id;
    return 0;
}
static const outs_t *g_pl_outs;                         /* playlist pools: the songs left out (pool_load) */
static int playlist_out(void *ud, const mdb_row_t *r){
    int *id = ud;
    if(g_pl_outs && r->path && outs_has(g_pl_outs, r->path, r->title && r->title[0] ? r->title : r->name ? r->name : "")) *id = 0;
    return 1;
}
static const char *pool_dir_abs(void){
    static char full[PATH_MAX + 16];
    snprintf(full, sizeof full, "%s%s%s", FOLDERBROWSER_ROOT, g_dir[0] ? "/" : "", g_dir);
    return full;
}
static int pool_load(const char **why){
    free(g_pool); free(g_used); g_pool = NULL; g_used = NULL; g_npool = g_nused = g_nout = 0;
    outs_t outs; outs_load(&outs);
    idlist_t l = { 0 }; int r; l.out = &outs;
    if(G.set[HG_S_POOL] == 2){
        int n = mdb_playlist_count(g_pl);
        if(g_pl <= 0 || n < 0){ *why = "Choose the game's playlist first"; return -1; }
        l.ids = calloc((size_t)n + 1, sizeof *l.ids); l.cap = n;
        r = l.ids ? mdb_playlist_song_ids(g_pl, l.ids, n) : -1;
        l.n = r > 0 ? r : 0;
        g_pl_outs = &outs;
        if(outs.n) for(int i = 0; i < l.n; i++) mdb_rows(MDB_ROWS_ID, NULL, l.ids[i], playlist_out, &l.ids[i]);   /* left out: id 0 */
    } else r = mdb_rows(G.set[HG_S_POOL] == 1 ? MDB_ROWS_UNDER : MDB_ROWS_ALL, G.set[HG_S_POOL] == 1 ? pool_dir_abs() : NULL, 0, collect, &l);
    int nout = outs.n; g_pl_outs = NULL; outs_free(&outs);
    if(r < 0){ free(l.ids); *why = "Couldn't read the library - is the SD card in?"; return -1; }
    int k = 0;
    if(l.n){ qsort(l.ids, (size_t)l.n, sizeof *l.ids, id_cmp); for(int i = 0; i < l.n; i++) if(l.ids[i] > 0 && (!k || l.ids[k - 1] != l.ids[i])) l.ids[k++] = l.ids[i]; }
    if(!k){ free(l.ids); *why = nout ? "Every song here is left out (corrections)" : G.set[HG_S_POOL] == 2 ? "The playlist has no songs of the library" : G.set[HG_S_POOL] == 1 ? "No songs of the library in that folder" : "The library is empty"; return -1; }
    g_pool = l.ids; g_npool = k; g_used = calloc((size_t)k, 1);
    if(!g_used){ free(g_pool); g_pool = NULL; g_npool = 0; *why = "Out of memory"; return -1; }
    return 0;
}
static int pool_at(int id){
    int lo = 0, hi = g_npool - 1;
    while(lo <= hi){ int m = (lo + hi) / 2; if(g_pool[m] == id) return m; if(g_pool[m] < id) lo = m + 1; else hi = m - 1; }
    return -1;
}
static int pool_left(void){ return !g_npool || g_npool == g_nout ? 0 : G.set[HG_S_REPEAT] ? -1 : g_npool - g_nused - g_nout; }
static int can_play(int id){ int i = pool_at(id); return i >= 0 && g_used[i] != 2 && (G.set[HG_S_REPEAT] || !g_used[i]) && id != G.next_song; }
static int pool_random(const int *not, int nnot){
    if(!g_npool || pool_left() == 0) return 0;
    for(int tries = 0; tries < 8; tries++){
        int i = rand() % g_npool, ok = 0;
        for(int k = 0; k < g_npool && !ok; k++){
            int id = g_pool[(i + k) % g_npool];
            if(!can_play(id)) continue;
            ok = 1;
            for(int j = 0; j < nnot; j++) if(not[j] == id) ok = 0;
            if(ok) return id;
        }
    }
    return 0;
}
typedef struct { int id, dur; char title[HG_TXT_MAX], artist[HG_TXT_MAX], album[HG_TXT_MAX], date[11], path[PATH_MAX]; } song_t;
static int one(void *ud, const mdb_row_t *r){
    song_t *s = ud; s->id = r->id; s->dur = r->dur_ms;
    snprintf(s->title, sizeof s->title, "%s", r->title && r->title[0] ? r->title : r->name ? r->name : "");
    snprintf(s->artist, sizeof s->artist, "%s", r->artist ? r->artist : ""); snprintf(s->album, sizeof s->album, "%s", r->album ? r->album : "");
    snprintf(s->date, sizeof s->date, "%s", r->date ? r->date : "");
    snprintf(s->path, sizeof s->path, "%s", r->path ? r->path : "");
    return 1;
}
static int song_get(int id, song_t *s){ memset(s, 0, sizeof *s); return mdb_rows(MDB_ROWS_ID, NULL, id, one, s) > 0 && s->id == id; }

/* ---------------------------------------------------------------- playing */
static int g_loop;                                       /* Repeat One is on for the game */
static void loop_check(void){                            /* the game is over or off: the user's play mode again */
    if(g_loop && (party_mode() != 2 || G.phase == HG_LOBBY || G.phase == HG_END)){ ui_play_mode_restore(); g_loop = 0; }
}
static struct { int on, seen, seeks, done; uint32_t at, deadline; char before[160], title[160]; long start, end, dur; } P;
static void send(rmt_cmd_kind_t k, long arg){
    rmt_cmd_t c; memset(&c, 0, sizeof c); c.kind = k; c.arg = arg;
    const char *why = NULL; char *body = NULL;
    if(remote_exec(&c, &why, &body)) hlog("player: %s refused (%s)", rmt_cmd_name(k), why ? why : "?");
    free(body);
}
static int play_song(int id, const char **why){
    rmt_state_t s; remote_state(&s);
    rmt_cmd_t c; memset(&c, 0, sizeof c); c.kind = RMT_CMD_PLAY_ITEM; c.sub = RMT_I_SONG; c.song_id = id;
    char *body = NULL;
    int r = remote_exec(&c, why, &body); free(body);
    if(r) return -1;
    /* Repeat One until the next song starts (Jo, 2026-11-18): the song never runs out into the next one of the list
     * (a song that ended before the next player had chosen once came with a player crash and reboot on the Disc).
     * Not saved; the user's mode comes back when the game ends (hitguess_tick). */
    if(!g_loop){ ui_play_mode_temp(2); g_loop = 1; }
    memset(&P, 0, sizeof P);
    P.on = 1; P.at = lv_tick_get() | 1; snprintf(P.before, sizeof P.before, "%s", s.has_track ? s.title : "");
    return 0;
}
static void stop_song(const char *who){ if(P.on){ send(RMT_CMD_PAUSE, 0); P.on = 0; hlog("%s stopped the song", who); } }
static int countdown_left(void){                        /* seconds, -1 none */
    if(!G.set[HG_S_COUNTDOWN] || G.phase != HG_PLAY || !P.deadline) return -1;
    int32_t ms = (int32_t)(P.deadline - lv_tick_get());
    return ms > 0 ? (ms + 999) / 1000 : 0;
}
static void play_tick(void);
static void screen_update(void);
void hitguess_tick(void){
    static uint32_t last;
    if(last && lv_tick_elaps(last) < 200) return;
    last = lv_tick_get() | 1;
    if(g_ready || party_mode() == 2){ ready(); screen_update(); }
    if(P.on && party_mode() == 2) play_tick();
    loop_check();
}
static void play_tick(void){
    rmt_state_t s; remote_state(&s);
    if(!P.seen){
        if(lv_tick_elaps(P.at) > 15000){ P.on = 0; hlog("the song didn't start"); return; }
        if(!s.has_track || !s.playing || lv_tick_elaps(P.at) < 400 || (!strcmp(s.title, P.before) && s.position_ms >= 3000)) return;
        P.seen = 1; snprintf(P.title, sizeof P.title, "%s", s.title);
        long dur = s.duration_ms > 0 ? s.duration_ms : P.dur;
        if(G.set[HG_S_START] == 1){                       /* a part from a random place, not in the first or last seconds */
            long sn = (long)G.set[HG_S_SNIP] * 1000, lo = dur / 10, room = dur - sn - 3000;
            P.start = room > lo ? lo + rand() % (room - lo + 1) : room > 0 ? room / 2 : 0;
            P.end = P.start + sn;
            if(P.start > 1500){ send(RMT_CMD_SEEK, P.start); P.seeks = 1; }
        } else { P.start = 0; P.end = 0; }               /* the whole song, again and again (Repeat One) */
        long len = (P.end ? P.end : dur) - P.start;
        if(G.set[HG_S_COUNTDOWN] && !P.deadline) P.deadline = (lv_tick_get() + (uint32_t)(len > 0 ? len : 0) + (uint32_t)G.set[HG_S_CDEXTRA] * 1000u) | 1;
        hlog("playing \"%s\" (%ld s), %s %ld..%ld ms", s.title, dur / 1000, G.set[HG_S_START] ? "part" : "whole", P.start, P.end);
        return;
    }
    if(strcmp(s.title, P.title)){ send(RMT_CMD_PAUSE, 0); P.on = 0; hlog("the next song began - paused"); return; }
    if(P.seeks && P.seeks < 3 && s.position_ms + 3000 < P.start && !P.done){ send(RMT_CMD_SEEK, P.start); P.seeks++; return; }
    if(s.playing && P.end > 0 && s.position_ms >= P.end){ send(RMT_CMD_PAUSE, 0); P.done = 1; }
}

/* ---------------------------------------------------------------- the Disc's own screen (Jo, 2026-11-18)
 * While the guests' link is in game mode the Disc shows the game over everything: in the lobby the link as a QR code,
 * then a big "?" while a song is chosen and guessed, the song once it is revealed, the winner at the end. A tap hides
 * it until the game moves on (the next phase or song), so the host can still use the Disc. */
static lv_obj_t *g_scr, *g_cd; static char g_scr_sig[2600], g_hide_sig[64], g_cd_txt[200];
static lv_obj_t *scr_label(const char *t, const lv_font_t *f, lv_color_t c, int y, int w, int lines){
    lv_obj_t *l = lv_label_create(g_scr);
    lv_label_set_text(l, t);
    lv_label_set_long_mode(l, lines > 1 ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);   /* never scrolls vertically */
    lv_obj_set_width(l, w);
    if(lines > 1){ lv_obj_set_height(l, lv_font_get_line_height(f) * lines); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); }
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}
static void scr_close(void){ if(g_scr){ lv_obj_delete(g_scr); g_scr = NULL; } g_cd = NULL; g_scr_sig[0] = 0; }
/* the ranking as one line, "1. Anna (4)  ·  2. Ben (3)" (no breakdown; Jo, 2026-11-18); players who left are out */
static void rank_line(char *out, size_t cap){
    int idx[HG_PLAYERS], place[HG_PLAYERS], n = hg_rank(&G, idx, place); size_t o = 0; out[0] = 0;
    for(int k = 0; k < n && o < cap; k++){
        const hg_player_t *p = &G.p[idx[k]];
        if(!p->in) continue;
        char ps[16];
        int w = snprintf(out + o, cap - o, "%s%d. %s (%s)", o ? "  \xC2\xB7  " : "", place[k], p->name, hg_pts_str(hg_total(p), ps, sizeof ps));
        if(w < 0 || (size_t)w >= cap - o){ out[o] = 0; break; }   /* never a cut name */
        o += (size_t)w;
    }
}
static void phase_sig(char *out, size_t cap){ snprintf(out, cap, "%d/%d/%d", G.phase, G.nt, G.cur); }
static void scr_tap(lv_event_t *e){ (void)e; phase_sig(g_hide_sig, sizeof g_hide_sig); scr_close(); }
static void screen_update(void){
    if(party_mode() != 2){ scr_close(); g_hide_sig[0] = 0; return; }
    char ps[64]; phase_sig(ps, sizeof ps);
    if(g_hide_sig[0]){ if(!strcmp(ps, g_hide_sig)) return; g_hide_sig[0] = 0; }
    const hg_turn_t *t = G.nt && G.phase >= HG_PLAY ? &G.t[G.nt - 1] : NULL;
    char url[96] = "", a[200] = "", b[200] = "", c[200] = "", big[8] = "?", rk[1400] = "";
    if(G.phase != HG_LOBBY) rank_line(rk, sizeof rk);
    int players = 0; for(int i = 0; i < G.np; i++) players += G.p[i].in;
    switch(G.phase){
    case HG_LOBBY:
        if(remote_guests_url(url, sizeof url)) snprintf(a, sizeof a, "Connect the Disc to Wi-Fi");
        snprintf(b, sizeof b, "%d player%s - the host starts on the game page", players, players == 1 ? "" : "s"); big[0] = 0; break;
    case HG_PICK: snprintf(a, sizeof a, "%s chooses a song", G.cur >= 0 ? G.p[G.cur].name : ""); break;
    case HG_PLAY: {
        snprintf(a, sizeof a, "%s's song", G.p[G.cur].name);
        if(t && t->year) snprintf(b, sizeof b, "Guess the year too");
        int cd = countdown_left(); if(cd >= 0) snprintf(c, sizeof c, cd ? "%d s" : "Time is up", cd);
        break; }
    case HG_REVEAL:
        if(!t) break;
        big[0] = 0; snprintf(a, sizeof a, "%s", t->title); snprintf(b, sizeof b, "%s", t->artist);
        if(t->year) snprintf(c, sizeof c, "%d", t->year);         /* the year that counts, not the library's full date */
        break;
    default: {
        int idx[HG_PLAYERS], place[HG_PLAYERS], n = hg_rank(&G, idx, place);
        big[0] = 0; snprintf(a, sizeof a, "Game over");
        char ps[16];
        if(n) snprintf(b, sizeof b, "%s wins", G.p[idx[0]].name), snprintf(c, sizeof c, "%s point%s", hg_pts_str(hg_total(&G.p[idx[0]]), ps, sizeof ps), hg_total(&G.p[idx[0]]) == 2 ? "" : "s");
        break; }
    }
    char sig[sizeof g_scr_sig];
    int cd_live = G.phase == HG_PLAY && countdown_left() >= 0;   /* the countdown changes in place, so the ranking keeps scrolling */
    snprintf(sig, sizeof sig, "%s|%s|%s|%s|%s|%s|%s", ps, url, a, b, cd_live ? "cd" : c, big, rk);
    if(g_scr && !strcmp(sig, g_scr_sig)){
        if(g_cd && strcmp(c, g_cd_txt)){ snprintf(g_cd_txt, sizeof g_cd_txt, "%s", c); lv_label_set_text(g_cd, c); ui_request_wake(); }
        return;
    }
    scr_close(); snprintf(g_scr_sig, sizeof g_scr_sig, "%s", sig);
    g_scr = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_scr);
    lv_obj_set_size(g_scr, 360, 360);
    lv_obj_set_style_bg_color(g_scr, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_scr, LV_OBJ_FLAG_CLICKABLE);              /* swallow taps behind it; a tap hides it */
    lv_obj_remove_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_scr, scr_tap, LV_EVENT_CLICKED, NULL);
    scr_label("Hit&Guess", TH_F_DETAIL, TC(ACCENT_PRIMARY), -140, 200, 1);
    if(G.phase == HG_LOBBY){
        if(url[0]){
            lv_obj_t *qr = lv_qrcode_create(g_scr);
            lv_qrcode_set_size(qr, 150);
            lv_qrcode_set_dark_color(qr, TC(FIXED_QR_DARK)); lv_qrcode_set_light_color(qr, TC(FIXED_QR_LIGHT));
            lv_qrcode_update(qr, url, (uint32_t)strlen(url));
            lv_obj_set_style_border_width(qr, 5, 0); lv_obj_set_style_border_color(qr, TC(FIXED_QR_LIGHT), 0);
            lv_obj_align(qr, LV_ALIGN_CENTER, 0, -35);
            const char *addr = strncmp(url, "http://", 7) ? url : url + 7;   /* the address to type, without http:// */
            const lv_font_t *af = TH_F_DETAIL;
            if(lv_text_get_width(addr, (uint32_t)strlen(addr), af, 0) > 300) af = TF(UI_14);
            scr_label(addr, af, TC(TEXT_PRIMARY), 68, 310, 1);
        } else scr_label(a, TH_F_DETAIL, TC(TEXT_PRIMARY), -20, 260, 2);
        scr_label(b, TF(UI_14), TC(TEXT_MUTED), 108, 240, 2);
    } else if(big[0]){
        lv_obj_t *q = scr_label(big, TF(UI_48), TC(ACCENT_PRIMARY), -50, 120, 1);
        lv_obj_update_layout(q);
        lv_obj_set_style_transform_pivot_x(q, lv_obj_get_width(q) / 2, 0); lv_obj_set_style_transform_pivot_y(q, lv_obj_get_height(q) / 2, 0);
        lv_obj_set_style_transform_scale(q, 640, 0);              /* 2.5 x: the big question mark */
        scr_label(a, TH_F_DETAIL, TC(TEXT_PRIMARY), 20, 280, 1);
        if(b[0]) scr_label(b, TF(UI_14), TC(ACCENT_EMPHASIS), 55, 260, 1);
        if(c[0]){ g_cd = scr_label(c, TF(UI_24), TC(TEXT_PRIMARY), 92, 200, 1); snprintf(g_cd_txt, sizeof g_cd_txt, "%s", c); }
    } else {
        scr_label(a, TF(MUSIC_TITLE), TC(TEXT_PRIMARY), -30, 290, 2);
        scr_label(b, TF(MUSIC_ARTIST), TC(TEXT_SECONDARY), 35, 270, 1);
        if(c[0]) scr_label(c, TF(UI_24), TC(ACCENT_PRIMARY), 85, 220, 1);
    }
    if(rk[0]){                                           /* the ranking at the bottom, scrolling sideways when long */
        lv_obj_t *r = scr_label(rk, TF(UI_14), TC(TEXT_SECONDARY), 122, 240, 1);
        lv_label_set_long_mode(r, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    ui_request_wake();
}

/* A ticks right guesses while the song still plays (Jo, 2026-11-18), when A knows the song; the others see the ticks
 * only once it is revealed */
static int early_marks(int me){ return G.phase == HG_PLAY && me >= 0 && me == G.cur && hg_knows(&G, me); }

/* ---------------------------------------------------------------- choosing */
#define HG_REROLLS 3                                     /* new suggestions per turn (Jo, 2026-11-18: three times) */
static int g_sugg[3], g_nsugg, g_sugg_for = -1, g_sugg_nt = -1, g_rerolled;
static int chooser(void){                                /* who may choose a song now: A in PICK, B while A's song is revealed */
    if(G.phase == HG_PICK) return G.cur;
    if(G.phase == HG_REVEAL && !G.next_song) return hg_after(&G, G.cur);
    return -1;
}
static void suggest(int force){
    int ch = chooser();
    if(ch < 0 || G.set[HG_S_PICK] != 1) return;
    if(!force && g_sugg_for == ch && g_sugg_nt == G.nt) return;
    if(g_sugg_for != ch || g_sugg_nt != G.nt) g_rerolled = 0;
    g_sugg_for = ch; g_sugg_nt = G.nt; g_nsugg = 0;
    int cur = G.nt && G.phase == HG_REVEAL ? G.t[G.nt - 1].song : 0;
    int not[4] = { cur };
    for(int k = 0; k < 3; k++){ int id = pool_random(not, 1 + g_nsugg); if(!id) break; g_sugg[g_nsugg++] = id; not[g_nsugg] = id; }
}
/* saves the corrections for song s (all empty: the line goes) */
static int fix_save(const song_t *s, const fix_t *f, const char *who, const char **why){
    char *t = NULL; int r;
    char key[PATH_MAX + HG_TXT_MAX + 4]; int kn = fix_key(key, sizeof key, s->path, s->title);
    if(kn < 0){ *why = "The song's path is too long"; return -1; }
    if(!sds_begin()){ *why = "No SD card for the correction"; return -1; }
    r = fix_read(HG_FIXES, &t);
    if(r < 0){ sds_end(); *why = "Couldn't read the corrections on the SD card"; return -1; }
    if(r == 0 || !t){ free(t); t = strdup(HG_FIXES_HEAD); }
    if(!t){ sds_end(); *why = "Out of memory"; return -1; }
    char *e, *l = fix_line(t, key, kn, &e);
    if(l) memmove(l, e, strlen(e) + 1);                    /* the newer one replaces the older */
    size_t n = strlen(t);
    char line[PATH_MAX + 5 * HG_TXT_MAX + 128] = "";
    int ln = 0;
    if(f->artist[0] || f->title[0] || f->year || f->out){
        char ar[HG_TXT_MAX], fa[HG_TXT_MAX], ft[HG_TXT_MAX], wh[HG_NAME_MAX + 8], when[32], y[16] = "";
        tsv_field(ar, sizeof ar, s->artist); tsv_field(fa, sizeof fa, f->artist); tsv_field(ft, sizeof ft, f->title); tsv_field(wh, sizeof wh, who);
        if(f->year) snprintf(y, sizeof y, "%d", f->year);
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
        ln = snprintf(line, sizeof line, "%s%s\t%s\t%s\t%s\t%s\t%s\t%s\n", key, ar, s->date, fa, ft, y, f->out ? "1" : "", wh);
        if(ln > 0 && ln < (int)sizeof line - (int)strlen(when)){ line[--ln] = 0; ln += snprintf(line + ln, sizeof line - (size_t)ln, "\t%s\n", when); }
        else ln = 0;
    }
    char *nt = realloc(t, n + (size_t)ln + 1);
    if(!nt){ free(t); sds_end(); *why = "Out of memory"; return -1; }
    memcpy(nt + n, line, (size_t)ln + 1);
    r = sds_mkdir() == 0 ? sds_write(HG_FIXES, nt, n + (size_t)ln) : -1;
    sds_end(); free(nt);
    if(r){ *why = "Couldn't save the correction on the SD card"; return -1; }
    return 0;
}
/* turn k: the right artist, title, year and/or leave it out; the game uses them now, the file keeps them */
static int fix_turn(int k, const rmt_cmd_t *c, const char *who, const char **why){
    if(k < 0 || k >= G.nt){ *why = "No such song in this game"; return -1; }
    song_t s;
    if(!song_get(G.t[k].song, &s)){ *why = "That song is no longer in the library"; return -1; }
    fix_t f; memset(&f, 0, sizeof f);
    /* only what differs from the library counts; the page sends every field as shown */
    if(c->name[0] && strcmp(c->name, s.artist)) snprintf(f.artist, sizeof f.artist, "%.*s", HG_TXT_MAX - 1, c->name);
    if(c->text[0] && strcmp(c->text, s.title)) snprintf(f.title, sizeof f.title, "%.*s", HG_TXT_MAX - 1, c->text);
    if(c->offset && c->offset != hg_year_of(s.date)) f.year = (int)c->offset;
    f.out = c->item;
    if(fix_save(&s, &f, who, why)) return -1;
    hlog("%s corrects \"%s\" - %s (%s): artist \"%s\", title \"%s\", year %d%s", who, s.title, s.artist, s.date[0] ? s.date : "no date",
         f.artist, f.title, f.year, f.out ? ", left out from now on" : "");
    /* the game shows the corrected song (back to the library's values where a field is no longer corrected) */
    hg_fix_song(&G, k, f.artist[0] ? f.artist : s.artist, f.title[0] ? f.title : s.title, f.year ? f.year : 0);
    if(!f.year){ hg_turn_t *t = &G.t[k]; snprintf(t->date, sizeof t->date, "%s", s.date); if(G.set[HG_S_YEAR]) t->year = hg_year_of(s.date); }
    int pi = pool_at(G.t[k].song);
    if(pi >= 0){
        if(f.out && g_used[pi] != 2){ if(g_used[pi] == 1) g_nused--; g_used[pi] = 2; g_nout++; }
        else if(!f.out && g_used[pi] == 2){ g_used[pi] = 1; g_nout--; g_nused++; }
    }
    return 0;
}
static int start_turn_song(int id, int chance, const char **why){
    song_t s;
    if(!song_get(id, &s)){ *why = "That song is no longer in the library"; return -1; }
    if(play_song(id, why)) return -1;
    P.dur = s.dur;
    int pi = pool_at(id);
    if(pi >= 0 && !g_used[pi]){ g_used[pi] = 1; g_nused++; }
    fix_t fx; int fixed = fix_get(s.path, s.title, &fx);
    hg_song(&G, id, chance, s.title, s.artist, s.album, s.date);
    if(fixed){ hlog("corrections for \"%s\": artist \"%s\", title \"%s\", year %d", s.title, fx.artist, fx.title, fx.year); hg_fix_song(&G, G.nt - 1, fx.artist, fx.title, fx.year); }
    hlog("song %d by %s (%s): \"%s\" - %s, album \"%s\", date %s%s", G.nt, G.p[G.cur].name, chance ? "chance" : "chosen",
         s.title, s.artist, s.album, s.date[0] ? s.date : "none", G.t[G.nt - 1].year ? "" : " (no year to guess)");
    return 0;
}
/* A confirmed or the host skipped: B's early choice starts at once */
static void after_turn(const char *what){
    hlog("%s; next: %s", what, G.phase == HG_END ? "the end" : G.cur >= 0 ? G.p[G.cur].name : "-");
    if(G.phase == HG_END){
        int idx[HG_PLAYERS], place[HG_PLAYERS], n = hg_rank(&G, idx, place);
        hlog("game over (%s) after %d songs, %d rounds", G.ended_why == 1 ? "no song left" : G.ended_why == 2 ? "ended by the host" : "goal reached", G.nt, G.rounds);
        for(int k = 0; k < n; k++){ const hg_player_t *p = &G.p[idx[k]];
            char t0[16], t1[16], t2[16], t3[16];
            hlog("  %d. %s: %s point%s (artist %s, title %s, year %s)%s", place[k], p->name, hg_pts_str(hg_total(p), t0, 16), hg_total(p) == 2 ? "" : "s",
                 hg_pts_str(p->pa, t1, 16), hg_pts_str(p->pt, t2, 16), hg_pts_str(p->py, t3, 16), p->in ? "" : " (left)"); }
        return;
    }
    if(G.phase == HG_PICK && G.next_song){
        int id = G.next_song, ch = G.next_chance; const char *why = NULL;
        G.next_song = 0;
        if(start_turn_song(id, ch, &why)) hlog("the early choice didn't play: %s", why);
    }
}

/* ---------------------------------------------------------------- the views (JSON) */
static const char *PH[] = { "lobby", "pick", "play", "reveal", "end" };
static void song_json(rmt_buf_t *b, const hg_turn_t *t){
    rmt_buf_printf(b, "{\"id\":%d,\"title\":", t->song); rmt_buf_str(b, t->title);
    rmt_buf_put(b, ",\"artist\":"); rmt_buf_str(b, t->artist); rmt_buf_put(b, ",\"album\":"); rmt_buf_str(b, t->album);
    int pi = pool_at(t->song);
    rmt_buf_put(b, ",\"date\":"); rmt_buf_str(b, t->date); rmt_buf_printf(b, ",\"year\":%d,\"out\":%d}", hg_year_of(t->date), pi >= 0 && g_used[pi] == 2);
}
static void sugg_json(rmt_buf_t *b){
    rmt_buf_put(b, ",\"sugg\":[");
    for(int k = 0; k < g_nsugg; k++){
        song_t s; if(!song_get(g_sugg[k], &s)) continue;
        rmt_buf_printf(b, "%s{\"id\":%d,\"title\":", k ? "," : "", s.id); rmt_buf_str(b, s.title);
        rmt_buf_put(b, ",\"artist\":"); rmt_buf_str(b, s.artist); rmt_buf_put(b, "}");
    }
    rmt_buf_printf(b, "],\"rerolls_left\":%d", HG_REROLLS - g_rerolled);
}
static char *view_json(int me, int admin){
    suggest(0);
    rmt_buf_t b; memset(&b, 0, sizeof b);
    const hg_turn_t *t = G.nt && G.phase >= HG_PLAY ? &G.t[G.nt - 1] : NULL;
    int nx = G.phase >= HG_PICK && G.phase <= HG_REVEAL ? hg_after(&G, G.cur) : -1, ch = chooser();
    rmt_buf_printf(&b, "{\"ok\":true,\"phase\":\"%s\",\"you\":%d,\"cur\":%d,\"next\":%d,\"round\":%d,\"songs\":%d,\"left\":%d,\"pool\":%d,\"ended\":%d,",
                   PH[G.phase], me, G.cur, nx, G.rounds + 1, G.nt, pool_left(), g_npool, G.ended_why);
    rmt_buf_put(&b, "\"set\":{");
    for(int k = 0; k < HG_S_N; k++) rmt_buf_printf(&b, "%s\"%s\":%d", k ? "," : "", HG_KEYS[k].key, G.set[k]);
    rmt_buf_put(&b, "},\"players\":[");
    for(int i = 0; i < G.np; i++){
        const hg_player_t *p = &G.p[i];
        rmt_buf_printf(&b, "%s{\"name\":", i ? "," : ""); rmt_buf_str(&b, p->name);
        char t0[16], t1[16], t2[16], t3[16];                /* JSON numbers: 3 or 3.5 */
        rmt_buf_printf(&b, ",\"in\":%d,\"pa\":%s,\"pt\":%s,\"py\":%s,\"pts\":%s,\"joined\":%d,\"tipped\":%d,\"guesses\":%d", p->in,
                       hg_pts_str(p->pa, t1, 16), hg_pts_str(p->pt, t2, 16), hg_pts_str(p->py, t3, 16),
                       hg_pts_str(hg_total(p), t0, 16), p->joined, p->tipped, G.phase == HG_PLAY && hg_guesses(&G, i));
        if(t && (G.phase == HG_REVEAL || admin || early_marks(me))){
            rmt_buf_printf(&b, ",\"mark\":%d", t->mark[i]);
            if(G.set[HG_S_FORM] && p->tipped){ rmt_buf_put(&b, ",\"tip\":{\"a\":"); rmt_buf_str(&b, p->tip_a); rmt_buf_put(&b, ",\"t\":"); rmt_buf_str(&b, p->tip_t); rmt_buf_printf(&b, ",\"y\":%d}", p->tip_y); }
        } else if(i == me && p->tipped){ rmt_buf_put(&b, ",\"tip\":{\"a\":"); rmt_buf_str(&b, p->tip_a); rmt_buf_put(&b, ",\"t\":"); rmt_buf_str(&b, p->tip_t); rmt_buf_printf(&b, ",\"y\":%d}", p->tip_y); }
        rmt_buf_put(&b, "}");
    }
    rmt_buf_put(&b, "]");
    if(t){
        rmt_buf_printf(&b, ",\"turn\":{\"chance\":%d,\"year_counts\":%d,\"countdown\":%d,\"all_tipped\":%d,\"part\":%d,\"done\":%d,\"song\":",
                       t->chance, t->year > 0, countdown_left(), hg_all_tipped(&G), G.set[HG_S_START], P.done || !P.on);
        if(admin || hg_knows(&G, me)) song_json(&b, t); else rmt_buf_put(&b, "null");
        rmt_buf_put(&b, "}");
    }
    if(admin || (me >= 0 && me == ch)){ rmt_buf_printf(&b, ",\"choose\":%d", ch); if(G.set[HG_S_PICK] == 1) sugg_json(&b); }
    if(G.next_song) rmt_buf_put(&b, ",\"next_ready\":true");
    rmt_buf_printf(&b, ",\"can\":{\"choose\":%d,\"pick\":%d,\"tip\":%d,\"reveal\":%d,\"mark\":%d,\"replay\":%d,\"stop\":%d}",
                   me >= 0 && me == ch, me >= 0 && me == ch && G.set[HG_S_PICK] != 2,
                   G.phase == HG_PLAY && G.set[HG_S_FORM] && hg_guesses(&G, me) && countdown_left() != 0,
                   G.phase == HG_PLAY && me == G.cur, (G.phase == HG_REVEAL && me == G.cur) || early_marks(me),
                   G.phase == HG_PLAY && me == G.cur && G.set[HG_S_REPLAY] && P.seen,
                   hg_may_stop(&G, me) && P.on);
    {                                                    /* the ranking, for everybody (Jo, 2026-11-18: the phones show it too) */
        int idx[HG_PLAYERS], place[HG_PLAYERS], n = hg_rank(&G, idx, place);
        rmt_buf_put(&b, ",\"rank\":[");
        for(int k = 0; k < n; k++) rmt_buf_printf(&b, "%s[%d,%d]", k ? "," : "", place[k], idx[k]);
        rmt_buf_put(&b, "]");
    }
    if(G.phase == HG_END || admin){
        rmt_buf_put(&b, ",\"tracks\":[");
        int upto = G.phase == HG_END ? G.nt : G.nt - (G.phase == HG_PLAY);   /* the song still being guessed stays out */
        for(int k = 0; k < upto; k++){
            rmt_buf_printf(&b, "%s{\"by\":%d,\"chance\":%d,\"skipped\":%d,\"s\":", k ? "," : "", G.t[k].player, G.t[k].chance, G.t[k].skipped);
            song_json(&b, &G.t[k]); rmt_buf_put(&b, "}");
        }
        rmt_buf_put(&b, "]");
    }
    { int v = party_volume_now(); if(v >= 0) rmt_buf_printf(&b, ",\"volume\":%d", v); }   /* every player may change it */
    if(admin){
        rmt_buf_put(&b, ",\"dir\":"); rmt_buf_str(&b, g_dir); rmt_buf_printf(&b, ",\"pl\":%ld,\"mode\":%d,\"link\":", g_pl, party_mode());
        rmt_buf_str(&b, party_link_name());
    }
    rmt_buf_put(&b, "}");
    return rmt_buf_take(&b);
}
int hitguess_cover_ok(const char *gid){ ready(); return hg_knows(&G, hg_find(&G, gid)); }

/* ---------------------------------------------------------------- a player's requests */
static int search(const rmt_cmd_t *c, int me, const char **why, char **body);
static int choose(int me, int id, int chance, const char **why){
    if(chance){ id = pool_random(NULL, 0); if(!id){ *why = "No song left"; return -1; } }
    else if(!can_play(id)){ *why = pool_at(id) < 0 ? "That song isn't part of this game" : "That song has played already"; return -1; }
    if(G.phase == HG_REVEAL){                             /* B chooses early: it plays once A has confirmed */
        G.next_song = id; G.next_chance = chance;
        hlog("%s chose the next song early (%s)", G.p[me].name, chance ? "chance" : "chosen");
        return 0;
    }
    return start_turn_song(id, chance, why);
}
static int player(const rmt_cmd_t *c, const char **why, char **body){
    int me = hg_find(&G, c->gid);
    if(c->kind == RMT_CMD_GAME_SEARCH) return search(c, me, why, body);
    int r = 0;
    switch(c->sub){
    case RMT_G_STATE: break;
    case RMT_G_JOIN: {
        int was = me, n = G.np;
        me = hg_join(&G, c->gid, c->gname, why);
        if(me < 0) return -1;
        if(G.np > n) hlog("%s joined%s", G.p[me].name, G.phase == HG_LOBBY ? "" : " the running game");
        else if(was >= 0) hlog("player %d is now called %s", me, G.p[me].name);
        break; }
    case RMT_G_LEAVE:
        if(me < 0) break;
        hlog("%s left", G.p[me].name);
        hg_remove(&G, me, pool_left()); after_turn("a player left");
        me = hg_find(&G, c->gid); break;
    default:
        if(me < 0 || !G.p[me].in){ *why = "Join the game first"; return -1; }
        switch(c->sub){
        case RMT_G_PICK: case RMT_G_RANDOM:
            if(me != chooser()){ *why = "It's not your turn to choose"; return -1; }
            if(c->sub == RMT_G_PICK && G.set[HG_S_PICK] == 2){ *why = "Only chance chooses in this game"; return -1; }
            r = choose(me, (int)c->song_id, c->sub == RMT_G_RANDOM, why);
            break;
        case RMT_G_REROLL:
            if(me != chooser() || G.set[HG_S_PICK] != 1){ *why = "Not now"; return -1; }
            if(g_rerolled >= HG_REROLLS){ *why = "No new suggestions left this turn"; return -1; }
            suggest(1); g_rerolled++; break;
        case RMT_G_TIP:
            if(countdown_left() == 0){ *why = "Time is up"; return -1; }
            if(!G.set[HG_S_FORM]){ *why = "Say your guess aloud in this game"; return -1; }
            r = hg_tip(&G, me, c->q, c->text, (int)c->arg, why);
            if(!r) hlog("%s guesses: artist \"%s\", title \"%s\", year %ld", G.p[me].name, c->q, c->text, c->arg);
            break;
        case RMT_G_REVEAL:
            if(G.phase != HG_PLAY || me != G.cur){ *why = "Only the player whose turn it is reveals the song"; return -1; }
            hg_reveal(&G); hlog("%s revealed the song", G.p[me].name); break;
        case RMT_G_MARK:
            if(!(G.phase == HG_REVEAL && me == G.cur) && !early_marks(me)){ *why = "Only the player whose turn it is ticks (before the reveal: when they know the song)"; return -1; }
            if(c->pos >= G.np){ *why = "No such player"; return -1; }
            hg_mark(&G, (int)c->pos, (int)c->arg); break;
        case RMT_G_CONFIRM: {
            if(G.phase != HG_REVEAL || me != G.cur){ *why = "Only the player whose turn it is confirms"; return -1; }
            const hg_turn_t *t = &G.t[G.nt - 1];
            for(int i = 0; i < G.np; i++) if(t->mark[i]){ int m = t->mark[i]; char ps[16];
                hlog("  %s: %s%s%s%s%s%s= %s", G.p[i].name, m & HG_M_ARTIST ? "artist " : "", m & HG_M_ANEAR ? "near artist " : "",
                     m & HG_M_TITLE ? "title " : "", m & HG_M_TNEAR ? "near title " : "", m & HG_M_YEAR ? "year " : "", m & HG_M_NEAR ? "near year " : "",
                     hg_pts_str(hg_points(m), ps, sizeof ps)); }
            hg_confirm(&G, pool_left()); after_turn("confirmed");
            break; }
        case RMT_G_REPLAY:
            if(G.phase != HG_PLAY || me != G.cur || !G.set[HG_S_REPLAY] || !P.seen){ *why = "Not now"; return -1; }
            if(!P.on){ *why = "The song isn't playing any more"; return -1; }
            send(RMT_CMD_SEEK, P.start); send(RMT_CMD_PLAY, 0); P.done = 0; hlog("%s played it again", G.p[me].name);
            break;
        case RMT_G_STOP:
            if(!hg_may_stop(&G, me)){ *why = "You may not stop the song"; return -1; }
            stop_song(G.p[me].name); break;
        case RMT_G_FIXSONG:
            if(G.phase != HG_REVEAL || me != G.cur){ *why = "The player whose turn it is corrects the song, once it is revealed"; return -1; }
            if(fix_turn(G.nt - 1, c, G.p[me].name, why)) return -1;
            break;
        default: *why = "Not in this game"; return -1;
        }
    }
    if(r) return -1;
    *body = view_json(me, 0);
    if(!*body){ *why = "Out of memory"; return -1; }
    return 0;
}
typedef struct { const char *q; int off, lim, n, skip; rmt_buf_t *b; } find_t;
static int found(void *ud, const mdb_row_t *r){
    find_t *f = ud;
    if(!can_play(r->id) || !(rmt_match(r->title, f->q) || rmt_match(r->artist, f->q) || rmt_match(r->album, f->q))) return 0;
    if(f->skip < f->off){ f->skip++; return 0; }
    if(f->n == f->lim){ f->n++; return 1; }               /* one more: there are more */
    rmt_buf_printf(f->b, "%s{\"id\":%d,\"title\":", f->n ? "," : "", r->id); rmt_buf_str(f->b, r->title && r->title[0] ? r->title : r->name);
    rmt_buf_put(f->b, ",\"artist\":"); rmt_buf_str(f->b, r->artist); rmt_buf_put(f->b, ",\"album\":"); rmt_buf_str(f->b, r->album); rmt_buf_put(f->b, "}");
    f->n++;
    return 0;
}
static int search(const rmt_cmd_t *c, int me, const char **why, char **body){
    if(me < 0 || me != chooser() || G.set[HG_S_PICK] == 2){ *why = "It's not your turn to choose"; return -1; }
    rmt_buf_t b; memset(&b, 0, sizeof b);
    find_t f = { c->q, (int)c->offset, (int)c->limit, 0, 0, &b };
    rmt_buf_put(&b, "{\"ok\":true,\"items\":[");
    int r;
    if(G.set[HG_S_POOL] == 2){
        r = 0;
        for(int i = 0; i < g_npool && r >= 0; i++){ r = mdb_rows(MDB_ROWS_ID, NULL, g_pool[i], found, &f); if(f.n > f.lim) break; }
    } else r = mdb_rows(G.set[HG_S_POOL] == 1 ? MDB_ROWS_UNDER : MDB_ROWS_ALL, G.set[HG_S_POOL] == 1 ? pool_dir_abs() : NULL, 0, found, &f);
    rmt_buf_printf(&b, "],\"more\":%s}", f.n > f.lim ? "true" : "false");
    *body = rmt_buf_take(&b);
    if(r < 0){ free(*body); *body = NULL; *why = "Couldn't read the library"; return -1; }
    if(!*body){ *why = "Out of memory"; return -1; }
    return 0;
}

/* ---------------------------------------------------------------- the host */
static int host(const rmt_cmd_t *c, const char **why, char **body){
    if(c->arg) switch(c->sub){
    case RMT_G_SETUP: {
        if(G.phase != HG_LOBBY && G.phase != HG_END){ *why = "Change the setup before the game starts (or after it)"; return -1; }
        int old[HG_S_N]; memcpy(old, G.set, sizeof old); char od[PATH_MAX]; snprintf(od, sizeof od, "%s", g_dir); long op = g_pl;
        for(int k = 0; k < HG_S_N; k++) if(c->game[k] >= 0) G.set[k] = c->game[k];
        if(c->item) snprintf(g_dir, sizeof g_dir, "%s", c->name);
        if(c->id >= 0) g_pl = c->id;
        if(setup_save()){ memcpy(G.set, old, sizeof old); snprintf(g_dir, sizeof g_dir, "%s", od); g_pl = op; *why = "Couldn't save on the SD card - is a card in the Disc?"; return -1; }
        break; }
    case RMT_G_START:
        if(party_mode() != 2){ *why = "Set the guests' link to the game first (System > Party)"; return -1; }
        if(G.phase == HG_END) hg_again(&G);
        if(G.phase != HG_LOBBY){ *why = "The game runs already"; return -1; }
        if(!G.np){ *why = "Nobody plays yet - open the game's link on the phones first"; return -1; }
        if(pool_load(why) || hg_start(&G, why)) return -1;
        g_sugg_for = -1; memset(&P, 0, sizeof P);
        log_open();
        hlog("game starts: %d songs (%s%s), %d players", g_npool, G.set[HG_S_POOL] == 2 ? "playlist id " : G.set[HG_S_POOL] == 1 ? "folder " : "the whole library",
             G.set[HG_S_POOL] == 1 ? g_dir : "", G.np);
        if(G.set[HG_S_POOL] == 2) hlog("playlist id %ld", g_pl);
        { char s[512]; int o = 0; for(int k = 0; k < HG_S_N && o < (int)sizeof s; k++) o += snprintf(s + o, sizeof s - (size_t)o, " %s=%d", HG_KEYS[k].key, G.set[k]); hlog("setup:%s", s); }
        for(int i = 0; i < G.np; i++) hlog("player %d: %s", i + 1, G.p[i].name);
        hlog("%s begins", G.p[G.cur].name);
        break;
    case RMT_G_SKIP:
        if(G.phase == HG_LOBBY || G.phase == HG_END){ *why = "No game runs"; return -1; }
        hg_skip(&G, pool_left()); after_turn("the host skipped the turn"); break;
    case RMT_G_REMOVE:
        if(c->pos >= G.np){ *why = "No such player"; return -1; }
        hlog("the host removed %s", G.p[c->pos].name);
        { int was = G.cur; hg_remove(&G, (int)c->pos, pool_left()); if(G.cur != was || G.phase == HG_END) after_turn("a player was removed"); }
        break;
    case RMT_G_END:
        if(G.phase == HG_LOBBY || G.phase == HG_END){ *why = "No game runs"; return -1; }
        hg_end(&G, 2); after_turn("the host ended the game"); break;
    case RMT_G_FIXSONG:                                  /* the host: any song of this game (default the one now) */
        if(G.phase == HG_PLAY && (c->pos < 0 || c->pos == G.nt - 1)){ *why = "Reveal the song first"; return -1; }
        if(fix_turn(c->pos >= 0 ? (int)c->pos : G.nt - 1, c, "the host", why)) return -1;
        break;
    case RMT_G_AGAIN: hg_again(&G); memset(&P, 0, sizeof P); break;
    case RMT_G_RESET: hg_reset(&G, G.set); memset(&P, 0, sizeof P); break;
    default: *why = "Unknown action"; return -1;
    }
    *body = view_json(-1, 1);
    if(!*body){ *why = "Out of memory"; return -1; }
    return 0;
}
int hitguess_dispatch(const rmt_cmd_t *c, const char **why, char **body){
    *body = NULL;
    ready();
    int r;
    if(c->kind == RMT_CMD_GAME_ADMIN) r = host(c, why, body);
    else if(party_mode() != 2){ *why = "No game is on"; r = -1; }
    else r = player(c, why, body);
    loop_check();
    return r;
}
