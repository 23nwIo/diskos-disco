/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote control over Wi-Fi: the pure parts (no sockets, no LVGL), unit-tested off-device in tests/remote_test.c.
 *
 * The control page and its JSON API live under a secret path: http://<wlan0-ip>:REMOTE_PORT/<token>/...
 *   GET  /<token>/                 the control page (HTML + JS, no external resources)
 *   GET  /<token>/system           the system page: Status, Bluetooth, Screen
 *   GET  /<token>/api/state        playback state as JSON
 *   POST /<token>/api/<command>    play_pause | play | pause | next | previous
 *                                  volume (value=0..120 or step=-120..120) | seek (position_ms=N) | play_mode (mode=0..4)
 *                                  balance (value=-10..10: 0 centred, negative left, positive right)
 *                                  list_end (mode=0..3 [pos=N url=U: a radio favourite as the station] [pl=ID flags=0..3:
 *                                  the playlist for mode 3, Repeat 1 + Shuffle 2]; own player only)
 *                                  outputs (one of aux=0/1, bt=0/1, vol_aux=0..120, vol_bt=0..120, auto=0..2, codec=0..4;
 *                                  own player only)
 * Parameters come from the query string or an application/x-www-form-urlencoded body. Commands are POST only, so a
 * link preview or prefetch can never press a button. Anything without the exact token is a plain 404. */
#ifndef DISKOS_REMOTE_CORE_H
#define DISKOS_REMOTE_CORE_H
#include <stddef.h>
#include "hitguess_core.h"

#define REMOTE_PORT       8088
#define REMOTE_TOKEN_LEN  20          /* base32 characters = 100 random bits */
#define REMOTE_REQ_MAX    4096        /* whole request (headers + body) */

typedef enum {
    RMT_CMD_NONE = 0,
    RMT_CMD_PLAY_PAUSE, RMT_CMD_PLAY, RMT_CMD_PAUSE, RMT_CMD_NEXT, RMT_CMD_PREV,
    RMT_CMD_VOLUME, RMT_CMD_VOLUME_STEP, RMT_CMD_SEEK, RMT_CMD_PLAY_MODE,
    RMT_CMD_BROWSE,          /* list part of the library: sub = rmt_list_t; the answer is a JSON body */
    RMT_CMD_PLAY_ITEM,       /* start something from the library: sub = rmt_item_t */
    RMT_CMD_RADIO_STOP,      /* stop the web radio, back to local playback */
    RMT_CMD_RADIO_SEARCH,    /* start a station search: q ("" = popular) */
    RMT_CMD_COVER,           /* the current cover image (binary answer) */
    RMT_CMD_STATUS,          /* the Status tab: device facts (JSON answer) */
    RMT_CMD_EQ_LIST,         /* the EQ presets and the current one (JSON answer) */
    RMT_CMD_EQ,              /* select EQ preset arg (0 = off .. 20) */
    RMT_CMD_EDIT,            /* change playlists / favourites: sub = rmt_edit_t */
    RMT_CMD_RESCAN,          /* arg 1 start / 0 stop a library rescan */
    RMT_CMD_BT_LIST,         /* Bluetooth state and devices (JSON answer) */
    RMT_CMD_BT,              /* sub = rmt_bt_t; q = device address, name = a local name */
    RMT_CMD_WORK_MODE,       /* arg 0 Playback, 1 USB DAC, 2 BT DAC, 3 USB storage, 4 BT streaming, 5 AirPlay */
    RMT_CMD_REBOOT,          /* restart the Disc (as Settings > Restart) */
    RMT_CMD_RADIO_FAV,       /* arg 1 save / 0 remove a station: sub = rmt_item_t (RADIO or RADIO_HIT), pos, name = url */
    RMT_CMD_SCREEN,          /* the screen as it looks now (binary answer: BMP) */
    RMT_CMD_TOUCH,           /* a finger on the screen: pos,id = x,y down; offset,limit = x,y up; arg = ms down */
    RMT_CMD_BALANCE,         /* left/right balance arg -10..10 (as Settings > Audio > Balance) */
    RMT_CMD_OUTPUTS,         /* Settings > Audio > Outputs: sub = rmt_out_t, arg = its value */
    RMT_CMD_QUEUE,           /* what plays next (queueview.h): sub = rmt_queue_t; see api/queue below */
    RMT_CMD_PARTY_STATE,     /* a guest's view of the state (party page; JSON answer) */
    RMT_CMD_PARTY_RADIO,     /* a guest picks a station (their own list or a search hit): q = name, name = url, arg 1 now / 0 when the queue is empty */
    RMT_CMD_PARTY_UPLOAD,    /* a guest's upload is on the card: name = its path relative to the card (the server thread wrote it) */
    RMT_CMD_PARTY_STATIONS,  /* a guest's own radio stations (kept in their browser), for the host to see: text */
    RMT_CMD_PARTY,           /* the party settings: arg 0 read (JSON answer), 1 change (party[] / name; -1 = leave) */
    RMT_CMD_GAME,            /* a player's request in a game (party link while a game is the mode): sub = rmt_game_t */
    RMT_CMD_GAME_SEARCH,     /* the player whose turn it is searches the game's songs: q, offset, limit */
    RMT_CMD_GAME_ADMIN,      /* the host's game page: arg 0 read, 1 an action (sub) or the setup (game[], name = folder, id = playlist) */
    RMT_CMD_BANDCAMP,        /* Bandcamp (bandcamp.h): sub = rmt_bc_t; q = search text, name = a Bandcamp address, pos = track */
    RMT_CMD_LIST_END         /* Playback > After the List Ends: arg = mode 0 stop, 1 shuffle all, 2 station; pos = radio favourite
                              * (1-based, 0 = keep the station), name = its url (checked) */
} rmt_cmd_kind_t;
/* POST api/touch x, y (0..359), optional x2, y2 (where it lifts; default x, y) and ms (0..3000, default 100). */
#define RMT_SCREEN_PX 360
#define RMT_TOUCH_MS_MAX 3000

/* GET api/library/<list>: albums | album (name) | artists | artist (name) | playlists | playlist (id) | favorites |
 * songs (q: search) | folder (path, relative to the SD card). Lists take q (filter), offset, limit. */
typedef enum { RMT_L_ALBUMS = 1, RMT_L_ALBUM, RMT_L_ARTISTS, RMT_L_ARTIST, RMT_L_PLAYLISTS, RMT_L_PLAYLIST,
               RMT_L_FAVORITES, RMT_L_SONGS, RMT_L_FOLDER, RMT_L_RADIO, RMT_L_RADIO_HITS } rmt_list_t;
/* POST api/play_item with kind=album (name, song_id) | artist (name, song_id) | playlist (id, pos) | favorites (pos)
 * | song (song_id) | file (path) | all | radio (pos, optional url: refused if that favourite is no longer there)
 * | radio_hit (pos, url: a station from the last search).
 * song_id / pos are optional: the list starts at its first song. */
typedef enum { RMT_I_ALBUM = 1, RMT_I_ARTIST, RMT_I_PLAYLIST, RMT_I_FAVORITES, RMT_I_SONG, RMT_I_FILE, RMT_I_ALL, RMT_I_RADIO, RMT_I_RADIO_HIT, RMT_I_FOLDER, RMT_I_RADIO_NOW, RMT_I_STREAM } rmt_item_t;   /* radio_now: the station playing; stream: a URL typed in (q = name, name = url) */

/* POST api/playlist_create (name) | playlist_rename (id, name) | playlist_delete (id) | playlist_add (id + kind=song
 * song_id | kind=album name | kind=artist name) | playlist_remove (id, pos, song_id: refused unless that row still holds
 * that song) | favorite (on=0/1, song_id; no song_id = the song playing now) */
/* POST api/bluetooth action=on|off|refresh|search; POST api/bt_device action=connect|disconnect|forget|rename, mac
 * (AA:BB:CC:DD:EE:FF), name (rename; "" clears the local name) */
/* action=codec with codec=0..9: the Bluetooth Codec setting (arg) */
/* POST api/outputs: exactly one of aux=0/1 | bt=0/1 | vol_aux=0..120 | vol_bt=0..120 | auto=0..2 (0 off, 1 newest, 2 all connected)
 * | codec=0..4 (this Bluetooth connection: 0 SBC, 1 AAC, 2 LDAC, 3 aptX, 4 aptX HD) */
typedef enum { RMT_OUT_AUX = 1, RMT_OUT_BT, RMT_OUT_VOL_AUX, RMT_OUT_VOL_BT, RMT_OUT_AUTO, RMT_OUT_CODEC } rmt_out_t;
/* GET api/queue [limit=1..500 Up Next rows, default 100]: the list (JSON answer). POST api/queue with ver (the list's
 * "ver", refused when it changed since: the page reads the list again) and action=jump (kind=queue i=N | kind=next b=ID)
 * | remove (i) | move (i, to) | clear; action=add (no ver) kind=song song_id | kind=album name | kind=artist name | kind=next b=ID
 * (an Up next row: item -1, id = b) adds to the end of the queue. cmd: sub = action, item = 1 queue / 2 next, pos = i, id = b, offset = to,
 * arg = ver (list: limit). GET api/state?q=1 also reports queue_ver (only asked for while a page shows the queue). */
/* Games (Hit&Guess, hitguess.c). The players' link is the party link while the mode (party setting "on") is 2:
 *   GET  api/game?g=&n=           the player's view (JSON)
 *   POST api/game action=... g= n= join | pick song_id | random | reroll | reveal | mark p= m= | confirm | replay | stop
 *                                 | tip a= t= y= | leave
 *   GET  api/game_search?g=&q=&offset=&limit=   the game's songs (only for the player choosing now)
 * The host: GET /<token>/hitguess (page), GET api/game_admin, POST api/game_admin action=start | skip | remove p= | end
 * | again | reset, or the setup keys (HG_KEYS) with dir= (the pool's folder, relative to the card) and pl= (playlist id). */
typedef enum { RMT_G_STATE = 0, RMT_G_JOIN, RMT_G_PICK, RMT_G_RANDOM, RMT_G_REROLL, RMT_G_REVEAL, RMT_G_MARK, RMT_G_CONFIRM,
               RMT_G_REPLAY, RMT_G_STOP, RMT_G_TIP, RMT_G_LEAVE,
               RMT_G_START, RMT_G_SKIP, RMT_G_REMOVE, RMT_G_END, RMT_G_AGAIN, RMT_G_RESET, RMT_G_SETUP,
               RMT_G_FIXSONG } rmt_game_t;   /* corrections for a song (name = artist, text = title, offset = year, item = leave it
                                              * out): the player whose turn it is, or the host (pos = the turn, -1 the one now) */
/* Bandcamp (own player only): GET api/bandcamp = search results, the page opened and what plays (JSON);
 * POST api/bandcamp action=search q= | open url= (an artist's site, an album or track page) | play url= [track=N, the
 * track's index on the page; absent = the first that plays] | stop. The downloads run on the Disc; the page polls. */
typedef enum { RMT_BC_STATE = 0, RMT_BC_SEARCH, RMT_BC_OPEN, RMT_BC_PLAY, RMT_BC_STOP } rmt_bc_t;
typedef enum { RMT_Q_LIST = 1, RMT_Q_JUMP, RMT_Q_REMOVE, RMT_Q_MOVE, RMT_Q_CLEAR, RMT_Q_ADD } rmt_queue_t;
typedef enum { RMT_BT_ON = 1, RMT_BT_OFF, RMT_BT_REFRESH, RMT_BT_SEARCH, RMT_BT_CONNECT, RMT_BT_DISCONNECT, RMT_BT_FORGET, RMT_BT_RENAME, RMT_BT_CODEC } rmt_bt_t;
typedef enum { RMT_E_PL_CREATE = 1, RMT_E_PL_RENAME, RMT_E_PL_DELETE, RMT_E_PL_ADD, RMT_E_PL_REMOVE, RMT_E_FAV } rmt_edit_t;
#define RMT_PL_NAME_MAX 64
/* Party mode: guests use their own link /<party name>/ with a reduced page (rmt_route_party). The settings, as numbers;
 * "on" is the mode of that link: 0 off, 1 the party, 2 the game Hit&Guess (one at a time, Jo 2026-11-18): */
enum { RMT_P_ON, RMT_P_VOL, RMT_P_MAXVOL, RMT_P_NP, RMT_P_QUEUE, RMT_P_SKIP, RMT_P_TOTAL, RMT_P_CONSEC, RMT_P_REPEAT,
       RMT_P_RADIO, RMT_P_NAMES, RMT_P_UPLOAD, RMT_P_UPMAX, RMT_P_N };
typedef struct { const char *key; int min, max, def; } rmt_party_key_t;
extern const rmt_party_key_t RMT_PARTY_KEYS[RMT_P_N];   /* key = the API parameter and (with "party_") the cfg key */
#define RMT_PARTY_NAME_MAX 32
#define RMT_GID_MAX 24          /* a guest's random id (the page makes it), [A-Za-z0-9], 8..24 */
#define RMT_GNAME_MAX 24        /* a guest's name as shown in the queue (bytes) */
/* 1 if s can be a party name: A-Z a-z 0-9 '-' '_', 2..RMT_PARTY_NAME_MAX, not "api" / "system" */
int  rmt_party_name_ok(const char *s);
/* Where a guest's song goes in the queue. who[0..n-1]: the guest of each queued song (0 = the host's). Guests take
 * turns: the song joins the guest's next round (round k = each guest's k-th queued song), after every host song and
 * every song of that round or earlier. now_who: the guest whose song plays now. Returns the position (0..n), or -1
 * when it would make more than consec of g's songs play in a row (someone else has to add one first) - unless every
 * queued song is g's (nobody else waits: Jo, 2026-11-16). */
int  rmt_party_place(const int *who, int n, int g, int consec, int now_who);
/* Guests' uploads go to <card>/RMT_UPLOAD_DIR/<party name>/ (Jo, 2026-11-16: no spaces in the path). */
#define RMT_UPLOAD_DIR "PartyUploads"
#define RMT_UPLOAD_EXTS "mp3 flac m4a ogg opus oga wav aif aiff ape wma aac dsf dff dts"   /* the scanner's (scanner.c is_audio) */
/* The file name an upload is saved under: "<guest>_<name>.<ext>", spaces and characters a card can't take turned into
 * '_', at most ~120 bytes. guest "" = "guest". 0 ok, -1 not a music file (RMT_UPLOAD_EXTS) or no usable name. */
int  rmt_upload_name(const char *guest, const char *file, char *out, size_t cap);
#define RMT_LIMIT_DEF 200
#define RMT_LIMIT_MAX 500
typedef struct {
    rmt_cmd_kind_t kind;
    long arg;                /* volume / step / seek ms / play mode */
    int  sub;                /* rmt_list_t, rmt_item_t or rmt_edit_t */
    int  item;               /* playlist_add: what to add (RMT_I_SONG / ALBUM / ARTIST) */
    long id, song_id, pos;   /* playlist id, SONG.ID, 1-based position (0 = absent) */
    long offset, limit;
    char name[512];          /* album / artist name, a folder/file path relative to the SD card, or a station URL */
    char q[96];              /* filter text */
    int  guest;              /* 1: came in through the party link (rmt_route_party); gid/gname from g= / n= */
    char gid[RMT_GID_MAX + 1], gname[RMT_GNAME_MAX + 1];
    int  party[RMT_P_N];     /* RMT_CMD_PARTY: new values, -1 = unchanged; name = the new party name ("" = unchanged) */
    char text[3072];         /* RMT_CMD_PARTY_STATIONS: the guest's stations, "name\turl\n" each; RMT_G_TIP: the title */
    int  game[HG_S_N];       /* RMT_G_SETUP: new values, -1 = unchanged */
} rmt_cmd_t;
/* An upload is POST /<party>/api/upload?g=<guest id>&n=<name>&f=<file name> with the file as the whole body
 * (Content-Length, no chunking): too big for the normal request buffer, so the server checks the head first.
 * buf: what has arrived (NUL-terminated, n bytes). 1 = an upload (cmd: guest, gid, gname, name = f; *len = body bytes,
 * *hlen = head bytes incl. the blank line), 0 = another request or the head isn't complete yet, -1 = a malformed upload. */
int  rmt_upload_head(const char *buf, size_t n, const char *party, rmt_cmd_t *cmd, long long *len, size_t *hlen);

typedef enum {
    RMT_R_NOT_FOUND = 0,   /* 404: wrong/missing token or unknown path */
    RMT_R_REDIRECT,        /* 301 to /<token>/ (the page uses relative URLs) */
    RMT_R_PAGE,            /* 200 the control page */
    RMT_R_SYS_PAGE,        /* 200 the system page (/<token>/system) */
    RMT_R_PARTY_PAGE,      /* 200 the guests' page (/<party>/): the party's, or the game's while a game is the mode */
    RMT_R_STATE,           /* 200 state JSON */
    RMT_R_CMD,             /* run route.cmd */
    RMT_R_BAD_METHOD,      /* 405 */
    RMT_R_BAD_PARAM,       /* 400: missing or out-of-range parameter */
    RMT_R_GAME_PAGE        /* 200 the host's game page (/<token>/hitguess) */
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
/* Route a request on the guests' link /<party>/ (party = a valid party name, mode on). Only what guests may use
 * routes; everything else is RMT_R_NOT_FOUND. Commands carry guest 1 and the guest's g= / n=; the main thread still
 * checks the party settings (remote.c). REDIRECT here means 301 to /<party>/. */
rmt_route_t rmt_route_party(const rmt_req_t *r, const char *party);

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
    int  balance;            /* -10..10, 0 centred (the saved setting) */
    int  end_mode;           /* After the List Ends 0..3, -1 = not offered (stock player) */
    char end_station[96];    /* its station's name */
    char end_playlist[96];   /* its playlist's name ("" none or gone), id and flags (Repeat 1, Shuffle 2) */
    long end_pl_id; int end_pl_flags;
    unsigned queue_ver;      /* the queue list's version (queueview.h); 0 while no page shows the queue */
    /* Settings > Audio > Outputs, only while the own player runs (out_own 1; else "outputs":null) */
    int  out_own, out_aux, out_bt, out_vol_aux, out_vol_bt, out_bt_ready, out_auto, out_jack, out_bal;
    int  out_codec, out_codecs, out_switching;   /* the link's codec 0..4 (-1 none), offered codecs (bit per id), a switch runs */
    int  play_mode;          /* 0..4, -1 unknown */
    int  work_mode;          /* the source mode, 0..6 (6 web radio), -1 unknown */
    int  controllable;       /* 1 if transport commands can reach a player now */
    const char *source;      /* "local" | "music_assistant" | "radio" | "other" (static string) */
    const char *radio_state; /* "off" | "starting" | "playing" | "error" (static string; NULL = "off") */
    char station[96], radio_error[96];
    char bt_device[64];      /* the connected Bluetooth headphones' shown name ("" none) */
    int  station_fav;        /* 1/0 the playing station is a radio favourite, -1 no station */
    char bt_codec[16];       /* the codec the Bluetooth headphones run now ("" when the output is not Bluetooth) */
    char stream_title[330];  /* the song the station names (ICY / Ogg comments), "" none */
    char station_url[512];   /* the playing station's stream URL ("" when no radio plays) */
    char format[16];         /* "FLAC", "MP3", "DSD128", a radio codec; "" unknown */
    int  kbps, bits, sample_rate, channels;   /* 0 = unknown */
    int  favorite;           /* -1 unknown, 0/1 */
    int  eq;                 /* the EQ preset in use (0 = off), -1 unknown */
    char eq_name[48];
    char art[32];            /* changes whenever the cover does; "" = none (GET api/cover?v=<art>) */
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

/* The pages (static HTML, remote_page.c, generated from ui/remote_page/ by ui/tools/gen_remote_page.py): the control
 * page at /<token>/ (Now playing, queue, library) and the system page at /<token>/system (Status, Bluetooth, Screen).
 * Both are self-contained and use relative URLs. */
extern const char RMT_PAGE_HTML[], RMT_SYS_HTML[], RMT_PARTY_HTML[], RMT_HG_HTML[], RMT_HGADMIN_HTML[];
#endif
