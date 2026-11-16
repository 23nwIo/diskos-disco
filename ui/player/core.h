/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* What the own player does with each UI command (docs/PLAYER_PROTOCOL.md, column "Step 2"). No I/O of its own: replies
 * go through the send callback and notes through the log callback, so ui/tests can drive it on the host.
 *
 * Playback itself (queue, MPD, DAC) is behind the transport and play callbacks (main.c wires them to playback.c). */
#ifndef DISKOS_PLAYER_CORE_H
#define DISKOS_PLAYER_CORE_H

#define PL_WORK_LOCAL 8            /* 0657 / a607: local playback */
#define PL_WORK_DLNA  12           /* 0657 / a607: the stock player's DLNA renderer (0C); the Radio app's mode, in which
                                    * the own player takes the station with 0d10 */
#define PL_ROUTE_DAC  6            /* 0666: internal DAC */
#define PL_ROUTE_BT   2            /* 0666: Bluetooth headphones (the MAC comes with 06b3) */
#define PL_VOL_MAX    120
/* 0d05: what connecting and disconnecting does to the outputs (Settings > Audio > Outputs > Auto Outputs) */
#define PL_AUTO_OFF    0           /* nothing: the choice stays as it is; a device that is not on gets a notice */
#define PL_AUTO_NEWEST 1           /* the device just connected becomes the only output (as before 0d05) */
#define PL_AUTO_ALL    2           /* every connected device plays: a plug in AUX, connected headphones Bluetooth */
#define PL_NOTICE_AUX  1           /* notice: a plug went in, AUX is off (auto off) */
#define PL_NOTICE_BT   2           /* notice: headphones connected, Bluetooth is off (auto off) */

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
/* 0d10: play the station url (name: the station, for the track frame), url "" = stop it. 0 or -1. */
typedef int (*pl_radio_fn)(void *ctx, const char *name, const char *url);

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
    unsigned mode_seq;             /* bumps on every accepted work mode change, so the main loop starts or ends the
                                    * DLNA renderer (playback.c pb_stream_mode) */
    int route;                     /* 0666 output route; -1 none yet */
    char bt_mac[18];               /* 06b3: the Bluetooth headphones ("AA:BB:CC:DD:EE:FF"), "" none yet */
    int bt_codec;                  /* 06b3 VALUE1: 0 SBC, 1 AAC, 2/3/4 LDAC mobile/standard/high */
    unsigned route_seq;            /* bumps on every accepted 0666, 06b3 and 0d01 and when a plug turns an output on
                                    * or off, so the main loop switches MPD's outputs (playback.c pb_route) */
    int idle_off_s, sleep_s;       /* 0 off */
    unsigned sleep_seq;            /* bumps on every 0806, so the main loop (re)starts or cancels the timer */
    int jack;                      /* 3.5 mm plug: 1 in, 0 out, -1 not known */
    int bal;                       /* 4.4 mm balanced plug: 1 in, 0 out, -1 not known */
    /* Outputs (diskOS, own player only; docs/PLAYER_PROTOCOL.md "Outputs"): MPD plays to every output that is on. A
     * plug going in turns AUX on and Bluetooth off; headphones routed by the UI (0666 2 + 06b3) turn Bluetooth on and
     * AUX off; 0d01 sets both by hand. Each has its own volume; core.volume (what the UI shows) is the lead output's:
     * Bluetooth when it is on, else AUX. The volume keys and 0715 move every output that is on by the same step. */
    int out_aux, out_bt;
    int vol_aux, vol_bt;           /* 0..max_volume */
    int bt_ready;                  /* the UI routed to headphones (0666 2) and named them (06b3); 0666 6 (the UI routed
                                    * away, or the headphones' link is gone) clears it. out_bt may stay 1 without it: the
                                    * choice waits for the headphones and the DAC plays meanwhile (playback.c pb_route) */
    int bt_auto;                   /* 0666 2 seen: the next 06b3 turns Bluetooth on and AUX off */
    int codec_want;                /* 0d04: the codec asked for this connection (ui/bt.c numbering), -1 none */
    int auto_out;                  /* 0d05: PL_AUTO_*; PL_AUTO_NEWEST until the UI says */
    int notice;                    /* PL_NOTICE_* of the last notice, for the UI (OPL_OUTPUTS "notice=") */
    unsigned notice_seq;           /* bumps on every notice */
    unsigned codec_seq;            /* bumps on every accepted 0d04 */
    int usb_gadget;                /* 0642: 0 none, 1 USB storage (2 and 5, the USB audio gadgets, not yet) */
    unsigned usb_seq;              /* bumps on every accepted 0642, so the main loop starts or stops the gadget */
    /* remembered only (Step 2 "keep") */
    int gapless, rotation, track_numbers, resume_mode, folder_play, replaygain, charge, key_action[3];
    /* counters, for the log and the tests */
    unsigned frames, bad_frames, unknown_tags;

    pl_send_fn send; pl_log_fn log; void *ctx;
    pl_transport_fn transport;     /* set after pl_core_init; may stay NULL */
    pl_play_fn play;               /* set after pl_core_init; NULL = 0100 is only logged */
    pl_radio_fn radio;             /* set after pl_core_init; NULL = 0d10 is only logged */
} pl_core_t;

void pl_core_init(pl_core_t *c, pl_send_fn send, pl_log_fn log, void *ctx);

/* Handle one frame as received from /player. Returns 0 when it was understood, -1 when it was malformed or unknown
 * (it is logged either way). */
int pl_core_handle(pl_core_t *c, const char *buf, int n);

/* Volume key: one step up (+1) or down (-1), within 0..max_volume; the UI hears it as a714. */
void pl_core_volume_step(pl_core_t *c, int dir);

/* Headphone jack level changed (1 in, 0 out): pl_core_jack for the 3.5 mm jack, pl_core_bal for the 4.4 mm one. The
 * first call of each only records the level. A plug going in (none was in) turns AUX on (auto newest: and Bluetooth
 * off; auto off: nothing, a notice if AUX is off); playback stays as it is (paused stays paused, as the stock player;
 * Jo 2026-11-15). The last plug coming out pauses playback unless connected headphones play: then, unless auto is
 * off, AUX goes off and the music goes on. */
void pl_core_jack(pl_core_t *c, int plugged);
void pl_core_bal(pl_core_t *c, int plugged);
/* 1 when a plug is in either jack (the stock player uses the high gain curve only then) */
int pl_core_plugged(const pl_core_t *c);
/* Set the volume shown in the UI to v (0715, keys): every output that is on moves by the same step, each within
 * 0..max_volume. Tells the UI (a714) when the lead volume changed. */
void pl_core_set_volume(pl_core_t *c, int v);
/* The headphones changed their own volume (AVRCP absolute volume, btavrcp.h): the Bluetooth volume becomes v (within
 * 0..max_volume); the UI hears it (a714) when Bluetooth is the lead output. AUX is left alone. */
void pl_core_set_bt_volume(pl_core_t *c, int v);
/* Set both volumes as stored (at start); the UI is told the lead one. */
void pl_core_load_volumes(pl_core_t *c, int vol_aux, int vol_bt);

/* Send the current state as an a2 frame (what 0202 answers). */
void pl_core_send_state(pl_core_t *c);
#endif
