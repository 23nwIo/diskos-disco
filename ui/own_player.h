/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Own player switch (docs/MQ_PLAYER_REPLACEMENT.md, development phase). When /usr/data/diskos-own-player exists, the
 * launcher (mq_ui started as /usr/data/mq_player) execs /usr/data/diskos-player instead of the stock
 * /usr/bin/mq_player, under the name "mq_player" so fiio_init's `pgrep -x mq_player` still finds it. The own player
 * speaks the stock protocol (docs/PLAYER_PROTOCOL.md), so the UI does not change. Without the switch file nothing
 * changes at all.
 *
 * Fail safe, two counters; when either runs out the switch file is renamed to diskos-own-player.failed and the stock
 * player runs from then on:
 *   - every own-player start in this boot is counted in a tmpfs file (fiio_init respawns a dead player through the
 *     launcher, so a player that keeps dying is seen here): at most OPL_MAX_STARTS per boot;
 *   - every start is also counted in /usr/data/diskos-own-player.unstable, which the player deletes once it has run
 *     for a while (OPL_STABLE_S). A player that crashes early, or makes the Disc reboot (it arms the hardware
 *     watchdog like the stock player), leaves it behind: at most OPL_MAX_UNSTABLE such starts in a row, so a broken
 *     player cannot cause an endless reboot loop.
 * A missing or non-ELF binary also means stock (the switch file stays). The exec itself falls back to stock.
 * Header-only so the host test (ui/tests) can drive it with its own paths. Runs inside the launcher's deadline: plain
 * file calls only. */
#ifndef OWN_PLAYER_H
#define OWN_PLAYER_H
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OPL_SWITCH "/usr/data/diskos-own-player"
#define OPL_BIN    "/usr/data/diskos-player"
#define OPL_STARTS "/tmp/.diskos_own_player_starts"   /* tmpfs: counts this boot only */
#define OPL_UNSTABLE "/usr/data/diskos-own-player.unstable"   /* NAND: survives a reboot the player caused */
#define OPL_MAX_STARTS 3
#define OPL_MAX_UNSTABLE 2
#define OPL_STABLE_S 60     /* the player removes OPL_UNSTABLE after running this long */

enum { OPL_OFF = 0, OPL_ON, OPL_NO_BINARY, OPL_GAVE_UP };

typedef struct { const char *sw, *bin, *starts, *unstable; } opl_paths_t;

static inline const char *opl_reason(int r){
    switch(r){
    case OPL_OFF:       return "off";
    case OPL_ON:        return "on";
    case OPL_NO_BINARY: return "binary missing or not an executable ELF file - stock player started";
    case OPL_GAVE_UP:   return "started too often, or crashed early twice in a row - switched off (switch file renamed to .failed), stock player started";
    }
    return "?";
}

/* 1 if p is a regular executable file that starts with the ELF magic */
static inline int opl_binary_ok(const char *p){
    struct stat st;
    if(stat(p, &st) != 0 || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) return 0;
    unsigned char m[4] = {0};
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if(fd < 0) return 0;
    ssize_t n = read(fd, m, 4);
    close(fd);
    return n == 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
}

static inline int opl_read_int(const char *p){
    char b[16] = {0}; int n = 0;
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if(fd >= 0){ if(read(fd, b, sizeof b - 1) > 0) n = atoi(b); close(fd); }
    return (n < 0 || n > 1000) ? 0 : n;
}
static inline void opl_write_int(const char *p, int n, int sync){
    char b[16]; int len = snprintf(b, sizeof b, "%d\n", n);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if(fd < 0) return;
    if(write(fd, b, (size_t)len) != len){ /* best effort: an uncounted start only delays the give-up */ }
    if(sync) fsync(fd);
    close(fd);
}

/* Decide. OPL_ON: exec p->bin. Anything else: the stock player. Counts the start only when it returns OPL_ON. */
static inline int opl_prepare(const opl_paths_t *p, int *start_no){
    if(start_no) *start_no = 0;
    if(access(p->sw, F_OK) != 0) return OPL_OFF;
    int n = opl_read_int(p->starts), u = opl_read_int(p->unstable);
    if(n >= OPL_MAX_STARTS || u >= OPL_MAX_UNSTABLE){
        char failed[256];
        snprintf(failed, sizeof failed, "%s.failed", p->sw);
        if(rename(p->sw, failed) != 0) unlink(p->sw);
        unlink(p->unstable);
        return OPL_GAVE_UP;
    }
    if(!opl_binary_ok(p->bin)) return OPL_NO_BINARY;
    opl_write_int(p->unstable, u + 1, 1);      /* first, and synced: a reboot right after the exec must still count */
    opl_write_int(p->starts, n + 1, 0);
    if(start_no) *start_no = n + 1;
    return OPL_ON;
}
#endif
