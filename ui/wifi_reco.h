/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Wi-Fi boot hold on V2.57 (wifi.c radio reconciliation), the pure decision.
 * V2.57's player turns Wi-Fi off at boot when BOTH radios are saved on in its SYSCONFIG, then saves WIFI_STATUS=0
 * (fwcaps.c fw_has_radio_boot_arbitration). diskOS keeps the user's own "wifi_on" choice across a reboot, so it only
 * waits until that boot decision can no longer switch Wi-Fi off, then lets the keepalive restore the saved choice. */
#ifndef DISKOS_WIFI_RECO_H
#define DISKOS_WIFI_RECO_H

/* 1 = the player's boot decision cannot (or can no longer) turn Wi-Fi off: stop holding.
 * db_wifi/db_bt: the player's saved WIFI_STATUS/BT_STATUS (0/1, -1 = unread). eff_wifi: 0 = Wi-Fi really down,
 * 1 = up, -1 = unknown. Only "both saved on" can still be switched off; it is over once the player has saved Wi-Fi
 * off AND Wi-Fi is really down (so the keepalive's start cannot be cut short by the player's own wifi_down). */
static inline int wifi_reco_release(int db_wifi, int db_bt, int eff_wifi){
    if(db_wifi < 0 || db_bt < 0) return 0;          /* unread: keep waiting (the caller's window still ends it) */
    if(db_wifi == 1 && db_bt == 1) return 0;        /* the player may still switch Wi-Fi off */
    if(db_wifi == 0 && db_bt == 1) return eff_wifi == 0;   /* saved off (now or on an earlier boot): wait until it's down */
    return 1;                                       /* only one radio saved on: the player has nothing to decide */
}

#endif
