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
const char *radio_last_error(void);                     /* "" none */

void radio_mode_leaving(void);              /* main.c ui_set_source_mode: another source takes over (no mode switch here) */
#endif
