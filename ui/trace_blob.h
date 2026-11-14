/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef TRACE_BLOB_H
#define TRACE_BLOB_H
#include <stddef.h>
const unsigned char *trace_blob_data(void);
size_t trace_blob_size(void);
#endif
