/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "usbdrive_core.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UDC_MIN_BYTES (1024ULL * 1024ULL)

static unsigned long long read_sectors(const char *path){
    FILE *f = fopen(path, "r"); if(!f) return 0;
    unsigned long long v = 0;
    if(fscanf(f, "%llu", &v) != 1) v = 0;
    fclose(f);
    return v;
}
static int name_ok(const char *n){                   /* "sd" + letters, then optional digits: sda, sdb2, sdaa1 */
    if(strncmp(n, "sd", 2) != 0) return 0;
    const char *p = n + 2; int letters = 0;
    while(*p >= 'a' && *p <= 'z'){ p++; letters++; }
    if(!letters) return 0;
    while(*p >= '0' && *p <= '9') p++;
    return *p == 0 && strlen(n) < sizeof(((udc_dev_t *)0)->name);
}
static void add(udc_dev_t *out, int *n, int max, const char *name, unsigned long long bytes){
    if(*n >= max || bytes < UDC_MIN_BYTES) return;
    snprintf(out[*n].name, sizeof out[*n].name, "%s", name);
    out[*n].bytes = bytes; (*n)++;
}
static int cmp_dev(const void *a, const void *b){
    const udc_dev_t *x = a, *y = b;
    size_t lx = strlen(x->name), ly = strlen(y->name);      /* sdb2 before sdb10: shorter first within a disk */
    int c = strncmp(x->name, y->name, 3);
    if(c) return c;
    if(lx != ly) return lx < ly ? -1 : 1;
    return strcmp(x->name, y->name);
}

int udc_scan(const char *sysblock, udc_dev_t *out, int max){
    int n = 0;
    DIR *d = opendir(sysblock); if(!d) return 0;
    struct dirent *e;
    while((e = readdir(d)) && n < max){
        if(!name_ok(e->d_name)) continue;
        char disk[512]; snprintf(disk, sizeof disk, "%s/%s", sysblock, e->d_name);
        char p[640]; snprintf(p, sizeof p, "%s/size", disk);
        unsigned long long dbytes = read_sectors(p) * 512ULL;
        if(!dbytes) continue;                                  /* an empty card-reader slot */
        int parts = 0;
        DIR *pd = opendir(disk);
        if(pd){
            struct dirent *pe;
            size_t dl = strlen(e->d_name);
            while((pe = readdir(pd)) && n < max){
                if(strncmp(pe->d_name, e->d_name, dl) != 0 || !name_ok(pe->d_name) || !pe->d_name[dl]) continue;
                char pp[800]; snprintf(pp, sizeof pp, "%s/%s/partition", disk, pe->d_name);
                if(access(pp, F_OK) != 0) continue;
                parts++;
                snprintf(pp, sizeof pp, "%s/%s/size", disk, pe->d_name);
                add(out, &n, max, pe->d_name, read_sectors(pp) * 512ULL);
            }
            closedir(pd);
        }
        if(!parts) add(out, &n, max, e->d_name, dbytes);
    }
    closedir(d);
    if(n > 1) qsort(out, (size_t)n, sizeof *out, cmp_dev);
    return n;
}

int udc_ext_label(const char *devpath, char *out, size_t cap){
    if(cap) out[0] = 0;
    int fd = open(devpath, O_RDONLY | O_CLOEXEC); if(fd < 0) return 0;
    unsigned char sb[1024];
    ssize_t r = pread(fd, sb, sizeof sb, 1024);              /* the superblock starts 1 KiB in */
    close(fd);
    if(r != (ssize_t)sizeof sb || sb[0x38] != 0x53 || sb[0x39] != 0xEF) return 0;
    if(cap){
        size_t k = 0;
        for(; k < 16 && k + 1 < cap && sb[0x78 + k]; k++){
            unsigned char c = sb[0x78 + k];
            out[k] = (c < 0x20 || c == 0x7f) ? '_' : (char)c;
        }
        out[k] = 0;
        while(k && out[k - 1] == ' ') out[--k] = 0;
    }
    return 1;
}

void udc_fmt_size(unsigned long long b, char *out, size_t cap){
    if(b >= 100000000000ULL)    snprintf(out, cap, "%llu GB", (b + 500000000ULL) / 1000000000ULL);
    else if(b >= 1000000000ULL) snprintf(out, cap, "%llu.%llu GB", b / 1000000000ULL, (b % 1000000000ULL) / 100000000ULL);
    else                        snprintf(out, cap, "%llu MB", (b + 500000ULL) / 1000000ULL);
}

int udc_is_windows_fs(const char *t){
    return t && (!strcmp(t, "vfat") || !strcmp(t, "msdos") || !strcmp(t, "exfat") || !strcmp(t, "ntfs") || !strcmp(t, "fuseblk"));
}
