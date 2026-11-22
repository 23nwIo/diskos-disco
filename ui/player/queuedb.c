/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "queuedb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The stock schema (V2.57, sqlite3 .schema on a Disc, 2026-11-15). LIST_SONG_3 has the same columns. */
#define LIST_COLS "(ID INTEGER PRIMARY KEY autoincrement, LIST_ID INT, POS_ID INT, PATH TEXT, NAME TEXT, TITLE TEXT, " \
    "ALBUM TEXT, ARTIST TEXT, GENRE TEXT, DISC INT, TRACK INT, IS_CUE INT, IS_ISO INT, OFFSET BIGINT, DURATION BIGINT, " \
    "ADD_TIME INT8, IS_SELECT INT, SONG_TYPE INT, ALBUM_ARTIST TEXT, IS_M3U INT, M3U_PATH TEXT)"

/* The stock orderings. Keep in step with ui/musicdb.c: ORDER_TAIL = mdb_play_pos ORDER_TAIL and MDB_FAV_ORDER (types
 * 1, 2, 6, 10), ORDER_ALBUM = MDB_ALBUM_ORDER (types 3, 7, 8). A custom playlist (type 5) plays in its row order. */
#define ORDER_TAIL \
    "CASE WHEN IS_CUE=0 AND IS_ISO=0 THEN 1 WHEN IS_CUE=1 OR IS_ISO=1 THEN 2 END," \
    "CASE WHEN (IS_CUE=0 AND IS_ISO=0) THEN CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END END," \
    "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN NAME_CODE END," \
    "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN ID END," \
    "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN TRACK END"
#define ORDER_ALBUM \
    "CASE WHEN DISC=0 THEN 1 ELSE 0 END,DISC,CASE WHEN TRACK=0 THEN 1 ELSE 0 END,TRACK," \
    "CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END"

sqlite3 *qd_open(const char *path){
    sqlite3 *d = NULL;
    if(sqlite3_open_v2(path, &d, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK){ if(d) sqlite3_close(d); return NULL; }
    sqlite3_busy_timeout(d, 3000);
    return d;
}

int qd_sys_get(const char *sysconfig_path, const char *col){
    sqlite3 *c = NULL; int v = -1;
    char sql[128]; snprintf(sql, sizeof sql, "SELECT %s FROM SYSCONFIG WHERE ID=1;", col);
    if(sqlite3_open_v2(sysconfig_path, &c, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(c, 500);
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(c, sql, -1, &st, NULL) == SQLITE_OK){
            if(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_INTEGER){
                int x = sqlite3_column_int(st, 0);
                if(x >= 0) v = x;
            }
            sqlite3_finalize(st);
        }
    }
    if(c) sqlite3_close(c);
    return v;
}

int qd_sys_set(const char *sysconfig_path, const char *col, int v){
    sqlite3 *c = NULL; int rc = -1;
    char sql[128]; snprintf(sql, sizeof sql, "UPDATE SYSCONFIG SET %s=%d WHERE ID=1;", col, v);
    if(sqlite3_open_v2(sysconfig_path, &c, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(c, 1000);
        if(sqlite3_exec(c, sql, NULL, NULL, NULL) == SQLITE_OK && sqlite3_changes(c) == 1) rc = 0;
    }
    if(c) sqlite3_close(c);
    return rc;
}

int qd_artist_class(const char *sysconfig_path){
    int v = qd_sys_get(sysconfig_path, "ARTIST_CLASS_TYPE");
    return (v == 0 || v == 1) ? v : -1;
}

static int has_col(sqlite3 *d, const char *table, const char *col){
    char sql[96]; snprintf(sql, sizeof sql, "PRAGMA table_info(%s);", table);
    sqlite3_stmt *st; int found = 0;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    while(!found && sqlite3_step(st) == SQLITE_ROW){
        const unsigned char *n = sqlite3_column_text(st, 1);
        if(n && !strcmp((const char *)n, col)) found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

/* The SELECT list for the stock INSERT columns from `table`; a column the table lacks (older firmware schemas) is
 * filled with the value the stock schema defaults to. */
static void select_list(sqlite3 *d, const char *table, char *out, size_t cap){
    static const struct { const char *col, *fallback; } C[] = {
        { "PATH", "NULL" }, { "NAME", "NULL" }, { "TITLE", "NULL" }, { "ALBUM", "NULL" }, { "ARTIST", "NULL" },
        { "ALBUM_ARTIST", "NULL" }, { "GENRE", "NULL" }, { "DISC", "0" }, { "TRACK", "0" }, { "DURATION", "0" },
        { "IS_CUE", "0" }, { "IS_ISO", "0" }, { "OFFSET", "0" }, { "IS_M3U", "0" }, { "M3U_PATH", "''" },
        { "ADD_TIME", "NULL" },
    };
    size_t len = 0; out[0] = 0;
    for(size_t i = 0; i < sizeof C / sizeof C[0] && len < cap; i++)
        len += (size_t)snprintf(out + len, cap - len, "%s%s", i ? ", " : "", has_col(d, table, C[i].col) ? C[i].col : C[i].fallback);
}

/* value of "key":"..." in the type 7/8 payload, read like the stock player (sscanf %[^"]: no escapes) */
static int payload_val(const char *s, const char *key, char *out, size_t cap){
    char k[32]; snprintf(k, sizeof k, "\"%s\":\"", key);
    const char *p = strstr(s, k);
    if(!p) return 0;
    p += strlen(k);
    const char *e = strchr(p, '"');
    if(!e || (size_t)(e - p) >= cap) return 0;
    memcpy(out, p, (size_t)(e - p)); out[e - p] = 0;
    return 1;
}

static int exec_(sqlite3 *d, const char *sql, char *err, int errcap){
    char *m = NULL;
    if(sqlite3_exec(d, sql, NULL, NULL, &m) == SQLITE_OK) return 0;
    if(err) snprintf(err, (size_t)errcap, "%s", m ? m : sqlite3_errmsg(d));
    sqlite3_free(m);
    return -1;
}

int qd_build(sqlite3 *d, int type, const char *name, int artist_class, char *err, int errcap){
    if(err && errcap > 0) err[0] = 0;
    if(!name) name = "";
    const char *table = "SONG", *where = "", *order = ORDER_TAIL;
    const char *p1 = NULL, *p2 = NULL; int seq = -1;
    char a[1024], b[1024];
    const char *artist_col = artist_class == 1 ? "COALESCE(ALBUM_ARTIST, ARTIST)" : "ARTIST";
    char wbuf[160];
    switch(type){
    case QD_ALL: break;
    case QD_ARTIST: snprintf(wbuf, sizeof wbuf, " WHERE %s=?1", artist_col); where = wbuf; p1 = name; break;
    case QD_ALBUM:  order = ORDER_ALBUM; if(name[0]){ where = " WHERE ALBUM=?1"; p1 = name; } else where = " WHERE ALBUM IS NULL"; break;
    case QD_GENRE:  if(name[0]){ where = " WHERE GENRE=?1"; p1 = name; } else where = " WHERE GENRE IS NULL"; break;
    case QD_FAVOURITES: table = "MY_LOVE"; break;
    case QD_PLAYLIST:
        /* the n-th registered playlist; diskOS always sends 0 = its reserved slot (ui/musicdb.c) */
        seq = name[0] ? atoi(name) : 0;
        table = "CUSTOM_PLAYLIST"; order = "ID";
        where = " WHERE PLAYLIST_ID=(SELECT LIST_ID FROM CUSTOM_PLAYLIST_INDEX ORDER BY LIST_ID LIMIT 1 OFFSET ?1)";
        break;
    case QD_ARTIST_ALBUM: case QD_GENRE_ALBUM:
        if(!payload_val(name, type == QD_ARTIST_ALBUM ? "artist" : "style", a, sizeof a) || !payload_val(name, "album", b, sizeof b)){
            if(err) snprintf(err, (size_t)errcap, "type %d: payload not understood: %.80s", type, name);
            return -1;
        }
        order = ORDER_ALBUM; p1 = a; p2 = b;
        if(type == QD_ARTIST_ALBUM) snprintf(wbuf, sizeof wbuf, " WHERE %s=?1 AND ALBUM=?2", artist_col);
        else snprintf(wbuf, sizeof wbuf, " WHERE GENRE=?1 AND ALBUM=?2");
        where = wbuf;
        break;
    default:
        if(err) snprintf(err, (size_t)errcap, "list type %d not supported", type);
        return -1;
    }
    char sel[600], sql[2400];
    select_list(d, table, sel, sizeof sel);
    snprintf(sql, sizeof sql,
             "INSERT INTO LIST_SONG_0 (PATH, NAME, TITLE, ALBUM, ARTIST, ALBUM_ARTIST, GENRE, DISC, TRACK, DURATION, "
             "IS_CUE, IS_ISO, OFFSET, IS_M3U, M3U_PATH, ADD_TIME, SONG_TYPE) SELECT %s, 3 FROM %s%s ORDER BY %s;",
             sel, table, where, order);

    if(exec_(d, "BEGIN IMMEDIATE;", err, errcap)) return -1;
    if(exec_(d, "DROP TABLE IF EXISTS LIST_SONG_3; DROP TABLE IF EXISTS LIST_SONG_0; CREATE TABLE LIST_SONG_0 " LIST_COLS ";",
             err, errcap)) goto fail;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK){
        if(err) snprintf(err, (size_t)errcap, "%s", sqlite3_errmsg(d));
        goto fail;
    }
    if(seq >= 0) sqlite3_bind_int(st, 1, seq);
    else if(p1) sqlite3_bind_text(st, 1, p1, -1, SQLITE_STATIC);
    if(p2) sqlite3_bind_text(st, 2, p2, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if(rc != SQLITE_DONE){ if(err) snprintf(err, (size_t)errcap, "%s", sqlite3_errmsg(d)); goto fail; }
    int n = sqlite3_changes(d);
    if(exec_(d, "COMMIT;", err, errcap)) goto fail;
    return n;
fail:
    sqlite3_exec(d, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
}

static int read_ints(sqlite3 *d, const char *sql, int **out){
    *out = NULL;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK){
        /* no such table: an empty queue, not an error */
        return strstr(sqlite3_errmsg(d), "no such table") ? 0 : -1;
    }
    int n = 0, cap = 0, rc; int *v = NULL;
    while((rc = sqlite3_step(st)) == SQLITE_ROW){
        if(n == cap){
            cap = cap ? cap * 2 : 256;
            int *nv = realloc(v, (size_t)cap * sizeof *v);
            if(!nv){ free(v); sqlite3_finalize(st); return -1; }
            v = nv;
        }
        v[n++] = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    if(rc != SQLITE_DONE){ free(v); return -1; }
    *out = v;
    return n;
}

int qd_ids(sqlite3 *d, int **ids){ return read_ints(d, "SELECT ID FROM LIST_SONG_0 ORDER BY ID;", ids); }
int qd_shuffle_read(sqlite3 *d, int **order){ return read_ints(d, "SELECT POS_ID FROM LIST_SONG_3 ORDER BY ID;", order); }

int qd_shuffle_drop(sqlite3 *d){ return exec_(d, "DROP TABLE IF EXISTS LIST_SONG_3;", NULL, 0); }

int qd_shuffle_write(sqlite3 *d, const int *order, int n){
    if(exec_(d, "BEGIN IMMEDIATE;", NULL, 0)) return -1;
    if(exec_(d, "DROP TABLE IF EXISTS LIST_SONG_3; CREATE TABLE LIST_SONG_3 " LIST_COLS ";", NULL, 0)) goto fail;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "INSERT INTO LIST_SONG_3 (POS_ID) VALUES (?1);", -1, &st, NULL) != SQLITE_OK) goto fail;
    for(int i = 0; i < n; i++){
        sqlite3_bind_int(st, 1, order[i]);
        if(sqlite3_step(st) != SQLITE_DONE){ sqlite3_finalize(st); goto fail; }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    if(exec_(d, "COMMIT;", NULL, 0)) goto fail;
    return 0;
fail:
    sqlite3_exec(d, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
}

static void col_(sqlite3_stmt *st, int i, char *out, size_t cap){
    const unsigned char *t = sqlite3_column_text(st, i);
    snprintf(out, cap, "%s", t ? (const char *)t : "");
}

static int row_love(sqlite3 *d, qd_row_t *r);
int qd_row(sqlite3 *d, int id, qd_row_t *r){
    memset(r, 0, sizeof *r);
    sqlite3_stmt *st;
    /* SONG.ID by the same join the stock player's Up Next statement uses (PATH, IS_CUE, IS_ISO, TRACK) */
    if(sqlite3_prepare_v2(d,
        "SELECT L.ID, L.PATH, COALESCE(L.TITLE, L.NAME), L.ARTIST, L.ALBUM, L.GENRE, IFNULL(L.TRACK,0), IFNULL(L.IS_CUE,0),"
        " IFNULL(L.IS_ISO,0), IFNULL(L.IS_M3U,0), L.M3U_PATH, IFNULL(L.OFFSET,0), IFNULL(L.DURATION,0),"
        " (SELECT S.ID FROM SONG S WHERE S.PATH=L.PATH AND IFNULL(S.IS_CUE,0)=IFNULL(L.IS_CUE,0)"
        "  AND IFNULL(S.IS_ISO,0)=IFNULL(L.IS_ISO,0) AND IFNULL(S.TRACK,0)=IFNULL(L.TRACK,0) LIMIT 1)"
        " FROM LIST_SONG_0 L WHERE L.ID=?1;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, id);
    int rc = sqlite3_step(st);
    if(rc == SQLITE_ROW){
        r->id = sqlite3_column_int(st, 0);
        col_(st, 1, r->path, sizeof r->path);
        col_(st, 2, r->title, sizeof r->title);
        col_(st, 3, r->artist, sizeof r->artist);
        col_(st, 4, r->album, sizeof r->album);
        col_(st, 5, r->genre, sizeof r->genre);
        r->track = sqlite3_column_int(st, 6);
        r->is_cue = sqlite3_column_int(st, 7);
        r->is_iso = sqlite3_column_int(st, 8);
        r->is_m3u = sqlite3_column_int(st, 9);
        col_(st, 10, r->m3u_path, sizeof r->m3u_path);
        r->offset = sqlite3_column_int64(st, 11);
        r->duration_ms = sqlite3_column_int64(st, 12);
        r->song_id = sqlite3_column_int(st, 13);
    }
    sqlite3_finalize(st);
    if(rc == SQLITE_DONE) return 0;
    if(rc != SQLITE_ROW) return -1;
    return row_love(d, r);
}
int qd_song(sqlite3 *d, int song_id, qd_row_t *r){
    memset(r, 0, sizeof *r);
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d,
        "SELECT ID, PATH, COALESCE(TITLE, NAME), ARTIST, ALBUM, GENRE, IFNULL(TRACK,0), IFNULL(IS_CUE,0),"
        " IFNULL(IS_ISO,0), 0, '', IFNULL(OFFSET,0), IFNULL(DURATION,0)"   /* IS_M3U/M3U_PATH: not in every SONG schema */
        " FROM SONG WHERE ID=?1;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, song_id);
    int rc = sqlite3_step(st);
    if(rc == SQLITE_ROW){
        r->song_id = sqlite3_column_int(st, 0);
        col_(st, 1, r->path, sizeof r->path);
        col_(st, 2, r->title, sizeof r->title);
        col_(st, 3, r->artist, sizeof r->artist);
        col_(st, 4, r->album, sizeof r->album);
        col_(st, 5, r->genre, sizeof r->genre);
        r->track = sqlite3_column_int(st, 6);
        r->is_cue = sqlite3_column_int(st, 7);
        r->is_iso = sqlite3_column_int(st, 8);
        r->is_m3u = sqlite3_column_int(st, 9);
        col_(st, 10, r->m3u_path, sizeof r->m3u_path);
        r->offset = sqlite3_column_int64(st, 11);
        r->duration_ms = sqlite3_column_int64(st, 12);
    }
    sqlite3_finalize(st);
    if(rc == SQLITE_DONE) return 0;
    if(rc != SQLITE_ROW) return -1;
    return row_love(d, r);
}
static int row_love(sqlite3 *d, qd_row_t *r){
    sqlite3_stmt *st;
    /* favourite: MY_LOVE is keyed by (PATH, TRACK); a missing table only means "no favourites" */
    if(sqlite3_prepare_v2(d, "SELECT 1 FROM MY_LOVE WHERE PATH=?1 AND IFNULL(TRACK,0)=?2 LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, r->path, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 2, r->track);
        r->love = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    return 1;
}

int qd_memory_get(sqlite3 *d, int *music_id, long *pos_ms, int *track){
    sqlite3_stmt *st;
    *music_id = 0; *pos_ms = 0; *track = 0;
    if(sqlite3_prepare_v2(d, "SELECT IFNULL(MUSIC_ID,0), IFNULL(POSITION,0), IFNULL(TRACK,0) FROM MEMORY_PLAY WHERE ID=1;",
                          -1, &st, NULL) != SQLITE_OK) return -1;
    int rc = sqlite3_step(st);
    if(rc == SQLITE_ROW){
        *music_id = sqlite3_column_int(st, 0);
        *pos_ms = (long)sqlite3_column_int64(st, 1);
        *track = sqlite3_column_int(st, 2);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

int qd_memory_set(sqlite3 *d, int music_id, long pos_ms, int is_cue, int is_iso, int track){
    for(int pass = 0; pass < 2; pass++){
        sqlite3_stmt *st;
        if(sqlite3_prepare_v2(d, "UPDATE MEMORY_PLAY SET MUSIC_ID=?1, IS_PLAYING=1, POSITION=?2, IS_CUE=?3, IS_ISO=?4,"
                                 " TRACK=?5, IS_NAS=0 WHERE ID=1;", -1, &st, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_int(st, 1, music_id);
        sqlite3_bind_int64(st, 2, pos_ms);
        sqlite3_bind_int(st, 3, is_cue);
        sqlite3_bind_int(st, 4, is_iso);
        sqlite3_bind_int(st, 5, track);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if(rc != SQLITE_DONE) return -1;
        if(sqlite3_changes(d) == 1) return 0;
        /* no row yet: the stock player's default row (ID 1) */
        if(pass || exec_(d, "INSERT INTO MEMORY_PLAY (MUSIC_ID) VALUES (0);", NULL, 0) != 0) return -1;
    }
    return -1;
}

int qd_memory_pos(sqlite3 *d, long pos_ms){
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "UPDATE MEMORY_PLAY SET POSITION=?1 WHERE ID=1;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, pos_ms);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int qd_list_row_of_song(sqlite3 *d, int music_id){
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d,
        "SELECT L.ID FROM LIST_SONG_0 L JOIN SONG S ON S.PATH=L.PATH AND IFNULL(S.IS_CUE,0)=IFNULL(L.IS_CUE,0)"
        " AND IFNULL(S.IS_ISO,0)=IFNULL(L.IS_ISO,0) AND IFNULL(S.TRACK,0)=IFNULL(L.TRACK,0)"
        " WHERE S.ID=?1 ORDER BY L.ID LIMIT 1;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, music_id);
    int rc = sqlite3_step(st), id = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? id : -1;
}
