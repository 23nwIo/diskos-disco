/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Quick scan (ui/scanner.c, DISKOS_SCAN_STAMP): a rescan of the card library opens only new and changed files; rows of
 * unchanged files stay as they are (same ID), removed files go, a full rescan or a new tag reader reads everything. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define DIR_ "/tmp/diskos_scanquick"
#define DB_PATH DIR_ "/db/library.db"
#define SCAN_ROOT DIR_ "/card"
#define SCAN_TEST_ANY_ROOT 1
#include "../scanner.c"
int sd_io_begin(void){ return 1; }
void sd_io_end(void){}

static void mp3(const char *name, const char *title){   /* an ID3v2.3 file with just a TIT2 */
    char path[256]; snprintf(path, sizeof path, SCAN_ROOT "/%s", name);
    unsigned char b[256]; size_t n = 0, tl = strlen(title) + 1;
    unsigned char h[10] = { 'I', 'D', '3', 3, 0, 0, 0, 0, 0, (unsigned char)(10 + tl) };
    memcpy(b, h, 10); n = 10;
    memcpy(b + n, "TIT2", 4); n += 4;
    b[n++] = 0; b[n++] = 0; b[n++] = 0; b[n++] = (unsigned char)tl; b[n++] = 0; b[n++] = 0;
    b[n++] = 3; memcpy(b + n, title, tl - 1); n += tl - 1;
    FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(b, 1, n, f) == n); assert(!fclose(f));
}
static int scan(int full){
    assert((full ? scanner_start_full() : scanner_start()) == 0);
    while(scanner_active()) usleep(10000);
    assert(scanner_outcome() == SCAN_OUT_COMMITTED);
    return g_reused;
}
static long long q1(const char *sql){
    sqlite3 *d; sqlite3_stmt *st; long long v = -1;
    assert(sqlite3_open(DB_PATH, &d) == SQLITE_OK);
    assert(sqlite3_prepare_v2(d, sql, -1, &st, NULL) == SQLITE_OK);
    if(sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st); sqlite3_close(d);
    return v;
}
static void ex(const char *sql){ sqlite3 *d; assert(sqlite3_open(DB_PATH, &d) == SQLITE_OK); assert(sqlite3_exec(d, sql, 0, 0, 0) == SQLITE_OK); sqlite3_close(d); }
static int title_is(const char *file, const char *t){
    char sql[256]; snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM SONG WHERE PATH='" SCAN_ROOT "/%s' AND TITLE='%s';", file, t);
    return q1(sql) == 1;
}

int main(void){
    assert(system("rm -rf " DIR_ " && mkdir -p " DIR_ "/db " SCAN_ROOT) == 0);
    { sqlite3 *d; assert(libdb_open_v2(DB_PATH, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) == SQLITE_OK);
      assert(sqlite3_exec(d, SCHEMA_SONG, 0, 0, 0) == SQLITE_OK && libdb_card_schema(d) == SQLITE_OK); sqlite3_close(d); }
    mp3("a.mp3", "Alpha"); mp3("b.mp3", "Beta"); mp3("c.mp3", "Gamma");

    /* first scan of a library: no tag-reader version yet -> every file read and stamped */
    assert(scan(0) == 0 && !g_quick);
    assert(q1("SELECT COUNT(*) FROM SONG;") == 3 && q1("SELECT COUNT(*) FROM DISKOS_SCAN_STAMP;") == 3);
    assert(q1("SELECT V FROM DISKOS_META WHERE K='SCAN_TAGS_VERSION';") == SCAN_TAGS_VERSION);
    long long id_a = q1("SELECT ID FROM SONG WHERE PATH LIKE '%/a.mp3';");
    puts("PASS first scan: all files read, stamped, tag reader noted");

    /* quick scan: unchanged a.mp3 is not opened (its row keeps a value only the DB has), changed b.mp3 is read,
     * new d.mp3 comes in, removed c.mp3 goes with its stamp */
    ex("UPDATE SONG SET TITLE='Kept' WHERE PATH LIKE '%/a.mp3';");
    mp3("b.mp3", "Beta two"); mp3("d.mp3", "Delta"); assert(unlink(SCAN_ROOT "/c.mp3") == 0);
    assert(scan(0) == 1 && g_quick);
    assert(title_is("a.mp3", "Kept") && title_is("b.mp3", "Beta two") && title_is("d.mp3", "Delta"));
    assert(q1("SELECT COUNT(*) FROM SONG;") == 3 && q1("SELECT COUNT(*) FROM DISKOS_SCAN_STAMP;") == 3);
    assert(q1("SELECT COUNT(*) FROM DISKOS_SCAN_STAMP WHERE PATH LIKE '%/c.mp3';") == 0);
    assert(q1("SELECT ID FROM SONG WHERE PATH LIKE '%/a.mp3';") == id_a);
    puts("PASS quick scan: unchanged not read, changed/new read, removed pruned, IDs kept");

    /* a stamped file whose ordinary row is gone (e.g. turned into CUE tracks, or an audiobook moved to BOOKS) is read */
    ex("DELETE FROM SONG WHERE PATH LIKE '%/d.mp3';");
    assert(scan(0) == 2 && title_is("d.mp3", "Delta"));
    puts("PASS quick scan: a stamp without its song row does not hide the file");

    /* full rescan: every file again */
    assert(scan(1) == 0 && !g_quick && title_is("a.mp3", "Alpha"));
    puts("PASS full rescan: every file read");

    /* an older tag reader took the stamps -> the next quick scan reads everything once */
    ex("UPDATE SONG SET TITLE='Old' WHERE PATH LIKE '%/a.mp3'; UPDATE DISKOS_META SET V=0 WHERE K='SCAN_TAGS_VERSION';");
    assert(scan(0) == 0 && !g_quick && title_is("a.mp3", "Alpha"));
    assert(scan(0) == 3 && g_quick);
    puts("PASS new tag reader: one full read, then quick again");

    assert(system("rm -rf " DIR_) == 0);
    puts("COMPLETE scanner_quick_test");
    return 0;
}
