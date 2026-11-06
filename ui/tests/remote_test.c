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
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

static const char *TOK;   /* set in main */

static rmt_route_t route(const char *raw){
    static char buf[REMOTE_REQ_MAX];
    snprintf(buf, sizeof buf, "%s", raw);
    rmt_req_t r; rmt_route_t bad; memset(&bad, 0, sizeof bad);
    if(rmt_req_complete(buf, (int)strlen(buf)) != 1) { bad.kind = -1; return bad; }
    if(rmt_req_parse(buf, &r)) { bad.kind = -2; return bad; }
    return rmt_route(&r, TOK);
}
static rmt_route_t req(const char *method, const char *path, const char *body){
    char raw[1024];
    if(body) snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n%s", method, path, strlen(body), body);
    else     snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: x\r\n\r\n", method, path);
    return route(raw);
}
static char *p(const char *suffix){ static char b[256]; snprintf(b, sizeof b, "/%s%s", TOK, suffix); return b; }

static void test_core(void){
    unsigned char rnd[13]; for(int i = 0; i < 13; i++) rnd[i] = (unsigned char)(i * 37 + 11);
    char t[REMOTE_TOKEN_LEN + 1]; rmt_token_encode(rnd, sizeof rnd, t);
    CHECK(strlen(t) == REMOTE_TOKEN_LEN); CHECK(rmt_token_valid(t));
    unsigned char z[13] = {0}; char tz[REMOTE_TOKEN_LEN + 1]; rmt_token_encode(z, 13, tz);
    CHECK(!strcmp(tz, "aaaaaaaaaaaaaaaaaaaa"));
    unsigned char f[13]; memset(f, 0xff, 13); char tf[REMOTE_TOKEN_LEN + 1]; rmt_token_encode(f, 13, tf);
    CHECK(!strcmp(tf, "77777777777777777777"));
    CHECK(!rmt_token_valid("")); CHECK(!rmt_token_valid("ABCDEFGHIJKLMNOPQRST")); CHECK(!rmt_token_valid("aaaa"));
    CHECK(rmt_token_eq(t, strlen(t), t));
    CHECK(!rmt_token_eq(t, strlen(t) - 1, t));
    CHECK(!rmt_token_eq("", 0, ""));                       /* an unset token never matches */
    char t2[REMOTE_TOKEN_LEN + 1]; strcpy(t2, t); t2[5] = t2[5] == 'a' ? 'b' : 'a';
    CHECK(!rmt_token_eq(t2, strlen(t2), t));

    /* completeness */
    CHECK(rmt_req_complete("GET / HTTP/1.1\r\nHost: x\r\n", 25) == 0);
    CHECK(rmt_req_complete("GET / HTTP/1.1\r\n\r\n", 18) == 1);
    const char *pb = "POST /a HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc";
    CHECK(rmt_req_complete(pb, (int)strlen(pb)) == 0);
    const char *big = "POST /a HTTP/1.1\r\ncontent-length: 999999\r\n\r\n";
    CHECK(rmt_req_complete(big, (int)strlen(big)) == -1);
    const char *neg = "POST /a HTTP/1.1\r\nContent-Length: -1\r\n\r\n";
    CHECK(rmt_req_complete(neg, (int)strlen(neg)) == -1);

    /* routing */
    CHECK(req("GET", "/", NULL).kind == RMT_R_NOT_FOUND);
    CHECK(req("GET", "/aaaaaaaaaaaaaaaaaaaa/", NULL).kind == RMT_R_NOT_FOUND);
    CHECK(req("GET", p(""), NULL).kind == RMT_R_REDIRECT);
    CHECK(req("GET", p("/"), NULL).kind == RMT_R_PAGE);
    CHECK(req("POST", p("/"), "").kind == RMT_R_BAD_METHOD);
    CHECK(req("GET", p("/api/state"), NULL).kind == RMT_R_STATE);
    CHECK(req("GET", p("/api/state?x=1"), NULL).kind == RMT_R_STATE);
    CHECK(req("POST", p("/api/state"), "").kind == RMT_R_BAD_METHOD);
    CHECK(req("GET", p("/api/next"), NULL).kind == RMT_R_BAD_METHOD);   /* commands are POST only */
    CHECK(req("POST", p("/api/nope"), "").kind == RMT_R_NOT_FOUND);
    CHECK(req("POST", p("/../api/next"), "").kind == RMT_R_NOT_FOUND);
    rmt_route_t r = req("POST", p("/api/next"), "");
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_NEXT);
    r = req("POST", p("/api/previous"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_PREV);
    r = req("POST", p("/api/play_pause"), ""); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_PLAY_PAUSE);
    r = req("POST", p("/api/play"), ""); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_PLAY);
    r = req("POST", p("/api/pause"), ""); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_PAUSE);
    r = req("POST", p("/api/volume"), "value=50"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_VOLUME && r.cmd.arg == 50);
    r = req("POST", p("/api/volume?value=7"), ""); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_VOLUME && r.cmd.arg == 7);
    r = req("POST", p("/api/volume"), "step=-5"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_VOLUME_STEP && r.cmd.arg == -5);
    r = req("POST", p("/api/volume"), "step=%2B5"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_VOLUME_STEP && r.cmd.arg == 5);
    r = req("POST", p("/api/volume"), "x=1&step=+5"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_VOLUME_STEP && r.cmd.arg == 5);
    CHECK(req("POST", p("/api/volume"), "value=121").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/volume"), "value=-1").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/volume"), "value=5x").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/volume"), "value=").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/volume"), "").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/volume"), "value=99999999999999999999999").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/seek"), "position_ms=61000"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_SEEK && r.cmd.arg == 61000);
    CHECK(req("POST", p("/api/seek"), "").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/play_mode"), "mode=3"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_PLAY_MODE && r.cmd.arg == 3);
    CHECK(req("POST", p("/api/play_mode"), "mode=5").kind == RMT_R_BAD_PARAM);
    /* library */
    r = req("GET", p("/api/library/albums"), NULL);
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_BROWSE && r.cmd.sub == RMT_L_ALBUMS && r.cmd.offset == 0 && r.cmd.limit == RMT_LIMIT_DEF);
    r = req("GET", p("/api/library/albums?q=Caf%C3%A9+del+Mar&offset=200&limit=50"), NULL);
    CHECK(r.kind == RMT_R_CMD && !strcmp(r.cmd.q, "Caf\xc3\xa9 del Mar") && r.cmd.offset == 200 && r.cmd.limit == 50);
    CHECK(req("GET", p("/api/library/albums?limit=0"), NULL).kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/library/albums?limit=501"), NULL).kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/library/albums?q=%zz"), NULL).kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/library/albums?q=a%00b"), NULL).kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/library/albums"), "").kind == RMT_R_BAD_METHOD);
    CHECK(req("GET", p("/api/library/nope"), NULL).kind == RMT_R_NOT_FOUND);
    r = req("GET", p("/api/library/album?name=A%26B%20%2B%20C"), NULL);
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_L_ALBUM && !strcmp(r.cmd.name, "A&B + C"));
    CHECK(req("GET", p("/api/library/album"), NULL).kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/library/playlist?id=0"), NULL).kind == RMT_R_BAD_PARAM);
    r = req("GET", p("/api/library/playlist?id=4"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.id == 4);
    r = req("GET", p("/api/library/folder"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_L_FOLDER && !r.cmd.name[0]);
    r = req("GET", p("/api/library/folder?path=Music%2FJazz"), NULL); CHECK(r.kind == RMT_R_CMD && !strcmp(r.cmd.name, "Music/Jazz"));
    CHECK(req("GET", p("/api/library/folder?path=..%2Fetc"), NULL).kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/library/folder?path=%2Fetc"), NULL).kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/library/folder?path=a%2F..%2F.."), NULL).kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/library/folder?path=a%2F%2Fb"), NULL).kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/play_item"), "kind=album&name=Blue&song_id=12");
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_PLAY_ITEM && r.cmd.sub == RMT_I_ALBUM && !strcmp(r.cmd.name, "Blue") && r.cmd.song_id == 12);
    r = req("POST", p("/api/play_item"), "kind=playlist&id=3&pos=5"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_I_PLAYLIST && r.cmd.id == 3 && r.cmd.pos == 5);
    r = req("POST", p("/api/play_item"), "kind=file&path=Music%2Fa.flac"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_I_FILE && !strcmp(r.cmd.name, "Music/a.flac"));
    r = req("POST", p("/api/play_item"), "kind=all"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_I_ALL);
    CHECK(req("POST", p("/api/play_item"), "kind=file&path=..%2Fx").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/play_item"), "kind=radio&pos=2&url=http%3A%2F%2Fs.example%2Fa");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_I_RADIO && r.cmd.pos == 2 && !strcmp(r.cmd.name, "http://s.example/a"));
    r = req("POST", p("/api/play_item"), "kind=radio&pos=1"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_I_RADIO && !r.cmd.name[0]);
    CHECK(req("POST", p("/api/play_item"), "kind=radio").kind == RMT_R_BAD_PARAM);          /* which favourite */
    CHECK(req("POST", p("/api/play_item"), "kind=radio&pos=1&url=a%00b").kind == RMT_R_BAD_PARAM);
    r = req("GET", p("/api/library/radio?q=jazz"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_L_RADIO && !strcmp(r.cmd.q, "jazz"));
    r = req("POST", p("/api/radio_stop"), ""); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_RADIO_STOP);
    CHECK(req("GET", p("/api/radio_stop"), NULL).kind == RMT_R_BAD_METHOD);
    r = req("GET", p("/api/status"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_STATUS);
    r = req("GET", p("/api/eq"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_EQ_LIST);
    r = req("POST", p("/api/eq"), "preset=20"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_EQ && r.cmd.arg == 20);
    CHECK(req("POST", p("/api/eq"), "preset=21").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/status"), "").kind == RMT_R_BAD_METHOD);
    r = req("POST", p("/api/rescan"), "action=start"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_RESCAN && r.cmd.arg == 1);
    r = req("POST", p("/api/rescan"), "action=stop"); CHECK(r.kind == RMT_R_CMD && r.cmd.arg == 0);
    CHECK(req("POST", p("/api/rescan"), "").kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/rescan"), NULL).kind == RMT_R_BAD_METHOD);
    r = req("GET", p("/api/bluetooth"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_BT_LIST);
    r = req("POST", p("/api/bluetooth"), "action=off"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_BT && r.cmd.sub == RMT_BT_OFF);
    r = req("POST", p("/api/bluetooth"), "action=search"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BT_SEARCH);
    CHECK(req("POST", p("/api/bluetooth"), "action=connect&mac=00:11:22:33:44:55").kind == RMT_R_BAD_PARAM);   /* a device action */
    CHECK(req("POST", p("/api/bluetooth"), "action=dance").kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/bt_device"), NULL).kind == RMT_R_BAD_METHOD);
    r = req("POST", p("/api/bt_device"), "action=connect&mac=00:11:22:aa:BB:55"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BT_CONNECT && !strcmp(r.cmd.q, "00:11:22:aa:BB:55"));
    r = req("POST", p("/api/bt_device"), "action=rename&mac=00:11:22:33:44:55&name=Kitchen+%3Cx%3E"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BT_RENAME && !strcmp(r.cmd.name, "Kitchen <x>"));
    r = req("POST", p("/api/bt_device"), "action=rename&mac=00:11:22:33:44:55"); CHECK(r.kind == RMT_R_CMD && !r.cmd.name[0]);      /* clears it */
    CHECK(req("POST", p("/api/bt_device"), "action=rename&mac=00:11:22:33:44:55&name=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/bt_device"), "action=forget&mac=00:11:22:33:44").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/bt_device"), "action=forget&mac=00:11:22:33:44:5G").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/bt_device"), "action=forget&mac=00-11-22-33-44-55").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/bt_device"), "action=forget&mac=00:11:22:33:44:55%3Bx").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/bt_device"), "action=on&mac=00:11:22:33:44:55").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/playlist_create"), "name=Road+trip"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_EDIT && r.cmd.sub == RMT_E_PL_CREATE && !strcmp(r.cmd.name, "Road trip"));
    CHECK(req("POST", p("/api/playlist_create"), "name=++").kind == RMT_R_BAD_PARAM);             /* blank */
    CHECK(req("POST", p("/api/playlist_create"), "name=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa").kind == RMT_R_BAD_PARAM);   /* 65 bytes */
    r = req("POST", p("/api/playlist_rename"), "id=3&name=X"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_E_PL_RENAME && r.cmd.id == 3);
    CHECK(req("POST", p("/api/playlist_rename"), "name=X").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/playlist_delete"), "id=3"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_E_PL_DELETE);
    CHECK(req("GET", p("/api/playlist_delete?id=3"), NULL).kind == RMT_R_BAD_METHOD);
    r = req("POST", p("/api/playlist_add"), "id=3&kind=song&song_id=9"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_E_PL_ADD && r.cmd.item == RMT_I_SONG && r.cmd.song_id == 9);
    r = req("POST", p("/api/playlist_add"), "id=3&kind=album&name=Blue"); CHECK(r.kind == RMT_R_CMD && r.cmd.item == RMT_I_ALBUM && !strcmp(r.cmd.name, "Blue"));
    CHECK(req("POST", p("/api/playlist_add"), "id=3&kind=genre&name=Pop").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/playlist_remove"), "id=3&pos=2&song_id=9"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_E_PL_REMOVE && r.cmd.pos == 2);
    CHECK(req("POST", p("/api/playlist_remove"), "id=3&pos=2").kind == RMT_R_BAD_PARAM);          /* which song: lists change */
    r = req("POST", p("/api/favorite"), "on=1"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_E_FAV && r.cmd.arg == 1 && !r.cmd.song_id);
    r = req("POST", p("/api/favorite"), "on=0&song_id=5"); CHECK(r.kind == RMT_R_CMD && r.cmd.arg == 0 && r.cmd.song_id == 5);
    CHECK(req("POST", p("/api/favorite"), "on=2").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/radio_search"), "q=jazz+fm"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_RADIO_SEARCH && !strcmp(r.cmd.q, "jazz fm"));
    r = req("POST", p("/api/radio_search"), ""); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_RADIO_SEARCH && !r.cmd.q[0]);   /* popular */
    CHECK(req("GET", p("/api/radio_search"), NULL).kind == RMT_R_BAD_METHOD);           /* it downloads: POST only */
    r = req("GET", p("/api/library/radio_search"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_L_RADIO_HITS);
    r = req("POST", p("/api/play_item"), "kind=radio_hit&pos=3&url=http%3A%2F%2Fx"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_I_RADIO_HIT && r.cmd.pos == 3);
    CHECK(req("POST", p("/api/play_item"), "kind=radio_hit&pos=3").kind == RMT_R_BAD_PARAM);   /* hits need their url */
    r = req("POST", p("/api/radio_fav_add"), "kind=radio_hit&pos=1&url=http%3A%2F%2Fx");
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_RADIO_FAV && r.cmd.arg == 1 && r.cmd.sub == RMT_I_RADIO_HIT);
    r = req("POST", p("/api/radio_fav_remove"), "kind=radio&pos=2&url=http%3A%2F%2Fx"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_RADIO_FAV && r.cmd.arg == 0);
    CHECK(req("POST", p("/api/radio_fav_remove"), "kind=radio&pos=2").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/radio_fav_add"), "kind=song&song_id=2&url=x").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/play_item"), "kind=song").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/play_item"), "kind=album").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/play_item"), "kind=playlist&id=-1").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/play_item"), "kind=bogus").kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/play_item?kind=all"), NULL).kind == RMT_R_BAD_METHOD);
    CHECK(rmt_relpath_ok("") && rmt_relpath_ok("a/b c/d.flac") && rmt_relpath_ok("..a/b..") && !rmt_relpath_ok("a/") && !rmt_relpath_ok("./a"));
    CHECK(rmt_match("Hello World", "WORLD") && rmt_match("x", "") && !rmt_match("abc", "abd"));
    rmt_buf_t bb = {0}; rmt_buf_put(&bb, "["); rmt_buf_str(&bb, "q\"<"); for(int i = 0; i < 2000; i++) rmt_buf_printf(&bb, ",%d", i); rmt_buf_put(&bb, "]");
    char *bt = rmt_buf_take(&bb); CHECK(bt && !strncmp(bt, "[\"q\\\"\\u003c\",0,1,", 17) && strstr(bt, ",1999]")); free(bt);
    CHECK((int)route("GARBAGE\r\n\r\n").kind == -2);
    CHECK((int)route("GET nopath HTTP/1.1\r\n\r\n").kind == -2);

    /* JSON */
    char j[64];
    CHECK(rmt_json_escape(j, sizeof j, "a\"b\\c\n<\x01") > 0);
    CHECK(!strcmp(j, "a\\\"b\\\\c\\n\\u003c\\u0001"));
    CHECK(rmt_json_escape(j, 4, "abcdef") == -1 && strlen(j) < 4);
    rmt_state_t s; memset(&s, 0, sizeof s);
    snprintf(s.title, sizeof s.title, "Song \"1\""); snprintf(s.artist, sizeof s.artist, "Art");
    s.has_track = 1; s.playing = 1; s.position_ms = 1000; s.duration_ms = 2000; s.volume = 40; s.volume_max = 120;
    s.play_mode = 1; s.controllable = 1; s.source = "local";
    char js[12000]; CHECK(rmt_state_json(js, sizeof js, &s) > 0);
    CHECK(strstr(js, "\"radio_state\":\"off\"") && strstr(js, "\"station\":\"\""));
    CHECK(strstr(js, "\"title\":\"Song \\\"1\\\"\"") != NULL);
    CHECK(strstr(js, "\"playing\":true") && strstr(js, "\"volume\":40") && strstr(js, "\"source\":\"local\""));
    s.source = "radio"; s.radio_state = "playing"; snprintf(s.station, sizeof s.station, "Radio <%s>", "X");
    snprintf(s.station_url, sizeof s.station_url, "https://radio.example/stream?a=1&b=\"2\"");
    CHECK(rmt_state_json(js, sizeof js, &s) > 0 && strstr(js, "\"source\":\"radio\"") && strstr(js, "\"station\":\"Radio \\u003cX>\""));
    CHECK(strstr(js, "\"station_url\":\"https://radio.example/stream?a=1\\u0026b=\\\"2\\\"\"") || strstr(js, "\"station_url\":\"https://radio.example/stream?a=1&b=\\\"2\\\"\""));
    memset(s.title, 1, sizeof s.title - 1); s.title[sizeof s.title - 1] = 0;   /* worst-case escaping still fits */
    memset(s.album, 1, sizeof s.album - 1); s.album[sizeof s.album - 1] = 0; memset(s.artist, 1, sizeof s.artist - 1); s.artist[sizeof s.artist - 1] = 0;
    memset(s.station, 1, sizeof s.station - 1); s.station[sizeof s.station - 1] = 0; memset(s.radio_error, 1, sizeof s.radio_error - 1); s.radio_error[sizeof s.radio_error - 1] = 0; memset(s.station_url, 1, sizeof s.station_url - 1); s.station_url[sizeof s.station_url - 1] = 0; memset(s.stream_title, 1, sizeof s.stream_title - 1); s.stream_title[sizeof s.stream_title - 1] = 0;
    CHECK(rmt_state_json(js, sizeof js, &s) > 0);
    CHECK(strlen(RMT_PAGE_HTML) < 32768);
}

/* ---------------------------------------------------------------- server round trip */
static volatile int g_emu_run = 1, g_emu_pause = 0;
static int g_scan;
static time_t g_search_until; static char g_search_q[96]; static int g_nfav = 3;   /* stand-in radio directory */
static rmt_cmd_t g_last; static int g_ncmd;
static void notify(void){}
/* stand-in library: 250 albums "Album N" (by "Artist N%7"), 12 songs each; a folder tree Music/ with files */
static char *fake_browse(const rmt_cmd_t *c){
    rmt_buf_t b = {0}; long total = 0; int first = 1;
    rmt_buf_put(&b, "{\"ok\":true,\"items\":[");
#define ITEM_BEGIN() if(total++ < c->offset || total > c->offset + c->limit) continue; if(!first) rmt_buf_put(&b, ","); first = 0
    char nm[64];
    switch(c->sub){
    case RMT_L_ALBUMS:
        for(int i = 1; i <= 250; i++){ snprintf(nm, sizeof nm, "Album %d", i); if(!rmt_match(nm, c->q)) continue; ITEM_BEGIN();
            rmt_buf_put(&b, "{\"name\":"); rmt_buf_str(&b, nm); rmt_buf_printf(&b, ",\"artist\":\"Artist %d\",\"count\":12}", i % 7); }
        break;
    case RMT_L_ARTISTS: case RMT_L_PLAYLISTS:
        for(int i = 0; i < 7; i++){ snprintf(nm, sizeof nm, c->sub == RMT_L_ARTISTS ? "Artist %d" : "Playlist %d", i); ITEM_BEGIN();
            rmt_buf_printf(&b, "{\"id\":%d,\"name\":", i + 1); rmt_buf_str(&b, nm); rmt_buf_put(&b, "}"); }
        break;
    case RMT_L_RADIO: case RMT_L_RADIO_HITS: {
        int hits = c->sub == RMT_L_RADIO_HITS, busy = hits && time(NULL) < g_search_until;
        if(hits){ b.n = 0; rmt_buf_printf(&b, "{\"ok\":true,\"search\":\"%s\",\"items\":[", busy ? "busy" : g_search_q[0] ? "done" : "idle"); }
        for(int i = 1; i <= (hits ? (busy ? 0 : 5) : g_nfav); i++){
            if(hits) snprintf(nm, sizeof nm, "%.40s %d", g_search_q[0] == '*' ? "Popular" : g_search_q, i);
            else snprintf(nm, sizeof nm, "Station %d", i);
            if(!hits && !rmt_match(nm, c->q)) continue;
            ITEM_BEGIN();
            rmt_buf_printf(&b, "{\"pos\":%d,\"name\":", i); rmt_buf_str(&b, nm);
            rmt_buf_printf(&b, ",\"url\":\"http://s.example/%s%d\",\"codec\":\"MP3\",\"bitrate\":128,\"country\":\"Germany\",\"fav\":%s}",
                           hits ? "h" : "", i, hits && i != 2 ? "false" : "true"); }
        break; }
    case RMT_L_FOLDER:
        if(!c->name[0]) do{ ITEM_BEGIN(); rmt_buf_put(&b, "{\"name\":\"Music\",\"dir\":true}"); }while(0);
        else for(int i = 1; i <= 5; i++){ ITEM_BEGIN(); snprintf(nm, sizeof nm, "%02d \"quoted\" <b>.flac", i);
            rmt_buf_put(&b, "{\"name\":"); rmt_buf_str(&b, nm); rmt_buf_put(&b, ",\"dir\":false}"); }
        break;
    default:
        for(int i = 1; i <= 12; i++){ snprintf(nm, sizeof nm, "Song %d of %.40s", i, c->name[0] ? c->name : "all"); if(!rmt_match(nm, c->q)) continue; ITEM_BEGIN();
            rmt_buf_printf(&b, "{\"id\":%d,\"fav\":%s,\"title\":", 1000 + i, i % 3 ? "false" : "true"); rmt_buf_str(&b, nm);
            rmt_buf_printf(&b, ",\"artist\":\"Artist\",\"album\":\"Album\",\"dur_ms\":180000,\"track\":%d,\"pos\":%d}", i, i); }
    }
#undef ITEM_BEGIN
    rmt_buf_printf(&b, "],\"total\":%ld,\"offset\":%ld}", total, c->offset);
    return rmt_buf_take(&b);
}
/* stand-in Bluetooth: three devices, a search "finds" a fourth after 3 s, switching on takes 2 s */
static int g_bt_on = 1, g_bt_conn = 0, g_bt_n = 3; static time_t g_bt_until, g_bt_up;
static char g_bt_alias[4][48] = { "", "Kitchen", "", "" };
static const char *const BT_MAC[4] = { "00:11:22:33:44:01", "00:11:22:33:44:02", "00:11:22:33:44:03", "00:11:22:33:44:04" };
static const char *const BT_OWN[4] = { "FiiO EH11", "JBL Flip 5", "WH-1000XM4 <b>", "Car audio" };
static char *fake_bt(void){
    rmt_buf_t b; memset(&b, 0, sizeof b);
    time_t now = time(NULL);
    if(g_bt_until && now >= g_bt_until){ g_bt_until = 0; g_bt_n = 4; }
    if(g_bt_up && now >= g_bt_up){ g_bt_up = 0; g_bt_on = 1; }
    rmt_buf_printf(&b, "{\"ok\":true,\"state\":\"%s\",\"busy\":%s,\"devices\":[", g_bt_up ? "turning_on" : g_bt_on ? "on" : "off", g_bt_until ? "true" : "false");
    for(int pass = 0, k = 0; g_bt_on && pass < 2; pass++) for(int i = 0; i < g_bt_n; i++){
        if((i == g_bt_conn) == pass) continue;
        rmt_buf_put(&b, k++ ? ",{\"mac\":" : "{\"mac\":"); rmt_buf_str(&b, BT_MAC[i]);
        rmt_buf_put(&b, ",\"name\":"); rmt_buf_str(&b, g_bt_alias[i][0] ? g_bt_alias[i] : BT_OWN[i]);
        rmt_buf_put(&b, ",\"own\":"); rmt_buf_str(&b, BT_OWN[i]); rmt_buf_put(&b, ",\"alias\":"); rmt_buf_str(&b, g_bt_alias[i]);
        rmt_buf_printf(&b, ",\"connected\":%s,\"codec\":\"%s\"}", i == g_bt_conn ? "true" : "false", i == g_bt_conn ? "LDAC" : "");
    }
    rmt_buf_put(&b, "]}");
    return rmt_buf_take(&b);
}
static void fake_bt_cmd(const rmt_cmd_t *c){
    int i = 0; while(i < 4 && strcasecmp(BT_MAC[i], c->q)) i++;
    switch(c->sub){
    case RMT_BT_ON: if(!g_bt_on && !g_bt_up) g_bt_up = time(NULL) + 2; break;
    case RMT_BT_OFF: g_bt_on = 0; g_bt_up = 0; g_bt_until = 0; break;
    case RMT_BT_SEARCH: if(!g_bt_on){ rsrv_done(0, "Bluetooth is off"); return; } g_bt_until = time(NULL) + 3; break;
    case RMT_BT_REFRESH: break;
    case RMT_BT_CONNECT: if(i >= g_bt_n){ rsrv_done(0, "Couldn't reach that device"); return; } g_bt_conn = i; break;
    case RMT_BT_DISCONNECT: if(i == g_bt_conn) g_bt_conn = -1; break;
    case RMT_BT_FORGET: if(i >= g_bt_n){ rsrv_done(0, "Couldn't forget that device"); return; }
        if(i == g_bt_conn) g_bt_conn = -1;
        if(i == 3) g_bt_n = 3; else g_bt_alias[i][0] = 0;
        break;
    case RMT_BT_RENAME: if(i >= g_bt_n){ rsrv_done(0, "Couldn't save the name"); return; }
        snprintf(g_bt_alias[i], sizeof g_bt_alias[i], "%.47s", strcmp(c->name, BT_OWN[i]) ? c->name : ""); break;
    }
    rsrv_done(1, NULL);
}
static void *emu(void *a){
    (void)a;
    rmt_state_t s; memset(&s, 0, sizeof s);
    snprintf(s.title, sizeof s.title, "Track <one>"); s.has_track = 1; s.volume = 33; s.volume_max = 120; s.source = "local"; s.controllable = 1;
    snprintf(s.format, sizeof s.format, "FLAC"); s.bits = 24; s.sample_rate = 96000; s.kbps = 2304; s.channels = 2; s.favorite = 0;
    snprintf(s.art, sizeof s.art, "a1"); s.eq = 1; snprintf(s.eq_name, sizeof s.eq_name, "Jazz");
    while(g_emu_run){
        if(!g_emu_pause){
            rsrv_publish(&s);
            rmt_cmd_t c;
            if(rsrv_take(&c)){
                g_last = c; g_ncmd++;
                if(c.kind == RMT_CMD_PAUSE) rsrv_done(0, "Not in local \"playback\"");
                else if(c.kind == RMT_CMD_BROWSE) rsrv_done_body(1, NULL, fake_browse(&c));
                else if(c.kind == RMT_CMD_RESCAN){ g_scan = (int)c.arg; rsrv_done(1, NULL); }
                else if(c.kind == RMT_CMD_BT_LIST) rsrv_done_body(1, NULL, fake_bt());
                else if(c.kind == RMT_CMD_BT) fake_bt_cmd(&c);
                else if(c.kind == RMT_CMD_STATUS)
                    rsrv_done_body(1, NULL, strdup(g_scan ? "{\"ok\":true,\"scan\":{\"active\":true,\"done\":12},\"groups\":[{\"name\":\"Playback\",\"rows\":[[\"Library scan\",\"Running, 12 files so far\"]]}]}" : "{\"ok\":true,\"groups\":[{\"name\":\"Wi-Fi\",\"rows\":[[\"Network\",\"<home>\"],[\"Signal\",\"-60 dBm (good)\"]]},"
                                                    "{\"name\":\"Battery\",\"rows\":[[\"Charge\",\"89%\"],[\"Temperature\",\"33.0 \xC2\xB0" "C\"]]},{\"name\":\"Empty\",\"rows\":[]}]}"));
                else if(c.kind == RMT_CMD_EQ_LIST){
                    char b[400]; snprintf(b, sizeof b, "{\"ok\":true,\"current\":%d,\"presets\":[{\"id\":0,\"name\":\"Off\"},{\"id\":1,\"name\":\"Jazz\"},{\"id\":2,\"name\":\"R&B <x>\"}]}", s.eq);
                    rsrv_done_body(1, NULL, strdup(b));
                } else if(c.kind == RMT_CMD_EDIT){                       /* stand-in edits: create answers an id */
                    if(c.sub == RMT_E_PL_CREATE) rsrv_done_body(1, NULL, strdup("{\"ok\":true,\"id\":42}"));
                    else if(c.sub == RMT_E_PL_ADD) rsrv_done_body(1, NULL, strdup(c.item == RMT_I_SONG ? "{\"ok\":true,\"added\":1}" : "{\"ok\":true,\"added\":12}"));
                    else if(c.sub == RMT_E_FAV && !c.song_id){ s.favorite = (int)c.arg; rsrv_done(1, NULL); }
                    else rsrv_done(1, NULL);
                } else if(c.kind == RMT_CMD_EQ){ s.eq = (int)c.arg; snprintf(s.eq_name, sizeof s.eq_name, "%s", c.arg == 0 ? "Off" : c.arg == 1 ? "Jazz" : "R&B <x>"); rsrv_done(1, NULL); }
                else if(c.kind == RMT_CMD_COVER){                       /* a 16x16 24-bit BMP, a colour gradient */
                    size_t n = 54 + 16 * 16 * 3; unsigned char *b = calloc(1, n);
                    memcpy(b, "BM", 2); b[2] = (unsigned char)n; b[3] = (unsigned char)(n >> 8); b[10] = 54; b[14] = 40; b[18] = 16; b[22] = 16; b[26] = 1; b[28] = 24;
                    for(int i = 0; i < 256; i++){ b[54 + i * 3] = (unsigned char)(i); b[55 + i * 3] = 80; b[56 + i * 3] = (unsigned char)(255 - i); }
                    rsrv_done_blob((char *)b, n, "image/bmp");
                }
                else if(c.kind == RMT_CMD_PLAY_ITEM && c.sub == RMT_I_RADIO){        /* stand-in radio: it plays at once */
                    s.source = "radio"; s.radio_state = "playing"; snprintf(s.station, sizeof s.station, "Station %ld", c.pos);
                    snprintf(s.title, sizeof s.title, "Station %ld", c.pos);
                    snprintf(s.stream_title, sizeof s.stream_title, "Rude Norton - Tits On The Beach (1980) <x>");
                    snprintf(s.station_url, sizeof s.station_url, "%s", c.name[0] ? c.name : "https://radio.example/a-rather-long/path/to/the/stream.mp3?listener=remote"); rsrv_done(1, NULL);
                } else if(c.kind == RMT_CMD_RADIO_SEARCH){              /* "answers" two seconds later */
                    snprintf(g_search_q, sizeof g_search_q, "%s", c.q[0] ? c.q : "*"); g_search_until = time(NULL) + 2; rsrv_done(1, NULL);
                } else if(c.kind == RMT_CMD_RADIO_FAV){
                    if(!c.arg && c.sub == RMT_I_RADIO && g_nfav > 0) g_nfav--;
                    rsrv_done(1, NULL);
                } else if(c.kind == RMT_CMD_RADIO_STOP){
                    s.source = "local"; s.radio_state = "off"; s.station[0] = 0; s.station_url[0] = 0; s.stream_title[0] = 0; snprintf(s.title, sizeof s.title, "Track <one>"); rsrv_done(1, NULL);
                }
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
    static char out[65536];

    CHECK(call("GET", p("/"), NULL, out, sizeof out) == 200);
    CHECK(strstr(out, "diskOS Remote") && strstr(out, "Content-Security-Policy:") && strstr(out, "Cache-Control: no-store"));
    CHECK(call("GET", p(""), NULL, out, sizeof out) == 301 && strstr(out, "Location: /"));
    CHECK(call("GET", "/", NULL, out, sizeof out) == 404);
    CHECK(call("GET", "/aaaaaaaaaaaaaaaaaaaa/api/state", NULL, out, sizeof out) == 404);
    struct timespec ts = { 0, 50000000 }; nanosleep(&ts, NULL);
    CHECK(call("GET", p("/api/state"), NULL, out, sizeof out) == 200);
    CHECK(strstr(out, "application/json") && strstr(out, "\"title\":\"Track \\u003cone>\"") && strstr(out, "\"volume\":33"));
    CHECK(call("POST", p("/api/next"), "", out, sizeof out) == 200 && strstr(out, "{\"ok\":true}"));
    CHECK(g_last.kind == RMT_CMD_NEXT);
    CHECK(call("GET", p("/api/cover?v=a1"), NULL, out, sizeof out) == 200 && strstr(out, "Content-Type: image/bmp") && strstr(out, "Content-Length: 822"));
    CHECK(call("POST", p("/api/cover"), "", out, sizeof out) == 405);
    CHECK(call("GET", p("/api/state"), NULL, out, sizeof out) == 200 && strstr(out, "\"format\":\"FLAC\",\"kbps\":2304,\"bits\":24,\"sample_rate\":96000") && strstr(out, "\"art\":\"a1\""));
    CHECK(call("POST", p("/api/volume"), "value=77", out, sizeof out) == 200);
    CHECK(g_last.kind == RMT_CMD_VOLUME && g_last.arg == 77);
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
    CHECK(call("POST", p("/api/previous"), "", out, sizeof out) == 200 && g_last.kind == RMT_CMD_PREV);

    /* library through the main-thread mailbox, answered as JSON */
    CHECK(call("GET", p("/api/library/albums?q=Album+2&limit=5&offset=2"), NULL, out, sizeof out) == 200);
    CHECK(strstr(out, "\"total\":62") && strstr(out, "\"offset\":2") && strstr(out, "\"name\":\"Album 21\""));
    CHECK(call("GET", p("/api/library/folder?path=Music"), NULL, out, sizeof out) == 200 && strstr(out, "\\\"quoted\\\" \\u003cb>"));
    CHECK(call("POST", p("/api/play_item"), "kind=album&name=Album%201&song_id=1003", out, sizeof out) == 200);
    CHECK(g_last.kind == RMT_CMD_PLAY_ITEM && g_last.sub == RMT_I_ALBUM && g_last.song_id == 1003 && !strcmp(g_last.name, "Album 1"));
    CHECK(call("GET", p("/api/library/albums"), NULL, out, sizeof out) == 200 && strstr(out, "\"total\":250"));
    rsrv_stop();
    CHECK(!rsrv_running());
    CHECK(rsrv_start("127.0.0.1", 0, TOK) == 0);         /* restartable (a new Wi-Fi address) */
    CHECK(call("POST", p("/api/play"), "", out, sizeof out) == 200 && g_last.kind == RMT_CMD_PLAY);
    rsrv_stop();
    g_emu_run = 0; pthread_join(th, NULL);
}

int main(int argc, char **argv){
    static char tok[REMOTE_TOKEN_LEN + 1];
    unsigned char rnd[13] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    rmt_token_encode(rnd, sizeof rnd, tok); TOK = tok;
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
