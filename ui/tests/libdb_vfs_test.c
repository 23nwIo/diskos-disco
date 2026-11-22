/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* ui/libdb_vfs.c: holding SD access closes every card library file under open connections (so the card can be
 * unmounted), the connections stay usable objects that only report errors, new opens wait for the resume, and a
 * detached connection never reattaches. */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "libdb.h"

static char g_dir[64];
static int open_fds(void){            /* this process's fds on files in g_dir */
    int n = 0; DIR *d = opendir("/proc/self/fd"); struct dirent *e; char p[300], t[512];
    assert(d);
    while((e = readdir(d))){
        snprintf(p, sizeof p, "/proc/self/fd/%s", e->d_name);
        ssize_t k = readlink(p, t, sizeof t - 1); if(k <= 0) continue; t[k] = 0;
        if(!strncmp(t, g_dir, strlen(g_dir))) n++;
    }
    closedir(d);
    return n;
}
static int count(sqlite3 *d){
    sqlite3_stmt *st = NULL; int v = -1;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM SONG;", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

static char g_path[128]; static sqlite3 *g_shared; static volatile int g_stop;
static void *hammer(void *a){              /* a worker with its own reader, also using the shared writer */
    (void)a; sqlite3 *d = NULL; int ok = 0, err = 0;
    libdb_open_v2(g_path, &d, SQLITE_OPEN_READONLY, NULL);
    while(!g_stop){ int c1 = d ? count(d) : -1, c2 = count(g_shared); if(c1 > 0 && c2 > 0) ok++; else err++; }
    sqlite3_close(d);
    return (void *)(long)(ok > 0);
}
int main(void){
    snprintf(g_dir, sizeof g_dir, "/tmp/diskos-vfs-XXXXXX"); assert(mkdtemp(g_dir));
    char path[128]; snprintf(path, sizeof path, "%s/library.db", g_dir);
    assert(libdb_vfs_register() == 0 && sqlite3_vfs_find(LIBDB_VFS));
    assert(libdb_gen() == 0);

    sqlite3 *w, *r, *r2;
    assert(libdb_open_v2(path, &w, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK);
    assert(sqlite3_exec(w, "CREATE TABLE SONG(ID INTEGER PRIMARY KEY, PATH TEXT); INSERT INTO SONG(PATH) VALUES('a'),('b');", 0, 0, 0) == SQLITE_OK);
    assert(libdb_open_v2(path, &r, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(libdb_open_v2(path, &r2, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(count(w) == 2 && count(r) == 2);
    /* r2 is in the middle of a read (holds a shared lock), w has written (journal file exists) */
    sqlite3_stmt *mid; assert(sqlite3_prepare_v2(r2, "SELECT PATH FROM SONG;", -1, &mid, NULL) == SQLITE_OK && sqlite3_step(mid) == SQLITE_ROW);
    assert(open_fds() >= 3);
    puts("PASS card DB opened through the VFS, three connections, one mid-read");

    assert(libdb_vfs_detach() >= 3);
    assert(open_fds() == 0);
    assert(libdb_gen() == 1);
    puts("PASS SD held: no file on the card left open (umount would succeed)");

    sqlite3_step(mid); sqlite3_finalize(mid);              /* may still come from its page cache: no I/O, no crash */
    assert(count(w) == -1 && count(r) == -1);
    assert(sqlite3_exec(w, "INSERT INTO SONG(PATH) VALUES('c');", 0, 0, 0) != SQLITE_OK);
    sqlite3 *n; assert(libdb_open_v2(path, &n, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK && n == NULL);
    assert(libdb_vfs_detach() == 0 && libdb_gen() == 1);   /* nothing left to close: no new generation */
    puts("PASS held: old connections only report errors, new opens refused");

    libdb_vfs_allow(1);
    assert(count(r) == -1);                                 /* never reattached */
    assert(libdb_open_v2(path, &n, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK && count(n) == 2);
    assert(sqlite3_exec(n, "INSERT INTO SONG(PATH) VALUES('c');", 0, 0, 0) == SQLITE_OK && count(n) == 3);
    sqlite3_close(n);
    assert(sqlite3_close(w) == SQLITE_OK && sqlite3_close(r) == SQLITE_OK && sqlite3_close(r2) == SQLITE_OK);
    assert(open_fds() == 0);
    puts("PASS resumed: a new connection reads and writes, the DB is intact; old ones close cleanly");

    /* the flash DB (any other name) never goes through the VFS */
    char flash[128]; snprintf(flash, sizeof flash, "%s/song.db", g_dir);
    sqlite3 *f; assert(libdb_open_v2(flash, &f, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) == SQLITE_OK);
    assert(sqlite3_exec(f, "CREATE TABLE T(X);", 0, 0, 0) == SQLITE_OK);
    libdb_vfs_detach(); assert(sqlite3_exec(f, "INSERT INTO T VALUES(1);", 0, 0, 0) == SQLITE_OK);
    sqlite3_close(f); libdb_vfs_allow(1);
    puts("PASS flash DB untouched by a hold");

    /* threads in the middle of queries while SD access is held: no crash, no file left open */
    snprintf(g_path, sizeof g_path, "%s", path);
    assert(libdb_open_v2(path, &g_shared, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK);
    pthread_t th[4]; for(int i = 0; i < 4; i++) assert(pthread_create(&th[i], NULL, hammer, NULL) == 0);
    usleep(200000);
    assert(libdb_vfs_detach() >= 1);
    usleep(100000);
    assert(open_fds() == 0);
    g_stop = 1;
    for(int i = 0; i < 4; i++){ void *rv; pthread_join(th[i], &rv); assert(rv); }
    sqlite3_close(g_shared); libdb_vfs_allow(1);
    puts("PASS four threads querying through a hold: no crash, nothing left open");

    char cmd[128]; snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir); assert(system(cmd) == 0);
    puts("COMPLETE libdb_vfs_test");
    return 0;
}
