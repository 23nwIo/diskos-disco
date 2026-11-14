/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_USBDRIVE_CORE_H
#define DISKOS_USBDRIVE_CORE_H
#include <stddef.h>

/* The pure half of the USB drive support (usbdrive.c): which block devices to mount, and how to name them. No LVGL,
 * no global state, host-tested by tests/usbdrive_core_test.c against a fake /sys/block tree. */

typedef struct { char name[16]; unsigned long long bytes; } udc_dev_t;

/* The mount candidates under `sysblock` (normally "/sys/block"): every USB/SCSI disk "sd*". A disk with partitions gives
 * its partitions (sdb1, sdb2 ...), a disk without gives itself (an unpartitioned ext4 card). Empty card-reader slots
 * (size 0) and tiny entries (< 1 MiB, e.g. an MBR extended-partition container) are skipped. Sorted by name; returns
 * the count (at most `max`). */
int udc_scan(const char *sysblock, udc_dev_t *out, int max);

/* 1 when the block device or image at `devpath` holds an ext2/3/4 superblock; its volume label (may be empty) goes to
 * out. 0 for anything else (and on a read error). */
int udc_ext_label(const char *devpath, char *out, size_t cap);

/* "119 GB", "7.4 GB", "512 MB": decimal units, as cards are sold */
void udc_fmt_size(unsigned long long bytes, char *out, size_t cap);

/* FAT, exFAT and NTFS (in-kernel or FUSE): the file systems the transfer dialog warns about */
int udc_is_windows_fs(const char *fstype);

#endif
