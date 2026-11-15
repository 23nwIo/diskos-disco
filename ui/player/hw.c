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
    run("killall bluetoothd bluealsa brcm_patchram_plus", dry, log);
    if(!dry) sync();
    run("poweroff -f", dry, log);
}
