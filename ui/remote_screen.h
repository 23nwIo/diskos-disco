/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote Control: the Screen tab (GET api/screen, POST api/touch). Main thread only. */
#ifndef DISKOS_REMOTE_SCREEN_H
#define DISKOS_REMOTE_SCREEN_H
#include <stddef.h>
/* The screen as it looks now, upright: a 24-bit BMP (malloc'd, *len bytes), or NULL with *why. */
char *rscr_bmp(size_t *len, const char **why);
/* Play a finger on the touch panel: down at (x1,y1), moving to (x2,y2) over ms, then up (logical screen pixels).
 * A tap is x1,y1 == x2,y2 with a short ms; a long press is the same with a long ms. 0 queued, -1 refused with *why. */
int rscr_touch(int x1, int y1, int x2, int y2, int ms, const char **why);
#endif
