/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef WIFI_OVERRIDE_H
#define WIFI_OVERRIDE_H
/* User override for the stock Wi-Fi bring-up script, without a new flash.
 *
 * Stock /usr/bin/wifi_up.sh lives on the read-only squashfs. It starts udhcpc with
 * `-x hostname:FIIO_$product_model`, and on the Disc /etc/product_version/ has no model
 * directory, so the DHCP server sees the hostname "FIIO_". If /usr/data/wifi_up.sh exists,
 * mq_ui bind-mounts it over /usr/bin/wifi_up.sh at start, so every caller (mq_ui's keepalive
 * and the stock player) runs the user's copy. Same mechanism as the debug /etc/shadow overlay.
 *
 * Fail-safe: a copy that is not a regular file (a symlink is refused), not executable by its
 * owner, empty, oversized or not a "#!" script is ignored and the stock script stays in use,
 * because a broken bind would leave Wi-Fi unable to start. Idempotent: mq_ui respawns, and a
 * target that already is the override (same device and inode) is left as it is.
 * Header-only so ui/tests/wifi_override_test.c can check it on the host. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>

#define WOV_SRC "/usr/data/wifi_up.sh"
#define WOV_DST "/usr/bin/wifi_up.sh"
#define WOV_MAX_BYTES (64 * 1024)

enum { WOV_APPLIED = 0, WOV_ALREADY, WOV_NONE, WOV_NOT_REGULAR, WOV_NOT_EXEC, WOV_BAD_SIZE,
       WOV_NOT_SCRIPT, WOV_NO_TARGET, WOV_MOUNT_FAILED };

static inline const char *wov_reason(int r){
    switch(r){
    case WOV_APPLIED:      return "applied";
    case WOV_ALREADY:      return "already active";
    case WOV_NONE:         return "no override";
    case WOV_NOT_REGULAR:  return "not a regular file";
    case WOV_NOT_EXEC:     return "not executable (chmod +x)";
    case WOV_BAD_SIZE:     return "empty or too large";
    case WOV_NOT_SCRIPT:   return "does not start with #!";
    case WOV_NO_TARGET:    return "stock script missing";
    default:               return "bind mount failed";
    }
}

/* Is `src` usable as a replacement script? WOV_APPLIED (0) when it is, else the reason. */
static inline int wov_check_src(const char *src){
    struct stat st;
    if(lstat(src, &st) != 0) return WOV_NONE;
    if(!S_ISREG(st.st_mode)) return WOV_NOT_REGULAR;
    if(!(st.st_mode & S_IXUSR)) return WOV_NOT_EXEC;
    if(st.st_size < 3 || st.st_size > WOV_MAX_BYTES) return WOV_BAD_SIZE;
    int fd = open(src, O_RDONLY | O_NOFOLLOW);
    if(fd < 0) return WOV_NOT_REGULAR;
    char hdr[2] = {0, 0};
    ssize_t n = read(fd, hdr, 2);
    close(fd);
    if(n != 2 || hdr[0] != '#' || hdr[1] != '!') return WOV_NOT_SCRIPT;
    return WOV_APPLIED;
}

/* Bind `src` over `dst` if `src` passes wov_check_src. Returns a WOV_* code. */
static inline int wov_apply(const char *src, const char *dst){
    int r = wov_check_src(src);
    if(r != WOV_APPLIED) return r;
    struct stat s, d;
    if(stat(dst, &d) != 0 || !S_ISREG(d.st_mode)) return WOV_NO_TARGET;
    if(stat(src, &s) != 0) return WOV_NONE;
    if(s.st_dev == d.st_dev && s.st_ino == d.st_ino) return WOV_ALREADY;   /* an earlier mq_ui did it */
    if(mount(src, dst, NULL, MS_BIND, NULL) != 0) return WOV_MOUNT_FAILED;
    return WOV_APPLIED;
}

#endif
