/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The Bluetooth headphones' side of playing to Bluetooth, through the device's BlueALSA (started by the UI, ui/bt.c):
 * the link's sample rate, the volume, the codec. MPD's Bluetooth output itself is in engine.c. All of it goes through
 * bluealsa-cli, as the stock player does (`strings mq_player`: "bluealsa-cli list-pcms", "bluealsa-cli info %s"). */
#ifndef DISKOS_PLAYER_BTOUT_H
#define DISKOS_PLAYER_BTOUT_H
#include <stddef.h>

typedef void (*bto_log_fn)(const char *fmt, ...);

/* BlueALSA's A2DP PCM of the headphones mac ("AA:BB:CC:DD:EE:FF"): "/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink".
 * 0, or -1 when mac is not a MAC. */
int bto_pcm_path(const char *mac, char *out, size_t cap);
/* The sample rate in bluealsa-cli's "info" text ("Sampling: 44100 Hz" or "Rate: 44100"), 0 if none. Pure, for the tests. */
unsigned bto_parse_rate(const char *info);
/* The link's rate now (bluealsa-cli info), 0 if unknown (no PCM yet, or bluealsa-cli failed; logged). */
unsigned bto_rate(const char *mac, bto_log_fn log);
/* The headphones' volume for the player volume 0..max (0..127 in BlueALSA). 0 or -1 (logged). */
int bto_volume(const char *mac, int volume, int max, bto_log_fn log);
/* Ask BlueALSA for the codec of 06b3 VALUE1 (1 AAC, 2..4 LDAC; 0 SBC), in the background: a codec change
 * renegotiates the link and takes a moment. Its output goes to the engine's bt.log. */
void bto_codec(const char *mac, int value, bto_log_fn log);
#endif
