/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* own_player.h: the own player starts only with the switch file and a real binary, and gives up after too many
 * starts in one boot. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "own_player.h"

static int fails;
#define EXPECT(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)
static char dir[64];
static char pbuf[8][160]; static int pn;
static char *path(const char *n){ char *b = pbuf[pn++ % 8]; snprintf(b, 160, "%s/%s", dir, n); return b; }
static void put(const char *p, const char *s, int mode){ FILE *f = fopen(p, "w"); if(f){ fputs(s, f); fclose(f); } chmod(p, mode); }

int main(void){
    snprintf(dir, sizeof dir, "/tmp/opltestXXXXXX");
    if(!mkdtemp(dir)){ perror("mkdtemp"); return 2; }
    opl_paths_t p = { path("switch"), path("player"), path("starts"), path("unstable") };
    int no = -1;
    EXPECT(opl_prepare(&p, &no) == OPL_OFF && no == 0);                  /* no switch: stock, nothing counted */
    EXPECT(access(p.starts, F_OK) != 0);
    put(p.sw, "", 0644);
    EXPECT(opl_prepare(&p, &no) == OPL_NO_BINARY);                       /* switch but no binary */
    put(p.bin, "#!/bin/sh\n", 0755);
    EXPECT(opl_prepare(&p, &no) == OPL_NO_BINARY);                       /* not ELF */
    put(p.bin, "\x7f" "ELF....", 0644);
    EXPECT(opl_prepare(&p, &no) == OPL_NO_BINARY);                       /* not executable */
    chmod(p.bin, 0755);
    EXPECT(access(p.starts, F_OK) != 0);                                 /* refusals do not count */
    for(int i = 1; i <= OPL_MAX_STARTS; i++){
        EXPECT(opl_prepare(&p, &no) == OPL_ON && no == i);
        unlink(p.unstable);                                              /* each of these ran long enough */
    }
    EXPECT(opl_prepare(&p, &no) == OPL_GAVE_UP);                         /* one start too many this boot: off for good */
    EXPECT(access(p.sw, F_OK) != 0);
    char failed[200]; snprintf(failed, sizeof failed, "%s.failed", p.sw);
    EXPECT(access(failed, F_OK) == 0);
    EXPECT(opl_prepare(&p, &no) == OPL_OFF);
    unlink(p.starts); put(p.sw, "", 0644);                               /* a new boot */
    EXPECT(opl_prepare(&p, &no) == OPL_ON && no == 1);                   /* crashes early (unstable stays) */
    unlink(p.starts);                                                    /* ... and the Disc reboots */
    EXPECT(opl_prepare(&p, &no) == OPL_ON && no == 1);
    unlink(p.starts);
    EXPECT(opl_prepare(&p, &no) == OPL_GAVE_UP);                         /* two unstable starts in a row: off */
    EXPECT(access(p.unstable, F_OK) != 0);
    put(p.starts, "garbage", 0600); put(p.sw, "", 0644);                 /* an unreadable count starts again at 0 */
    EXPECT(opl_prepare(&p, &no) == OPL_ON && no == 1);
    char cmd[100]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if(system(cmd)){ /* cleanup only */ }
    if(fails){ fprintf(stderr, "own_player_test: %d failure(s)\n", fails); return 1; }
    printf("own_player_test: ok\n");
    return 0;
}
