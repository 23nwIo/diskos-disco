/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote control over Wi-Fi (Settings > Connectivity > Remote Control): the UI side. See remote_core.h for the API. */
#ifndef DISKOS_REMOTE_H
#define DISKOS_REMOTE_H

#define REMOTE_CFG_ON    "remote_on"      /* 0/1, default 0 (off) */
#define REMOTE_CFG_TOKEN "remote_token"   /* the secret path segment; made on first use, replaced by New Link */

void remote_boot(void);          /* after screens_init: starts the 1 s watcher (the server comes up with Wi-Fi) */
void remote_pump(void);          /* main loop, every pass: publish the snapshot, run a waiting command */
void remote_set_on(int on);      /* Settings toggle (the cfg value is already saved) */
void remote_show_link(void);     /* QR + address overlay */
void remote_new_link(void);      /* confirm, then a new token (old links stop working) */
int  remote_link_shown(void);    /* the overlay is up (keeps the screen lit) */
void remote_test_link(const char *url);   /* host renders only */
#endif
