/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote control: unit tests for remote_core.c, and a real HTTP round trip through remote_srv.c on 127.0.0.1 with a
 * stand-in for the main thread. Run: make -C ui/tests remote_test && ./ui/tests/remote_test */
#include "remote_core.h"
#include "remote_srv.h"
/* the upload thread's SD lease (sdio.c): the test's card is a temp folder */
int sd_io_begin(void){ return 1; }
void sd_io_end(void){}
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

static rmt_route_t preq(const char *method, const char *path, const char *body){
    static char buf[REMOTE_REQ_MAX];
    if(body) snprintf(buf, sizeof buf, "%s %s HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s", method, path, strlen(body), body);
    else     snprintf(buf, sizeof buf, "%s %s HTTP/1.1\r\n\r\n", method, path);
    rmt_req_t r; rmt_route_t bad; memset(&bad, 0, sizeof bad); bad.kind = -1;
    if(rmt_req_parse(buf, &r)) return bad;
    return rmt_route_party(&r, "Fest_1");
}
/* Hit&Guess (Jo, 2026-11-18): the players' requests on the guests' link, the host's on the own link */
/* Jukebox guests and Bandcamp (Jo, 2026-11-22): search, a page and a pick of one track, as themselves */
static void test_party_bandcamp(void){
    rmt_route_t r = preq("GET", "/Fest_1/api/bandcamp?g=AnnaAnnaAnna1234", NULL);
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_BANDCAMP && r.cmd.sub == RMT_BC_STATE && r.cmd.guest);
    CHECK(preq("GET", "/Fest_1/api/bandcamp", NULL).kind == RMT_R_BAD_PARAM);                 /* a guest is their id */
    r = preq("POST", "/Fest_1/api/bandcamp", "action=search&q=aphex&g=AnnaAnnaAnna1234");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_SEARCH && !strcmp(r.cmd.q, "aphex"));
    r = preq("POST", "/Fest_1/api/bandcamp", "action=open&url=https%3A%2F%2Fa.bandcamp.com%2Falbum%2Fx&g=AnnaAnnaAnna1234");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_OPEN);
    r = preq("POST", "/Fest_1/api/bandcamp", "action=download&url=https%3A%2F%2Fa.bandcamp.com%2Falbum%2Fx&track=2&g=AnnaAnnaAnna1234");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_DOWNLOAD && r.cmd.pos == 2);
    /* not a whole album, nor the host's own actions */
    CHECK(preq("POST", "/Fest_1/api/bandcamp", "action=download&url=https%3A%2F%2Fa.bandcamp.com%2Falbum%2Fx&g=AnnaAnnaAnna1234").kind == RMT_R_NOT_FOUND);
    CHECK(preq("POST", "/Fest_1/api/bandcamp", "action=play&url=https%3A%2F%2Fa.bandcamp.com%2Falbum%2Fx&g=AnnaAnnaAnna1234").kind == RMT_R_NOT_FOUND);
    CHECK(preq("POST", "/Fest_1/api/bandcamp", "action=quality&q=1&g=AnnaAnnaAnna1234").kind == RMT_R_NOT_FOUND);
    CHECK(preq("POST", "/Fest_1/api/bandcamp", "action=fav_remove&url=x&g=AnnaAnnaAnna1234").kind == RMT_R_NOT_FOUND);
    CHECK(preq("GET", "/Fest_1/api/bandcamp?favs=1&g=AnnaAnnaAnna1234", NULL).kind == RMT_R_NOT_FOUND);
}
static void test_game(void){
    rmt_route_t r = preq("GET", "/Fest_1/api/game?g=AnnaAnnaAnna1234", NULL);
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_GAME && r.cmd.sub == RMT_G_STATE && r.cmd.guest && !strcmp(r.cmd.gid, "AnnaAnnaAnna1234"));
    CHECK(preq("GET", "/Fest_1/api/game", NULL).kind == RMT_R_BAD_PARAM);                    /* a player is their id */
    r = preq("POST", "/Fest_1/api/game", "action=join&g=AnnaAnnaAnna1234&n=Anna+B");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_G_JOIN && !strcmp(r.cmd.gname, "Anna B"));
    r = preq("POST", "/Fest_1/api/game", "action=pick&song_id=7&g=AnnaAnnaAnna1234");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_G_PICK && r.cmd.song_id == 7);
    CHECK(preq("POST", "/Fest_1/api/game", "action=pick&g=AnnaAnnaAnna1234").kind == RMT_R_BAD_PARAM);
    r = preq("POST", "/Fest_1/api/game", "action=mark&p=2&m=5&g=AnnaAnnaAnna1234");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_G_MARK && r.cmd.pos == 2 && r.cmd.arg == 5);
    CHECK(preq("POST", "/Fest_1/api/game", "action=mark&p=2&m=64&g=AnnaAnnaAnna1234").kind == RMT_R_BAD_PARAM);
    CHECK(preq("POST", "/Fest_1/api/game", "action=mark&p=32&m=1&g=AnnaAnnaAnna1234").kind == RMT_R_BAD_PARAM);
    r = preq("POST", "/Fest_1/api/game", "action=tip&a=Queen&t=Bohemian+Rhapsody&y=1975&g=AnnaAnnaAnna1234");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_G_TIP && !strcmp(r.cmd.q, "Queen") && !strcmp(r.cmd.text, "Bohemian Rhapsody") && r.cmd.arg == 1975);
    r = preq("POST", "/Fest_1/api/game", "action=tip&g=AnnaAnnaAnna1234");
    CHECK(r.kind == RMT_R_CMD && !r.cmd.q[0] && !r.cmd.text[0] && !r.cmd.arg);
    CHECK(preq("POST", "/Fest_1/api/game", "action=start&g=AnnaAnnaAnna1234").kind == RMT_R_BAD_PARAM);   /* the host's, not a player's */
    CHECK(preq("POST", "/Fest_1/api/game", "action=nope&g=AnnaAnnaAnna1234").kind == RMT_R_BAD_PARAM);
    r = preq("GET", "/Fest_1/api/game_search?q=abba&offset=40&limit=40&g=AnnaAnnaAnna1234", NULL);
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_GAME_SEARCH && !strcmp(r.cmd.q, "abba") && r.cmd.offset == 40 && r.cmd.limit == 40);
    CHECK(preq("GET", "/Fest_1/api/game_search?limit=500&g=AnnaAnnaAnna1234", NULL).kind == RMT_R_BAD_PARAM);
    CHECK(preq("GET", "/Fest_1/api/game_admin", NULL).kind == RMT_R_NOT_FOUND);              /* guests never reach the host's */
    CHECK(preq("GET", "/Fest_1/hitguess", NULL).kind == RMT_R_NOT_FOUND);
    CHECK(req("GET", p("/api/game?g=AnnaAnnaAnna1234"), NULL).kind == RMT_R_NOT_FOUND);       /* the players' API is on their link */
    CHECK(req("GET", p("/hitguess"), NULL).kind == RMT_R_GAME_PAGE);
    r = req("GET", p("/api/game_admin"), NULL);
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_GAME_ADMIN && !r.cmd.arg);
    r = req("POST", p("/api/game_admin"), "action=remove&p=3");
    CHECK(r.kind == RMT_R_CMD && r.cmd.arg == 1 && r.cmd.sub == RMT_G_REMOVE && r.cmd.pos == 3);
    CHECK(req("POST", p("/api/game_admin"), "action=remove").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/game_admin"), "action=start");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_G_START);
    r = req("POST", p("/api/game_admin"), "pick=2&win_n=10&dir=Music%2F80s");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_G_SETUP && r.cmd.game[HG_S_PICK] == 2 && r.cmd.game[HG_S_WIN_N] == 10 && r.cmd.game[HG_S_YEAR] == -1
          && r.cmd.item == 1 && !strcmp(r.cmd.name, "Music/80s") && r.cmd.id == -1);
    r = req("POST", p("/api/game_admin"), "dir=");
    CHECK(r.kind == RMT_R_CMD && r.cmd.item == 1 && !r.cmd.name[0]);                          /* the card's root */
    CHECK(req("POST", p("/api/game_admin"), "pick=3").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/game_admin"), "dir=..%2Fetc").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/game_admin"), "").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/game_admin"), "pl=4");
    CHECK(r.kind == RMT_R_CMD && r.cmd.id == 4 && !r.cmd.item);
    CHECK(req("POST", p("/api/party"), "on=2").kind == RMT_R_CMD && req("POST", p("/api/party"), "on=3").kind == RMT_R_BAD_PARAM);
}
static void test_party(void){
    CHECK(rmt_party_name_ok("ab") && rmt_party_name_ok("Fest_1-x") && !rmt_party_name_ok("a") && !rmt_party_name_ok("System")
          && !rmt_party_name_ok("api") && !rmt_party_name_ok("a b") && !rmt_party_name_ok("f%C3%BC") && !rmt_party_name_ok("123456789012345678901234567890123"));
    /* turns: who queued each song (0 = the host) */
    int q1[] = { 1, 2 }; CHECK(rmt_party_place(q1, 2, 1, 1, 0) == 2);          /* A B + A -> A B A */
    int q2[] = { 1, 2, 1 }; CHECK(rmt_party_place(q2, 3, 2, 1, 0) == 3);       /* B's 2nd after A's 2nd */
    int q3[] = { 1, 1, 1 }; CHECK(rmt_party_place(q3, 3, 2, 1, 0) == 1);       /* B's 1st right after A's 1st */
    CHECK(rmt_party_place(q3, 3, 1, 1, 0) == 3);                                 /* only A's songs wait: A may go on */
    CHECK(rmt_party_place(NULL, 0, 1, 1, 1) == 0);                               /* nothing queued, A's song plays: fine */
    int q4[] = { 2, 1 }; CHECK(rmt_party_place(q4, 2, 1, 1, 0) == -1);         /* B A + A: two of A's in a row */
    CHECK(rmt_party_place(q4, 2, 1, 2, 0) == 2);                                 /* allowed with 2 in a row */
    int q5[] = { 0, 0, 2 }; CHECK(rmt_party_place(q5, 3, 1, 1, 0) == 3);       /* never before the host's songs */
    int q6[] = { 1, 0 }; CHECK(rmt_party_place(q6, 2, 1, 1, 1) == 2);          /* the host's song separates */
    int q7[] = { 2 }; CHECK(rmt_party_place(q7, 1, 1, 1, 1) == 1);             /* A plays now, B queued: A after B */
    /* the guests' link: their page and API only, everything else 404 */
    CHECK(preq("GET", "/Fest_1", NULL).kind == RMT_R_REDIRECT && preq("GET", "/Fest_1", NULL).cmd.guest);
    CHECK(preq("GET", "/Fest_1/", NULL).kind == RMT_R_PARTY_PAGE);
    CHECK(preq("GET", "/fest_1/", NULL).kind == RMT_R_NOT_FOUND && preq("GET", "/Fest_12/", NULL).kind == RMT_R_NOT_FOUND);
    CHECK(preq("GET", "/Fest_1/system", NULL).kind == RMT_R_NOT_FOUND);
    rmt_route_t r = preq("GET", "/Fest_1/api/state?g=abcdefgh12", NULL);
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_PARTY_STATE && r.cmd.guest && !strcmp(r.cmd.gid, "abcdefgh12"));
    CHECK(preq("GET", "/Fest_1/api/state?g=short", NULL).kind == RMT_R_BAD_PARAM);
    r = preq("POST", "/Fest_1/api/queue", "action=add&kind=song&song_id=3&g=abcdefgh12&n=Anna+B");
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_QUEUE && r.cmd.sub == RMT_Q_ADD && r.cmd.song_id == 3 && !strcmp(r.cmd.gname, "Anna B"));
    CHECK(preq("POST", "/Fest_1/api/queue", "action=add&kind=song&song_id=3").kind == RMT_R_BAD_PARAM);   /* who adds it? */
    CHECK(preq("POST", "/Fest_1/api/queue", "action=add&kind=album&name=X&g=abcdefgh12").kind == RMT_R_NOT_FOUND);   /* single songs only */
    CHECK(preq("POST", "/Fest_1/api/queue", "action=clear&ver=1&g=abcdefgh12").kind == RMT_R_NOT_FOUND);
    CHECK(preq("POST", "/Fest_1/api/queue", "action=add&kind=file&path=a%2Fb.flac&g=abcdefgh12").cmd.item == RMT_I_FILE);
    CHECK(preq("GET", "/Fest_1/api/queue", NULL).kind == RMT_R_CMD);
    CHECK(preq("POST", "/Fest_1/api/volume", "value=100").kind == RMT_R_CMD && preq("POST", "/Fest_1/api/volume", "value=101").kind == RMT_R_BAD_PARAM);
    CHECK(preq("POST", "/Fest_1/api/volume", "step=5").kind == RMT_R_NOT_FOUND);
    CHECK(preq("POST", "/Fest_1/api/next", "").kind == RMT_R_CMD && preq("POST", "/Fest_1/api/previous", "").kind == RMT_R_NOT_FOUND);
    const char *no[] = { "/Fest_1/api/play_pause", "/Fest_1/api/balance", "/Fest_1/api/outputs", "/Fest_1/api/eq", "/Fest_1/api/bluetooth",
                         "/Fest_1/api/reboot", "/Fest_1/api/party", "/Fest_1/api/list_end", "/Fest_1/api/favorite", "/Fest_1/api/playlist_create",
                         "/Fest_1/api/radio_fav_add", "/Fest_1/api/work_mode", "/Fest_1/api/touch", "/Fest_1/api/seek", "/Fest_1/api/radio_stop" };
    for(size_t i = 0; i < sizeof no / sizeof no[0]; i++) CHECK(preq("POST", no[i], "value=1&preset=1&confirm=1&g=abcdefgh12").kind == RMT_R_NOT_FOUND
                                                               || preq("POST", no[i], "value=1&preset=1&confirm=1&g=abcdefgh12").kind == RMT_R_BAD_PARAM);
    CHECK(preq("GET", "/Fest_1/api/status", NULL).kind == RMT_R_NOT_FOUND && preq("GET", "/Fest_1/api/screen", NULL).kind == RMT_R_NOT_FOUND);
    CHECK(preq("GET", "/Fest_1/api/library/playlists", NULL).kind == RMT_R_CMD && preq("GET", "/Fest_1/api/library/folder?path=Music", NULL).kind == RMT_R_CMD);
    CHECK(preq("GET", "/Fest_1/api/library/radio", NULL).kind == RMT_R_NOT_FOUND && preq("GET", "/Fest_1/api/library/favorites", NULL).kind == RMT_R_NOT_FOUND);
    CHECK(preq("POST", "/Fest_1/api/play_item", "kind=song&song_id=1").kind == RMT_R_NOT_FOUND);
    CHECK(preq("POST", "/Fest_1/api/play_item", "kind=radio_hit&pos=1&url=http%3A%2F%2Fx").kind == RMT_R_NOT_FOUND);
    r = preq("POST", "/Fest_1/api/radio", "name=Jazz+FM&url=https%3A%2F%2Fs.example%2Fj&now=1&g=abcdefgh12");
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_PARTY_RADIO && r.cmd.arg == 1 && !strcmp(r.cmd.q, "Jazz FM") && !strcmp(r.cmd.name, "https://s.example/j") && r.cmd.guest);
    CHECK(preq("POST", "/Fest_1/api/radio", "name=X&url=file%3A%2F%2F%2Fetc%2Fpasswd").kind == RMT_R_BAD_PARAM && preq("POST", "/Fest_1/api/radio", "url=http%3A%2F%2Fx").kind == RMT_R_BAD_PARAM);
    CHECK(preq("GET", "/Fest_1/api/radio", NULL).kind == RMT_R_BAD_METHOD && req("POST", p("/api/radio"), "name=X&url=http%3A%2F%2Fx").kind == RMT_R_NOT_FOUND);
    CHECK(preq("POST", "/Fest_1/api/radio_search", "q=jazz").kind == RMT_R_CMD && preq("GET", "/Fest_1/api/library/radio_search", NULL).kind == RMT_R_CMD);
    /* the guests' own stations, for the host to see: s=<url> <name>, bad ones left out */
    r = preq("POST", "/Fest_1/api/stations", "g=abcdefgh12&n=Anna&s=https%3A%2F%2Fa.example%2Fs+Jazz+%3CFM%3E&s=ftp%3A%2F%2Fx+Bad&s=nospace&s=http%3A%2F%2Fb.example+B");
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_PARTY_STATIONS && r.cmd.guest && !strcmp(r.cmd.text, "Jazz <FM>\thttps://a.example/s\nB\thttp://b.example\n"));
    CHECK(preq("POST", "/Fest_1/api/stations", "s=http%3A%2F%2Fb.example+B").kind == RMT_R_BAD_PARAM && preq("GET", "/Fest_1/api/stations", NULL).kind == RMT_R_BAD_METHOD);
    CHECK(preq("POST", "/Fest_1/api/stations", "g=abcdefgh12").kind == RMT_R_CMD && !preq("POST", "/Fest_1/api/stations", "g=abcdefgh12").cmd.text[0]);   /* all removed */
    CHECK(strstr(RMT_PARTY_HTML, "'" RMT_UPLOAD_EXTS "'") != NULL);   /* the page offers what the Disc takes */
    /* uploads: the saved name, and the request's head (the body goes straight to the card) */
    char nm[160];
    CHECK(rmt_upload_name("Anna B", "C:\\Music\\My Song (live).MP3", nm, sizeof nm) == 0 && !strcmp(nm, "Anna_B_My_Song_(live).mp3"));
    CHECK(rmt_upload_name("", "../../etc/x.flac", nm, sizeof nm) == 0 && !strcmp(nm, "guest_x.flac"));
    CHECK(rmt_upload_name("Zoë", "Für Elise.opus", nm, sizeof nm) == 0 && !strcmp(nm, "Zoë_Für_Elise.opus"));
    CHECK(rmt_upload_name("A/B", "a?b*c.ogg", nm, sizeof nm) == 0 && !strcmp(nm, "A_B_a_b_c.ogg"));
    CHECK(rmt_upload_name("A", "song.exe", nm, sizeof nm) == -1 && rmt_upload_name("A", "noext", nm, sizeof nm) == -1 && rmt_upload_name("A", ".flac", nm, sizeof nm) == -1
          && rmt_upload_name("A", "x.mp", nm, sizeof nm) == -1 && rmt_upload_name("A", "x.m4b", nm, sizeof nm) == -1 && rmt_upload_name("A", "???.mp3", nm, sizeof nm) == -1);
    char longn[400]; memset(longn, 'x', 300); strcpy(longn + 300, ".flac");
    CHECK(rmt_upload_name("A", longn, nm, sizeof nm) == 0 && strlen(nm) < 128 && strstr(nm, ".flac") == nm + strlen(nm) - 5);
    rmt_cmd_t uc; long long ul = 0; size_t uh = 0;
    const char *head = "POST /Fest_1/api/upload?g=abcdefgh12&n=Anna&f=My+Song.mp3 HTTP/1.1\r\nHost: x\r\ncontent-length: 123456789\r\n\r\nID3";
    CHECK(rmt_upload_head(head, strlen(head), "Fest_1", &uc, &ul, &uh) == 1 && ul == 123456789 && uh == strlen(head) - 3
          && uc.kind == RMT_CMD_PARTY_UPLOAD && uc.guest && !strcmp(uc.gid, "abcdefgh12") && !strcmp(uc.gname, "Anna") && !strcmp(uc.name, "My Song.mp3"));
    CHECK(rmt_upload_head("POST /Fest_1/api/upload?g=abcdefgh12&f=a.mp3 HTTP/1.1\r\nHost: x", 40, "Fest_1", &uc, &ul, &uh) == 0);   /* not all there yet */
    CHECK(rmt_upload_head("POST /Fest_2/api/upload?g=abcdefgh12&f=a.mp3 HTTP/1.1\r\nContent-Length: 5\r\n\r\n", 70, "Fest_1", &uc, &ul, &uh) == 0);
    CHECK(rmt_upload_head("POST /Fest_1/api/queue?g=abcdefgh12 HTTP/1.1\r\nContent-Length: 5\r\n\r\n", 60, "Fest_1", &uc, &ul, &uh) == 0);
    CHECK(rmt_upload_head("POST /Fest_1/api/upload?f=a.mp3 HTTP/1.1\r\nContent-Length: 5\r\n\r\n", 60, "Fest_1", &uc, &ul, &uh) == -1);   /* who? */
    CHECK(rmt_upload_head("POST /Fest_1/api/upload?g=abcdefgh12&f=a.mp3 HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 80, "Fest_1", &uc, &ul, &uh) == -1);
    CHECK(rmt_upload_head("POST /Fest_1/api/upload?g=abcdefgh12&f=a.mp3 HTTP/1.1\r\nContent-Length: -5\r\n\r\n", 70, "Fest_1", &uc, &ul, &uh) == -1);
    CHECK(rmt_upload_head("POST /Fest_1/api/upload?g=abcdefgh12 HTTP/1.1\r\nContent-Length: 5\r\n\r\n", 60, "Fest_1", &uc, &ul, &uh) == -1);   /* no file name */
    /* the favourite symbols and colours to choose from (look.conf) */
    rmt_look_t lk;
    CHECK(rmt_look_parse(RMT_LOOK_DEFAULT, &lk) == 0 && lk.nsym == 5 && !strcmp(lk.sym[0], "\xe2\x9d\xa4\xef\xb8\x8f") && !strcmp(lk.sym[4], "\xf0\x90\x95\xa3")
          && lk.ncol == 9 && !strcmp(lk.col[0].name, "Red") && !strcmp(lk.col[0].light, "#d61f3b") && !strcmp(lk.col[8].name, "Black") && !strcmp(lk.col[8].light, "#000000"));
    CHECK(rmt_look_parse("# x\nsymbol=*\n\ncolor=Dark Red #800000\r\n", &lk) == 0 && lk.nsym == 1 && !strcmp(lk.col[0].name, "Dark Red") && !strcmp(lk.col[0].dark, "#800000"));
    CHECK(rmt_look_parse("symbol=*\ncolor=Red #12345\n", &lk) == 2 && rmt_look_parse("symbol=a b\ncolor=R #123456\n", &lk) == 1);
    CHECK(rmt_look_parse("symbol=*\ncolor=R #123456\ncolor=R #654321\n", &lk) == 3 && rmt_look_parse("symbol=*\n", &lk) == 2);   /* names pick them; no colour */
    CHECK(rmt_look_parse("symbol=<b>\ncolor=R #123456\n", &lk) == 1 && rmt_look_parse("colour=R #123456\n", &lk) == 1 && rmt_look_parse("symbol=*\ncolor=#123456\n", &lk) == 2);
    rmt_buf_t lb = {0}; rmt_look_parse("symbol=*\ncolor=R #123456 #abcdef\n", &lk); rmt_look_json(&lb, &lk); char *lj = rmt_buf_take(&lb);
    CHECK(lj && !strcmp(lj, "\"symbols\":[\"*\"],\"colors\":[{\"name\":\"R\",\"dark\":\"#123456\",\"light\":\"#abcdef\"}],\"schemes\":[]")); free(lj);
    /* page colour schemes: name, then frame, page, text, muted, link */
    CHECK(rmt_look_parse(RMT_LOOK_DEFAULT, &lk) == 0 && lk.nsch == 1 && !strcmp(lk.sch[0].name, "Pink and black") && !strcmp(lk.sch[0].c[0], "#dc007c") && !strcmp(lk.sch[0].c[4], "#3fa3e8"));
    CHECK(rmt_look_parse("symbol=*\ncolor=R #123456\nscheme=S #111111 #222222 #333333 #444444\n", &lk) == 3);          /* one missing */
    CHECK(rmt_look_parse("symbol=*\ncolor=R #123456\nscheme=S #111111 #222222 #333333 #444444 #555555 #666666\n", &lk) == 3);
    CHECK(rmt_look_parse("symbol=*\ncolor=R #123456\nscheme=#111111 #222222 #333333 #444444 #555555\n", &lk) == 3);   /* no name */
    CHECK(rmt_look_parse("symbol=*\ncolor=R #123456\nscheme=S #111111 #222222 #333333 #444444 #55555g\n", &lk) == 3);
    lb = (rmt_buf_t){0}; CHECK(rmt_look_parse("symbol=*\ncolor=R #123456\nscheme=S x #111111 #222222 #333333 #444444 #555555\n", &lk) == 0); rmt_look_json(&lb, &lk); lj = rmt_buf_take(&lb);
    CHECK(lj && strstr(lj, ",\"schemes\":[{\"name\":\"S x\",\"frame\":\"#111111\",\"page\":\"#222222\",\"text\":\"#333333\",\"muted\":\"#444444\",\"link\":\"#555555\"}]")); free(lj);
    r = req("POST", p("/api/look"), "l=symbol%3D%E2%AD%90&l=&l=color%3DBlack+%23000000");
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_LOOK && r.cmd.arg == 1 && !strcmp(r.cmd.text, "symbol=\xe2\xad\x90\n\ncolor=Black #000000\n"));
    r = req("POST", p("/api/look"), "l=symbol%3D*&l=color%3DR+%23123456&l=scheme%3DS+%23111111+%23222222+%23333333+%23444444+%23555555&l=scheme%3DS+%23111111+%23222222+%23333333+%23444444+%23555555");
    CHECK(r.kind == RMT_R_BAD_PARAM && r.cmd.kind == RMT_CMD_LOOK && !strcmp(r.cmd.text, "Not saved: line 4: this scheme name is used twice"));
    r = req("POST", p("/api/look"), "l=symbol%3D*&l=%23+x");
    CHECK(r.kind == RMT_R_BAD_PARAM && !strcmp(r.cmd.text, "Not saved: no color= line"));
    CHECK(rmt_look_parse("symbol=*\ncolor=R #12345\n", &lk) == 2 && lk.why && !strcmp(lk.why, "one or two colours as #rrggbb"));
    CHECK(req("POST", p("/api/look"), "l=symbol%3D%E2%AD%90").kind == RMT_R_BAD_PARAM && req("GET", p("/api/look"), NULL).cmd.arg == 0);
    CHECK(preq("GET", "/Fest_1/api/look", NULL).kind == RMT_R_CMD && preq("POST", "/Fest_1/api/look", "l=symbol%3D*&l=color%3DR+%23123456").kind == RMT_R_NOT_FOUND);   /* guests choose, the host decides */
    /* the host's settings */
    r = req("POST", p("/api/party"), "on=1&name=Fest_1&maxvol=80&total=5");
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_PARTY && r.cmd.arg == 1 && r.cmd.party[RMT_P_ON] == 1 && r.cmd.party[RMT_P_MAXVOL] == 80
          && r.cmd.party[RMT_P_TOTAL] == 5 && r.cmd.party[RMT_P_SKIP] == -1 && !strcmp(r.cmd.name, "Fest_1"));
    CHECK(req("POST", p("/api/party"), "maxvol=0").kind == RMT_R_BAD_PARAM && req("POST", p("/api/party"), "name=a").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/party"), "").kind == RMT_R_BAD_PARAM && req("GET", p("/api/party"), NULL).cmd.arg == 0);
}
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
    CHECK(req("GET", p("/system"), NULL).kind == RMT_R_SYS_PAGE && req("POST", p("/system"), "").kind == RMT_R_BAD_METHOD);
    CHECK(req("GET", p("/system/"), NULL).kind == RMT_R_NOT_FOUND);
    CHECK(req("POST", p("/"), "").kind == RMT_R_BAD_METHOD);
    CHECK(req("GET", p("/api/state"), NULL).kind == RMT_R_STATE);
    CHECK(req("GET", p("/api/state?x=1"), NULL).kind == RMT_R_STATE);
    CHECK(req("POST", p("/api/state"), "").kind == RMT_R_BAD_METHOD);
    CHECK(req("GET", p("/api/next"), NULL).kind == RMT_R_BAD_METHOD);   /* commands are POST only */
    CHECK(req("POST", p("/api/nope"), "").kind == RMT_R_NOT_FOUND);
    CHECK(req("POST", p("/../api/next"), "").kind == RMT_R_NOT_FOUND);
    rmt_route_t r = req("POST", p("/api/next"), "");
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_NEXT);
    /* the Screen tab: a picture (GET) and a finger (POST: x,y down; x2,y2 up, default x,y; ms down, default 100) */
    r = req("GET", p("/api/screen?t=1"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_SCREEN);
    CHECK(req("POST", p("/api/screen"), "").kind == RMT_R_BAD_METHOD);
    CHECK(req("GET", p("/api/touch?x=1&y=2"), NULL).kind == RMT_R_BAD_METHOD);
    r = req("POST", p("/api/touch"), "x=10&y=359");
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_TOUCH && r.cmd.pos == 10 && r.cmd.id == 359 && r.cmd.offset == 10 && r.cmd.limit == 359 && r.cmd.arg == 100);
    r = req("POST", p("/api/touch"), "x=0&y=180&x2=300&y2=170&ms=250");
    CHECK(r.kind == RMT_R_CMD && r.cmd.pos == 0 && r.cmd.id == 180 && r.cmd.offset == 300 && r.cmd.limit == 170 && r.cmd.arg == 250);
    CHECK(req("POST", p("/api/touch"), "x=10").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/touch"), "x=360&y=0").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/touch"), "x=-1&y=0").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/touch"), "x=1&y=1&x2=400").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/touch"), "x=1&y=1&ms=3001").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/touch"), "x=1&y=1&ms=abc").kind == RMT_R_BAD_PARAM);
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
    r = req("POST", p("/api/balance"), "value=-10"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_BALANCE && r.cmd.arg == -10);
    r = req("POST", p("/api/balance"), "value=10"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_BALANCE && r.cmd.arg == 10);
    CHECK(req("POST", p("/api/balance"), "value=11").kind == RMT_R_BAD_PARAM && req("POST", p("/api/balance"), "").kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/balance"), NULL).kind == RMT_R_BAD_METHOD && !strcmp(rmt_cmd_name(RMT_CMD_BALANCE), "balance"));
    r = req("POST", p("/api/outputs"), "bt=1"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_OUTPUTS && r.cmd.sub == RMT_OUT_BT && r.cmd.arg == 1);
    r = req("POST", p("/api/outputs"), "vol_aux=120"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_OUT_VOL_AUX && r.cmd.arg == 120);
    r = req("POST", p("/api/outputs?auto=2"), ""); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_OUT_AUTO && r.cmd.arg == 2);
    r = req("POST", p("/api/outputs"), "aux=0"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_OUT_AUX && r.cmd.arg == 0);
    CHECK(req("POST", p("/api/outputs"), "aux=2").kind == RMT_R_BAD_PARAM && req("POST", p("/api/outputs"), "vol_bt=121").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/outputs"), "aux=1&bt=1").kind == RMT_R_BAD_PARAM && req("POST", p("/api/outputs"), "").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/outputs"), "dig=1"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_OUT_DIG && r.cmd.arg == 1);
    r = req("POST", p("/api/outputs"), "vol_dig=120"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_OUT_VOL_DIG && r.cmd.arg == 120);
    r = req("POST", p("/api/outputs"), "dig=2"); CHECK(r.kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/outputs"), "usb=1"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_OUT_USB && r.cmd.arg == 1);
    r = req("POST", p("/api/outputs"), "vol_usb=7"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_OUT_VOL_USB && r.cmd.arg == 7);
    r = req("POST", p("/api/outputs"), "cap_usb=60"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_OUT_CAP_USB && r.cmd.arg == 60);
    r = req("POST", p("/api/outputs"), "fade_bt=0"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_OUT_FADE_BT && r.cmd.arg == 0);
    CHECK(req("POST", p("/api/outputs"), "cap_aux=121").kind == RMT_R_BAD_PARAM && req("POST", p("/api/outputs"), "fade_usb=2").kind == RMT_R_BAD_PARAM);
    CHECK(RMT_OUT_USB == 9 && RMT_OUT_VOL_USB == 10 && RMT_OUT_CAP_AUX == 11 && RMT_OUT_FADE_USB == 18);   /* settings_outputs_cmd numbers */
    r = req("POST", p("/api/outputs"), "codec=4"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_OUT_CODEC && r.cmd.arg == 4);
    CHECK(req("POST", p("/api/outputs"), "codec=5").kind == RMT_R_BAD_PARAM);
    r = req("GET", p("/api/queue"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_QUEUE && r.cmd.sub == RMT_Q_LIST && r.cmd.limit == 100);
    r = req("GET", p("/api/queue?limit=500"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.limit == 500);
    CHECK(req("GET", p("/api/queue?limit=501"), NULL).kind == RMT_R_BAD_PARAM && req("GET", p("/api/queue?limit=x"), NULL).kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/queue"), "action=jump&ver=7&kind=queue&i=3"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_Q_JUMP && r.cmd.item == 1 && r.cmd.pos == 3 && r.cmd.arg == 7);
    r = req("POST", p("/api/queue"), "action=jump&ver=7&kind=next&b=812"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_Q_JUMP && r.cmd.item == 2 && r.cmd.id == 812);
    r = req("POST", p("/api/queue"), "action=move&ver=2&i=4&to=0"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_Q_MOVE && r.cmd.pos == 4 && r.cmd.offset == 0);
    r = req("POST", p("/api/queue"), "action=remove&ver=2&i=1"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_Q_REMOVE && r.cmd.pos == 1);
    r = req("POST", p("/api/queue"), "action=clear&ver=2"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_Q_CLEAR);
    r = req("POST", p("/api/queue"), "action=add&kind=song&song_id=9"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_Q_ADD && r.cmd.item == RMT_I_SONG && r.cmd.song_id == 9);
    r = req("POST", p("/api/queue"), "action=add&kind=next&b=812"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_Q_ADD && r.cmd.item == -1 && r.cmd.id == 812);
    CHECK(req("POST", p("/api/queue"), "action=add&kind=next").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/queue"), "action=add&kind=album&name=Blue%20Album"); CHECK(r.kind == RMT_R_CMD && r.cmd.item == RMT_I_ALBUM && !strcmp(r.cmd.name, "Blue Album"));
    CHECK(req("POST", p("/api/queue"), "action=remove&i=1").kind == RMT_R_BAD_PARAM);             /* no ver */
    CHECK(req("POST", p("/api/queue"), "action=jump&ver=1&kind=next").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/queue"), "action=move&ver=1&i=1").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/queue"), "action=add&kind=song").kind == RMT_R_BAD_PARAM && req("POST", p("/api/queue"), "action=nope&ver=1").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/list_end"), "mode=1"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_LIST_END && r.cmd.arg == 1 && r.cmd.pos == 0);
    r = req("POST", p("/api/list_end"), "mode=2&pos=3&url=http%3A%2F%2Fs.example%2Fa"); CHECK(r.kind == RMT_R_CMD && r.cmd.arg == 2 && r.cmd.pos == 3 && !strcmp(r.cmd.name, "http://s.example/a"));
    CHECK(req("POST", p("/api/list_end"), "mode=4").kind == RMT_R_BAD_PARAM && req("POST", p("/api/list_end"), "mode=2&pos=3").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/list_end"), "mode=3"); CHECK(r.kind == RMT_R_CMD && r.cmd.arg == 3 && !r.cmd.item);
    r = req("POST", p("/api/list_end"), "mode=3&pl=7&flags=3"); CHECK(r.kind == RMT_R_CMD && r.cmd.arg == 3 && r.cmd.item == 1 && r.cmd.id == 7 && r.cmd.offset == 3);
    CHECK(req("POST", p("/api/list_end"), "mode=3&pl=7").kind == RMT_R_BAD_PARAM && req("POST", p("/api/list_end"), "mode=2&pl=7&flags=0").kind == RMT_R_BAD_PARAM
          && req("POST", p("/api/list_end"), "mode=3&pl=0&flags=0").kind == RMT_R_BAD_PARAM && req("POST", p("/api/list_end"), "mode=3&pl=1&flags=4").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/list_end"), "mode=2&pos=0&url=x").kind == RMT_R_BAD_PARAM && req("GET", p("/api/list_end"), NULL).kind == RMT_R_BAD_METHOD);
    r = req("GET", p("/api/state?q=1"), NULL); CHECK(r.kind == RMT_R_STATE && r.cmd.arg == 1);
    r = req("GET", p("/api/state"), NULL); CHECK(r.kind == RMT_R_STATE && r.cmd.arg == 0);
    CHECK(req("POST", p("/api/outputs"), "auto=x").kind == RMT_R_BAD_PARAM && req("GET", p("/api/outputs"), NULL).kind == RMT_R_BAD_METHOD);
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
    r = req("POST", p("/api/play_item"), "kind=folder&path=Music%2FJazz"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_I_FOLDER && !strcmp(r.cmd.name, "Music/Jazz"));
    r = req("POST", p("/api/play_item"), "kind=folder"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_I_FOLDER && !r.cmd.name[0]);   /* the whole card */
    CHECK(req("POST", p("/api/play_item"), "kind=folder&path=Music%2F..%2F..").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/play_item"), "kind=folder&path=%2Fetc").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/radio_fav_add"), "kind=radio_now"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_RADIO_FAV && r.cmd.sub == RMT_I_RADIO_NOW && r.cmd.arg == 1);
    r = req("POST", p("/api/radio_fav_remove"), "kind=radio_now"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_I_RADIO_NOW && r.cmd.arg == 0);
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
    r = req("POST", p("/api/rescan"), "action=full"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_RESCAN && r.cmd.arg == 2);
    CHECK(req("POST", p("/api/rescan"), "").kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/rescan"), NULL).kind == RMT_R_BAD_METHOD);
    r = req("GET", p("/api/bluetooth"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_BT_LIST);
    r = req("POST", p("/api/bluetooth"), "action=off"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_BT && r.cmd.sub == RMT_BT_OFF);
    r = req("POST", p("/api/bluetooth"), "action=search"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BT_SEARCH);
    CHECK(req("POST", p("/api/bluetooth"), "action=connect&mac=00:11:22:33:44:55").kind == RMT_R_BAD_PARAM);   /* a device action */
    CHECK(req("POST", p("/api/bluetooth"), "action=dance").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/bluetooth"), "action=codec&codec=4"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BT_CODEC && r.cmd.arg == 4);
    CHECK(req("POST", p("/api/bluetooth"), "action=codec").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/bluetooth"), "action=codec&codec=10").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/bt_device"), "action=codec&codec=1&mac=00:11:22:33:44:55").kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/bt_device"), NULL).kind == RMT_R_BAD_METHOD);
    r = req("POST", p("/api/work_mode"), "mode=3"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_WORK_MODE && r.cmd.arg == 3);
    CHECK(req("POST", p("/api/work_mode"), "mode=6").kind == RMT_R_BAD_PARAM);         /* web radio starts from a station */
    CHECK(req("GET", p("/api/work_mode"), NULL).kind == RMT_R_BAD_METHOD);
    r = req("POST", p("/api/reboot"), "confirm=1"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_REBOOT);
    CHECK(req("POST", p("/api/reboot"), "").kind == RMT_R_BAD_PARAM);
    CHECK(req("GET", p("/api/reboot"), NULL).kind == RMT_R_BAD_METHOD);
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
    /* Bandcamp: GET = everything as JSON; POST action=search|open|play|stop */
    r = req("GET", p("/api/bandcamp"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_BANDCAMP && r.cmd.sub == RMT_BC_STATE);
    r = req("POST", p("/api/bandcamp"), "action=search&q=aphex+twin"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_SEARCH && !strcmp(r.cmd.q, "aphex twin"));
    CHECK(req("POST", p("/api/bandcamp"), "action=search&q=").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/bandcamp"), "action=open&url=https%3A%2F%2Faphextwin.bandcamp.com");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_OPEN && !strcmp(r.cmd.name, "https://aphextwin.bandcamp.com"));
    CHECK(req("POST", p("/api/bandcamp"), "action=open&url=http%3A%2F%2Fx").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/bandcamp"), "action=play&url=https%3A%2F%2Fa.bandcamp.com%2Falbum%2Fx&track=3");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_PLAY && r.cmd.pos == 3 && !strcmp(r.cmd.name, "https://a.bandcamp.com/album/x"));
    r = req("POST", p("/api/bandcamp"), "action=play&url=https%3A%2F%2Fa.bandcamp.com%2Falbum%2Fx"); CHECK(r.kind == RMT_R_CMD && r.cmd.pos == -1);
    CHECK(req("POST", p("/api/bandcamp"), "action=play&url=https%3A%2F%2Fa.bandcamp.com%2Falbum%2Fx&track=-2").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/bandcamp"), "action=download&url=https%3A%2F%2Fa.bandcamp.com%2Falbum%2Fx"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_DOWNLOAD && r.cmd.pos == -1);
    r = req("POST", p("/api/bandcamp"), "action=download&url=https%3A%2F%2Fa.bandcamp.com%2Ftrack%2Fy&track=0"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_DOWNLOAD && r.cmd.pos == 0);
    r = req("POST", p("/api/bandcamp"), "action=stop_downloads"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_DL_STOP);
    r = req("POST", p("/api/bandcamp"), "action=quality&q=2"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_QUALITY && r.cmd.pos == 2);
    CHECK(req("POST", p("/api/bandcamp"), "action=quality&q=4").kind == RMT_R_BAD_PARAM);
    r = req("GET", p("/api/bandcamp?favs=1"), NULL); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_FAVS);
    r = req("POST", p("/api/bandcamp"), "action=fav_add&type=t&name=Soko+Linx&artist=UP&url=https%3A%2F%2Fa.bandcamp.com%2Ftrack%2Fs");
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_FAV_ADD && !strcmp(r.cmd.text, "t\tSoko Linx\tUP\t\thttps://a.bandcamp.com/track/s\t"));
    CHECK(req("POST", p("/api/bandcamp"), "action=fav_add&type=q&name=x&url=https%3A%2F%2Fa.bandcamp.com").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/bandcamp"), "action=fav_add&type=a&url=https%3A%2F%2Fa.bandcamp.com").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/bandcamp"), "action=fav_remove&url=https%3A%2F%2Fa.bandcamp.com"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_FAV_REMOVE);
    CHECK(req("POST", p("/api/bandcamp"), "action=play").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/bandcamp"), "action=stop"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_BC_STOP);
    CHECK(req("POST", p("/api/bandcamp"), "action=dance").kind == RMT_R_BAD_PARAM && req("PUT", p("/api/bandcamp"), NULL).kind == RMT_R_BAD_METHOD);
    CHECK(preq("POST", "/Fest_1/api/bandcamp", "action=stop").kind == RMT_R_NOT_FOUND);   /* not for party guests */
    r = req("POST", p("/api/play_item"), "kind=radio_hit&pos=3&url=http%3A%2F%2Fx"); CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_I_RADIO_HIT && r.cmd.pos == 3);
    CHECK(req("POST", p("/api/play_item"), "kind=radio_hit&pos=3").kind == RMT_R_BAD_PARAM);   /* hits need their url */
    r = req("POST", p("/api/radio_fav_add"), "kind=radio_hit&pos=1&url=http%3A%2F%2Fx");
    CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_RADIO_FAV && r.cmd.arg == 1 && r.cmd.sub == RMT_I_RADIO_HIT);
    r = req("POST", p("/api/radio_fav_remove"), "kind=radio&pos=2&url=http%3A%2F%2Fx"); CHECK(r.kind == RMT_R_CMD && r.cmd.kind == RMT_CMD_RADIO_FAV && r.cmd.arg == 0);
    CHECK(req("POST", p("/api/radio_fav_remove"), "kind=radio&pos=2").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/radio_fav_add"), "kind=song&song_id=2&url=x").kind == RMT_R_BAD_PARAM);
    r = req("POST", p("/api/radio_fav_add"), "kind=stream&name=Mine&url=https%3A%2F%2Fm.example%2Fa");   /* a stream address of your own */
    CHECK(r.kind == RMT_R_CMD && r.cmd.sub == RMT_I_STREAM && r.cmd.arg == 1 && !strcmp(r.cmd.q, "Mine") && !strcmp(r.cmd.name, "https://m.example/a"));
    CHECK(req("POST", p("/api/radio_fav_add"), "kind=stream&name=Mine&url=ftp%3A%2F%2Fm").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/radio_fav_add"), "kind=stream&url=http%3A%2F%2Fm").kind == RMT_R_BAD_PARAM);
    CHECK(req("POST", p("/api/radio_fav_remove"), "kind=stream&name=Mine&url=http%3A%2F%2Fm").kind == RMT_R_BAD_PARAM);
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
    rmt_state_t s; memset(&s, 0, sizeof s); s.end_mode = -1;
    snprintf(s.title, sizeof s.title, "Song \"1\""); snprintf(s.artist, sizeof s.artist, "Art");
    s.has_track = 1; s.playing = 1; s.position_ms = 1000; s.duration_ms = 2000; s.volume = 40; s.volume_max = 120;
    s.play_mode = 1; s.controllable = 1; s.source = "local";
    char js[14000]; CHECK(rmt_state_json(js, sizeof js, &s) > 0);
    CHECK(strstr(js, "\"radio_state\":\"off\"") && strstr(js, "\"station\":\"\""));
    CHECK(strstr(js, "\"title\":\"Song \\\"1\\\"\"") != NULL);
    CHECK(strstr(js, "\"playing\":true") && strstr(js, "\"volume\":40") && strstr(js, "\"balance\":0,") && strstr(js, "\"outputs\":null,\"list_end\":null}") && strstr(js, "\"source\":\"local\""));
    s.out_own = 1; s.out_codec = -1; s.out_codecs = 1 | 4 | 16; s.out_aux = 1; s.out_vol_aux = 30; s.out_vol_bt = 12; s.out_auto = 2; s.out_jack = 1; s.out_bal = 0;
    s.out_dig = 1; s.out_vol_dig = 77;
    s.out_usb = 1; s.out_vol_usb = 40; s.out_usb_ready = 1; s.out_main = 50; snprintf(s.out_usb_name, sizeof s.out_usb_name, "USB \"DAC\"");
    for(int i = 0; i < 4; i++){ s.out_cap[i] = 120 - i; s.out_fade[i] = i != 2; }
    CHECK(rmt_state_json(js, sizeof js, &s) > 0 && strstr(js, "\"outputs\":{\"aux\":true,\"bt\":false,\"dig\":true,\"vol_aux\":30,\"vol_bt\":12,\"vol_dig\":77,\"bt_ready\":false,\"auto\":2,\"jack\":1,\"bal\":0,\"codec\":-1,\"codecs\":[0,2,4],\"switching\":false,"
          "\"usb\":true,\"vol_usb\":40,\"usb_ready\":true,\"usb_name\":\"USB \\\"DAC\\\"\",\"main\":50,\"cap\":[120,119,118,117],\"fade\":[1,1,0,1]},\"list_end\":null}"));
    s.out_own = 0;
    s.source = "radio"; s.radio_state = "playing"; snprintf(s.station, sizeof s.station, "Radio <%s>", "X");
    snprintf(s.station_url, sizeof s.station_url, "https://radio.example/stream?a=1&b=\"2\"");
    CHECK(rmt_state_json(js, sizeof js, &s) > 0 && strstr(js, "\"source\":\"radio\"") && strstr(js, "\"station\":\"Radio \\u003cX>\""));
    CHECK(strstr(js, "\"station_url\":\"https://radio.example/stream?a=1\\u0026b=\\\"2\\\"\"") || strstr(js, "\"station_url\":\"https://radio.example/stream?a=1&b=\\\"2\\\"\""));
    memset(s.title, 1, sizeof s.title - 1); s.title[sizeof s.title - 1] = 0;   /* worst-case escaping still fits */
    memset(s.album, 1, sizeof s.album - 1); s.album[sizeof s.album - 1] = 0; memset(s.artist, 1, sizeof s.artist - 1); s.artist[sizeof s.artist - 1] = 0;
    memset(s.station, 1, sizeof s.station - 1); s.station[sizeof s.station - 1] = 0; memset(s.radio_error, 1, sizeof s.radio_error - 1); s.radio_error[sizeof s.radio_error - 1] = 0; memset(s.station_url, 1, sizeof s.station_url - 1); s.station_url[sizeof s.station_url - 1] = 0; memset(s.stream_title, 1, sizeof s.stream_title - 1); s.stream_title[sizeof s.stream_title - 1] = 0; memset(s.bt_codec, 1, sizeof s.bt_codec - 1); s.bt_codec[sizeof s.bt_codec - 1] = 0; memset(s.bt_device, 1, sizeof s.bt_device - 1); s.bt_device[sizeof s.bt_device - 1] = 0;
    CHECK(rmt_state_json(js, sizeof js, &s) > 0);
    CHECK(strlen(RMT_PAGE_HTML) < 43008 && strlen(RMT_SYS_HTML) < 32768 && strlen(RMT_PARTY_HTML) < 32768 && strlen(RMT_HG_HTML) < 32768 && strlen(RMT_HGADMIN_HTML) < 32768);   /* each page stays light (the main page: 40 KB since the Bandcamp tab, 42 KB since USB audio) */
}

/* ---------------------------------------------------------------- server round trip */
static volatile int g_emu_run = 1, g_emu_pause = 0;
static int g_scan, g_work, g_reboots;
static time_t g_search_until; static char g_search_q[96]; static int g_nfav = 3;   /* stand-in radio directory */
static rmt_cmd_t g_last; static int g_ncmd;
/* stand-in queue: 4 queued songs, 30 Up Next rows; g_qver moves on every change */
static char g_q[8][32] = { "Queued one", "Queued <two>", "Queued three", "Queued four" }; static int g_qn = 4; static unsigned g_qver = 1;
static char *fake_queue(const rmt_cmd_t *c, const char **why){
    if(c->sub == RMT_Q_ADD){ if(g_qn < 8){ if(c->item == -1) snprintf(g_q[g_qn++], 32, "Next %ld", c->id - 5); else snprintf(g_q[g_qn++], 32, "Added %ld", c->song_id); g_qver++; } return strdup("{\"ok\":true,\"added\":1}"); }
    if(c->sub != RMT_Q_LIST && (unsigned)c->arg != g_qver){ *why = "The queue changed - here it is again"; return NULL; }
    if(c->sub == RMT_Q_REMOVE && c->pos < g_qn){ memmove(g_q[c->pos], g_q[c->pos + 1], (size_t)(g_qn - c->pos - 1) * 32); g_qn--; g_qver++; }
    if(c->sub == RMT_Q_MOVE && c->pos < g_qn && c->offset < g_qn){ char t[32]; memcpy(t, g_q[c->pos], 32);
        if(c->pos < c->offset) memmove(g_q[c->pos], g_q[c->pos + 1], (size_t)(c->offset - c->pos) * 32); else memmove(g_q[c->offset + 1], g_q[c->offset], (size_t)(c->pos - c->offset) * 32);
        memcpy(g_q[c->offset], t, 32); g_qver++; }
    if(c->sub == RMT_Q_CLEAR){ g_qn = 0; g_qver++; }
    if(c->sub == RMT_Q_JUMP){ g_qver++; return strdup("{\"ok\":true}"); }
    if(c->sub != RMT_Q_LIST) return strdup("{\"ok\":true}");
    rmt_buf_t b = {0};
    rmt_buf_printf(&b, "{\"ok\":true,\"ver\":%u,\"shuffle\":false,\"items\":[{\"k\":\"cur\",\"i\":0,\"b\":5,\"d\":200000,\"st\":false,\"t\":\"Track <one>\",\"a\":\"Artist\",\"f\":\"one.flac\"}", g_qver);
    for(int i = 0; i < g_qn; i++){ rmt_buf_printf(&b, ",{\"k\":\"queue\",\"i\":%d,\"b\":0,\"d\":180000,\"st\":false,\"t\":", i); rmt_buf_str(&b, g_q[i]); rmt_buf_put(&b, ",\"a\":\"Someone\",\"f\":\"q.flac\"}"); }
    for(int i = 0; i < 30; i++) rmt_buf_printf(&b, ",{\"k\":\"next\",\"i\":0,\"b\":%d,\"d\":%d,\"st\":false,\"t\":\"Next %d\",\"a\":\"Band\",\"f\":\"n.flac\"}", 6 + i, 150000 + i * 1000, i + 1);
    rmt_buf_printf(&b, "],\"more\":12}");
    return rmt_buf_take(&b);
}
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
static int g_bt_codec = 3; static int g_bt_on = 1, g_bt_conn = 0, g_bt_n = 3; static time_t g_bt_until, g_bt_up;
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
    static const char *const CN[5] = { "SBC", "AAC", "aptX/LDAC Mobile", "aptX/LDAC Standard", "aptX/LDAC High" };
    rmt_buf_printf(&b, "],\"codec\":{\"settable\":true,\"current\":%d,\"live\":\"%s\",\"choices\":[", g_bt_codec, g_bt_on && g_bt_conn >= 0 ? "LDAC" : "");
    for(int i = 0; i < 5; i++){ rmt_buf_printf(&b, "%s{\"id\":%d,\"name\":", i ? "," : "", i); rmt_buf_str(&b, CN[i]); rmt_buf_printf(&b, ",\"hint\":\"Hint for %s <x>\"}", i ? "a choice" : "SBC"); }
    rmt_buf_put(&b, "]}}");
    return rmt_buf_take(&b);
}
static void fake_bt_cmd(const rmt_cmd_t *c){
    int i = 0; while(i < 4 && strcasecmp(BT_MAC[i], c->q)) i++;
    switch(c->sub){
    case RMT_BT_ON: if(!g_bt_on && !g_bt_up) g_bt_up = time(NULL) + 2; break;
    case RMT_BT_OFF: g_bt_on = 0; g_bt_up = 0; g_bt_until = 0; break;
    case RMT_BT_SEARCH: if(!g_bt_on){ rsrv_done(0, "Bluetooth is off"); return; } g_bt_until = time(NULL) + 3; break;
    case RMT_BT_REFRESH: break;
    case RMT_BT_CODEC: if(c->arg > 4){ rsrv_done(0, "Unknown codec"); return; } g_bt_codec = (int)c->arg; break;
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
/* stand-in party: settings, guests' state and queue (the real rules are remote_library_test.py's) */
static int g_pv[RMT_P_N]; static char g_pname[40] = "Fest_1"; static int g_pq;
static void fake_party(const rmt_cmd_t *c, rmt_state_t *s){
    rmt_buf_t b = {0};
    if(c->kind == RMT_CMD_PARTY){
        for(int k = 0; c->arg && k < RMT_P_N; k++) if(c->party[k] >= 0) g_pv[k] = c->party[k];
        if(c->arg && c->name[0]){ snprintf(g_pname, sizeof g_pname, "%.32s", c->name); rsrv_set_party(g_pname); }
        if(c->arg){ char d[256]; snprintf(d, sizeof d, "/tmp/remote_test_card/" RMT_UPLOAD_DIR "/%s", g_pname);
                    rsrv_set_upload(g_pv[RMT_P_UPLOAD] ? d : "", (long long)g_pv[RMT_P_UPMAX] * 1024 * 1024); }
        rmt_buf_put(&b, "{\"ok\":true,\"settings\":{");
        for(int k = 0; k < RMT_P_N; k++) rmt_buf_printf(&b, "\"%s\":%d,", RMT_PARTY_KEYS[k].key, g_pv[k]);
        rmt_buf_put(&b, "\"own_player\":true,\"name\":"); rmt_buf_str(&b, g_pname);
        rmt_buf_put(&b, "},\"guests\":[{\"name\":\"Anna <A>\",\"queued\":2,\"seen_s\":4},{\"name\":\"\",\"queued\":0,\"seen_s\":300}]}");
    } else if(c->kind == RMT_CMD_PARTY_STATE){
        rmt_buf_printf(&b, "{\"ok\":true,\"party\":{\"np\":%d,\"queue\":%d,\"vol\":%d,\"skip\":%d,\"radio\":%d,\"names\":%d,\"total\":%d,\"consec\":%d,\"repeat\":%d,\"mine\":%d,\"left\":%d,\"radio_end\":%d,\"radio_now\":%d,\"upload\":%d,\"upmax\":%d,\"you\":",
                       g_pv[RMT_P_NP], g_pv[RMT_P_QUEUE], g_pv[RMT_P_VOL], g_pv[RMT_P_SKIP], g_pv[RMT_P_RADIO], g_pv[RMT_P_NAMES], g_pv[RMT_P_TOTAL], g_pv[RMT_P_CONSEC],
                       g_pv[RMT_P_REPEAT], g_pq, g_pv[RMT_P_TOTAL] > g_pq ? g_pv[RMT_P_TOTAL] - g_pq : 0, g_pv[RMT_P_RADIO], g_pv[RMT_P_RADIO] && g_pv[RMT_P_SKIP], g_pv[RMT_P_UPLOAD], g_pv[RMT_P_UPMAX]);
        rmt_buf_str(&b, c->gname);
        rmt_buf_printf(&b, "},\"playing\":true,\"has_track\":true,\"controllable\":true,\"source\":\"local\",\"volume\":%d,\"title\":", s->volume * 100 / 120);
        rmt_buf_str(&b, s->title); rmt_buf_put(&b, ",\"artist\":\"Artist\",\"album\":\"Album\",\"station\":\"\",\"stream_title\":\"\",\"art\":\"a1\",\"position_ms\":61000,\"duration_ms\":200000,\"by\":\"Anna <A>\",\"by_you\":false");
        rmt_buf_printf(&b, ",\"queue_ver\":%d}", g_pq + 1);
    } else if(c->kind == RMT_CMD_QUEUE && c->sub == RMT_Q_ADD){
        if(g_pq >= g_pv[RMT_P_TOTAL]){ rsrv_done(0, "Your songs are all in the queue - you can add another once one plays"); return; }
        g_pq++; rmt_buf_printf(&b, "{\"ok\":true,\"added\":1,\"place\":%d,\"left\":%d}", g_pq * 2, g_pv[RMT_P_TOTAL] - g_pq);
    } else if(c->kind == RMT_CMD_QUEUE){
        rmt_buf_put(&b, "{\"ok\":true,\"ver\":1,\"items\":[{\"k\":\"cur\",\"d\":200000,\"mine\":false,\"t\":\"Track <one>\",\"a\":\"Artist\",\"by\":\"Anna <A>\"}");
        for(int i = 0; i < 4 + g_pq; i++) rmt_buf_printf(&b, ",{\"k\":\"queue\",\"d\":180000,\"mine\":%s,\"t\":\"Song %d\",\"a\":\"Band\",\"by\":\"%s\"}",
                                                         i >= 4 ? "true" : "false", i + 1, i >= 4 ? c->gname : i % 2 ? "" : "Ben");
        rmt_buf_put(&b, "]}");
    } else if(c->kind == RMT_CMD_PARTY_UPLOAD){
        rmt_buf_put(&b, "{\"ok\":true,\"song_id\":77,\"file\":"); rmt_buf_str(&b, c->name); rmt_buf_put(&b, ",\"title\":\"Uploaded song\"}");
    } else if(c->kind == RMT_CMD_NEXT && !g_pv[RMT_P_SKIP]){ rsrv_done(0, "Skipping is off at this party"); return; }
    else if(c->kind == RMT_CMD_BROWSE){ rsrv_done_body(1, NULL, fake_browse(c)); return; }
    else if(c->kind == RMT_CMD_VOLUME){ s->volume = (int)(c->arg * 120 / 100); rsrv_done(1, NULL); return; }
    else { rsrv_done(1, NULL); return; }
    rsrv_done_body(1, NULL, rmt_buf_take(&b));
}
static int g_touch_n, g_touch_x, g_touch_y;   /* the stand-in screen marks where the last touch lifted */
static void *emu(void *a){
    (void)a;
    rmt_state_t s; memset(&s, 0, sizeof s);
    snprintf(s.title, sizeof s.title, "Track <one>"); s.has_track = 1; s.volume = 33; s.volume_max = 120; s.source = "local"; s.controllable = 1;
    snprintf(s.format, sizeof s.format, "FLAC"); s.bits = 24; s.sample_rate = 96000; s.kbps = 2304; s.channels = 2; s.favorite = 0;
    snprintf(s.art, sizeof s.art, "a1"); s.eq = 1; snprintf(s.eq_name, sizeof s.eq_name, "Jazz"); snprintf(s.bt_codec, sizeof s.bt_codec, "LDAC"); snprintf(s.bt_device, sizeof s.bt_device, "XQ-50 <PRO2>"); s.station_fav = -1;
    s.queue_ver = 1; s.end_mode = 0;
    s.out_own = 1; s.out_aux = 1; s.out_bt = 1; s.out_vol_aux = 40; s.out_vol_bt = 25; s.out_bt_ready = 1; s.out_auto = 1; s.out_jack = 0; s.out_bal = 1; s.out_codec = 2; s.out_codecs = 7;
    while(g_emu_run){
        if(!g_emu_pause){
            rsrv_publish(&s);
            rmt_cmd_t c;
            if(rsrv_take(&c)){
                g_last = c; g_ncmd++;
                if(c.kind == RMT_CMD_LOOK){                       /* the stand-in card: look.conf in RAM */
                    static char lt[3072]; rmt_look_t lk;
                    if(c.arg) snprintf(lt, sizeof lt, "%s", c.text);
                    if(!lt[0] || rmt_look_parse(lt, &lk)) rmt_look_parse(RMT_LOOK_DEFAULT, &lk);
                    rmt_buf_t b = {0}; rmt_buf_put(&b, "{\"ok\":true,"); rmt_look_json(&b, &lk);
                    rmt_buf_put(&b, ",\"on_card\":false,\"bad_line\":0,\"text\":"); rmt_buf_str(&b, lt[0] ? lt : RMT_LOOK_DEFAULT); rmt_buf_put(&b, "}");
                    rsrv_done_body(1, NULL, rmt_buf_take(&b));
                } else
                if((c.guest && c.kind != RMT_CMD_COVER) || c.kind == RMT_CMD_PARTY) fake_party(&c, &s);
                else if(c.kind == RMT_CMD_PAUSE) rsrv_done(0, "Not in local \"playback\"");
                else if(c.kind == RMT_CMD_BROWSE) rsrv_done_body(1, NULL, fake_browse(&c));
                else if(c.kind == RMT_CMD_RESCAN){ g_scan = (int)c.arg; rsrv_done(1, NULL); }
                else if(c.kind == RMT_CMD_BT_LIST) rsrv_done_body(1, NULL, fake_bt());
                else if(c.kind == RMT_CMD_BT) fake_bt_cmd(&c);
                else if(c.kind == RMT_CMD_WORK_MODE){ s.work_mode = (int)c.arg; g_work = (int)c.arg; rsrv_done(1, NULL); }
                else if(c.kind == RMT_CMD_REBOOT){ g_reboots++; rsrv_done(1, NULL); }
                else if(c.kind == RMT_CMD_STATUS){
                    const char *sb = g_scan ? "{\"ok\":true,\"scan\":{\"active\":true,\"done\":12},\"groups\":[{\"name\":\"Playback\",\"rows\":[[\"Library scan\",\"Running, 12 files so far\"]]}]}" : "{\"ok\":true,\"groups\":[{\"name\":\"Wi-Fi\",\"rows\":[[\"Network\",\"<home>\"],[\"Signal\",\"-60 dBm (good)\"]]},"
                                                    "{\"name\":\"Battery\",\"rows\":[[\"Charge\",\"89%\"],[\"Temperature\",\"33.0 \xC2\xB0" "C\"]]},{\"name\":\"Empty\",\"rows\":[]}]}";
                    char *o = malloc(strlen(sb) + 32); sprintf(o, "{\"work_mode\":%d,%s", g_work, sb + 1); rsrv_done_body(1, NULL, o);
                }
                else if(c.kind == RMT_CMD_EQ_LIST){
                    char b[400]; snprintf(b, sizeof b, "{\"ok\":true,\"current\":%d,\"presets\":[{\"id\":0,\"name\":\"Off\"},{\"id\":1,\"name\":\"Jazz\"},{\"id\":2,\"name\":\"R&B <x>\"}]}", s.eq);
                    rsrv_done_body(1, NULL, strdup(b));
                } else if(c.kind == RMT_CMD_EDIT){                       /* stand-in edits: create answers an id */
                    if(c.sub == RMT_E_PL_CREATE) rsrv_done_body(1, NULL, strdup("{\"ok\":true,\"id\":42}"));
                    else if(c.sub == RMT_E_PL_ADD) rsrv_done_body(1, NULL, strdup(c.item == RMT_I_SONG ? "{\"ok\":true,\"added\":1}" : "{\"ok\":true,\"added\":12}"));
                    else if(c.sub == RMT_E_FAV && !c.song_id){ s.favorite = (int)c.arg; rsrv_done(1, NULL); }
                    else rsrv_done(1, NULL);
                } else if(c.kind == RMT_CMD_EQ){ s.eq = (int)c.arg; snprintf(s.eq_name, sizeof s.eq_name, "%s", c.arg == 0 ? "Off" : c.arg == 1 ? "Jazz" : "R&B <x>"); rsrv_done(1, NULL); }
                else if(c.kind == RMT_CMD_SCREEN){                      /* a 360x360 24-bit BMP: a disc on dark grey */
                    size_t row = 360 * 3, n = 54 + row * 360; unsigned char *b = calloc(1, n);
                    memcpy(b, "BM", 2); b[2] = (unsigned char)n; b[3] = (unsigned char)(n >> 8); b[4] = (unsigned char)(n >> 16); b[10] = 54; b[14] = 40;
                    b[18] = 360 & 255; b[19] = 360 >> 8; b[22] = 360 & 255; b[23] = 360 >> 8; b[26] = 1; b[28] = 24;
                    for(int y = 0; y < 360; y++) for(int x = 0; x < 360; x++){
                        unsigned char *q = b + 54 + (size_t)(359 - y) * row + (size_t)x * 3; int dx = x - 180, dy = y - 180, in = dx * dx + dy * dy < 150 * 150;
                        int hit = g_touch_n && abs(x - g_touch_x) < 8 && abs(y - g_touch_y) < 8;
                        q[0] = hit ? 0 : in ? (unsigned char)(x * 255 / 359) : 40; q[1] = hit ? 0 : in ? 120 : 40; q[2] = hit ? 255 : in ? (unsigned char)(y * 255 / 359) : 40;
                    }
                    rsrv_done_blob((char *)b, n, "image/bmp");
                }
                else if(c.kind == RMT_CMD_OUTPUTS){                     /* stand-in Outputs page: AUX analog/digital + headphones */
                    if(c.sub == RMT_OUT_CODEC){ if(!(s.out_codecs & (1 << c.arg))) rsrv_done(0, "These headphones don't offer it"); else { s.out_codec = (int)c.arg; rsrv_done(1, NULL); } }
                    else {
                        if(c.sub == RMT_OUT_AUX) s.out_aux = (int)c.arg;
                        if(c.sub == RMT_OUT_BT) s.out_bt = (int)c.arg;
                        if(c.sub == RMT_OUT_DIG) s.out_dig = (int)c.arg;
                        if(c.sub == RMT_OUT_VOL_AUX) s.out_vol_aux = (int)c.arg;
                        if(c.sub == RMT_OUT_VOL_BT) s.out_vol_bt = (int)c.arg;
                        if(c.sub == RMT_OUT_VOL_DIG) s.out_vol_dig = (int)c.arg;
                        if(c.sub == RMT_OUT_AUTO) s.out_auto = (int)c.arg;
                        rsrv_done(1, NULL);
                    }
                }
                else if(c.kind == RMT_CMD_LIST_END){ if(c.arg == 2 && !c.pos && !s.end_station[0]) rsrv_done(0, "Choose a station first"); else if(c.arg == 3 && !c.item && !s.end_pl_id) rsrv_done(0, "Choose a playlist first");
                    else { s.end_mode = (int)c.arg; if(c.pos) snprintf(s.end_station, sizeof s.end_station, "Station %ld", c.pos);
                           if(c.item){ s.end_pl_id = c.id; s.end_pl_flags = (int)c.offset; snprintf(s.end_playlist, sizeof s.end_playlist, "Playlist %ld", c.id - 1); } rsrv_done(1, NULL); } }
                else if(c.kind == RMT_CMD_QUEUE){ const char *w = NULL; char *o = fake_queue(&c, &w); s.queue_ver = g_qver; rsrv_done_body(o != NULL, w, o); }
                else if(c.kind == RMT_CMD_BALANCE){ s.balance = (int)c.arg; rsrv_done(1, NULL); }
                else if(c.kind == RMT_CMD_TOUCH){ g_touch_n++; g_touch_x = (int)c.offset; g_touch_y = (int)c.limit; rsrv_done(1, NULL); }
                else if(c.kind == RMT_CMD_COVER){                       /* a 16x16 24-bit BMP, a colour gradient */
                    size_t n = 54 + 16 * 16 * 3; unsigned char *b = calloc(1, n);
                    memcpy(b, "BM", 2); b[2] = (unsigned char)n; b[3] = (unsigned char)(n >> 8); b[10] = 54; b[14] = 40; b[18] = 16; b[22] = 16; b[26] = 1; b[28] = 24;
                    for(int i = 0; i < 256; i++){ b[54 + i * 3] = (unsigned char)(i); b[55 + i * 3] = 80; b[56 + i * 3] = (unsigned char)(255 - i); }
                    rsrv_done_blob((char *)b, n, "image/bmp");
                }
                else if(c.kind == RMT_CMD_PLAY_ITEM && c.sub == RMT_I_RADIO){        /* stand-in radio: it plays at once */
                    s.source = "radio"; s.radio_state = "playing"; s.station_fav = 0; snprintf(s.station, sizeof s.station, "Station %ld", c.pos);
                    snprintf(s.title, sizeof s.title, "Station %ld", c.pos);
                    snprintf(s.stream_title, sizeof s.stream_title, "Rude Norton - Tits On The Beach (1980) <x>");
                    snprintf(s.station_url, sizeof s.station_url, "%s", c.name[0] ? c.name : "https://radio.example/a-rather-long/path/to/the/stream.mp3?listener=remote"); rsrv_done(1, NULL);
                } else if(c.kind == RMT_CMD_RADIO_SEARCH){              /* "answers" two seconds later */
                    snprintf(g_search_q, sizeof g_search_q, "%s", c.q[0] ? c.q : "*"); g_search_until = time(NULL) + 2; rsrv_done(1, NULL);
                } else if(c.kind == RMT_CMD_RADIO_FAV && c.sub == RMT_I_RADIO_NOW){
                    if(s.station_fav < 0) rsrv_done(0, "No station is playing"); else { s.station_fav = (int)c.arg; rsrv_done(1, NULL); }
                } else if(c.kind == RMT_CMD_RADIO_FAV){
                    if(!c.arg && c.sub == RMT_I_RADIO && g_nfav > 0) g_nfav--;
                    rsrv_done(1, NULL);
                } else if(c.kind == RMT_CMD_RADIO_STOP){
                    s.source = "local"; s.radio_state = "off"; s.station[0] = 0; s.station_url[0] = 0; s.stream_title[0] = 0; s.station_fav = -1; snprintf(s.title, sizeof s.title, "Track <one>"); rsrv_done(1, NULL);
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
    CHECK(call("GET", p("/system"), NULL, out, sizeof out) == 200 && strstr(out, "<title>diskOS System</title>") && strstr(out, "Content-Security-Policy:"));
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
    CHECK(call("GET", p("/api/screen?t=5"), NULL, out, sizeof out) == 200 && strstr(out, "Content-Type: image/bmp") && strstr(out, "Content-Length: 388854"));
    CHECK(call("POST", p("/api/touch"), "x=20&y=30", out, sizeof out) == 200 && g_touch_n == 1 && g_touch_x == 20 && g_touch_y == 30);
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
    /* a guest's upload: the body goes to the card on its own thread, then the main thread hears of it */
    {
        CHECK(system("rm -rf /tmp/remote_test_card && mkdir /tmp/remote_test_card") == 0);   /* the card's root (a mount: never made here) */
        rsrv_set_party("Fest_1");
        size_t bl = 200000; char *raw = malloc(bl + 512); int hl;
        hl = snprintf(raw, 512, "POST /Fest_1/api/upload?g=abcdefgh12&n=Anna&f=My+Song.mp3 HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n", bl);
        for(size_t i = 0; i < bl; i++) raw[hl + (int)i] = (char)('a' + i % 26);
        raw[hl + (int)bl] = 0;
        CHECK(http(raw, out, sizeof out) == 403 && strstr(out, "doesn't take uploads"));   /* uploads off */
        rsrv_set_upload("/tmp/remote_test_card/" RMT_UPLOAD_DIR "/Fest_1", 100000);
        CHECK(http(raw, out, sizeof out) == 413);                                          /* larger than allowed */
        rsrv_set_upload("/tmp/remote_test_card/" RMT_UPLOAD_DIR "/Fest_1", 1000000);
        CHECK(http(raw, out, sizeof out) == 200 && strstr(out, "\"song_id\":77") && strstr(out, "\"file\":\"" RMT_UPLOAD_DIR "/Fest_1/Anna_My_Song.mp3\""));
        CHECK(g_last.kind == RMT_CMD_PARTY_UPLOAD && g_last.guest && !strcmp(g_last.gid, "abcdefgh12"));
        FILE *f = fopen("/tmp/remote_test_card/" RMT_UPLOAD_DIR "/Fest_1/Anna_My_Song.mp3", "rb"); long sz = -1; int same = 0;
        if(f){ char *got = malloc(bl); fseek(f, 0, SEEK_END); sz = ftell(f); rewind(f); same = fread(got, 1, bl, f) == bl && !memcmp(got, raw + hl, bl); fclose(f); free(got); }
        CHECK(sz == (long)bl && same);
        CHECK(http(raw, out, sizeof out) == 200 && strstr(out, "Anna_My_Song-2.mp3"));      /* never overwritten */
        char bad[256]; snprintf(bad, sizeof bad, "POST /Fest_1/api/upload?g=abcdefgh12&f=x.exe HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc");
        CHECK(http(bad, out, sizeof out) == 415);
        snprintf(bad, sizeof bad, "POST /Fest_1/api/upload?g=abcdefgh12&f=x.mp3 HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc");   /* breaks off */
        int c = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons((unsigned short)rsrv_port());
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if(connect(c, (void *)&a, sizeof a) == 0){
            send(c, bad, strlen(bad), MSG_NOSIGNAL); shutdown(c, SHUT_WR);
            size_t got = 0; ssize_t rr;
            while(got < sizeof out - 1 && (rr = recv(c, out + got, sizeof out - 1 - got, 0)) > 0) got += (size_t)rr;
            out[got] = 0;
        }
        close(c);
        CHECK(strstr(out, "400") && strstr(out, "broke off") && access("/tmp/remote_test_card/" RMT_UPLOAD_DIR "/Fest_1/guest_x.mp3", F_OK) && access("/tmp/remote_test_card/" RMT_UPLOAD_DIR "/Fest_1/.upload.part", F_OK));
        CHECK(call("GET", p("/api/state"), NULL, out, sizeof out) == 200);                  /* the server went on serving */
        free(raw); rsrv_set_upload("", 0); rsrv_set_party("");
        CHECK(system("rm -rf /tmp/remote_test_card") == 0);
    }
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
        for(int k = 0; k < RMT_P_N; k++) g_pv[k] = RMT_PARTY_KEYS[k].def;
        g_pv[RMT_P_ON] = 1; rsrv_set_party(g_pname);
        printf("http://127.0.0.1:%d/%s/\n", rsrv_port(), TOK); fflush(stdout);
        pthread_t th; pthread_create(&th, NULL, emu, NULL); pthread_join(th, NULL);
        return 0;
    }
    test_core();
    test_party();
    test_game();
    test_party_bandcamp();
    test_server();
    if(fails){ fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    puts("PASS remote control: parsing, routing, token, JSON, HTTP round trip");
    return 0;
}
