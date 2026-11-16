/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* What plays next, as one list: the song playing now, the songs queued on the Disc (ui/queue.c: Play next / Add to
 * queue, also from MPD clients and the web page), then the rest of the player's own list ("Up Next", LIST_SONG_0).
 * Shared by the MPD server (mpd.c: playlistinfo, play, ...) and Remote Control (remote.c: GET/POST api/queue).
 * Main thread only (it reads the player's database and the queue). */
#ifndef QUEUEVIEW_H
#define QUEUEVIEW_H
#include "musicdb.h"
#include "remote_core.h"

#define QV_NEXT_MAX 500            /* Up Next rows in the list at most */
#define QV_CAP      (QV_NEXT_MAX + 400 + 2)   /* + the queue (ui/queue.c QMAX) + the current song */

enum { QV_CUR = 1, QV_QUEUE, QV_NEXT, QV_OTHER };
typedef struct {
    int kind;                      /* QV_* */
    int base_id, ord;              /* QV_CUR/QV_NEXT: LIST_SONG_0.ID and its 1-based position (0 in shuffle);
                                    * QV_OTHER: ord 1 = a web radio station (artist holds its name) */
    int idx;                       /* QV_QUEUE: index in the queue */
    int dur_ms;
    char path[512];                /* absolute path (QV_OTHER: the station's stream URL, or "") */
    char title[MDB_STR], artist[MDB_STR];
} qv_entry_t;
typedef struct {
    qv_entry_t *e; int n;          /* e[0] is the song playing now (QV_CUR / QV_OTHER) when there is one */
    int more;                      /* Up Next rows left out (beyond QV_NEXT_MAX) */
    int shuffle;                   /* the player shuffles: Up Next is in play order, but positions are looked up on a tap */
    unsigned ver;                  /* changes whenever the content does (a page or client reads the list again) */
} qv_view_t;

/* The list for state s (rmt_state_t: source, radio state, the current song). Cached: built again when the song, the
 * play mode or the queue changed, after 3 s (the player can change Up Next by itself), or with fresh. NULL when out of
 * memory. */
const qv_view_t *qv_view(const rmt_state_t *s, int fresh);
void qv_invalidate(void);
/* Play entry v (QV_QUEUE or QV_NEXT) now. 0 sent; otherwise *why and QV_E_*. */
enum { QV_E_REFUSED = -1, QV_E_CHANGED = -2, QV_E_BUSY = -3, QV_E_KIND = -4 };
int qv_play(const qv_entry_t *v, const char **why);
#endif
