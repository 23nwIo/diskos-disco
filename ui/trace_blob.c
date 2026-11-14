/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* diskos-trace.so (ui/trace, MIT), built by the Makefile and carried inside mq_ui so a signed SD update brings it. The
 * launcher writes it to /usr/data and preloads it into the stock player when the trace is switched on
 * (trace_launch.h). Plain bytes in .rodata, never run by mq_ui. */
#include "trace_blob.h"

#ifndef TRACE_BIN
#define TRACE_BIN "trace/diskos-trace.so"
#endif
__asm__(".section .rodata\n.balign 4\n.global trace_bin_start\ntrace_bin_start:\n.incbin \"" TRACE_BIN "\"\n"
        ".global trace_bin_end\ntrace_bin_end:\n.previous\n");
extern const unsigned char trace_bin_start[], trace_bin_end[];

const unsigned char *trace_blob_data(void){ return trace_bin_start; }
size_t trace_blob_size(void){ return (size_t)(trace_bin_end - trace_bin_start); }
