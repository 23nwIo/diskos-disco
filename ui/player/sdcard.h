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
 * other devices, change events). Pure, for the host test. Format checked on a Disc with `busybox uevent`
 * (2026-11-15): "remove@/devices/platform/md_ingenic,sdhci.2/mmc_host/mmc1/mmc1:aaaa/block/mmcblk0", SUBSYSTEM=block,
 * DEVNAME=mmcblk0, DEVTYPE=disk. */
int sd_uevent_kind(const char *msg, size_t len);

/* Open the kernel uevent socket; returns the descriptor or -1 (errno set). */
int sd_uevent_open(void);

typedef void (*sd_log_fn)(const char *fmt, ...);

/* 1 if something is mounted on SD_MOUNTPOINT (per /proc/mounts). */
int sd_is_mounted(void);

/* Card inserted: mount it. A mount left over from before the card was pulled (the pull was missed, or its unmount
 * failed) is dead - reading it fails with EIO - and is detached first; a working mount is kept. The file system is on mmcblk0p1 when that exists, else on the
 * whole card. Tried in order: ext4 (without iocharset, which ext4 does not take; the stock player cannot mount it),
 * then vfat and exfat with iocharset=utf8 as the stock player. 0 = mounted (or already), -1 = not. */
int sd_mount(sd_log_fn log);

/* Card pulled: sync, then unmount (lazily if busy, so the dead card cannot hold the mountpoint). */
void sd_unmount(sd_log_fn log);
#endif
