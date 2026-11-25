/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "dac.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DAC_A "/dev/cs43131"
#define DAC_C "/dev/cs43131c"
#define IOW1(nr)   (0x80014d00UL | (unsigned long)(nr))   /* _IOW('M',nr,1 byte), MIPS encoding */
#define IO_M48     0x20004d30UL
#define IO_M46     0x20004d2eUL
#define IO_M47     0x20004d2fUL   /* dac_power_down (mq_player 0x4e83b8, set_spdif_mode_link on) */

static int g_a = -1, g_c = -1, g_dry, g_reset_done;
static unsigned g_rate;

int dac_rate_code(unsigned r){
    switch(r){
    case 44100: return 1; case 48000: return 2; case 88200: return 3;
    case 96000: return 4; case 176400: return 5; case 192000: return 6;
    }
    return 0;
}

static unsigned char clamp_byte(int v){ return (unsigned char)(v < 0 ? 0 : v > 0xff ? 0xff : v); }

/* The stock player's volume curve (mq_player 0x4e7980, read from the binary 2026-11-15; it matches the trace's data
 * points): the CS43131 volume byte, 0.5 dB steps down from 0 dB, for volume 1..120 at high gain with the 3.5 mm
 * plug in. Steps of 8 from volume 1 (218), then 6 from 4, 4 from 11, 2 from 23 and 1 from 37, down to 0 at 120. */
int dac_curve(int v){
    if(v <= 0) return 0xff;
    if(v > 120) v = 120;
    if(v < 4)  return 218 - 8 * (v - 1);
    if(v < 11) return 196 - 6 * (v - 4);
    if(v < 23) return 156 - 4 * (v - 11);
    if(v < 37) return 110 - 2 * (v - 23);
    return 83 - (v - 37);
}

void dac_attenuation(int volume, int gain, int balanced, int balance, unsigned char *a, unsigned char *c){
    if(volume <= 0){ *a = *c = 0xff; return; }
    if(volume > 120) volume = 120;
    /* The stock player's four curves (0x4e7800 balanced/high, 0x4e78c0 balanced/low, 0x4e7980 3.5 mm/high,
     * 0x4e7a40 3.5 mm/low): low gain is 12 units (6 dB) more everywhere; the 4.4 mm balanced output 4 units (2 dB)
     * more up to volume 36, then 3, 2, 1 and none from 40. The stock player uses the low curve whenever no plug is
     * seen; the caller passes gain 0 then. */
    int base = dac_curve(volume) + (gain ? 0 : 12);
    if(balanced){ int d = 40 - volume; base += d > 4 ? 4 : d > 0 ? d : 0; }
    int steps = balance & 0xff, side = (balance >> 8) & 1;
    /* 0713 0x01NN turned A (right) down by NN in that trace; 0x00NN is taken as the other side (C, left). */
    *a = clamp_byte(base + (side == 1 ? steps : 0));
    *c = clamp_byte(base + (side == 0 ? steps : 0));
}

int dac_open(int dry, dac_log_fn log){
    g_dry = dry;
    if(dry){ log("dac: (dry run) open"); return 0; }
    if(g_a < 0) g_a = open(DAC_A, O_RDWR | O_CLOEXEC);
    if(g_c < 0) g_c = open(DAC_C, O_RDWR | O_CLOEXEC);
    if(g_a < 0 || g_c < 0){ log("dac: open: %s", strerror(errno)); return -1; }
    log("dac: open (fd %d, %d)", g_a, g_c);
    return 0;
}

void dac_close(void){
    if(g_a >= 0) close(g_a);
    if(g_c >= 0) close(g_c);
    g_a = g_c = -1; g_rate = 0;
}

static int w1(int fd, int nr, unsigned char v){
    if(g_dry || fd < 0) return 0;
    return ioctl(fd, IOW1(nr), &v);
}

int dac_start(unsigned rate, unsigned char a, unsigned char c, dac_log_fn log){
    int code = dac_rate_code(rate);
    if(!code){ log("dac: rate %u Hz not supported", rate); return -1; }
    if(!g_dry && (g_a < 0 || g_c < 0)) return -1;
    if(!g_reset_done){ if(!g_dry) ioctl(g_a, IO_M48, 0); g_reset_done = 1; }
    /* The stock order (trace run 1, 578 s). The second M56 returns an odd value with errno 2 on the stock player too. */
    w1(g_a, 56, 3); w1(g_c, 60, 3);
    if(!g_dry) usleep(400000);
    w1(g_a, 56, 0); w1(g_c, 60, 0);
    w1(g_a, 52, (unsigned char)code); w1(g_c, 54, (unsigned char)code);
    w1(g_a, 41, 0); w1(g_c, 50, 0);
    w1(g_a, 45, a); w1(g_a, 66, a); w1(g_c, 47, c); w1(g_c, 68, c);
    if(!g_dry) ioctl(g_a, IO_M46, 0);
    w1(g_a, 45, a); w1(g_a, 66, a); w1(g_c, 47, c); w1(g_c, 68, c);   /* the stock player repeats the group after M46 */
    g_rate = rate;
    log("dac: started at %u Hz (code %d)%s", rate, code, g_dry ? " (dry run)" : "");
    return 0;
}

void dac_set_attenuation(unsigned char a, unsigned char c, dac_log_fn log){
    w1(g_a, 45, a); w1(g_a, 66, a); w1(g_c, 47, c); w1(g_c, 68, c);   /* volume key: only this group (trace) */
    log("dac: attenuation right 0x%02x left 0x%02x", a, c);
}

unsigned dac_current_rate(void){ return g_rate; }

void dac_power_down(dac_log_fn log){
    if(!g_dry && g_a >= 0 && ioctl(g_a, IO_M47, 0) != 0) log("dac: power down: %s", strerror(errno));
    else log("dac: powered down%s", g_dry ? " (dry run)" : "");
    g_rate = 0; g_reset_done = 0;                    /* dac_start powers it up (M48) and starts it anew */
}
