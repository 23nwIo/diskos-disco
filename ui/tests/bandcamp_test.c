/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Host test of ui/bandcamp_core.c against pages fetched on the Disc on 2026-11-22 (fixtures/bandcamp).
 * Run: make -C ui/tests bandcamp_test && ./ui/tests/bandcamp_test */
#include "../bandcamp_core.h"
#include "../own_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

static char *slurp(const char *name, size_t *len){
    char p[256]; snprintf(p, sizeof p, "fixtures/bandcamp/%s", name);
    FILE *f = fopen(p, "rb");
    if(!f){ fprintf(stderr, "FAIL: no fixture %s\n", p); fails++; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if(!b || fread(b, 1, (size_t)n, f) != (size_t)n){ fclose(f); free(b); fails++; return NULL; }
    fclose(f); b[n] = 0;
    if(len) *len = (size_t)n;
    return b;
}

static void test_urls(void){
    char o[BC_URL_LEN];
    CHECK(bc_url_ok("https://x.bandcamp.com/album/a") && !bc_url_ok("http://x.bandcamp.com") && !bc_url_ok("https:///x")
          && !bc_url_ok("https://x.com/a b") && !bc_url_ok("https://x.com/\"") && !bc_url_ok(NULL));
    CHECK(bc_url_base("https://aphextwin.bandcamp.com/music?x=1", o, sizeof o) == 0 && !strcmp(o, "https://aphextwin.bandcamp.com"));
    CHECK(bc_url_base("ftp://x", o, sizeof o) == -1);
    CHECK(bc_url_join("https://a.bandcamp.com/music", "/album/drukqs", o, sizeof o) == 0 && !strcmp(o, "https://a.bandcamp.com/album/drukqs"));
    CHECK(bc_url_join("https://a.bandcamp.com", "https://l.bandcamp.com/album/x?label=1&tab=music", o, sizeof o) == 0
          && !strcmp(o, "https://l.bandcamp.com/album/x"));
    CHECK(bc_url_join("https://a.bandcamp.com", "//evil.com/x", o, sizeof o) == -1 && bc_url_join(NULL, "/album/x", o, sizeof o) == -1);
    char b[256];
    CHECK(bc_search_body("aphex \"twin\"\\", b, sizeof b) > 0
          && !strcmp(b, "{\"search_text\":\"aphex \\\"twin\\\"\\\\\",\"search_filter\":\"\",\"full_page\":false,\"fan_id\":null}"));
    CHECK(bc_search_body("x", b, 20) == -1);
    char e[] = "a &amp; b &quot;c&quot; &#39;d&#39; &#x263A; &lt;&gt; &bogus; &";
    bc_entities(e);
    CHECK(!strcmp(e, "a & b \"c\" 'd' \xE2\x98\xBA <> &bogus; &"));
}

static void test_search(void){
    size_t n; char *js = slurp("search.json", &n);
    if(!js) return;
    bc_item_t it[BC_MAX_HITS];
    int k = bc_parse_search(js, n, it, BC_MAX_HITS);
    CHECK(k == 28);                                           /* 29 results, one of them a fan */
    CHECK(it[0].type == 'b' && !strcmp(it[0].name, "Unicorn Partisans") && !strcmp(it[0].url, "https://unicornpartisans.bandcamp.com")
          && !it[0].artist[0]);
    CHECK(it[1].type == 'a' && !strcmp(it[1].name, "Repression Depression") && !strcmp(it[1].artist, "Unicorn Partisans")
          && !strcmp(it[1].url, "https://unicornpartisans.bandcamp.com/album/repression-depression") && !strncmp(it[1].art, "https://f4.bcbits.com/img/", 26));
    CHECK(it[5].type == 't' && !strcmp(it[5].name, "Soko Linx") && !strcmp(it[5].album, "Repression Depression")
          && !strcmp(it[5].url, "https://unicornpartisans.bandcamp.com/track/soko-linx"));
    for(int i = 0; i < k; i++) CHECK(bc_url_ok(it[i].url) && it[i].type != 'f');
    CHECK(bc_parse_search(js, n, it, 3) == 3);
    CHECK(bc_parse_search("[1,2]", 5, it, 3) == -1 && bc_parse_search("{\"auto\":{\"results\":[]}}", 23, it, 3) == 0
          && bc_parse_search("{bad", 4, it, 3) == -1);
    free(js);
}

static void test_album(void){
    char *h = slurp("album.html", NULL);
    if(!h) return;
    bc_release_t r; bc_track_t t[BC_MAX_TRACKS];
    int n = bc_parse_release(h, &r, t, BC_MAX_TRACKS);
    CHECK(n == 6 && r.ntracks == 6 && r.type == 'a' && !strcmp(r.title, "Repression Depression") && !strcmp(r.artist, "Unicorn Partisans")
          && !strcmp(r.album, "Repression Depression") && !strcmp(r.url, "https://unicornpartisans.bandcamp.com/album/repression-depression")
          && !strcmp(r.art, "https://f4.bcbits.com/img/a3581967167_2.jpg"));
    CHECK(!strcmp(t[0].title, "Computer kaputt") && t[0].ms == 499803 && t[0].num == 1 && !strcmp(t[0].artist, "Unicorn Partisans")
          && !strcmp(t[0].page, "https://unicornpartisans.bandcamp.com/track/computer-kaputt")
          && !strncmp(t[0].stream, "https://t4.bcbits.com/stream/60b1860a737fb7198c44dd523451e4ec/mp3-128/940895579?p=0&ts=", 87)
          && strstr(t[0].stream, "&token=") != NULL);
    for(int i = 0; i < n; i++) CHECK(t[i].stream[0] && t[i].num == i + 1);
    CHECK(bc_parse_release(h, &r, t, 2) == 2 && r.ntracks == 6);
    /* the player's list: streamable tracks only, with their index */
    t[2].stream[0] = 0;
    char out[8192]; int map[8]; size_t len;
    CHECK(bc_parse_release(h, &r, t, BC_MAX_TRACKS) == 6);
    t[2].stream[0] = 0;
    snprintf(t[1].title, sizeof t[1].title, "Go\taway\n");
    int lines = bc_list_format(&r, t, 6, out, sizeof out, map, &len);
    CHECK(lines == 5 && map[0] == 0 && map[1] == 1 && map[2] == 3 && map[4] == 5 && len == strlen(out));
    CHECK(!strncmp(out, "499803\thttps://t4.bcbits.com/stream/", 36) && strstr(out, "\tGo away \tUnicorn Partisans\tRepression Depression\n"));
    int nl = 0; for(char *c = out; *c; c++) nl += *c == '\n';
    CHECK(nl == 5);
    CHECK(bc_list_format(&r, t, 6, out, 100, map, &len) == -1);
    CHECK(bc_parse_release("<html>no release</html>", &r, t, 4) == -1);
    free(h);
}

static void test_track(void){
    char *h = slurp("track.html", NULL);
    if(!h) return;
    bc_release_t r; bc_track_t t[4];
    int n = bc_parse_release(h, &r, t, 4);
    CHECK(n == 1 && r.type == 't' && !strcmp(r.title, "Soko Linx") && !strcmp(r.artist, "Unicorn Partisans")
          && !strcmp(r.album, "Repression Depression") && !strcmp(r.url, "https://unicornpartisans.bandcamp.com/track/soko-linx"));
    CHECK(!strcmp(t[0].title, "Soko Linx") && t[0].ms == 317432 && t[0].num == 4 && !strncmp(t[0].stream, "https://t4.bcbits.com/stream/", 29));
    /* one release only: /music shows that release's page */
    bc_item_t it[4]; bc_release_t one;
    CHECK(bc_parse_music(h, "https://unicornpartisans.bandcamp.com", it, 4, &one) == 0 && !strcmp(one.title, "Soko Linx"));
    free(h);
}

static void test_music(void){
    char *h = slurp("artist-music.html", NULL);
    if(!h) return;
    static bc_item_t it[BC_MAX_REL];
    int n = bc_parse_music(h, "https://aphextwin.bandcamp.com", it, BC_MAX_REL, NULL);
    CHECK(n == 27);                                           /* 16 in the grid, 11 more in data-client-items */
    CHECK(it[0].type == 'a' && !strcmp(it[0].name, "Drukqs") && !strcmp(it[0].artist, "Aphex Twin")
          && !strcmp(it[0].url, "https://aphextwin.bandcamp.com/album/drukqs") && !strcmp(it[0].art, "https://f4.bcbits.com/img/a2683258024_2.jpg"));
    CHECK(!strcmp(it[1].name, "Surfing On Sine Waves (Expanded Edition)"));
    CHECK(!strcmp(it[16].name, "Come To Daddy") && !strcmp(it[16].url, "https://aphextwin.bandcamp.com/album/come-to-daddy")
          && !strcmp(it[16].art, "https://f4.bcbits.com/img/a3992891009_2.jpg") && !strcmp(it[16].artist, "Aphex Twin"));
    for(int i = 0; i < n; i++){
        CHECK(it[i].name[0] && bc_url_ok(it[i].url) && !strchr(it[i].name, '<') && !strchr(it[i].name, '\n'));
        for(int j = 0; j < i; j++) CHECK(strcmp(it[i].url, it[j].url));
        if(getenv("BC_DUMP")) printf("%c %s | %s | %s\n", it[i].type, it[i].name, it[i].artist, it[i].url);
    }
    CHECK(bc_parse_music(h, "https://aphextwin.bandcamp.com", it, 5, NULL) == 5);
    CHECK(bc_parse_music("<html></html>", "https://x.bandcamp.com", it, 5, NULL) == -1);
    /* a label's grid: the artist under the title */
    const char *label = "<p id=\"band-name-location\"><span class=\"title\">Warp</span></p>"
        "<li data-item-id=\"album-1\" class=\"music-grid-item\"><a href=\"https://boc.bandcamp.com/album/geogaddi?label=1&amp;tab=music\">"
        "<div class=\"art\"><img class=\"lazy\" src=\"/img/0.gif\" data-original=\"https://f4.bcbits.com/img/a0000000001_2.jpg\"></div>"
        "<p class=\"title\">\n  Geogaddi &amp; more\n  <br><span class=\"artist-override\">\n Boards of Canada\n </span>\n</p></a></li>"
        "<li data-item-id=\"track-2\"><a href=\"/track/single\"><p class=\"title\">A Single</p></a></li>";
    CHECK(bc_parse_music(label, "https://warp.bandcamp.com", it, 5, NULL) == 2);
    CHECK(!strcmp(it[0].name, "Geogaddi & more") && !strcmp(it[0].artist, "Boards of Canada") && !strcmp(it[0].url, "https://boc.bandcamp.com/album/geogaddi")
          && !strcmp(it[0].art, "https://f4.bcbits.com/img/a0000000001_2.jpg"));
    CHECK(it[1].type == 't' && !strcmp(it[1].name, "A Single") && !strcmp(it[1].artist, "Warp") && !strcmp(it[1].url, "https://warp.bandcamp.com/track/single"));
    free(h);
}

int main(void){
    test_urls();
    test_search();
    test_album();
    test_track();
    test_music();
    if(fails){ fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    puts("bandcamp_test: all checks passed");
    return 0;
}
