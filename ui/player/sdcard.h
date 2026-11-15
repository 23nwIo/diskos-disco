/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The SD card: mount it when it is inserted, unmount it when it is pulled. The stock player does this (trace run 4,
 * docs/MQ_PLAYER_TRACE.md) and the UI relies on it for hot-insert; the cold-boot mount is done by the UI itself
 * (ui/main.c coldplug). */
#ifndef DISKOS_PLAYER_SDCARD_H
#define DISKOS_PLAYER_SDCARD_H
#include <stddef.h>

#define SD_MOUNTPOINT "/tmp/sdcard"

/* 1 if the kernel currently has the card (/sys/block/mmcblk0 exists). The player polls this once a second: on the
 * Disc the kernel notices a pulled or inserted card ("mmc1: card aaaa removed"), but the netlink uevent listener of
 * d342e43 logged nothing in the device test 2026-11-15 (cause not found), so the edge is taken from sysfs instead. */
int sd_card_present(void);

typedef void (*sd_log_fn)(const char *fmt, ...);

/* 1 if something is mounted on SD_MOUNTPOINT (per /proc/mounts). */
int sd_is_mounted(void);

/* Card inserted: mount it. A mount left over from before the card was pulled (the pull was missed, or its unmount
 * failed) is dead - reading it fails with EIO - and is detached first; a working mount is kept. The file system is on mmcblk0p1 when that exists, else on the
 * whole card. Tried in order: vfat and exfat with iocharset=utf8 (as the stock player), then ext4 without (ext4 does
 * not take iocharset; the stock player cannot mount it). 0 = mounted (or already), -1 = not. */
int sd_mount(sd_log_fn log);

/* Card pulled: sync, then unmount (lazily if busy, so the dead card cannot hold the mountpoint). */
void sd_unmount(sd_log_fn log);
#endif
