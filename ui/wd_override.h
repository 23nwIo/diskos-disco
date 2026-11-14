/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef WD_OVERRIDE_H
#define WD_OVERRIDE_H
/* Our own cmd_watchdog (ui/watchdog, built into mq_ui as wd_blob.c) in place of the stock one, without a new flash.
 *
 * The stock player arms the hardware watchdog with `cmd_watchdog start 10000` and feeds it once per second with
 * `cmd_watchdog feed` (popen, no path, exit status ignored; docs/MQ_PLAYER_REPLACEMENT.md). Stock /usr/bin/cmd_watchdog
 * lives on the read-only squashfs. At every start mq_ui writes the embedded binary to /usr/data/diskos-watchdog (only
 * when the bytes differ, so the NAND is not rewritten each boot), checks it, and bind-mounts it over the stock tool.
 * The bind lives in RAM: after any reboot the stock tool is back until mq_ui runs again, and the Vol-Up stock boot never
 * runs mq_ui, so the stock firmware always keeps the original tool.
 *
 * Fail-safe, because a tool that cannot feed reboots the Disc every 10 s: the bind is made only when the file on disk
 * is byte-identical to the embedded copy (read back after writing) AND executing it with --help exits 0 within 2 s.
 * Anything else leaves the stock tool in place. /usr/data/diskos-no-wd-override switches the override off.
 * Idempotent: a target that already holds our exact bytes is left alone (mq_ui respawns); an older bind of ours is
 * detached before the new one, so binds never stack.
 * Header-only so ui/tests/wd_override_test.c can check it on the host. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define WDO_SRC "/usr/data/diskos-watchdog"
#define WDO_DST "/usr/bin/cmd_watchdog"
#define WDO_OFF "/usr/data/diskos-no-wd-override"
#define WDO_SELFTEST_MS 2000

enum { WDO_APPLIED = 0, WDO_ALREADY, WDO_DISABLED, WDO_NO_TARGET, WDO_WRITE_FAILED, WDO_VERIFY_FAILED,
       WDO_SELFTEST_FAILED, WDO_MOUNT_FAILED };

static inline const char *wdo_reason(int r){
    switch(r){
    case WDO_APPLIED:         return "applied";
    case WDO_ALREADY:         return "already active";
    case WDO_DISABLED:        return "switched off by " WDO_OFF;
    case WDO_NO_TARGET:       return "stock tool missing";
    case WDO_WRITE_FAILED:    return "could not write the binary";
    case WDO_VERIFY_FAILED:   return "binary on disk differs from the embedded copy";
    case WDO_SELFTEST_FAILED: return "self-test (--help) failed";
    default:                  return "bind mount failed";
    }
}

/* 1 when `path` is a regular file (symlinks are not followed) holding exactly data[0..len). */
static inline int wdo_same_bytes(const char *path, const unsigned char *data, size_t len){
    struct stat st;
    if(lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size != len) return 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if(fd < 0) return 0;
    unsigned char buf[4096]; size_t off = 0; int same = 1;
    while(same && off < len){
        ssize_t n = read(fd, buf, sizeof buf);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0 || off + (size_t)n > len || memcmp(buf, data + off, (size_t)n)) same = 0;
        else off += (size_t)n;
    }
    if(same){ ssize_t n = read(fd, buf, 1); if(n != 0) same = 0; }   /* nothing past len */
    close(fd);
    return same && off == len;
}

/* Write data to `path` via a temporary file + rename, mode 0755. 0 on success. */
static inline int wdo_write(const char *path, const unsigned char *data, size_t len){
    char tmp[256];
    if(snprintf(tmp, sizeof tmp, "%s.new", path) >= (int)sizeof tmp) return -1;
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0755);
    if(fd < 0) return -1;
    size_t off = 0;
    while(off < len){
        ssize_t n = write(fd, data + off, len - off);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0){ close(fd); unlink(tmp); return -1; }
        off += (size_t)n;
    }
    if(fchmod(fd, 0755) != 0 || fsync(fd) != 0){ close(fd); unlink(tmp); return -1; }
    if(close(fd) != 0 || rename(tmp, path) != 0){ unlink(tmp); return -1; }
    return 0;
}

/* Run `path --help` with no output; 0 when it exits 0 within WDO_SELFTEST_MS. */
static inline int wdo_selftest(const char *path){
    pid_t pid = fork();
    if(pid < 0) return -1;
    if(pid == 0){
        int nul = open("/dev/null", O_RDWR);
        if(nul >= 0){ dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        char *av[] = { (char *)"cmd_watchdog", (char *)"--help", NULL };
        execv(path, av);
        _exit(127);
    }
    for(int waited = 0; waited <= WDO_SELFTEST_MS; waited += 20){
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if(r == pid) return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
        if(r < 0 && errno != EINTR) return -1;
        struct timespec ts = { 0, 20 * 1000000L };
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

static inline int wdo_apply(const unsigned char *data, size_t len, const char *src, const char *dst, const char *off){
    struct stat d, s;
    if(off && access(off, F_OK) == 0) return WDO_DISABLED;
    if(stat(dst, &d) != 0 || !S_ISREG(d.st_mode)) return WDO_NO_TARGET;
    if(wdo_same_bytes(dst, data, len)) return WDO_ALREADY;            /* an earlier mq_ui already bound this build */
    if(!wdo_same_bytes(src, data, len)){
        if(wdo_write(src, data, len) != 0) return WDO_WRITE_FAILED;
        if(!wdo_same_bytes(src, data, len)) return WDO_VERIFY_FAILED;
    }
    if(wdo_selftest(src) != 0) return WDO_SELFTEST_FAILED;
    /* Detach any older bind of ours first. Required, not tidiness: an update replaces the source by rename, and the
     * kernel refuses (ENOENT) to bind onto a mount whose source was unlinked. On the plain stock file this is a no-op
     * (EINVAL: not a mount point). Bounded, in case binds were stacked by hand. */
    for(int i = 0; i < 4 && umount2(dst, MNT_DETACH) == 0; i++) {}
    if(mount(src, dst, NULL, MS_BIND, NULL) != 0) return WDO_MOUNT_FAILED;
    if(stat(src, &s) != 0 || stat(dst, &d) != 0 || s.st_dev != d.st_dev || s.st_ino != d.st_ino){
        umount2(dst, MNT_DETACH);
        return WDO_MOUNT_FAILED;
    }
    return WDO_APPLIED;
}

#endif
