/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* What the own player does with each UI command (docs/PLAYER_PROTOCOL.md, column "Step 2"). No I/O of its own: replies
 * go through the send callback and notes through the log callback, so ui/tests can drive it on the host.
 *
 * Playback itself (queue, MPD, DAC) is behind the transport and play callbacks (main.c wires them to playback.c). */
#ifndef DISKOS_PLAYER_CORE_H
#define DISKOS_PLAYER_CORE_H

#define PL_WORK_LOCAL 8            /* 0657 / a607: local playback */
#define PL_ROUTE_DAC  6            /* 0666: internal DAC */
#define PL_ROUTE_BT   2            /* 0666: Bluetooth headphones (the MAC comes with 06b3) */
#define PL_VOL_MAX    120

typedef void (*pl_send_fn)(void *ctx, const char *frame, int len);
typedef void (*pl_log_fn)(void *ctx, const char *msg);
/* Transport requests to the audio engine (main.c wires them to engine.c; NULL = no engine, logged only). */
enum { PL_T_TOGGLE = 0, PL_T_NEXT = 1, PL_T_PREV = 2, PL_T_SEEK = 3,
       PL_T_PAUSE = 4,             /* pause if playing (headphones pulled) */
       PL_T_MODE = 5,              /* the play mode changed (arg = new mode) */
       PL_T_STATE = 6 };           /* 0202: send the full state if a track is loaded (0), else -1 */
typedef int (*pl_transport_fn)(void *ctx, int op, long arg);
/* 0100: play list type from 0-based position pos; name is the frame's name field, NUL terminated. 0 or -1. Frames to
 * the UI are the callee's (it sends the track start sequence). */
typedef int (*pl_play_fn)(void *ctx, int type, int pos, const char *name);

typedef struct {
    /* applied settings (Step 2 "do") */
    int volume, max_volume;        /* 0..120 */
    int balance;                   /* raw 0713 value: 0 centre, 00NN / 01NN one side or the other */
    int gain;                      /* 0 low, 1 high */
    int dac_filter;                /* 0..5 */
    int dre;                       /* 0 on, 1 off */
    int eq_preset;                 /* 0..20 */
    int play_mode;                 /* 0 in order, 1 shuffle, 2 repeat one, 3 repeat all, 4 single */
    int artist_grouping;           /* 0 artist, 1 album artist, -1 not sent yet */
    int work_mode;                 /* last mode set; -1 none yet */
    int route;                     /* 0666 output route; -1 none yet */
    char bt_mac[18];               /* 06b3: the Bluetooth headphones ("AA:BB:CC:DD:EE:FF"), "" none yet */
    int bt_codec;                  /* 06b3 VALUE1: 0 SBC, 1 AAC, 2/3/4 LDAC mobile/standard/high */
    unsigned route_seq;            /* bumps on every accepted 0666 and 06b3, so the main loop switches the output */
    int idle_off_s, sleep_s;       /* 0 off */
    unsigned sleep_seq;            /* bumps on every 0806, so the main loop (re)starts or cancels the timer */
    int jack;                      /* headphone plug: 1 in, 0 out, -1 not known */
    int usb_gadget;                /* 0642: 0 none, 1 USB storage (2 and 5, the USB audio gadgets, not yet) */
    unsigned usb_seq;              /* bumps on every accepted 0642, so the main loop starts or stops the gadget */
    /* remembered only (Step 2 "keep") */
    int gapless, rotation, track_numbers, resume_mode, folder_play, replaygain, charge, key_action[3];
    /* counters, for the log and the tests */
    unsigned frames, bad_frames, unknown_tags;

    pl_send_fn send; pl_log_fn log; void *ctx;
    pl_transport_fn transport;     /* set after pl_core_init; may stay NULL */
    pl_play_fn play;               /* set after pl_core_init; NULL = 0100 is only logged */
} pl_core_t;

void pl_core_init(pl_core_t *c, pl_send_fn send, pl_log_fn log, void *ctx);

/* Handle one frame as received from /player. Returns 0 when it was understood, -1 when it was malformed or unknown
 * (it is logged either way). */
int pl_core_handle(pl_core_t *c, const char *buf, int n);

/* Volume key: one step up (+1) or down (-1), within 0..max_volume; the UI hears it as a714. */
void pl_core_volume_step(pl_core_t *c, int dir);

/* Headphone jack level changed (1 in, 0 out). Unplugging pauses playback, plugging in again leaves it paused (as the
 * stock player; Jo 2026-11-15). The first call only records the level. */
void pl_core_jack(pl_core_t *c, int plugged);

/* Send the current state as an a2 frame (what 0202 answers). */
void pl_core_send_state(pl_core_t *c);
#endif
