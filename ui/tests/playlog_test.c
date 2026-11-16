/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "playlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int fails;
#define CHECK(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)
static char buf[8192];
static int lines(const char *p){
    FILE *f = fopen(p, "r"); if(!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    int c = 0; for(size_t i = 0; i < n; i++) c += buf[i] == '\n';
    return c;
}
int main(void){
    char p[] = "/tmp/playlog_test_XXXXXX"; int fd = mkstemp(p); close(fd); unlink(p);
    playlog_set_path(p);
    playlog_song("/s/a.flac", 1, "Song A", "Band", "Album", 0);
    playlog_song("/s/a.flac", 1, "Song A", "Band", "Album", 0);          /* still the same song: no line */
    CHECK(lines(p) == 1 && strstr(buf, "  Band - Song A  [Album]\n"));
    playlog_song("/s/b.flac", 2, "Song B", "", "", 0);
    CHECK(lines(p) == 2 && strstr(buf, "  Song B\n"));
    playlog_reset();
    playlog_song("/s/b.flac", 2, "Song B", "", "", 0);                    /* played again after a stop */
    CHECK(lines(p) == 3);
    /* the station after the list: the station first, then each title */
    playlog_song("https://fm4.example/live", 0, "FM4", "FM4", "Web radio", 1);
    playlog_song("https://fm4.example/live", 0, "X - Y", "FM4", "Web radio", 1);
    playlog_song("https://fm4.example/live", 0, "X - Y", "FM4", "Web radio", 1);
    CHECK(lines(p) == 5 && strstr(buf, "  Radio FM4\n") && strstr(buf, "  Radio FM4: X - Y\n"));
    /* the Radio app */
    playlog_radio("Bayern 3", "");
    playlog_radio("Bayern 3", "Artist - Title");
    playlog_radio("Bayern 3", "Artist - Title");
    CHECK(lines(p) == 7 && strstr(buf, "  Radio Bayern 3\n") && strstr(buf, "  Radio Bayern 3: Artist - Title\n"));
    CHECK(buf[4] == '-' && buf[10] == ' ' && buf[13] == ':');             /* "YYYY-MM-DD HH:MM:SS" */
    unlink(p);
    if(fails){ fprintf(stderr, "playlog_test: %d failures\n", fails); return 1; }
    puts("playlog_test: all checks passed");
    return 0;
}
