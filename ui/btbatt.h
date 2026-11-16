/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The Bluetooth headphones' battery level, as BlueALSA reports it (org.bluealsa.RFCOMM1.Battery, 0..100).
 *
 * Headphones send it over the hands-free profile (HFP): Apple's AT+IPHONEACCEV after AT+XAPL, in tenths (so 0, 11,
 * 22 .. 100), which BlueALSA 4.0 answers when it runs with --profile=hfp-ag (ui/bt.c, Settings > Headphone Battery).
 * BlueZ 5.54 on the Disc has no BatteryProviderManager1, so BlueZ itself never shows it. Works with the stock player
 * and the own player alike, as long as Disco started BlueALSA. docs/BLUETOOTH.md "Headphone battery, buttons and volume". */
#ifndef DISKOS_BTBATT_H
#define DISKOS_BTBATT_H

/* Call every few seconds with the connected headphones' MAC ("" or NULL: none; the bus is then not opened). Reads
 * the signals that arrived and asks for the level when the headphones change or once a minute. */
void btbatt_poll(const char *mac, void (*log)(const char *fmt, ...));
/* The last level of mac (0..100), -1 when not known. */
int btbatt_level(const char *mac);
/* pure, for the tests: the MAC in an object path ("/org/bluealsa/hci0/dev_AA_BB_.../rfcomm"), 0 or -1 */
int btbatt_mac_of_path(const char *path, char *mac, int cap);
#endif
