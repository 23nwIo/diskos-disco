/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote control over Wi-Fi: the pure parts (no sockets, no LVGL), unit-tested off-device in tests/remote_test.c.
 *
 * The control page and its JSON API live under a secret path: http://<wlan0-ip>:REMOTE_PORT/<token>/...
 *   GET  /<token>/                 the control page (HTML + JS, no external resources)
 *   GET  /<token>/api/state        playback state as JSON
 *   POST /<token>/api/<command>    play_pause | play | pause | next | previous
 *                                  volume (value=0..120 or step=-120..120) | seek (position_ms=N) | play_mode (mode=0..4)
 * Parameters come from the query string or an application/x-www-form-urlencoded body. Commands are POST only, so a
 * link preview or prefetch can never press a button. Anything without the exact token is a plain 404. */
#ifndef DISKOS_REMOTE_CORE_H
#define DISKOS_REMOTE_CORE_H
#include <stddef.h>

#define REMOTE_PORT       8088
#define REMOTE_TOKEN_LEN  20          /* base32 characters = 100 random bits */
#define REMOTE_REQ_MAX    4096        /* whole request (headers + body) */

typedef enum {
    RMT_CMD_NONE = 0,
    RMT_CMD_PLAY_PAUSE, RMT_CMD_PLAY, RMT_CMD_PAUSE, RMT_CMD_NEXT, RMT_CMD_PREV,
    RMT_CMD_VOLUME, RMT_CMD_VOLUME_STEP, RMT_CMD_SEEK, RMT_CMD_PLAY_MODE,
    RMT_CMD_BROWSE,          /* list part of the library: sub = rmt_list_t; the answer is a JSON body */
    RMT_CMD_PLAY_ITEM,       /* start something from the library: sub = rmt_item_t */
    RMT_CMD_RADIO_STOP       /* stop the web radio, back to local playback */
} rmt_cmd_kind_t;

/* GET api/library/<list>: albums | album (name) | artists | artist (name) | playlists | playlist (id) | favorites |
 * songs (q: search) | folder (path, relative to the SD card). Lists take q (filter), offset, limit. */
typedef enum { RMT_L_ALBUMS = 1, RMT_L_ALBUM, RMT_L_ARTISTS, RMT_L_ARTIST, RMT_L_PLAYLISTS, RMT_L_PLAYLIST,
               RMT_L_FAVORITES, RMT_L_SONGS, RMT_L_FOLDER, RMT_L_RADIO } rmt_list_t;
/* POST api/play_item with kind=album (name, song_id) | artist (name, song_id) | playlist (id, pos) | favorites (pos)
 * | song (song_id) | file (path) | all | radio (pos, optional url: refused if that favourite is no longer there).
 * song_id / pos are optional: the list starts at its first song. */
typedef enum { RMT_I_ALBUM = 1, RMT_I_ARTIST, RMT_I_PLAYLIST, RMT_I_FAVORITES, RMT_I_SONG, RMT_I_FILE, RMT_I_ALL, RMT_I_RADIO } rmt_item_t;

#define RMT_LIMIT_DEF 200
#define RMT_LIMIT_MAX 500
typedef struct {
    rmt_cmd_kind_t kind;
    long arg;                /* volume / step / seek ms / play mode */
    int  sub;                /* rmt_list_t or rmt_item_t */
    long id, song_id, pos;   /* playlist id, SONG.ID, 1-based position (0 = absent) */
    long offset, limit;
    char name[512];          /* album / artist name, a folder/file path relative to the SD card, or a station URL */
    char q[96];              /* filter text */
} rmt_cmd_t;

typedef enum {
    RMT_R_NOT_FOUND = 0,   /* 404: wrong/missing token or unknown path */
    RMT_R_REDIRECT,        /* 301 to /<token>/ (the page uses relative URLs) */
    RMT_R_PAGE,            /* 200 the control page */
    RMT_R_STATE,           /* 200 state JSON */
    RMT_R_CMD,             /* run route.cmd */
    RMT_R_BAD_METHOD,      /* 405 */
    RMT_R_BAD_PARAM        /* 400: missing or out-of-range parameter */
} rmt_route_kind_t;

typedef struct { rmt_route_kind_t kind; rmt_cmd_t cmd; } rmt_route_t;

/* A parsed request; the pointers point into the caller's (modified) buffer. */
typedef struct {
    char *method, *path, *query, *body;   /* query/body: "" when absent */
    long content_length;                   /* -1 when absent */
} rmt_req_t;

/* Has buf (n bytes, NUL-terminated) received a whole request yet? 1 complete, 0 need more, -1 malformed/too large. */
int  rmt_req_complete(const char *buf, int n);
/* Parse a complete request in place. 0 ok, -1 malformed. */
int  rmt_req_parse(char *buf, rmt_req_t *r);
/* Route a parsed request against the token. */
rmt_route_t rmt_route(const rmt_req_t *r, const char *token);

/* Constant-time comparison of a path segment of length n with the token (1 = equal). */
int  rmt_token_eq(const char *seg, size_t n, const char *token);
/* Encode REMOTE_TOKEN_LEN base32 characters from at least 13 random bytes. out needs REMOTE_TOKEN_LEN + 1. */
void rmt_token_encode(const unsigned char *rnd, size_t n, char *out);
/* 1 if s looks like a token this code made (right length and alphabet). */
int  rmt_token_valid(const char *s);

/* Playback snapshot the main thread publishes for the server. */
typedef struct {
    char title[160], artist[160], album[160];
    int  has_track, playing;
    long position_ms, duration_ms;
    int  volume, volume_max;
    int  play_mode;          /* 0..4, -1 unknown */
    int  controllable;       /* 1 if transport commands can reach a player now */
    const char *source;      /* "local" | "music_assistant" | "radio" | "other" (static string) */
    const char *radio_state; /* "off" | "starting" | "playing" | "error" (static string; NULL = "off") */
    char station[96], radio_error[96];
} rmt_state_t;

/* JSON helpers. Both return the length written (truncation-safe, always NUL-terminated), or -1 if it didn't fit. */
int  rmt_json_escape(char *dst, size_t cap, const char *src);
int  rmt_state_json(char *dst, size_t cap, const rmt_state_t *s);

const char *rmt_cmd_name(rmt_cmd_kind_t k);

/* A relative SD path from the API: no leading '/', no "." or ".." segments, no empty segments, no control bytes.
 * "" is the card's root. 1 = acceptable. (The caller still checks the resolved path stays on the card.) */
int  rmt_relpath_ok(const char *p);
/* Case-insensitive (ASCII) substring test; an empty needle matches everything. */
int  rmt_match(const char *hay, const char *needle);

/* A growing JSON text buffer (main thread). Writes after an allocation failure are dropped and oom is set. */
typedef struct { char *p; size_t n, cap; int oom; } rmt_buf_t;
void rmt_buf_put(rmt_buf_t *b, const char *s);
void rmt_buf_printf(rmt_buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void rmt_buf_str(rmt_buf_t *b, const char *s);   /* a quoted, escaped JSON string */
char *rmt_buf_take(rmt_buf_t *b);                 /* the text (caller frees), NULL after oom */

/* The control page (static HTML). */
extern const char RMT_PAGE_HTML[];
#endif
