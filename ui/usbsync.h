/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_USBSYNC_H
#define DISKOS_USBSYNC_H

/* Import from / export to a USB drive (usbdrive.h) with rsync (USC_RSYNC). Files > USB drive:
 *   hold a folder or song on a drive    -> Import to player (the same folder path on the SD card)
 *   hold a drive in the drive list      -> Import everything / Export everything / Eject
 *   hold a folder or song on the player -> Export (Files menu), to the same path on the drive
 * One dialog: Mode (Only missing / Update changed files, with Compare thoroughly and Keep backups), then a dry run
 * counts what would be copied and checks the free space, then Start. The copy runs on a worker thread; its dialog
 * can be hidden (a toast reports the end). Replaced files go to <target>/_changedN/..., rsync's per-file log to
 * <target>/synclog/. An export makes the drive writable for its run only. After an import the library rescans. */

void usbsync_import_open(const char *dir, const char *name, void (*done)(void));   /* name "" = all of dir */
void usbsync_export_open(const char *dir, const char *name, void (*done)(void));
void usbsync_volume_menu(const char *mnt, void (*done)(void));
int  usbsync_busy(void);              /* a count or a copy is running */
void usbsync_preview(int step);       /* host renders: 0 dialog, 1 update mode, 2 counted, 3 progress, 4 result, 5 drive menu */

#endif
