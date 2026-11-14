/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_USBDRIVE_H
#define DISKOS_USBDRIVE_H

/* USB drive: the Disc as USB host for a stick, a card reader, or a second Disc in "USB storage" mode.
 * Asked from Files > USB drive. The player is asked for its USB host port (0642 5, the gadget selector its USB Audio
 * output uses), every file system found on /dev/sd* is mounted READ-ONLY under /tmp/usbdrive/<dev> by a worker
 * thread, and Eject (or leaving Playback) unmounts and hands the port back (0642 0). An export remounts one volume
 * read-write for its run only (usbdrive_set_rw). Log: /usr/data/usbdrive.log. */

#define UD_ROOT "/tmp/usbdrive"
#define UD_MAX  8

enum { UD_OFF, UD_ON, UD_STOPPING };

typedef struct {
    char dev[16];          /* sdb2 */
    char mnt[40];          /* /tmp/usbdrive/sdb2 */
    char label[24];        /* the ext volume label, "" when none */
    char fstype[8];        /* ext4, exfat, vfat, ntfs, xfs */
    unsigned long long bytes;
    int rw;                /* 1 while an export holds it writable */
    int remounts;          /* times someone else unmounted it and diskOS mounted it again */
} ud_vol_t;

const char *usbdrive_blocked(void);     /* why the host port can't be taken now (toast text), NULL = it can */
int  usbdrive_start(void);              /* take the host port and start looking; 0 = on (or already), -1 = refused (toasted) */
int  usbdrive_stop(void);               /* eject: unmount everything, then hand the port back; -1 while a transfer runs */
int  usbdrive_state(void);              /* UD_OFF / UD_ON / UD_STOPPING */
unsigned usbdrive_on_ms(void);          /* ms since the port was taken (0 when off): "Looking..." vs "No drive found" */
int  usbdrive_count(void);              /* mounted volumes */
int  usbdrive_get(int i, ud_vol_t *out);/* copy of volume i; 0 = no such volume */
int  usbdrive_find(const char *path, ud_vol_t *out);   /* the volume `path` lies on (its index), -1 = none */
const char *usbdrive_name(const ud_vol_t *v, char *buf, int cap);   /* "DISC" or "USB drive (sdb2)" */
void usbdrive_on_change(void (*cb)(void));             /* the UI thread is told when volumes come or go */

/* transfers (usbsync.c), from its worker thread */
int  usbdrive_pin(const char *dev);     /* keep this volume mounted and the port held; 0 = it is gone */
void usbdrive_unpin(void);
int  usbdrive_set_rw(const char *dev, int rw);         /* remount; 0 = done */

#endif
