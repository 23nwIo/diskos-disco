/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* wd_override.h: our cmd_watchdog is bound only when its bytes on disk match the embedded copy and it runs. */
#include <stdio.h>
#include <stdlib.h>
#include "wd_override.h"

static int fails;
#define EXPECT(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

static char dir[64];
static const char *path(const char *n){ static char b[4][128]; static int i; i = (i + 1) % 4; snprintf(b[i], sizeof b[i], "%s/%s", dir, n); return b[i]; }
static void put(const char *n, const char *body, mode_t mode){
    FILE *f = fopen(path(n), "w"); if(!f){ perror(n); exit(2); }
    fputs(body, f); fclose(f); chmod(path(n), mode);
}
#define V1 "#!/bin/sh\n[ \"$1\" = --help ] && exit 0\nexit 1\n"
#define V2 "#!/bin/sh\n# v2\n[ \"$1\" = --help ] && exit 0\nexit 1\n"
#define BAD "#!/bin/sh\nexit 3\n"
#define U(s) (const unsigned char *)(s), sizeof(s) - 1

static int apply(const unsigned char *d, size_t n){
    char src[128], dst[128], off[128];
    snprintf(src, sizeof src, "%s", path("data/diskos-watchdog"));
    snprintf(dst, sizeof dst, "%s", path("bin/cmd_watchdog"));
    snprintf(off, sizeof off, "%s", path("data/off"));
    return wdo_apply(d, n, src, dst, off);
}

int main(void){
    snprintf(dir, sizeof dir, "/tmp/wdotestXXXXXX");
    if(!mkdtemp(dir)){ perror("mkdtemp"); return 2; }
    mkdir(path("data"), 0755); mkdir(path("bin"), 0755);

    EXPECT(apply(U(V1)) == WDO_NO_TARGET);                      /* no stock tool: nothing to replace */
    put("bin/cmd_watchdog", "#!/bin/sh\necho stock\n", 0755);
    put("data/off", "", 0644);
    EXPECT(apply(U(V1)) == WDO_DISABLED);
    unlink(path("data/off"));

    /* a binary that fails its self-test is written but never bound */
    EXPECT(apply(U(BAD)) == WDO_SELFTEST_FAILED);
    EXPECT(wdo_same_bytes(path("data/diskos-watchdog"), U(BAD)));
    EXPECT(!wdo_same_bytes(path("bin/cmd_watchdog"), U(BAD)));

    /* stale or foreign bytes at the source are replaced; a symlink is replaced, never followed */
    put("data/elsewhere", "keep me\n", 0644);
    unlink(path("data/diskos-watchdog"));
    if(symlink(path("data/elsewhere"), path("data/diskos-watchdog")) != 0) perror("symlink");
    int r = apply(U(V1));
    EXPECT(wdo_same_bytes(path("data/diskos-watchdog"), U(V1)));
    { FILE *f = fopen(path("data/elsewhere"), "r"); char b[16] = {0}; if(f){ if(!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); } EXPECT(!strcmp(b, "keep me\n")); }
    { struct stat st; EXPECT(stat(path("data/diskos-watchdog"), &st) == 0 && (st.st_mode & 0777) == 0755); }
    EXPECT(access(path("data/diskos-watchdog.new"), F_OK) != 0);   /* no temp file left */

    if(r == WDO_APPLIED){
        EXPECT(wdo_same_bytes(path("bin/cmd_watchdog"), U(V1)));
        EXPECT(apply(U(V1)) == WDO_ALREADY);                        /* respawned mq_ui: no second bind */
        { int r2 = apply(U(V2)); if(r2 != WDO_APPLIED) fprintf(stderr, "v2: %s\n", wdo_reason(r2)); EXPECT(r2 == WDO_APPLIED); }   /* an update replaces the old bind */
        EXPECT(wdo_same_bytes(path("bin/cmd_watchdog"), U(V2)));
        EXPECT(umount2(path("bin/cmd_watchdog"), MNT_DETACH) == 0);
        EXPECT(umount2(path("bin/cmd_watchdog"), MNT_DETACH) != 0);  /* exactly one bind: the old one was detached */
        { FILE *f = fopen(path("bin/cmd_watchdog"), "r"); char b[32] = {0}; if(f){ if(!fgets(b, sizeof b, f)) b[0] = 0; if(!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); } EXPECT(!strcmp(b, "echo stock\n")); }
        puts("  (bind mount exercised)");
    } else {
        EXPECT(r == WDO_MOUNT_FAILED);
        puts("  (bind mount not permitted here: skipped)");
    }

    char cmd[96]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if(system(cmd)) {}
    if(fails) return 1;
    puts("PASS cmd_watchdog override: bound only when verified and runnable, never stacked");
    return 0;
}
