/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* diskos-watchdog (ui/watchdog, MIT), built by the Makefile with the same toolchain and carried inside mq_ui so a signed
 * SD update brings it. main.c writes it to /usr/data and binds it over the stock cmd_watchdog (wd_override.h). Plain
 * bytes in .rodata, never run from here. */
#include "wd_blob.h"

#ifndef WD_BIN
#define WD_BIN "watchdog/diskos-watchdog"
#endif
__asm__(".section .rodata\n.balign 4\n.global wd_bin_start\nwd_bin_start:\n.incbin \"" WD_BIN "\"\n"
        ".global wd_bin_end\nwd_bin_end:\n.previous\n");
extern const unsigned char wd_bin_start[], wd_bin_end[];

const unsigned char *wd_blob_data(void){ return wd_bin_start; }
size_t wd_blob_size(void){ return (size_t)(wd_bin_end - wd_bin_start); }
