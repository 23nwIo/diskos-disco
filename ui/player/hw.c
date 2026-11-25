/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "hw.h"
#include "../watchdog/watchdog.h"
#include "../panel.h"
#include "../powersrc.h"

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
#define RFKILL0         "/sys/class/rfkill/rfkill0/state"
#define GPIO_IOC_LEVEL  0x2000477aUL      /* _IO('G',122): level of the pin named by the argument */
#define BL_STOCK_LEVEL  31                /* what the stock player writes; the UI then sets its own level */

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

int hw_screen_toggle(int dry, hw_log_fn log){
    /* One lock for every screen off (ui/panel.c, shared with the UI's idle timeout and Sleep tile): a press always
     * turns a locked screen on, whoever locked it, and the UI follows the brightness (ui/main.c, PW-10). */
    int br = panel_brightness(log);
    int on = panel_is_on(log) == 1;
    log("power key: screen %s -> %s (brightness %d)%s", on ? "on" : "off", on ? "off" : "on", br, dry ? " (dry run)" : "");
    if(dry) return !on;
    if(on) panel_lock(log);
    else   panel_unlock(BL_STOCK_LEVEL, log);
    return !on;
}

int hw_jack_open(void){ return open(JACK_DEV, O_RDWR | O_CLOEXEC); }

int hw_jack_read(int fd){
    /* the stock player's buffer: "pb20", NUL padded (its next bytes are unrelated text, so the driver reads a string) */
    char pin[16] = "pb20";
    int r = ioctl(fd, GPIO_IOC_LEVEL, pin);
    if(r < 0) return -1;
    return r ? 1 : 0;
}

#define ADC_IOC_ENABLE 0x2000410bUL       /* _IO('A',11), what the stock player sends after opening the ADC */

int hw_bal_open(void){
    int fd = open(BAL_DEV, O_RDWR | O_CLOEXEC);
    if(fd < 0) return -1;
    if(ioctl(fd, ADC_IOC_ENABLE, 0) < 0){ int e = errno; close(fd); errno = e; return -1; }
    return fd;
}

int hw_bal_read(int fd){
    int v = 0;
    ssize_t n = read(fd, &v, sizeof v);
    if(n != (ssize_t)sizeof v){ if(n >= 0) errno = EIO; return -1; }
    return v;
}

int hw_bal_level(int v){
    if(v >= 1501 || (v >= 801 && v <= 949)) return 1;
    if(v >= 1151 && v <= 1349) return 0;
    return -1;
}

int hw_debounce(int *state, int *count, int level){
    if(level < 0) return 0;
    if(*state < 0){ *state = level; *count = 0; return 1; }
    if(level == *state){ *count = 0; return 0; }
    if(++*count < 4) return 0;
    *state = level; *count = 0;
    return 1;
}

void hw_charge_init(hw_charge_t *s){ memset(s, 0, sizeof *s); s->sent = -1; s->logged_pct = -1; s->polled_at = -1e9; }


int hw_charge_step(hw_charge_t *s, int limit_on, int pct, int power, double now){
    if(power == 0 || power == -1){ s->sent = -1; return -1; }       /* nothing to charge from, or the Disc is the source */
    int want;
    if(!limit_on) want = 1;
    else if(pct < 0) return -1;                                       /* no gauge: leave it as it is */
    else want = pct >= CHG_LIMIT_PCT ? 0 : 1;
    if(want == s->sent && !(want == 0 && (now - s->sent_at) * 1000.0 >= CHG_REASSERT_MS)) return -1;
    if(want == 1 && s->sent < 0 && !limit_on && power == 2) return -1; /* stock: limit off and no USB-C signal = untouched */
    s->sent = want; s->sent_at = now;
    return want;
}


static void charge_send(int on, const char *pct_text, int dry, hw_log_fn log){
    if(dry) return;
    int fd = open(CHG_DEV, O_RDWR | O_CLOEXEC);
    if(fd < 0){ log("charge: open %s: %s", CHG_DEV, strerror(errno)); return; }
    /* the stock player passes its capacity text buffer as the argument (trace: "98\n"); the same here */
    char arg[16]; snprintf(arg, sizeof arg, "%s", pct_text ? pct_text : "");
    if(ioctl(fd, on ? CHG_IOC_ON : CHG_IOC_OFF, arg) < 0)
        log("charge: %s ioctl 0x%08lx: %s", CHG_DEV, on ? CHG_IOC_ON : CHG_IOC_OFF, strerror(errno));
    close(fd);
}

void hw_charge_tick(hw_charge_t *s, int limit_on, double now, int dry, hw_log_fn log){
    if((now - s->polled_at) * 1000.0 < CHG_POLL_MS) return;
    s->polled_at = now;
    char cap[32] = "";
    int pct = -1;
    powersrc_gauge(&pct, NULL);                                        /* the shared reads (ui/powersrc.c) */
    if(pct >= 0) snprintf(cap, sizeof cap, "%d", pct);
    int power = powersrc_port();                                      /* cc_term + typec_state: a sound card is no charger */
    if(power == PS_PORT_NONE){
        if(!s->cc_missing_logged){ s->cc_missing_logged = 1; log("charge: %s: missing - limit without the plug check", PS_CC_TERM); }
        power = 2;
    }
    int was = s->sent;
    int act = hw_charge_step(s, limit_on, pct, power, now);
    if(act < 0) return;
    cap[strcspn(cap, "\r\n")] = 0;
    if(act != was) log("charge: limit %s, battery %d %%, power %s -> charging %s%s", limit_on ? "on" : "off", pct,
                       power == 1 ? "in" : "(not checked)", act ? "on" : "off", dry ? " (dry run)" : "");
    strcat(cap, "\n");
    charge_send(act, cap, dry, log);
}

void hw_charge_release(hw_charge_t *s, int dry, hw_log_fn log){
    if(s->sent != 0) return;
    log("charge: player stops, charging back on%s", dry ? " (dry run)" : "");
    charge_send(1, "", dry, log);
    s->sent = 1;
}
