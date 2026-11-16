/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Web radio (radio.c): the Radio app and its player link. The stock mq_player plays the stream itself: the Disc
 * switches it to its DLNA work mode (source mode 6, frame 0657000C000C) and hands it the station over UPnP on the
 * device's own address, as any DLNA controller would. Everything here is main-thread only (LVGL thread). */
#ifndef RADIO_H
#define RADIO_H
#include "lvgl/lvgl.h"
#include "radio_core.h"

void radio_create(lv_obj_t *root);          /* screens_init */
void radio_open(void);                      /* the Radio screen (favourites) */
int  radio_back_consumed(void);             /* search results -> favourites */

/* API for the Radio screen and other front ends (e.g. Remote Control). */
int  radio_favorites(rc_station_t *out, int cap);       /* the saved stations, in order; returns count */
int  radio_play(const rc_station_t *s);                 /* 0 start queued (async), -1 refused (reason toasted, radio_last_error) */
int  radio_play_url(const char *name, const char *url); /* the same for a bare URL (http/https, rc_url_ok) */
void radio_stop(void);                                  /* stop the stream and return to local playback */
int  radio_active(void);                                /* 1 while the radio holds the player (source mode 6) */
const char *radio_state(void);                          /* "off" | "starting" | "playing" | "error" */
const char *radio_station_name(void);                   /* the current station, "" none */
int  radio_station(rc_station_t *out);                  /* the current station with codec and bitrate; 0 none */
const char *radio_last_error(void);                     /* "" none */
const char *radio_stream_title(void);                   /* the song the stream names ("Artist - Title"), "" none / not yet */
/* The Radio screen's directory search, for other front ends: the same worker, download and parser. One search runs at
 * a time (a newer one replaces a waiting one); the Disc's Results view shows whichever finished last. */
int  radio_search(const char *query);                   /* "" = popular; 0 started, -1 no Wi-Fi */
int  radio_search_result(rc_station_t *out, int cap, int *state);   /* stations copied; state 0 idle 1 busy 2 done 3 failed */
int  radio_fav_add(const rc_station_t *s);              /* 0 saved (or already there), -1 refused (radio_last_error) */
int  radio_fav_remove(const char *url);                 /* 0 removed (or not there), -1 couldn't save */
int  radio_fav_count(void);                             /* the saved stations (up to RC_MAX_FAV) */
int  radio_fav_get(int i, rc_station_t *out);           /* favourite i (0-based); 0, -1 no such */
int  radio_fav_has(const char *url);                    /* 1 = url is a favourite */
int  radio_fav_nocard(void);                            /* 1 = the list is kept on the SD card and no card is in */
/* Settings > System > Library > Radio Favourites changed to the card (sd 1) or the player (sd 0); the setting is
 * already saved; nothing is copied. 0 done, 1 done with a note in *why (no card). */
int  radio_fav_move(int sd, const char **why);

/* After the list ends (own player only; docs/MPD_CORE.md "After the list ends"): what plays when the last song of a
 * list - the queue included - has ended. mode: RADIO_END_STOP, RADIO_END_SHUFFLE (every song, shuffled),
 * RADIO_END_STATION (the station named here; by default the last one played) or RADIO_END_PLAYLIST (the playlist of
 * radio_end_set_playlist). Kept in diskos.conf (end_mode, end_st_name, end_st_url, end_pl_id, end_pl_flags); the
 * player gets 0d11 and 0d12. All main thread only. */
#define RADIO_END_STOP     0
#define RADIO_END_SHUFFLE  1
#define RADIO_END_STATION  2
#define RADIO_END_PLAYLIST 3
#define RADIO_END_PL_REPEAT  1    /* radio_end_set_playlist flags: play it again and again (Repeat All) */
#define RADIO_END_PL_SHUFFLE 2    /* in the Shuffle play mode (which goes on endlessly, Repeat or not) */
int  radio_end_set(int mode, const rc_station_t *station);   /* mode -1 / station NULL: keep it; 0, -1 bad value */
int  radio_end_get(rc_station_t *station);                    /* the mode; the station into *station (may be NULL) */
int  radio_end_set_playlist(long pid, int flags);             /* the playlist (its id, > 0) for RADIO_END_PLAYLIST; 0, -1 bad */
long radio_end_playlist(int *flags);                          /* its id (0 none); the flags into *flags (may be NULL) */
void radio_end_sync(void);                                    /* tell the own player (at its start, after a change) */
void radio_end_list_ended(void);                              /* main.c: the player said the list ended (ad11) */
void radio_restart(void);                   /* main.c: the output was re-initialised (Bluetooth -> headphones): start the station again */
void radio_mode_leaving(void);              /* main.c ui_set_source_mode: another source takes over (no mode switch here) */
#endif
