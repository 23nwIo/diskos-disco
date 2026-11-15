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
/* Power off or an orderly stop: store the position in MEMORY_PLAY. */
void pb_shutdown(void);
/* The last engine status and when it was read (CLOCK_MONOTONIC seconds); NULL before the first. */
const eng_status_t *pb_status(double *when);
#endif
