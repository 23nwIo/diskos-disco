/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Host test of ui/bandcamp.c: its worker, the stream list it writes for the own player and the frames it sends, with a
 * stand-in wget (fixtures/bandcamp/wget) that serves the pages fetched on the Disc and stubs for the rest of the UI.
 * Run: make -C ui/tests bandcamp_app_test && ./ui/tests/bandcamp_app_test */
#include "../bandcamp.h"
#include "../ipc.h"
#include "../own_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

/* ---- stubs for the UI around it ---- */
static int mode, mode_refuse;
static char sent[16][32]; static int nsent;
static char toast[128];
static track_state_t state;
int  ui_get_source_mode(void){ return mode; }
int  ui_set_source_mode(int m){ if(mode_refuse) return -1; mode = m; if(m != BANDCAMP_SOURCE) bandcamp_mode_leaving(); return 0; }
void ui_toast(const char *m){ snprintf(toast, sizeof toast, "%s", m); }
void ui_loop_wake(void){}
int  ipc_send_cmd(const char *f){ if(nsent < 16) snprintf(sent[nsent++], sizeof sent[0], "%s", f); return 0; }
void ipc_get_state(track_state_t *o){ *o = state; }
int  test_opl_running(void){ return 1; }
static int card_ok = 1, scanning; static char added[16][512]; static int nadded;
int  sd_io_begin(void){ return card_ok; }
void sd_io_end(void){}
int  scanner_active(void){ return scanning; }
static int cfg_q = 0;
int  cfg_get_int(const char *k, int def){ (void)def; return !strcmp(k, "bc_quality") ? cfg_q : def; }
int  cfg_set_int(const char *k, int v){ if(!strcmp(k, "bc_quality")) cfg_q = v; return 0; }
int  scanner_add_file(const char *p){ if(nadded < 16) snprintf(added[nadded++], sizeof added[0], "%s", p); return nadded; }
static char *file_text(const char *p, size_t *len){
    FILE *f = fopen(p, "rb"); if(!f) return NULL;
    static char b[4096]; size_t n = fread(b, 1, sizeof b, f); fclose(f); *len = n; return b;
}
static int wait_dl(const char *key){
    for(int i = 0; i < 300; i++){ bandcamp_tick(); char *j = bandcamp_json(); int ok = j && strstr(j, key); free(j); if(ok) return 1; usleep(20000); }
    fprintf(stderr, "timeout waiting for %s\n", key); return 0;
}

static int wait_for(const char *what, const char *key){     /* polls bandcamp_json until key shows up */
    for(int i = 0; i < 200; i++){
        bandcamp_tick();
        char *j = bandcamp_json(); int ok = j && strstr(j, key); free(j);
        if(ok) return 1;
        usleep(20000);
    }
    fprintf(stderr, "timeout waiting for %s\n", what);
    return 0;
}
static int wait_sent(int n){ for(int i = 0; i < 200 && nsent < n; i++){ bandcamp_tick(); usleep(20000); } return nsent >= n; }
static char *list_text(void){
    FILE *f = fopen(OPL_BC_LIST, "r"); if(!f) return NULL;
    static char b[65536]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0; return b;
}

int main(void){
    char path[1024], *old = getenv("PATH");
    snprintf(path, sizeof path, "%s/fixtures/bandcamp:%s", getcwd((char[512]){0}, 512), old ? old : "/bin");
    setenv("PATH", path, 1);
    unlink(OPL_BC_LIST);
    const char *why = NULL;

    /* search */
    CHECK(bandcamp_search("   ", &why) == -1 && why);
    CHECK(bandcamp_search("unicorn partisans", &why) == 0);
    CHECK(wait_for("search", "\"search\":{\"state\":\"done\""));
    { char *j = bandcamp_json(); CHECK(j && strstr(j, "\"name\":\"Repression Depression\"") && strstr(j, "\"q\":\"unicorn partisans\"")); free(j); }

    /* an artist's releases, then a release's tracks */
    CHECK(bandcamp_open("https://aphextwin.bandcamp.com", &why) == 0);
    CHECK(wait_for("artist", "\"kind\":\"artist\""));
    { char *j = bandcamp_json(); CHECK(j && strstr(j, "\"name\":\"Aphex Twin\"") && strstr(j, "\"name\":\"Come To Daddy\"")); free(j); }
    CHECK(bandcamp_open("ftp://nope", &why) == -1);
    CHECK(bandcamp_open("https://unicornpartisans.bandcamp.com/album/repression-depression", &why) == 0);
    CHECK(wait_for("release", "\"kind\":\"release\""));
    { char *j = bandcamp_json(); CHECK(j && strstr(j, "\"title\":\"Computer kaputt\"") && strstr(j, "\"ms\":499803") && strstr(j, "\"plays\":true")); free(j); }

    /* play the album from its third track: the list, the source switch, 0d14 for that line */
    setenv("BC_WGET_LOG", "/tmp/bc_wget.log", 1); unlink("/tmp/bc_wget.log");
    CHECK(bandcamp_play("https://example.com/", 0, &why) == -1);
    CHECK(bandcamp_play("https://unicornpartisans.bandcamp.com/album/repression-depression", 2, &why) == 0);
    CHECK(wait_sent(1) && !strcmp(sent[0], "0d14000C0002") && mode == BANDCAMP_SOURCE && bandcamp_active() && !strcmp(bandcamp_state(), "playing"));
    CHECK(access("/tmp/bc_wget.log", F_OK) != 0);                 /* the page just opened was fresh: no second download */
    char *l = list_text();
    int lines = 0; for(char *c = l; c && *c; c++) lines += *c == '\n';
    CHECK(l && lines == 6 && !strncmp(l, "499803\thttps://t4.bcbits.com/stream/", 36) && strstr(l, "\tEverybody is arrested\tUnicorn Partisans\tRepression Depression\n"));

    /* the player says line 2 didn't play: fresh links (a download), the same line again; twice in a row: an error */
    state.have_track = 1; state.pos_id = 3;
    { char *j = bandcamp_json(); CHECK(j && strstr(j, "\"track\":2")); free(j); }
    bandcamp_link_failed(2);
    CHECK(wait_sent(2) && !strcmp(sent[1], "0d14000C0002") && access("/tmp/bc_wget.log", F_OK) == 0);
    bandcamp_link_failed(2);
    CHECK(!strcmp(bandcamp_state(), "error") && !strcmp(toast, "Bandcamp didn't send this track") && nsent == 2);

    /* a single track's page */
    CHECK(bandcamp_play("https://unicornpartisans.bandcamp.com/track/soko-linx", -1, &why) == 0);
    CHECK(wait_sent(3) && !strcmp(sent[2], "0d14000C0000"));
    l = list_text();
    CHECK(l && strstr(l, "317432\thttps://t4.bcbits.com/stream/") && strstr(l, "\tSoko Linx\tUnicorn Partisans\tRepression Depression\n"));

    /* Bandcamp unreachable: an error, nothing sent (a page not opened just now is downloaded) */
    setenv("BC_WGET_FAIL", "1", 1);
    CHECK(bandcamp_play("https://unicornpartisans.bandcamp.com/track/soko-linx", -1, &why) == 0);
    for(int i = 0; i < 100 && strcmp(bandcamp_state(), "error"); i++){ bandcamp_tick(); usleep(20000); }
    bandcamp_tick();
    CHECK(!strcmp(bandcamp_state(), "error") && !strcmp(bandcamp_last_error(), "Couldn't reach Bandcamp") && nsent == 3);
    unsetenv("BC_WGET_FAIL");

    /* downloads: the album into BandcampDownloads with tags and the cover, each file into the library */
    unsetenv("BC_WGET_LOG");
    if(system("rm -rf /tmp/bc_test_card && mkdir /tmp/bc_test_card")) fails++;
    const char *dir = "/tmp/bc_test_card/BandcampDownloads/Unicorn Partisans/Repression Depression";
    CHECK(bandcamp_download("https://example.com/x", -1, &why) == -1);
    CHECK(bandcamp_download("https://unicornpartisans.bandcamp.com/album/repression-depression", 2, &why) == 0);
    CHECK(bandcamp_download("https://unicornpartisans.bandcamp.com/album/repression-depression", 2, &why) == -1 && !strcmp(why, "That is already being saved"));
    CHECK(wait_dl("\"state\":\"done\",\"track\":2,\"done\":1,\"total\":1"));
    for(int i = 0; i < 20 && nadded < 1; i++){ bandcamp_tick(); usleep(10000); }
    { char p[600]; size_t len = 0; snprintf(p, sizeof p, "%s/03 Everybody is arrested.mp3", dir);
      char *b = file_text(p, &len);
      CHECK(b && len > 20 && !memcmp(b, "ID3\4", 4) && memmem(b, len, "\3Everybody is arrested", 22) && memmem(b, len, "\377\330\377\340cover", 9));
      CHECK(b && len > 14 && !memcmp(b + len - 9, "\377\373\220\000audio", 9) && !memmem(b, len, "xx", 2));   /* its own tag went */
      CHECK(nadded == 1 && !strcmp(added[0], p));
      snprintf(p, sizeof p, "%s/cover.jpg", dir); b = file_text(p, &len);
      CHECK(b && len == 9 && !memcmp(b, "\377\330\377\340cover", 9));
      snprintf(p, sizeof p, "%s/.bandcamp-download", dir); CHECK(access(p, F_OK) != 0); }
    /* the whole album: the track saved before stays, the rest come; a scan running holds them back from the library */
    scanning = 1;
    CHECK(bandcamp_download("https://unicornpartisans.bandcamp.com/album/repression-depression", -1, &why) == 0);
    CHECK(wait_dl("\"state\":\"done\",\"track\":-1,\"done\":6,\"total\":6"));
    CHECK(nadded == 1);
    scanning = 0;
    for(int i = 0; i < 20 && nadded < 7; i++){ bandcamp_tick(); usleep(10000); }
    CHECK(nadded == 7 && strstr(added[6], "/06 "));
    { char p[600]; snprintf(p, sizeof p, "%s/01 Computer kaputt.mp3", dir); CHECK(access(p, F_OK) == 0); }
    /* the card not available: failed, with the reason */
    card_ok = 0;
    CHECK(bandcamp_download("https://unicornpartisans.bandcamp.com/track/soko-linx", -1, &why) == 0);
    CHECK(wait_dl("The SD card is not available right now"));
    card_ok = 1;
    CHECK(bandcamp_downloading() == 0);
    /* the best quality: a free album comes as Bandcamp's FLAC ZIP (the first stat answer: expired, then ok) */
    bandcamp_set_quality(BC_Q_BEST); CHECK(cfg_q == BC_Q_BEST && bandcamp_quality() == BC_Q_BEST);
    nadded = 0;
    CHECK(bandcamp_download("https://skycolourgirl.bandcamp.com/album/ambulocetus", -1, &why) == 0);
    CHECK(wait_dl("\"state\":\"done\",\"track\":-1,\"done\":2,\"total\":2"));
    for(int i = 0; i < 20 && nadded < 2; i++){ bandcamp_tick(); usleep(10000); }
    { const char *fd = "/tmp/bc_test_card/BandcampDownloads/Skycolourgirl/Ambulocetus";
      char p[600]; size_t len;
      snprintf(p, sizeof p, "%s/Skycolourgirl - Ambulocetus - 01 Ambulocetus.flac", fd); char *b = file_text(p, &len);
      CHECK(b && len >= 4 && !memcmp(b, "fLaC", 4));
      CHECK(nadded == 2 && (strstr(added[0], ".flac") && strstr(added[1], ".flac")));
      snprintf(p, sizeof p, "%s/cover.jpg", fd); b = file_text(p, &len);
      CHECK(b && len == 9 && !memcmp(b, "\377\330\377\340cover", 9));   /* ours was there first: theirs isn't written over */
      snprintf(p, sizeof p, "%s/.bandcamp-download", fd); CHECK(access(p, F_OK) != 0); }
    if(system("rm -rf /tmp/bc_test_card")) fails++;

    /* stop: back to local playback */
    bandcamp_stop();
    CHECK(mode == 0 && !bandcamp_active() && !strcmp(bandcamp_state(), "off"));
    unlink(OPL_BC_LIST); unlink("/tmp/bc_wget.log");
    if(fails){ fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    puts("bandcamp_app_test: all checks passed");
    return 0;
}
