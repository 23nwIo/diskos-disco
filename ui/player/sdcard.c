/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sdcard.h"

#include <errno.h>
#include <dirent.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

int sd_uevent_kind(const char *msg, size_t len){
    int kind = SDEV_NONE, is_card = 0, is_disk = 0, is_block = 0;
    size_t i = 0;
    /* header "add@/devices/.../mmcblk0" */
    if(len >= 4 && !strncmp(msg, "add@", 4)) kind = SDEV_ADD;
    else if(len >= 7 && !strncmp(msg, "remove@", 7)) kind = SDEV_REMOVE;
    else return SDEV_NONE;
    while(i < len){
        const char *s = msg + i;
        size_t l = strnlen(s, len - i);
        if(l == 15 && !strncmp(s, "DEVNAME=mmcblk0", 15)) is_card = 1;
        else if(l == 12 && !strncmp(s, "DEVTYPE=disk", 12)) is_disk = 1;
        else if(l == 15 && !strncmp(s, "SUBSYSTEM=block", 15)) is_block = 1;
        i += l + 1;
    }
    return (is_card && is_disk && is_block) ? kind : SDEV_NONE;
}

int sd_uevent_open(void){
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if(fd < 0) return -1;
    struct sockaddr_nl a; memset(&a, 0, sizeof a);
    a.nl_family = AF_NETLINK; a.nl_groups = 1;     /* kernel uevents */
    int rcv = 256 * 1024;                          /* as busybox uevent: room for a burst (card + bdi + mmc events) */
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    if(bind(fd, (struct sockaddr *)&a, sizeof a) != 0){ close(fd); return -1; }
    return fd;
}

/* 1 if the mounted card can be read: a mount whose card has gone returns EIO */
static int sd_mount_alive(void){
    DIR *d = opendir(SD_MOUNTPOINT);
    if(!d) return 0;
    errno = 0;
    while(readdir(d)) {}
    int ok = errno == 0;
    closedir(d);
    return ok;
}

int sd_is_mounted(void){
    FILE *m = fopen("/proc/mounts", "re");
    if(!m) return 0;
    char line[512], dev[160], target[160]; int ok = 0;
    while(fgets(line, sizeof line, m))
        if(sscanf(line, "%159s %159s", dev, target) == 2 && !strcmp(target, SD_MOUNTPOINT)){ ok = 1; break; }
    fclose(m);
    return ok;
}

int sd_mount(sd_log_fn log){
    if(sd_is_mounted()){
        if(sd_mount_alive()){ log("sd: already mounted"); return 0; }
        int r = umount2(SD_MOUNTPOINT, MNT_DETACH);
        log("sd: the old mount is dead (card was pulled), detached: %s", r == 0 ? "ok" : strerror(errno));
    }
    struct stat sb;
    const char *dev = stat("/sys/class/block/mmcblk0p1", &sb) == 0 ? "/dev/mmcblk0p1" : "/dev/mmcblk0";
    if(mkdir(SD_MOUNTPOINT, 0755) != 0 && errno != EEXIST){ log("sd: mkdir %s: %s", SD_MOUNTPOINT, strerror(errno)); return -1; }
    static const struct { const char *type, *opts; } fs[] = {
        { "ext4", "" }, { "vfat", "iocharset=utf8" }, { "exfat", "iocharset=utf8" },   /* ext4 first (Jo 2026-11-15) */
    };
    for(unsigned i = 0; i < sizeof fs / sizeof fs[0]; i++){
        if(mount(dev, SD_MOUNTPOINT, fs[i].type, MS_NOATIME, fs[i].opts) == 0){
            log("sd: mounted %s (%s) on %s", dev, fs[i].type, SD_MOUNTPOINT);
            return 0;
        }
        int e = errno;
        if(e == EBUSY && sd_is_mounted()){ log("sd: mounted meanwhile by someone else"); return 0; }
        log("sd: mount %s as %s: %s", dev, fs[i].type, strerror(e));
    }
    return -1;
}

void sd_unmount(sd_log_fn log){
    if(!sd_is_mounted()){ log("sd: card pulled, nothing mounted"); return; }
    sync();
    if(umount(SD_MOUNTPOINT) == 0){ log("sd: card pulled, unmounted"); return; }
    int e = errno;
    if(umount2(SD_MOUNTPOINT, MNT_DETACH) == 0) log("sd: card pulled, umount: %s - detached instead", strerror(e));
    else log("sd: card pulled, umount failed: %s / detach: %s", strerror(e), strerror(errno));
}
