/* SPDX-License-Identifier: GPL-3.0-or-later */
/* diskos-watchdog embedded in mq_ui (wd_blob.c). */
#ifndef WD_BLOB_H
#define WD_BLOB_H
#include <stddef.h>
const unsigned char *wd_blob_data(void);
size_t wd_blob_size(void);
#endif
