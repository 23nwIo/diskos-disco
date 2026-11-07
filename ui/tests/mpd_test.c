/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* MPD server: the pure parts (tokenizer, arguments, filters, sessions, command lists) and the network thread with a
 * fake player behind it. `make -C ui/tests mpd_test && ./mpd_test`; `./mpd_test --serve PORT` keeps a server with the
 * fake player on 127.0.0.1:PORT (password "testtesttesttesttest") for real clients (mpc, python-mpd2):
 * tests/mpd_client_test.py drives it. */
#include "mpd_core.h"
#include "mpd_srv.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char PASS[] = "testtesttesttesttest";

/* ---------------------------------------------------------------- a fake player */
static int f_vol = 50, f_playing, f_mode, f_nq;
static char f_q[16][64];
static int fake(void *ud, int argc, char **argv, rmt_buf_t *o, const char **msg, const mpd_sess_t *s){
    (void)ud; (void)s;
    const char *c = argv[0];
    if(!strcmp(c, "status")){
        int r, p, si; mpd_flags_from_mode(f_mode, &r, &p, &si);
        rmt_buf_printf(o, "volume: %d\nrepeat: %d\nrandom: %d\nsingle: %d\nconsume: 0\nplaylist: 1\nplaylistlength: %d\nstate: %s\n",
                       f_vol, p, r, si, f_nq, f_nq ? (f_playing ? "play" : "pause") : "stop");
        if(f_nq) rmt_buf_put(o, "song: 0\nsongid: 1\nelapsed: 1.000\nduration: 100.000\n");
        return 0;
    }
    if(!strcmp(c, "currentsong")){ if(f_nq) rmt_buf_printf(o, "file: %s\nTitle: T\nPos: 0\nId: 1\n", f_q[0]); return 0; }
    if(!strcmp(c, "setvol")){ long v; if(argc != 2 || mpd_parse_int(argv[1], 0, 100, &v)){ *msg = "Invalid volume value"; return MPD_ACK_ARG; } f_vol = (int)v; msrv_events(MPD_EV_MIXER); return 0; }
    if(!strcmp(c, "play")){ if(!f_nq) return 0; f_playing = 1; msrv_events(MPD_EV_PLAYER); return 0; }
    if(!strcmp(c, "pause") || !strcmp(c, "stop")){ f_playing = 0; msrv_events(MPD_EV_PLAYER); return 0; }
    if(!strcmp(c, "random") || !strcmp(c, "repeat") || !strcmp(c, "single")){
        int v; if(argc != 2 || mpd_parse_bool(argv[1], &v)){ *msg = "Boolean (0/1) expected"; return MPD_ACK_ARG; }
        int r, p, si; mpd_flags_from_mode(f_mode, &r, &p, &si);
        if(c[1] == 'a') r = v; else if(c[1] == 'e') p = v; else si = v;
        int m = mpd_mode_from_flags(r, p, si); if(m < 0){ *msg = "The Disc has no such play mode"; return MPD_ACK_SYSTEM; }
        f_mode = m; msrv_events(MPD_EV_OPTIONS); return 0;
    }
    if(!strcmp(c, "add")){
        if(argc != 2){ *msg = "wrong number of arguments for \"add\""; return MPD_ACK_ARG; }
        if(f_nq >= 16){ *msg = "full"; return MPD_ACK_PLAYLIST_MAX; }
        snprintf(f_q[f_nq++], sizeof f_q[0], "%s", argv[1]); msrv_events(MPD_EV_PLAYLIST); return 0;
    }
    if(!strcmp(c, "clear")){ f_nq = 0; f_playing = 0; msrv_events(MPD_EV_PLAYLIST | MPD_EV_PLAYER); return 0; }
    if(!strcmp(c, "playlistinfo")){ for(int i = 0; i < f_nq; i++) rmt_buf_printf(o, "file: %s\nPos: %d\nId: %d\n", f_q[i], i, i + 1); return 0; }
    if(!strcmp(c, "lsinfo")){ rmt_buf_put(o, "directory: Music\nfile: Music/a b.flac\nTitle: A \"quoted\" title\n"); return 0; }
    if(!strcmp(c, "outputs")){ rmt_buf_put(o, "outputid: 0\noutputname: Disc\nplugin: disc\noutputenabled: 1\n"); return 0; }
    if(!strcmp(c, "stats")){ rmt_buf_put(o, "artists: 1\nalbums: 1\nsongs: 1\nuptime: 1\nplaytime: 0\ndb_playtime: 1\ndb_update: 0\n"); return 0; }
    if(!strcmp(c, "commands")){ static const char *const C[] = { "status", "add", "play", "pause", NULL }; for(int i = 0; C[i]; i++) mpd_kv(o, "command", C[i]); return 0; }
    if(!strcmp(c, "notcommands") || !strcmp(c, "urlhandlers") || !strcmp(c, "decoders")) return 0;
    if(!strcmp(c, "replay_gain_status")){ rmt_buf_put(o, "replay_gain_mode: off\n"); return 0; }
    *msg = "unknown command"; return MPD_ACK_UNKNOWN;
}

/* ---------------------------------------------------------------- unit tests */
static void t_tokenize(void){
    char l1[] = "find artist \"Ana & \\\"Bo\\\"\"  album   X"; char *a[8];
    assert(mpd_tokenize(l1, a, 8) == 5);
    assert(!strcmp(a[0], "find") && !strcmp(a[2], "Ana & \"Bo\"") && !strcmp(a[4], "X"));
    char l2[] = "add \"unterminated"; assert(mpd_tokenize(l2, a, 8) == -1);
    char l3[] = "a b c d e f g h i"; assert(mpd_tokenize(l3, a, 8) == -2);
    char l4[] = "   "; assert(mpd_tokenize(l4, a, 8) == 0);
    char l5[] = "x \"\""; assert(mpd_tokenize(l5, a, 8) == 2 && a[1][0] == 0);
    char l6[] = "x \"a\"b"; assert(mpd_tokenize(l6, a, 8) == -1);
    char l7[] = "x a\"b"; assert(mpd_tokenize(l7, a, 8) == -1);
}
static void t_args(void){
    long a, b, ms; int rel, v;
    assert(!mpd_parse_range("3", &a, &b) && a == 3 && b == 4);
    assert(!mpd_parse_range("2:5", &a, &b) && a == 2 && b == 5);
    assert(!mpd_parse_range("2:", &a, &b) && a == 2 && b == -1);
    assert(mpd_parse_range("5:2", &a, &b) && mpd_parse_range("x", &a, &b) && mpd_parse_range(":3", &a, &b) && mpd_parse_range("-1", &a, &b));
    assert(!mpd_parse_time("12.5", &ms, &rel) && ms == 12500 && rel == 0);
    assert(!mpd_parse_time("+3", &ms, &rel) && ms == 3000 && rel == 1);
    assert(!mpd_parse_time("-0.5", &ms, &rel) && ms == 500 && rel == -1);
    assert(mpd_parse_time("+3", &ms, NULL) && mpd_parse_time("abc", &ms, &rel) && mpd_parse_time("", &ms, &rel));
    assert(!mpd_parse_bool("1", &v) && v == 1 && mpd_parse_bool("yes", &v));
    assert(!mpd_parse_int("100", 0, 100, &a) && mpd_parse_int("101", 0, 100, &a) && mpd_parse_int("1x", 0, 100, &a));
}
static void t_modes(void){
    for(int m = 0; m <= 4; m++){ int r, p, s; mpd_flags_from_mode(m, &r, &p, &s); assert(mpd_mode_from_flags(r, p, s) == m); }
    assert(mpd_mode_from_flags(1, 1, 0) == -1 && mpd_mode_from_flags(1, 0, 1) == -1);
    assert(mpd_vol_to_mpd(120, 120) == 100 && mpd_vol_to_mpd(0, 120) == 0 && mpd_vol_to_mpd(60, 120) == 50 && mpd_vol_to_mpd(-1, 120) == -1);
    assert(mpd_vol_from_mpd(100, 120) == 120 && mpd_vol_from_mpd(50, 120) == 60 && mpd_vol_from_mpd(1, 120) == 1);
    for(int p = 0; p <= 100; p++) assert(mpd_vol_to_mpd(mpd_vol_from_mpd(p, 120), 120) == p);   /* round trip */
}
static void t_filters(void){
    mpd_tags_t s; memset(&s, 0, sizeof s);
    s.v[MPD_TAG_FILE] = "Music/Blue Album/01 Uno.flac"; s.v[MPD_TAG_TITLE] = "Uno"; s.v[MPD_TAG_ARTIST] = "Ana & \"Bo\"";
    s.v[MPD_TAG_ALBUM] = "Blue Album"; s.v[MPD_TAG_TRACK] = "1"; s.v[MPD_TAG_GENRE] = "Pop";
    mpd_filter_t f; const char *err;
    char *o1[] = { "artist", "Ana & \"Bo\"" }; assert(!mpd_filter_parse(&f, 2, o1, 0, &err) && mpd_filter_match(&f, &s));
    char *o2[] = { "artist", "ana" }; assert(!mpd_filter_parse(&f, 2, o2, 0, &err) && !mpd_filter_match(&f, &s));
    assert(!mpd_filter_parse(&f, 2, o2, 1, &err) && mpd_filter_match(&f, &s));      /* search: folded substring */
    char *o3[] = { "any", "blue" }; assert(!mpd_filter_parse(&f, 2, o3, 1, &err) && mpd_filter_match(&f, &s));
    char *o4[] = { "artist" }; assert(mpd_filter_parse(&f, 1, o4, 0, &err));
    char *o5[] = { "nope", "x" }; assert(mpd_filter_parse(&f, 2, o5, 0, &err));
    char *e1[] = { "(artist == 'Ana & \\\"Bo\\\"')" }; assert(!mpd_filter_parse(&f, 1, e1, 0, &err) && mpd_filter_match(&f, &s));
    char *e2[] = { "((album == \"Blue Album\") AND (track != '2'))" }; assert(!mpd_filter_parse(&f, 1, e2, 0, &err) && f.n == 2 && mpd_filter_match(&f, &s));
    char *e3[] = { "(!(genre == 'Pop'))" }; assert(!mpd_filter_parse(&f, 1, e3, 0, &err) && !mpd_filter_match(&f, &s));
    char *e4[] = { "(title contains 'un')" }; assert(!mpd_filter_parse(&f, 1, e4, 1, &err) && mpd_filter_match(&f, &s));
    char *e5[] = { "(base 'Music/Blue Album')" }; assert(!mpd_filter_parse(&f, 1, e5, 0, &err) && mpd_filter_match(&f, &s));
    char *e6[] = { "(base 'Music/Blue')" }; assert(!mpd_filter_parse(&f, 1, e6, 0, &err) && !mpd_filter_match(&f, &s));
    char *e7[] = { "(file starts_with 'Music/')" }; assert(!mpd_filter_parse(&f, 1, e7, 0, &err) && mpd_filter_match(&f, &s));
    char *b1[] = { "(artist =~ 'A.*')" }; assert(mpd_filter_parse(&f, 1, b1, 0, &err) && strstr(err, "Regular"));
    char *b2[] = { "((a == 'x') OR (b == 'y'))" }; assert(mpd_filter_parse(&f, 1, b2, 0, &err));
    char *b3[] = { "(artist == 'x'" }; assert(mpd_filter_parse(&f, 1, b3, 0, &err));
    char *b4[] = { "(artist == 'x') junk" }; assert(mpd_filter_parse(&f, 1, b4, 0, &err));
}
static char *run_lines(mpd_sess_t *s, const char *const *lines, const char *pass){
    rmt_buf_t out = { 0 };
    for(int i = 0; lines[i]; i++){
        char l[512]; snprintf(l, sizeof l, "%s", lines[i]);
        int a = mpd_sess_line(s, l, pass, &out);
        if(a == MPD_A_RUN) mpd_run_batch(s, pass, fake, NULL, &out);
        if(a == MPD_A_CLOSE) rmt_buf_put(&out, "<close>\n");
        if(a == MPD_A_IDLE) rmt_buf_put(&out, "<idle>\n");
        if(a == MPD_A_NOIDLE) rmt_buf_put(&out, "<noidle>\n");
    }
    return rmt_buf_take(&out);
}
static void t_sessions(void){
    mpd_sess_t s; mpd_sess_init(&s, 0);
    const char *const a[] = { "status", "password wrong", "ping", "password testtesttesttesttest", "setvol 30", "status", NULL };
    char *r = run_lines(&s, a, PASS);
    assert(!strncmp(r, "ACK [4@0] {status} you don't have permission for \"status\"\nACK [3@0] {password} incorrect password\nOK\nOK\nOK\nvolume: 30\n", 112));
    free(r);
    const char *const b[] = { "command_list_ok_begin", "setvol 40", "status", "command_list_end", NULL };
    r = run_lines(&s, b, PASS);
    assert(strstr(r, "list_OK\nvolume: 40\n") && strstr(r, "list_OK\nOK\n") && !strstr(r, "ACK")); free(r);
    const char *const c[] = { "command_list_begin", "setvol 41", "setvol 400", "setvol 42", "command_list_end", NULL };
    r = run_lines(&s, c, PASS);
    assert(!strcmp(r, "ACK [2@1] {setvol} Invalid volume value\n") && f_vol == 41); free(r);
    const char *const d[] = { "command_list_begin", "ping", "idle", "command_list_end", "nonsense", "", "add \"x", NULL };
    r = run_lines(&s, d, PASS);
    assert(strstr(r, "ACK [5@1] {idle}") && strstr(r, "ACK [5@0] {nonsense} unknown command") && strstr(r, "ACK [5@0] {} No command given")
           && strstr(r, "ACK [2@0] {} Invalid unquoted character")); free(r);
    const char *const e[] = { "idle player mixer", NULL }; r = run_lines(&s, e, PASS); assert(!strcmp(r, "<idle>\n") && s.idle && s.idle_mask == (MPD_EV_PLAYER | MPD_EV_MIXER)); free(r);
    const char *const f[] = { "status", NULL }; r = run_lines(&s, f, PASS); assert(!strcmp(r, "<close>\n")); free(r);   /* talking during idle */
    s.idle = 0;
    const char *const g[] = { "idle bogus", "noidle", "tagtypes", "tagtypes clear", "close", NULL }; r = run_lines(&s, g, PASS);
    assert(strstr(r, "ACK [2@0] {idle} Unrecognized idle event\n") && strstr(r, "tagtype: Artist\n") && strstr(r, "OK\nOK\n<close>")); free(r);
    mpd_sess_free(&s);
}

/* ---------------------------------------------------------------- the server, through a socket */
static volatile int g_stop;
static void *ui_thread(void *a){ (void)a; while(!g_stop){ msrv_pump(); usleep(1000); } return NULL; }
static int dial(int port){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    assert(connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0);
    return fd;
}
static void say(int fd, const char *s){ assert(send(fd, s, strlen(s), 0) == (ssize_t)strlen(s)); }
/* read until the answer ends with OK / ACK (or the timeout) */
static char *hear(int fd, int ms){
    static char b[65536]; int n = 0;
    for(;;){
        struct pollfd p = { fd, POLLIN, 0 };
        if(poll(&p, 1, ms) <= 0) break;
        ssize_t r = recv(fd, b + n, sizeof b - 1 - (size_t)n, 0);
        if(r <= 0) break;
        n += (int)r; b[n] = 0;
        if(n >= 3 && (!strcmp(b + n - 3, "OK\n") || (strrchr(b, '\n') && strstr(b, "ACK [") && b[n - 1] == '\n'))) break;
        if(!strncmp(b, "OK MPD", 6) && b[n - 1] == '\n' && !strchr(b, '\n')[1]) break;
    }
    b[n] = 0; return b;
}
static void t_server(void){
    pthread_t ui; g_stop = 0;
    msrv_set_handler(fake, NULL);
    assert(msrv_start("0.0.0.0", 0, PASS) == -1);                 /* never every interface */
    assert(msrv_start("127.0.0.1", 0, "short") == -1);
    assert(msrv_start("127.0.0.1", 0, PASS) == 0);
    int port = msrv_port(); assert(port > 0);
    pthread_create(&ui, NULL, ui_thread, NULL);
    int a = dial(port);
    assert(!strcmp(hear(a, 2000), "OK MPD " MPD_PROTO_VERSION "\n"));
    say(a, "status\n"); assert(strstr(hear(a, 2000), "ACK [4@0]"));
    say(a, "password testtesttesttesttest\nstatus\n");
    char *r = hear(a, 2000); if(!strstr(r, "state:")) r = hear(a, 2000);
    assert(strstr(r, "state: stop"));
    /* idle on a second connection; the first one changes the volume */
    int b = dial(port); hear(b, 2000);
    say(b, "password testtesttesttesttest\n"); assert(!strcmp(hear(b, 2000), "OK\n"));
    say(b, "idle mixer\n"); assert(!strcmp(hear(b, 200), ""));      /* nothing yet */
    say(a, "setvol 70\n"); assert(!strcmp(hear(a, 2000), "OK\n"));
    assert(!strcmp(hear(b, 2000), "changed: mixer\nOK\n"));
    say(b, "idle\n"); assert(!strcmp(hear(b, 200), ""));
    say(b, "noidle\n"); assert(!strcmp(hear(b, 2000), "OK\n"));
    /* events that happened between idles are kept */
    say(a, "add Music/x.flac\n"); hear(a, 2000);
    say(b, "idle playlist player\n"); assert(!strcmp(hear(b, 2000), "changed: playlist\nOK\n"));
    /* a long line is refused, the connection survives */
    char big[MPD_LINE_MAX + 100]; memset(big, 'x', sizeof big); big[sizeof big - 2] = '\n'; big[sizeof big - 1] = 0;
    say(a, big); assert(strstr(hear(a, 2000), "Line too long"));
    say(a, "ping\n"); assert(!strcmp(hear(a, 2000), "OK\n"));
    say(a, "close\n"); assert(!strcmp(hear(a, 1000), ""));
    close(a); close(b);
    /* more clients than slots: the extra one is closed at once */
    int fds[MPD_CLIENTS + 1];
    for(int i = 0; i <= MPD_CLIENTS; i++){ fds[i] = dial(port); usleep(20000); }
    assert(!strcmp(hear(fds[MPD_CLIENTS], 1000), ""));
    for(int i = 0; i <= MPD_CLIENTS; i++) close(fds[i]);
    usleep(100000);
    g_stop = 1; pthread_join(ui, NULL);
    msrv_stop();
    assert(!msrv_running());
}

static int serve(int port){
    pthread_t ui; g_stop = 0;
    msrv_set_handler(fake, NULL);
    if(msrv_start("127.0.0.1", port, PASS)){ fprintf(stderr, "can't listen\n"); return 1; }
    printf("serving on 127.0.0.1:%d\n", msrv_port()); fflush(stdout);
    pthread_create(&ui, NULL, ui_thread, NULL);
    pause();
    return 0;
}
int main(int argc, char **argv){
    if(argc == 3 && !strcmp(argv[1], "--serve")) return serve(atoi(argv[2]));
    t_tokenize(); t_args(); t_modes(); t_filters(); t_sessions(); t_server();
    printf("mpd_test: all passed\n");
    return 0;
}
