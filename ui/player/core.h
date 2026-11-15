/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* What the own player does with each UI command (docs/PLAYER_PROTOCOL.md, column "Step 2"). No I/O of its own: replies
 * go through the send callback and notes through the log callback, so ui/tests can drive it on the host.
 *
 * Skeleton stage (2026-11-15): every command is parsed, settings are remembered, state requests are answered with "no
 * current track". Playback, the DAC and the queue tables come in the next steps. */
#ifndef DISKOS_PLAYER_CORE_H
#define DISKOS_PLAYER_CORE_H

#define PL_WORK_LOCAL 8            /* 0657 / a607: local playback */
#define PL_ROUTE_DAC  6            /* 0666: internal DAC */
#define PL_VOL_MAX    120

typedef void (*pl_send_fn)(void *ctx, const char *frame, int len);
typedef void (*pl_log_fn)(void *ctx, const char *msg);

typedef struct {
    /* applied settings (Step 2 "do") */
    int volume, max_volume;        /* 0..120 */
    int balance;                   /* raw 0713 value: 0 centre, 00NN / 01NN one side or the other */
    int gain;                      /* 0 low, 1 high */
    int dac_filter;                /* 0..5 */
    int dre;                       /* 0 on, 1 off */
    int eq_preset;                 /* 0..20 */
    int play_mode;                 /* 0 in order, 1 shuffle, 2 repeat one, 3 repeat all, 4 single */
    int artist_grouping;           /* 0 artist, 1 album artist */
    int work_mode;                 /* last mode set; -1 none yet */
    int route;                     /* 0666 output route; -1 none yet */
    int idle_off_s, sleep_s;       /* 0 off */
    /* remembered only (Step 2 "keep") */
    int gapless, rotation, track_numbers, resume_mode, folder_play, replaygain, charge, key_action[3];
    /* counters, for the log and the tests */
    unsigned frames, bad_frames, unknown_tags;

    pl_send_fn send; pl_log_fn log; void *ctx;
} pl_core_t;

void pl_core_init(pl_core_t *c, pl_send_fn send, pl_log_fn log, void *ctx);

/* Handle one frame as received from /player. Returns 0 when it was understood, -1 when it was malformed or unknown
 * (it is logged either way). */
int pl_core_handle(pl_core_t *c, const char *buf, int n);

/* Send the current state as an a2 frame (what 0202 answers). */
void pl_core_send_state(pl_core_t *c);
#endif
