/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The scanner keeps outside-source IDs from the tags (ui/libdb.h, docs/LIBRARY_DB.md): Bandcamp's track and album ID
 * from ID3 TXXX (UTF-8 and UTF-16) and Vorbis comments, written to SOURCE_ID on the card DB, nothing on the flash DB. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define DB_PATH "/tmp/diskos_scansrc/library.db"
#include "../scanner.c"
int sd_io_begin(void){ return 1; }
void sd_io_end(void){}

#define DIR_ "/tmp/diskos_scansrc"
typedef struct { unsigned char b[4096]; size_t n; } buf_t;
static void put(buf_t *b, const void *p, size_t n){ assert(b->n + n <= sizeof b->b); memcpy(b->b + b->n, p, n); b->n += n; }
static void put_be32(buf_t *b, uint32_t v){ unsigned char x[4] = { v >> 24, v >> 16, v >> 8, v }; put(b, x, 4); }
static void save(const char *path, const buf_t *b){ FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(b->b, 1, b->n, f) == b->n); assert(!fclose(f)); }
static void frame(buf_t *t, const char *id, const void *body, size_t n){ put(t, id, 4); put_be32(t, (uint32_t)n); put(t, "\0\0", 2); put(t, body, n); }
static void txxx8(buf_t *t, const char *k, const char *v){
    buf_t b = {0}; put(&b, "\3", 1); put(&b, k, strlen(k) + 1); put(&b, v, strlen(v)); frame(t, "TXXX", b.b, b.n);
}
static void txxx16(buf_t *t, const char *k, const char *v){   /* encoding 1: UTF-16LE with BOM, both parts */
    buf_t b = {0}; put(&b, "\1\xFF\xFE", 3);
    for(const char *p = k; *p; p++){ put(&b, p, 1); put(&b, "", 1); } put(&b, "\0\0", 2);
    put(&b, "\xFF\xFE", 2); for(const char *p = v; *p; p++){ put(&b, p, 1); put(&b, "", 1); }
    frame(t, "TXXX", b.b, b.n);
}
static void mp3(const char *path, const buf_t *t){
    buf_t o = {0};
    unsigned char h[10] = { 'I', 'D', '3', 3, 0, 0, 0, 0, (unsigned char)(t->n >> 7), (unsigned char)(t->n & 127) };
    put(&o, h, 10); put(&o, t->b, t->n); save(path, &o);
}
static void readf(const char *path){
    char t[TAGLEN], a[TAGLEN], al[TAGLEN], g[TAGLEN], aa[TAGLEN]; int tr, di; long long dur; tagdate_t dt;
    assert(tags_from_file(path, strrchr(path, '/') + 1, t, a, al, g, aa, &tr, &di, &dt, &dur) != 0);
}

int main(void){
    assert(system("rm -rf " DIR_ " && mkdir -p " DIR_) == 0);
    { buf_t t = {0}; frame(&t, "TIT2", "\3Song", 5); txxx8(&t, "BANDCAMP_TRACK_ID", "1234567"); txxx8(&t, "BANDCAMP_ALBUM_ID", "89");
      txxx8(&t, "BANDCAMP_BAND_ID", "5"); mp3(DIR_ "/a.mp3", &t); }
    readf(DIR_ "/a.mp3"); assert(!strcmp(g_bc_track, "1234567") && !strcmp(g_bc_album, "89"));
    { buf_t t = {0}; txxx16(&t, "BANDCAMP_TRACK_ID", "42"); mp3(DIR_ "/b.mp3", &t); }
    readf(DIR_ "/b.mp3"); assert(!strcmp(g_bc_track, "42") && !g_bc_album[0]);
    { buf_t t = {0}; txxx8(&t, "BANDCAMP_TRACK_ID", "12a"); txxx8(&t, "OTHER", "7"); mp3(DIR_ "/c.mp3", &t); }
    readf(DIR_ "/c.mp3"); assert(!g_bc_track[0] && !g_bc_album[0]);   /* not digits, other keys: ignored; reset per file */
    { char kv[] = "bandcamp_track_id=777"; g_bc_track[0] = 0; char t[TAGLEN] = "", a[TAGLEN] = "", al[TAGLEN] = "", g[TAGLEN] = "", aa[TAGLEN] = "";
      int tr = 0, di = 0; tagdate_t dt; memset(&dt, 0, sizeof dt); vc_apply(kv, t, a, al, g, aa, &tr, &di, &dt); assert(!strcmp(g_bc_track, "777")); }
    puts("PASS tags: TXXX UTF-8 and UTF-16, Vorbis comment, digits only");

    /* card DB: the ID lands in SOURCE_ID for the song's row; flash DB (no table): nothing prepared */
    assert(sqlite3_open(DB_PATH, &g_db) == SQLITE_OK);
    assert(sqlite3_exec(g_db, "PRAGMA foreign_keys=ON; CREATE TABLE SONG(ID INTEGER PRIMARY KEY autoincrement, PATH TEXT, IS_CUE INT, IS_ISO INT);"
                              "INSERT INTO SONG(PATH) VALUES('/x/a.mp3');", 0, 0, 0) == SQLITE_OK);
    assert(sqlite3_prepare_v2(g_db, SRCID_PUT, -1, &g_srcid, NULL) != SQLITE_OK); sqlite3_finalize(g_srcid); g_srcid = NULL;
    assert(libdb_card_schema(g_db) == SQLITE_OK);
    assert(sqlite3_prepare_v2(g_db, SRCID_PUT, -1, &g_srcid, NULL) == SQLITE_OK);
    snprintf(g_bc_track, sizeof g_bc_track, "1234567"); snprintf(g_bc_album, sizeof g_bc_album, "89");
    source_note("/x/a.mp3");
    sqlite3_stmt *st; assert(sqlite3_prepare_v2(g_db, "SELECT SOURCE,EXT_ID,ALBUM_ID,SONG_ID FROM SOURCE_ID;", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "bandcamp")
           && !strcmp((const char *)sqlite3_column_text(st, 1), "1234567") && !strcmp((const char *)sqlite3_column_text(st, 2), "89")
           && sqlite3_column_int(st, 3) == 1 && sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    /* moved file: new row, same track ID -> the mapping follows it */
    assert(sqlite3_exec(g_db, "DELETE FROM SONG WHERE ID=1; INSERT INTO SONG(PATH) VALUES('/x/moved.mp3');", 0, 0, 0) == SQLITE_OK);
    source_note("/x/moved.mp3");
    assert(sqlite3_prepare_v2(g_db, "SELECT COUNT(*), MAX(SONG_ID) FROM SOURCE_ID;", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1 && sqlite3_column_int(st, 1) == 2);
    sqlite3_finalize(st); sqlite3_finalize(g_srcid); sqlite3_close(g_db);
    puts("PASS SOURCE_ID: written on the card DB, follows a moved file");

    /* scanner_add_file (a single new file, e.g. a Bandcamp download): its row and its mapping */
    unlink(DB_PATH);
    { sqlite3 *d; assert(libdb_open_v2(DB_PATH, &d, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) == SQLITE_OK);
      assert(sqlite3_exec(d, SCHEMA_SONG, 0, 0, 0) == SQLITE_OK && libdb_card_schema(d) == SQLITE_OK); sqlite3_close(d); }
    int id = scanner_add_file(DIR_ "/a.mp3");
    assert(id > 0);
    { sqlite3 *d; assert(sqlite3_open(DB_PATH, &d) == SQLITE_OK);
      assert(sqlite3_prepare_v2(d, "SELECT EXT_ID, ALBUM_ID, SONG_ID FROM SOURCE_ID WHERE SOURCE='bandcamp';", -1, &st, NULL) == SQLITE_OK);
      assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "1234567")
             && !strcmp((const char *)sqlite3_column_text(st, 1), "89") && sqlite3_column_int(st, 2) == id);
      sqlite3_finalize(st); sqlite3_close(d); }
    puts("PASS scanner_add_file: the new song's Bandcamp IDs are kept");
    assert(system("rm -rf " DIR_) == 0);
    puts("COMPLETE scanner_srcid_test");
    return 0;
}
