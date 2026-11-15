/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* USB storage mode (0642 1): the SD card is handed to a USB host as a mass storage device, as the stock player does it
 * (trace run 5, docs/MQ_PLAYER_TRACE.md): unmount the card, USB role "device", a configfs gadget "storage_demo" with
 * the whole card (/dev/mmcblk0) as its one LUN, bound to the UDC 13500000.otg_new. 0642 0 undoes it and mounts the
 * card again. While the card is exported the player must not mount it (the host writes to it). */
#ifndef DISKOS_PLAYER_USBSTORAGE_H
#define DISKOS_PLAYER_USBSTORAGE_H

typedef void (*us_log_fn)(const char *fmt, ...);

/* Export the card. 0 = the gadget is bound, -1 = not (the card is mounted again in that case). dry_run: log only. */
int us_start(int dry_run, us_log_fn log);
/* Stop exporting and mount the card again. Harmless when the gadget is not active. */
void us_stop(int dry_run, us_log_fn log);
/* 1 while the card is exported */
int us_active(void);
#endif
