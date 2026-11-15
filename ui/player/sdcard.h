/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The SD card: mount it when it is inserted, unmount it when it is pulled. The stock player does this (trace run 4,
 * docs/MQ_PLAYER_TRACE.md) and the UI relies on it for hot-insert; the cold-boot mount is done by the UI itself
 * (ui/main.c coldplug). The kernel announces the card with uevents on a netlink socket. */
#ifndef DISKOS_PLAYER_SDCARD_H
#define DISKOS_PLAYER_SDCARD_H
#include <stddef.h>

#define SD_MOUNTPOINT "/tmp/sdcard"

enum { SDEV_NONE = 0, SDEV_ADD, SDEV_REMOVE };

/* Classify one uevent message (NUL separated "key=value" strings after a "action@devpath" header): SDEV_ADD or
 * SDEV_REMOVE for the whole card (block device mmcblk0, DEVTYPE=disk), SDEV_NONE for anything else (partitions,
 * other devices, change events). Pure, for the host test. */
int sd_uevent_kind(const char *msg, size_t len);

/* Open the uevent socket; returns the descriptor or -1. */
int sd_uevent_open(void);

typedef void (*sd_log_fn)(const char *fmt, ...);

/* 1 if something is mounted on SD_MOUNTPOINT (per /proc/mounts). */
int sd_is_mounted(void);

/* Card inserted: mount it unless something already is. The file system is on mmcblk0p1 when that exists, else on the
 * whole card. Tried in order: vfat and exfat with iocharset=utf8 (as the stock player), then ext4 without (ext4 does
 * not take iocharset; the stock player cannot mount it). 0 = mounted (or already), -1 = not. */
int sd_mount(sd_log_fn log);

/* Card pulled: sync, then unmount (lazily if busy, so the dead card cannot hold the mountpoint). */
void sd_unmount(sd_log_fn log);
#endif
