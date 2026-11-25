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
#define ENG_BT_SCRIPT ENG_RUN_DIR "/bt-out.sh"   /* MPD's Bluetooth pipe command (engine.c) */
#define ENG_BT_DEV    ENG_RUN_DIR "/bt-dev"      /* the headphones' MAC for it, one line */
#define ENG_CA_OWN    OPL_DIR "/cacert.pem"      /* a CA bundle for https streams, used before the system's (engine.c) */

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
    char title[256], artist[160];  /* its Title / Artist tags ("" none); a web radio's ICY StreamTitle is the Title */
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
 * eng_load: replace both slots with path (a file, or an http:// / https:// stream) and start it at start_ms; paused = load only, play starts on eng_toggle (a
 *           resume). 0, or -1 (the file can't be added: missing, not readable, or no engine).
 *           off_ms / len_ms: play only that part of the file (a CUE track; len_ms 0 = to the end; both 0 = all of it);
 *           start_ms, elapsed time and seeks then count from the part's start.
 * eng_set_next: the song to follow the current one (NULL = none, playback stops after it). must_rate (0 = any): it is
 *           queued only when its sample rate is that one. 0 queued, 1 not queued (another rate), -1 not added.
 * eng_advanced: after an event, 1 when MPD has moved on to that next song (it is now the current one; the caller then
 *           sets a new next), else 0. */
/* The outputs MPD plays to, any or both: the DAC (aux), the Bluetooth headphones whose MAC is in ENG_BT_DEV through
 * the pipe output for the link's rate (44100, 48000 or 96000; anything else uses 44100). Outputs that are off are
 * closed first, so calling it again with bt on reopens nothing that stays, but a Bluetooth output that was off is
 * opened anew. Kept over a restart of MPD. 0 or -1. */
int eng_output(int aux, int bt, unsigned rate);
/* AUX plays on S/PDIF (hw:0,4) instead of the DAC; takes effect with the next eng_output */
void eng_set_spdif(int on);
int eng_load(const char *path, long off_ms, long len_ms, long start_ms, int paused);
int eng_set_next(const char *path, long off_ms, long len_ms, unsigned must_rate);
/* Called with the stream's sample rate right before MPD starts a song (opens the PCM): the player starts the DAC. */
typedef void (*eng_prepare_fn)(unsigned rate);
void eng_set_prepare(eng_prepare_fn fn);
int eng_advanced(const eng_status_t *s);
int eng_toggle(void);                  /* play <-> pause; from stop or a paused load: play */
int eng_pause(int on);
int eng_seek_ms(long ms);
int eng_status(eng_status_t *st);
/* The CA bundle MPD's curl input is given for https streams, NULL none found. */
const char *eng_ca_file(void);
/* 1 when OPL_NO_CERT_CHECK came or went since MPD was started (its curl input reads it only at start). */
int eng_cert_stale(void);
/* Stop MPD and start it again with a fresh configuration (its queue is gone; the outputs are kept). 0 or -1. */
int eng_restart(void);
/* Raw command for tests and diagnosis; the answer in out. */
int eng_cmd(const char *cmd, char *out, unsigned cap);
#endif
