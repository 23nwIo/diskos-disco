/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The scanner's date tags (Jo, 2026-11-18): every tag format's date reaches SONG_PRODUCTION_YEAR as ISO text, as
 * exactly as the tag states it; the original release date wins over this edition's. Builds small tagged files in a
 * temp dir, reads them with the production tags_from_file, and writes rows with the production SQL. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define DB_PATH "/tmp/diskos_scandate/song.db"
#include "../scanner.c"
int sd_io_begin(void){ return 1; }
void sd_io_end(void){}

#define DIR_ "/tmp/diskos_scandate"
typedef struct { unsigned char b[4096]; size_t n; } buf_t;
static void put(buf_t *b, const void *p, size_t n){ assert(b->n + n <= sizeof b->b); memcpy(b->b + b->n, p, n); b->n += n; }
static void put_be32(buf_t *b, uint32_t v){ unsigned char x[4] = { v >> 24, v >> 16, v >> 8, v }; put(b, x, 4); }
static void put_le32(buf_t *b, uint32_t v){ unsigned char x[4] = { v, v >> 8, v >> 16, v >> 24 }; put(b, x, 4); }
static void save(const char *path, const buf_t *b){ FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(b->b, 1, b->n, f) == b->n); assert(!fclose(f)); }

/* ID3v2.3/2.4 with text frames "ID\0value" pairs (sizes stay < 128, so plain and synchsafe sizes agree) */
static void id3(buf_t *o, int ver, const char *const *fr){
    buf_t t = {0};
    for(int i = 0; fr[i]; i += 2){
        put(&t, fr[i], 4); put_be32(&t, (uint32_t)strlen(fr[i + 1]) + 1); put(&t, "\0\0\0", 3); put(&t, fr[i + 1], strlen(fr[i + 1]));
    }
    unsigned char h[10] = { 'I', 'D', '3', (unsigned char)ver, 0, 0, 0, 0, (unsigned char)(t.n >> 7), (unsigned char)(t.n & 127) };
    put(o, h, 10); put(o, t.b, t.n);
}
static void read_date(const char *path, const char *want_rel, const char *want_orig, const char *want_pick){
    char t[TAGLEN], a[TAGLEN], al[TAGLEN], g[TAGLEN], aa[TAGLEN]; int tr, di; long long dur; tagdate_t dt;
    const char *fname = strrchr(path, '/') + 1;
    assert(tags_from_file(path, fname, t, a, al, g, aa, &tr, &di, &dt, &dur) != 0);
    if(strcmp(dt.rel, want_rel) || strcmp(dt.orig, want_orig) || strcmp(date_pick(&dt), want_pick)){
        fprintf(stderr, "%s: rel '%s' orig '%s' pick '%s', want '%s' '%s' '%s'\n", path, dt.rel, dt.orig, date_pick(&dt),
                want_rel, want_orig, want_pick);
        assert(0);
    }
}
static void norm(const char *in, const char *want){
    char out[DATELEN] = "";
    int ok = date_norm(in, out);
    if(want ? (!ok || strcmp(out, want)) : ok){ fprintf(stderr, "date_norm('%s') = %d '%s', want '%s'\n", in, ok, out, want ? want : "(none)"); assert(0); }
}
static void mp4_atom(buf_t *o, const char *type, const buf_t *pay){ put_be32(o, (uint32_t)(8 + pay->n)); put(o, type, 4); put(o, pay->b, pay->n); }

int main(void){
    norm("2001", "2001"); norm("2001-05", "2001-05"); norm("2001-05-03", "2001-05-03"); norm("2001/05/03", "2001-05-03");
    norm("2001.05.03", "2001-05-03"); norm("20010503", "2001-05-03"); norm("03.05.2001", "2001-05-03");
    norm(" 2001-05-03T07:00:00Z", "2001-05-03"); norm("1975 (remaster)", "1975"); norm("2001-00-00", "2001");
    norm("", NULL); norm("0000", NULL); norm("19750", NULL); norm("1975abc", NULL); norm("2001-13-01", NULL);
    norm("unknown", NULL); norm("75", NULL); norm("2001-00-05", NULL);

    assert(system("rm -rf " DIR_ " && mkdir -p " DIR_) == 0);
    { buf_t b = {0}; const char *f[] = { "TIT2", "Song", "TYER", "2001", "TDAT", "0305", NULL }; id3(&b, 3, f);
      save(DIR_ "/v23.mp3", &b); read_date(DIR_ "/v23.mp3", "2001-05-03", "", "2001-05-03"); }
    { buf_t b = {0}; const char *f[] = { "TYER", "2001", NULL }; id3(&b, 3, f);
      save(DIR_ "/v23y.mp3", &b); read_date(DIR_ "/v23y.mp3", "2001", "", "2001"); }
    { buf_t b = {0}; const char *f[] = { "TDRC", "2015-03-01T10:00", "TDOR", "1975", NULL }; id3(&b, 4, f);
      save(DIR_ "/v24.mp3", &b); read_date(DIR_ "/v24.mp3", "2015-03-01", "1975", "1975"); }
    { buf_t b = {0}; unsigned char pad[200] = {0}; put(&b, pad, sizeof pad);    /* ID3v1 only */
      unsigned char v1[128] = { 'T', 'A', 'G' }; memcpy(v1 + 3, "Title", 5); memcpy(v1 + 93, "1999", 4); put(&b, v1, 128);
      save(DIR_ "/v1.mp3", &b); read_date(DIR_ "/v1.mp3", "1999", "", "1999"); }
    { buf_t b = {0}; unsigned char pad[200] = {0}; put(&b, pad, sizeof pad);    /* ID3v1 with an empty year */
      unsigned char v1[128] = { 'T', 'A', 'G' }; memcpy(v1 + 3, "Title", 5); put(&b, v1, 128);
      save(DIR_ "/v1none.mp3", &b); read_date(DIR_ "/v1none.mp3", "", "", ""); }
    { buf_t b = {0}, vc = {0};                                                   /* FLAC Vorbis comments */
      const char *items[] = { "TITLE=Song", "DATE=1984-07", "ORIGINALYEAR=1983", NULL };
      put_le32(&vc, 4); put(&vc, "test", 4); put_le32(&vc, 3);
      for(int i = 0; items[i]; i++){ put_le32(&vc, (uint32_t)strlen(items[i])); put(&vc, items[i], strlen(items[i])); }
      put(&b, "fLaC", 4); unsigned char h[4] = { 0x84, 0, (unsigned char)(vc.n >> 8), (unsigned char)vc.n }; put(&b, h, 4); put(&b, vc.b, vc.n);
      save(DIR_ "/v.flac", &b); read_date(DIR_ "/v.flac", "1984-07", "1983", "1983"); }
    { buf_t data = {0}, day = {0}, ilst = {0}, meta = {0}, udta = {0}, moov = {0}, file = {0};   /* MP4 (c)day */
      put_be32(&data, 1); put_be32(&data, 0); put(&data, "2003-02-01T08:00:00Z", 20);
      mp4_atom(&day, "data", &data);
      mp4_atom(&ilst, "\xa9" "day", &day);
      put_be32(&meta, 0); mp4_atom(&meta, "ilst", &ilst);
      mp4_atom(&udta, "meta", &meta);
      mp4_atom(&moov, "udta", &udta);
      mp4_atom(&file, "moov", &moov);
      save(DIR_ "/v.m4a", &file); read_date(DIR_ "/v.m4a", "2003-02-01", "", "2003-02-01"); }

    /* the production SQL: a new row gets the date, a rescan without one keeps it, a rescan with one replaces it */
    { sqlite3 *db; assert(sqlite3_open(DB_PATH, &db) == SQLITE_OK);
      assert(sqlite3_exec(db, SCHEMA_SONG, 0, 0, 0) == SQLITE_OK); sqlite3_close(db); }
    int id = scanner_add_file(DIR_ "/v24.mp3");
    assert(id > 0);
    sqlite3 *db; assert(sqlite3_open(DB_PATH, &db) == SQLITE_OK);
    #define YEAR_IS(w) do { sqlite3_stmt *q; assert(sqlite3_prepare_v2(db, "SELECT IFNULL(SONG_PRODUCTION_YEAR,'') FROM SONG WHERE ID=?1;", -1, &q, NULL) == SQLITE_OK); \
        sqlite3_bind_int(q, 1, id); assert(sqlite3_step(q) == SQLITE_ROW); \
        if(strcmp((const char *)sqlite3_column_text(q, 0), w)){ fprintf(stderr, "year '%s', want '%s'\n", sqlite3_column_text(q, 0), w); assert(0); } \
        sqlite3_finalize(q); } while(0)
    YEAR_IS("1975");
    for(int k = 0; k < 2; k++){
        sqlite3_stmt *u; assert(sqlite3_prepare_v2(db, UPD_SONG, -1, &u, NULL) == SQLITE_OK);
        bind_tags(u, 1, "v24.mp3", "Song", "A", "B", "C");
        sqlite3_bind_text(u, 11, DIR_ "/v24.mp3", -1, SQLITE_TRANSIENT);
        bind_meta(u, 12, 0, 0, "");
        sqlite3_bind_int64(u, 16, 1000);
        sqlite3_bind_text(u, 17, k ? "1976-01-02" : "", -1, SQLITE_TRANSIENT);
        assert(sqlite3_step(u) == SQLITE_DONE); sqlite3_finalize(u);
        YEAR_IS(k ? "1976-01-02" : "1975");
    }
    sqlite3_close(db);
    puts("scanner_date_test: ok");
    return 0;
}
