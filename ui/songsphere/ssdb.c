/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SongSphere data. Its own file (sphere.db in the app's folder) holds the placements: one row per placed song, album
 * or artist (kind, key, x, y, when). Songs are keyed by file path, albums by album name (as the Library groups them),
 * artists by album artist, else artist. The library itself (song.db) is only read, never written. A song's spot is
 * its own, else its album's, else its artist's. */
#include "ssdb.h"
#include "sqlite3.h"
#include "../libdb.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static sqlite3 *g_lib, *g_ss;                          /* the library (read-only, with ours attached), ours (writes) */
static int g_has_stats;                                /* PLAY_STATS exists: plays and recent plays are known */

static const char *g_lib_path, *g_ss_path;            /* set by ssdb_paths() (mq_ui), else the app's own */
void ssdb_paths(const char *lib, const char *ss){ g_lib_path = lib; g_ss_path = ss; }
static const char *lib_path(void){ const char *p = getenv("SS_LIB"); return p ? p : g_lib_path ? g_lib_path : libdb_path(); }
static const char *ss_path(void){ const char *p = getenv("SS_DB"); return p ? p : g_ss_path ? g_ss_path : "/usr/data/apps/songsphere/sphere.db"; }

int ssdb_open(void){
    if(g_lib) return 1;
    if(sqlite3_open_v2(ss_path(), &g_ss, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) goto fail;
    sqlite3_busy_timeout(g_ss, 1500);
    if(sqlite3_exec(g_ss, "CREATE TABLE IF NOT EXISTS PLACE(KIND INTEGER NOT NULL, KEY TEXT NOT NULL, X REAL NOT NULL, "
                          "Y REAL NOT NULL, AT INTEGER NOT NULL, PRIMARY KEY(KIND, KEY)) WITHOUT ROWID;", 0, 0, 0) != SQLITE_OK) goto fail;
    if(libdb_open_v2(lib_path(), &g_lib, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto fail;
    sqlite3_busy_timeout(g_lib, 1500);
    char *q = sqlite3_mprintf("ATTACH %Q AS ss;", ss_path());
    int rc = sqlite3_exec(g_lib, q, 0, 0, 0); sqlite3_free(q);
    if(rc != SQLITE_OK) goto fail;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(g_lib, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='PLAY_STATS';", -1, &st, NULL) == SQLITE_OK){
        g_has_stats = sqlite3_step(st) == SQLITE_ROW; sqlite3_finalize(st);
    }
    return 1;
fail:
    ssdb_close();
    return 0;
}
void ssdb_close(void){
    if(g_lib) sqlite3_close(g_lib);
    if(g_ss) sqlite3_close(g_ss);
    g_lib = g_ss = NULL;
}
static const char *col(sqlite3_stmt *st, int i){ const char *t = (const char *)sqlite3_column_text(st, i); return t ? t : ""; }

#define OWNER "COALESCE(NULLIF(ALBUM_ARTIST,''),ARTIST,'')"
int ssdb_items(int kind, ss_item_t **out){
    *out = NULL;
    if(!g_lib) return -1;
    char sql[1400];
    const char *grp = kind == SS_ALBUM ? "ALBUM" : OWNER;
    snprintf(sql, sizeof sql,
        "WITH A AS (SELECT %s AS K, MIN(O) AS MN, MAX(O) AS MX, COUNT(*) AS N FROM (SELECT ALBUM, ARTIST, ALBUM_ARTIST, %s AS O "
        "FROM SONG WHERE PATH NOT LIKE '%%.m4b') WHERE IFNULL(%s,'')<>'' GROUP BY 1)%s "
        "SELECT A.K, A.MN, A.MX, A.N, %s, X.X, X.Y FROM A %s LEFT JOIN ss.PLACE X ON X.KIND=%d AND X.KEY=A.K "
        "ORDER BY 5 DESC, A.K COLLATE NOCASE;",
        grp, OWNER, grp,
        g_has_stats ? (kind == SS_ALBUM ? ", P AS (SELECT S.ALBUM AS K, SUM(P.PLAYS) AS PL FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH GROUP BY 1)"
                                        : ", P AS (SELECT " "COALESCE(NULLIF(S.ALBUM_ARTIST,''),S.ARTIST,'')" " AS K, SUM(P.PLAYS) AS PL FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH GROUP BY 1)")
                    : "",
        g_has_stats ? "IFNULL(P.PL,0)" : "0", g_has_stats ? "LEFT JOIN P ON P.K=A.K" : "", kind);
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(g_lib, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int cap = 256, n = 0; ss_item_t *v = malloc((size_t)cap * sizeof *v);
    while(v && sqlite3_step(st) == SQLITE_ROW){
        if(n == cap){ cap *= 2; ss_item_t *nv = realloc(v, (size_t)cap * sizeof *v); if(!nv){ free(v); v = NULL; break; } v = nv; }
        ss_item_t *it = &v[n++]; memset(it, 0, sizeof *it);
        snprintf(it->name, sizeof it->name, "%s", col(st, 0));
        if(kind == SS_ALBUM) snprintf(it->sub, sizeof it->sub, "%s", strcmp(col(st, 1), col(st, 2)) ? "Various Artists" : col(st, 1));
        it->songs = sqlite3_column_int(st, 3); it->plays = sqlite3_column_int(st, 4);
        it->placed = sqlite3_column_type(st, 5) != SQLITE_NULL;
        it->x = (float)sqlite3_column_double(st, 5); it->y = (float)sqlite3_column_double(st, 6);
    }
    sqlite3_finalize(st);
    if(!v) return -1;
    *out = v;
    return n;
}
int ssdb_get(int kind, const char *key, float *x, float *y){
    sqlite3_stmt *st; int ok = 0;
    if(!g_ss || sqlite3_prepare_v2(g_ss, "SELECT X,Y FROM PLACE WHERE KIND=?1 AND KEY=?2;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, kind); sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC);
    if(sqlite3_step(st) == SQLITE_ROW){ *x = (float)sqlite3_column_double(st, 0); *y = (float)sqlite3_column_double(st, 1); ok = 1; }
    sqlite3_finalize(st);
    return ok;
}
int ssdb_put(int kind, const char *key, float x, float y){
    sqlite3_stmt *st; int ok = 0;
    if(!g_ss || sqlite3_prepare_v2(g_ss, "INSERT OR REPLACE INTO PLACE(KIND,KEY,X,Y,AT) VALUES(?1,?2,?3,?4,?5);", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, kind); sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 3, roundf(x * 1000) / 1000); sqlite3_bind_double(st, 4, roundf(y * 1000) / 1000);
    sqlite3_bind_int64(st, 5, (long long)time(NULL));
    ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}
int ssdb_del(int kind, const char *key){
    sqlite3_stmt *st; int ok = 0;
    if(!g_ss || sqlite3_prepare_v2(g_ss, "DELETE FROM PLACE WHERE KIND=?1 AND KEY=?2;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, kind); sqlite3_bind_text(st, 2, key, -1, SQLITE_STATIC);
    ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}
int ssdb_coverage(int *placed, int *albums){
    sqlite3_stmt *st; *placed = *albums = 0;
    if(!g_lib || sqlite3_prepare_v2(g_lib, "SELECT COUNT(DISTINCT S.ALBUM), COUNT(DISTINCT X.KEY) FROM SONG S LEFT JOIN ss.PLACE X "
                                           "ON X.KIND=1 AND X.KEY=S.ALBUM WHERE IFNULL(S.ALBUM,'')<>'' AND S.PATH NOT LIKE '%.m4b';",
                                    -1, &st, NULL) != SQLITE_OK) return 0;
    if(sqlite3_step(st) == SQLITE_ROW){ *albums = sqlite3_column_int(st, 0); *placed = sqlite3_column_int(st, 1); }
    sqlite3_finalize(st);
    return 1;
}

/* ---- the playlist ---- */
typedef struct { int id; float d; unsigned alb, art; int recent, taken; } cand_t;
static unsigned hstr(const char *s){ unsigned h = 2166136261u; for(; *s; s++){ h ^= (unsigned char)*s; h *= 16777619u; } return h ? h : 1; }
#define HN 4096
typedef struct { unsigned k[HN]; unsigned char c[HN]; } counts_t;   /* how many picked per album / artist */
/* Only songs actually picked are counted (at most `want` keys, far below HN), so the table never fills; the probe is
 * bounded all the same. cnt_get: 0 for a key not seen; cnt_add: count one more (0 if, impossibly, the table is full). */
static int cnt_slot(const counts_t *t, unsigned k){
    unsigned i = k & (HN - 1);
    for(int n = 0; n < HN; n++, i = (i + 1) & (HN - 1)) if(!t->k[i] || t->k[i] == k) return (int)i;
    return -1;
}
static unsigned cnt_get(const counts_t *t, unsigned k){ int i = cnt_slot(t, k); return i >= 0 && t->k[i] == k ? t->c[i] : 0; }
static int cnt_add(counts_t *t, unsigned k){
    int i = cnt_slot(t, k); if(i < 0) return 0;
    t->k[i] = k; if(t->c[i] < 255) t->c[i]++;
    return 1;
}
static unsigned rnd_state;
static float frand(void){ rnd_state = rnd_state * 1103515245u + 12345u; return (float)((rnd_state >> 8) & 0xFFFFFF) / 16777216.0f; }

int ssdb_pick(float x, float y, int want, int **ids, float *radius_out){
    *ids = NULL; if(radius_out) *radius_out = 0;
    if(!g_lib || want <= 0) return 0;
    char sql[1200];
    snprintf(sql, sizeof sql,
        "SELECT S.ID, IFNULL(S.ALBUM,''), " "COALESCE(NULLIF(S.ALBUM_ARTIST,''),S.ARTIST,'')" ", COALESCE(a.X,b.X,c.X), COALESCE(a.Y,b.Y,c.Y), %s "
        "FROM SONG S LEFT JOIN ss.PLACE a ON a.KIND=0 AND a.KEY=S.PATH LEFT JOIN ss.PLACE b ON b.KIND=1 AND b.KEY=S.ALBUM "
        "LEFT JOIN ss.PLACE c ON c.KIND=2 AND c.KEY=COALESCE(NULLIF(S.ALBUM_ARTIST,''),S.ARTIST) %s "
        "WHERE S.PATH NOT LIKE '%%.m4b' AND COALESCE(a.X,b.X,c.X) IS NOT NULL;",
        g_has_stats ? "IFNULL(P.LAST_PLAYED,0)" : "0", g_has_stats ? "LEFT JOIN PLAY_STATS P ON P.PATH=S.PATH" : "");
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(g_lib, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    long long recent_after = (long long)time(NULL) - 3 * 86400;
    int cap = 1024, n = 0; cand_t *c = malloc((size_t)cap * sizeof *c);
    while(c && sqlite3_step(st) == SQLITE_ROW){
        if(n == cap){ cap *= 2; cand_t *nc = realloc(c, (size_t)cap * sizeof *c); if(!nc){ free(c); c = NULL; break; } c = nc; }
        float dx = (float)sqlite3_column_double(st, 3) - x, dy = (float)sqlite3_column_double(st, 4) - y;
        c[n].id = sqlite3_column_int(st, 0); c[n].d = sqrtf(dx * dx + dy * dy);
        c[n].alb = hstr(col(st, 1)); c[n].art = hstr(col(st, 2));
        long long last = sqlite3_column_int64(st, 5);
        c[n].recent = last > recent_after; c[n].taken = 0; n++;
    }
    sqlite3_finalize(st);
    if(!c || n == 0){ free(c); return 0; }
    rnd_state = (unsigned)time(NULL) ^ (unsigned)(x * 7919) ^ (unsigned)(y * 104729);
    int *out = malloc((size_t)want * sizeof *out); counts_t *t = calloc(2, sizeof *t);
    float *w = malloc((size_t)n * sizeof *w);
    if(!out || !t || !w){ free(out); free(t); free(w); free(c); return 0; }
    /* Look a little further each round until there are enough. Nearer songs are likelier, never certain (so the same
     * spot doesn't always give the same list); at most 3 per album and 4 per artist; songs played in the last three
     * days much less likely. If the caps leave too few even on the whole sphere, a last round drops them. */
    int got = 0; float r = 0.30f;
    for(int round = 0; got < want; round++){
        int capped = r < 2.0f;
        double total = 0; int avail = 0;
        for(int i = 0; i < n; i++){
            w[i] = 0;
            if(c[i].taken || c[i].d > r) continue;
            float k = 1 - c[i].d / (r + 1e-3f); w[i] = k * k + 0.05f;
            if(c[i].recent) w[i] *= 0.15f;
            total += w[i]; avail++;
        }
        while(got < want && total > 1e-9 && avail > 0){
            double pick = frand() * total; int i = 0;
            for(; i < n; i++){ if(w[i] <= 0) continue; pick -= w[i]; if(pick <= 0) break; }
            if(i >= n){ for(i = n - 1; i >= 0 && w[i] <= 0; i--); if(i < 0) break; }
            total -= w[i]; avail--; float wi = w[i]; w[i] = 0; (void)wi;
            if(capped && (cnt_get(&t[0], c[i].alb) >= 3 || cnt_get(&t[1], c[i].art) >= 4)) continue;   /* full for now: may come back in the last round */
            cnt_add(&t[0], c[i].alb); cnt_add(&t[1], c[i].art);         /* only what is taken is ever counted */
            c[i].taken = 1; out[got++] = c[i].id;
        }
        if(got >= want) break;
        if(r >= 2.0f){ if(!capped || round > 40) break; }
        r = r >= 2.0f ? 2.01f : fminf(2.0f, r + 0.12f);
    }
    free(t); free(w); free(c);
    if(radius_out) *radius_out = r > 2.0f ? 2.0f : r;
    if(!got){ free(out); return 0; }
    *ids = out;
    return got;
}
