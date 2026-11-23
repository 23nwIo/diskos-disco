/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Moving the own player's library to the card (docs/LIBRARY_DB.md, Jo 2026-11-22).
 *
 * When the own player runs and the card in it has no library.db yet, the flash library is copied onto the card
 * (libdb_migrate) - only on external power, and only while nothing plays. Meanwhile a full-screen overlay says the
 * move is running and that neither the power nor the card may be removed; it is removed when the move ends, either
 * way. Afterwards the Disc restarts (every connection then opens the card library). A failure keeps the flash library
 * and tries again at the next start. Without power a notice asks for it, once per boot.
 *
 * After the restart: a copy marked for a rescan (its paths are mostly not on this card) starts one. */
#include "screens.h"
#include "theme.h"
#include "musicdb.h"
#include "scanner.h"
#include "sdstate.h"
#include "sdio.h"
#include "artcache.h"      /* sd_write_begin / sd_write_end */
#include "libdb.h"
#include "own_player.h"      /* opl_running: which player runs, to explain a missing pid marker */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void settings_restart_now(void);
int  usage_plugged_in(void);
int  ui_local_playback_allowed(void);

enum { LM_IDLE, LM_RUNNING, LM_DONE, LM_FAILED, LM_OFF };
static int g_state = LM_IDLE;
static lv_obj_t *g_ov;
static _Atomic int g_result = -1;          /* worker: 0 ok, L1xx code; -1 still running */
static char g_err[160];
static uint32_t g_next_look, g_done_at;
static int g_power_told, g_rescan_checked;
static int g_manual;                       /* started from Maintenance: a failure shows a dialog, not a toast */
static void ok_noop(void){}
static void report(const char *title, const char *why){
    fprintf(stderr, "libdb: %s: %s\n", title, why); fflush(stderr);
    fileops_confirm(title, why, "OK", ok_noop);
}

static void ov_close(void){ if(g_ov){ lv_obj_delete(g_ov); g_ov = NULL; } }
static lv_obj_t *ov_label(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c, int y){
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);   /* wraps inside its width; never scrolls */
    lv_obj_set_width(l, 280);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}
static void ov_show(void){
    ov_close();
    g_ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_ov);
    lv_obj_set_size(g_ov, 360, 360);
    lv_obj_set_style_bg_color(g_ov, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_ov, LV_OBJ_FLAG_CLICKABLE);            /* taps do nothing while it runs */
    lv_obj_clear_flag(g_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sp = lv_spinner_create(g_ov);
    lv_obj_set_size(sp, 56, 56);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, -82);
    lv_obj_set_style_arc_color(sp, ui_current_accent(), LV_PART_INDICATOR);
    ov_label(g_ov, "Moving the library to the SD card", TF(UI_20), TC(TEXT_PRIMARY), -14);
    ov_label(g_ov, "Keep the power connected.\nDo not remove the SD card.", TF(UI_16), TC(TEXT_SECONDARY), 48);
}

static void *worker(void *a){
    (void)a;
    int rc;
    if(!sds_begin()){ snprintf(g_err, sizeof g_err, "L102: SD card not ready"); rc = LIBDB_E_OPEN_DST; }
    else {
        if(!sd_write_begin()){ snprintf(g_err, sizeof g_err, "L102: SD card is read-only"); rc = LIBDB_E_OPEN_DST; }
        else {
            if(sds_mkdir() != 0){ snprintf(g_err, sizeof g_err, "L102: can't create %s%s", SDS_CARD, SDS_SUBDIR); rc = LIBDB_E_OPEN_DST; }
            else rc = libdb_migrate(LIBDB_FLASH, LIBDB_CARD, g_err, sizeof g_err);
            sync();
            sd_write_end();
        }
        sds_end();
    }
    atomic_store(&g_result, rc);
    return NULL;
}

/* The move may run now: own player, card local and writable, no card library yet, a flash library to copy. */
static int wanted(void){
    return libdb_own_player_runs() && ui_local_playback_allowed() && !scanner_active()
        && access(LIBDB_CARD, F_OK) != 0 && access(LIBDB_FLASH, R_OK) == 0;
}

/* After the restart: a copy marked LIBDB_RESCAN starts a rescan of this card, once. */
static void rescan_check(void){
    if(g_rescan_checked || !libdb_is_card(libdb_path()) || !ui_local_playback_allowed() || scanner_active()) return;
    g_rescan_checked = 1;
    if(!sd_io_begin()) { g_rescan_checked = 0; return; }
    sqlite3 *d = NULL; int want = 0;
    if(libdb_open_v2(libdb_path(), &d, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(d, 2000);
        sqlite3_stmt *st = NULL;
        if(sqlite3_prepare_v2(d, "SELECT V FROM DISKOS_META WHERE K='LIBDB_RESCAN';", -1, &st, NULL) == SQLITE_OK
           && sqlite3_step(st) == SQLITE_ROW) want = sqlite3_column_int(st, 0) == 1;
        sqlite3_finalize(st);
        if(want && scanner_start() == 0){
            sqlite3_exec(d, "UPDATE DISKOS_META SET V=0 WHERE K='LIBDB_RESCAN';", 0, 0, 0);
            fprintf(stderr, "libdb: card library copied from another card's library, rescanning\n"); fflush(stderr);
        } else if(want) g_rescan_checked = 0;               /* the scan didn't start: try again later */
    }
    sqlite3_close(d);
    sd_io_end();
}

static int start(void){
    atomic_store(&g_result, -1); g_err[0] = 0;
    pthread_t th;
    if(pthread_create(&th, NULL, worker, NULL) != 0){ g_state = LM_OFF; return -1; }
    pthread_detach(th);
    fprintf(stderr, "libdb: moving the library to the card%s\n", g_manual ? " (Maintenance)" : ""); fflush(stderr);
    ov_show();
    g_state = LM_RUNNING;
    return 0;
}

/* Settings > System > Maintenance > Move Library to SD Card (Jo 2026-11-23): the same move, started by hand. Checks
 * every condition in turn and shows the first that fails; an existing card library always stops it. */
static void manual_go(void){
    if(g_state == LM_RUNNING) return;
    g_manual = 1;
    if(start() != 0) report("Library not moved", "Couldn't start the worker thread");
}
void libdb_ui_manual(int v){
    (void)v;
    char why[200];
    int pl = usage_plugged_in();
    if(g_state == LM_RUNNING) snprintf(why, sizeof why, "The move is already running");
    else if(g_state == LM_DONE) snprintf(why, sizeof why, "The library was moved - the Disc restarts");
    else if(!LIBDB_CARD_ENABLED) snprintf(why, sizeof why, "This build keeps the library on the flash (LIBDB_CARD_ENABLED 0)");
    else if(!libdb_own_player_runs())
        snprintf(why, sizeof why, opl_running() ? "The own player runs, but its marker %s is missing or wrong"
                                                : "Only the own player uses the card library (Boot to Stock Player off, restart)",
                 LIBDB_OWN_PID);
    else if(!ui_local_playback_allowed()) snprintf(why, sizeof why, "SD card not ready (no card, USB storage, or SD access held)");
    else if(access(LIBDB_CARD, F_OK) == 0) snprintf(why, sizeof why, "The SD card already has a library: %s", LIBDB_CARD);
    else if(access(LIBDB_FLASH, R_OK) != 0) snprintf(why, sizeof why, "No library on the internal flash: %s", LIBDB_FLASH);
    else if(scanner_active()) snprintf(why, sizeof why, "A library scan is running - let it finish first");
    else if(pl != 1) snprintf(why, sizeof why, pl == 0 ? "Connect external power first" : "Can't tell whether external power is connected");
    else if(ui_is_playing()) snprintf(why, sizeof why, "Stop playback first");
    else {
        fileops_confirm("Move library to SD card?", "Copies the library to the card, then restarts. Keep the power connected.",
                        "Move", manual_go);
        return;
    }
    report("Library not moved", why);
}

/* UI thread, every 500 ms (main.c scanner_poll). */
void libdb_ui_tick(void){
#if !LIBDB_CARD_ENABLED
    return;
#endif
    uint32_t now = lv_tick_get();
    switch(g_state){
    case LM_IDLE:
        rescan_check();
        if((int32_t)(now - g_next_look) < 0) return;
        g_next_look = now + 5000;
        if(!wanted()) return;
        if(usage_plugged_in() != 1){
            if(!g_power_told){ g_power_told = 1; ui_toast("Connect power to move the library to the SD card"); }
            return;
        }
        if(ui_is_playing()) return;                       /* when playback stops */
        start();
        return;
    case LM_RUNNING: {
        int rc = atomic_load(&g_result);
        if(rc < 0) return;
        ov_close();                                        /* the overlay goes, whatever the outcome */
        if(rc == 0){
            fprintf(stderr, "libdb: library moved to the card, restarting\n"); fflush(stderr);
            ui_toast("Library moved to the SD card - restarting");
            g_state = LM_DONE; g_done_at = now;
        } else {
            fprintf(stderr, "libdb: moving the library failed: %s\n", g_err); fflush(stderr);
            if(g_manual) report("Library not moved", g_err);   /* the first error, with its L1xx code */
            else { char b[200]; snprintf(b, sizeof b, "Library not moved (%.4s) - next start tries again", g_err); ui_toast(b); }
            g_manual = 0;
            g_state = LM_FAILED;
        }
        return; }
    case LM_DONE:
        if(now - g_done_at >= 2500){ g_state = LM_OFF; settings_restart_now(); }
        return;
    default:
        return;
    }
}
