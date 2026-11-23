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
#include <signal.h>
#include <sys/wait.h>
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
        "CREATE INDEX SONG_PATH ON SONG(PATH);"
        "CREATE TABLE MY_LOVE (ID INTEGER PRIMARY KEY autoincrement, PATH TEXT, TITLE TEXT);"
        "INSERT INTO MY_LOVE(PATH) VALUES('/tmp/sdcard/b.mp3'),('/tmp/sdcard/a.flac');"
        "CREATE TABLE CUSTOM_PLAYLIST_INDEX (ID INTEGER PRIMARY KEY autoincrement, LIST_ID INTEGER UNIQUE, LIST_NAME TEXT);"
        "INSERT INTO CUSTOM_PLAYLIST_INDEX(LIST_ID,LIST_NAME) VALUES(4,'Mix');"
        "CREATE TABLE CUSTOM_PLAYLIST (ID INTEGER PRIMARY KEY autoincrement, PLAYLIST_ID INTEGER, PATH TEXT);"
        "INSERT INTO CUSTOM_PLAYLIST(PLAYLIST_ID,PATH) VALUES(4,'/tmp/sdcard/c.mp3'),(4,'/tmp/sdcard/a.flac');"
        "CREATE TABLE PEQ (ID INTEGER PRIMARY KEY autoincrement, STYLE_NAME TEXT, STYLE_PRESET INT); INSERT INTO PEQ(STYLE_NAME,STYLE_PRESET) VALUES('Mine',11);"
        "CREATE TABLE PLAY_STATS(PATH TEXT PRIMARY KEY, PLAYS INTEGER DEFAULT 0);"
        "INSERT INTO PLAY_STATS VALUES('/tmp/sdcard/c.mp3',5),('/tmp/sdcard/a.flac',2);"
        "CREATE TABLE MEMORY_PLAY (ID INTEGER PRIMARY KEY, MUSIC_ID INT); INSERT INTO MEMORY_PLAY VALUES(1,1);"
        "CREATE TABLE LIST_SONG_0 (ID INTEGER PRIMARY KEY, PATH TEXT); INSERT INTO LIST_SONG_0(PATH) VALUES('x');"
        "CREATE TABLE LIST_SONG_3 (ID INTEGER PRIMARY KEY, POS_ID INTEGER);", 0, 0, 0) == SQLITE_OK);
    /* the flash DB gets none of the card settings */
    char jm[32]; one_text(s, "PRAGMA journal_mode;", jm, sizeof jm); assert(!strcmp(jm, "delete"));
    assert(one_int(s, "PRAGMA foreign_keys;") == 0);
    sqlite3_close(s);
    long src_size = fsize(src);
    puts("PASS flash DB opened without the card settings");

    /* as in mq_ui: the card DB opens through libdb_vfs (the take-over attaches the flash DB through it too) */
    assert(libdb_vfs_register() == 0); libdb_vfs_allow(1);
    /* an attempt that lost power left a temp file behind: it is replaced */
    FILE *f = fopen(tmp, "w"); assert(f); fputs("junk", f); fclose(f);
    assert(libdb_create(src, dst, err, sizeof err) == 0);
    assert(access(tmp, F_OK) != 0 && access(dst, F_OK) == 0);
    assert(fsize(src) == src_size);
    puts("PASS new card library: library.db written, no temp file left, flash DB unchanged");

    sqlite3 *d = NULL;
    assert(libdb_open_v2(dst, &d, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    assert(one_int(d, "PRAGMA foreign_keys;") == 1);
    one_text(d, "PRAGMA journal_mode;", jm, sizeof jm); assert(!strcmp(jm, "truncate"));
    assert(one_int(d, "PRAGMA synchronous;") == 2);
    assert(one_int(d, "SELECT COUNT(*) FROM SONG;") == 0);                 /* no songs: the scan fills it */
    assert(one_int(d, "SELECT COUNT(*) FROM MY_LOVE;") == 0);
    assert(one_int(d, "SELECT COUNT(*) FROM sqlite_master WHERE name='SONG_PATH';") == 1);   /* indexes come along */
    assert(one_int(d, "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'LIST_SONG_%';") == 0);
    assert(one_int(d, "SELECT V FROM DISKOS_META WHERE K='LIBDB_VERSION';") == LIBDB_VERSION);
    assert(one_int(d, "SELECT V FROM DISKOS_META WHERE K='LIBDB_RESCAN';") == 1);
    assert(one_int(d, "SELECT V FROM DISKOS_META WHERE K='LIBDB_IMPORT';") == 1);
    puts("PASS card DB: the flash schema without songs or queue tables, card settings on, scan + take-over marked");

    /* SOURCE_ID: unique per source, needs its song, goes with it */
    assert(sqlite3_exec(d, "INSERT INTO SONG(ID,PATH,TITLE) VALUES(2,'/tmp/sdcard/b.mp3','B'),(3,'/tmp/sdcard/c.mp3','C');", 0, 0, 0) == SQLITE_OK);
    assert(sqlite3_exec(d, "INSERT INTO SOURCE_ID VALUES('bandcamp','123','9',2);", 0, 0, 0) == SQLITE_OK);
    assert(sqlite3_exec(d, "INSERT INTO SOURCE_ID VALUES('bandcamp','123','9',3);", 0, 0, 0) != SQLITE_OK);
    assert(sqlite3_exec(d, "INSERT INTO SOURCE_ID VALUES('navidrome','123',NULL,3);", 0, 0, 0) == SQLITE_OK);
    assert(sqlite3_exec(d, "INSERT INTO SOURCE_ID VALUES('bandcamp','777',NULL,99);", 0, 0, 0) != SQLITE_OK);
    assert(sqlite3_exec(d, "UPDATE SONG SET TITLE='B2' WHERE PATH='/tmp/sdcard/b.mp3';", 0, 0, 0) == SQLITE_OK);
    assert(one_int(d, "SELECT COUNT(*) FROM SOURCE_ID WHERE SONG_ID=2;") == 1);
    assert(sqlite3_exec(d, "DELETE FROM SONG WHERE ID=2;", 0, 0, 0) == SQLITE_OK);
    assert(one_int(d, "SELECT COUNT(*) FROM SOURCE_ID;") == 1);
    puts("PASS SOURCE_ID: one row per source and ID, unknown song refused, deleted song cascades");

    /* the scan found two songs of the flash library (b, c) and a new one; a favourite was set already */
    assert(sqlite3_exec(d, "DELETE FROM SOURCE_ID; DELETE FROM SONG;"
        "INSERT INTO SONG(ID,PATH,TITLE) VALUES(7,'/tmp/sdcard/c.mp3','C'),(8,'/tmp/sdcard/b.mp3','B'),(9,'/tmp/sdcard/new.flac','N');"
        "INSERT INTO MY_LOVE(PATH) VALUES('/tmp/sdcard/new.flac');", 0, 0, 0) == SQLITE_OK);
    sqlite3_close(d);
    char sum[200];
    assert(libdb_import(src, dst, err, sizeof err, sum, sizeof sum) == 0);
    assert(fsize(src) == src_size);
    assert(libdb_open_v2(dst, &d, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    assert(one_int(d, "SELECT COUNT(*) FROM MY_LOVE;") == 2);              /* b (on this card) + the new one; a is not here */
    assert(one_int(d, "SELECT COUNT(*) FROM MY_LOVE WHERE PATH='/tmp/sdcard/a.flac';") == 0);
    assert(one_int(d, "SELECT COUNT(*) FROM CUSTOM_PLAYLIST;") == 1);      /* c, not a */
    assert(one_int(d, "SELECT PLAYLIST_ID FROM CUSTOM_PLAYLIST;") == 4);   /* playlist IDs kept */
    assert(one_int(d, "SELECT COUNT(*) FROM CUSTOM_PLAYLIST_INDEX WHERE LIST_ID=4;") == 1);
    assert(one_int(d, "SELECT COUNT(*) FROM PEQ;") == 1);
    assert(one_int(d, "SELECT PLAYS FROM PLAY_STATS WHERE PATH='/tmp/sdcard/c.mp3';") == 5);
    assert(one_int(d, "SELECT COUNT(*) FROM PLAY_STATS;") == 1);
    assert(one_int(d, "SELECT V FROM DISKOS_META WHERE K='LIBDB_IMPORT';") == 0);
    assert(one_int(d, "SELECT COUNT(*) FROM SONG;") == 3);                 /* the scan's songs, untouched */
    assert(strstr(sum, "MY_LOVE 1") && strstr(sum, "CUSTOM_PLAYLIST 1"));
    sqlite3_close(d);
    printf("PASS take-over by PATH: %s\n", sum);

    /* never over an existing card DB */
    assert(libdb_create(src, dst, err, sizeof err) == LIBDB_E_EXISTS && !strncmp(err, "L108", 4));
    /* no flash library: the stock tables, no take-over */
    char dst2[256]; snprintf(dst2, sizeof dst2, "%s/fresh.library.db", root);
    char nosrc[256]; snprintf(nosrc, sizeof nosrc, "%s/none.db", root);
    assert(libdb_create(nosrc, dst2, err, sizeof err) == 0);
    assert(libdb_open_v2(dst2, &d, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(one_int(d, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
                      "('SONG','MY_LOVE','CUSTOM_PLAYLIST','CUSTOM_PLAYLIST_INDEX','PEQ','PLAY_LIST','RECORD_SONG','SOURCE_ID');") == 8);
    assert(one_int(d, "SELECT V FROM DISKOS_META WHERE K='LIBDB_IMPORT';") == 0);
    assert(one_int(d, "SELECT V FROM DISKOS_META WHERE K='LIBDB_RESCAN';") == 1);
    sqlite3_close(d);
    char nodir[256]; snprintf(nodir, sizeof nodir, "%s/nodir/library.db", root);
    assert(libdb_create(src, nodir, err, sizeof err) == LIBDB_E_OPEN_DST);
    snprintf(tmp, sizeof tmp, "%s.tmp", nodir); assert(access(tmp, F_OK) != 0);
    puts("PASS refusals: existing card DB (L108), missing folder (L102); no flash library = stock tables");

    /* which player runs (the launcher's marker, LIBDB_OWN_PID set by the Makefile) */
    unlink(LIBDB_OWN_PID);
    assert(libdb_player() == LIBDB_PL_PENDING);                       /* not decided yet */
    f = fopen(LIBDB_OWN_PID, "w"); fputs("stock\n", f); fclose(f);
    assert(libdb_player() == LIBDB_PL_STOCK);
    f = fopen(LIBDB_OWN_PID, "w"); fprintf(f, "%ld\n", (long)getpid()); fclose(f);
    assert(libdb_player() == LIBDB_PL_PENDING);                       /* our own binary: the launcher before its exec */
    pid_t c = fork(); if(c == 0){ execl("/bin/sleep", "sleep", "5", (char *)NULL); _exit(1); }
    usleep(200000);
    f = fopen(LIBDB_OWN_PID, "w"); fprintf(f, "%ld\n", (long)c); fclose(f);
    assert(libdb_player() == LIBDB_PL_STOCK);                         /* another binary: not the own player */
    kill(c, SIGKILL); waitpid(c, NULL, 0);
    assert(libdb_player() == LIBDB_PL_STOCK);                         /* gone */
    assert(!libdb_own_player_runs());
    unlink(LIBDB_OWN_PID);
    puts("PASS player marker: missing/launcher = pending, stock, other or dead pid = stock");

    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); assert(system(cmd) == 0);
    puts("COMPLETE libdb_test");
    return 0;
}
