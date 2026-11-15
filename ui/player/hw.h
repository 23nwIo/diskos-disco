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
#endif
