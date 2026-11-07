/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote Control: the Screen tab. See remote_screen.h.
 *
 * The picture: mq_ui draws only into /dev/fb0 (fb_pan.c renders straight into the page on screen; no overlay plane is
 * used), so the visible fb0 page IS the screen. It is read here on the main thread, between frames, and turned back
 * upright with the same mapping fb_pan uses (ctl_rot_map), so it matches what a person sees whatever the stored
 * Screen rotation is.
 *
 * Touch: a page tap or drag is played into the real touch device (the evdev pointer main.c made): its read callback is
 * wrapped, so LVGL, the main loop's swipe gestures (back, hub, ring seek) and the press-travel checks all see it as a
 * finger. A real finger always wins: the first real press drops a playing injection. */
#include "remote_screen.h"
#include "controls.h"
#include "screens.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/indev/lv_indev_private.h"   /* enabled: main.c turns touch off while the screen is dark */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

/* ---------------------------------------------------------------- the picture */
static int g_fd = -1;
static uint8_t *g_map;
static size_t g_maplen;

static int fb_open(void){
    if(g_map) return 0;
    int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if(fd < 0) return -1;
    struct fb_fix_screeninfo fi;
    if(ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0 || !fi.smem_len){ close(fd); return -1; }
    void *m = mmap(NULL, fi.smem_len, PROT_READ, MAP_SHARED, fd, 0);
    if(m == MAP_FAILED){ close(fd); return -1; }
    g_fd = fd; g_map = m; g_maplen = fi.smem_len;
    return 0;
}

static void le16(uint8_t *p, unsigned v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, unsigned long v){ le16(p, (unsigned)(v & 0xffff)); le16(p + 2, (unsigned)(v >> 16)); }

char *rscr_bmp(size_t *len, const char **why){
    *len = 0;
    if(fb_open()){ *why = "The screen can't be read"; return NULL; }
    struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    if(ioctl(g_fd, FBIOGET_VSCREENINFO, &vi) < 0 || ioctl(g_fd, FBIOGET_FSCREENINFO, &fi) < 0){ *why = "The screen can't be read"; return NULL; }
    int w = (int)vi.xres, h = (int)vi.yres;
    size_t off = (size_t)vi.yoffset * fi.line_length;
    if(vi.bits_per_pixel != 32 || w <= 0 || h <= 0 || w > 2048 || h > 2048 ||
       off + (size_t)(h - 1) * fi.line_length + (size_t)w * 4 > g_maplen){ *why = "Unexpected screen format"; return NULL; }
    size_t row = ((size_t)w * 3 + 3) & ~(size_t)3, n = 54 + row * (size_t)h;
    uint8_t *b = calloc(1, n);
    if(!b){ *why = "Out of memory"; return NULL; }
    b[0] = 'B'; b[1] = 'M'; le32(b + 2, n); le32(b + 10, 54);
    le32(b + 14, 40); le32(b + 18, (unsigned long)w); le32(b + 22, (unsigned long)h); le16(b + 26, 1); le16(b + 28, 24);
    le32(b + 34, row * (size_t)h); le32(b + 38, 2835); le32(b + 42, 2835);
    const uint8_t *base = g_map + off;
    int rot = ctl_rot_get();
    for(int y = 0; y < h; y++){
        uint8_t *d = b + 54 + row * (size_t)(h - 1 - y);    /* bottom-up rows: every browser reads these */
        for(int x = 0; x < w; x++){
            int rx, ry; ctl_rot_map(rot, w, h, x, y, &rx, &ry);
            uint32_t p = ((const uint32_t *)(base + (size_t)ry * fi.line_length))[rx];   /* XRGB8888 */
            d[0] = (uint8_t)p; d[1] = (uint8_t)(p >> 8); d[2] = (uint8_t)(p >> 16); d += 3;
        }
    }
    *len = n;
    return (char *)b;
}

/* ---------------------------------------------------------------- touch */
#define INJ_START_MS 2000          /* a gesture LVGL never got to read (a screen change in progress) is dropped */
static lv_indev_t *g_in;
static lv_indev_read_cb_t g_orig;
static struct {
    int on, started, at_end;
    int x1, y1, x2, y2, ms;
    uint32_t queued, t0;
} g_inj;

static void inj_read(lv_indev_t *in, lv_indev_data_t *d){
    g_orig(in, d);                                          /* the real panel first: drains it, keeps its state current */
    if(!g_inj.on) return;
    if(d->state == LV_INDEV_STATE_PRESSED){ g_inj.on = 0; return; }   /* a real finger wins */
    uint32_t now = lv_tick_get();
    if(!g_inj.started){
        if(lv_tick_elaps(g_inj.queued) > INJ_START_MS){ g_inj.on = 0; return; }   /* too late to mean anything */
        g_inj.started = 1; g_inj.t0 = now;
        d->point.x = g_inj.x1; d->point.y = g_inj.y1; d->state = LV_INDEV_STATE_PRESSED;
        return;
    }
    uint32_t t = now - g_inj.t0;                           /* wraps correctly in uint32_t */
    if(t < (uint32_t)g_inj.ms){
        d->point.x = g_inj.x1 + (int)((long)(g_inj.x2 - g_inj.x1) * (long)t / g_inj.ms);
        d->point.y = g_inj.y1 + (int)((long)(g_inj.y2 - g_inj.y1) * (long)t / g_inj.ms);
        d->state = LV_INDEV_STATE_PRESSED;
        return;
    }
    d->point.x = g_inj.x2; d->point.y = g_inj.y2;
    if(!g_inj.at_end){ g_inj.at_end = 1; d->state = LV_INDEV_STATE_PRESSED; return; }   /* the end point, still down */
    d->state = LV_INDEV_STATE_RELEASED;
    g_inj.on = 0;
}

static lv_indev_t *touch_indev(void){
    if(g_in) return g_in;
    for(lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)){
        if(lv_indev_get_type(i) != LV_INDEV_TYPE_POINTER) continue;
        lv_indev_read_cb_t cb = lv_indev_get_read_cb(i);
        if(!cb) continue;
        g_orig = cb; g_in = i;
        lv_indev_set_read_cb(i, inj_read);
        return i;
    }
    return NULL;
}

int rscr_touch(int x1, int y1, int x2, int y2, int ms, const char **why){
    lv_indev_t *in = touch_indev();
    if(!in){ *why = "Touch is turned off on the Disc"; return -1; }
    if(g_inj.on && lv_tick_elaps(g_inj.queued) < (uint32_t)(INJ_START_MS + g_inj.ms)){ *why = "The last touch is still playing"; return -1; }
    if(!in->enabled || ui_display_is_off()){                /* a dark screen ignores touch; light it as a touch would */
        ui_request_wake();
        *why = "The screen was dark - it is on now, tap again";
        return -1;
    }
    g_inj.x1 = x1; g_inj.y1 = y1; g_inj.x2 = x2; g_inj.y2 = y2;
    g_inj.ms = ms < 60 ? 60 : ms;                           /* a tap still spans a few reads, like a quick finger */
    g_inj.started = g_inj.at_end = 0; g_inj.queued = lv_tick_get(); g_inj.on = 1;
    return 0;
}
