/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The Disc's Bandcamp screen (SCR_BANDCAMP, the app "Bandcamp"): search, an artist's releases, a release's tracks,
 * playing and saving to the SD card, and the downloads. A plain list in the Radio screen's look; everything runs on
 * bandcamp.c (its workers), this only shows it. Main thread only. docs/BANDCAMP.md "On the Disc".
 *
 *   Bandcamp        Search Bandcamp / Downloads / Stop Bandcamp (while it plays) / Results of the last search
 *   Results         artists and albums open their page, a track plays (hold: save it)
 *   an artist       the releases (tap: open)
 *   a release       Play all / Save to the SD card / the tracks (tap: play from it, hold: save it)
 *   Downloads       waiting, saving n/N, saved, failed (with why) / Stop the downloads
 * Back goes one view up; the pages already seen are kept (no new download). */
#include "screens.h"
#include "bandcamp.h"
#include "bandcamp_ui.h"
#include "theme.h"
#include "theme_kit.h"
#include "curvelist.h"
#include "vlist.h"
#include "zune_dial.h"
#include "own_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void screen_show(int which);

enum { V_HOME, V_HITS, V_PAGE, V_DL };
#define DEPTH 8
typedef struct {
    int v;                                  /* V_* */
    char url[BC_URL_LEN];                   /* V_PAGE: the page */
    char title[BC_NAME_LEN];
    int state;                              /* BC_BUSY until its answer is here */
    int kind;                               /* V_PAGE: 'b' artist, 'r' release */
    bc_item_t *items; int n;                /* V_HITS, V_PAGE 'b' */
    bc_release_t rel; bc_track_t *tracks;   /* V_PAGE 'r' (n tracks) */
    int scroll;
} view_t;
static view_t g_st[DEPTH]; static int g_depth;
static lv_obj_t *g_root, *g_title, *g_list;
static curvelist_t g_cl;
static vlist_t g_vl;
static lv_timer_t *g_poll;
static unsigned g_seen;
static char g_query[96];
static char g_play_state[16];

#define ROW_W 268
#define ROW_H TH_ROW_H
/* the action rows (ids < 0); items are ids >= 0 */
enum { A_SEARCH = -1, A_DOWNLOADS = -2, A_STOP = -3, A_RESULTS = -4, A_PLAYALL = -5, A_SAVEALL = -6, A_DLSTOP = -7, A_QUALITY = -8 };

static view_t *top(void){ return g_depth ? &g_st[g_depth - 1] : NULL; }
static void view_free(view_t *v){ free(v->items); free(v->tracks); v->items = NULL; v->tracks = NULL; v->n = 0; }
static void rebuild(void);
static void poll_cb(lv_timer_t *t);
static void poll_start(void){ if(!g_poll) g_poll = lv_timer_create(poll_cb, 400, NULL); }

static void note(const char *msg, const char *help){
    if(th_zune()){ zune_empty(g_list, msg, help); return; }
    lv_obj_t *l = lv_label_create(g_list);
    char b[256]; snprintf(b, sizeof b, help ? "%s\n%s" : "%s", msg, help ? help : "");
    lv_label_set_text(l, b);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, ROW_W - 40);
    lv_obj_set_style_pad_top(l, 12, 0);
    lv_obj_set_style_text_color(l, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_font(l, TF(UI_16), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}
static void row_cb(lv_event_t *e);
static void row_long_cb(lv_event_t *e);
static lv_obj_t *add_row(intptr_t id, const char *glyph, const char *name, const char *detail, int accent, int hold){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, ROW_W, ROW_H);
    lv_obj_add_flag(r, LV_OBJ_FLAG_USER_1);                                     /* curves with the circle */
    lv_obj_set_style_radius(r, TH_R_ROW, 0);
    lv_obj_set_style_bg_color(r, TC(SURFACE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(r, 2);
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_SHORT_CLICKED, (void *)id);
    if(hold) lv_obj_add_event_cb(r, row_long_cb, LV_EVENT_LONG_PRESSED, (void *)id);
    lv_obj_t *ic = lv_label_create(r);
    lv_label_set_text(ic, glyph);
    lv_obj_set_pos(ic, 12, 17);
    lv_obj_set_style_text_font(ic, TF(UI_16), 0);
    lv_obj_set_style_text_color(ic, accent ? ui_current_accent() : TC(TEXT_DISABLED), 0);
    int has_detail = detail && detail[0];
    lv_obj_t *nm = lv_label_create(r);
    lv_label_set_text(nm, name);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(nm, 40, has_detail ? 6 : 15);
    lv_obj_set_size(nm, ROW_W - 50, 22);
    lv_obj_set_style_text_font(nm, ui_font_user(18), 0);                      /* names in any script */
    lv_obj_set_style_text_color(nm, TC(TEXT_PRIMARY), 0);
    if(has_detail){
        lv_obj_t *dt = lv_label_create(r);
        lv_label_set_text(dt, detail);
        lv_label_set_long_mode(dt, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(dt, 40, 30);
        lv_obj_set_width(dt, ROW_W - 56);
        lv_obj_set_style_text_font(dt, TH_F_DETAIL, 0);
        lv_obj_set_style_text_color(dt, TC(TEXT_SECONDARY), 0);
    }
    theme_list_row(r);
    if(th_zune()) ui_library_row(r);
    return r;
}

/* ---- the rows of each view */
static const char *type_name(char t){ return t == 'b' ? "Artist" : t == 'a' ? "Album" : t == 't' ? "Track" : ""; }
static void item_row(int i){
    view_t *v = top(); if(!v || i < 0 || i >= v->n) return;
    const bc_item_t *it = &v->items[i];
    char d[300]; const char *ty = v->v == V_HITS ? type_name(it->type) : it->type == 't' ? "Track" : "";
    snprintf(d, sizeof d, "%s%s%s%s%s", ty, ty[0] && it->artist[0] ? " \xC2\xB7 " : "", it->artist,
             it->album[0] && it->type == 't' ? " \xC2\xB7 " : "", it->type == 't' ? it->album : "");
    add_row(i, it->type == 't' ? LV_SYMBOL_AUDIO : it->type == 'b' ? LV_SYMBOL_HOME : LV_SYMBOL_LIST, it->name, d, 0, it->type == 't');
}
static void track_row(int i){
    view_t *v = top(); if(!v || i < 0 || i >= v->n) return;
    const bc_track_t *t = &v->tracks[i];
    char name[BC_NAME_LEN + 16], d[200] = "";
    if(t->num > 0) snprintf(name, sizeof name, "%d. %s", t->num % 10000, t->title); else snprintf(name, sizeof name, "%s", t->title);
    char tm[24] = ""; if(t->ms > 0) snprintf(tm, sizeof tm, "%ld:%02ld", t->ms / 60000 % 10000, t->ms / 1000 % 60);
    int other = strcmp(t->artist, v->rel.artist) != 0;
    snprintf(d, sizeof d, "%s%s%s%s", other ? t->artist : "", other && tm[0] ? " \xC2\xB7 " : "", tm,
             t->stream[0] ? "" : (other || tm[0]) ? " \xC2\xB7 not streamable" : "not streamable");
    add_row(i, LV_SYMBOL_AUDIO, name, d, 0, t->stream[0] != 0);
}
static void vl_scroll_cb(lv_event_t *e){ (void)e; vlist_follow(&g_vl); }
static void rows(int n, void (*add)(int)){                  /* n rows after the action rows already in the list */
    if(n >= VLIST_MIN){
        int lead = (int)lv_obj_get_child_count(g_list) * (ROW_H + 4);
        vlist_begin(&g_vl, g_list, n, ROW_H, 4, lead, th_zune() ? 236 : 272, add);
    } else for(int i = 0; i < n; i++) add(i);
}
static void title(const char *t){
    char low[BC_NAME_LEN]; snprintf(low, sizeof low, "%s", t);
    if(th_zune()) for(char *c = low; *c; c++) if(*c >= 'A' && *c <= 'Z') *c += 32;
    theme_title_text(g_title, low);
}
static void rebuild(void){
    view_t *v = top();
    if(!g_list || !v) return;
    vlist_end(&g_vl);
    lv_obj_clean(g_list);
    if(v->v == V_HOME){
        title("Bandcamp");
        add_row(A_SEARCH, LV_SYMBOL_EDIT, "Search Bandcamp", "Artists, albums, tracks", 1, 0);
        bc_dl_info_t d[4]; int n = bandcamp_downloads(d, 4), run = 0;
        for(int i = 0; i < n; i++) run += d[i].state == BC_DL_WAITING || d[i].state == BC_DL_SAVING;
        char dd[64]; snprintf(dd, sizeof dd, run ? "%d saving to the SD card" : n ? "Saved to BandcampDownloads" : "Nothing saved yet", run);
        add_row(A_DOWNLOADS, LV_SYMBOL_DOWNLOAD, "Downloads", dd, 1, 0);
        if(bandcamp_active()) add_row(A_STOP, LV_SYMBOL_CLOSE, "Stop Bandcamp", "Back to your music", 1, 0);
        int st; bc_item_t one; if(g_query[0] && (bandcamp_hits(&one, 1, &st) > 0 || st == BC_BUSY))
            add_row(A_RESULTS, LV_SYMBOL_LIST, "Last search", g_query, 1, 0);
        add_row(A_QUALITY, LV_SYMBOL_SETTINGS, "Download quality", bandcamp_quality_name(bandcamp_quality()), 1, 0);
        if(!opl_running()) note("Bandcamp plays with the diskOS player", "Settings > About > Player");
    } else if(v->v == V_HITS){
        title("Results");
        if(v->state == BC_BUSY) note("Searching Bandcamp...", NULL);
        else if(v->state == BC_FAILED) note("Couldn't reach Bandcamp", "Check Wi-Fi and try again.");
        else if(!v->n) note("Nothing found", "Try another name.");
        else rows(v->n, item_row);
    } else if(v->v == V_PAGE){
        title(v->title);
        if(v->state == BC_BUSY) note("Loading from Bandcamp...", NULL);
        else if(v->state == BC_FAILED) note("Couldn't load this page", "Check Wi-Fi and try again.");
        else if(v->kind == 'b'){ if(!v->n) note("No releases", NULL); else rows(v->n, item_row); }
        else {
            int plays = 0; for(int i = 0; i < v->n; i++) plays += v->tracks[i].stream[0] != 0;
            if(plays){
                add_row(A_PLAYALL, LV_SYMBOL_PLAY, "Play all", v->rel.artist, 1, 0);
                add_row(A_SAVEALL, LV_SYMBOL_DOWNLOAD, v->rel.type == 't' ? "Save to the SD card" : "Save the album",
                        v->rel.free_dl[0] && bandcamp_quality() != BC_Q_STREAM ? "To the SD card, free download" : "To the SD card, 128 kbit/s", 1, 0);
            }
            if(!v->n) note("No tracks", NULL);
            else if(!plays) note("The artist doesn't let these be streamed", NULL);
            rows(v->n, track_row);
        }
    } else {
        title("Downloads");
        static bc_dl_info_t d[32]; int n = bandcamp_downloads(d, 32), run = 0;
        for(int i = 0; i < n; i++) run += d[i].state == BC_DL_WAITING || d[i].state == BC_DL_SAVING;
        if(run) add_row(A_DLSTOP, LV_SYMBOL_CLOSE, "Stop the downloads", "After the current track", 1, 0);
        if(!n) note("Nothing saved yet", "Open an album: Save the album. Or hold a track.");
        static const char *sn[] = { "Waiting", "Saving", "Saved", "Failed", "Stopped" };
        for(int i = 0; i < n; i++){
            char det[200];
            snprintf(det, sizeof det, "%s%s%d/%d%s%s%s%s", sn[d[i].state], d[i].total ? " " : "", d[i].done, d[i].total,
                     d[i].artist[0] ? " \xC2\xB7 " : "", d[i].artist,
                     d[i].state == BC_DL_FAILED && d[i].err[0] ? " \xC2\xB7 " : "", d[i].state == BC_DL_FAILED ? d[i].err : "");
            if(!d[i].total){ char *p = strstr(det, "0/0"); if(p) memmove(p, p + 3, strlen(p + 3) + 1); }
            add_row(1000 + i, d[i].state == BC_DL_SAVED ? LV_SYMBOL_OK : d[i].state == BC_DL_FAILED ? LV_SYMBOL_CLOSE : LV_SYMBOL_DOWNLOAD,
                    d[i].title, det, d[i].state == BC_DL_SAVING, 0);
        }
    }
    lv_obj_update_layout(g_list);
    lv_obj_scroll_to_y(g_list, v->scroll, LV_ANIM_OFF);
    vlist_follow(&g_vl);
    curvelist_update(&g_cl);
}

/* ---- going places */
static view_t *push(int kind, const char *t){
    view_t *cur = top();
    if(cur) cur->scroll = lv_obj_get_scroll_y(g_list);
    if(g_depth == DEPTH){ view_free(&g_st[1]); memmove(&g_st[1], &g_st[2], sizeof g_st[0] * (DEPTH - 2)); g_depth--; }   /* the oldest page goes */
    view_t *v = &g_st[g_depth++];
    memset(v, 0, sizeof *v);
    v->v = kind; snprintf(v->title, sizeof v->title, "%s", t);
    return v;
}
static void fill_hits(view_t *v){
    view_free(v);
    v->items = malloc(sizeof *v->items * BC_MAX_HITS);
    v->n = v->items ? bandcamp_hits(v->items, BC_MAX_HITS, &v->state) : 0;
    if(!v->items) v->state = BC_FAILED;
}
static void search_done(const char *text){
    if(!text) return;
    while(*text == ' ') text++;
    if(!text[0]) return;
    snprintf(g_query, sizeof g_query, "%s", text);
    const char *why = NULL;
    if(bandcamp_search(g_query, &why)){ ui_toast(why ? why : "Couldn't search"); return; }
    view_t *v = push(V_HITS, "Results"); v->state = BC_BUSY;
    g_seen = bandcamp_gen();
    rebuild(); poll_start();
}
static void open_page(const bc_item_t *it){
    const char *why = NULL;
    if(bandcamp_open(it->url, &why)){ ui_toast(why ? why : "Couldn't open it"); return; }
    view_t *v = push(V_PAGE, it->name); v->state = BC_BUSY;
    snprintf(v->url, sizeof v->url, "%s", it->url);
    rebuild(); poll_start();
}
static void play(const char *url, int idx){
    const char *why = NULL;
    if(!opl_running()){ ui_toast("Bandcamp plays with the diskOS player"); return; }
    if(bandcamp_play(url, idx, &why)){ ui_toast(why ? why : "Couldn't play it"); return; }
    ui_toast("Starting Bandcamp...");
    snprintf(g_play_state, sizeof g_play_state, "starting");
    poll_start();
}
static void save(const char *url, int idx, const char *name){
    const char *why = NULL;
    if(bandcamp_download(url, idx, &why)){ ui_toast(why ? why : "Couldn't save it"); return; }
    char m[BC_NAME_LEN + 40]; snprintf(m, sizeof m, "Saving %s", name); ui_toast(m);
    poll_start();
}
static void row_cb(lv_event_t *e){
    intptr_t id = (intptr_t)lv_event_get_user_data(e);
    view_t *v = top(); if(!v) return;
    switch(id){
    case A_SEARCH: kbinput_open("Search Bandcamp", g_query, search_done); return;
    case A_DOWNLOADS: push(V_DL, "Downloads"); rebuild(); poll_start(); return;
    case A_STOP: bandcamp_stop(); rebuild(); return;
    case A_RESULTS: { view_t *h = push(V_HITS, "Results"); fill_hits(h); rebuild(); if(h->state == BC_BUSY) poll_start(); return; }
    case A_PLAYALL: play(v->rel.url, -1); return;
    case A_SAVEALL: save(v->rel.url, -1, v->rel.title); return;
    case A_QUALITY: bandcamp_set_quality((bandcamp_quality() + 1) % (BC_Q_STREAM + 1)); rebuild(); return;   /* tap: the next one */
    case A_DLSTOP: bandcamp_download_cancel(); ui_toast("The downloads stop after this track"); rebuild(); return;
    }
    if(id < 0 || id >= v->n) return;
    if(v->v == V_PAGE && v->kind == 'r'){
        if(v->tracks[id].stream[0]) play(v->rel.url, (int)id);
        else ui_toast("The artist doesn't let this track be streamed");
        return;
    }
    if(v->v == V_HITS || (v->v == V_PAGE && v->kind == 'b')){
        bc_item_t it = v->items[id];
        if(it.type == 't' && v->v == V_HITS) play(it.url, -1);
        else open_page(&it);
    }
}
static void row_long_cb(lv_event_t *e){                     /* hold a track: save it */
    intptr_t id = (intptr_t)lv_event_get_user_data(e);
    view_t *v = top(); if(!v || id < 0 || id >= v->n) return;
    if(v->v == V_PAGE && v->kind == 'r'){ if(v->tracks[id].stream[0]) save(v->rel.url, (int)id, v->tracks[id].title); }
    else if(v->items && v->items[id].type == 't') save(v->items[id].url, -1, v->items[id].name);
}
static void poll_cb(lv_timer_t *t){
    (void)t;
    view_t *v = top();
    unsigned g = bandcamp_gen();
    const char *ps = bandcamp_state();
    if(strcmp(ps, g_play_state)){                            /* playing began: show it */
        int began = !strcmp(g_play_state, "starting") && !strcmp(ps, "playing");
        snprintf(g_play_state, sizeof g_play_state, "%s", ps);
        if(began && screen_current() == SCR_BANDCAMP) screen_show(SCR_NOWPLAYING);
    }
    int busy = !strcmp(ps, "starting") || bandcamp_downloading();
    if(v && g != g_seen){
        g_seen = g;
        if(v->v == V_HITS && v->state == BC_BUSY){ fill_hits(v); if(v->state != BC_BUSY && screen_current() == SCR_BANDCAMP) rebuild(); }
        else if(v->v == V_PAGE && v->state == BC_BUSY){
            char url[BC_URL_LEN], name[BC_NAME_LEN]; int st, kind; bc_item_t *items; bc_release_t rel; bc_track_t *tr;
            int n = bandcamp_page(url, sizeof url, &st, &kind, name, sizeof name, &items, &rel, &tr);
            if(!strcmp(url, v->url) && st != BC_BUSY){
                view_free(v);
                v->state = st; v->kind = kind; v->n = n; v->items = items; v->tracks = tr; items = NULL; tr = NULL;
                if(kind == 'r') v->rel = rel;
                if(name[0] && st == BC_DONE) snprintf(v->title, sizeof v->title, "%s", name);
                if(screen_current() == SCR_BANDCAMP) rebuild();
            }
            free(items); free(tr);
        } else if((v->v == V_DL || v->v == V_HOME) && screen_current() == SCR_BANDCAMP){
            int y = lv_obj_get_scroll_y(g_list); v->scroll = y; rebuild();
        }
    }
    if(v && (v->v == V_HITS || v->v == V_PAGE) && v->state == BC_BUSY) busy = 1;
    if(!busy && g_poll){ lv_timer_delete(g_poll); g_poll = NULL; }
}
int bandcamp_ui_back_consumed(void){
    if(g_depth <= 1) return 0;
    view_free(&g_st[--g_depth]);
    rebuild();
    return 1;
}
static void header_back_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED && !bandcamp_ui_back_consumed()) screen_back();
}
void bandcamp_ui_create(lv_obj_t *root){
    g_root = root;
    lv_obj_set_style_bg_color(root, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    if(th_zune()){ g_title = zune_pivot(root, "bandcamp", header_back_cb); zune_status(root, SCR_BANDCAMP); }
    else g_title = ui_header_cb(root, "Bandcamp", header_back_cb);
    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, (360 - ROW_W) / 2, th_zune() ? 124 : 72);
    lv_obj_set_size(g_list, ROW_W, th_zune() ? 236 : 272);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);                               /* the last row clears the round bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_list, 4, 0);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_event_cb(g_list, vl_scroll_cb, LV_EVENT_SCROLL, NULL);
    curvelist_attach(&g_cl, g_list, root, ROW_W);
    if(th_zune()) ui_library_list_watch(g_list);
}
void bandcamp_ui_open(void){
    while(g_depth) view_free(&g_st[--g_depth]);
    push(V_HOME, "Bandcamp");
    snprintf(g_play_state, sizeof g_play_state, "%s", bandcamp_state());
    g_seen = bandcamp_gen();
    rebuild();
    screen_show(SCR_BANDCAMP);
    if(bandcamp_downloading()) poll_start();
}
/* host renders (tests/host_render.c): the views with made-up pages, no network */
void bandcamp_ui_test_hits(const bc_item_t *it, int n){
    bandcamp_ui_open();
    view_t *v = push(V_HITS, "Results");
    v->items = malloc(sizeof *it * (size_t)(n > 0 ? n : 1)); v->n = v->items ? n : 0; v->state = BC_DONE;
    if(v->n) memcpy(v->items, it, sizeof *it * (size_t)n);
    rebuild();
}
void bandcamp_ui_test_release(const bc_release_t *r, const bc_track_t *t, int n){
    bandcamp_ui_open();
    view_t *v = push(V_PAGE, r->title);
    v->kind = 'r'; v->rel = *r; v->state = BC_DONE;
    v->tracks = malloc(sizeof *t * (size_t)(n > 0 ? n : 1)); v->n = v->tracks ? n : 0;
    if(v->n) memcpy(v->tracks, t, sizeof *t * (size_t)n);
    rebuild();
}
