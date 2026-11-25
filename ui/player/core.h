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
#define PL_ROUTE_SPDIF 4           /* 0666: S/PDIF (coaxial, 3.5 mm jack): AUX digital on, AUX analog off (the stock
                                    * form; the UI uses 0d01) */
#define PL_VOL_MAX    120
/* 0d05: what connecting and disconnecting does to the outputs (Settings > Audio > Outputs > Auto Outputs) */
#define PL_AUTO_OFF    0           /* nothing: the choice stays as it is; a device that is not on gets a notice */
#define PL_AUTO_NEWEST 1           /* the device just connected becomes the only output (as before 0d05) */
#define PL_AUTO_ALL    2           /* every connected device plays: a plug in AUX, connected headphones Bluetooth */
/* 0d11: what plays when the list ends (the last song of a list that doesn't repeat, the UI's queue included) */
#define PL_END_STOP    0           /* nothing (the stock behaviour) */
#define PL_END_UI      1           /* the UI picks what plays next (Shuffle All, a playlist): the player stops and
                                     * sends ad11 (ui/radio.c radio_end_list_ended) */
#define PL_END_STATION 2           /* the station of 0d12, until something else is played */
#define PL_NOTICE_AUX  1           /* notice: a plug went in, AUX is off (auto off) */
#define PL_NOTICE_BT   2           /* notice: headphones connected, Bluetooth is off (auto off) */
#define PL_NOTICE_NONE 3           /* notice: play was asked, but no output is on (engine.c eng_blocked_seq) */
#define PL_NOTICE_USB  4           /* notice: a USB sound card connected, but USB audio is off (new device, or auto off) */
/* 0d01 bits: the outputs that are on */
#define PL_OUT_AUX     1           /* AUX analog: the DAC on the 3.5 mm and 4.4 mm jacks */
#define PL_OUT_BT      2           /* Bluetooth headphones */
#define PL_OUT_DIG     4           /* AUX digital: S/PDIF, coaxial on the 3.5 mm jack's ring 2 */
#define PL_OUT_USB     8           /* USB audio: a USB sound card (usbaudio.c), the Disc as USB host */
/* Output indexes of 0d09 / 0d0a and the device table */
enum { PL_O_AUX = 0, PL_O_BT, PL_O_DIG, PL_O_USB, PL_O_N };
/* Volumes (docs/USB_AUDIO.md, Jo 2026-11-25): one main volume (core.volume, what the UI shows and the keys move) and
 * per device an offset to it and a cap: an output plays at clamp(main + offset, 0, min(cap, max_volume)). Devices are
 * AUX analog ("aux"), AUX digital ("dig"), each pair of Bluetooth headphones ("bt:<MAC>") and each USB sound card with
 * a serial number ("usb:<vid>:<pid>:<serial>"). A device seen for the first time starts at offset -PL_VOL_MAX (silent
 * whatever the main volume) and switched off; a USB card without a serial number has nothing stored and starts so on
 * every plug in (Jo: concert PA systems, a loud surprise can hurt people). */
#define PL_DEV_MAX     48          /* stored devices; the one not seen for the longest goes first */
#define PL_FADE_MS     4000        /* fade in on switching an output on and on play after a pause (Jo: default 4 s) */
typedef struct {
    char key[100];                 /* "" = free */
    int off, cap, fade;            /* offset -PL_VOL_MAX..PL_VOL_MAX, cap 0..PL_VOL_MAX, fade in 0/1 */
    unsigned used;                 /* last use (core.dev_clock), for dropping the oldest */
} pl_dev_t;

typedef void (*pl_send_fn)(void *ctx, const char *frame, int len);
typedef void (*pl_log_fn)(void *ctx, const char *msg);
/* Transport requests to the audio engine (main.c wires them to engine.c; NULL = no engine, logged only). */
enum { PL_T_TOGGLE = 0, PL_T_NEXT = 1, PL_T_PREV = 2, PL_T_SEEK = 3,
       PL_T_PAUSE = 4,             /* pause if playing (headphones pulled) */
       PL_T_MODE = 5,              /* the play mode changed (arg = new mode) */
       PL_T_STATE = 6,             /* 0202: send the full state if a track is loaded (0), else -1 */
       PL_T_LOVE = 7 };            /* 0104: the song playing into (arg 1) or out of (0) the favourites, then the state */
typedef int (*pl_transport_fn)(void *ctx, int op, long arg);
/* 0100: play list type from 0-based position pos; name is the frame's name field, NUL terminated. 0 or -1. Frames to
 * the UI are the callee's (it sends the track start sequence). */
typedef int (*pl_play_fn)(void *ctx, int type, int pos, const char *name);
/* 0d10: play the station url (name: the station, for the track frame), url "" = stop it. 0 or -1. */
typedef int (*pl_radio_fn)(void *ctx, const char *name, const char *url);
/* 0d13: the UI's queue (Up next), kept in RAM only (Jo, 2026-11-22): SONG.IDs in play order; they play before the
 * list goes on. ack = the number of ad13 starts the UI has seen; party = 1: when they have played the list ends
 * (0d11), it does not go on. 0 or -1. */
#define PL_UP_MAX 400
typedef int (*pl_upnext_fn)(void *ctx, unsigned ack, int party, const int *ids, int n);
/* 0d14: play the stream list the UI wrote to OPL_BC_LIST (Bandcamp, ui/bandcamp.c) from track idx (0-based), in the
 * radio work mode. 0 or -1. */
typedef int (*pl_slist_fn)(void *ctx, int idx);
/* 0d15: play library song SONG.ID now, on its own (no list, nothing written to song.db; Hit&Guess). Repeat One and
 * Repeat All play it again, otherwise the up-next songs (0d13) follow, then the list end (0d11). 0 or -1. */
typedef int (*pl_playsong_fn)(void *ctx, int song_id);

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
    /* Outputs (diskOS, own player only; docs/PLAYER_PROTOCOL.md "Outputs"): MPD plays to every output that is on:
     * AUX analog (out_aux, the DAC), AUX digital (out_dig, S/PDIF) and Bluetooth, any of them or none (playback then
     * waits). A plug going in turns the last AUX choice on (mem_aux / mem_dig) and Bluetooth off; headphones routed by the UI (0666 2 + 06b3) turn
     * Bluetooth on and AUX off; 0d01 sets them by hand. The Disc can't tell a coaxial cable from headphones, so AUX digital comes
     * back with a plug only when it was on before. Each has
     * its own volume: core.volume is the main volume, each output plays at main + its device's offset within its cap
     * (see PL_DEV_MAX above). 0d02/0d03/0d07/0d08 set an output's volume, which is stored as the offset to main. A USB
     * sound card (out_usb, usb_ready: one, the newest plugged) is a fourth output. */
    int out_aux, out_bt, out_dig, out_usb;
    int mem_aux, mem_dig;          /* the last AUX choice with something on (analog, digital or both): a plug going in
                                    * brings it back (Jo 2026-11-25); 1/0 until one is made */
    int vol_aux, vol_bt, vol_dig, vol_usb;   /* what each output plays at: main + offset within cap (pl_core_recalc) */
    int app_aux, app_bt, app_dig, app_usb;   /* what goes to the hardware: vol_* during a fade in less (main.c) */
    int off[PL_O_N], cap[PL_O_N], fade[PL_O_N];   /* the current device of each output (devs[], or a USB card's own) */
    pl_dev_t devs[PL_DEV_MAX];
    unsigned dev_clock;
    int bt_legacy;                 /* the Bluetooth volume of a volumes file from before offsets, for the first
                                    * headphones seen; -1 none */
    int usb_ready;                 /* a USB sound card is there (pl_core_usb) */
    int usb_stored;                /* it has a serial number: its offset and cap are in devs[] */
    char usb_key[100], usb_name[80];
    unsigned vol_seq;              /* bumps when the main volume or a device entry changed (main.c stores them) */
    int bt_ready;                  /* the UI routed to headphones (0666 2) and named them (06b3); 0666 6 (the UI routed
                                    * away, or the headphones' link is gone) clears it. out_bt may stay 1 without it: the
                                    * choice waits for the headphones and the DAC plays meanwhile (playback.c pb_route) */
    int bt_auto;                   /* 0666 2 seen: the next 06b3 turns Bluetooth on and AUX off */
    int codec_want;                /* 0d04: the codec asked for this connection (ui/bt.c numbering), -1 none */
    int auto_out;                  /* 0d05: PL_AUTO_*; PL_AUTO_NEWEST until the UI says */
    int hp_vol_all;                /* 0d06: 1 = the headphones' own volume also moves AUX when it plays too; 0 (default)
                                    * = only the Bluetooth volume */
    int end_mode;                  /* 0d11: PL_END_*; PL_END_STOP until the UI says */
    char end_name[128], end_url[600];   /* 0d12: the station for PL_END_STATION ("" none) */
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
    pl_upnext_fn upnext;           /* set after pl_core_init; NULL = 0d13 is only logged */
    pl_slist_fn slist;             /* set after pl_core_init; NULL = 0d14 is only logged */
    pl_playsong_fn playsong;       /* set after pl_core_init; NULL = 0d15 is only logged */
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
/* Set the main volume to v (0715, keys): every output, on or off, plays at main + its offset. Tells the UI (a714). */
void pl_core_set_volume(pl_core_t *c, int v);
/* The headphones changed their own volume (AVRCP absolute volume, btavrcp.h): Bluetooth plays at v from now on. By
 * default only its offset changes; with hp_vol_all (0d06) the main volume moves instead, so every output follows. */
void pl_core_set_bt_volume(pl_core_t *c, int v);
/* The stored volumes (OPL_VOLUMES): pl_core_vol_parse takes the text (also a file from before offsets: vol_aux= is
 * the main volume, vol_dig= its offset, vol_bt= goes to the first headphones) and tells the UI the main volume;
 * pl_core_vol_format writes it ("main=N" and one "dev KEY off=N cap=N fade=N" line per device). 0 or -1. */
int pl_core_vol_parse(pl_core_t *c, const char *text);
int pl_core_vol_format(const pl_core_t *c, char *buf, unsigned cap);
/* each output's vol_* from main, offset, cap and max volume (called by everything that changes one of them) */
void pl_core_recalc(pl_core_t *c);
/* A USB sound card came (present 1: key from usbaudio.c, stored 1 when it has a serial number, name for the UI) or
 * went (present 0). A card not known before (or without a serial) starts silent and off, with a notice; a known one
 * follows Auto Outputs like a plug. Going away: USB audio off; when it was the only output, pause (a plug in and auto
 * on: AUX takes over). */
void pl_core_usb(pl_core_t *c, int present, const char *key, int stored, const char *name);
/* Tell the UI something about the outputs (PL_NOTICE_*): notice/notice_seq, which the main loop writes to OPL_OUTPUTS. */
void pl_core_notice(pl_core_t *c, int what);

/* Send the current state as an a2 frame (what 0202 answers). */
void pl_core_send_state(pl_core_t *c);
#endif
