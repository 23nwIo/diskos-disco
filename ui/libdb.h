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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sqlite3.h"

#define LIBDB_FLASH "/usr/data/fiio/db/song.db"            /* stock player's library, internal NAND */
#define LIBDB_CARD_ROOT "/tmp/sdcard"
#define LIBDB_CARD  LIBDB_CARD_ROOT "/.diskos/state/library.db" /* own player's library, on the card */

/* The card library is on (2026-11-23): mq_ui closes its files when the card goes away (libdb_vfs.c), and moves the
 * library to the card with the overlay (libdb_ui.c). Build with -DLIBDB_CARD_ENABLED=0 to keep the flash DB only. */
#ifndef LIBDB_CARD_ENABLED
#define LIBDB_CARD_ENABLED 1
#endif
/* The own player writes its pid here when it starts (player/main.c, tmpfs: gone at reboot). Not the switch file
 * /usr/data/diskos-own-player: that one names the NEXT boot's player (Settings > Maintenance changes it without a
 * restart), and the stock player may run although it exists (the own player failed). */
#define LIBDB_OWN_PID "/tmp/.diskos_libdb_own_pid"
#define LIBDB_OWN_BIN "/usr/data/diskos-own/mq_player"    /* = OPL_BIN (own_player.h) */
#include <stdio.h>
static inline int libdb_own_player_runs(void){
    char b[32] = "", p[48], x[64];
    FILE *f = fopen(LIBDB_OWN_PID, "r");
    if(!f) return 0;
    int ok = fgets(b, sizeof b, f) != NULL; fclose(f);
    long pid = ok ? strtol(b, NULL, 10) : 0;
    if(pid <= 0) return 0;
    snprintf(p, sizeof p, "/proc/%ld/exe", pid);
    ssize_t n = readlink(p, x, sizeof x - 1);
    if(n <= 0) return 0;
    x[n] = 0;
    return !strcmp(x, LIBDB_OWN_BIN);
}

/* The library DB in use now: the card DB when the own player runs and the mounted card has one, else the flash DB.
 * A card without library.db (not moved yet) keeps the flash DB, as before. */
static inline const char *libdb_path(void){
#if LIBDB_CARD_ENABLED
    struct stat c, p;
    if(stat(LIBDB_CARD_ROOT, &c) == 0 && stat("/tmp", &p) == 0 && c.st_dev != p.st_dev
       && access(LIBDB_CARD, R_OK) == 0 && libdb_own_player_runs())
        return LIBDB_CARD;
#endif
    return LIBDB_FLASH;
}

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

/* The card DB's file layer in mq_ui (libdb_vfs.c): files that can be closed under open connections when SD access
 * is held. Registered by mq_ui only; elsewhere (the own player, host tests) the card DB opens with SQLite's own VFS.
 * Weak, so code built without libdb_vfs.c still links: libdb_vfs_gen is then NULL. */
#define LIBDB_VFS "diskos-libdb"
int libdb_vfs_register(void);                         /* 0 ok */
int libdb_vfs_detach(void);                           /* SD held: close every card DB file; how many */
void libdb_vfs_allow(int on);                         /* SD resumed (1): card DB opens work again */
__attribute__((weak)) unsigned libdb_vfs_gen(void);   /* bumped by each detach that closed something */
static inline unsigned libdb_gen(void){ return libdb_vfs_gen ? libdb_vfs_gen() : 0; }

static inline int libdb_open_v2(const char *path, sqlite3 **out, int flags, const char *vfs){
    *out = NULL;
    if(!vfs && libdb_is_card(path) && sqlite3_vfs_find(LIBDB_VFS)) vfs = LIBDB_VFS;
    int rc = sqlite3_open_v2(path, out, flags, vfs);
    if(rc != SQLITE_OK){ if(*out) sqlite3_close(*out); *out = NULL; return rc; }
    libdb_setup(*out, path, flags);
    return SQLITE_OK;
}

/* The tables diskOS adds to the card DB. Idempotent; 0 or an SQLite error code.
 * SOURCE_ID maps a song to its ID at an outside source (SOURCE 'bandcamp' now; 'navidrome', 'rsync', ... later).
 * The rows go with their song (ON DELETE CASCADE). A rescan keeps SONG.ID (it updates rows by PATH); a moved or
 * renamed file gets a new ID and the scanner fills its row again from the file's tags (docs/LIBRARY_DB.md). */
#define LIBDB_VERSION 1
static inline int libdb_card_schema(sqlite3 *d){
    return sqlite3_exec(d,
        "CREATE TABLE IF NOT EXISTS SOURCE_ID(SOURCE TEXT NOT NULL, EXT_ID TEXT NOT NULL, ALBUM_ID TEXT, "
        "SONG_ID INTEGER NOT NULL REFERENCES SONG(ID) ON DELETE CASCADE, PRIMARY KEY(SOURCE, EXT_ID));"
        "CREATE INDEX IF NOT EXISTS SOURCE_ID_SONG ON SOURCE_ID(SONG_ID);"
        "CREATE TABLE IF NOT EXISTS DISKOS_META(K TEXT PRIMARY KEY, V INTEGER NOT NULL);"
        "INSERT OR REPLACE INTO DISKOS_META(K, V) VALUES('LIBDB_VERSION', 1);", 0, 0, 0);
}

/* Migration (libdb_migrate.c, mq_ui only): copy src (the flash DB) to dst (the card DB) as one consistent snapshot.
 * Writes dst.tmp, drops the stock player's queue tables (LIST_SONG_0, LIST_SONG_3: the own player keeps its list in
 * RAM), adds the tables above, marks it for a rescan (DISKOS_META LIBDB_RESCAN = 1) when most of a sample of its
 * card paths are missing on this card, checks integrity, fsyncs, renames to dst and fsyncs the folder. dst is never
 * half-written: on any failure dst.tmp is removed and src is untouched. 0, or an L1xx code (docs/LIBRARY_DB.md)
 * with err filled. */
enum { LIBDB_E_OPEN_SRC = 101, LIBDB_E_OPEN_DST = 102, LIBDB_E_COPY = 103, LIBDB_E_SCHEMA = 104,
       LIBDB_E_CHECK = 105, LIBDB_E_SYNC = 106, LIBDB_E_RENAME = 107, LIBDB_E_EXISTS = 108 };
int libdb_migrate(const char *src, const char *dst, char *err, int cap);
#endif
