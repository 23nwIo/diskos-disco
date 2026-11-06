/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote control: unit tests for remote_core.c, and a real HTTP round trip through remote_srv.c on 127.0.0.1 with a
 * stand-in for the main thread. Run: make -C ui/tests remote_test && ./ui/tests/remote_test */
#include "remote_core.h"
#include "remote_srv.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

static const char *TOK;   /* set in main */

static rc_route_t route(const char *raw){
    static char buf[REMOTE_REQ_MAX];
    snprintf(buf, sizeof buf, "%s", raw);
    rc_req_t r; rc_route_t bad = { RC_R_NOT_FOUND, { RC_CMD_NONE, 0 } };
    if(rc_req_complete(buf, (int)strlen(buf)) != 1) { bad.kind = -1; return bad; }
    if(rc_req_parse(buf, &r)) { bad.kind = -2; return bad; }
    return rc_route(&r, TOK);
}
static rc_route_t req(const char *method, const char *path, const char *body){
    char raw[1024];
    if(body) snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n%s", method, path, strlen(body), body);
    else     snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: x\r\n\r\n", method, path);
    return route(raw);
}
static char *p(const char *suffix){ static char b[256]; snprintf(b, sizeof b, "/%s%s", TOK, suffix); return b; }

static void test_core(void){
    unsigned char rnd[13]; for(int i = 0; i < 13; i++) rnd[i] = (unsigned char)(i * 37 + 11);
    char t[REMOTE_TOKEN_LEN + 1]; rc_token_encode(rnd, sizeof rnd, t);
    CHECK(strlen(t) == REMOTE_TOKEN_LEN); CHECK(rc_token_valid(t));
    unsigned char z[13] = {0}; char tz[REMOTE_TOKEN_LEN + 1]; rc_token_encode(z, 13, tz);
    CHECK(!strcmp(tz, "aaaaaaaaaaaaaaaaaaaa"));
    unsigned char f[13]; memset(f, 0xff, 13); char tf[REMOTE_TOKEN_LEN + 1]; rc_token_encode(f, 13, tf);
    CHECK(!strcmp(tf, "77777777777777777777"));
    CHECK(!rc_token_valid("")); CHECK(!rc_token_valid("ABCDEFGHIJKLMNOPQRST")); CHECK(!rc_token_valid("aaaa"));
    CHECK(rc_token_eq(t, strlen(t), t));
    CHECK(!rc_token_eq(t, strlen(t) - 1, t));
    CHECK(!rc_token_eq("", 0, ""));                       /* an unset token never matches */
    char t2[REMOTE_TOKEN_LEN + 1]; strcpy(t2, t); t2[5] = t2[5] == 'a' ? 'b' : 'a';
    CHECK(!rc_token_eq(t2, strlen(t2), t));

    /* completeness */
    CHECK(rc_req_complete("GET / HTTP/1.1\r\nHost: x\r\n", 25) == 0);
    CHECK(rc_req_complete("GET / HTTP/1.1\r\n\r\n", 18) == 1);
    const char *pb = "POST /a HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc";
    CHECK(rc_req_complete(pb, (int)strlen(pb)) == 0);
    const char *big = "POST /a HTTP/1.1\r\ncontent-length: 999999\r\n\r\n";
    CHECK(rc_req_complete(big, (int)strlen(big)) == -1);
    const char *neg = "POST /a HTTP/1.1\r\nContent-Length: -1\r\n\r\n";
    CHECK(rc_req_complete(neg, (int)strlen(neg)) == -1);

    /* routing */
    CHECK(req("GET", "/", NULL).kind == RC_R_NOT_FOUND);
    CHECK(req("GET", "/aaaaaaaaaaaaaaaaaaaa/", NULL).kind == RC_R_NOT_FOUND);
    CHECK(req("GET", p(""), NULL).kind == RC_R_REDIRECT);
    CHECK(req("GET", p("/"), NULL).kind == RC_R_PAGE);
    CHECK(req("POST", p("/"), "").kind == RC_R_BAD_METHOD);
    CHECK(req("GET", p("/api/state"), NULL).kind == RC_R_STATE);
    CHECK(req("GET", p("/api/state?x=1"), NULL).kind == RC_R_STATE);
    CHECK(req("POST", p("/api/state"), "").kind == RC_R_BAD_METHOD);
    CHECK(req("GET", p("/api/next"), NULL).kind == RC_R_BAD_METHOD);   /* commands are POST only */
    CHECK(req("POST", p("/api/nope"), "").kind == RC_R_NOT_FOUND);
    CHECK(req("POST", p("/../api/next"), "").kind == RC_R_NOT_FOUND);
    rc_route_t r = req("POST", p("/api/next"), "");
    CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_NEXT);
    r = req("POST", p("/api/previous"), NULL); CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_PREV);
    r = req("POST", p("/api/play_pause"), ""); CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_PLAY_PAUSE);
    r = req("POST", p("/api/play"), ""); CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_PLAY);
    r = req("POST", p("/api/pause"), ""); CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_PAUSE);
    r = req("POST", p("/api/volume"), "value=50"); CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_VOLUME && r.cmd.arg == 50);
    r = req("POST", p("/api/volume?value=7"), ""); CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_VOLUME && r.cmd.arg == 7);
    r = req("POST", p("/api/volume"), "step=-5"); CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_VOLUME_STEP && r.cmd.arg == -5);
    r = req("POST", p("/api/volume"), "step=%2B5"); CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_VOLUME_STEP && r.cmd.arg == 5);
    r = req("POST", p("/api/volume"), "x=1&step=+5"); CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_VOLUME_STEP && r.cmd.arg == 5);
    CHECK(req("POST", p("/api/volume"), "value=121").kind == RC_R_BAD_PARAM);
    CHECK(req("POST", p("/api/volume"), "value=-1").kind == RC_R_BAD_PARAM);
    CHECK(req("POST", p("/api/volume"), "value=5x").kind == RC_R_BAD_PARAM);
    CHECK(req("POST", p("/api/volume"), "value=").kind == RC_R_BAD_PARAM);
    CHECK(req("POST", p("/api/volume"), "").kind == RC_R_BAD_PARAM);
    CHECK(req("POST", p("/api/volume"), "value=99999999999999999999999").kind == RC_R_BAD_PARAM);
    r = req("POST", p("/api/seek"), "position_ms=61000"); CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_SEEK && r.cmd.arg == 61000);
    CHECK(req("POST", p("/api/seek"), "").kind == RC_R_BAD_PARAM);
    r = req("POST", p("/api/play_mode"), "mode=3"); CHECK(r.kind == RC_R_CMD && r.cmd.kind == RC_CMD_PLAY_MODE && r.cmd.arg == 3);
    CHECK(req("POST", p("/api/play_mode"), "mode=5").kind == RC_R_BAD_PARAM);
    CHECK((int)route("GARBAGE\r\n\r\n").kind == -2);
    CHECK((int)route("GET nopath HTTP/1.1\r\n\r\n").kind == -2);

    /* JSON */
    char j[64];
    CHECK(rc_json_escape(j, sizeof j, "a\"b\\c\n<\x01") > 0);
    CHECK(!strcmp(j, "a\\\"b\\\\c\\n\\u003c\\u0001"));
    CHECK(rc_json_escape(j, 4, "abcdef") == -1 && strlen(j) < 4);
    rc_state_t s; memset(&s, 0, sizeof s);
    snprintf(s.title, sizeof s.title, "Song \"1\""); snprintf(s.artist, sizeof s.artist, "Art");
    s.has_track = 1; s.playing = 1; s.position_ms = 1000; s.duration_ms = 2000; s.volume = 40; s.volume_max = 120;
    s.play_mode = 1; s.controllable = 1; s.source = "local";
    char js[3400]; CHECK(rc_state_json(js, sizeof js, &s) > 0);
    CHECK(strstr(js, "\"title\":\"Song \\\"1\\\"\"") != NULL);
    CHECK(strstr(js, "\"playing\":true") && strstr(js, "\"volume\":40") && strstr(js, "\"source\":\"local\""));
    memset(s.title, 1, sizeof s.title - 1); s.title[sizeof s.title - 1] = 0;   /* worst-case escaping still fits */
    CHECK(rc_state_json(js, sizeof js, &s) > 0);
    CHECK(strlen(RC_PAGE_HTML) < 8192);
}

/* ---------------------------------------------------------------- server round trip */
static volatile int g_emu_run = 1, g_emu_pause = 0;
static rc_cmd_t g_last; static int g_ncmd;
static void notify(void){}
static void *emu(void *a){
    (void)a;
    rc_state_t s; memset(&s, 0, sizeof s);
    snprintf(s.title, sizeof s.title, "Track <one>"); s.has_track = 1; s.volume = 33; s.volume_max = 120; s.source = "local"; s.controllable = 1;
    while(g_emu_run){
        if(!g_emu_pause){
            rsrv_publish(&s);
            rc_cmd_t c;
            if(rsrv_take(&c)){
                g_last = c; g_ncmd++;
                if(c.kind == RC_CMD_PAUSE) rsrv_done(0, "Not in local \"playback\"");
                else rsrv_done(1, NULL);
            }
        }
        struct timespec ts = { 0, 5000000 }; nanosleep(&ts, NULL);
    }
    return NULL;
}
static int http(const char *raw, char *out, size_t cap){
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons((unsigned short)rsrv_port());
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if(connect(c, (void *)&a, sizeof a)){ close(c); return -1; }
    size_t n = strlen(raw), o = 0;
    while(o < n){ ssize_t w = send(c, raw + o, n - o, MSG_NOSIGNAL); if(w <= 0) break; o += (size_t)w; }
    size_t got = 0; ssize_t r;
    while(got < cap - 1 && (r = recv(c, out + got, cap - 1 - got, 0)) > 0) got += (size_t)r;
    out[got] = 0; close(c);
    int code = 0; sscanf(out, "HTTP/1.1 %d", &code);
    return code;
}
static int call(const char *method, const char *path, const char *body, char *out, size_t cap){
    char raw[1024];
    snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n\r\n%s",
             method, path, body ? strlen(body) : 0, body ? body : "");
    return http(raw, out, cap);
}
static void test_server(void){
    CHECK(rsrv_start("0.0.0.0", 0, TOK) == -1);          /* never every interface */
    CHECK(rsrv_start("127.0.0.1", 0, "short") == -1);    /* never without a real token */
    CHECK(rsrv_start("127.0.0.1", 0, TOK) == 0);
    CHECK(rsrv_running() && rsrv_port() > 0);
    rsrv_set_notify(notify);
    pthread_t th; pthread_create(&th, NULL, emu, NULL);
    static char out[16384];

    CHECK(call("GET", p("/"), NULL, out, sizeof out) == 200);
    CHECK(strstr(out, "diskOS Remote") && strstr(out, "Content-Security-Policy:") && strstr(out, "Cache-Control: no-store"));
    CHECK(call("GET", p(""), NULL, out, sizeof out) == 301 && strstr(out, "Location: /"));
    CHECK(call("GET", "/", NULL, out, sizeof out) == 404);
    CHECK(call("GET", "/aaaaaaaaaaaaaaaaaaaa/api/state", NULL, out, sizeof out) == 404);
    struct timespec ts = { 0, 50000000 }; nanosleep(&ts, NULL);
    CHECK(call("GET", p("/api/state"), NULL, out, sizeof out) == 200);
    CHECK(strstr(out, "application/json") && strstr(out, "\"title\":\"Track \\u003cone>\"") && strstr(out, "\"volume\":33"));
    CHECK(call("POST", p("/api/next"), "", out, sizeof out) == 200 && strstr(out, "{\"ok\":true}"));
    CHECK(g_last.kind == RC_CMD_NEXT);
    CHECK(call("POST", p("/api/volume"), "value=77", out, sizeof out) == 200);
    CHECK(g_last.kind == RC_CMD_VOLUME && g_last.arg == 77);
    CHECK(call("POST", p("/api/pause"), "", out, sizeof out) == 409);
    CHECK(strstr(out, "\"message\":\"Not in local \\\"playback\\\"\""));
    CHECK(call("GET", p("/api/next"), NULL, out, sizeof out) == 405);
    CHECK(call("POST", p("/api/volume"), "value=500", out, sizeof out) == 400);
    int before = g_ncmd;
    CHECK(call("POST", "/aaaaaaaaaaaaaaaaaaaa/api/next", "", out, sizeof out) == 404);
    CHECK(g_ncmd == before);                              /* a wrong token never reaches the player */

    /* too large: refused, not buffered */
    char *huge = malloc(REMOTE_REQ_MAX + 100);
    memset(huge, 'a', REMOTE_REQ_MAX + 99); memcpy(huge, "GET /", 5); huge[REMOTE_REQ_MAX + 99] = 0;
    CHECK(http(huge, out, sizeof out) == 400);
    free(huge);

    /* main thread not answering: 504 after the wait, and the command is withdrawn (it never runs late) */
    g_emu_pause = 1; before = g_ncmd;
    time_t t0 = time(NULL);
    CHECK(call("POST", p("/api/next"), "", out, sizeof out) == 504);
    CHECK(time(NULL) - t0 >= 2);
    g_emu_pause = 0; nanosleep(&ts, NULL);
    CHECK(g_ncmd == before);
    CHECK(call("POST", p("/api/previous"), "", out, sizeof out) == 200 && g_last.kind == RC_CMD_PREV);

    rsrv_stop();
    CHECK(!rsrv_running());
    CHECK(rsrv_start("127.0.0.1", 0, TOK) == 0);         /* restartable (a new Wi-Fi address) */
    CHECK(call("POST", p("/api/play"), "", out, sizeof out) == 200 && g_last.kind == RC_CMD_PLAY);
    rsrv_stop();
    g_emu_run = 0; pthread_join(th, NULL);
}

int main(int argc, char **argv){
    static char tok[REMOTE_TOKEN_LEN + 1];
    unsigned char rnd[13] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    rc_token_encode(rnd, sizeof rnd, tok); TOK = tok;
    if(argc == 3 && !strcmp(argv[1], "--serve")){          /* manual/browser check: serve the page with the stand-in player */
        if(rsrv_start("127.0.0.1", atoi(argv[2]), TOK)) return 1;
        printf("http://127.0.0.1:%d/%s/\n", rsrv_port(), TOK); fflush(stdout);
        pthread_t th; pthread_create(&th, NULL, emu, NULL); pthread_join(th, NULL);
        return 0;
    }
    test_core();
    test_server();
    if(fails){ fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    puts("PASS remote control: parsing, routing, token, JSON, HTTP round trip");
    return 0;
}
