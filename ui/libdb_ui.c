/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The own player's library on the card (docs/LIBRARY_DB.md, Jo 2026-11-22/23).
 *
 * A card without library.db gets a new one when the own player runs (libdb_create): the flash library's schema (or
 * the stock tables), no songs - only on external power and while nothing plays. Meanwhile a full-screen overlay says
 * it is being made and that neither the power nor the card may be removed; it is removed when that ends, either way.
 * Afterwards the Disc restarts (every connection then opens the card library) and a scan of the card starts.
 * After that scan, the user's data from the flash library (favourites, playlists, play history, audiobook progress,
 * EQ curves) is taken over for the songs this card has (libdb_import), once, behind the overlay again.
 * A failure keeps nothing half-made and tries again at the next start. Without power a notice asks for it, once
 * per boot. */
#include "screens.h"
#include "theme.h"
#include "musicdb.h"
#include "scanner.h"
#include "sdstate.h"
#include "sdio.h"
#include "artcache.h"      /* sd_write_begin / sd_write_end */
#include "libdb.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void settings_restart_now(void);
void ui_library_reload(void);
int  usage_plugged_in(void);
int  ui_local_playback_allowed(void);

enum { LM_IDLE, LM_RUNNING, LM_DONE, LM_FAILED, LM_OFF };
static int g_state = LM_IDLE;
static lv_obj_t *g_ov;
static _Atomic int g_result = -1;          /* worker: 0 ok, L1xx code; -1 still running */
static char g_err[160];
static uint32_t g_next_look, g_done_at;
static int g_power_told;
static int g_import;                       /* the running job is the take-over, not the new library */
static char g_summary[200];
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
static void ov_show(const char *title){
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
    ov_label(g_ov, title, TF(UI_20), TC(TEXT_PRIMARY), -14);
    ov_label(g_ov, "Keep the power connected.\nDo not remove the SD card.", TF(UI_16), TC(TEXT_SECONDARY), 48);
}

static void *worker(void *a){
    (void)a;
    int rc;
    if(!sds_begin()){ snprintf(g_err, sizeof g_err, "L102: SD card not ready"); rc = LIBDB_E_OPEN_DST; }
    else {
        if(!sd_write_begin()){ snprintf(g_err, sizeof g_err, "L102: SD card is read-only"); rc = LIBDB_E_OPEN_DST; }
        else {
            if(g_import) rc = libdb_import(LIBDB_FLASH, LIBDB_CARD, g_err, sizeof g_err, g_summary, sizeof g_summary);
            else if(sds_mkdir() != 0){ snprintf(g_err, sizeof g_err, "L102: can't create %s%s", SDS_CARD, SDS_SUBDIR); rc = LIBDB_E_OPEN_DST; }
            else rc = libdb_create(LIBDB_FLASH, LIBDB_CARD, g_err, sizeof g_err);
            sync();
            sd_write_end();
        }
        sds_end();
    }
    atomic_store(&g_result, rc);
    return NULL;
}

/* A new card library may be made now: own player, card local, no scan, no card library yet. */
static int wanted(void){
    return libdb_own_player_runs() && ui_local_playback_allowed() && !scanner_active() && access(LIBDB_CARD, F_OK) != 0;
}

static int start(int import);
/* The card library's DISKOS_META: LIBDB_RESCAN = 1 starts the scan of a new library, once; LIBDB_IMPORT = 1 after
 * that scan (none pending or running, songs found) starts the take-over. */
static int g_meta_done;
static void meta_check(void){
    if(g_meta_done || !libdb_is_card(libdb_path()) || !ui_local_playback_allowed() || scanner_active()) return;
    if(access(LIBDB_CARD, F_OK) != 0) return;
    if(!sd_io_begin()) return;
    sqlite3 *d = NULL; int rescan = 0, import = 0;
    if(libdb_open_v2(libdb_path(), &d, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK){
        sqlite3_busy_timeout(d, 2000);
        sqlite3_stmt *st = NULL;
        if(sqlite3_prepare_v2(d, "SELECT K, V FROM DISKOS_META WHERE K IN ('LIBDB_RESCAN','LIBDB_IMPORT');", -1, &st, NULL) == SQLITE_OK)
            while(sqlite3_step(st) == SQLITE_ROW){
                const char *k = (const char *)sqlite3_column_text(st, 0);
                int v = sqlite3_column_int(st, 1) == 1;
                if(k && !strcmp(k, "LIBDB_RESCAN")) rescan = v; else if(k) import = v;
            }
        sqlite3_finalize(st);
        if(rescan){
            int r = scanner_start();
            if(r == 0){
                sqlite3_exec(d, "UPDATE DISKOS_META SET V=0 WHERE K='LIBDB_RESCAN';", 0, 0, 0);
                fprintf(stderr, "libdb: new card library, scanning the card\n"); fflush(stderr);
            }
        }
    }
    sqlite3_close(d);
    sd_io_end();
    if(!rescan && !import) g_meta_done = 1;                    /* nothing left to do for this card this boot */
    else if(!rescan && import && mdb_total_song_count() > 0){
        g_meta_done = 1;
        start(1);
    }
}

static int start(int import){
    atomic_store(&g_result, -1); g_err[0] = 0; g_summary[0] = 0; g_import = import;
    pthread_t th;
    if(pthread_create(&th, NULL, worker, NULL) != 0){ g_state = LM_OFF; return -1; }
    pthread_detach(th);
    fprintf(stderr, "libdb: %s\n", import ? "taking over favourites, playlists, ... from the flash library"
                                          : "making a new library on the card"); fflush(stderr);
    ov_show(import ? "Taking over favourites and playlists" : "Creating the library on the SD card");
    g_state = LM_RUNNING;
    return 0;
}

/* UI thread, every 500 ms (main.c scanner_poll). */
void libdb_ui_tick(void){
#if !LIBDB_CARD_ENABLED
    return;
#endif
    uint32_t now = lv_tick_get();
    switch(g_state){
    case LM_IDLE:
        if((int32_t)(now - g_next_look) < 0) return;
        g_next_look = now + 5000;
        meta_check();
        if(g_state != LM_IDLE) return;
        if(!wanted()) return;
        if(usage_plugged_in() != 1){
            if(!g_power_told){ g_power_told = 1; ui_toast("Connect power to create the library on the SD card"); }
            return;
        }
        if(ui_is_playing()) return;                       /* when playback stops */
        start(0);
        return;
    case LM_RUNNING: {
        int rc = atomic_load(&g_result);
        if(rc < 0) return;
        ov_close();                                        /* the overlay goes, whatever the outcome */
        if(rc == 0 && g_import){
            fprintf(stderr, "libdb: taken over from the flash library: %s\n", g_summary[0] ? g_summary : "nothing for this card");
            fflush(stderr);
            ui_library_reload();
            ui_toast(g_summary[0] ? "Favourites and playlists taken over" : "Nothing to take over for this card");
            g_state = LM_IDLE;
        } else if(rc == 0){
            fprintf(stderr, "libdb: new card library made, restarting\n"); fflush(stderr);
            ui_toast("Library created on the SD card - restarting");
            g_state = LM_DONE; g_done_at = now;
        } else {
            fprintf(stderr, "libdb: %s failed: %s\n", g_import ? "taking over" : "making the card library", g_err); fflush(stderr);
            char b[200]; snprintf(b, sizeof b, "%s (%.4s) - next start tries again", g_import ? "Favourites not taken over" : "Library not created", g_err);
            ui_toast(b);
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
