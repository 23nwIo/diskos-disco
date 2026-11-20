/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_SDFSCHECK_H
#define DISKOS_SDFSCHECK_H
#include <stddef.h>

/* Is a filesystem marked "in use" on disk? Reads only the first SDFS_HEAD bytes of the filesystem (the ext superblock,
 * the FAT or exFAT boot sector). Used while the card is shared over USB: a computer that still has the card mounted
 * leaves these marks set, and a card switched back to the player in that state comes back dirty (2026-11-20: ext4
 * from a Linux PC then even refuses a writable mount on the Disc).
 *   ext2/3/4: journal needs recovery (incompat 0x4), orphan_present (ro_compat 0x10000) or the state lacks "valid"
 *   FAT12/16/32: the dirty bit in the boot sector's state byte (Linux sets it while mounted)
 *   exFAT: VolumeDirty in VolumeFlags
 * Windows and macOS may not set these marks while mounted (unverified), so CLEAN does not prove that no computer
 * has the card open. */
#define SDFS_HEAD 2048
enum { SDFS_UNKNOWN = -1, SDFS_CLEAN = 0, SDFS_DIRTY = 1 };
int sdfs_state(const unsigned char *b, size_t n);
/* the filesystem's name for logs: "ext", "fat", "exfat" or "unknown" */
const char *sdfs_kind(const unsigned char *b, size_t n);

#endif
