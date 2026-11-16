/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_SDSTATE_H
#define DISKOS_SDSTATE_H
#include <stddef.h>

/* Data diskOS keeps on the SD card instead of the internal NAND (Jo, 2026-11-16: no recurring flash writes):
 * <card>/.diskos/state/. The other folders in .diskos/ are caches that may be deleted; this one is not.
 * Every use runs inside a lease: sds_begin() is 1 only when the card is mounted at /tmp/sdcard and SD I/O is
 * admitted (sdio.c; not during USB storage mode or a format), and then holds an SD I/O lease until sds_end(). */
#define SDS_CARD "/tmp/sdcard"
#define SDS_SUBDIR "/.diskos/state"

int  sds_begin(void);
void sds_end(void);
/* <card>/.diskos/state/<name>; -1 if it does not fit */
int  sds_path(char *out, size_t cap, const char *name);
/* creates <card>/.diskos/state if needed; 0 or -1 (errno) */
int  sds_mkdir(void);
/* inside a lease: write <name> whole (temp file, fsync, rename); 0 or -1 (errno) */
int  sds_write(const char *name, const void *data, size_t n);
/* inside a lease: read <name> whole into a malloc'd, NUL-terminated buffer; 1 read, 0 no such file, -1 error */
int  sds_read(const char *name, char **data, size_t *n, size_t max);
/* host tests: use dir as the card (no mount check, no lease); NULL back to the card */
void sds_test_root(const char *dir);

#endif
