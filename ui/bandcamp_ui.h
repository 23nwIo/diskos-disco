/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Disc's Bandcamp screen (bandcamp_ui.c, SCR_BANDCAMP). Main thread only. */
#ifndef BANDCAMP_UI_H
#define BANDCAMP_UI_H
#include "lvgl/lvgl.h"
void bandcamp_ui_create(lv_obj_t *root);    /* screens_init */
void bandcamp_ui_open(void);                /* the app "Bandcamp": its first view */
int  bandcamp_ui_back_consumed(void);       /* one view up; 0 at the first */
#endif
