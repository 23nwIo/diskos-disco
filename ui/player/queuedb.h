/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The queue tables in song.db, written exactly as the stock player writes them (docs/PLAYER_PROTOCOL.md, "Shared data
 * the player uses"): the UI reads LIST_SONG_0 and LIST_SONG_3 for Up Next and computes every 0100 position against
 * the orderings below (ui/musicdb.c mdb_play_pos, MDB_ALBUM_ORDER, MDB_FAV_ORDER), so they have to stay in step. */
#ifndef DISKOS_PLAYER_QUEUEDB_H
#define DISKOS_PLAYER_QUEUEDB_H
#include "../sqlite3.h"

#define QD_SONG_DB      "/usr/data/fiio/db/song.db"
#define QD_SYSCONFIG_DB "/usr/data/fiio/db/sysconfig.db"

/* 0100 list types */
enum { QD_RESUME = 0, QD_ALL = 1, QD_ARTIST = 2, QD_ALBUM = 3, QD_PLAYLIST = 5, QD_FAVOURITES = 6,
       QD_ARTIST_ALBUM = 7, QD_GENRE_ALBUM = 8, QD_GENRE = 10 };

/* Open song.db read-write with a busy timeout (the UI reads and writes it too). NULL on failure. */
sqlite3 *qd_open(const char *path);

/* An integer column of the single SYSCONFIG row (the stock player's settings): the value (>= 0), or -1 when it can't
 * be read. qd_sys_set writes one (col is a constant, never user text): 0 or -1. */
int qd_sys_get(const char *sysconfig_path, const char *col);
int qd_sys_set(const char *sysconfig_path, const char *col, int v);
/* SYSCONFIG.ARTIST_CLASS_TYPE (0 ARTIST, 1 album artist), -1 if it can't be read. */
int qd_artist_class(const char *sysconfig_path);

/* Rebuild LIST_SONG_0 for a 0100 list (type 1..10, name as sent) like the stock player: DROP, CREATE with the stock
 * schema (so the IDs start at 1 again), INSERT ... SELECT with the stock filter and ORDER BY. artist_class selects the
 * artist column for types 2 and 7. Drops LIST_SONG_3 (a new list starts unshuffled; the caller writes a new one).
 * Returns the number of rows, or -1 (err says why). */
int qd_build(sqlite3 *db, int type, const char *name, int artist_class, char *err, int errcap);

/* LIST_SONG_0.ID in ID order (malloc'd into *ids). Count (0 = empty or no table), -1 on a database error. */
int qd_ids(sqlite3 *db, int **ids);
/* LIST_SONG_3's order (its POS_ID values in ID order, malloc'd). Count, 0 if there is none, -1 on error. */
int qd_shuffle_read(sqlite3 *db, int **order);
/* Write LIST_SONG_3 (DROP, CREATE, one POS_ID row per entry in play order) / drop it. 0 or -1. */
int qd_shuffle_write(sqlite3 *db, const int *order, int n);
int qd_shuffle_drop(sqlite3 *db);

/* One queue row with what the a2 song JSON needs. */
typedef struct {
    int id;                        /* LIST_SONG_0.ID (pos_id) */
    int song_id;                   /* SONG.ID of the same track, 0 if not in the library */
    char path[1024];
    char title[512], artist[512], album[512], genre[256], m3u_path[1024];
    int track, is_cue, is_iso, is_m3u, love;
    long long offset, duration_ms;
} qd_row_t;
/* Row id of LIST_SONG_0. 1 found, 0 not there, -1 error. */
int qd_row(sqlite3 *db, int id, qd_row_t *r);
#endif
