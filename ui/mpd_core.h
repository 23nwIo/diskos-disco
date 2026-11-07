/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* MPD protocol server (mpc, ncmpcpp and other MPD clients): the pure parts - no sockets, no LVGL, no database - unit-tested
 * off-device in tests/mpd_test.c. Protocol reference: https://mpd.readthedocs.io/en/latest/protocol.html
 *
 * A client sends lines; a command is one line, or a command list (command_list_begin / command_list_ok_begin ...
 * command_list_end) run as one batch. Every answer ends with "OK" or with "ACK [code@index] {command} message".
 * The session framing (password, idle/noidle, command lists, the commands that need no player) lives here; the commands
 * that touch the player or the library run on the UI thread through a handler (mpd.c on the Disc). */
#ifndef DISKOS_MPD_CORE_H
#define DISKOS_MPD_CORE_H
#include <stddef.h>
#include "remote_core.h"   /* rmt_buf_t, rmt_token_eq, REMOTE_TOKEN_LEN */

#define MPD_PORT          6600
#define MPD_PROTO_VERSION "0.23.5"     /* the protocol level we answer as (clients gate features on it) */
#define MPD_LINE_MAX      4096         /* one request line */
#define MPD_ARGS_MAX      64
#define MPD_LIST_MAX      512          /* commands in one command list */
#define MPD_OUT_MAX       (16u << 20)  /* largest answer (a listallinfo of a big card) */

/* ACK codes (MPD's enum ack) */
enum { MPD_ACK_NOT_LIST = 1, MPD_ACK_ARG = 2, MPD_ACK_PASSWORD = 3, MPD_ACK_PERMISSION = 4, MPD_ACK_UNKNOWN = 5,
       MPD_ACK_NO_EXIST = 50, MPD_ACK_PLAYLIST_MAX = 51, MPD_ACK_SYSTEM = 52, MPD_ACK_PLAYLIST_LOAD = 53,
       MPD_ACK_UPDATE_ALREADY = 54, MPD_ACK_PLAYER_SYNC = 55, MPD_ACK_EXIST = 56 };

/* idle subsystems (bit masks) */
enum { MPD_EV_DATABASE = 1 << 0, MPD_EV_UPDATE = 1 << 1, MPD_EV_STORED_PLAYLIST = 1 << 2, MPD_EV_PLAYLIST = 1 << 3,
       MPD_EV_PLAYER = 1 << 4, MPD_EV_MIXER = 1 << 5, MPD_EV_OUTPUT = 1 << 6, MPD_EV_OPTIONS = 1 << 7 };
#define MPD_EV_COUNT 8
#define MPD_EV_ALL   ((1u << MPD_EV_COUNT) - 1)
const char *mpd_event_name(int bit);       /* bit index 0..MPD_EV_COUNT-1 -> "database" ... */
int  mpd_event_bit(const char *name);      /* "player" -> its mask, 0 unknown */

/* Split a request line into arguments in place: words separated by spaces/tabs; a word in double quotes may hold
 * spaces, and inside quotes a backslash escapes the next character. Returns argc, -1 for bad quoting, -2 for too many. */
int  mpd_tokenize(char *line, char **argv, int max);
/* Quote s for a filter expression or an argument (adds "..." and backslashes). */
void mpd_buf_quoted(rmt_buf_t *b, const char *s);

/* Argument parsing. Each returns 0 ok, -1 bad. */
int  mpd_parse_int(const char *s, long lo, long hi, long *out);
int  mpd_parse_bool(const char *s, int *out);                      /* "0" / "1" */
int  mpd_parse_range(const char *s, long *start, long *end);       /* "N" -> [N,N+1), "A:B" -> [A,B), "A:" -> [A,-1 = open) */
int  mpd_parse_time(const char *s, long *ms, int *relative);       /* "12.5" / "+3" / "-3" (relative = +1 / -1 / 0) */

/* The Disc's play modes against MPD's random/repeat/single switches. Modes: 0 Sequential, 1 Shuffle, 2 Repeat one,
 * 3 Repeat all, 4 Single. The switches can combine in ways the Disc has no mode for: mpd_mode_from_flags returns -1. */
void mpd_flags_from_mode(int mode, int *random, int *repeat, int *single);
int  mpd_mode_from_flags(int random, int repeat, int single);
/* Volume: the Disc's 0..vmax <-> MPD's 0..100 (rounded; every MPD step reaches a distinct Disc level or the next one) */
int  mpd_vol_to_mpd(int v, int vmax);
int  mpd_vol_from_mpd(int p, int vmax);

/* ---- filters: find / search / list / count / findadd / searchadd
 * Old form: TYPE VALUE [TYPE VALUE ...]; new form: one expression argument such as
 *   (artist == "Ana")   ((album == "X") AND (track != "3"))   (title contains "nuit")   (!(genre == "Pop"))
 *   (file starts_with "Music/")   (base "Music/Jazz")   (any contains "x")
 * Operators: == != contains starts_with; AND only (as MPD); a leading ! negates a term. Regular expressions (=~ !~) are
 * refused. find compares exactly; search folds ASCII case (both match substrings for "search"). */
enum { MPD_TAG_ANY = 0, MPD_TAG_FILE, MPD_TAG_BASE, MPD_TAG_TITLE, MPD_TAG_ARTIST, MPD_TAG_ALBUM, MPD_TAG_ALBUMARTIST,
       MPD_TAG_GENRE, MPD_TAG_TRACK, MPD_TAG_DISC, MPD_TAG_DATE, MPD_TAG_NAME, MPD_TAG_COUNT };
int  mpd_tag_from_name(const char *name);  /* case-insensitive; -1 unknown (also "ArtistSort" etc. map to their base tag) */
const char *mpd_tag_name(int tag);         /* the canonical key printed in answers ("AlbumArtist") */
enum { MPD_OP_EQ = 1, MPD_OP_NE, MPD_OP_CONTAINS, MPD_OP_STARTS };
#define MPD_FILTER_MAX 16
typedef struct { int tag, op, neg; char value[256]; } mpd_term_t;
typedef struct { int n; mpd_term_t t[MPD_FILTER_MAX]; int fold; } mpd_filter_t;
/* Parse argv[0..argc) as a filter (either form). fold: 1 for search-like commands. 0 ok, -1 bad (*err says why). */
int  mpd_filter_parse(mpd_filter_t *f, int argc, char **argv, int fold, const char **err);
/* The tags of one song, as strings ("" = absent). */
typedef struct { const char *v[MPD_TAG_COUNT]; } mpd_tags_t;
int  mpd_filter_match(const mpd_filter_t *f, const mpd_tags_t *s);

/* ---- a client session (owned by the server thread; the handler may read it while the server waits) */
typedef struct {
    int authed;           /* the password was given (or none is needed) */
    int list;             /* 0 none, 1 command_list_begin, 2 command_list_ok_begin */
    int nlist;            /* commands collected so far */
    int list_bad;         /* the list overflowed: answered with an ACK at its end */
    rmt_buf_t batch;      /* the collected command lines, '\n'-terminated */
    unsigned idle_mask;   /* while idle: the subsystems waited for */
    int idle;             /* 1 = in idle */
} mpd_sess_t;
void mpd_sess_init(mpd_sess_t *s, int authed);
void mpd_sess_free(mpd_sess_t *s);

/* What the server does with one received line (mpd_sess_line). */
enum { MPD_A_NONE = 0,     /* nothing to send (a line collected into a command list) */
       MPD_A_REPLY,        /* send *out */
       MPD_A_RUN,          /* run s->batch (s->list says how to frame it) on the UI thread through the handler */
       MPD_A_IDLE,         /* enter idle (s->idle_mask); the server answers when an event comes or on noidle */
       MPD_A_NOIDLE,       /* leave idle: answer with what is pending, then "OK" */
       MPD_A_CLOSE };      /* close the connection without an answer */
/* line: one request line without its newline (modified in place). password: the required password (NULL = none). */
int  mpd_sess_line(mpd_sess_t *s, char *line, const char *password, rmt_buf_t *out);

/* The UI-thread handler for one command. Writes its answer lines to out (without the final OK) and returns 0, or
 * returns an ACK code with *msg set (a static or long-lived string). sess: the client's session (read-only). */
typedef int (*mpd_handler_t)(void *ud, int argc, char **argv, rmt_buf_t *out, const char **msg, const mpd_sess_t *sess);
/* Run a batch (s->batch) with the framing s->list asked for; writes the whole answer (ending in OK or an ACK) to out
 * and resets the list state. Commands the session layer owns (ping, password, tagtypes ...) work inside lists too. */
void mpd_run_batch(mpd_sess_t *s, const char *password, mpd_handler_t h, void *ud, rmt_buf_t *out);

/* Is this a command the session layer knows (so "commands" can list it)? The handler's own list is in mpd.c. */
extern const char *const MPD_SESSION_COMMANDS[];
/* "ACK [code@index] {command} message\n" */
void mpd_ack(rmt_buf_t *out, int code, int index, const char *cmd, const char *msg);
/* A key: value line; value's newlines become spaces (a value can never break the framing). */
void mpd_kv(rmt_buf_t *out, const char *key, const char *value);
#endif
