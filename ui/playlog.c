/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "playlog.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char g_path[256] = PLAYLOG_PATH;
static char g_last[1200];                 /* what the last line was about ("" none) */
void playlog_set_path(const char *path){ snprintf(g_path, sizeof g_path, "%s", path); g_last[0] = 0; }
void playlog_reset(void){ g_last[0] = 0; }

static void write_line(const char *text){
    char ts[32], line[1400];
    time_t now = time(NULL); struct tm tm;
    if(!localtime_r(&now, &tm) || !strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm)) snprintf(ts, sizeof ts, "%ld", (long)now);
    int n = snprintf(line, sizeof line, "%s  %s\n", ts, text);
    if(n <= 0) return;
    if(n >= (int)sizeof line){ n = (int)sizeof line - 1; line[n - 1] = '\n'; }
    int fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);   /* /tmp: RAM */
    if(fd < 0){ fprintf(stderr, "play log: %s: can't open\n", g_path); return; }
    if(write(fd, line, (size_t)n) != n) fprintf(stderr, "play log: %s: write failed\n", g_path);
    close(fd);
}
/* one line per new `key` */
static void note(const char *key, const char *text){
    if(!strcmp(key, g_last)) return;
    snprintf(g_last, sizeof g_last, "%s", key);
    write_line(text);
}
void playlog_song(const char *path, int pos_id, const char *title, const char *artist, const char *album, int stream){
    if(!path || !path[0]) return;
    if(!title) title = "";
    if(!artist) artist = "";
    if(!album) album = "";
    char key[1200], text[1200];
    snprintf(key, sizeof key, "s|%s|%d|%s", path, stream ? 0 : pos_id, title);
    if(stream){                                    /* the station after the list: artist = the station */
        if(!title[0] || !strcmp(title, artist)) snprintf(text, sizeof text, "Radio %s", artist[0] ? artist : path);
        else snprintf(text, sizeof text, "Radio %s: %s", artist[0] ? artist : path, title);
    } else {
        const char *t = title[0] ? title : path;
        if(artist[0] && album[0]) snprintf(text, sizeof text, "%s - %s  [%s]", artist, t, album);
        else if(artist[0])        snprintf(text, sizeof text, "%s - %s", artist, t);
        else if(album[0])         snprintf(text, sizeof text, "%s  [%s]", t, album);
        else                      snprintf(text, sizeof text, "%s", t);
    }
    note(key, text);
}
void playlog_radio(const char *station, const char *title){
    if(!station) station = "";
    if(!title) title = "";
    char key[1200], text[1200];
    snprintf(key, sizeof key, "r|%s|%s", station, title);
    if(title[0]) snprintf(text, sizeof text, "Radio %s: %s", station[0] ? station : "?", title);
    else snprintf(text, sizeof text, "Radio %s", station[0] ? station : "?");
    note(key, text);
}
