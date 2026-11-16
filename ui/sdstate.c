/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sdstate.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SDS_NO_DEVICE
#include "sdio.h"
#include "system.h"
#endif

static const char *g_test_root;
void sds_test_root(const char *dir){ g_test_root = dir; }
static const char *root(void){ return g_test_root ? g_test_root : SDS_CARD; }

int sds_begin(void){
    if(g_test_root) return 1;
#ifndef SDS_NO_DEVICE
    /* not mounted: /tmp/sdcard is a plain folder in RAM, and anything written there would vanish */
    if(!sys_is_mounted("/proc/mounts", SDS_CARD)) return 0;
    return sd_io_begin();
#else
    return 0;
#endif
}
void sds_end(void){
#ifndef SDS_NO_DEVICE
    if(!g_test_root) sd_io_end();
#endif
}
int sds_path(char *out, size_t cap, const char *name){
    int n = snprintf(out, cap, "%s" SDS_SUBDIR "/%s", root(), name);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
int sds_mkdir(void){
    char p[512];
    snprintf(p, sizeof p, "%s/.diskos", root());
    if(mkdir(p, 0755) != 0 && errno != EEXIST) return -1;
    snprintf(p, sizeof p, "%s" SDS_SUBDIR, root());
    if(mkdir(p, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}
int sds_write(const char *name, const void *data, size_t n){
    char p[512], tmp[540];
    if(sds_path(p, sizeof p, name) != 0){ errno = ENAMETOOLONG; return -1; }
    if(sds_mkdir() != 0) return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(fd < 0) return -1;
    const char *b = data; size_t off = 0;
    while(off < n){
        ssize_t w = write(fd, b + off, n - off);
        if(w < 0){ if(errno == EINTR) continue; int e = errno; close(fd); unlink(tmp); errno = e; return -1; }
        off += (size_t)w;
    }
    if(fsync(fd) != 0 || close(fd) != 0){ int e = errno; unlink(tmp); errno = e; return -1; }
    if(rename(tmp, p) != 0){ int e = errno; unlink(tmp); errno = e; return -1; }
    return 0;
}
int sds_read(const char *name, char **data, size_t *n, size_t max){
    char p[512];
    *data = NULL; if(n) *n = 0;
    if(sds_path(p, sizeof p, name) != 0){ errno = ENAMETOOLONG; return -1; }
    FILE *f = fopen(p, "rb");
    if(!f) return errno == ENOENT ? 0 : -1;
    char *b = malloc(max + 1); size_t got = 0;
    if(!b){ fclose(f); return -1; }
    got = fread(b, 1, max, f);
    int err = ferror(f);
    fclose(f);
    if(err){ free(b); return -1; }
    b[got] = 0; *data = b; if(n) *n = got;
    return 1;
}
