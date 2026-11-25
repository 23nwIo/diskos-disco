/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The ALSA mixer route of the x2000 card, set with the control ioctls directly (no alsa-lib in the player).
 * The stock player routes the PCM it plays to the DAC itself (mixer_set_dma: LO0/LO2/LO3/LO7_MUX). Measured while it
 * played on hw:0,3 (2026-11-15): LO3_MUX = 12 ("LI11"), all other LOx_MUX 0. Without it, after a boot with the own
 * player, the kernel says "ASoC: no backend DAIs enabled for DMA3 playback" and every hw_params fails with EINVAL. */
#ifndef DISKOS_PLAYER_MIXER_H
#define DISKOS_PLAYER_MIXER_H

#define MIX_CTL_DEV   "/dev/snd/controlC0"
#define MIX_DMA3_MUX  "LO3_MUX"
#define MIX_DMA3_DAC  12           /* item "LI11" */

typedef void (*mix_log_fn)(const char *fmt, ...);

/* Set an enumerated mixer control by name (iface MIXER). 0, or -errno. Logs only a change or a failure. */
int mix_set_enum(const char *dev, const char *name, unsigned value, mix_log_fn log);
/* S/PDIF: the stock player plays it on hw:0,4 and sets LO2_MUX = 13 (mq_player set_out_device, out mode 4; read from
 * the binary 2026-11-25, not measured): DMA4 -> the S/PDIF transmitter, as DMA3 = 12 -> the DAC. */
#define MIX_SPDIF_MUX "LO2_MUX"
#define MIX_SPDIF_DMA4 13
static inline int mix_route_spdif(mix_log_fn log){ return mix_set_enum(MIX_CTL_DEV, MIX_SPDIF_MUX, MIX_SPDIF_DMA4, log); }
/* The route for local playback on hw:0,3 (DMA3 -> DAC). */
static inline int mix_route_dac(mix_log_fn log){ return mix_set_enum(MIX_CTL_DEV, MIX_DMA3_MUX, MIX_DMA3_DAC, log); }
#endif
