/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* wifi_reco.h: when the V2.57 boot hold may release so the saved Wi-Fi choice is restored. */
#include <stdio.h>
#include <stdlib.h>
#include "wifi_reco.h"

static int fails;
#define EXPECT(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

int main(void){
    /* both saved on: the player may still switch Wi-Fi off, whatever Wi-Fi does right now */
    EXPECT(!wifi_reco_release(1, 1, 0)); EXPECT(!wifi_reco_release(1, 1, 1)); EXPECT(!wifi_reco_release(1, 1, -1));
    /* the player saved Wi-Fi off with Bluetooth on (this boot or an earlier one): release once Wi-Fi is really down */
    EXPECT(wifi_reco_release(0, 1, 0)); EXPECT(!wifi_reco_release(0, 1, 1)); EXPECT(!wifi_reco_release(0, 1, -1));
    /* only one radio (or none) saved on: nothing for the player to decide */
    EXPECT(wifi_reco_release(1, 0, 1)); EXPECT(wifi_reco_release(1, 0, 0)); EXPECT(wifi_reco_release(0, 0, -1));
    /* unread database: keep waiting (the caller's window ends it) */
    EXPECT(!wifi_reco_release(-1, -1, 0)); EXPECT(!wifi_reco_release(0, -1, 0)); EXPECT(!wifi_reco_release(-1, 1, 0));
    /* only the stock player arbitrates the radios at boot; the diskOS player never does, so no hold for it */
    EXPECT(!wifi_reco_player_decides(WIFI_RECO_PL_OWN));
    EXPECT(wifi_reco_player_decides(WIFI_RECO_PL_STOCK)); EXPECT(wifi_reco_player_decides(WIFI_RECO_PL_PENDING));
    if(fails) return 1;
    puts("PASS wifi boot hold releases only once the player can no longer switch Wi-Fi off, and never holds for the diskOS player");
    return 0;
}
