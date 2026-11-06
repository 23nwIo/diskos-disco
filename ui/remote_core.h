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
    RC_CMD_NONE = 0,
    RC_CMD_PLAY_PAUSE, RC_CMD_PLAY, RC_CMD_PAUSE, RC_CMD_NEXT, RC_CMD_PREV,
    RC_CMD_VOLUME, RC_CMD_VOLUME_STEP, RC_CMD_SEEK, RC_CMD_PLAY_MODE
} rc_cmd_kind_t;

typedef struct { rc_cmd_kind_t kind; long arg; } rc_cmd_t;

typedef enum {
    RC_R_NOT_FOUND = 0,   /* 404: wrong/missing token or unknown path */
    RC_R_REDIRECT,        /* 301 to /<token>/ (the page uses relative URLs) */
    RC_R_PAGE,            /* 200 the control page */
    RC_R_STATE,           /* 200 state JSON */
    RC_R_CMD,             /* run route.cmd */
    RC_R_BAD_METHOD,      /* 405 */
    RC_R_BAD_PARAM        /* 400: missing or out-of-range parameter */
} rc_route_kind_t;

typedef struct { rc_route_kind_t kind; rc_cmd_t cmd; } rc_route_t;

/* A parsed request; the pointers point into the caller's (modified) buffer. */
typedef struct {
    char *method, *path, *query, *body;   /* query/body: "" when absent */
    long content_length;                   /* -1 when absent */
} rc_req_t;

/* Has buf (n bytes, NUL-terminated) received a whole request yet? 1 complete, 0 need more, -1 malformed/too large. */
int  rc_req_complete(const char *buf, int n);
/* Parse a complete request in place. 0 ok, -1 malformed. */
int  rc_req_parse(char *buf, rc_req_t *r);
/* Route a parsed request against the token. */
rc_route_t rc_route(const rc_req_t *r, const char *token);

/* Constant-time comparison of a path segment of length n with the token (1 = equal). */
int  rc_token_eq(const char *seg, size_t n, const char *token);
/* Encode REMOTE_TOKEN_LEN base32 characters from at least 13 random bytes. out needs REMOTE_TOKEN_LEN + 1. */
void rc_token_encode(const unsigned char *rnd, size_t n, char *out);
/* 1 if s looks like a token this code made (right length and alphabet). */
int  rc_token_valid(const char *s);

/* Playback snapshot the main thread publishes for the server. */
typedef struct {
    char title[160], artist[160], album[160];
    int  has_track, playing;
    long position_ms, duration_ms;
    int  volume, volume_max;
    int  play_mode;          /* 0..4, -1 unknown */
    int  controllable;       /* 1 if transport commands can reach a player now */
    const char *source;      /* "local" | "music_assistant" | "other" (static string) */
} rc_state_t;

/* JSON helpers. Both return the length written (truncation-safe, always NUL-terminated), or -1 if it didn't fit. */
int  rc_json_escape(char *dst, size_t cap, const char *src);
int  rc_state_json(char *dst, size_t cap, const rc_state_t *s);

const char *rc_cmd_name(rc_cmd_kind_t k);

/* The control page (static HTML). */
extern const char RC_PAGE_HTML[];
#endif
