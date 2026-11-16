/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The songs played, one line each, in /tmp (RAM: gone after a reboot, no flash writes; Jo 2026-11-16) - for "what
 * was that song a minute ago?". With the stock and the own player: main.c feeds it the track state and the Radio
 * app's state. Rotated by main.c's log upkeep (2 MB). Main thread only. */
#ifndef DISKOS_PLAYLOG_H
#define DISKOS_PLAYLOG_H
#define PLAYLOG_PATH "/tmp/diskos-played.log"
void playlog_set_path(const char *path);   /* tests */
/* A song plays (once per song: path, list position and title; the station after the list as stream = 1, each new
 * stream title once) */
void playlog_song(const char *path, int pos_id, const char *title, const char *artist, const char *album, int stream);
/* The Radio app plays `station`; title = the stream's song ("Artist - Title", "" none yet): each change once */
void playlog_radio(const char *station, const char *title);
/* Nothing plays any more: the same song played again is a new line */
void playlog_reset(void);
#endif
