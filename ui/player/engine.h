/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The audio engine: MPD as a child process (docs/MPD_CORE.md). The player owns the DAC; MPD only decodes and writes
 * hw:0,3. Commands go over one socket connection, MPD's changes come back on a second one that sits in "idle" and is
 * polled by the main loop (no status polling). */
#ifndef DISKOS_PLAYER_ENGINE_H
#define DISKOS_PLAYER_ENGINE_H
#include "../own_player.h"

#define ENG_MPD_BIN   OPL_DIR "/mpd"           /* put there by hand for now (docs/MPD_CORE.md) */
#define ENG_RUN_DIR   "/tmp/diskos-mpd"
#define ENG_SOCK      ENG_RUN_DIR "/sock"
#define ENG_CONF      ENG_RUN_DIR "/mpd.conf"

typedef void (*eng_log_fn)(const char *fmt, ...);

enum { ENG_STOP = 0, ENG_PLAY, ENG_PAUSE };
typedef struct {
    int state;                     /* ENG_* */
    long elapsed_ms, duration_ms;
    int song;                      /* MPD queue position, -1 none */
    int songid;                    /* MPD song id, -1 none */
    int bits, channels, kbps;      /* of the decoded stream, 0 unknown */
    int songs;                     /* queue length */
    unsigned rate;                 /* sample rate of the current stream, 0 unknown */
    char file[1100];               /* current song's path (file:// stripped) */
} eng_status_t;

/* Start MPD (if its binary exists) and connect. 0, or -1 when there is no engine (the player then runs silent). */
int eng_start(int dry_run, eng_log_fn log);
void eng_stop(void);
/* The idle connection's descriptor for poll, -1 when not connected. */
int eng_idle_fd(void);
/* Call when eng_idle_fd is readable: returns the MC_EV_* mask (0 none) and re-arms the idle; -1 lost connection. */
int eng_idle_event(void);
/* Call once per main loop turn: restarts MPD if it died (a few times per boot). */
void eng_check(void);
int eng_ready(void);

/* MPD holds the current song and the next one (for gapless); the queue is the player's (queue.c).
 * eng_load: replace both slots with path and start it at start_ms; paused = load only, play starts on eng_toggle (a
 *           resume). 0, or -1 (the file can't be added: missing, not readable, or no engine).
 * eng_set_next: the song to follow the current one (NULL = none, playback stops after it). 0 or -1 (not added).
 * eng_advanced: after an event, 1 when MPD has moved on to that next song (it is now the current one; the caller then
 *           sets a new next), else 0. */
int eng_load(const char *path, long start_ms, int paused);
int eng_set_next(const char *path);
int eng_advanced(const eng_status_t *s);
int eng_toggle(void);                  /* play <-> pause; from stop or a paused load: play */
int eng_pause(int on);
int eng_seek_ms(long ms);
int eng_status(eng_status_t *st);
/* Raw command for tests and diagnosis; the answer in out. */
int eng_cmd(const char *cmd, char *out, unsigned cap);
#endif
