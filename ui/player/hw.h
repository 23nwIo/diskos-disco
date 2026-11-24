/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Hardware actions of the own player that do not depend on the audio engine. Values from the stock player's trace
 * (docs/MQ_PLAYER_TRACE.md, third run). */
#ifndef DISKOS_PLAYER_HW_H
#define DISKOS_PLAYER_HW_H

typedef void (*hw_log_fn)(const char *fmt, ...);

/* The stock player's power off routine (menu power off / sleep timer). dry_run: only log each step. Does not return
 * unless dry_run is set or poweroff itself fails. wd_fd: the armed watchdog (or -1), stopped before the end. */
void hw_power_off(int wd_fd, int dry_run, hw_log_fn log);
/* Power key short press: the screen locked or back on (ui/panel.c, the same lock the UI's idle timeout uses). The UI
 * follows the backlight (ui/main.c, PW-10) and puts its own brightness back on. Returns 1 = now on, 0 = now off. */
int hw_screen_toggle(int dry_run, hw_log_fn log);

/* Headphone jack, as the stock player reads it (trace runs 1-3): /dev/gpio ioctl _IO('G',122) = 0x2000477a with a
 * pointer to the pin name "pb20"; the return value is the level, 1 = plug in, 0 = out. The stock player polls it about
 * three times a second (no kernel event for it is known yet). hw_jack_open: the descriptor or -1 (errno set);
 * hw_jack_read: 1, 0 or -1 (errno set). */
#define JACK_DEV  "/dev/gpio"
#define JACK_POLL_MS 330
int hw_jack_open(void);
int hw_jack_read(int fd);

/* The 4.4 mm balanced plug, as the stock player detects it (mq_player adc_bal_det_handler, read from the binary
 * 2026-11-15, not device-checked): an ADC, /dev/jz_adc_aux_2, enabled once with _IO('A',11) and read as one 32-bit
 * value every 330 ms (its log calls it a voltage). hw_bal_open: the descriptor or -1 (errno set); hw_bal_read: the
 * value or -1 (errno set). hw_bal_level (pure): 1 plugged (>= 1501, or 801..949), 0 not (1151..1349), -1 for any
 * other value, which leaves the state as it is. hw_debounce: the stock player takes a new level only after 4 reads in
 * a row that differ from the current one; *state starts at -1 (unknown, the first known level is taken at once).
 * Returns 1 when *state changed. */
#define BAL_DEV  "/dev/jz_adc_aux_2"
int hw_bal_open(void);
int hw_bal_read(int fd);
int hw_bal_level(int value);
int hw_debounce(int *state, int *count, int level);

/* Keys of /dev/input/event0 ("x2000_key", device-checked 2026-11-15). The driver reports clicks: press and release
 * arrive together, and a held Power key comes as its own code. */
#define KEY_DEV        "/dev/input/event0"
#define KEYC_POWER     265
#define KEYC_POWER_ALT 259               /* every second short press comes as 259 (device log 2026-11-15) */
#define KEYC_POWER_LONG 264
#define KEYC_VOL_UP    251
#define KEYC_VOL_DOWN  252
#define KEYC_PLAY      250
/* The driver tells a double click and a long press apart itself and sends own codes for them (device log 2026-11-15,
 * Jo: Volume up/down held 3 s, double clicked; Play double clicked) */
#define KEYC_VOL_UP_LONG     263
#define KEYC_VOL_DOWN_LONG   262
#define KEYC_VOL_UP_DOUBLE   266
#define KEYC_VOL_DOWN_DOUBLE 267
#define KEYC_PLAY_DOUBLE     269
#define KEYC_PLAY_LONG       268         /* Play held (Jo 2026-11-15); a Power double click sends no code at all */
/* Charging Limit (0808 = SYSCONFIG.CHARGE_PROTECT), as the stock player's battery thread "loop_power" does it (mq_player
 * 0x4e2c08, read from the binary 2026-11-24): with the limit on it reads the gauge (cw221X-bat/capacity) once a second
 * and sends _IO('I',40) to /dev/sgm41513 at 80 % or more (charging off; its log calls it SGM41513_CHARGE_OFF) and
 * _IO('I',41) below 80 % (charging on), the latter only while the USB-C controller reports power. With the limit off
 * it sends nothing. The own player did not do this at all before (Jo 2026-11-24: "the limit still doesn't work").
 *
 * Here: every CHG_POLL_MS, and only while a charger/computer powers the Disc (cc_term Rdef/R1p5/R3p0, the signal
 * ui/usage.c uses; device-probed 2026-11-14). "Off" is sent again every CHG_REASSERT_MS in case the charger IC fell
 * back to its defaults; "on" once per change. Unplugged or unknown (the Disc as USB host): nothing is sent, and the
 * state is forgotten so the next plug-in decides afresh. When the cc_term node is missing (other firmware) the limit
 * works without that check, like the stock player. Not device-tested. */
#define CHG_DEV          "/dev/sgm41513"
#define CHG_CAPACITY     "/sys/class/power_supply/cw221X-bat/capacity"
#define CHG_CC_TERM      "/sys/bus/i2c/devices/2-0022/control/cc_term"
#define CHG_IOC_OFF      0x20004928UL     /* _IO('I',40) */
#define CHG_IOC_ON       0x20004929UL     /* _IO('I',41) */
#define CHG_LIMIT_PCT    80
#define CHG_POLL_MS      5000
#define CHG_REASSERT_MS  30000
typedef struct { int sent; double sent_at, polled_at; int logged_pct, cc_missing_logged; } hw_charge_t;   /* sent: -1 none, 0 off, 1 on */
void hw_charge_init(hw_charge_t *s);
/* 1 = USB-C power in, 0 = nothing plugged in, -1 = anything else (pure; NULL -> -1) */
int hw_cc_power_in(const char *cc_term);
/* The decision (pure). power: 1 in, 0 out, -1 unknown, 2 = no cc_term node (act like the stock player). pct: the gauge
 * or -1. Returns what to send now: 1 on, 0 off, -1 nothing; updates s->sent / s->sent_at when it returns 0 or 1. */
int hw_charge_step(hw_charge_t *s, int limit_on, int pct, int power, double now);
/* Read the gauge and cc_term, decide, and send the ioctl (dry: log only). Call it from the main loop; it polls itself
 * every CHG_POLL_MS. */
void hw_charge_tick(hw_charge_t *s, int limit_on, double now, int dry, hw_log_fn log);
/* At the player's orderly stop: charging back on if this player turned it off, so the next player (or none) does
 * not inherit a held battery. */
void hw_charge_release(hw_charge_t *s, int dry, hw_log_fn log);
#endif
