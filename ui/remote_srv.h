/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote control over Wi-Fi: the HTTP server thread (sockets + pthreads, no LVGL), host-tested in tests/remote_test.c.
 *
 * Threading: the server thread never touches the UI or the player. The main thread publishes a playback snapshot
 * (rsrv_publish) and runs commands it takes from a one-slot mailbox (rsrv_take, then rsrv_done with the result);
 * the request that posted the command waits up to RSRV_CMD_WAIT_MS for that result. One client is served at a time,
 * with per-socket and per-request deadlines, so a slow or stuck client cannot pile up threads or memory. */
#ifndef DISKOS_REMOTE_SRV_H
#define DISKOS_REMOTE_SRV_H
#include "remote_core.h"

#define RSRV_CMD_WAIT_MS 3000

/* Listen on bind_ip:port (dotted quad, never "any") under the given token. 0 ok, -1 failed (nothing left running). */
int  rsrv_start(const char *bind_ip, int port, const char *token);
void rsrv_stop(void);                     /* joins the thread; safe when not running */
int  rsrv_running(void);
int  rsrv_port(void);                     /* the bound port (useful with port 0 in tests) */

void rsrv_publish(const rc_state_t *s);   /* main thread: the latest snapshot (strings copied) */
void rsrv_set_notify(void (*cb)(void));  /* called on the server thread when a command waits or state is asked for */
int  rsrv_take(rc_cmd_t *out);            /* main thread: 1 = a command is waiting (now taken), 0 = none */
void rsrv_done(int ok, const char *message);   /* main thread: result of the taken command (message: why not, or NULL) */
#endif
