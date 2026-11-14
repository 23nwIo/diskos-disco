/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Player trace switch (docs/MQ_PLAYER_TRACE.md). When /usr/data/diskos-trace-player exists, the launcher writes the
 * embedded diskos-trace.so (ui/trace, built by ui/Makefile) to /usr/data and starts the stock player with LD_PRELOAD
 * set to it. Nothing else changes: without the switch file the player starts exactly as before.
 *
 * Fail safe: the library writes /usr/data/diskos-trace.ok when it has loaded into the player. Before a traced start
 * the launcher removes that file and leaves a pending note; if the next start finds the note but no .ok, the glibc
 * loader did not get as far as our library (or the player died before it), so the switch file is renamed to
 * diskos-trace-player.failed and the player starts untraced. Header-only so the host test (ui/tests) can drive it with
 * its own paths. Runs inside the launcher's deadline: plain file calls only, and *env is set last, complete or not
 * at all, because the deadline handler may exec at any moment. */
#ifndef TRACE_LAUNCH_H
#define TRACE_LAUNCH_H
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "wd_override.h"     /* wdo_same_bytes / wdo_write */

#define TRL_SWITCH  "/usr/data/diskos-trace-player"
#define TRL_LIB     "/usr/data/diskos-trace.so"
#define TRL_OK      "/usr/data/diskos-trace.ok"
#define TRL_PENDING "/usr/data/diskos-trace.pending"

enum { TRL_OFF = 0, TRL_ON, TRL_FAILED_BEFORE, TRL_NO_LIBDL, TRL_WRITE_FAILED, TRL_ENV_FAILED };

typedef struct {
    const char *sw, *lib, *ok, *pending;
    const char *const *libdl;     /* NULL-terminated places where the device's glibc keeps libdl.so.2 */
} trl_paths_t;

static inline const char *trl_reason(int r){
    switch(r){
    case TRL_OFF:           return "off";
    case TRL_ON:            return "on";
    case TRL_FAILED_BEFORE: return "previous traced start did not load the library - switched off (switch file renamed to .failed)";
    case TRL_NO_LIBDL:      return "libdl.so.2 not found - not started";
    case TRL_WRITE_FAILED:  return "could not write the library - not started";
    case TRL_ENV_FAILED:    return "out of memory - not started";
    }
    return "?";
}

static inline int trl_exists(const char *p){ return access(p, F_OK) == 0; }

/* Decide and prepare. On TRL_ON, *env holds a copy of envp with LD_PRELOAD=<lib> (any earlier LD_PRELOAD dropped). */
static inline int trl_prepare(const trl_paths_t *p, const unsigned char *data, size_t len,
                              char *const *envp, char ***env){
    if(!trl_exists(p->sw)) return TRL_OFF;
    if(trl_exists(p->pending) && !trl_exists(p->ok)){
        char failed[256];
        snprintf(failed, sizeof failed, "%s.failed", p->sw);
        if(rename(p->sw, failed) != 0) unlink(p->sw);
        unlink(p->pending);
        return TRL_FAILED_BEFORE;
    }
    int dl = 0;
    for(const char *const *d = p->libdl; d && *d; d++) if(trl_exists(*d)){ dl = 1; break; }
    if(!dl) return TRL_NO_LIBDL;
    if(!wdo_same_bytes(p->lib, data, len) && (wdo_write(p->lib, data, len) != 0 || !wdo_same_bytes(p->lib, data, len)))
        return TRL_WRITE_FAILED;
    size_t n = 0;
    while(envp && envp[n]) n++;
    char **e = calloc(n + 2, sizeof *e);
    size_t plen = strlen(p->lib) + sizeof "LD_PRELOAD=";
    char *pre = malloc(plen);
    if(!e || !pre){ free(e); free(pre); return TRL_ENV_FAILED; }
    snprintf(pre, plen, "LD_PRELOAD=%s", p->lib);
    size_t k = 0;
    for(size_t i = 0; i < n; i++) if(strncmp(envp[i], "LD_PRELOAD=", 11) != 0) e[k++] = envp[i];
    e[k++] = pre; e[k] = NULL;
    unlink(p->ok);
    int fd = open(p->pending, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(fd >= 0) close(fd);
    *env = e;                     /* last: the deadline handler sees either nothing or the finished array */
    return TRL_ON;
}
#endif
