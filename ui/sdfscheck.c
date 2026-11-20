/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sdfscheck.h"
#include <stdint.h>
#include <string.h>

static uint32_t le16(const unsigned char *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const unsigned char *p){ return le16(p) | le16(p + 2) << 16; }

enum { K_UNKNOWN, K_EXT, K_FAT16, K_FAT32, K_EXFAT };
static int kind(const unsigned char *b, size_t n){
    if(n >= 1024 + 0x68 && le16(b + 1024 + 0x38) == 0xEF53) return K_EXT;
    if(n < 512) return K_UNKNOWN;
    if(!memcmp(b + 3, "EXFAT   ", 8)) return K_EXFAT;
    if(b[510] != 0x55 || b[511] != 0xAA) return K_UNKNOWN;
    if(!memcmp(b + 82, "FAT32   ", 8)) return K_FAT32;
    if(!memcmp(b + 54, "FAT1", 4)) return K_FAT16;          /* FAT12 and FAT16 share this layout */
    return K_UNKNOWN;
}

int sdfs_state(const unsigned char *b, size_t n){
    switch(kind(b, n)){
    case K_EXT: {
        const unsigned char *s = b + 1024;
        uint32_t state = le16(s + 0x3A), incompat = le32(s + 0x60), ro_compat = le32(s + 0x64);
        return (incompat & 0x4) || (ro_compat & 0x10000) || !(state & 0x1) ? SDFS_DIRTY : SDFS_CLEAN;
    }
    case K_FAT32: return (b[0x41] & 0x1) ? SDFS_DIRTY : SDFS_CLEAN;
    case K_FAT16: return (b[0x25] & 0x1) ? SDFS_DIRTY : SDFS_CLEAN;
    case K_EXFAT: return (le16(b + 106) & 0x2) ? SDFS_DIRTY : SDFS_CLEAN;
    }
    return SDFS_UNKNOWN;
}

const char *sdfs_kind(const unsigned char *b, size_t n){
    switch(kind(b, n)){
    case K_EXT: return "ext";
    case K_FAT16: case K_FAT32: return "fat";
    case K_EXFAT: return "exfat";
    }
    return "unknown";
}
