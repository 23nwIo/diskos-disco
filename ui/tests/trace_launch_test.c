/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* trace_launch.h: the player trace starts only with the switch file, and switches itself off after a start that never
 * loaded the library. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "trace_launch.h"

static int fails;
#define EXPECT(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)
static char dir[64];
static char pbuf[8][160]; static int pn;
static char *path(const char *n){ char *b = pbuf[pn++ % 8]; snprintf(b, 160, "%s/%s", dir, n); return b; }
static void env_free(char **e){ if(!e) return; for(char **x = e; *x; x++) if(!strncmp(*x, "LD_PRELOAD=", 11) || !strncmp(*x, "DISKOS_TRACE_FILE=", 18)) free(*x); free(e); }
static void touch(const char *p){ FILE *f = fopen(p, "w"); if(f) fclose(f); }
#define LIB "\x7f" "ELF fake library"

int main(void){
    snprintf(dir, sizeof dir, "/tmp/trltestXXXXXX");
    if(!mkdtemp(dir)){ perror("mkdtemp"); return 2; }
    const char *libdl_none[] = { path("nolib/libdl.so.2"), NULL };
    const char *libdl_ok[] = { path("nolib/libdl.so.2"), path("libdl.so.2"), NULL };
    trl_paths_t p = { path("switch"), path("trace.so"), path("trace.ok"), path("trace.pending"), "/tmp/diskos-trace", libdl_ok };
    char *envp[] = { "PATH=/bin", "LD_PRELOAD=/old.so", "HOME=/", NULL };
    char **env = NULL;
    const unsigned char *d = (const unsigned char *)LIB; size_t n = sizeof LIB - 1;

    EXPECT(trl_prepare(&p, d, n, envp, &env) == TRL_OFF && !env);          /* no switch: nothing happens */
    EXPECT(!trl_exists(p.lib));
    touch(p.sw);
    p.libdl = libdl_none;
    EXPECT(trl_prepare(&p, d, n, envp, &env) == TRL_NO_LIBDL && !env);
    p.libdl = libdl_ok; touch(libdl_ok[1]);
    EXPECT(trl_prepare(&p, d, n, envp, &env) == TRL_ON && env);
    EXPECT(wdo_same_bytes(p.lib, d, n));
    EXPECT(trl_exists(p.pending) && !trl_exists(p.ok));
    int pre = 0, old = 0, cnt = 0;
    for(char **e = env; *e; e++){ cnt++; if(!strncmp(*e, "LD_PRELOAD=", 11)){ pre++; if(strstr(*e, "/old.so")) old++; } }
    EXPECT(cnt == 4 && pre == 1 && !old);
    char want[200]; snprintf(want, sizeof want, "LD_PRELOAD=%s", p.lib);
    EXPECT(!strcmp(env[2], want));
    EXPECT(!strncmp(env[3], "DISKOS_TRACE_FILE=/tmp/diskos-trace/player-trace-2", 50) && strstr(env[3], ".log"));
    touch(p.ok);                                                              /* the library loaded */
    env_free(env); env = NULL;
    EXPECT(trl_prepare(&p, d, n, envp, &env) == TRL_ON && env);              /* and is used again */
    env_free(env); env = NULL;                                                /* this start never wrote .ok */
    EXPECT(trl_prepare(&p, d, n, envp, &env) == TRL_FAILED_BEFORE && !env);
    EXPECT(!trl_exists(p.sw));
    char failed[200]; snprintf(failed, sizeof failed, "%s.failed", p.sw);
    EXPECT(trl_exists(failed) && !trl_exists(p.pending));
    EXPECT(trl_prepare(&p, d, n, envp, &env) == TRL_OFF && !env);           /* stays off */
    char cmd[128]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if(system(cmd)) {}
    if(fails){ fprintf(stderr, "trace_launch_test: %d failures\n", fails); return 1; }
    printf("trace_launch_test: ok\n");
    return 0;
}
