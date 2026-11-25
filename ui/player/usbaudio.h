/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* USB audio output (docs/USB_AUDIO.md): the Disc as USB host, the USB sound cards the kernel's snd-usb-audio made into
 * ALSA cards, and their hardware volume control. No alsa-lib: /proc/asound, sysfs and the control ioctls, like
 * mixer.c. Every path starts with ua_root (normally ""), so ui/tests can build a fake tree.
 *
 * Verified on a Disc (2026-11-25, tools/usbaudio-probe.sh): the OTG role switch stays "none" when a device is plugged
 * in, so the Disc only becomes host when we write "host"; the USB-C controller (AW35615) then says data_role DFP. A PC
 * makes it UFP. A C-Media 0d8c:0014 behind a hub became card 1 "Device", S16_LE 44.1/48 kHz only, with the control
 * "Speaker Playback Volume" 0..37 (-37..0 dB) and "Speaker Playback Switch"; it has no serial number. */
#ifndef DISKOS_PLAYER_USBAUDIO_H
#define DISKOS_PLAYER_USBAUDIO_H

#define UA_ROLE      "/sys/class/usb_role/13500000.otg_new-role-switch/role"
#define UA_DATA_ROLE "/sys/bus/i2c/devices/2-0022/control/data_role"
#define UA_MAX_CARDS 8

typedef void (*ua_log_fn)(const char *fmt, ...);

/* prefix for every path ("" on the Disc; a temporary directory in the tests) */
extern char ua_root[256];

/* ---- the host role ----
 * Once per call: the USB-C controller's data role decides. DFP (the Disc is the data host: a USB device, hub or dock
 * is plugged in) -> the OTG role becomes "host"; anything else after we made it host -> "none" again (the stock
 * default; a PC then finds the Disc as before). Left alone while USB storage mode has the port (storage = 1: it set
 * "device"). Returns 1 while the role is host, 0 otherwise, -1 when the nodes are missing. */
int ua_role_tick(int storage, ua_log_fn log);
/* pure: "DFP\n" -> 1, "UFP"/"" -> 0, NULL -> -1 */
int ua_data_role_parse(const char *s);

/* ---- the cards ---- */
typedef struct {
    int card;                      /* ALSA card number */
    int pcm;                       /* its first playback PCM device */
    char usbid[10];                /* "0d8c:0014" */
    char serial[64];               /* the USB serial number, "" when the device has none */
    char id[32];                   /* ALSA card id ("Device") */
    char name[80];                 /* the USB product string, else the ALSA card name */
    char key[96];                  /* "usb:0d8c:0014:<serial>", or "usb:0d8c:0014" without a serial */
} ua_card_t;
/* The USB sound cards that can play (a usbid and a playback PCM), by card number. Returns how many (0..max). */
int ua_scan(ua_card_t *out, int max);
/* pure: the key for usbid + serial (serial "" = none) */
void ua_make_key(char *key, unsigned cap, const char *usbid, const char *serial);

/* ---- the hardware volume ----
 * The first integer "... Playback Volume" control that is not a monitor of an input (Mic, Line, ...); Master, PCM,
 * Speaker, Headphone are preferred in that order. Its "... Playback Switch" mutes at volume 0. */
typedef struct {
    int ok;                        /* a volume control was found */
    unsigned vol_numid, sw_numid;  /* sw_numid 0: none */
    int channels;
    long min, max;
    char name[44];                 /* "Speaker Playback Volume" */
} ua_mixer_t;
int ua_mixer_find(int card, ua_mixer_t *m, ua_log_fn log);
/* volume 0..scale onto the control's range (linear in its steps; the C-Media's steps are 1 dB); 0 mutes. 0 or -errno. */
int ua_mixer_set(int card, const ua_mixer_t *m, int volume, int scale, ua_log_fn log);
/* pure: the raw control value for volume 0..scale */
long ua_mixer_raw(const ua_mixer_t *m, int volume, int scale);
/* pure: how good a control name is as the output volume: 0 not at all, higher is better */
int ua_mixer_rank(const char *name);
/* A card without a volume control of its own gets one from us: alsa-lib's softvol plugin (ua_alsa_conf_write soft)
 * scales the samples by a user control on the card. alsa-lib would create that control at 0 dB (full level) when MPD
 * opens the stream; so it is made here first, at 0 = silence, with exactly what softvol checks (integer, 2 channels,
 * 0..UA_SOFTVOL_MAX, a dB scale), and softvol takes it as it is (pcm_softvol.c softvol_load_control, alsa-lib 1.2.12).
 * Each card has its own control, so no output shares a volume with another one. 0, or -1 (then the card must not be
 * played: its level could not be controlled). */
#define UA_SOFTVOL_NAME   "diskOS USB Playback Volume"
#define UA_SOFTVOL_MAX    120                    /* steps; 0 = silence (softvol), 120 = 0 dB */
#define UA_SOFTVOL_MIN_DB (-60.0)
int ua_softvol_prepare(int card, ua_log_fn log);

/* ---- MPD's view ----
 * MPD's USB output plays to the ALSA name "diskos_usb", which this file defines (engine.c points MPD's alsa-lib at it
 * with ALSA_CONFIG_PATH). alsa-lib reads its files again when one changed (snd_config_update_r: inode or mtime), so
 * another card needs no restart of MPD: the file is replaced and the output opened again. card < 0: a null device;
 * soft: through softvol with UA_SOFTVOL_NAME (ua_softvol_prepare first). */
int ua_alsa_conf_write(const char *path, int card, int pcm, int soft, ua_log_fn log);
#endif
