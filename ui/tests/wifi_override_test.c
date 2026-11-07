/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* wifi_override.h: only a sane, executable "#!" script in /usr/data may replace the stock wifi_up.sh. */
#include <stdio.h>
#include <stdlib.h>
#include "wifi_override.h"

static int fails;
#define EXPECT(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

static char dir[64];
static const char *path(const char *n){ static char b[4][128]; static int i; i = (i + 1) % 4; snprintf(b[i], sizeof b[i], "%s/%s", dir, n); return b[i]; }
static void put(const char *n, const char *body, mode_t mode){
    FILE *f = fopen(path(n), "w"); if(!f){ perror(n); exit(2); }
    fputs(body, f); fclose(f); chmod(path(n), mode);
}

int main(void){
    snprintf(dir, sizeof dir, "/tmp/wovtestXXXXXX");
    if(!mkdtemp(dir)){ perror("mkdtemp"); return 2; }
    put("stock.sh", "#!/bin/sh\necho stock\n", 0755);
    put("good.sh", "#!/bin/sh\necho mine\n", 0755);
    put("noexec.sh", "#!/bin/sh\necho mine\n", 0644);
    put("empty.sh", "", 0755);
    put("text.sh", "echo no shebang\n", 0755);
    if(symlink(path("good.sh"), path("link.sh")) != 0) perror("symlink");
    mkdir(path("adir"), 0755);

    EXPECT(wov_check_src(path("missing.sh")) == WOV_NONE);
    EXPECT(wov_check_src(path("good.sh")) == WOV_APPLIED);
    EXPECT(wov_check_src(path("noexec.sh")) == WOV_NOT_EXEC);
    EXPECT(wov_check_src(path("empty.sh")) == WOV_BAD_SIZE);
    EXPECT(wov_check_src(path("text.sh")) == WOV_NOT_SCRIPT);
    EXPECT(wov_check_src(path("link.sh")) == WOV_NOT_REGULAR);      /* a symlink is never followed */
    EXPECT(wov_check_src(path("adir")) == WOV_NOT_REGULAR);
    { FILE *f = fopen(path("big.sh"), "w"); fputs("#!/bin/sh\n", f); for(int i = 0; i < WOV_MAX_BYTES; i++) fputc('#', f); fclose(f); chmod(path("big.sh"), 0755); }
    EXPECT(wov_check_src(path("big.sh")) == WOV_BAD_SIZE);

    /* a bad override never reaches mount(): the stock script stays */
    EXPECT(wov_apply(path("noexec.sh"), path("stock.sh")) == WOV_NOT_EXEC);
    EXPECT(wov_apply(path("missing.sh"), path("stock.sh")) == WOV_NONE);
    EXPECT(wov_apply(path("good.sh"), path("nostock.sh")) == WOV_NO_TARGET);
    EXPECT(wov_apply(path("good.sh"), path("good.sh")) == WOV_ALREADY);   /* respawned mq_ui: already bound */

    /* the real bind, where this host allows it (root with CAP_SYS_ADMIN) */
    int r = wov_apply(path("good.sh"), path("stock.sh"));
    if(r == WOV_APPLIED){
        char buf[64] = {0}; FILE *f = fopen(path("stock.sh"), "r"); if(f){ if(!fgets(buf, sizeof buf, f)) buf[0] = 0; if(!fgets(buf, sizeof buf, f)) buf[0] = 0; fclose(f); }
        EXPECT(!strcmp(buf, "echo mine\n"));
        EXPECT(wov_apply(path("good.sh"), path("stock.sh")) == WOV_ALREADY);   /* no second, stacked bind */
        umount(path("stock.sh"));
        puts("  (bind mount exercised)");
    } else {
        EXPECT(r == WOV_MOUNT_FAILED);
        puts("  (bind mount not permitted here: skipped)");
    }

    char cmd[96]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if(system(cmd)) {}
    if(fails) return 1;
    puts("PASS wifi_up.sh override: only a sane executable script is bound, once");
    return 0;
}
