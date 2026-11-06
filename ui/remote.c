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
#include "scanner.h"
#include "system.h"
#include "modes.h"
#include <sys/statvfs.h>
#include <stdarg.h>
#include "version.h"
#include "ota.h"
#include "simple_theme.h"
/* defined elsewhere without a shared header */
int  bt_state(void);
int  bt_peer_name(char *out, int cap);
int  ui_bt_live_codec(char *out, int cap);
int  wifi_radio_live(void);
int  ui_battery(int *charging);
void settings_diskos_id(char *buf, int n);
#include <arpa/inet.h>
#include <ctype.h>
#include <pthread.h>
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
/* ---- the playing file's technical details: format and average rate on the main thread (a DB row, a stat), bit depth
 * and channels from Song Info's bounded header read on a short-lived thread (it reads the card) */
static pthread_mutex_t g_tech_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_tech_path[256], g_tech_fmt[16];
static int g_tech_kbps, g_tech_bits, g_tech_ch, g_tech_probing, g_tech_sr;
static unsigned g_tech_gen;
static void *tech_worker(void *arg){
    (void)arg;
    char path[256]; unsigned gen;
    pthread_mutex_lock(&g_tech_mu); snprintf(path, sizeof path, "%s", g_tech_path); gen = g_tech_gen; pthread_mutex_unlock(&g_tech_mu);
    int bits = 0, ch = 0, kbps = 0;
    if(sd_io_begin()){ songinfo_probe(path, &bits, &ch, &kbps); sd_io_end(); }
    pthread_mutex_lock(&g_tech_mu);
    if(gen == g_tech_gen){ g_tech_bits = bits; g_tech_ch = ch; if(kbps > 0) g_tech_kbps = kbps; }
    g_tech_probing = 0;
    pthread_mutex_unlock(&g_tech_mu);
    return NULL;
}
static void tech_fill(rmt_state_t *s, const track_state_t *st){
    if(!st->have_track || !st->path[0]) return;
    pthread_mutex_lock(&g_tech_mu);
    if(strcmp(g_tech_path, st->path)){
        snprintf(g_tech_path, sizeof g_tech_path, "%s", st->path); g_tech_gen++;
        g_tech_kbps = g_tech_bits = g_tech_ch = g_tech_sr = 0; g_tech_fmt[0] = 0;
        const char *ext = strrchr(st->path, '.'), *sl = strrchr(st->path, '/');
        if(ext && (!sl || ext > sl) && ext[1]){ size_t i; for(i = 0; i < sizeof g_tech_fmt - 1 && ext[1 + i]; i++) g_tech_fmt[i] = (char)toupper((unsigned char)ext[1 + i]); g_tech_fmt[i] = 0; }
        pthread_mutex_unlock(&g_tech_mu);
        int br = 0, sr = 0;
        mdb_song_rates(st->path, &br, &sr);                /* the scanner's values, as Song Info shows them */
        pthread_mutex_lock(&g_tech_mu);
        if(br > 0) g_tech_kbps = (br + 500) / 1000;
        g_tech_sr = sr;
        if(!g_tech_probing){
            pthread_t th;
            if(pthread_create(&th, NULL, tech_worker, NULL) == 0){ pthread_detach(th); g_tech_probing = 1; }
        }
    }
    if(!g_tech_kbps && st->duration_ms > 0){              /* else the file's average, once its length is known */
        struct stat fs;
        if(stat(st->path, &fs) == 0 && fs.st_size > 0) g_tech_kbps = (int)(((long long)fs.st_size * 8 + st->duration_ms / 2) / st->duration_ms);
    }
    snprintf(s->format, sizeof s->format, "%s", st->is_dsd ? "DSD" : g_tech_fmt);
    s->kbps = st->is_dsd ? 0 : g_tech_kbps; s->bits = g_tech_bits; s->channels = g_tech_ch;
    s->sample_rate = st->sample_rate > 0 ? st->sample_rate : g_tech_sr;
    pthread_mutex_unlock(&g_tech_mu);
}
static const char *cover_file(void){                     /* the decoded cover of the playing song, or NULL */
    const char *c = ui_current_cover_src();
    if(!c) return NULL;
    return !strncmp(c, "A:", 2) ? c + 2 : c;
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
    s->favorite = -1;
    s->eq = simple_cfg_get_int("eq_preset", 0); if(s->eq < 0 || s->eq > 20) s->eq = 0;
    eqpreset_name(s->eq, s->eq_name, sizeof s->eq_name);
    rc_station_t rs;
    if(radio_active() && radio_station(&rs)){             /* the station's own codec and rate (the directory's word) */
        snprintf(s->station_url, sizeof s->station_url, "%s", rs.url);
        snprintf(s->stream_title, sizeof s->stream_title, "%s", radio_stream_title());
        snprintf(s->format, sizeof s->format, "%s", rs.codec); s->kbps = rs.bitrate;
    } else if(!music_assistant() && st.have_track){
        tech_fill(s, &st);
        s->favorite = st.is_favorite ? 1 : 0;
        const char *cf = cover_file(); struct stat cs;
        if(cf && stat(cf, &cs) == 0)                       /* the file is rewritten per track: its identity + time name it */
            snprintf(s->art, sizeof s->art, "%lx-%lx-%lx", (unsigned long)cs.st_ino, (unsigned long)cs.st_mtime, (unsigned long)cs.st_mtim.tv_nsec);
    }
}
static int radio_fav_cmd(const rmt_cmd_t *c, const char **why);
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
    case RMT_CMD_RADIO_SEARCH:
        if(radio_search(c->q)){ *why = "Connect the Disc to Wi-Fi to find stations"; return -1; }
        return 0;
    case RMT_CMD_RADIO_FAV: return radio_fav_cmd(c, why);
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
static void page_begin_with(page_t *pg, const rmt_cmd_t *c, const char *extra){   /* extra: more fields, e.g. "\"x\":1," */
    memset(pg, 0, sizeof *pg); pg->offset = c->offset; pg->limit = c->limit; pg->first = 1;
    rmt_buf_printf(&pg->b, "{\"ok\":true,%s\"items\":[", extra ? extra : "");
}
static void page_begin(page_t *pg, const rmt_cmd_t *c){ page_begin_with(pg, c, NULL); }
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
/* The favourites' SONG.IDs, sorted, so song lists can mark them (one query per list, not one per row). */
typedef struct { int *ids, n; } favset_t;
static int int_cmp(const void *a, const void *b){ int x = *(const int *)a, y = *(const int *)b; return x < y ? -1 : x > y; }
static void favset_load(favset_t *f){
    f->ids = NULL; f->n = 0;
    int n = mdb_favorite_count(); if(n <= 0) return;
    mdb_song_t *rows = calloc((size_t)n, sizeof *rows); f->ids = calloc((size_t)n, sizeof *f->ids);
    if(!rows || !f->ids){ free(rows); free(f->ids); f->ids = NULL; return; }
    int got = mdb_favorites(rows, n, 0);
    for(int i = 0; i < got && i < n; i++) if(rows[i].id > 0) f->ids[f->n++] = rows[i].id;
    free(rows);
    qsort(f->ids, (size_t)f->n, sizeof *f->ids, int_cmp);
}
static int favset_has(const favset_t *f, int id){ return f->n && bsearch(&id, f->ids, (size_t)f->n, sizeof id, int_cmp) != NULL; }
static favset_t *g_fs;                                  /* the set for the list being written (NULL: no "fav" field) */
static int g_fs_all;                                    /* the list is the favourites themselves */
static void song_json(rmt_buf_t *b, const mdb_song_t *s, long pos){
    rmt_buf_printf(b, "{\"id\":%d,\"title\":", s->id); rmt_buf_str(b, s->title);
    if(g_fs) rmt_buf_printf(b, ",\"fav\":%s", g_fs_all || favset_has(g_fs, s->id) ? "true" : "false");
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
        favset_t fs; favset_load(&fs); g_fs = &fs;
        int filter = c->sub != RMT_L_SONGS;                          /* search already filtered */
        for(int i = 0; i < got && i < cap; i++){
            if((filter && !song_hit(rows[i], c->q)) || !page_take(&pg)) continue;
            song_json(&pg.b, rows[i], 0);
        }
        g_fs = NULL; free(fs.ids);
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
        if(!fav && got > 0){                                         /* playlist rows carry no SONG.ID: look them up */
            int *ids = calloc((size_t)got, sizeof *ids);
            int k = ids ? mdb_playlist_song_ids(c->id, ids, got) : 0;
            for(int i = 0; i < k && i < got; i++) rows[i].id = ids[i];
            free(ids);
        }
        page_begin(&pg, c);
        favset_t fs = { NULL, 0 };
        if(!fav) favset_load(&fs);
        g_fs = &fs; g_fs_all = fav;                                   /* every favourite is one */
        for(int i = 0; i < got && i < n; i++){
            if(!song_hit(&rows[i], c->q) || !page_take(&pg)) continue;
            song_json(&pg.b, &rows[i], i + 1);                       /* pos: what play_item takes to start here */
        }
        g_fs = NULL; g_fs_all = 0; free(fs.ids);
        free(rows);
        return page_end(&pg); }
    case RMT_L_FOLDER: return list_folder(c, why);
    case RMT_L_RADIO: case RMT_L_RADIO_HITS: {             /* the Radio app's favourites, or its last search; pos counts from 1 */
        static rc_station_t fav[RC_MAX_FAV], hits[RC_MAX_HITS];
        static const char *const SEARCH[] = { "idle", "busy", "done", "failed" };
        int nf = radio_favorites(fav, RC_MAX_FAV), n = nf, st = 0;
        const rc_station_t *l = fav;
        if(c->sub == RMT_L_RADIO_HITS){
            n = radio_search_result(hits, RC_MAX_HITS, &st); l = hits;
            char ex[32]; snprintf(ex, sizeof ex, "\"search\":\"%s\",", SEARCH[st >= 0 && st <= 3 ? st : 0]);
            page_begin_with(&pg, c, ex);
        } else page_begin(&pg, c);
        for(int i = 0; i < n; i++){
            if(!(rmt_match(l[i].name, c->q) || rmt_match(l[i].country, c->q)) || !page_take(&pg)) continue;
            rmt_buf_printf(&pg.b, "{\"pos\":%d,\"name\":", i + 1); rmt_buf_str(&pg.b, l[i].name);
            rmt_buf_put(&pg.b, ",\"url\":"); rmt_buf_str(&pg.b, l[i].url);
            rmt_buf_put(&pg.b, ",\"codec\":"); rmt_buf_str(&pg.b, l[i].codec);
            rmt_buf_printf(&pg.b, ",\"bitrate\":%d,\"country\":", l[i].bitrate); rmt_buf_str(&pg.b, l[i].country);
            rmt_buf_printf(&pg.b, ",\"fav\":%s}", l == fav || rc_fav_find(fav, nf, l[i].url) >= 0 ? "true" : "false");
        }
        return page_end(&pg); }
    default: *why = "Unknown list"; return out;
    }
}
static int play_song_id(int id){                       /* in the all-songs scope, as Songs / Search / Files do */
    int pos = mdb_play_pos(id, 1, "");
    return pos >= 1 && ui_play_list(1, "", pos);
}
/* The station a request names: favourite or search hit number pos, checked against its url when one is given. */
static const rc_station_t *radio_pick(const rmt_cmd_t *c, const char **why){
    static rc_station_t l[RC_MAX_FAV > RC_MAX_HITS ? RC_MAX_FAV : RC_MAX_HITS];
    int hit = c->sub == RMT_I_RADIO_HIT, st = 0;
    int n = hit ? radio_search_result(l, RC_MAX_HITS, &st) : radio_favorites(l, RC_MAX_FAV);
    if(c->pos < 1 || c->pos > n || (c->name[0] && strcmp(c->name, l[c->pos - 1].url))){
        *why = hit ? "The search results changed - search again" : "The radio favourites changed - reload"; return NULL;
    }
    return &l[c->pos - 1];
}
static int radio_fav_cmd(const rmt_cmd_t *c, const char **why){
    const rc_station_t *s = radio_pick(c, why);
    if(!s) return -1;
    if((c->arg ? radio_fav_add(s) : radio_fav_remove(s->url)) == 0) return 0;
    const char *e = radio_last_error();
    *why = e && e[0] ? e : "Couldn't save favourites";
    return -1;
}
static int play_radio(const rmt_cmd_t *c, const char **why){
    const rc_station_t *s = radio_pick(c, why);
    if(!s) return -1;
    rc_station_t copy = *s;
    if(radio_play(&copy) == 0) return 0;
    const char *e = radio_last_error();
    *why = e && e[0] ? e : "Couldn't start the radio";
    return -1;
}
static int play_item(const rmt_cmd_t *c, const char **why){
    static const char *const REFUSED = "The player refused - see the Disc";
    if(c->sub == RMT_I_RADIO || c->sub == RMT_I_RADIO_HIT) return play_radio(c, why);   /* the radio switches the player's mode itself */
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
/* ---------------------------------------------------------------- the Status tab (main thread; files under /proc and
 * /sys, statvfs, cached Bluetooth/Wi-Fi facts - nothing that spawns or waits, except the Wi-Fi status thread below) */
static pthread_mutex_t g_wst_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_wst_ssid[64], g_wst_freq[16], g_wst_sec[32], g_wst_bssid[24], g_wst_cipher[16], g_wst_gen[8];
static uint32_t g_wst_at; static int g_wst_busy, g_wst_want_wifi, g_wst_want_bt;
static long long g_bt_rx = -1, g_bt_tx = -1;
static void *wst_worker(void *arg){                       /* wpa_cli status, as Settings > Wi-Fi reads it; hciconfig's counters */
    (void)arg;
    char ssid[64] = "", freq[16] = "", sec[32] = "", bssid[24] = "", cipher[16] = "", gen[8] = "", line[160];
    pthread_mutex_lock(&g_wst_mu); int want_wifi = g_wst_want_wifi, want_bt = g_wst_want_bt; pthread_mutex_unlock(&g_wst_mu);
    long long brx = -1, btx = -1;
    FILE *h = want_bt ? popen("/usr/bin/hciconfig hci0 2>/dev/null", "r") : NULL;
    if(h){
        while(fgets(line, sizeof line, h)){
            char *q;
            if((q = strstr(line, "RX bytes:"))) brx = atoll(q + 9);
            if((q = strstr(line, "TX bytes:"))) btx = atoll(q + 9);
        }
        pclose(h);
    }
    FILE *p = want_wifi ? popen("/usr/sbin/wpa_cli -i wlan0 status 2>/dev/null", "r") : NULL;
    if(p){
        while(fgets(line, sizeof line, p)){
            line[strcspn(line, "\r\n")] = 0;
            if(!strncmp(line, "ssid=", 5)) snprintf(ssid, sizeof ssid, "%.63s", line + 5);
            else if(!strncmp(line, "freq=", 5)) snprintf(freq, sizeof freq, "%.15s", line + 5);
            else if(!strncmp(line, "key_mgmt=", 9)) snprintf(sec, sizeof sec, "%.31s", line + 9);
            else if(!strncmp(line, "pairwise_cipher=", 16)) snprintf(cipher, sizeof cipher, "%.15s", line + 16);
            else if(!strncmp(line, "wifi_generation=", 16)) snprintf(gen, sizeof gen, "%.7s", line + 16);
            else if(!strncmp(line, "bssid=", 6)) snprintf(bssid, sizeof bssid, "%.23s", line + 6);
        }
        pclose(p);
    }
    pthread_mutex_lock(&g_wst_mu);
    g_bt_rx = brx; g_bt_tx = btx;
    memcpy(g_wst_cipher, cipher, sizeof cipher); memcpy(g_wst_gen, gen, sizeof gen);
    memcpy(g_wst_ssid, ssid, sizeof ssid); memcpy(g_wst_freq, freq, sizeof freq); memcpy(g_wst_sec, sec, sizeof sec); memcpy(g_wst_bssid, bssid, sizeof bssid);
    g_wst_busy = 0;
    pthread_mutex_unlock(&g_wst_mu);
    return NULL;
}
static int read_line(const char *path, char *out, size_t n){
    FILE *f = fopen(path, "r"); if(!f){ if(n) out[0] = 0; return -1; }
    if(!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f); out[strcspn(out, "\r\n")] = 0;
    return out[0] ? 0 : -1;
}
static long long read_ll(const char *path){ char b[32]; return read_line(path, b, sizeof b) ? -1 : atoll(b); }
static int uevent(const char *text, const char *key, char *out, size_t n){   /* KEY=value from a uevent file */
    size_t kl = strlen(key);
    for(const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL){
        if(!strncmp(p, key, kl) && p[kl] == '='){ size_t l = strcspn(p + kl + 1, "\n"); if(l >= n) l = n - 1; memcpy(out, p + kl + 1, l); out[l] = 0; return 0; }
    }
    if(n) out[0] = 0;
    return -1;
}
typedef struct { rmt_buf_t b; int groups, rows; } st_t;
static void st_group(st_t *s, const char *name){
    rmt_buf_put(&s->b, s->groups++ ? "]},{\"name\":" : "{\"ok\":true,\"groups\":[{\"name\":");
    rmt_buf_str(&s->b, name); rmt_buf_put(&s->b, ",\"rows\":["); s->rows = 0;
}
static void st_row(st_t *s, const char *label, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void st_row(st_t *s, const char *label, const char *fmt, ...){
    char v[160]; va_list ap; va_start(ap, fmt); vsnprintf(v, sizeof v, fmt, ap); va_end(ap);
    if(!v[0]) return;
    rmt_buf_put(&s->b, s->rows++ ? ",[" : "["); rmt_buf_str(&s->b, label); rmt_buf_put(&s->b, ","); rmt_buf_str(&s->b, v); rmt_buf_put(&s->b, "]");
}
static const char *bytes(unsigned long long b, char *buf, size_t n){ sys_fmt_bytes(b, buf, n); return buf; }
static char *status_json(void){
    st_t s; memset(&s, 0, sizeof s);
    char a[64], b2[64], c[64];
    track_state_t st; ipc_get_state(&st);
    /* playback */
    static const char *const MODE[] = { "Playback", "USB DAC", "Bluetooth DAC", "USB storage", "BT streaming", "AirPlay", "Web radio" };
    int sm = ui_get_source_mode();
    st_group(&s, "Playback");
    st_row(&s, "Working mode", "%s", music_assistant() ? "Music Assistant" : sm >= 0 && sm < 7 ? MODE[sm] : "Other");
    int route = modes_output_route();
    st_row(&s, "Output", "%s", ui_route_mac()[0] ? "Bluetooth" : route == OUT_USB ? "USB-C audio" : route == OUT_SPDIF ? "S/PDIF" : "Headphone jack");
    st_row(&s, "Volume", "%d / %d", st.volume, VOL_MAX);
    char eqn[48]; int eq = simple_cfg_get_int("eq_preset", 0); eqpreset_name(eq, eqn, sizeof eqn);
    st_row(&s, "EQ", "%s", eqn);
    int sdone = 0, stot = 0, sact = scanner_active();
    if(sact){ scanner_progress(&sdone, &stot); st_row(&s, "Library scan", "Running, %d files so far", sdone); }
    if(sd_io_begin()){
        st_row(&s, "Library", "%d songs, %d albums, %d artists", mdb_song_count(), mdb_album_count(), mdb_artist_count());
        st_row(&s, "Favourites", "%d", mdb_favorite_count());
        sd_io_end();
    }
    /* the slow facts (wpa_cli, hciconfig) come from a thread, at most every 10 s */
    char ip[32] = "";
    int up = wlan_ip(ip, sizeof ip) == 0, bs = bt_state();   /* wlan_ip: 0 = has an address */
    pthread_mutex_lock(&g_wst_mu);
    if((up || bs == BT_ON) && !g_wst_busy && (!g_wst_at || lv_tick_elaps(g_wst_at) >= 10000)){
        pthread_t th; g_wst_want_wifi = up; g_wst_want_bt = bs == BT_ON;
        if(pthread_create(&th, NULL, wst_worker, NULL) == 0){ pthread_detach(th); g_wst_busy = 1; g_wst_at = lv_tick_get() | 1; }
    }
    long long brx = g_bt_rx, btx = g_bt_tx;
    pthread_mutex_unlock(&g_wst_mu);
    /* Bluetooth */
    st_group(&s, "Bluetooth");
    st_row(&s, "Bluetooth", "%s", bs == BT_ON ? "On" : bs == BT_TURNING_ON ? "Turning on" : "Off");
    if(bs == BT_ON){
        char peer[64]; int conn = bt_peer_name(peer, sizeof peer);
        st_row(&s, "Connected", "%s", conn ? (peer[0] ? peer : "Yes") : "Nothing");
        char codec[16];
        if(ui_route_mac()[0] && ui_bt_live_codec(codec, sizeof codec) && codec[0]) st_row(&s, "Codec", "%s", codec);
    }
    if(bs == BT_ON && brx >= 0 && btx >= 0)                 /* the controller's counters since Bluetooth came on */
        st_row(&s, "Data since on", "%s received, %s sent", bytes((unsigned long long)brx, a, sizeof a), bytes((unsigned long long)btx, b2, sizeof b2));
    if(read_line("/sys/class/bluetooth/hci0/address", a, sizeof a) == 0) st_row(&s, "Address", "%s", a);
    /* Wi-Fi */
    st_group(&s, "Wi-Fi");
    st_row(&s, "Wi-Fi", "%s", wifi_radio_live() ? (up ? "Connected" : "On, not connected") : "Off");
    if(up){
        pthread_mutex_lock(&g_wst_mu);
        char ssid[64], freq[16], sec[32], bssid[24], cipher[16], gen[8];
        memcpy(ssid, g_wst_ssid, sizeof ssid); memcpy(freq, g_wst_freq, sizeof freq); memcpy(sec, g_wst_sec, sizeof sec); memcpy(bssid, g_wst_bssid, sizeof bssid);
        memcpy(cipher, g_wst_cipher, sizeof cipher); memcpy(gen, g_wst_gen, sizeof gen);
        pthread_mutex_unlock(&g_wst_mu);
        st_row(&s, "Network (SSID)", "%s", ssid);
        st_row(&s, "Access point MAC", "%s", bssid);
        if(sec[0]) st_row(&s, "Security", "%s%s%s", sec, cipher[0] ? ", " : "", cipher);
        int mhz = atoi(freq);
        if(mhz >= 2412 && mhz <= 2484) st_row(&s, "Channel", "%d (%d MHz)%s%s", mhz == 2484 ? 14 : (mhz - 2407) / 5, mhz, gen[0] ? ", Wi-Fi " : "", gen);
        /* address, prefix and network from the interface; the default gateway from the kernel's route table */
        int prefix = -1; struct in_addr net = { 0 };
        int sk = socket(AF_INET, SOCK_DGRAM, 0);
        if(sk >= 0){
            struct ifreq ifr; memset(&ifr, 0, sizeof ifr); snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "wlan0");
            if(ioctl(sk, SIOCGIFNETMASK, &ifr) == 0){
                uint32_t m = ntohl(((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr.s_addr);
                prefix = 0; while(prefix < 32 && (m & (0x80000000u >> prefix))) prefix++;
                struct in_addr ia; if(inet_pton(AF_INET, ip, &ia) == 1) net.s_addr = ia.s_addr & htonl(m);
            }
            close(sk);
        }
        if(prefix >= 0){ char ns[INET_ADDRSTRLEN] = ""; inet_ntop(AF_INET, &net, ns, sizeof ns); st_row(&s, "IP address", "%s/%d (network %s)", ip, prefix, ns); }
        else st_row(&s, "IP address", "%s", ip);
        FILE *rt = fopen("/proc/net/route", "r");
        if(rt){
            char line[200], ifn[16]; unsigned long dst, gw; unsigned flags;
            while(fgets(line, sizeof line, rt))
                if(sscanf(line, "%15s %lx %lx %x", ifn, &dst, &gw, &flags) == 4 && !strcmp(ifn, "wlan0") && dst == 0 && (flags & 2)){
                    struct in_addr g; g.s_addr = (in_addr_t)gw; char gs[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &g, gs, sizeof gs);
                    st_row(&s, "Gateway", "%s", gs); break;
                }
            fclose(rt);
        }
        FILE *rc = fopen("/etc/resolv.conf", "r");
        if(rc){
            char line[128], dns[96] = ""; size_t dl = 0;
            while(fgets(line, sizeof line, rc)){
                char a1[64];
                if(sscanf(line, "nameserver %63s", a1) == 1 && dl + strlen(a1) + 3 < sizeof dns) dl += (size_t)snprintf(dns + dl, sizeof dns - dl, "%s%s", dl ? ", " : "", a1);
            }
            fclose(rc);
            st_row(&s, "DNS", "%s", dns);
        }
        FILE *w = fopen("/proc/net/wireless", "r");
        if(w){
            char line[200]; int lvl = 0, qual = 0, got = 0;
            while(fgets(line, sizeof line, w)){
                char *p = strstr(line, "wlan0:"); float q, l;
                if(p && sscanf(p + 6, "%*s %f %f", &q, &l) == 2){ lvl = (int)l; qual = (int)q; got = 1; }
            }
            fclose(w);
            if(got && lvl < 0) st_row(&s, "Signal", "%d dBm (%s)", lvl, lvl >= -55 ? "excellent" : lvl >= -67 ? "good" : lvl >= -75 ? "fair" : "weak");
            if(got && qual > 0) st_row(&s, "Link quality", "%d/70 (%d%%)", qual, qual * 100 / 70 > 100 ? 100 : qual * 100 / 70);
        }
    }
    long long rx = read_ll("/sys/class/net/wlan0/statistics/rx_bytes"), tx = read_ll("/sys/class/net/wlan0/statistics/tx_bytes");
    if(rx >= 0 && tx >= 0) st_row(&s, "Data since start", "%s received, %s sent", bytes((unsigned long long)rx, a, sizeof a), bytes((unsigned long long)tx, b2, sizeof b2));
    if(read_line("/sys/class/net/wlan0/address", a, sizeof a) == 0) st_row(&s, "MAC", "%s", a);
    /* storage */
    st_group(&s, "Storage");
    struct statvfs sv;
    if(sd_io_begin()){
        if(statvfs(FOLDERBROWSER_ROOT, &sv) == 0 && sv.f_blocks){
            unsigned long long tot = (unsigned long long)sv.f_blocks * sv.f_frsize, fr = (unsigned long long)sv.f_bavail * sv.f_frsize;
            st_row(&s, "SD card", "%s free of %s (%d%% used)", bytes(fr, a, sizeof a), bytes(tot, b2, sizeof b2), (int)((tot - fr) * 100 / tot));
        }
        FILE *m = fopen("/proc/mounts", "r");                /* the card's file system and device, as mounted now */
        if(m){
            char line[512], dev[128], mp[128], fs[32], opt[256];
            while(fgets(line, sizeof line, m))
                if(sscanf(line, "%127s %127s %31s %255s", dev, mp, fs, opt) == 4 && !strcmp(mp, FOLDERBROWSER_ROOT)){
                    const char *nm = !strcmp(fs, "vfat") ? "FAT32" : !strcmp(fs, "exfat") ? "exFAT" : !strcmp(fs, "ext4") ? "ext4" : fs;
                    st_row(&s, "File system", "%s (%s%s)", nm, dev, !strncmp(opt, "ro", 2) && (opt[2] == ',' || !opt[2]) ? ", read-only" : "");
                }
            fclose(m);
        }
        sd_io_end();
    } else st_row(&s, "SD card", "Not available");
    if(statvfs("/usr/data", &sv) == 0 && sv.f_blocks)
        st_row(&s, "Internal", "%s free of %s", bytes((unsigned long long)sv.f_bavail * sv.f_frsize, a, sizeof a), bytes((unsigned long long)sv.f_blocks * sv.f_frsize, b2, sizeof b2));
    /* battery */
    st_group(&s, "Battery");
    char ue[1024] = "";
    FILE *u = fopen("/sys/class/power_supply/cw221X-bat/uevent", "r");
    if(u){ size_t n = fread(ue, 1, sizeof ue - 1, u); ue[n] = 0; fclose(u); }
    int chg = 0, pct = ui_battery(&chg);
    if(pct >= 0) st_row(&s, "Charge", "%d%%%s", pct, chg ? ", charging" : "");
    if(!uevent(ue, "POWER_SUPPLY_VOLTAGE_NOW", a, sizeof a)) st_row(&s, "Voltage", "%.2f V", atof(a) / 1e6);
    if(!uevent(ue, "POWER_SUPPLY_TEMP", a, sizeof a)) st_row(&s, "Temperature", "%.1f \xC2\xB0" "C", atof(a) / 10.0);
    if(!uevent(ue, "POWER_SUPPLY_HEALTH", a, sizeof a)) st_row(&s, "Health", "%s", a);
    if(!uevent(ue, "POWER_SUPPLY_CYCLE_COUNT", a, sizeof a)) st_row(&s, "Cycles", "%s", a);
    /* system */
    st_group(&s, "System");
    char id[40] = ""; settings_diskos_id(id, sizeof id);
    st_row(&s, "diskOS", "%s", id[0] ? id : DISCO_VERSION);
    {   /* the label diskos-sign.sh put in the manifest of the SD/online update that runs now: S97 writes it to the run
         * record, the launcher states what it really started; a flashed UI (or records that disagree) has none */
        char t[1024], u[1024], b1[16], b2[16], h1[80], h2[80], v[40]; size_t n1 = 0, n2 = 0;
        FILE *f = fopen(OTA_DEV.run_file, "r"); if(f){ n1 = fread(t, 1, sizeof t - 1, f); fclose(f); } t[n1] = 0;
        f = fopen(OTA_DEV.launched_file, "r"); if(f){ n2 = fread(u, 1, sizeof u - 1, f); fclose(f); } u[n2] = 0;
        int same = n1 && n2 && !sys_kv(t, "BUILD", b1, sizeof b1) && !sys_kv(u, "BUILD", b2, sizeof b2) && !strcmp(b1, b2)
                && !sys_kv(t, "SHA256", h1, sizeof h1) && !sys_kv(u, "SHA256", h2, sizeof h2) && !strcmp(h1, h2);
        if(same && !sys_kv(t, "VERSION", v, sizeof v) && strcmp(b1, "flashed"))
            st_row(&s, "Update", "%s (%s)", v, !strcmp(b1, "trial") ? "on trial" : "installed");
        else st_row(&s, "Update", "none (flashed build)");
    }
    sys_about_row_t ar[12]; int na = sys_about_collect(NULL, id, ar, 12);
    for(int i = 0; i < na; i++) if(!strcmp(ar[i].label, "Firmware") || !strcmp(ar[i].label, "Model")) st_row(&s, ar[i].label, "%s", ar[i].value);
    double upt = 0; if(read_line("/proc/uptime", a, sizeof a) == 0) upt = atof(a);
    long us = (long)upt;
    if(us >= 86400) st_row(&s, "Uptime", "%ldd %ldh %ldm", us / 86400, (us % 86400) / 3600, (us % 3600) / 60);
    else if(us > 0) st_row(&s, "Uptime", "%ldh %ldm", us / 3600, (us % 3600) / 60);
    if(read_line("/proc/loadavg", a, sizeof a) == 0){ float l1, l5, l15; if(sscanf(a, "%f %f %f", &l1, &l5, &l15) == 3) st_row(&s, "Load", "%.2f, %.2f, %.2f (1, 5, 15 min)", l1, l5, l15); }
    static unsigned long long pbusy, ptot;                  /* CPU use since the last look */
    if(read_line("/proc/stat", c, sizeof c) == 0){
        unsigned long long v[8] = {0};
        if(sscanf(c, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4){
            unsigned long long tot = 0; for(int i = 0; i < 8; i++) tot += v[i];
            unsigned long long busy = tot - v[3] - v[4];
            if(ptot && tot > ptot) st_row(&s, "CPU", "%d%%", (int)((busy - pbusy) * 100 / (tot - ptot)));
            pbusy = busy; ptot = tot;
        }
    }
    FILE *m = fopen("/proc/meminfo", "r");
    if(m){
        char line[96]; long tot = -1, av = -1;
        while(fgets(line, sizeof line, m)){ sscanf(line, "MemTotal: %ld", &tot); sscanf(line, "MemAvailable: %ld", &av); }
        fclose(m);
        if(tot > 0 && av >= 0) st_row(&s, "Memory", "%ld MB free of %ld MB", av / 1024, tot / 1024);
    }
    FILE *ps = fopen("/proc/self/status", "r");
    if(ps){ char line[96]; long rss = -1; while(fgets(line, sizeof line, ps)) sscanf(line, "VmRSS: %ld", &rss); fclose(ps);
            if(rss > 0) st_row(&s, "diskOS UI memory", "%ld MB", (rss + 512) / 1024); }
    rmt_buf_put(&s.b, s.groups ? "]}]" : "{\"ok\":true,\"groups\":[]");
    rmt_buf_printf(&s.b, ",\"scan\":{\"active\":%s,\"done\":%d}}", sact ? "true" : "false", sdone);
    return rmt_buf_take(&s.b);
}
static char *eq_json(void){
    rmt_buf_t b; memset(&b, 0, sizeof b);
    int cur = simple_cfg_get_int("eq_preset", 0);
    rmt_buf_printf(&b, "{\"ok\":true,\"current\":%d,\"presets\":[", cur);
    for(int p = 0; p <= 20; p++){
        char n[48]; eqpreset_name(p, n, sizeof n);
        rmt_buf_printf(&b, "%s{\"id\":%d,\"name\":", p ? "," : "", p); rmt_buf_str(&b, n); rmt_buf_put(&b, "}");
    }
    rmt_buf_put(&b, "]}");
    return rmt_buf_take(&b);
}
/* ---------------------------------------------------------------- Bluetooth: the Bluetooth screen's own calls
 * (the radio switch, search, connect/disconnect, Forget, Rename) through bt.c */
static char *bt_json(void){
    static bt_remote_dev_t d[40];
    int st = bt_state(), busy = 0, n = st == BT_ON ? bt_remote_devices(d, 40, &busy) : 0;
    rmt_buf_t b; memset(&b, 0, sizeof b);
    rmt_buf_printf(&b, "{\"ok\":true,\"state\":\"%s\",\"busy\":%s,\"devices\":[",
                   st == BT_ON ? "on" : st == BT_TURNING_ON ? "turning_on" : "off", busy ? "true" : "false");
    for(int pass = 0, k = 0; pass < 2; pass++) for(int i = 0; i < n; i++){       /* connected first, as on the Disc */
        if((d[i].connected == 0) != pass) continue;
        rmt_buf_put(&b, k++ ? ",{\"mac\":" : "{\"mac\":"); rmt_buf_str(&b, d[i].mac);
        rmt_buf_put(&b, ",\"name\":"); rmt_buf_str(&b, d[i].shown);
        rmt_buf_put(&b, ",\"own\":"); rmt_buf_str(&b, d[i].own);
        rmt_buf_put(&b, ",\"alias\":"); rmt_buf_str(&b, d[i].alias);
        rmt_buf_printf(&b, ",\"connected\":%s,\"codec\":", d[i].connected ? "true" : "false"); rmt_buf_str(&b, d[i].codec);
        rmt_buf_put(&b, "}");
    }
    rmt_buf_put(&b, "]}");
    return rmt_buf_take(&b);
}
static int bt_cmd(const rmt_cmd_t *c, const char **why){
    int st = bt_state();
    switch(c->sub){
    case RMT_BT_ON:  if(st == BT_OFF) bt_toggle(); return 0;            /* already on or coming up: nothing to do */
    case RMT_BT_OFF: if(st != BT_OFF) bt_toggle(); return 0;
    default: break;
    }
    if(st != BT_ON){ *why = st == BT_TURNING_ON ? "Bluetooth is still turning on" : "Bluetooth is off"; return -1; }
    switch(c->sub){
    case RMT_BT_REFRESH: case RMT_BT_SEARCH: bt_remote_refresh(c->sub == RMT_BT_SEARCH); return 0;
    case RMT_BT_CONNECT: case RMT_BT_DISCONNECT:
        if(bt_remote_connect(c->q, c->sub == RMT_BT_CONNECT)){ *why = "Couldn't reach that device"; return -1; }
        return 0;
    case RMT_BT_FORGET: if(bt_remote_forget(c->q)){ *why = "Couldn't forget that device"; return -1; } return 0;
    case RMT_BT_RENAME: {
        int rc = bt_remote_rename(c->q, c->name);
        if(rc){ *why = rc == -2 ? "Name too long" : "Couldn't save the name"; return -1; }
        return 0; }
    default: *why = "Unknown action"; return -1;
    }
}
/* ---------------------------------------------------------------- playlists and favourites: the Disc's own calls
 * (Playlists screen, Add to playlist, the song menu's heart), then a Library refresh so an open list follows */
static int edit(const rmt_cmd_t *c, const char **why, char **body){
    char path[PATH_MAX];
    int ok = 0;
    switch(c->sub){
    case RMT_E_PL_CREATE: {
        long id = mdb_playlist_create(c->name);
        if(id <= 0){ *why = "Couldn't create the playlist"; return -1; }
        rmt_buf_t b; memset(&b, 0, sizeof b); rmt_buf_printf(&b, "{\"ok\":true,\"id\":%ld}", id); *body = rmt_buf_take(&b);
        ok = 1; break; }
    case RMT_E_PL_RENAME: ok = mdb_playlist_rename(c->id, c->name); if(!ok) *why = "Couldn't rename the playlist"; break;
    case RMT_E_PL_DELETE: ok = mdb_playlist_delete(c->id); if(!ok) *why = "Couldn't delete the playlist"; break;
    case RMT_E_PL_ADD: {
        int n = 0;
        if(c->item == RMT_I_SONG){
            if(mdb_song_path((int)c->song_id, path, sizeof path) != 1){ *why = "No such song"; return -1; }
            if(mdb_playlist_has_song(c->id, path)){ *why = "Already in this playlist"; return -1; }
            n = mdb_playlist_add_song(c->id, path);
        } else n = mdb_playlist_add_group(c->id, c->item == RMT_I_ALBUM ? "ALBUM" : "ARTIST", c->name);   /* skips duplicates */
        if(n <= 0){ *why = c->item == RMT_I_SONG ? "Couldn't add the song" : "Nothing added (already there?)"; return -1; }
        rmt_buf_t b; memset(&b, 0, sizeof b); rmt_buf_printf(&b, "{\"ok\":true,\"added\":%d}", n); *body = rmt_buf_take(&b);
        ok = 1; break; }
    case RMT_E_PL_REMOVE: {                               /* only if that row still holds that song: lists change under a page */
        int n = mdb_playlist_count(c->id);
        int *ids = n > 0 && c->pos <= n ? calloc((size_t)n, sizeof *ids) : NULL;
        int got = ids ? mdb_playlist_song_ids(c->id, ids, n) : 0, same = c->pos <= got && ids[c->pos - 1] == c->song_id;
        free(ids);
        if(!same){ *why = "The playlist changed - reload"; return -1; }
        ok = mdb_playlist_remove_at(c->id, (int)c->pos); if(!ok) *why = "Couldn't remove the song";
        break; }
    case RMT_E_FAV: {
        track_state_t st; ipc_get_state(&st);
        if(!c->song_id){                                    /* the song playing now: through the player, like the Disc's heart */
            if(!st.have_track || music_assistant() || radio_active()){ *why = "No song from the card is playing"; return -1; }
            ui_set_favorite((int)c->arg); ok = 1; break;
        }
        if(mdb_song_path((int)c->song_id, path, sizeof path) != 1){ *why = "No such song"; return -1; }
        if(st.have_track && !strcmp(path, st.path) && !music_assistant() && !radio_active()){ ui_set_favorite((int)c->arg); ok = 1; break; }
        ok = mdb_set_favorite_path(path, (int)c->arg) > 0; if(!ok) *why = "Couldn't change favourites";
        break; }
    default: *why = "Unknown change"; return -1;
    }
    if(ok) library_refresh();
    return ok ? 0 : -1;
}
static int g_blob_len;                                  /* dispatch's binary answer length (0 = JSON or none) */
static char *read_cover(const char **why){
    const char *cf = cover_file();
    FILE *f = cf ? fopen(cf, "rb") : NULL;
    if(!f){ *why = "No cover"; return NULL; }
    char *b = malloc(256 * 1024); size_t n = b ? fread(b, 1, 256 * 1024, f) : 0;
    fclose(f);
    if(!n || n >= 256 * 1024){ free(b); *why = "No cover"; return NULL; }   /* a 148 px BMP is ~66 KB */
    g_blob_len = (int)n;
    return b;
}
static int dispatch(const rmt_cmd_t *c, const char **why, char **body){
    *body = NULL; g_blob_len = 0;
    if(c->kind == RMT_CMD_COVER){ *body = read_cover(why); return *body ? 0 : -1; }
    if(c->kind == RMT_CMD_STATUS || c->kind == RMT_CMD_EQ_LIST){
        *body = c->kind == RMT_CMD_STATUS ? status_json() : eq_json();
        if(!*body) *why = "Out of memory";
        return *body ? 0 : -1;
    }
    if(c->kind == RMT_CMD_EDIT) return edit(c, why, body);
    if(c->kind == RMT_CMD_BT_LIST){ *body = bt_json(); if(!*body) *why = "Out of memory"; return *body ? 0 : -1; }
    if(c->kind == RMT_CMD_BT) return bt_cmd(c, why);
    if(c->kind == RMT_CMD_RESCAN){                          /* as Settings > Rescan library (the main loop reports the end) */
        if(!c->arg){
            if(!scanner_active()){ *why = "No scan is running"; return -1; }
            if(!scanner_cancel()){ *why = "The scan is saving its result and can't be stopped now"; return -1; }
            return 0;
        }
        if(scanner_active()){ *why = "A scan is already running"; return -1; }
        if(!ui_local_playback_allowed()){ *why = "Return the Disc to local playback to scan"; return -1; }
        if(!sd_io_allowed()){ *why = "The SD card is not ready"; return -1; }
        if(scanner_start() != 0){ *why = "Couldn't start the library scan"; return -1; }
        return 0;
    }
    if(c->kind == RMT_CMD_EQ){
        if(ui_eq_select((int)c->arg)){ *why = "Couldn't switch EQ"; return -1; }
        if(screen_current() == SCR_EQ) eqpreset_refresh();
        return 0;
    }
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
        int ro = c.kind == RMT_CMD_BROWSE || c.kind == RMT_CMD_COVER || c.kind == RMT_CMD_STATUS || c.kind == RMT_CMD_EQ_LIST || c.kind == RMT_CMD_BT_LIST;
        if(!ro || rc != 0){ fprintf(stderr, "remote: %s %d %ld -> %d\n", rmt_cmd_name(c.kind), c.sub, c.arg, rc); fflush(stderr); }   /* pages poll the reads: log only changes and failures */
        if(rc == 0 && g_blob_len > 0) rsrv_done_blob(body, (size_t)g_blob_len, "image/bmp");
        else rsrv_done_body(rc == 0, rc == 0 ? NULL : why, body);
        if(rc == 0 && c.kind == RMT_CMD_PLAY_MODE) g_mode_at = 0;   /* show the new mode at once */
        if(rc == 0 && !ro){ snapshot(&s); rsrv_publish(&s); last = lv_tick_get() | 1; }
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
