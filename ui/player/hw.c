/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "hw.h"
#include "../watchdog/watchdog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* MIPS encodings, spelled out like watchdog.h so the values never depend on the build host */
#define DAC_A           "/dev/cs43131"
#define DAC_IOC_M48     0x20004d30UL      /* _IO('M',48): sent on A before the very first stream and at power off */
#define LCD_DEV         "/dev/lcd_st77916"
#define LCD_IOC_OFF     0x2000ef01UL      /* _IO(0xef,1) display off */
#define BACKLIGHT       "/sys/class/backlight/backlight/brightness"
#define BL_POWER        "/sys/class/backlight/backlight/bl_power"
#define LCD_IOC_ON      0x2000ef02UL      /* _IO(0xef,2) display on */
#define TOUCH_DEV       "/dev/cst816t"
#define TOUCH_IOC_OFF   0x2000ef03UL      /* _IO(0xef,3) */
#define TOUCH_IOC_ON    0x2000ef04UL      /* _IO(0xef,4) */
#define RFKILL0         "/sys/class/rfkill/rfkill0/state"
#define BL_STOCK_LEVEL  "31"              /* what the stock player writes; the UI then sets its own level */

static void run(const char *cmd, int dry, hw_log_fn log){
    log("power off: %s", cmd);
    if(dry) return;
    int r = system(cmd);
    if(r != 0) log("power off: \"%s\" returned %d", cmd, r);
}
static void dev_ioctl(const char *dev, unsigned long req, const char *what, int dry, hw_log_fn log){
    log("power off: %s (%s ioctl 0x%08lx)", what, dev, req);
    if(dry) return;
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if(fd < 0){ log("power off: open %s: %s", dev, strerror(errno)); return; }
    if(ioctl(fd, req, 0) < 0) log("power off: %s: %s", what, strerror(errno));
    close(fd);
}

void hw_power_off(int wd_fd, int dry, hw_log_fn log){
    /* Order as the stock player's menu power off (trace run 3). The UI has already flushed and unmounted what it
     * owns before it asked for this (ui/power.c). */
    run("hciconfig hci0 down", dry, log);
    /* Not in the stock routine: end the SSH sessions while Wi-Fi is still up, so a connected client gets a clean
     * close instead of a timeout (Jo 2026-11-15: "server not responding" after the menu power off). */
    run("killall dropbear", dry, log);
    run("ifconfig wlan0 down", dry, log);
    run("killall udhcpc wpa_supplicant curl wget", dry, log);
    dev_ioctl(DAC_A, DAC_IOC_M48, "DAC power down", dry, log);
    log("power off: watchdog stop");
    if(!dry && wd_fd >= 0){ int r = wd_stop(wd_fd); if(r < 0) log("power off: watchdog stop: %s", strerror(-r)); }
    run("killall -9 fiio_init.sh", dry, log);      /* or its loop would restart a player that is gone */
    log("power off: backlight 0");
    if(!dry){
        int fd = open(BACKLIGHT, O_WRONLY | O_CLOEXEC);
        if(fd < 0 || write(fd, "0", 1) != 1) log("power off: backlight: %s", strerror(errno));
        if(fd >= 0) close(fd);
    }
    dev_ioctl(LCD_DEV, LCD_IOC_OFF, "display off", dry, log);
    run("killall -9 mq_ui", dry, log);
    log("power off: rfkill0 state 0");             /* the stock player's "rfkill 0" (trace run 4: this sysfs file) */
    if(!dry){
        int fd = open(RFKILL0, O_WRONLY | O_CLOEXEC);
        if(fd < 0 || write(fd, "0", 1) != 1) log("power off: rfkill0: %s", strerror(errno));
        if(fd >= 0) close(fd);
    }
    run("killall bluetoothd bluealsa brcm_patchram_plus", dry, log);
    if(!dry) sync();
    run("poweroff -f", dry, log);
}

static int read_int(const char *p){
    char b[16] = {0}; int fd = open(p, O_RDONLY | O_CLOEXEC);
    if(fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd);
    return n > 0 ? atoi(b) : -1;
}
static void write_str(const char *p, const char *v, hw_log_fn log){
    int fd = open(p, O_WRONLY | O_CLOEXEC);
    if(fd < 0 || write(fd, v, strlen(v)) != (ssize_t)strlen(v)) log("screen: write %s to %s: %s", v, p, strerror(errno));
    if(fd >= 0) close(fd);
}
static void quiet_ioctl(const char *dev, unsigned long req, const char *what, hw_log_fn log){
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if(fd < 0){ log("screen: open %s: %s", dev, strerror(errno)); return; }
    if(ioctl(fd, req, 0) < 0) log("screen: %s: %s", what, strerror(errno));
    close(fd);
}

int hw_screen_toggle(int dry, hw_log_fn log){
    /* "On" = what the user sees: brightness above 0 and the rail powered (the UI's own idle screen off only sets
     * bl_power 4 and leaves the brightness, ui/main.c ui_backlight). */
    int br = read_int(BACKLIGHT), bp = read_int(BL_POWER);
    int on = br > 0 && bp == 0;
    log("power key: screen %s -> %s (brightness %d, bl_power %d)%s", on ? "on" : "off", on ? "off" : "on", br, bp,
        dry ? " (dry run)" : "");
    if(dry) return !on;
    if(on){
        write_str(BACKLIGHT, "0", log);
        quiet_ioctl(TOUCH_DEV, TOUCH_IOC_OFF, "touch off", log);
        quiet_ioctl(LCD_DEV, LCD_IOC_OFF, "display off", log);
    } else {
        quiet_ioctl(LCD_DEV, LCD_IOC_ON, "display on", log);
        write_str(BACKLIGHT, BL_STOCK_LEVEL, log);
        quiet_ioctl(TOUCH_DEV, TOUCH_IOC_ON, "touch on", log);
    }
    return !on;
}
