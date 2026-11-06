/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef FOLDERBROWSER_H
#define FOLDERBROWSER_H
#include "lvgl/lvgl.h"
/* File/folder browser (SCR_FOLDER): a browsable directory tree of the SD card
 * (mounted at /tmp/sdcard). Tap a folder to descend; the header back-chevron
 * ascends one level (and leaves the screen at the SD root). Tap an audio file
 * to play that track. See folderbrowser.c for the honest limitation on
 * whole-folder queue playback. */
void folderbrowser_create(lv_obj_t *root);   /* build the screen once (from screens_init) */
void folderbrowser_open(void);               /* reset to the SD root + show SCR_FOLDER */
int  folderbrowser_pick_back(void);         /* picking: stop picking (1), else 0 */
void folderbrowser_pick_demo(const char *csv); /* renderer: tick these entries */
int  folderbrowser_is_audio(const char *name);  /* a file Files lists and plays (the scanner's music set) */
#define FOLDERBROWSER_ROOT "/tmp/sdcard"
#endif
