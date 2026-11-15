/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The CS43131 DAC pair, driven as the stock player drives it (docs/MQ_PLAYER_TRACE.md runs 1-3, docs/MPD_CORE.md):
 * A = /dev/cs43131 (right channel), C = /dev/cs43131c (left channel). Both stay open while the player runs: closing
 * them switches the DAC off again (device test 2026-11-15). The pure parts (rate code, attenuation) are separate so
 * ui/tests can check them on the host. */
#ifndef DISKOS_PLAYER_DAC_H
#define DISKOS_PLAYER_DAC_H

typedef void (*dac_log_fn)(const char *fmt, ...);

/* M52/M54 rate code for a sample rate in Hz: 1 = 44.1 kHz, 2 = 48, 4 = 96, 6 = 192 (device-checked, run 3);
 * 3 = 88.2 and 5 = 176.4 follow the pattern (unverified). 0 for a rate the DAC does not take. */
int dac_rate_code(unsigned rate_hz);

/* The stock volume byte for volume 0..120 at high gain on the 3.5 mm jack (0xff = muted at 0). */
int dac_curve(int volume);
/* The attenuation bytes for A (right) and C (left). volume 0..120 (UI scale), gain 0 low / 1 high (pass 0 when no
 * plug is in, as the stock player does), balanced 1 when the 4.4 mm plug is in, balance as 0713 sends it (0 centre,
 * 0x00NN / 0x01NN = NN steps towards one side). The curves are the stock player's (dac.c). 0xff = muted. */
void dac_attenuation(int volume, int gain, int balanced, int balance, unsigned char *a_right, unsigned char *c_left);

/* Open both devices (once); 0 or -1. dry_run: log only. */
int dac_open(int dry_run, dac_log_fn log);
void dac_close(void);
/* The stock start sequence for a stream at rate_hz with the given attenuation (see docs/MPD_CORE.md), ending with
 * _IO('M',46); the first call also sends _IO('M',48). */
int dac_start(unsigned rate_hz, unsigned char a_right, unsigned char c_left, dac_log_fn log);
/* Write the attenuation bytes (volume group M45/M66 on A, M47/M68 on C). */
void dac_set_attenuation(unsigned char a_right, unsigned char c_left, dac_log_fn log);
/* The rate the DAC was last started with, 0 if never. */
unsigned dac_current_rate(void);
#endif
