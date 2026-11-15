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
/* From bluealsa-cli's "info" text (pure, for the tests): the first word after "Selected codec:" (or "Codec:"), upper
 * case ("LDAC"; "" if none), and the codecs on the "Available codecs:" line and the lines after it without a ':' as
 * one line of upper-case words ("SBC AAC LDAC"; "" if none). */
void bto_parse_selected(const char *info, char *out, size_t cap);
void bto_parse_avail(const char *info, char *out, size_t cap);
/* The link now (bluealsa-cli info): its rate (0 unknown), its codec and the codecs the headphones offer (as the
 * parsers above; NULL to skip). 0, or -1 when there is no PCM yet or bluealsa-cli failed (logged). */
int bto_info(const char *mac, unsigned *rate, char *sel, size_t selcap, char *avail, size_t availcap, bto_log_fn log);
/* The link's rate now (bto_info), 0 if unknown. */
unsigned bto_rate(const char *mac, bto_log_fn log);
/* The headphones' volume for the player volume 0..max (0..127 in BlueALSA). 0 or -1 (logged). */
int bto_volume(const char *mac, int volume, int max, bto_log_fn log);
/* Ask BlueALSA for the codec of 06b3 VALUE1 (1 AAC, 2..4 LDAC; 0 SBC), in the background: a codec change
 * renegotiates the link and takes a moment. Its output goes to the engine's bt.log. */
void bto_codec(const char *mac, int value, bto_log_fn log);
/* The codec name BlueALSA takes for a ui/bt.c codec number: 0 SBC, 1 AAC, 2..4 LDAC, 5 aptX, 6 aptX-HD. */
const char *bto_codec_name(int value);
/* Switch the codec now (0d04): runs bluealsa-cli codec as a child and returns its pid (the caller waits for it with
 * waitpid), or -1. Its output goes to the engine's bt.log. */
int bto_codec_start(const char *mac, int value, bto_log_fn log);
#endif
