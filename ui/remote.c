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
#include "musicdb.h"
#include "fwcaps.h"
#include "sdio.h"
#include "folderbrowser.h"
#include "radio.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
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
    rmt_token_encode(b, sizeof b, out);
    memset(b, 0, sizeof b);
    return 0;
}
/* The saved token, or a new one saved now. NULL if none can be made or saved. */
static const char *token(void){
    const char *t = cfg_get_str(REMOTE_CFG_TOKEN, "");
    if(rmt_token_valid(t)) return t;
    char nt[REMOTE_TOKEN_LEN + 1];
    if(new_token(nt) || cfg_set_str(REMOTE_CFG_TOKEN, nt)){ fprintf(stderr, "remote: no token\n"); return NULL; }
    t = cfg_get_str(REMOTE_CFG_TOKEN, "");
    return rmt_token_valid(t) ? t : NULL;
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
    if(radio_active()) return "radio";
    return ui_get_source_mode() == 0 ? "local" : "other";
}
static void snapshot(rmt_state_t *s){
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
    s->source = source_name();
    s->radio_state = radio_state();
    snprintf(s->station, sizeof s->station, "%s", radio_station_name());
    snprintf(s->radio_error, sizeof s->radio_error, "%s", radio_last_error());
    s->controllable = music_assistant() || radio_active() || ui_local_playback_allowed();
}
static const char *const NOT_LOCAL = "The player is not in local playback (e.g. USB DAC, Bluetooth or AirPlay mode)";
static int run(const rmt_cmd_t *c, const char **why){
    static const char PP[] = "0201000C0000", NEXT[] = "0201000C0001", PREV[] = "0201000C0002";
    int ma = music_assistant();
    int transport = c->kind == RMT_CMD_PLAY_PAUSE || c->kind == RMT_CMD_PLAY || c->kind == RMT_CMD_PAUSE ||
                    c->kind == RMT_CMD_NEXT || c->kind == RMT_CMD_PREV || c->kind == RMT_CMD_SEEK;
    int radio = !ma && radio_active();
    if(radio && (c->kind == RMT_CMD_NEXT || c->kind == RMT_CMD_PREV || c->kind == RMT_CMD_SEEK || c->kind == RMT_CMD_PLAY_MODE)){
        *why = "Not available for web radio"; return -1;
    }
    if(transport && !ma && !radio && !ui_local_playback_allowed()){ *why = NOT_LOCAL; return -1; }   /* say why here, no toast on the Disc */
    switch(c->kind){
    case RMT_CMD_PLAY_PAUSE: return ui_transport_command(PP);
    case RMT_CMD_PLAY:  if(ma){ ma_send("play"); return 0; }  return ui_is_playing() ? 0 : ui_transport_command(PP);
    case RMT_CMD_PAUSE: if(ma){ ma_send("pause"); return 0; } return ui_is_playing() ? ui_transport_command(PP) : 0;
    case RMT_CMD_NEXT:  return ui_transport_command(NEXT);
    case RMT_CMD_PREV:  return ui_transport_command(PREV);
    case RMT_CMD_SEEK:  return ui_seek_to(c->arg);
    case RMT_CMD_VOLUME: return ui_set_volume((int)c->arg);
    case RMT_CMD_VOLUME_STEP: { track_state_t st; ipc_get_state(&st); return ui_set_volume(st.volume + (int)c->arg); }
    case RMT_CMD_PLAY_MODE:
        if(ma){ *why = "Music Assistant sets the play mode while it plays"; return -1; }
        if(!ui_local_playback_allowed()){ *why = NOT_LOCAL; return -1; }
        ui_set_workmode((int)c->arg);
        return 0;
    case RMT_CMD_RADIO_STOP:
        if(!radio_active() && !strcmp(radio_state(), "off")){ *why = "The radio is not playing"; return -1; }
        radio_stop();
        return 0;
    default: *why = "Unknown command"; return -1;
    }
}

/* ---------------------------------------------------------------- the library (main thread) */
/* Lists answer {"ok":true,"total":N,"offset":O,"items":[...]}: total counts every match of q, items is one page. */
typedef struct { rmt_buf_t b; long total, offset, limit; int first; } page_t;
static void page_begin(page_t *pg, const rmt_cmd_t *c){
    memset(pg, 0, sizeof *pg); pg->offset = c->offset; pg->limit = c->limit; pg->first = 1;
    rmt_buf_put(&pg->b, "{\"ok\":true,\"items\":[");
}
static int page_take(page_t *pg){                      /* count a match; 1 = it belongs on this page (write it) */
    long i = pg->total++;
    if(i < pg->offset || i >= pg->offset + pg->limit) return 0;
    if(!pg->first) rmt_buf_put(&pg->b, ",");
    pg->first = 0; return 1;
}
static char *page_end(page_t *pg){
    rmt_buf_printf(&pg->b, "],\"total\":%ld,\"offset\":%ld}", pg->total, pg->offset);
    return rmt_buf_take(&pg->b);
}
static void song_json(rmt_buf_t *b, const mdb_song_t *s, long pos){
    rmt_buf_printf(b, "{\"id\":%d,\"title\":", s->id); rmt_buf_str(b, s->title);
    rmt_buf_put(b, ",\"artist\":"); rmt_buf_str(b, s->artist);
    rmt_buf_put(b, ",\"album\":"); rmt_buf_str(b, s->album);
    rmt_buf_printf(b, ",\"dur_ms\":%d,\"track\":%d", s->dur_ms, s->track);
    if(pos > 0) rmt_buf_printf(b, ",\"pos\":%ld", pos);
    rmt_buf_put(b, "}");
}
static int song_hit(const mdb_song_t *s, const char *q){ return rmt_match(s->title, q) || rmt_match(s->artist, q) || rmt_match(s->album, q); }

/* The card path for an API-relative one, refusing anything that resolves off the card (symlinks included).
 * Call inside an sd_io lease. 0 ok. */
static int card_path(const char *rel, char *out, size_t cap){
    char full[PATH_MAX], real[PATH_MAX];
    if(!rmt_relpath_ok(rel)) return -1;
    int n = rel[0] ? snprintf(full, sizeof full, "%s/%s", FOLDERBROWSER_ROOT, rel) : snprintf(full, sizeof full, "%s", FOLDERBROWSER_ROOT);
    if(n <= 0 || n >= (int)sizeof full) return -1;
    if(!realpath(full, real)) return -1;
    size_t rl = strlen(FOLDERBROWSER_ROOT);
    if(strncmp(real, FOLDERBROWSER_ROOT, rl) || (real[rl] && real[rl] != '/')) return -1;
    if(snprintf(out, cap, "%s", full) >= (int)cap) return -1;
    return 0;
}
typedef struct { char name[256]; int dir; } fent_t;
static int fent_cmp(const void *a, const void *b){
    const fent_t *x = a, *y = b;
    if(x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}
#define RMT_FOLDER_MAX 5000
static char *list_folder(const rmt_cmd_t *c, const char **why){
    if(!sd_io_begin()){ *why = "The SD card is not available right now"; return NULL; }
    char dir[PATH_MAX]; fent_t *e = NULL; int n = 0, cap = 0; DIR *d = NULL;
    if(card_path(c->name, dir, sizeof dir) || !(d = opendir(dir))){ sd_io_end(); *why = "No such folder"; return NULL; }
    struct dirent *de;
    while((de = readdir(d)) && n < RMT_FOLDER_MAX){
        const char *nm = de->d_name;
        if(nm[0] == '.') continue;                                   /* . .. and hidden entries */
        int isdir;
        if(de->d_type == DT_DIR) isdir = 1;
        else if(de->d_type == DT_REG) isdir = 0;
        else {
            char p[PATH_MAX]; struct stat st;
            if(snprintf(p, sizeof p, "%s/%s", dir, nm) >= (int)sizeof p || lstat(p, &st)) continue;
            if(S_ISDIR(st.st_mode)) isdir = 1; else if(S_ISREG(st.st_mode)) isdir = 0; else continue;
        }
        if(!isdir && !folderbrowser_is_audio(nm)) continue;
        if(strlen(nm) >= sizeof e[0].name) continue;
        if(n == cap){ int nc = cap ? cap * 2 : 256; fent_t *ne = realloc(e, (size_t)nc * sizeof *e); if(!ne) break; e = ne; cap = nc; }
        snprintf(e[n].name, sizeof e[n].name, "%s", nm); e[n].dir = isdir; n++;
    }
    closedir(d);
    sd_io_end();
    if(n > 1) qsort(e, (size_t)n, sizeof *e, fent_cmp);
    page_t pg; page_begin(&pg, c);
    for(int i = 0; i < n; i++){
        if(!rmt_match(e[i].name, c->q) || !page_take(&pg)) continue;
        rmt_buf_put(&pg.b, "{\"name\":"); rmt_buf_str(&pg.b, e[i].name);
        rmt_buf_printf(&pg.b, ",\"dir\":%s}", e[i].dir ? "true" : "false");
    }
    free(e);
    return page_end(&pg);
}
static char *browse(const rmt_cmd_t *c, const char **why){
    page_t pg; char *out = NULL;
    switch(c->sub){
    case RMT_L_ALBUMS: case RMT_L_ARTISTS: {
        int albums = c->sub == RMT_L_ALBUMS;
        int n = albums ? mdb_album_count() : mdb_artist_count(); if(n < 0) n = 0;
        char (*names)[MDB_STR] = calloc((size_t)n + 1, MDB_STR);
        char (*arts)[MDB_STR] = albums ? calloc((size_t)n + 1, MDB_STR) : NULL;
        int *counts = albums ? calloc((size_t)n + 1, sizeof *counts) : NULL;
        if(!names || (albums && (!arts || !counts))){ free(names); free(arts); free(counts); *why = "Out of memory"; return NULL; }
        int got = albums ? mdb_albums(names, arts, counts, n) : mdb_artists(names, n);
        page_begin(&pg, c);
        for(int i = 0; i < got && i < n; i++){
            if(!(rmt_match(names[i], c->q) || (albums && rmt_match(arts[i], c->q))) || !page_take(&pg)) continue;
            rmt_buf_put(&pg.b, "{\"name\":"); rmt_buf_str(&pg.b, names[i]);
            if(albums){ rmt_buf_put(&pg.b, ",\"artist\":"); rmt_buf_str(&pg.b, arts[i]); rmt_buf_printf(&pg.b, ",\"count\":%d", counts[i]); }
            rmt_buf_put(&pg.b, "}");
        }
        free(names); free(arts); free(counts);
        return page_end(&pg); }
    case RMT_L_ALBUM: case RMT_L_ARTIST: case RMT_L_SONGS: {
        int cap = mdb_song_count(); if(cap < 0) cap = 0;
        const mdb_song_t **rows = calloc((size_t)cap + 1, sizeof *rows);
        if(!rows){ *why = "Out of memory"; return NULL; }
        int got;
        if(c->sub == RMT_L_ALBUM) got = mdb_album_songs(c->name, rows, cap);
        else if(c->sub == RMT_L_ARTIST) got = mdb_artist_songs(c->name, rows, cap);
        else if(c->q[0]) got = mdb_search(c->q, rows, cap);
        else { got = 0; for(int i = 0; i < cap; i++){ const mdb_song_t *s = mdb_song(i); if(s) rows[got++] = s; } }
        page_begin(&pg, c);
        int filter = c->sub != RMT_L_SONGS;                          /* search already filtered */
        for(int i = 0; i < got && i < cap; i++){
            if((filter && !song_hit(rows[i], c->q)) || !page_take(&pg)) continue;
            song_json(&pg.b, rows[i], 0);
        }
        free(rows);
        return page_end(&pg); }
    case RMT_L_PLAYLISTS: {
        int n = mdb_playlist_num(); if(n < 0) n = 0;
        char (*names)[MDB_STR] = calloc((size_t)n + 1, MDB_STR); long *ids = calloc((size_t)n + 1, sizeof *ids);
        if(!names || !ids){ free(names); free(ids); *why = "Out of memory"; return NULL; }
        int got = mdb_playlists(names, ids, n);
        page_begin(&pg, c);
        for(int i = 0; i < got && i < n; i++){
            if(!rmt_match(names[i], c->q) || !page_take(&pg)) continue;
            rmt_buf_printf(&pg.b, "{\"id\":%ld,\"name\":", ids[i]); rmt_buf_str(&pg.b, names[i]); rmt_buf_put(&pg.b, "}");
        }
        free(names); free(ids);
        return page_end(&pg); }
    case RMT_L_PLAYLIST: case RMT_L_FAVORITES: {
        int fav = c->sub == RMT_L_FAVORITES;
        int n = fav ? mdb_favorite_count() : mdb_playlist_count(c->id);
        if(n < 0){ *why = fav ? "Couldn't read the favourites" : "No such playlist"; return NULL; }
        mdb_song_t *rows = calloc((size_t)n + 1, sizeof *rows);
        if(!rows){ *why = "Out of memory"; return NULL; }
        int got = fav ? mdb_favorites(rows, n, fw_fav_play_by_love_id()) : mdb_playlist_songs(c->id, rows, n);
        page_begin(&pg, c);
        for(int i = 0; i < got && i < n; i++){
            if(!song_hit(&rows[i], c->q) || !page_take(&pg)) continue;
            song_json(&pg.b, &rows[i], i + 1);                       /* pos: what play_item takes to start here */
        }
        free(rows);
        return page_end(&pg); }
    case RMT_L_FOLDER: return list_folder(c, why);
    case RMT_L_RADIO: {                                    /* the Radio app's favourites; pos counts from 1 */
        static rc_station_t fav[RC_MAX_FAV];
        int n = radio_favorites(fav, RC_MAX_FAV);
        page_begin(&pg, c);
        for(int i = 0; i < n; i++){
            if(!(rmt_match(fav[i].name, c->q) || rmt_match(fav[i].country, c->q)) || !page_take(&pg)) continue;
            rmt_buf_printf(&pg.b, "{\"pos\":%d,\"name\":", i + 1); rmt_buf_str(&pg.b, fav[i].name);
            rmt_buf_put(&pg.b, ",\"url\":"); rmt_buf_str(&pg.b, fav[i].url);
            rmt_buf_put(&pg.b, ",\"codec\":"); rmt_buf_str(&pg.b, fav[i].codec);
            rmt_buf_printf(&pg.b, ",\"bitrate\":%d,\"country\":", fav[i].bitrate); rmt_buf_str(&pg.b, fav[i].country);
            rmt_buf_put(&pg.b, "}");
        }
        return page_end(&pg); }
    default: *why = "Unknown list"; return out;
    }
}
static int play_song_id(int id){                       /* in the all-songs scope, as Songs / Search / Files do */
    int pos = mdb_play_pos(id, 1, "");
    return pos >= 1 && ui_play_list(1, "", pos);
}
static int play_radio(const rmt_cmd_t *c, const char **why){
    static rc_station_t fav[RC_MAX_FAV];
    int n = radio_favorites(fav, RC_MAX_FAV);
    if(c->pos < 1 || c->pos > n || (c->name[0] && strcmp(c->name, fav[c->pos - 1].url))){ *why = "The radio favourites changed - reload"; return -1; }
    if(radio_play(&fav[c->pos - 1]) == 0) return 0;
    const char *e = radio_last_error();
    *why = e && e[0] ? e : "Couldn't start the radio";
    return -1;
}
static int play_item(const rmt_cmd_t *c, const char **why){
    static const char *const REFUSED = "The player refused - see the Disc";
    if(c->sub == RMT_I_RADIO) return play_radio(c, why);   /* the radio switches the player's mode itself */
    if(!ui_local_playback_allowed()){ *why = "The player is not in local playback (e.g. USB DAC, Bluetooth or AirPlay mode)"; return -1; }
    int ok = 0;
    switch(c->sub){
    case RMT_I_ALBUM: {
        int pos = c->song_id ? mdb_play_pos((int)c->song_id, 3, c->name) : 1;
        if(pos < 1){ *why = "That song is not in this album"; return -1; }
        ok = ui_play_list(3, c->name, pos); break; }
    case RMT_I_ARTIST: {
        mdb_plan_t plan;
        if(!mdb_artist_plan(c->name, &plan) || plan.count < 1){ *why = "Couldn't play that artist"; return -1; }
        int id = c->song_id ? (int)c->song_id : plan.ids[0];
        if(!mdb_plan_pos(&plan, id)){ mdb_plan_free(&plan); *why = "That song is not by this artist"; return -1; }
        ok = ui_play_plan(&plan, id); mdb_plan_free(&plan); break; }
    case RMT_I_PLAYLIST: ok = ui_play_playlist(c->id, c->pos ? (int)c->pos : 1); break;
    case RMT_I_FAVORITES: {
        int n = mdb_favorite_count(), pos = c->pos ? (int)c->pos : 1;
        if(n < 1){ *why = "No favourites"; return -1; }
        mdb_song_t *f = calloc((size_t)n, sizeof *f);
        if(!f){ *why = "Out of memory"; return -1; }
        int got = mdb_favorites(f, n, fw_fav_play_by_love_id());
        if(pos > got){ free(f); *why = "The favourites changed - reload"; return -1; }
        ok = fw_fav_play_by_love_id() ? ui_play_favorite(f[pos - 1].love_id, pos) : play_song_id(f[pos - 1].id);
        free(f); break; }
    case RMT_I_SONG: ok = play_song_id((int)c->song_id); break;
    case RMT_I_FILE: {
        char full[PATH_MAX]; int r = -1;
        if(sd_io_begin()){ r = card_path(c->name, full, sizeof full); sd_io_end(); }
        if(r){ *why = "No such file"; return -1; }
        if(!mdb_song_id_by_path(full)){ *why = "Not in the library yet - rescan the card on the Disc"; return -1; }
        ok = ui_play_song_by_path(full); break; }
    case RMT_I_ALL: ok = mdb_song_count() > 0 && ui_play_list(1, "", 1); break;
    default: *why = "Unknown item"; return -1;
    }
    if(!ok && !*why) *why = REFUSED;
    return ok ? 0 : -1;
}
static int dispatch(const rmt_cmd_t *c, const char **why, char **body){
    *body = NULL;
    if(c->kind == RMT_CMD_BROWSE){ *body = browse(c, why); if(!*body && !*why) *why = "Out of memory"; return *body ? 0 : -1; }
    if(c->kind == RMT_CMD_PLAY_ITEM) return play_item(c, why);
    return run(c, why);
}
/* Host tests: one request through the real routing and the main-thread half, without sockets. Returns the command's
 * result (0 ok, -1 refused with *why), or -100 - route kind when it never became a command. *out: a JSON answer. */
int remote_test_request(const char *method, const char *path_query, const char *body, char **out, const char **why){
    static const char tok[] = "testtesttesttesttest";
    char raw[REMOTE_REQ_MAX];
    *out = NULL; *why = NULL;
    snprintf(raw, sizeof raw, "%s /%s%s HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s", method, tok, path_query, strlen(body ? body : ""), body ? body : "");
    rmt_req_t r; if(rmt_req_parse(raw, &r)) return -99;
    rmt_route_t rt = rmt_route(&r, tok);
    if(rt.kind != RMT_R_CMD) return -100 - (int)rt.kind;
    return dispatch(&rt.cmd, why, out);
}
void remote_pump(void){
    static uint32_t last;
    if(!rsrv_running()) return;
    rmt_state_t s;
    if(!last || lv_tick_elaps(last) >= 250){ snapshot(&s); rsrv_publish(&s); last = lv_tick_get() | 1; }   /* a page polls 1/s */
    rmt_cmd_t c;
    if(rsrv_take(&c)){
        const char *why = NULL; char *body = NULL;
        int rc = dispatch(&c, &why, &body);
        fprintf(stderr, "remote: %s %d %ld -> %d\n", rmt_cmd_name(c.kind), c.sub, c.arg, rc); fflush(stderr);
        rsrv_done_body(rc == 0, rc == 0 ? NULL : why, body);
        if(rc == 0 && c.kind == RMT_CMD_PLAY_MODE) g_mode_at = 0;   /* show the new mode at once */
        if(rc == 0 && c.kind != RMT_CMD_BROWSE){ snapshot(&s); rsrv_publish(&s); last = lv_tick_get() | 1; }
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
