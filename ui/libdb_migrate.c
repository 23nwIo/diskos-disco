/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Copy the flash library DB to the card (ui/libdb.h, docs/LIBRARY_DB.md). Plain file and SQLite calls only, no UI:
 * the caller runs it on a worker thread behind the full-screen overlay, inside an SD I/O lease, on external power. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
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

int libdb_migrate(const char *src, const char *dst, char *err, int cap){
    char tmp[1024], dir[1024], jr[1100];
    if(snprintf(tmp, sizeof tmp, "%s.tmp", dst) >= (int)sizeof tmp) return fail(LIBDB_E_OPEN_DST, err, cap, "path too long");
    snprintf(jr, sizeof jr, "%s-journal", tmp);
    snprintf(dir, sizeof dir, "%s", dst);
    char *slash = strrchr(dir, '/');
    if(slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    if(access(dst, F_OK) == 0) return fail(LIBDB_E_EXISTS, err, cap, "%s already exists", dst);
    unlink(tmp); unlink(jr);                      /* left over from an attempt that lost power */

    sqlite3 *s = NULL, *d = NULL;
    int code = 0;
    if(libdb_open_v2(src, &s, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return fail(LIBDB_E_OPEN_SRC, err, cap, "can't open %s", src);
    sqlite3_busy_timeout(s, 5000);
    if(libdb_open_v2(tmp, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK){
        sqlite3_close(s);
        return fail(LIBDB_E_OPEN_DST, err, cap, "can't create %s: %s", tmp, strerror(errno));
    }
    sqlite3_busy_timeout(d, 5000);

    /* one consistent snapshot: the backup API copies every page in one step, holding the source's read lock */
    sqlite3_backup *b = sqlite3_backup_init(d, "main", s, "main");
    int rc = b ? sqlite3_backup_step(b, -1) : sqlite3_errcode(d);
    for(int tries = 0; b && (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && tries < 20; tries++){
        sqlite3_sleep(250);
        rc = sqlite3_backup_step(b, -1);
    }
    if(b) sqlite3_backup_finish(b);
    if(rc != SQLITE_DONE){ code = fail(LIBDB_E_COPY, err, cap, "copy failed: %s", sqlite3_errmsg(d)); goto out; }
    sqlite3_close(s); s = NULL;

    /* the copy keeps the source's settings: switch it to the card's (libdb_setup ran on the empty file) */
    libdb_setup(d, dst, SQLITE_OPEN_READWRITE);
    /* Does the flash library describe this card? A sample of its card paths: when most are missing (another card),
     * the copy is marked for a rescan, which the UI starts after the restart (libdb_ui.c). */
    int seen = 0, here = 0;
    sqlite3_stmt *sp = NULL;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM SONG WHERE PATH LIKE '" LIBDB_CARD_ROOT "/%' ORDER BY random() LIMIT 64;",
                          -1, &sp, NULL) == SQLITE_OK){
        while(sqlite3_step(sp) == SQLITE_ROW){
            const char *p = (const char *)sqlite3_column_text(sp, 0);
            if(!p) continue;
            seen++; if(access(p, F_OK) == 0) here++;
        }
    }
    sqlite3_finalize(sp);
    char meta[96];
    snprintf(meta, sizeof meta, "INSERT OR REPLACE INTO DISKOS_META(K, V) VALUES('LIBDB_RESCAN', %d);", seen == 0 || here * 2 < seen);
    if(sqlite3_exec(d, "BEGIN IMMEDIATE; DROP TABLE IF EXISTS LIST_SONG_0; DROP TABLE IF EXISTS LIST_SONG_3;", 0, 0, 0)
       != SQLITE_OK || libdb_card_schema(d) != SQLITE_OK || sqlite3_exec(d, meta, 0, 0, 0) != SQLITE_OK
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
        /* foreign keys: every SOURCE_ID row has its song (none yet after a copy, so this checks the declaration) */
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
