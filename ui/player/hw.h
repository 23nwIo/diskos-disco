/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Hardware actions of the own player that do not depend on the audio engine. Values from the stock player's trace
 * (docs/MQ_PLAYER_TRACE.md, third run). */
#ifndef DISKOS_PLAYER_HW_H
#define DISKOS_PLAYER_HW_H

typedef void (*hw_log_fn)(const char *fmt, ...);

/* The stock player's power off routine (menu power off / sleep timer). dry_run: only log each step. Does not return
 * unless dry_run is set or poweroff itself fails. wd_fd: the armed watchdog (or -1), stopped before the end. */
void hw_power_off(int wd_fd, int dry_run, hw_log_fn log);
/* Power key short press: the screen off or on, as the stock player does it (backlight, touch controller, panel). The
 * UI follows the backlight (ui/main.c, PW-10) and puts its own brightness back on. Returns 1 = now on, 0 = now off. */
int hw_screen_toggle(int dry_run, hw_log_fn log);

/* Keys of /dev/input/event0 ("x2000_key", device-checked 2026-11-15). The driver reports clicks: press and release
 * arrive together, and a held Power key comes as its own code. */
#define KEY_DEV        "/dev/input/event0"
#define KEYC_POWER     265
#define KEYC_POWER_ALT 259               /* every second short press comes as 259 (device log 2026-11-15) */
#define KEYC_POWER_LONG 264
#define KEYC_VOL_UP    251
#define KEYC_VOL_DOWN  252
#define KEYC_PLAY      250
#endif
