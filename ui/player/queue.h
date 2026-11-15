/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The play queue in memory: which LIST_SONG_0 rows play in which order, and which one is current. Pure logic (no
 * SQLite, no MPD), so ui/tests drive it on the host. The rows themselves stay in song.db (queuedb.c); this only holds
 * their IDs.
 *
 *   base[]  = LIST_SONG_0.ID in ID order (the list as built; a 0100 position counts in this order, also in shuffle)
 *   order[] = the play order: base[] in order, or the LIST_SONG_3 permutation in shuffle
 *   cur     = index into order[], -1 none */
#ifndef DISKOS_PLAYER_QUEUE_H
#define DISKOS_PLAYER_QUEUE_H

/* play modes, as 0102 sends them */
enum { PQ_IN_ORDER = 0, PQ_SHUFFLE = 1, PQ_REPEAT_ONE = 2, PQ_REPEAT_ALL = 3, PQ_SINGLE = 4 };

typedef struct {
    int n;
    int *base, *order;
    int cur;
    int shuffled;                  /* order[] is a permutation (shuffle), not base[] */
} pq_t;

/* Take over base (n IDs in ID order, malloc'd; pq owns it from now on, also on failure) and set cur to base position
 * pos (0-based; out of range = 0). order[] = base[]. 0, or -1 when out of memory (q is then empty). */
int pq_set(pq_t *q, int *base, int n, int pos);
void pq_free(pq_t *q);

/* Shuffle on: order[] becomes a random permutation with the current song first (rnd(k) returns 0..k-1).
 * Shuffle off: order[] = base[], cur stays on the same song. 0 or -1 (out of memory). */
int pq_shuffle(pq_t *q, int on, unsigned (*rnd)(unsigned k));

/* Take over a stored play order (LIST_SONG_3's POS_IDs, malloc'd; pq owns it): used only when it is an exact
 * permutation of base[] (as the UI's Up Next requires too). The current song stays current. 0, or -1 (rejected, q unchanged). */
int pq_adopt_order(pq_t *q, int *order, int n);

/* Index into order[] of LIST_SONG_0.ID id, -1 if not in the queue. */
int pq_find(const pq_t *q, int id);
/* Index into order[] of base position pos (a 0100 type 0 jump), -1 if out of range. */
int pq_from_base(const pq_t *q, int pos);

/* The song after cur, as an index into order[], -1 for none.
 *   end_of_song = 1: what plays when the current song ends (repeat one: the same; single: none; in order: none after
 *                    the last; shuffle and repeat all: wrap around);
 *   end_of_song = 0: the Next key (always the following song, wrapping around). */
int pq_next(const pq_t *q, int mode, int end_of_song);
/* The Previous key: the song before cur, wrapping around; -1 for an empty queue. */
int pq_prev(const pq_t *q);
/* 1-based position of cur in the play order, for "playing_num" ("3/8"). */
static inline int pq_number(const pq_t *q){ return q->cur + 1; }
static inline int pq_id(const pq_t *q, int i){ return (i >= 0 && i < q->n) ? q->order[i] : 0; }
#endif
