/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* MPD protocol server: the network thread. It accepts up to MPD_CLIENTS clients on one address, frames their lines
 * (mpd_core), answers what needs no player itself, and hands every other command (or command list) to the UI thread,
 * which runs it in msrv_pump through the handler and hands the answer back. idle waits on the event counters the UI
 * thread bumps with msrv_events. Nothing here touches LVGL or the database. */
#ifndef DISKOS_MPD_SRV_H
#define DISKOS_MPD_SRV_H
#include "mpd_core.h"

#define MPD_CLIENTS        8
#define MSRV_CMD_WAIT_MS   20000   /* a library-wide answer can take a while on the Disc */
#define MSRV_IDLE_TIMEOUT  120     /* s without a line from a client that is not in idle */

/* Listen on bind_ip:port (never on every interface; port 0 = any free port, for tests). password: the one clients
 * must give (copied; NULL = none, host tests only). 0 ok, -1 failed. */
int  msrv_start(const char *bind_ip, int port, const char *password);
void msrv_stop(void);
int  msrv_running(void);
int  msrv_port(void);
int  msrv_clients(void);                         /* connected clients now */
void msrv_set_notify(void (*cb)(void));          /* called (any thread) when a batch waits for the UI thread */
void msrv_set_handler(mpd_handler_t h, void *ud);
/* UI thread: run a waiting batch through the handler. 1 = one ran. */
int  msrv_pump(void);
/* UI thread (or any): these subsystems (MPD_EV_* mask) changed; idle clients are told. */
void msrv_events(unsigned mask);
#endif
