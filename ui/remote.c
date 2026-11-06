/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Remote control over Wi-Fi: the UI side (Settings > Connectivity > Remote Control).
 *
 * Off by default. When on and Wi-Fi has an address, remote_srv serves a small control page and JSON API on
 * http://<wlan0 address>:REMOTE_PORT/<token>/ (bound to that address only, never to every interface). The token is
 * 100 random bits from /dev/urandom, kept in diskos.conf so a bookmark keeps working; New Link replaces it.
 * Plain HTTP on the local network: the token keeps strangers out, but anyone who can watch the Wi-Fi traffic can
 * read it. The Settings text says so.
 *
 * Every command runs here, on the main thread, through the same functions the touch controls use
 * (ui_transport_command, ui_set_volume, ui_seek_to, ui_set_workmode), so all their guards apply: Music Assistant
 * routing, "local playback only", output switches in progress, audiobook skip-as-seek, the skip pacing. */
#include "remote.h"
#include "remote_srv.h"
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "ipc.h"
#include "ma.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

void ui_set_workmode(int mode);   /* main.c */
void ui_loop_wake(void);          /* main.c */

static char g_ip[24];             /* the address the server is bound to ("" = not running) */
static uint32_t g_retry_at;       /* after a failed start: not before this tick */
static lv_obj_t *g_ov;
static int g_mode = -1;           /* cached play mode for the snapshot */
static uint32_t g_mode_at;

static int wlan_ip(char *out, size_t cap){
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); if(s < 0) return -1;
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "wlan0");
    int r = ioctl(s, SIOCGIFADDR, &ifr); close(s);
    if(r < 0) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    if(sin->sin_addr.s_addr == htonl(INADDR_ANY)) return -1;
    if(!inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)cap)) return -1;
    return 0;
}
static int new_token(char *out){                       /* 0 ok, -1 no secure randomness (then: no server) */
    unsigned char b[13]; size_t n = 0;
    int f = open("/dev/urandom", O_RDONLY | O_CLOEXEC); if(f < 0) return -1;
    while(n < sizeof b){ ssize_t r = read(f, b + n, sizeof b - n); if(r > 0) n += (size_t)r; else if(r < 0 && errno == EINTR) continue; else break; }
    close(f);
    if(n != sizeof b) return -1;
    rc_token_encode(b, sizeof b, out);
    memset(b, 0, sizeof b);
    return 0;
}
/* The saved token, or a new one saved now. NULL if none can be made or saved. */
static const char *token(void){
    const char *t = cfg_get_str(REMOTE_CFG_TOKEN, "");
    if(rc_token_valid(t)) return t;
    char nt[REMOTE_TOKEN_LEN + 1];
    if(new_token(nt) || cfg_set_str(REMOTE_CFG_TOKEN, nt)){ fprintf(stderr, "remote: no token\n"); return NULL; }
    t = cfg_get_str(REMOTE_CFG_TOKEN, "");
    return rc_token_valid(t) ? t : NULL;
}

static void server_stop(void){
    if(rsrv_running()){ rsrv_stop(); fprintf(stderr, "remote: stopped\n"); fflush(stderr); }
    g_ip[0] = 0;
}
/* Keep the server matching the setting and the current Wi-Fi address (it changes on a new network or lease). */
static void server_sync(void){
    if(!cfg_get_int(REMOTE_CFG_ON, 0)){ server_stop(); return; }
    char ip[24];
    if(wlan_ip(ip, sizeof ip)){ server_stop(); return; }
    if(rsrv_running() && !strcmp(ip, g_ip)) return;
    server_stop();
    if(g_retry_at && (int32_t)(lv_tick_get() - g_retry_at) < 0) return;
    const char *t = token();
    if(!t || rsrv_start(ip, REMOTE_PORT, t)){
        g_retry_at = lv_tick_get() + 5000;
        fprintf(stderr, "remote: can't listen on %s:%d\n", ip, REMOTE_PORT); fflush(stderr);
        return;
    }
    g_retry_at = 0;
    snprintf(g_ip, sizeof g_ip, "%s", ip);
    fprintf(stderr, "remote: listening on %s:%d\n", ip, REMOTE_PORT); fflush(stderr);
}
static void remote_tick(lv_timer_t *t){ (void)t; server_sync(); }

void remote_boot(void){
    rsrv_set_notify(ui_loop_wake);
    lv_timer_create(remote_tick, 1000, NULL);
    server_sync();
}

/* ---------------------------------------------------------------- the main thread's half */
static int music_assistant(void){ return ma_controls(); }
static const char *source_name(void){
    if(music_assistant()) return "music_assistant";
    return ui_get_source_mode() == 0 ? "local" : "other";
}
static void snapshot(rc_state_t *s){
    track_state_t st; ipc_get_state(&st);
    memset(s, 0, sizeof *s);
    snprintf(s->title, sizeof s->title, "%s", st.title);
    snprintf(s->artist, sizeof s->artist, "%s", st.artist);
    snprintf(s->album, sizeof s->album, "%s", st.album);
    s->has_track = st.have_track;
    s->playing = music_assistant() ? ma_playing() : ui_is_playing();
    s->position_ms = st.position_ms; s->duration_ms = st.duration_ms;
    s->volume = st.volume; s->volume_max = VOL_MAX;
    if(!g_mode_at || lv_tick_elaps(g_mode_at) >= 2000){ g_mode = ui_play_mode_now(); g_mode_at = lv_tick_get() | 1; }   /* it can read the player's database */
    s->play_mode = music_assistant() ? -1 : g_mode;
    s->controllable = music_assistant() || ui_local_playback_allowed();
    s->source = source_name();
}
static const char *const NOT_LOCAL = "The player is not in local playback (e.g. USB DAC, Bluetooth or AirPlay mode)";
static int run(const rc_cmd_t *c, const char **why){
    static const char PP[] = "0201000C0000", NEXT[] = "0201000C0001", PREV[] = "0201000C0002";
    int ma = music_assistant();
    int transport = c->kind == RC_CMD_PLAY_PAUSE || c->kind == RC_CMD_PLAY || c->kind == RC_CMD_PAUSE ||
                    c->kind == RC_CMD_NEXT || c->kind == RC_CMD_PREV || c->kind == RC_CMD_SEEK;
    if(transport && !ma && !ui_local_playback_allowed()){ *why = NOT_LOCAL; return -1; }   /* say why here, no toast on the Disc */
    switch(c->kind){
    case RC_CMD_PLAY_PAUSE: return ui_transport_command(PP);
    case RC_CMD_PLAY:  if(ma){ ma_send("play"); return 0; }  return ui_is_playing() ? 0 : ui_transport_command(PP);
    case RC_CMD_PAUSE: if(ma){ ma_send("pause"); return 0; } return ui_is_playing() ? ui_transport_command(PP) : 0;
    case RC_CMD_NEXT:  return ui_transport_command(NEXT);
    case RC_CMD_PREV:  return ui_transport_command(PREV);
    case RC_CMD_SEEK:  return ui_seek_to(c->arg);
    case RC_CMD_VOLUME: return ui_set_volume((int)c->arg);
    case RC_CMD_VOLUME_STEP: { track_state_t st; ipc_get_state(&st); return ui_set_volume(st.volume + (int)c->arg); }
    case RC_CMD_PLAY_MODE:
        if(ma){ *why = "Music Assistant sets the play mode while it plays"; return -1; }
        if(!ui_local_playback_allowed()){ *why = NOT_LOCAL; return -1; }
        ui_set_workmode((int)c->arg);
        return 0;
    default: *why = "Unknown command"; return -1;
    }
}
void remote_pump(void){
    static uint32_t last;
    if(!rsrv_running()) return;
    rc_state_t s;
    if(!last || lv_tick_elaps(last) >= 250){ snapshot(&s); rsrv_publish(&s); last = lv_tick_get() | 1; }   /* a page polls 1/s */
    rc_cmd_t c;
    if(rsrv_take(&c)){
        const char *why = NULL;
        int rc = run(&c, &why);
        fprintf(stderr, "remote: %s %ld -> %d\n", rc_cmd_name(c.kind), c.arg, rc); fflush(stderr);
        rsrv_done(rc == 0, rc == 0 ? NULL : why);
        if(rc == 0 && c.kind == RC_CMD_PLAY_MODE) g_mode_at = 0;   /* show the new mode at once */
        if(rc == 0){ snapshot(&s); rsrv_publish(&s); last = lv_tick_get() | 1; }
    }
}

/* ---------------------------------------------------------------- Settings */
void remote_set_on(int on){
    if(on && !token()){ cfg_set_int(REMOTE_CFG_ON, 0); ui_toast("Couldn't create a secure link"); return; }
    g_retry_at = 0;
    server_sync();
    if(on && !rsrv_running()) ui_toast(wlan_ip((char[24]){0}, 24) ? "Starts when Wi-Fi is connected" : "Couldn't start - try again");
}

static void ov_close(void){ if(g_ov){ lv_obj_delete(g_ov); g_ov = NULL; } }
static void ov_close_cb(lv_event_t *e){ (void)e; ov_close(); }
int remote_link_shown(void){ return g_ov != NULL; }
static lv_obj_t *ov_label(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c, int y, int w){
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);             /* wraps; never scrolls vertically */
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}
static void show_link(const char *url);
void remote_show_link(void){
    if(!cfg_get_int(REMOTE_CFG_ON, 0)){ ui_toast("Turn on Remote Control first"); return; }
    server_sync();
    if(!rsrv_running()){ ui_toast("Connect to Wi-Fi first"); return; }
    const char *t = token(); if(!t) return;
    char url[96]; snprintf(url, sizeof url, "http://%s:%d/%s/", g_ip, REMOTE_PORT, t);
    show_link(url);
}
void remote_test_link(const char *url){ show_link(url); }   /* host renders: the overlay without Wi-Fi */
static void show_link(const char *url){
    ov_close();
    g_ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_ov);
    lv_obj_set_size(g_ov, 360, 360);
    lv_obj_set_style_bg_color(g_ov, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_ov, LV_OBJ_FLAG_CLICKABLE);              /* swallow taps behind it */
    lv_obj_remove_flag(g_ov, LV_OBJ_FLAG_SCROLLABLE);
    ov_label(g_ov, "Remote Control", TH_F_DETAIL, TC(TEXT_PRIMARY), -140, 220);
    lv_obj_t *qr = lv_qrcode_create(g_ov);
    lv_qrcode_set_size(qr, 140);
    lv_qrcode_set_dark_color(qr, TC(FIXED_QR_DARK));
    lv_qrcode_set_light_color(qr, TC(FIXED_QR_LIGHT));
    lv_qrcode_update(qr, url, (uint32_t)strlen(url));
    lv_obj_set_style_border_width(qr, 5, 0);
    lv_obj_set_style_border_color(qr, TC(FIXED_QR_LIGHT), 0);
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -42);
    char shown[100]; const char *path = strchr(url + 7, '/');  /* two lines, split before the token: fits every theme's font */
    if(path) snprintf(shown, sizeof shown, "%.*s\n%s", (int)(path - url), url, path); else snprintf(shown, sizeof shown, "%s", url);
    ov_label(g_ov, shown, TF(UI_14), TC(TEXT_SECONDARY), 62, 300);
    lv_obj_t *b = lv_button_create(g_ov);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 120, 44); lv_obj_align(b, LV_ALIGN_CENTER, 0, 124);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, theme_color_from_rgb(TH_SURF1), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(b, ov_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, "Close");
    lv_obj_set_style_text_font(l, TH_F_DETAIL, 0); lv_obj_set_style_text_color(l, TC(TEXT_PRIMARY), 0); lv_obj_center(l);
    ui_request_wake();
}
static void new_link_yes(void){
    char nt[REMOTE_TOKEN_LEN + 1];
    if(new_token(nt) || cfg_set_str(REMOTE_CFG_TOKEN, nt)){ ui_toast("Couldn't make a new link"); return; }
    memset(nt, 0, sizeof nt);
    server_stop(); g_retry_at = 0; server_sync();              /* old link stops working now */
    ui_toast("New link made - old links no longer work");
}
void remote_new_link(void){
    fileops_confirm("Make a new link?", "Old links and bookmarks stop working", "New link", new_link_yes);
}
