/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Host driver for the SD card formatter (sdformat_core.c): formats an image file the way the device formats the card.
 *   sdformat_test fat32|ext4 IMAGE        (IMAGE must exist; its size is the card size)
 * tests/sdformat_test.py runs it on sparse images of several sizes and checks the result with e2fsck, the kernel
 * (loop mount) and a FAT32 structure check. Also checks the CRC16 known answer. */
#define _GNU_SOURCE
#include "sdformat_core.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int wr(void *ud, uint64_t off, const void *buf, size_t len){
    int fd = *(int *)ud;
    const char *p = buf;
    while(len){ ssize_t n = pwrite(fd, p, len, (off_t)off); if(n <= 0) return -1; p += n; off += (uint64_t)n; len -= (size_t)n; }
    return 0;
}
static uint64_t last_done, last_total; static int calls;
static void prog(void *ud, uint64_t d, uint64_t t){ (void)ud; if(d < last_done || (last_total && t != last_total)) { fprintf(stderr, "progress not monotonic\n"); _exit(3); } last_done = d; last_total = t; calls++; }

int main(int argc, char **argv){
    /* CRC16 (ARC, as lib/crc16.c) of "123456789" from 0 is 0xBB3D */
    if(sdf_crc16(0, "123456789", 9) != 0xBB3D){ fprintf(stderr, "crc16 known answer failed\n"); return 2; }
    if(argc != 3){ fprintf(stderr, "usage: %s fat32|ext4 IMAGE\n", argv[0]); return 2; }
    int fd = open(argv[2], O_RDWR); if(fd < 0){ perror("open"); return 2; }
    struct stat st; fstat(fd, &st);
    sdf_target_t t; memset(&t, 0, sizeof t);
    t.write = wr; t.progress = prog; t.ud = &fd; t.bytes = (uint64_t)st.st_size; t.now = 1793000000u;
    for(int i = 0; i < 32; i++) t.rnd[i] = (uint8_t)(i * 37 + 11);
    char err[96];
    int rc = sdf_format(!strcmp(argv[1], "ext4") ? SDF_EXT4 : SDF_FAT32, &t, err, sizeof err);
    fsync(fd); close(fd);
    if(rc){ printf("FAIL %d %s\n", rc, err); return 1; }
    if(last_done != last_total || !calls){ fprintf(stderr, "progress did not reach the total\n"); return 3; }
    printf("OK %llu bytes written\n", (unsigned long long)last_done);
    return 0;
}
