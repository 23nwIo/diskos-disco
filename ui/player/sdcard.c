/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sdcard.h"

#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

int sd_card_present(void){
    struct stat sb;
    return stat("/sys/block/mmcblk0", &sb) == 0;
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
        { "vfat", "iocharset=utf8" }, { "exfat", "iocharset=utf8" }, { "ext4", "" },
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
