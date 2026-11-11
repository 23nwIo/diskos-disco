/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* SD card formatter, pure part. See sdformat_core.h. On-disk formats: Microsoft FAT32 specification (BPB, FSInfo,
 * MBR) and the Linux ext4 disk layout (Documentation/filesystems/ext4/), checked against e2fsck and the kernel in
 * tests/sdformat_test.py. Everything is written little-endian except the JBD2 journal superblock (big-endian). */
#include "sdformat_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZBUF (1u << 20)

typedef struct {
    const sdf_target_t *t;
    int count_only;          /* first pass: only add up the bytes the real pass will write */
    uint64_t done, total;
    uint8_t *zero;           /* ZBUF zero bytes */
} wr_t;

static int emit(wr_t *w, uint64_t off, const void *buf, size_t len){
    if(off + len > w->t->bytes || off + len < off) return -1;
    if(!w->count_only){
        if(w->t->write(w->t->ud, off, buf, len) != 0) return -1;
        w->done += len;
        if(w->t->progress) w->t->progress(w->t->ud, w->done, w->total);
    } else w->total += len;
    return 0;
}
static int emit_zero(wr_t *w, uint64_t off, uint64_t len){
    while(len){
        size_t n = len > ZBUF ? ZBUF : (size_t)len;
        if(emit(w, off, w->zero, n) != 0) return -1;
        off += n; len -= n;
    }
    return 0;
}

static void le16(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void be32(uint8_t *p, uint32_t v){ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void seterr(char *err, size_t n, const char *m){ if(err && n) snprintf(err, n, "%s", m); }

/* CRC16 as used by ext4 gdt_csum (lib/crc16.c in Linux: polynomial 0x8005, bit-reflected, LSB first) */
uint16_t sdf_crc16(uint16_t crc, const void *buf, size_t len){
    const uint8_t *p = buf;
    while(len--){
        crc ^= *p++;
        for(int k = 0; k < 8; k++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static void label_upper(char *out, size_t n, const char *in, int fat){
    /* FAT: 11 bytes, upper case, space padded; ext4: up to 16 bytes, NUL padded. Only A-Z 0-9 _ - kept. */
    memset(out, fat ? ' ' : 0, n);
    size_t k = 0;
    for(const char *s = in ? in : "DISC"; *s && k < n; s++){
        char c = *s;
        if(c >= 'a' && c <= 'z') c = (char)(c - 32);
        if((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') out[k++] = c;
    }
    if(!k){ memcpy(out, "DISC", 4); }
}

/* ======================================================================== FAT32 in an MBR partition */
#define SEC 512u
#define PART_START 8192u     /* 4 MiB: the SD Association's allocation-unit alignment */
#define ALIGN_SECS 8192u     /* the data area also starts on a 4 MiB boundary */

static void chs(uint8_t *p, uint64_t lba){
    /* 255 heads x 63 sectors; past cylinder 1023 the conventional "use LBA" marker */
    uint64_t c = lba / (255 * 63);
    if(c > 1023){ memcpy(p, "\xFE\xFF\xFF", 3); return; }
    uint32_t h = (uint32_t)((lba / 63) % 255), s = (uint32_t)(lba % 63) + 1;
    p[0] = (uint8_t)h; p[1] = (uint8_t)(s | ((c >> 2) & 0xC0)); p[2] = (uint8_t)c;
}

static int fat32_run(wr_t *w, char *err, size_t errn){
    const sdf_target_t *t = w->t;
    uint64_t total = t->bytes / SEC;
    if(total <= PART_START + ALIGN_SECS){ seterr(err, errn, "card too small"); return -2; }
    uint64_t psize = total - PART_START;
    if(psize > 0xFFFFFFFFull){ seterr(err, errn, "card too large for FAT32 (over 2 TB)"); return -3; }
    /* cluster size by partition size (as the SD card formatter and Windows do), smaller if needed for FAT32's
     * minimum of 65525 clusters */
    uint32_t spc = psize * SEC <= (8ull << 30) ? 8 : psize * SEC <= (16ull << 30) ? 16 : psize * SEC <= (32ull << 30) ? 32 : 64;
    uint32_t rsv = 0, fatsz = 0; uint64_t clusters = 0;
    for(;;){
        rsv = 32;
        fatsz = (uint32_t)((((psize - rsv) / spc + 2) * 4 + SEC - 1) / SEC);   /* upper bound, never too small */
        uint64_t data0 = PART_START + rsv + 2ull * fatsz;
        rsv += (uint32_t)((ALIGN_SECS - data0 % ALIGN_SECS) % ALIGN_SECS);    /* data area on a 4 MiB boundary */
        if(psize <= rsv + 2ull * fatsz){ seterr(err, errn, "card too small"); return -2; }
        clusters = (psize - rsv - 2ull * fatsz) / spc;
        if(clusters >= 65525 || spc == 1) break;
        spc /= 2;
    }
    if(clusters < 65525){ seterr(err, errn, "card too small for FAT32"); return -2; }
    if(clusters > 0x0FFFFFF4ull - 2){ seterr(err, errn, "card too large for FAT32"); return -3; }
    if((uint64_t)fatsz * (SEC / 4) < clusters + 2){ seterr(err, errn, "internal FAT size error"); return -9; }

    uint32_t volid = (uint32_t)t->rnd[0] | (uint32_t)t->rnd[1] << 8 | (uint32_t)t->rnd[2] << 16 | (uint32_t)t->rnd[3] << 24;
    char lab[11]; label_upper(lab, sizeof lab, t->label, 1);
    uint64_t p0 = (uint64_t)PART_START * SEC;               /* partition start, bytes */
    uint8_t s[SEC];

    /* wipe the area before the partition (old partition tables, old filesystem signatures) and the last MiB
     * (a backup GPT header). Then the MBR. */
    if(emit_zero(w, 0, p0) != 0) return -1;
    if(t->bytes >= p0 + (2u << 20) && emit_zero(w, (t->bytes & ~(uint64_t)(SEC - 1)) - (1u << 20), 1u << 20) != 0) return -1;

    /* reserved area, both FATs and the root directory cluster: zero first, then the structures */
    if(emit_zero(w, p0, (uint64_t)rsv * SEC) != 0) return -1;
    for(int f = 0; f < 2; f++) if(emit_zero(w, p0 + ((uint64_t)rsv + (uint64_t)f * fatsz) * SEC, (uint64_t)fatsz * SEC) != 0) return -1;
    uint64_t root = p0 + ((uint64_t)rsv + 2ull * fatsz) * SEC;   /* cluster 2 */
    if(emit_zero(w, root, (uint64_t)spc * SEC) != 0) return -1;

    /* boot sector (BPB), written to sector 0 and the backup at sector 6 */
    memset(s, 0, sizeof s);
    memcpy(s, "\xEB\x58\x90", 3);                     /* jmp short + nop */
    memcpy(s + 3, "MSWIN4.1", 8);
    le16(s + 11, SEC); s[13] = (uint8_t)spc; le16(s + 14, rsv); s[16] = 2;
    s[21] = 0xF8; le16(s + 24, 63); le16(s + 26, 255);
    le32(s + 28, PART_START); le32(s + 32, (uint32_t)psize);
    le32(s + 36, fatsz); le32(s + 44, 2); le16(s + 48, 1); le16(s + 50, 6);
    s[64] = 0x80; s[66] = 0x29; le32(s + 67, volid);
    memcpy(s + 71, lab, 11); memcpy(s + 82, "FAT32   ", 8);
    s[90] = 0xF4;                                            /* boot code: hlt (never booted) */
    s[510] = 0x55; s[511] = 0xAA;
    if(emit(w, p0, s, SEC) != 0 || emit(w, p0 + 6 * SEC, s, SEC) != 0) return -1;
    /* FSInfo at sector 1 and its backup at 7; sector 2 (and 8) carry the boot signature too */
    memset(s, 0, sizeof s);
    le32(s, 0x41615252); le32(s + 484, 0x61417272);
    le32(s + 488, (uint32_t)(clusters - 1)); le32(s + 492, 3);
    le32(s + 508, 0xAA550000);
    if(emit(w, p0 + 1 * SEC, s, SEC) != 0 || emit(w, p0 + 7 * SEC, s, SEC) != 0) return -1;
    memset(s, 0, sizeof s); s[510] = 0x55; s[511] = 0xAA;
    if(emit(w, p0 + 2 * SEC, s, SEC) != 0 || emit(w, p0 + 8 * SEC, s, SEC) != 0) return -1;
    /* the first FAT sector of both FATs: media, end-of-chain, root directory end-of-chain */
    memset(s, 0, sizeof s);
    le32(s, 0x0FFFFFF8); le32(s + 4, 0x0FFFFFFF); le32(s + 8, 0x0FFFFFFF);
    for(int f = 0; f < 2; f++) if(emit(w, p0 + ((uint64_t)rsv + (uint64_t)f * fatsz) * SEC, s, SEC) != 0) return -1;
    /* root directory: the volume label entry */
    memset(s, 0, sizeof s);
    memcpy(s, lab, 11); s[11] = 0x08;
    {   uint32_t tm = t->now, days = tm / 86400, sod = tm % 86400;   /* FAT date/time of the label entry (UTC) */
        int y = 1970; uint32_t d = days;
        for(;;){ int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; uint32_t dy = leap ? 366 : 365; if(d < dy) break; d -= dy; y++; }
        static const uint8_t ml[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
        int m = 0; for(; m < 12; m++){ uint32_t l = ml[m] + (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)); if(d < l) break; d -= l; }
        if(y < 1980) y = 1980;
        uint16_t fd = (uint16_t)(((y - 1980) << 9) | ((m + 1) << 5) | (d + 1));
        uint16_t ft = (uint16_t)(((sod / 3600) << 11) | (((sod / 60) % 60) << 5) | ((sod % 60) / 2));
        le16(s + 14, ft); le16(s + 16, fd); le16(s + 18, fd); le16(s + 22, ft); le16(s + 24, fd);
    }
    if(emit(w, root, s, SEC) != 0) return -1;

    /* MBR last: until it is written the card has no valid partition table */
    memset(s, 0, sizeof s);
    s[440] = t->rnd[4]; s[441] = t->rnd[5]; s[442] = t->rnd[6]; s[443] = t->rnd[7] | 1;   /* disk signature, never 0 */
    uint8_t *pe = s + 446;
    pe[0] = 0; chs(pe + 1, PART_START); pe[4] = 0x0C; chs(pe + 5, PART_START + psize - 1);
    le32(pe + 8, PART_START); le32(pe + 12, (uint32_t)psize);
    s[510] = 0x55; s[511] = 0xAA;
    if(emit(w, 0, s, SEC) != 0) return -1;
    return 0;
}

/* ======================================================================== ext4 on the whole card */
#define BS 4096u
#define BPG 32768u           /* blocks per group (one bitmap block) */
#define IPG 4096u            /* inodes per group: one inode per 32 KiB */
#define ISZ 256u
#define ITB (IPG * ISZ / BS) /* inode table blocks per group */
#define FIRST_INO 11u
#define ROOT_INO 2u
#define JOURNAL_INO 8u
#define LF_INO 11u
#define LF_BLOCKS 4u

static int has_super(uint32_t g){
    if(g <= 1) return 1;
    for(uint32_t b = 3; b <= 7; b += 2){ uint64_t p = b; while(p < g) p *= b; if(p == g) return 1; }
    return 0;
}

typedef struct {
    uint32_t ngroups, gdt_blocks, blocks, jblocks, jgroup, jstart;
    uint32_t root_blk, lf_blk;
} e4_t;
static uint32_t grp_start(uint32_t g){ return g * BPG; }
static uint32_t grp_size(const e4_t *e, uint32_t g){ return g + 1 < e->ngroups ? BPG : e->blocks - grp_start(g); }
static uint32_t grp_meta(const e4_t *e, uint32_t g){ return (has_super(g) ? 1 + e->gdt_blocks : 0) + 2 + ITB; }
static uint32_t grp_bb(const e4_t *e, uint32_t g){ return grp_start(g) + (has_super(g) ? 1 + e->gdt_blocks : 0); }

static void e4_inode(uint8_t *in, uint16_t mode, uint16_t links, uint64_t size, uint32_t blk, uint32_t nblk, uint32_t now){
    memset(in, 0, ISZ);
    le16(in + 0, mode); le32(in + 4, (uint32_t)size); le32(in + 8, now); le32(in + 12, now); le32(in + 16, now);
    le16(in + 26, links); le32(in + 28, nblk * (BS / 512)); le32(in + 32, 0x80000);   /* EXT4_EXTENTS_FL */
    uint8_t *ib = in + 40;                                    /* i_block: extent header + one extent */
    le16(ib + 0, 0xF30A); le16(ib + 2, 1); le16(ib + 4, 4); le16(ib + 6, 0); le32(ib + 8, 0);
    le32(ib + 12, 0); le16(ib + 16, (uint16_t)nblk); le16(ib + 18, 0); le32(ib + 20, blk);
    le32(in + 108, (uint32_t)(size >> 32));
    le16(in + 128, 32);                                       /* i_extra_isize */
    le32(in + 144, now);                                      /* i_crtime */
}

static int ext4_run(wr_t *w, char *err, size_t errn){
    const sdf_target_t *t = w->t;
    e4_t e; memset(&e, 0, sizeof e);
    uint64_t nb = t->bytes / BS;
    if(nb > 0xFFFFFFFFull){ seterr(err, errn, "card too large"); return -3; }
    e.blocks = (uint32_t)nb;
    for(;;){                                                   /* drop a last group too small for its own metadata */
        e.ngroups = (e.blocks + BPG - 1) / BPG;
        e.gdt_blocks = (e.ngroups * 32 + BS - 1) / BS;
        uint32_t last = e.ngroups - 1;
        if(e.ngroups > 1 && grp_size(&e, last) < grp_meta(&e, last) + 256){ e.blocks = grp_start(last); continue; }
        break;
    }
    if(e.ngroups < 1 || grp_size(&e, 0) < grp_meta(&e, 0) + 2048){ seterr(err, errn, "card too small"); return -2; }
    e.jblocks = e.blocks < 32768 ? 1024 : e.blocks < 262144 ? 4096 : 16384;   /* 4 / 16 / 64 MiB */
    e.root_blk = grp_bb(&e, 0) + 2 + ITB;
    e.lf_blk = e.root_blk + 1;
    /* the journal: a middle group (as mke2fs does), else group 0 after root and lost+found */
    e.jgroup = e.ngroups / 2;
    if(e.jgroup && grp_size(&e, e.jgroup) - grp_meta(&e, e.jgroup) >= e.jblocks) e.jstart = grp_bb(&e, e.jgroup) + 2 + ITB;
    else { e.jgroup = 0; e.jstart = e.lf_blk + LF_BLOCKS; }
    if(e.jstart + e.jblocks > grp_start(e.jgroup) + grp_size(&e, e.jgroup)){ seterr(err, errn, "card too small"); return -2; }

    uint8_t *blk = calloc(1, BS), *gdt = calloc(e.gdt_blocks, BS), *sb = calloc(1, 1024);
    if(!blk || !gdt || !sb){ free(blk); free(gdt); free(sb); seterr(err, errn, "out of memory"); return -4; }
    int rc = -1;
    uint64_t free_blocks = 0;
    uint64_t dev_end = (uint64_t)e.blocks * BS;

    /* wipe: the first 4 MiB of the card (old MBR/GPT, old boot sectors) and its last MiB (a backup GPT) */
    if(emit_zero(w, 0, 4u << 20 < dev_end ? 4u << 20 : dev_end) != 0) goto out;
    if(t->bytes >= (8u << 20) && emit_zero(w, (t->bytes & ~(uint64_t)(BS - 1)) - (1u << 20), 1u << 20) != 0) goto out;

    /* group descriptors and bitmaps */
    for(uint32_t g = 0; g < e.ngroups; g++){
        uint32_t gs = grp_start(g), size = grp_size(&e, g), bb = grp_bb(&e, g), ib = bb + 1, it = bb + 2;
        memset(blk, 0, BS);
        uint32_t used = grp_meta(&e, g);
        for(uint32_t b = 0; b < used; b++) blk[b >> 3] |= (uint8_t)(1u << (b & 7));
        if(g == 0) for(uint32_t b = e.root_blk; b < e.lf_blk + LF_BLOCKS; b++, used++) blk[(b - gs) >> 3] |= (uint8_t)(1u << ((b - gs) & 7));
        if(g == e.jgroup) for(uint32_t b = e.jstart; b < e.jstart + e.jblocks; b++, used++) blk[(b - gs) >> 3] |= (uint8_t)(1u << ((b - gs) & 7));
        for(uint32_t b = size; b < BPG; b++) blk[b >> 3] |= (uint8_t)(1u << (b & 7));   /* padding past the card's end */
        if(emit(w, (uint64_t)bb * BS, blk, BS) != 0) goto out;
        memset(blk, 0, BS);
        memset(blk + IPG / 8, 0xFF, BS - IPG / 8);                                      /* padding past IPG */
        uint32_t iused = g == 0 ? FIRST_INO : 0;
        for(uint32_t i = 0; i < iused; i++) blk[i >> 3] |= (uint8_t)(1u << (i & 7));
        if(emit(w, (uint64_t)ib * BS, blk, BS) != 0) goto out;
        uint8_t *d = gdt + (size_t)g * 32;
        le32(d + 0, bb); le32(d + 4, ib); le32(d + 8, it);
        le16(d + 12, size - used); le16(d + 14, IPG - iused); le16(d + 16, g == 0 ? 2 : 0);
        le16(d + 18, g == 0 ? 0x4 : 0x1);       /* group 0: inode table zeroed; others: inode bitmap/table uninit */
        le16(d + 28, IPG - iused);              /* bg_itable_unused */
        free_blocks += size - used;
    }

    /* superblock */
    {
        memset(sb, 0, 1024);
        le32(sb + 0, e.ngroups * IPG); le32(sb + 4, e.blocks); le32(sb + 8, 0);
        le32(sb + 12, (uint32_t)free_blocks); le32(sb + 16, e.ngroups * IPG - FIRST_INO);
        le32(sb + 20, 0); le32(sb + 24, 2); le32(sb + 28, 2); le32(sb + 32, BPG); le32(sb + 36, BPG); le32(sb + 40, IPG);
        le32(sb + 48, t->now); le16(sb + 52, 0); le16(sb + 54, 0xFFFF); le16(sb + 56, 0xEF53); le16(sb + 58, 1); le16(sb + 60, 1);
        le32(sb + 64, t->now); le32(sb + 76, 1); le32(sb + 84, FIRST_INO); le16(sb + 88, ISZ);
        le32(sb + 92, 0x0004 | 0x0008 | 0x0020);                 /* has_journal, ext_attr, dir_index */
        le32(sb + 96, 0x0002 | 0x0040);                          /* filetype, extents */
        le32(sb + 100, 0x0001 | 0x0002 | 0x0008 | 0x0010 | 0x0020 | 0x0040);  /* sparse_super, large_file, huge_file, gdt_csum, dir_nlink, extra_isize */
        memcpy(sb + 104, t->rnd + 8, 16);
        sb[104 + 6] = (uint8_t)((sb[104 + 6] & 0x0F) | 0x40); sb[104 + 8] = (uint8_t)((sb[104 + 8] & 0x3F) | 0x80);   /* UUID v4 */
        char lab[16]; label_upper(lab, sizeof lab, t->label, 0); memcpy(sb + 120, lab, 16);
        le32(sb + 224, JOURNAL_INO);
        memcpy(sb + 236, t->rnd + 16, 16);                       /* s_hash_seed */
        sb[252] = 1;                                             /* half_md4 */
        sb[253] = 1;                                             /* s_jnl_backup_type: EXT3_JNL_BACKUP_BLOCKS */
        le32(sb + 264, t->now);
        {   uint8_t ji[ISZ]; e4_inode(ji, 0100600, 1, (uint64_t)e.jblocks * BS, e.jstart, e.jblocks, t->now);
            memcpy(sb + 268, ji + 40, 60);                       /* s_jnl_blocks[0..14] = the journal's i_block */
            le32(sb + 268 + 15 * 4, 0); le32(sb + 268 + 16 * 4, e.jblocks * BS);
        }
        le16(sb + 348, 32); le16(sb + 350, 32);
        le32(sb + 352, 0x1);                                     /* EXT2_FLAGS_SIGNED_HASH */
    }
    /* checksums of the group descriptors need the UUID from the superblock */
    for(uint32_t g = 0; g < e.ngroups; g++){
        uint8_t *d = gdt + (size_t)g * 32, gl[4]; le32(gl, g);
        uint16_t c = sdf_crc16(0xFFFF, sb + 104, 16);
        c = sdf_crc16(c, gl, 4);
        c = sdf_crc16(c, d, 30);
        le16(d + 30, c);
    }

    /* inode table of group 0 (the others stay uninitialised, see bg_flags), with root, journal and lost+found */
    {
        uint32_t it = grp_bb(&e, 0) + 2;
        if(emit_zero(w, (uint64_t)it * BS, (uint64_t)ITB * BS) != 0) goto out;
        memset(blk, 0, BS);                                       /* inodes 1..16 live in the first table block */
        e4_inode(blk + (ROOT_INO - 1) * ISZ, 040755, 3, BS, e.root_blk, 1, t->now);
        e4_inode(blk + (JOURNAL_INO - 1) * ISZ, 0100600, 1, (uint64_t)e.jblocks * BS, e.jstart, e.jblocks, t->now);
        e4_inode(blk + (LF_INO - 1) * ISZ, 040700, 2, (uint64_t)LF_BLOCKS * BS, e.lf_blk, LF_BLOCKS, t->now);
        if(emit(w, (uint64_t)it * BS, blk, BS) != 0) goto out;
    }
    /* root directory */
    memset(blk, 0, BS);
    le32(blk + 0, ROOT_INO); le16(blk + 4, 12); blk[6] = 1; blk[7] = 2; blk[8] = '.';
    le32(blk + 12, ROOT_INO); le16(blk + 16, 12); blk[18] = 2; blk[19] = 2; blk[20] = '.'; blk[21] = '.';
    le32(blk + 24, LF_INO); le16(blk + 28, BS - 24); blk[30] = 10; blk[31] = 2; memcpy(blk + 32, "lost+found", 10);
    if(emit(w, (uint64_t)e.root_blk * BS, blk, BS) != 0) goto out;
    /* lost+found: "." and "..", then empty blocks (room for e2fsck to reconnect files without allocating) */
    memset(blk, 0, BS);
    le32(blk + 0, LF_INO); le16(blk + 4, 12); blk[6] = 1; blk[7] = 2; blk[8] = '.';
    le32(blk + 12, ROOT_INO); le16(blk + 16, BS - 12); blk[18] = 2; blk[19] = 2; blk[20] = '.'; blk[21] = '.';
    if(emit(w, (uint64_t)e.lf_blk * BS, blk, BS) != 0) goto out;
    memset(blk, 0, BS); le16(blk + 4, BS);
    for(uint32_t i = 1; i < LF_BLOCKS; i++) if(emit(w, (uint64_t)(e.lf_blk + i) * BS, blk, BS) != 0) goto out;
    /* journal: zeroed, then its superblock (JBD2 v2, big-endian, empty: s_start 0) */
    if(emit_zero(w, (uint64_t)e.jstart * BS, (uint64_t)e.jblocks * BS) != 0) goto out;
    memset(blk, 0, BS);
    be32(blk + 0, 0xC03B3998); be32(blk + 4, 4); be32(blk + 8, 0);
    be32(blk + 12, BS); be32(blk + 16, e.jblocks); be32(blk + 20, 1); be32(blk + 24, 1); be32(blk + 28, 0);
    memcpy(blk + 48, sb + 104, 16);
    be32(blk + 64, 1);                                            /* s_nr_users */
    if(emit(w, (uint64_t)e.jstart * BS, blk, BS) != 0) goto out;

    /* backup superblocks and descriptor tables, then the primary (last, so a half-written card is not ext4) */
    for(uint32_t g = 1; g < e.ngroups; g++){
        if(!has_super(g)) continue;
        uint8_t bsb[1024]; memcpy(bsb, sb, 1024); le16(bsb + 90, (uint16_t)g);
        memset(blk, 0, BS); memcpy(blk, bsb, 1024);                /* 4 KiB blocks: a backup sits at the block start */
        if(emit(w, (uint64_t)grp_start(g) * BS, blk, BS) != 0) goto out;
        if(emit(w, ((uint64_t)grp_start(g) + 1) * BS, gdt, (size_t)e.gdt_blocks * BS) != 0) goto out;
    }
    if(emit(w, (uint64_t)1 * BS, gdt, (size_t)e.gdt_blocks * BS) != 0) goto out;
    memset(blk, 0, BS); memcpy(blk + 1024, sb, 1024);
    if(emit(w, 0, blk, BS) != 0) goto out;
    rc = 0;
out:
    free(blk); free(gdt); free(sb);
    if(rc == -1 && err && !err[0]) seterr(err, errn, "write failed");
    return rc;
}

int sdf_format(sdf_kind_t kind, const sdf_target_t *t, char *err, size_t errn){
    if(err && errn) err[0] = 0;
    if(!t || !t->write){ seterr(err, errn, "no target"); return -9; }
    if(t->bytes < SDF_MIN_BYTES){ seterr(err, errn, "card too small"); return -2; }
    wr_t w; memset(&w, 0, sizeof w); w.t = t;
    w.zero = calloc(1, ZBUF);
    if(!w.zero){ seterr(err, errn, "out of memory"); return -4; }
    int (*run)(wr_t *, char *, size_t) = kind == SDF_EXT4 ? ext4_run : fat32_run;
    w.count_only = 1;
    int rc = run(&w, err, errn);                     /* pass 1: validate the layout and count the bytes */
    if(rc == 0){
        w.count_only = 0;
        rc = run(&w, err, errn);
        if(rc == -1 && err && !err[0]) seterr(err, errn, "write failed");
    }
    free(w.zero);
    return rc;
}
