/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The screen lock shared by the own player and mq_ui: see panel.h. */
#include "panel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void panel_ioctl(const char *dev, unsigned long req, const char *what, panel_log_fn log){
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if(fd < 0){ log("screen: open %s: %s", dev, strerror(errno)); return; }
    if(ioctl(fd, req, 0) < 0) log("screen: %s: %s", what, strerror(errno));
    close(fd);
}
static void write_str(const char *path, const char *v, panel_log_fn log){
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if(fd < 0 || write(fd, v, strlen(v)) != (ssize_t)strlen(v)) log("screen: write %s to %s: %s", v, path, strerror(errno));
    if(fd >= 0) close(fd);
}
static int read_int(const char *path, panel_log_fn log){
    char b[16] = {0}; int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd < 0){ log("screen: open %s: %s", path, strerror(errno)); return -1; }
    ssize_t n = read(fd, b, sizeof b - 1); int e = errno; close(fd);
    if(n <= 0){ log("screen: read %s: %s", path, n < 0 ? strerror(e) : "empty"); return -1; }
    return atoi(b);
}

void panel_lock(panel_log_fn log){
    write_str(BACKLIGHT, "0", log);
    write_str(BL_POWER, "4", log);
    panel_ioctl(TOUCH_DEV, TOUCH_IOC_OFF, "touch off", log);
    panel_ioctl(LCD_DEV, LCD_IOC_OFF, "display off", log);
}

void panel_light(int level, panel_log_fn log){
    char b[12];
    if(level < 1) level = 1;
    if(level > 40) level = 40;
    snprintf(b, sizeof b, "%d", level);
    panel_ioctl(LCD_DEV, LCD_IOC_ON, "display on", log);
    write_str(BACKLIGHT, b, log);                   /* the level first, then the rail on (no flash at the old level) */
    write_str(BL_POWER, "0", log);
}

void panel_unlock(int level, panel_log_fn log){
    panel_light(level, log);
    panel_ioctl(TOUCH_DEV, TOUCH_IOC_ON, "touch on", log);
}

int panel_brightness(panel_log_fn log){ return read_int(BACKLIGHT, log); }

int panel_is_on(panel_log_fn log){
    int br = read_int(BACKLIGHT, log), bp = read_int(BL_POWER, log);
    if(br < 0 || bp < 0) return -1;
    return br > 0 && bp == 0;
}
