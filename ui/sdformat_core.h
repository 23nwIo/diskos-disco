/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* SD card formatter, the pure part: lays out a filesystem on a block device through a write callback, so the host
 * tests can format an image file and check it with the real tools (tests/sdformat_test.c, tests/sdformat_test.py).
 * No LVGL, no device paths. sdformat.c is the device side (unmount, exclusive open, progress, restart).
 *
 *   FAT32: an MBR with one partition (type 0x0C, FAT32 LBA) starting at 4 MiB, FAT32 inside it. This is the layout
 *          the stock firmware, computers and other players expect.
 *   ext4:  the whole card, no partition table. 4 KiB blocks, extents, a 16-64 MiB journal, uninit_bg (the kernel
 *          zeroes the unused inode tables in the background), 0 % reserved blocks, one inode per 32 KiB.
 * Every block either format relies on is written explicitly (nothing is assumed to be zero on the card). */
#ifndef DISKOS_SDFORMAT_CORE_H
#define DISKOS_SDFORMAT_CORE_H
#include <stddef.h>
#include <stdint.h>

typedef enum { SDF_FAT32 = 0, SDF_EXT4 = 1 } sdf_kind_t;

typedef struct {
    /* write len bytes at byte offset off of the whole card; 0 = ok, anything else stops the format */
    int  (*write)(void *ud, uint64_t off, const void *buf, size_t len);
    /* optional: done/total bytes, called after each write */
    void (*progress)(void *ud, uint64_t done, uint64_t total);
    void *ud;
    uint64_t bytes;          /* card size in bytes */
    uint32_t now;            /* timestamps (seconds since 1970) */
    uint8_t  rnd[32];        /* random bytes: volume id / disk signature, ext4 UUID and hash seed */
    const char *label;       /* volume label, ASCII; NULL = "DISC" */
} sdf_target_t;

/* Minimum card sizes the formatter accepts. */
#define SDF_MIN_BYTES (64ULL << 20)

/* 0 = formatted. On failure returns a negative code and writes a short English reason to err. */
int sdf_format(sdf_kind_t kind, const sdf_target_t *t, char *err, size_t errn);

/* exposed for the tests */
uint16_t sdf_crc16(uint16_t crc, const void *buf, size_t len);
#endif
