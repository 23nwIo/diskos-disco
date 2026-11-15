/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* diskos-player (ui/player, the own player in development), built by the Makefile with the same toolchain and carried
 * inside mq_ui so a signed SD update brings it. The launcher writes it to /usr/data/diskos-player when the own player
 * switch is on (own_player.h). Plain bytes in .rodata, never run from here. */
#include "pl_blob.h"

#ifndef PL_BIN
#define PL_BIN "player/diskos-player"
#endif
__asm__(".section .rodata\n.balign 4\n.global pl_bin_start\npl_bin_start:\n.incbin \"" PL_BIN "\"\n"
        ".global pl_bin_end\npl_bin_end:\n.previous\n");
extern const unsigned char pl_bin_start[], pl_bin_end[];

const unsigned char *pl_blob_data(void){ return pl_bin_start; }
size_t pl_blob_size(void){ return (size_t)(pl_bin_end - pl_bin_start); }
