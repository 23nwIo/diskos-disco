/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The library database: which song.db diskOS uses, and how every connection to it is opened.
 *
 * Plan (Jo, 2026-11-22; docs/LIBRARY_DB.md once it lands): the own player gets its own copy of the library on the SD
 * card, <card>/.diskos/state/library.db, so each card carries the library of its own files. The stock player keeps
 * using and updating /usr/data/fiio/db/song.db on the internal flash, the fallback when the Disc boots to the stock
 * player. Every component that opens the library (UI, scanner, own player, stats, Album Roulette, SongSphere, Disc
 * Shelf) asks libdb_path() which file that is and opens it through libdb_open_v2(), never with a path of its own.
 *
 * Step 1 (this header as it is now): one place for the path and the open; libdb_path() still always answers the flash
 * DB, so nothing changes on the device.
 *
 * libdb_open_v2() takes the same arguments as sqlite3_open_v2(). On success it applies the per-connection settings
 * the card DB needs (libdb_setup); on failure it closes the handle sqlite3_open_v2() may still have returned, so the
 * caller never leaks one, and *out is NULL.
 *
 * Header-only (like own_player.h) so the player binary and the host tests use it without another source file. */
#ifndef DISKOS_LIBDB_H
#define DISKOS_LIBDB_H
#include <string.h>
#include "sqlite3.h"

#define LIBDB_FLASH "/usr/data/fiio/db/song.db"            /* stock player's library, internal NAND */
#define LIBDB_CARD  "/tmp/sdcard/.diskos/state/library.db" /* own player's library, on the card (not used yet) */

/* The library DB in use now. */
static inline const char *libdb_path(void){ return LIBDB_FLASH; }

/* 1 when path names the card DB (any path ending in /library.db, so host tests can use a temp dir). */
static inline int libdb_is_card(const char *path){
    size_t n = path ? strlen(path) : 0, k = sizeof "/library.db" - 1;
    return n >= k && !strcmp(path + n - k, "/library.db");
}

/* Per-connection settings. Only the card DB gets them: the flash DB is the stock player's, left exactly as it is.
 *  - foreign_keys=ON: SOURCE_ID.SONG_ID references SONG(ID) ON DELETE CASCADE, which SQLite only enforces on a
 *    connection that switched it on, so every connection must.
 *  - journal_mode=TRUNCATE: a rollback journal, not WAL (WAL needs a shared-memory -shm file mapped by every process,
 *    risky on FAT/exFAT). TRUNCATE empties the journal instead of deleting it, fewer directory writes on FAT.
 *    A read-only connection keeps the mode it gets (it cannot change it).
 *  - synchronous=FULL: the journal reaches the card before the DB is changed, so a pulled card or lost power rolls
 *    the last transaction back on the next open instead of leaving a broken file. */
static inline void libdb_setup(sqlite3 *d, const char *path, int flags){
    if(!d || !libdb_is_card(path)) return;
    sqlite3_exec(d, "PRAGMA foreign_keys=ON;", 0, 0, 0);
    if(flags & SQLITE_OPEN_READWRITE){
        sqlite3_exec(d, "PRAGMA journal_mode=TRUNCATE;", 0, 0, 0);
        sqlite3_exec(d, "PRAGMA synchronous=FULL;", 0, 0, 0);
    }
}

static inline int libdb_open_v2(const char *path, sqlite3 **out, int flags, const char *vfs){
    *out = NULL;
    int rc = sqlite3_open_v2(path, out, flags, vfs);
    if(rc != SQLITE_OK){ if(*out) sqlite3_close(*out); *out = NULL; return rc; }
    libdb_setup(*out, path, flags);
    return SQLITE_OK;
}
#endif
