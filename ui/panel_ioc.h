/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Display panel and touch controller switches, as the stock player sends them on a power key press
 * (docs/MQ_PLAYER_TRACE.md, third run). MIPS encodings spelled out so the values never depend on the build host.
 * Shared by the own player (player/hw.c, power key) and the UI (settings.c, the screen lock). */
#ifndef DISKOS_PANEL_IOC_H
#define DISKOS_PANEL_IOC_H

#define LCD_DEV         "/dev/lcd_st77916"
#define LCD_IOC_OFF     0x2000ef01UL      /* _IO(0xef,1) display off */
#define LCD_IOC_ON      0x2000ef02UL      /* _IO(0xef,2) display on */
#define TOUCH_DEV       "/dev/cst816t"
#define TOUCH_IOC_OFF   0x2000ef03UL      /* _IO(0xef,3) touch controller off: no touch events at all */
#define TOUCH_IOC_ON    0x2000ef04UL      /* _IO(0xef,4) */
#define BACKLIGHT       "/sys/class/backlight/backlight/brightness"
#define BL_POWER        "/sys/class/backlight/backlight/bl_power"

#endif
