/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Power source and battery, read in ONE place (Jo 2026-11-25): compiled into mq_ui (usage.c owns the shared state that
 * the status bar, the statistics, idle power-off, the library migration, cover prewarm and the charger notice read)
 * and into the own player (player/hw.c, Charging Limit). No LVGL, no state: callers decide how often to read. */
#ifndef DISKOS_POWERSRC_H
#define DISKOS_POWERSRC_H

#ifndef PS_GAUGE_CAPACITY
#define PS_GAUGE_CAPACITY "/sys/class/power_supply/cw221X-bat/capacity"
#endif
#ifndef PS_GAUGE_CURRENT
#define PS_GAUGE_CURRENT  "/sys/class/power_supply/cw221X-bat/current_now"
#endif
#ifndef PS_CC_TERM
#define PS_CC_TERM        "/sys/bus/i2c/devices/2-0022/control/cc_term"
#endif
#ifndef PS_TYPEC_STATE
#define PS_TYPEC_STATE    "/sys/bus/i2c/devices/2-0022/control/typec_state"
#endif

/* The fuel gauge (cw221X): *pct = level 0..100 or -1, *charging = 1 while the battery takes charge current. This gauge
 * has no `status` node; current_now is a crude boolean on this driver, 1 while charging, 0 on battery (live-probed
 * 2026-08-11). It reads 0 on a charger that holds the battery (full, or Charging Limit at 80 %): "charging" is not
 * "power in" (see powersrc_port). Returns 0, or -1 when the level could not be read. */
int powersrc_gauge(int *pct, int *charging);

/* The USB-C port (AW35615, i2c 2-0022): does power come in? 1 yes, 0 no, -1 unknown, PS_PORT_NONE when the Disc has no
 * such node (other firmware). Uncached: one or two small sysfs reads (an I2C transaction behind them). */
#define PS_PORT_NONE (-2)
int powersrc_port(void);

/* Pure parts, for the tests. cc_term: Rdef/R1p5/R3p0 -> 1, Undefined -> 0, else -1. With typec_state (NULL when the
 * node is missing): ...Source or Unattached -> 0, ...Sink + a cc_term level -> 1, else -1. */
int powersrc_cc_term_parse(const char *cc_term);
int powersrc_port_parse(const char *cc_term, const char *typec_state);

#endif
