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

void dac_attenuation(int volume, int gain, int balance, unsigned char *a, unsigned char *c){
    if(volume <= 0){ *a = *c = 0xff; return; }
    if(volume > 120) volume = 120;
    /* CS43131 volume byte: 0.5 dB steps down from 0 dB. One data point from trace 2026-11-15 02:41 (aa24 settings:
     * volume 27, gain 1, balance 0x0102): C 0x66, A 0x68; run 3: 2 units per volume step, gain low 12 units more. That
     * gives 156 - 2 x volume (0 dB at volume 78; louder levels clamp to 0 until more points are measured). */
    int base = 156 - 2 * volume + (gain ? 0 : 12);
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
