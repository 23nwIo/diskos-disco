/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The library reads of the own player. A 0100 list is read as SONG.IDs into RAM (qd_list); unlike the stock player
 * nothing is written to LIST_SONG_0/LIST_SONG_3 (Jo, 2026-11-22: no flash writes per list; the UI reads the list
 * from OPL_LIST in /tmp). The UI computes every 0100 position against the orderings in queuedb.c (ui/musicdb.c
 * mdb_play_pos, MDB_ALBUM_ORDER, MDB_FAV_ORDER), so they have to stay in step. */
#ifndef DISKOS_PLAYER_QUEUEDB_H
#define DISKOS_PLAYER_QUEUEDB_H
#include "../sqlite3.h"

#include "../libdb.h"
#define QD_SONG_DB      libdb_path()   /* the library in use (ui/libdb.h) */
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

/* The SONG.IDs of a 0100 list (type 1..10, name as sent) in the stock order (filter and ORDER BY as the stock player
 * builds LIST_SONG_0), malloc'd into *song_ids. artist_class selects the artist column for types 2 and 7. Favourites
 * and playlist rows whose song is no longer in the library are left out (*dropped counts them). Returns the count, or
 * -1 (err says why). Reads only. */
int qd_list(sqlite3 *db, int type, const char *name, int artist_class, int **song_ids, int *dropped, char *err, int errcap);

/* One queue row with what the a2 song JSON needs. */
typedef struct {
    int id;                        /* 1-based position in the playing list (the a2 pos_id), 0 = not from the list */
    int song_id;                   /* SONG.ID of the same track, 0 if not in the library */
    char path[1024];
    char title[512], artist[512], album[512], genre[256], m3u_path[1024];
    int track, is_cue, is_iso, is_m3u, love;
    long long offset, duration_ms;
} qd_row_t;
/* A library song (SONG.ID): r->id is 0 (the caller sets the list position). 1 found, 0 not there, -1 error. */
int qd_song(sqlite3 *db, int song_id, qd_row_t *r);

/* MEMORY_PLAY, the resume bookmark (one row, ID 1; the stock player's statements, `strings mq_player`):
 * MUSIC_ID = SONG.ID, POSITION in ms. qd_memory_get: 1 and the values, 0 no row, -1 error (no table). */
int qd_memory_get(sqlite3 *db, int *music_id, long *pos_ms, int *track);
/* A new song: the whole row (IS_PLAYING 1 and IS_NAS 0 as the stock player writes them); the row is added when it is
 * missing. qd_memory_pos: only POSITION. 0 or -1. */
int qd_memory_set(sqlite3 *db, int music_id, long pos_ms, int is_cue, int is_iso, int track);
int qd_memory_pos(sqlite3 *db, long pos_ms);
/* 0104, the heart: the song r (a qd_song row) into MY_LOVE (on) or out of it, keyed by PATH and TRACK like row_love.
 * The row copies SONG's columns that MY_LOVE also has, as the stock player and musicdb.c write it. 0 or -1. */
int qd_love_set(sqlite3 *db, const qd_row_t *r, int on);
#endif
