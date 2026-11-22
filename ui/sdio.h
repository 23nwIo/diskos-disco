/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_SDIO_H
#define DISKOS_SDIO_H

/* A lease covers the lifetime of every SD handle, including decoder children.
 * Closing admission and counting existing leases use the same mutex. */
void sd_io_init(int (*media_local)(void));
int sd_io_begin(void);
void sd_io_end(void);
void sd_io_hold(void);
unsigned sd_io_active(void);
int sd_io_resume(void);
int sd_io_allowed(void);
/* An unaccounted decoder keeps ownership uncertain across UI restarts. */
void sd_io_fault(void);
int sd_io_healthy(void);
/* Called (outside the lock) after sd_io_hold() closed admission and after sd_io_resume() reopened it: mq_ui closes
 * and reopens the card library's files there (ui/libdb_vfs.c). */
void sd_io_hooks(void (*on_hold)(void), void (*on_resume)(void));

#endif
