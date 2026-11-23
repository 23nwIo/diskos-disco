/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Make the own player's library on the card and take the user's data over from the flash library (ui/libdb.h,
 * docs/LIBRARY_DB.md). Plain file and SQLite calls only, no UI: the caller runs them on a worker thread behind the
 * full-screen overlay, inside an SD I/O lease. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "libdb.h"

static int fail(int code, char *err, int cap, const char *fmt, ...){
    if(err && cap > 0){
        int n = snprintf(err, (size_t)cap, "L%d: ", code);
        va_list ap; va_start(ap, fmt);
        if(n >= 0 && n < cap) vsnprintf(err + n, (size_t)(cap - n), fmt, ap);
        va_end(ap);
    }
    return code;
}

static int fsync_path(const char *path, int dir){
    int fd = open(path, (dir ? O_RDONLY | O_DIRECTORY : O_RDWR) | O_CLOEXEC);
    if(fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

/* The stock player's tables (V2.57 song.db, `.schema` from Jo's Disc 2026-11-23), for a card library made when the
 * flash has no library to take them from. diskOS's own tables (PLAY_LOG, BOOKS, ...) are made by musicdb and the
 * scanner on first use. No LIST_SONG_*: the own player keeps its list in RAM. */
static const char STOCK_SCHEMA[] =
    "CREATE TABLE PEQ (ID INTEGER PRIMARY KEY autoincrement, STYLE_NAME TEXT, MASTER_GAIN REAL, PARAMS_JSON TEXT, STYLE_PRESET INT);"
    "CREATE TABLE PLAY_LIST (ID INTEGER PRIMARY KEY autoincrement, LIST_ID INT, LIST_NAME TEXT);"
    "CREATE TABLE RECORD_SONG (ID INTEGER PRIMARY KEY autoincrement, PATH TEXT, NAME TEXT, TITLE TEXT, ALBUM TEXT, ARTIST TEXT, "
    "ALBUM_ARTIST TEXT, GENRE TEXT, DISC INT, TRACK INT, DURATION BIGINT, IS_CUE INT, IS_ISO INT, OFFSET BIGINT, IS_M3U INT, "
    "M3U_PATH TEXT, PLAY_COUNT INT, LAST_PLAY_TIME INT8, UNIQUE(PATH, TRACK));"
    "CREATE TABLE CUSTOM_PLAYLIST_INDEX (ID INTEGER PRIMARY KEY autoincrement, LIST_ID INTEGER UNIQUE, LIST_NAME TEXT, M3U_PATH TEXT);"
#define SONG_COLS "PATH TEXT, NAME TEXT, TITLE TEXT, ALBUM TEXT, ARTIST TEXT, GENRE TEXT, DISC INT, TRACK INT, IS_CUE INT, " \
    "IS_ISO INT, IS_DSD INT, OFFSET BIGINT, DURATION BIGINT, NAME_CODE INT, TITLE_CODE INT, ALBUM_CODE INT, ARTIST_CODE INT, " \
    "GENRE_CODE INT, ADD_TIME INT8, SAMPLE_RATE INT, BIT_PER_SAMPLE INT, CHANNELS INT, BIT_RATE INT, SONG_MIMETYPE TEXT, " \
    "SONG_PRODUCTION_YEAR TEXT, IS_SELECT INT, ALBUM_ARTIST TEXT, ALBUM_ARTIST_CODE INT, IS_M3U INT, M3U_PATH TEXT, " \
    "NAME_CODE_JP INT, TITLE_CODE_JP INT, ALBUM_CODE_JP INT, ARTIST_CODE_JP INT, GENRE_CODE_JP INT, ALBUM_ARTIST_CODE_JP INT"
    "CREATE TABLE CUSTOM_PLAYLIST (ID INTEGER PRIMARY KEY autoincrement, PLAYLIST_ID INTEGER, " SONG_COLS ");"
    "CREATE TABLE MY_LOVE (ID INTEGER PRIMARY KEY autoincrement, " SONG_COLS ");"
    "CREATE TABLE SONG (ID INTEGER PRIMARY KEY autoincrement, " SONG_COLS ", ACCENT INTEGER DEFAULT 0, ACCENT_KEY TEXT);";
#undef SONG_COLS

/* Tables the take-over never copies: songs and scan state come from this card's scan, the queue and resume are the
 * stock player's, the bookkeeping is the card's own. */
static int import_skip(const char *t){
    static const char *const skip[] = { "SONG", "PLAY_LIST", "MEMORY_PLAY", "DISKOS_META", "SOURCE_ID", "DISKOS_CUE_FILES",
                                        "BOOKS", "DISKOS_BOOK_MOVED", NULL };
    if(!strncmp(t, "sqlite_", 7) || !strncmp(t, "LIST_SONG_", 10)) return 1;
    for(int i = 0; skip[i]; i++) if(!strcasecmp(t, skip[i])) return 1;
    return 0;
}
static int ident_ok(const char *s){
    if(!s || !*s) return 0;
    for(; *s; s++) if(!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}

int libdb_create(const char *src, const char *dst, char *err, int cap){
    char tmp[1024], dir[1024], jr[1100];
    if(snprintf(tmp, sizeof tmp, "%s.tmp", dst) >= (int)sizeof tmp) return fail(LIBDB_E_OPEN_DST, err, cap, "path too long");
    snprintf(jr, sizeof jr, "%s-journal", tmp);
    snprintf(dir, sizeof dir, "%s", dst);
    char *slash = strrchr(dir, '/');
    if(slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    if(access(dst, F_OK) == 0) return fail(LIBDB_E_EXISTS, err, cap, "%s already exists", dst);
    unlink(tmp); unlink(jr);                      /* left over from an attempt that lost power */

    sqlite3 *s = NULL, *d = NULL;
    int code = 0, from_flash = src && access(src, F_OK) == 0;
    if(from_flash && libdb_open_v2(src, &s, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return fail(LIBDB_E_OPEN_SRC, err, cap, "can't open %s", src);
    if(s) sqlite3_busy_timeout(s, 5000);
    if(libdb_open_v2(tmp, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK){
        if(s) sqlite3_close(s);
        return fail(LIBDB_E_OPEN_DST, err, cap, "can't create %s: %s", tmp, strerror(errno));
    }
    sqlite3_busy_timeout(d, 5000);

    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK){ code = fail(LIBDB_E_SCHEMA, err, cap, "begin: %s", sqlite3_errmsg(d)); goto out; }
    if(s){
        /* the flash library's schema, empty: tables first, then their indexes and triggers */
        sqlite3_stmt *st = NULL;
        if(sqlite3_prepare_v2(s, "SELECT type, name, tbl_name, sql FROM sqlite_master WHERE sql IS NOT NULL "
                                 "ORDER BY type<>'table', rowid;", -1, &st, NULL) != SQLITE_OK){
            code = fail(LIBDB_E_COPY, err, cap, "read schema: %s", sqlite3_errmsg(s)); goto out;
        }
        int rc;
        while((rc = sqlite3_step(st)) == SQLITE_ROW){
            const char *name = (const char *)sqlite3_column_text(st, 1), *tbl = (const char *)sqlite3_column_text(st, 2);
            const char *sql = (const char *)sqlite3_column_text(st, 3);
            if(!name || !tbl || !sql || !strncmp(name, "sqlite_", 7) || !strncmp(tbl, "sqlite_", 7)
               || !strncmp(tbl, "LIST_SONG_", 10)) continue;
            if(sqlite3_exec(d, sql, 0, 0, 0) != SQLITE_OK){
                code = fail(LIBDB_E_COPY, err, cap, "%s: %s", name, sqlite3_errmsg(d)); break;
            }
        }
        if(!code && rc != SQLITE_DONE) code = fail(LIBDB_E_COPY, err, cap, "read schema: %s", sqlite3_errmsg(s));
        sqlite3_finalize(st);
        if(code) goto out;
        sqlite3_close(s); s = NULL;
    } else if(sqlite3_exec(d, STOCK_SCHEMA, 0, 0, 0) != SQLITE_OK){
        code = fail(LIBDB_E_SCHEMA, err, cap, "stock tables: %s", sqlite3_errmsg(d)); goto out;
    }
    char meta[200];
    /* a scan fills it; then the flash library's favourites, playlists, ... for the songs found (libdb_import) */
    snprintf(meta, sizeof meta, "INSERT OR REPLACE INTO DISKOS_META(K, V) VALUES('LIBDB_RESCAN', 1),('LIBDB_IMPORT', %d);", from_flash);
    if(libdb_card_schema(d) != SQLITE_OK || sqlite3_exec(d, meta, 0, 0, 0) != SQLITE_OK
       || sqlite3_exec(d, "COMMIT;", 0, 0, 0) != SQLITE_OK){
        code = fail(LIBDB_E_SCHEMA, err, cap, "tables: %s", sqlite3_errmsg(d)); goto out;
    }

    sqlite3_stmt *st = NULL; int ok = 0;
    if(sqlite3_prepare_v2(d, "PRAGMA integrity_check;", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW){
        const unsigned char *t = sqlite3_column_text(st, 0);
        ok = t && !strcmp((const char *)t, "ok");
    }
    sqlite3_finalize(st);
    if(ok){
        if(sqlite3_prepare_v2(d, "PRAGMA foreign_key_check(SOURCE_ID);", -1, &st, NULL) != SQLITE_OK) ok = 0;
        else { ok = sqlite3_step(st) == SQLITE_DONE; sqlite3_finalize(st); }
    }
    if(!ok){ code = fail(LIBDB_E_CHECK, err, cap, "integrity check failed"); goto out; }

out:
    if(s) sqlite3_close(s);
    if(d && sqlite3_close(d) != SQLITE_OK && !code) code = fail(LIBDB_E_SYNC, err, cap, "close failed");
    if(!code && (fsync_path(tmp, 0) != 0 || (access(jr, F_OK) == 0 && unlink(jr) != 0)))
        code = fail(LIBDB_E_SYNC, err, cap, "fsync %s: %s", tmp, strerror(errno));
    if(!code && rename(tmp, dst) != 0) code = fail(LIBDB_E_RENAME, err, cap, "rename: %s", strerror(errno));
    if(!code && fsync_path(dir, 1) != 0) code = fail(LIBDB_E_SYNC, err, cap, "fsync %s: %s", dir, strerror(errno));
    if(code){ unlink(tmp); unlink(jr); }
    return code;
}

/* After the first scan of a new card library: the user's data from the flash library, for the songs this card has.
 * Rows of a table with a PATH column come over when that PATH is a song in the card library (their own row ID is
 * renumbered: other rows may be there already); tables without one (playlist names, EQ curves, audiobook progress
 * by book) come over whole. Columns both sides have; INSERT OR IGNORE, so nothing already there is replaced. One
 * transaction, then LIBDB_IMPORT = 0. The flash library is only read. */
int libdb_import(const char *src, const char *dst, char *err, int cap, char *summary, int scap){
    sqlite3 *d = NULL;
    if(summary && scap > 0) summary[0] = 0;
    if(access(src, R_OK) != 0) return fail(LIBDB_E_OPEN_SRC, err, cap, "can't read %s", src);
    if(libdb_open_v2(dst, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, NULL) != SQLITE_OK)
        return fail(LIBDB_E_OPEN_DST, err, cap, "can't open %s", dst);
    sqlite3_busy_timeout(d, 5000);
    int code = 0, total = 0, sl = 0;
    char *q = sqlite3_mprintf("ATTACH 'file:%q?mode=ro' AS f;", src);
    int rc = q ? sqlite3_exec(d, q, 0, 0, 0) : SQLITE_NOMEM;
    sqlite3_free(q);
    if(rc != SQLITE_OK){ code = fail(LIBDB_E_OPEN_SRC, err, cap, "attach %s: %s", src, sqlite3_errmsg(d)); goto out; }
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK){ code = fail(LIBDB_E_COPY, err, cap, "begin: %s", sqlite3_errmsg(d)); goto det; }
    sqlite3_stmt *tl = NULL;
    if(sqlite3_prepare_v2(d, "SELECT name FROM f.sqlite_master WHERE type='table' AND name IN "
                             "(SELECT name FROM main.sqlite_master WHERE type='table') ORDER BY rowid;", -1, &tl, NULL) != SQLITE_OK){
        code = fail(LIBDB_E_COPY, err, cap, "tables: %s", sqlite3_errmsg(d)); goto rb;
    }
    while(!code && sqlite3_step(tl) == SQLITE_ROW){
        char t[64]; snprintf(t, sizeof t, "%s", (const char *)sqlite3_column_text(tl, 0));
        if(!ident_ok(t) || import_skip(t)) continue;
        char cols[2048] = ""; size_t cl = 0; int has_path = 0;
        char *cq = sqlite3_mprintf("SELECT a.name FROM pragma_table_info(%Q, 'f') a JOIN pragma_table_info(%Q, 'main') b "
                                   "ON a.name = b.name ORDER BY a.cid;", t, t);
        sqlite3_stmt *cs = NULL;
        if(!cq || sqlite3_prepare_v2(d, cq, -1, &cs, NULL) != SQLITE_OK){ sqlite3_free(cq); code = fail(LIBDB_E_COPY, err, cap, "%s columns: %s", t, sqlite3_errmsg(d)); break; }
        sqlite3_free(cq);
        while(sqlite3_step(cs) == SQLITE_ROW){
            const char *c = (const char *)sqlite3_column_text(cs, 0);
            if(!ident_ok(c)) continue;
            if(!strcasecmp(c, "PATH")) has_path = 1;
            if(cl < sizeof cols - 80) cl += (size_t)snprintf(cols + cl, sizeof cols - cl, "%s\"%s\"", cl ? "," : "", c);
        }
        sqlite3_finalize(cs);
        if(!cl) continue;
        if(has_path){                                     /* renumber the rows: drop "ID" from the list */
            char c2[2048] = ""; size_t l2 = 0;
            for(char *tok = strtok(cols, ","); tok; tok = strtok(NULL, ","))
                if(strcasecmp(tok, "\"ID\"")) l2 += (size_t)snprintf(c2 + l2, sizeof c2 - l2, "%s%s", l2 ? "," : "", tok);
            snprintf(cols, sizeof cols, "%s", c2);
            if(!cols[0]) continue;
        }
        char *iq = has_path
            ? sqlite3_mprintf("INSERT OR IGNORE INTO main.\"%w\"(%s) SELECT %s FROM f.\"%w\" "
                              "WHERE PATH IN (SELECT PATH FROM main.SONG) ORDER BY rowid;", t, cols, cols, t)
            : sqlite3_mprintf("INSERT OR IGNORE INTO main.\"%w\"(%s) SELECT %s FROM f.\"%w\" ORDER BY rowid;", t, cols, cols, t);
        if(!iq || sqlite3_exec(d, iq, 0, 0, 0) != SQLITE_OK){ code = fail(LIBDB_E_COPY, err, cap, "%s: %s", t, sqlite3_errmsg(d)); }
        else {
            int n = sqlite3_changes(d); total += n;
            if(n && summary && sl < scap - 40) sl += snprintf(summary + sl, (size_t)(scap - sl), "%s%s %d", sl ? ", " : "", t, n);
        }
        sqlite3_free(iq);
    }
    sqlite3_finalize(tl);
    if(!code && sqlite3_exec(d, "INSERT OR REPLACE INTO DISKOS_META(K, V) VALUES('LIBDB_IMPORT', 0);", 0, 0, 0) != SQLITE_OK)
        code = fail(LIBDB_E_SCHEMA, err, cap, "meta: %s", sqlite3_errmsg(d));
    if(!code && sqlite3_exec(d, "COMMIT;", 0, 0, 0) != SQLITE_OK) code = fail(LIBDB_E_COPY, err, cap, "commit: %s", sqlite3_errmsg(d));
rb:
    if(code) sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
det:
    sqlite3_exec(d, "DETACH f;", 0, 0, 0);
out:
    if(sqlite3_close(d) != SQLITE_OK && !code) code = fail(LIBDB_E_SYNC, err, cap, "close failed");
    (void)total;
    return code;
}
