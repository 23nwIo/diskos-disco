/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 diskOS contributors */
/* Hardware watchdog of the Snowsky Disc (Ingenic X2000, /dev/jz_watchdog).
 *
 * Open-source replacement for watchdog_start/stop/feed/reset in the stock libhardware2.so. The four ioctls below were
 * read from that library (V2.57 rootfs, 2026-11-14); see docs/MQ_PLAYER_REPLACEMENT.md. The stock library opens the
 * node, issues one ioctl and closes it again for every call, so closing the descriptor does not disarm the watchdog.
 *
 * All functions return 0 on success or a negative errno value. */
#ifndef DISKOS_WATCHDOG_H
#define DISKOS_WATCHDOG_H

#include <sys/ioctl.h>

#ifndef WD_DEV_PATH
#define WD_DEV_PATH "/dev/jz_watchdog"
#endif

/* The exact request numbers libhardware2.so passes, i.e. the MIPS encodings of _IOW('W', 13, int) and _IO('W', 14..16).
 * Spelled as literals so the values never depend on the build host's <asm/ioctl.h> (tests build for x86). */
#define WD_IOC_START 0x8004570dUL   /* argument: pointer to int timeout in ms */
#define WD_IOC_STOP  0x2000570eUL
#define WD_IOC_FEED  0x2000570fUL
#define WD_IOC_RESET 0x20005710UL

#define WD_MAX_TIMEOUT_MS 0x7fffffff

/* Open the watchdog node (O_RDWR | O_CLOEXEC). Returns the descriptor or a negative errno value. */
int wd_open(void);
void wd_close(int fd);

int wd_start(int fd, int timeout_ms);   /* arm with a timeout; timeout_ms must be > 0 */
int wd_stop(int fd);                    /* disarm */
int wd_feed(int fd);                    /* restart the countdown */
int wd_reset(int fd);                   /* stock "reset" request; expected to reboot the Disc (unverified) */

#endif
