/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The screen lock, one implementation for both programs (Jo 2026-11-23): compiled into the own player (player/hw.c,
 * power key) and into mq_ui (settings.c, the idle timeout and the Sleep tile), so every way the screen goes off leaves
 * the same state and every way it comes back runs the same steps. Device switches as the stock player sends them on a
 * power key press (docs/MQ_PLAYER_TRACE.md, third run); MIPS encodings spelled out so the values never depend on the
 * build host. */
#ifndef DISKOS_PANEL_H
#define DISKOS_PANEL_H

#define LCD_DEV         "/dev/lcd_st77916"
#define LCD_IOC_OFF     0x2000ef01UL      /* _IO(0xef,1) display off */
#define LCD_IOC_ON      0x2000ef02UL      /* _IO(0xef,2) display on */
#define TOUCH_DEV       "/dev/cst816t"
#define TOUCH_IOC_OFF   0x2000ef03UL      /* _IO(0xef,3) touch controller off: no touch events at all */
#define TOUCH_IOC_ON    0x2000ef04UL      /* _IO(0xef,4) */
#define BACKLIGHT       "/sys/class/backlight/backlight/brightness"
#define BL_POWER        "/sys/class/backlight/backlight/bl_power"


typedef void (*panel_log_fn)(const char *fmt, ...);

/* Locked: brightness 0, bl_power 4 (brightness 0 alone keeps the LED driver at its minimum), touch controller off (no
 * touch events at all, so nothing wakes in a pocket), panel off. Errors go to log. */
void panel_lock(panel_log_fn log);
/* Back on: panel on, brightness `level` (1..40), bl_power 0, touch controller on. */
void panel_unlock(int level, panel_log_fn log);
/* Visible but still locked: panel on, brightness `level`, bl_power 0; the touch controller stays off. For a short
 * notice on a locked screen (the charger plugged in); panel_lock ends it. */
void panel_light(int level, panel_log_fn log);
/* What the user sees: brightness above 0 and the rail powered. -1 when the backlight can't be read (log says why). */
int panel_is_on(panel_log_fn log);
/* The backlight brightness, -1 when unreadable (log says why). */
int panel_brightness(panel_log_fn log);

#endif
