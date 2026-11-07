/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* MPD server (Settings > Connectivity > Remote Control > MPD Server): lets MPD clients such as mpc and ncmpcpp control
 * the Disc over Wi-Fi. Off by default; the password is the Remote Control link's token. See docs/MPD.md. */
#ifndef DISKOS_MPD_H
#define DISKOS_MPD_H

#define MPD_CFG_ON "mpd_on"     /* 0/1, default 0 (off) */

void mpd_boot(void);            /* after remote_boot: the 1 s watcher (the server comes up with Wi-Fi) */
void mpd_pump(void);            /* main loop, every pass: run a waiting command, tell idle clients what changed */
void mpd_set_on(int on);        /* Settings toggle (the cfg value is already saved) */
void mpd_show_access(void);     /* the address and password overlay */
int  mpd_access_shown(void);    /* that overlay is up (keeps the screen lit) */
void mpd_token_changed(void);   /* Remote Control made a new token: restart with the new password */
/* host tests only: run one request line (or several, '\n'-separated, as a command list) through the real handler */
char *mpd_test_request(const char *lines);
void mpd_test_access(void);
void mpd_test_serve(int port, int seconds);   /* host tests: real clients against the real handler */     /* host renders: the overlay without Wi-Fi */
#endif
