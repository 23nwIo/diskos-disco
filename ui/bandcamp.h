/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Bandcamp without an account (docs/BANDCAMP.md): search, an artist's releases, a release's tracks, and playing them
 * in the own player (only that one, Jo 2026-11-22). The Disc switches to source mode 7 ("Bandcamp", the same player work
 * mode as the web radio, 0657 0C), writes the release's streams to own_player.h OPL_BC_LIST and sends 0d14 with the
 * track to start at. Downloads (wget, HTTPS) run on a worker thread; everything else is main thread only. Nothing is
 * written to the internal flash: the list and the downloads live in /tmp. */
#ifndef BANDCAMP_H
#define BANDCAMP_H
#include "bandcamp_core.h"

#define BANDCAMP_SOURCE 7      /* ui_set_source_mode: Bandcamp (player work mode 0C, as the web radio's 6) */

void bandcamp_tick(void);                       /* main.c, every main loop round: finished downloads, player notes */
int  bandcamp_active(void);                     /* 1 while Bandcamp holds the player (source mode 7) */
const char *bandcamp_state(void);               /* "off" | "starting" | "playing" | "error" */
const char *bandcamp_last_error(void);          /* "" none */

/* The searches and pages, for the Remote Control page (and later the Disc). Each starts a download and returns at
 * once: 0 started, -1 refused (*why). A newer one of the same kind replaces a waiting one. */
int  bandcamp_search(const char *q, const char **why);
int  bandcamp_open(const char *url, const char **why);         /* an artist's site, or an album / track page */
/* Play a release (album or track page) from track idx (its index on the page, -1 = the first that plays). The page
 * is downloaded again for fresh stream links unless it was just opened. */
int  bandcamp_play(const char *url, int idx, const char **why);
void bandcamp_stop(void);                       /* back to local playback */
/* All of it as JSON for GET api/bandcamp (malloc'd): {"search":{...},"page":{...},"now":{...}} */
char *bandcamp_json(void);

/* main.c */
void bandcamp_mode_leaving(void);               /* another source takes over */
void bandcamp_restart(void);                    /* the output was re-initialised: start the track again */
void bandcamp_link_failed(int line);            /* the player could not play list line `line` (ad14) */
#endif
