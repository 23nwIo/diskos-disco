/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Playback: ties the queue (queue.c, the LIST_SONG tables via queuedb.c) to the audio engine (engine.c, MPD) and the
 * DAC (dac.c), and tells the UI what plays (docs/PLAYER_PROTOCOL.md, "Measured on a live stream"):
 *   - 0100 builds the queue like the stock player and starts the song at the given position;
 *   - Next/Previous and the end of a song move through the queue by the play mode (0102);
 *   - every track start sends the full a2 frame (song JSON with pos_id = LIST_SONG_0.ID), play/pause the partial
 *     state frames, a1 the position once a second while playing (main.c).
 * MPD holds only the current song and the next one, so a big library queue never has to be copied into it. */
#ifndef DISKOS_PLAYER_PLAYBACK_H
#define DISKOS_PLAYER_PLAYBACK_H
#include "core.h"
#include "engine.h"
#include <stddef.h>

typedef void (*pb_log_fn)(const char *fmt, ...);

/* song_db / sysconfig_db: the database paths (QD_SONG_DB, QD_SYSCONFIG_DB on the Disc). The core's send callback
 * carries the frames to the UI. */
void pb_init(pl_core_t *c, const char *song_db, const char *sysconfig_db, pb_log_fn log);
void pb_close(void);

/* 0100: build the queue for (type, name) and play from 0-based position pos (type 0: jump to pos in the current
 * queue). 0, or -1 (nothing plays). */
int pb_play_list(int type, int pos, const char *name);
/* 0201 / 0103: PL_T_TOGGLE, PL_T_NEXT, PL_T_PREV, PL_T_SEEK (arg = ms). */
int pb_transport(int op, long arg);
/* Pause (the headphones were pulled). */
void pb_pause(void);
/* The play mode (0102) changed: rebuild or drop the shuffle order and the queued next song. */
void pb_mode_changed(void);
/* MPD reported a change: track start, play/pause, a new stream rate. */
void pb_engine_event(void);
/* Once per main loop turn: a1 while playing, and a track announcement whose stream format did not come in time. */
void pb_tick(double now);
/* Resume at start (the stock player's "memory play"): once the card has been mounted for a second and MPD is up,
 * play the song in MEMORY_PLAY from the queue in LIST_SONG_0 (and LIST_SONG_3 in shuffle) - at the remembered
 * position with resume mode 1 (0684), from its start with mode 2, not at all with 0. Given up after 30 s without a
 * card, or when something was played first. mounted() says whether the card is mounted (NULL: always).
 * flag_path: created while a resumed song has not played 10 s yet, removed then and by pb_shutdown; found at the
 * next start, it means the last resume crashed or hung the player, and that resume is skipped (NULL: no check). */
void pb_resume_begin(int (*mounted)(void), const char *flag_path);
/* The outputs changed (core out_aux / out_bt / route / bt_mac / bt_codec): MPD plays to the DAC, the Bluetooth
 * headphones or both (engine.c eng_output, btout.c). pb_bluetooth / pb_aux: 1 while playing to Bluetooth / the DAC;
 * pb_bt_mac: the headphones' MAC; pb_bt_codec / pb_bt_codecs: the link's codec and the ones the headphones offer
 * ("LDAC", "SBC AAC LDAC"; "" when not on Bluetooth). */
void pb_route(void);
int pb_bluetooth(void);
int pb_aux(void);
const char *pb_bt_mac(void);
const char *pb_bt_codec(void);
const char *pb_bt_codecs(void);
/* 0d04: switch the Bluetooth codec of this connection now (ui/bt.c numbering). The music pauses for it and plays on
 * once BlueALSA is done (pb_tick); pb_switching is 1 meanwhile. */
void pb_codec(int value);
int pb_switching(void);
/* The DAC's attenuation bytes for the AUX volume, gain (only with a plug in), the 4.4 mm curve and the balance. */
void pb_attenuation(unsigned char *a_right, unsigned char *c_left);
/* Power off or an orderly stop: store the position in MEMORY_PLAY. */
void pb_shutdown(void);
/* Web radio: the DLNA work mode (0657 0C, core mode_seq), driven by the renderer in upnp.c. pb_stream_mode(1) pauses
 * the local song and keeps its place; pb_stream_mode(0) stops the station and loads the local song again, paused where
 * it was. pb_stream_set takes the station (http/https URL, title), pb_stream_play starts it in MPD (or plays on after
 * a pause); pb_stream_state is the UPnP transport state ("PLAYING", "PAUSED_PLAYBACK", "TRANSITIONING", "STOPPED"). */
void pb_stream_mode(int on);
int  pb_stream_active(void);
int  pb_stream_set(const char *uri, const char *title);
int  pb_stream_play(void);
int  pb_stream_pause(void);
int  pb_stream_stop(void);
const char *pb_stream_state(void);
const char *pb_stream_uri(void);
/* upnp.c's action callback (up_action_fn) for the Radio app's renderer requests */
int pb_upnp_action(void *ctx, const char *action, const char *body, char *out, size_t cap);
/* The last engine status and when it was read (CLOCK_MONOTONIC seconds); NULL before the first. */
const eng_status_t *pb_status(double *when);
#endif
