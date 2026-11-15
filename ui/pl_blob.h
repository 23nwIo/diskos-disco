/* SPDX-License-Identifier: GPL-3.0-or-later */
/* diskos-player (ui/player) embedded in mq_ui (pl_blob.c). */
#ifndef PL_BLOB_H
#define PL_BLOB_H
#include <stddef.h>
const unsigned char *pl_blob_data(void);
size_t pl_blob_size(void);
#endif
