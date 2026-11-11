/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Format SD card, device worker (sdformat.c). The UI (system.c) starts it after ui_sd_format_begin() and polls it. */
#ifndef DISKOS_SDFORMAT_H
#define DISKOS_SDFORMAT_H
#include "sdformat_core.h"
#include <stdint.h>

enum { SDFMT_IDLE = 0, SDFMT_RUNNING = 1, SDFMT_DONE = 2 };
enum { SDFMT_PH_UNMOUNT = 1, SDFMT_PH_WRITE = 2, SDFMT_PH_VERIFY = 3 };
typedef struct { int state, phase, permille, elapsed, touched, rc; char err[160]; } sdfmt_status_t;   /* permille -1: no progress (mke2fs) */

int      sdfmt_start(sdf_kind_t kind);       /* 0 = worker started */
int      sdfmt_poll(sdfmt_status_t *s);      /* returns the state; DONE is reported once, then the job is idle again */
uint64_t sdfmt_card_bytes(void);             /* from sysfs; 0 = no card */

/* main.c */
const char *ui_sd_format_blocked(void);      /* NULL = may format now, else the reason */
void ui_sd_format_begin(void);               /* close card access, stop scan/artwork, pause */
void ui_sd_format_abort(void);               /* nothing written, card still mounted: reopen access */
#endif
