/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* ui/libdb.h + ui/libdb_migrate.c: the copy of the flash library to the card, the card connection settings and the
 * SOURCE_ID foreign key. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "libdb.h"

static int one_int(sqlite3 *d, const char *sql){
    sqlite3_stmt *st = NULL; int v = -1;
    assert(sqlite3_prepare_v2(d, sql, -1, &st, NULL) == SQLITE_OK);
    if(sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}
static void one_text(sqlite3 *d, const char *sql, char *out, int cap){
    sqlite3_stmt *st = NULL; out[0] = 0;
    assert(sqlite3_prepare_v2(d, sql, -1, &st, NULL) == SQLITE_OK);
    if(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) snprintf(out, (size_t)cap, "%s", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
}
static long fsize(const char *p){ struct stat s; return stat(p, &s) == 0 ? (long)s.st_size : -1; }

int main(void){
    char root[] = "/tmp/diskos-libdb-XXXXXX";
    assert(mkdtemp(root));
    char src[256], dst[256], tmp[300], err[256];
    snprintf(src, sizeof src, "%s/song.db", root);
    snprintf(dst, sizeof dst, "%s/library.db", root);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);

    /* a stock-shaped flash library: songs, a favourite, the stock player's queue tables */
    sqlite3 *s = NULL;
    assert(libdb_open_v2(src, &s, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) == SQLITE_OK);
    assert(sqlite3_exec(s,
        "CREATE TABLE SONG (ID INTEGER PRIMARY KEY autoincrement, PATH TEXT, TITLE TEXT);"
        "INSERT INTO SONG(PATH,TITLE) VALUES('/tmp/sdcard/a.flac','A'),('/tmp/sdcard/b.mp3','B'),('/tmp/sdcard/c.mp3','C');"
        "CREATE TABLE MY_LOVE (ID INTEGER PRIMARY KEY, PATH TEXT); INSERT INTO MY_LOVE(PATH) VALUES('/tmp/sdcard/b.mp3');"
        "CREATE TABLE LIST_SONG_0 (ID INTEGER PRIMARY KEY, PATH TEXT); INSERT INTO LIST_SONG_0(PATH) VALUES('x');"
        "CREATE TABLE LIST_SONG_3 (ID INTEGER PRIMARY KEY, POS_ID INTEGER);", 0, 0, 0) == SQLITE_OK);
    /* the flash DB gets none of the card settings */
    char jm[32]; one_text(s, "PRAGMA journal_mode;", jm, sizeof jm); assert(!strcmp(jm, "delete"));
    assert(one_int(s, "PRAGMA foreign_keys;") == 0);
    sqlite3_close(s);
    long src_size = fsize(src);
    puts("PASS flash DB opened without the card settings");

    /* an attempt that lost power left a temp file behind: it is replaced */
    FILE *f = fopen(tmp, "w"); assert(f); fputs("junk", f); fclose(f);
    assert(libdb_migrate(src, dst, err, sizeof err) == 0);
    assert(access(tmp, F_OK) != 0 && access(dst, F_OK) == 0);
    assert(fsize(src) == src_size);
    puts("PASS migration: library.db written, no temp file left, flash DB unchanged");

    sqlite3 *d = NULL;
    assert(libdb_open_v2(dst, &d, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    assert(one_int(d, "PRAGMA foreign_keys;") == 1);
    one_text(d, "PRAGMA journal_mode;", jm, sizeof jm); assert(!strcmp(jm, "truncate"));
    assert(one_int(d, "PRAGMA synchronous;") == 2);
    assert(one_int(d, "SELECT COUNT(*) FROM SONG;") == 3);
    assert(one_int(d, "SELECT COUNT(*) FROM MY_LOVE;") == 1);
    assert(one_int(d, "SELECT COUNT(*) FROM sqlite_master WHERE name IN ('LIST_SONG_0','LIST_SONG_3');") == 0);
    assert(one_int(d, "SELECT V FROM DISKOS_META WHERE K='LIBDB_VERSION';") == LIBDB_VERSION);
    assert(one_int(d, "SELECT V FROM DISKOS_META WHERE K='LIBDB_RESCAN';") == 1);   /* its /tmp/sdcard paths aren't here */
    puts("PASS card DB: songs and favourites copied, queue tables dropped, card settings on");

    /* SOURCE_ID: unique per source, needs its song, goes with it */
    assert(sqlite3_exec(d, "INSERT INTO SOURCE_ID VALUES('bandcamp','123','9',2);", 0, 0, 0) == SQLITE_OK);
    assert(sqlite3_exec(d, "INSERT INTO SOURCE_ID VALUES('bandcamp','123','9',3);", 0, 0, 0) != SQLITE_OK);
    assert(sqlite3_exec(d, "INSERT INTO SOURCE_ID VALUES('navidrome','123',NULL,3);", 0, 0, 0) == SQLITE_OK);
    assert(sqlite3_exec(d, "INSERT INTO SOURCE_ID VALUES('bandcamp','777',NULL,99);", 0, 0, 0) != SQLITE_OK);
    /* a rescan updates a song in place (same ID): the mapping stays */
    assert(sqlite3_exec(d, "UPDATE SONG SET TITLE='B2' WHERE PATH='/tmp/sdcard/b.mp3';", 0, 0, 0) == SQLITE_OK);
    assert(one_int(d, "SELECT COUNT(*) FROM SOURCE_ID WHERE SONG_ID=2;") == 1);
    /* a song that went from the card takes its mappings with it */
    assert(sqlite3_exec(d, "DELETE FROM SONG WHERE ID=2;", 0, 0, 0) == SQLITE_OK);
    assert(one_int(d, "SELECT COUNT(*) FROM SOURCE_ID;") == 1);
    sqlite3_close(d);
    puts("PASS SOURCE_ID: one row per source and ID, unknown song refused, deleted song cascades");

    /* never over an existing card DB */
    assert(libdb_migrate(src, dst, err, sizeof err) == LIBDB_E_EXISTS && !strncmp(err, "L108", 4));
    /* a missing source fails cleanly and leaves nothing behind */
    char dst2[256]; snprintf(dst2, sizeof dst2, "%s/other/library.db", root);
    char nosrc[256]; snprintf(nosrc, sizeof nosrc, "%s/none.db", root);
    assert(libdb_migrate(nosrc, dst2, err, sizeof err) == LIBDB_E_OPEN_SRC);
    char dst3[256]; snprintf(dst3, sizeof dst3, "%s/library.db", root);
    unlink(dst3);
    char nodir[256]; snprintf(nodir, sizeof nodir, "%s/nodir/library.db", root);
    assert(libdb_migrate(src, nodir, err, sizeof err) == LIBDB_E_OPEN_DST);
    snprintf(tmp, sizeof tmp, "%s.tmp", nodir); assert(access(tmp, F_OK) != 0);
    puts("PASS refusals: existing card DB (L108), missing source (L101), missing folder (L102)");

    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); assert(system(cmd) == 0);
    puts("COMPLETE libdb_test");
    return 0;
}
