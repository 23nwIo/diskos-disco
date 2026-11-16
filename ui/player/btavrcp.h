/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The headphones' buttons and absolute volume (AVRCP), through BlueZ on the system D-Bus (ui/dbusmini.c).
 *
 * The Disc's bluetoothd is BlueZ 5.54. There, AVRCP button presses (PASSTHROUGH) and the headphones' volume
 * notifications reach nobody unless a local player is registered with org.bluez.Media1.RegisterPlayer: without one,
 * keys would go to a uinput device, and the Disc has no /dev/uinput (BlueZ 5.54 profiles/audio/avrcp.c
 * handle_passthrough and avrcp_volume_changed; device check 2026-11-16: "AVRCP: failed to init uinput", presses
 * logged and dropped). The stock mq_player registers one ("RegisterPlayer" in its strings); this module does the same
 * for the own player: an org.mpris.MediaPlayer2.Player object that BlueZ calls for Play, Pause, Next, Previous and
 * Stop, and whose PlaybackStatus goes to the headphones.
 *
 * Absolute volume (AVRCP 1.4): with a player registered, BlueZ gives the headphones' volume to the A2DP transport
 * (org.bluez.MediaTransport1.Volume, 0..127). Its PropertiesChanged signals are passed on; main.c decides what to do
 * (BlueALSA's SoftVolume, the Bluetooth volume). docs/BLUETOOTH.md "Headphone battery, buttons and volume". */
#ifndef DISKOS_PLAYER_BTAVRCP_H
#define DISKOS_PLAYER_BTAVRCP_H

#define BTAV_PLAYER_PATH "/org/diskos/player"
/* there: the headphones' absolute volume is not used (the Bluetooth volume stays the Disc's own, BlueALSA's soft volume) */
#define BTAV_NO_ABS_VOLUME "/usr/data/diskos-own/no-abs-volume"

typedef struct {
    void (*log)(const char *fmt, ...);
    /* a button: PL_T_TOGGLE (Play, PlayPause), PL_T_PAUSE (Pause, Stop), PL_T_NEXT, PL_T_PREV */
    void (*transport)(int op);
    /* the headphones' volume (0..127) for their A2DP link; first = 1 the first time for this link (a new transport) */
    void (*volume)(const char *mac, int v127, int first);
} btav_cb;

/* Start (no I/O yet: btav_tick connects). bus = the system bus socket, NULL for the usual places. */
void btav_init(const btav_cb *cb, const char *bus);
/* The descriptor to poll for input, -1 while not connected. */
int btav_fd(void);
/* Call when btav_fd is readable. */
void btav_io(void);
/* Call once per main loop turn: connects, registers the player, retries (every few seconds), re-registers after
 * bluetoothd restarts. now = seconds, monotonic. */
void btav_tick(double now);
/* What plays (engine.h ENG_STOP/ENG_PLAY/ENG_PAUSE) and the track; sent to BlueZ only when it changes. */
void btav_status(int state, const char *title, const char *artist, long duration_ms);
/* The headphones' last absolute volume (0..127) for mac, -1 if they never sent one on this link. */
int btav_volume_of(const char *mac);
/* BlueALSA: switch the A2DP PCM of mac to soft volume (on = 1) or native volume (on = 0, the volume goes to the
 * headphones). 0 sent, -1 not connected or bad mac. */
int btav_soft_volume(const char *mac, int on);
/* BlueALSA: the A2DP PCM's volume (0..127, both channels), on this connection, so it arrives after a btav_soft_volume
 * sent before it. 0 sent, -1 not connected or bad mac. */
int btav_pcm_volume(const char *mac, int v127);
/* 1 once the player is registered with BlueZ */
int btav_registered(void);
void btav_close(void);

/* Pure helpers, for the tests: the MAC ("AA:BB:..") in a BlueZ or BlueALSA object path ("/org/bluez/hci0/dev_AA_BB_..."),
 * 0 or -1; and the player volume 0..max <-> 0..127 as bto_volume maps it. */
int btav_mac_of_path(const char *path, char *mac, int cap);
int btav_to127(int vol, int max);
int btav_from127(int v127, int max);
#endif
