/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* MPD server: the UI side (Settings > Connectivity > Remote Control > MPD Server) and every command that touches the
 * player or the library. Runs on the main thread (mpd_pump), through the same functions as the Remote Control page
 * (remote_exec: ui_transport_command, ui_set_volume, ui_seek_to, play modes, rescan), so all their guards apply.
 *
 * The MPD queue ("playlist") and the Disc
 * The stock player owns the list it plays; diskOS adds its own Queue (songs that play after the current one, ui/queue.c).
 * An MPD client sees:
 *   position 0         the song playing now
 *   then               the songs in the diskOS Queue (what "add" puts there)
 *   then               Up Next: the rest of the list the player plays, in play order (shuffled order in Shuffle)
 * "clear" empties the diskOS Queue, pauses, and starts a new list: "add" then collects songs, and "play" hands them to the
 * player as one list (the reserved playlist slot, as album and artist plays use). Only songs added through the diskOS
 * Queue (or a new list) can be deleted or moved; Up Next belongs to the player.
 * URIs are paths relative to the SD card root; a CUE/ISO sub-track is "<path>/trackNNNN" (as MPD names CUE tracks).
 *
 * Not supported (answered with an ACK): consume, crossfade, MixRamp, replay gain changes, outputs other than the Disc,
 * stickers, partitions, channels, regular-expression filters, album art (stage 2). stop pauses (the player has no stop). */
#include "mpd.h"
#include "mpd_srv.h"
#include "remote.h"
#include "screens.h"
#include "theme.h"
#include "theme_kit.h"
#include "config.h"
#include "ipc.h"
#include "musicdb.h"
#include "radio.h"
#include "scanner.h"
#include "folderbrowser.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

void ui_loop_wake(void);          /* main.c */

#define VIEW_MAX   500             /* Up Next rows shown to clients */
#define FULL_TAGS  100             /* queue entries answered with the library's full tags (one lookup each); the rest with what the list holds */
#define STAGE_MAX  2000            /* songs in a new list (clear + add) */
#define ID_QUEUE   1000000         /* MPD song ids: Up Next rows use the player's LIST_SONG_0.ID, the rest these bases */
#define ID_STAGED  2000000
#define ID_OTHER   3000000
enum { V_CUR = 1, V_QUEUE, V_NEXT, V_STAGED, V_OTHER };
typedef struct {
    int kind, id;                  /* V_*, the MPD song id */
    int base_id, ord;              /* V_CUR/V_NEXT: LIST_SONG_0.ID and its 1-based position (0 in shuffle) */
    int idx;                       /* V_QUEUE: queue index; V_STAGED: index in the new list */
    int song_id;                   /* V_STAGED: SONG.ID (exact, also for a CUE/ISO track) */
    int dur_ms;
    char path[512];                /* absolute path (or a stream URL for V_OTHER) */
    char title[MDB_STR], artist[MDB_STR];
} ventry_t;

static char g_ip[24];
static uint32_t g_retry_at;
static char g_msg[256];            /* formatted ACK messages */
static time_t g_started;

/* ---------------------------------------------------------------- the server's life (as Remote Control's) */
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
static void server_stop(void){
    if(msrv_running()){ msrv_stop(); fprintf(stderr, "mpd: stopped\n"); fflush(stderr); }
    g_ip[0] = 0;
}
static int handler(void *ud, int argc, char **argv, rmt_buf_t *out, const char **msg, const mpd_sess_t *sess);
static void server_sync(void){
    if(!cfg_get_int(MPD_CFG_ON, 0)){ server_stop(); return; }
    char ip[24];
    if(wlan_ip(ip, sizeof ip)){ server_stop(); return; }
    if(msrv_running() && !strcmp(ip, g_ip)) return;
    server_stop();
    if(g_retry_at && (int32_t)(lv_tick_get() - g_retry_at) < 0) return;
    const char *t = remote_token();
    if(!t || msrv_start(ip, MPD_PORT, t)){
        g_retry_at = lv_tick_get() + 5000;
        fprintf(stderr, "mpd: can't listen on %s:%d\n", ip, MPD_PORT); fflush(stderr);
        return;
    }
    g_retry_at = 0;
    snprintf(g_ip, sizeof g_ip, "%s", ip);
    fprintf(stderr, "mpd: listening on %s:%d\n", ip, MPD_PORT); fflush(stderr);
}
static void mpd_tick(lv_timer_t *t){ (void)t; server_sync(); }
void mpd_boot(void){
    g_started = time(NULL);
    msrv_set_notify(ui_loop_wake);
    msrv_set_handler(handler, NULL);
    lv_timer_create(mpd_tick, 1000, NULL);
    server_sync();
}
void mpd_set_on(int on){
    if(on && !remote_token()){ cfg_set_int(MPD_CFG_ON, 0); ui_toast("Couldn't create a password"); return; }
    g_retry_at = 0;
    server_sync();
    if(on && !msrv_running()) ui_toast(wlan_ip((char[24]){0}, 24) ? "Starts when Wi-Fi is connected" : "Couldn't start - try again");
}
void mpd_token_changed(void){ server_stop(); g_retry_at = 0; server_sync(); }

/* ---------------------------------------------------------------- URIs */
static const char ROOT[] = FOLDERBROWSER_ROOT;
/* SD path -> URI ("Music/a.flac", "Music/b.cue/track0002"). 0 ok, -1 not on the card. */
static int to_uri(const char *path, int sub, int track, char *out, size_t cap){
    size_t rl = strlen(ROOT);
    if(strncmp(path, ROOT, rl) || path[rl] != '/') return -1;
    int n = sub ? snprintf(out, cap, "%s/track%04d", path + rl + 1, track) : snprintf(out, cap, "%s", path + rl + 1);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}
static int uri_ok(const char *uri){ return rmt_relpath_ok(uri) && strlen(uri) < 400; }
static void abs_of(const char *uri, char *out, size_t cap){
    if(uri[0]) snprintf(out, cap, "%s/%s", ROOT, uri); else snprintf(out, cap, "%s", ROOT);
}
static const char *strip_slashes(char *u){              /* clients send "Music/" and "/" too */
    size_t n = strlen(u); while(n && u[n - 1] == '/') u[--n] = 0;
    while(*u == '/') u++;
    return u;
}

/* ---------------------------------------------------------------- songs */
typedef struct { char track[12], disc[12], file[600]; mpd_tags_t t; } tagbuf_t;
static void row_tags(const mdb_row_t *r, tagbuf_t *tb){
    memset(&tb->t, 0, sizeof tb->t);
    if(to_uri(r->path, r->is_cue || r->is_iso, r->track, tb->file, sizeof tb->file)) tb->file[0] = 0;
    snprintf(tb->track, sizeof tb->track, "%d", r->track > 0 ? r->track : 0); if(r->track <= 0) tb->track[0] = 0;
    snprintf(tb->disc, sizeof tb->disc, "%d", r->disc > 0 ? r->disc : 0); if(r->disc <= 0) tb->disc[0] = 0;
    tb->t.v[MPD_TAG_FILE] = tb->file; tb->t.v[MPD_TAG_TITLE] = r->title[0] ? r->title : r->name;
    tb->t.v[MPD_TAG_ARTIST] = r->artist; tb->t.v[MPD_TAG_ALBUM] = r->album;
    tb->t.v[MPD_TAG_ALBUMARTIST] = r->album_artist[0] ? r->album_artist : r->artist;
    tb->t.v[MPD_TAG_GENRE] = r->genre; tb->t.v[MPD_TAG_TRACK] = tb->track; tb->t.v[MPD_TAG_DISC] = tb->disc;
    tb->t.v[MPD_TAG_DATE] = r->date; tb->t.v[MPD_TAG_NAME] = "";
}
static void kv_if(rmt_buf_t *o, const char *k, const char *v){ if(v && v[0]) mpd_kv(o, k, v); }
static void print_time(rmt_buf_t *o, int ms){
    if(ms <= 0) return;
    rmt_buf_printf(o, "Time: %d\nduration: %d.%03d\n", (ms + 500) / 1000, ms / 1000, ms % 1000);
}
static void print_row(rmt_buf_t *o, const mdb_row_t *r){
    tagbuf_t tb; row_tags(r, &tb);
    if(!tb.file[0]) return;
    mpd_kv(o, "file", tb.file);
    kv_if(o, "Title", tb.t.v[MPD_TAG_TITLE]); kv_if(o, "Artist", r->artist); kv_if(o, "Album", r->album);
    kv_if(o, "AlbumArtist", r->album_artist); kv_if(o, "Genre", r->genre); kv_if(o, "Date", r->date);
    kv_if(o, "Track", tb.track); kv_if(o, "Disc", tb.disc);
    print_time(o, r->dur_ms);
}
/* the first library row of a path (and sub-track), for a queue entry's full tags */
typedef struct { const char *want_path; int found; rmt_buf_t *o; } one_t;
static int one_cb(void *ud, const mdb_row_t *r){ one_t *x = ud; print_row(x->o, r); x->found = 1; return 1; }

typedef struct { int track, found; rmt_buf_t *o; } sub_t;
static int sub_cb(void *ud, const mdb_row_t *r){ sub_t *y = ud; if((r->is_cue || r->is_iso) && r->track == y->track){ print_row(y->o, r); y->found = 1; return 1; } return 0; }
/* Every SONG.ID a URI names: a file (all its sub-tracks), one sub-track, a directory (recursively), "" = everything. */
typedef struct { int *ids, n, cap, oom; int want_track; } idlist_t;
static int idl_add(idlist_t *l, int id){
    if(l->n == l->cap){ int nc = l->cap ? l->cap * 2 : 64; int *p = realloc(l->ids, (size_t)nc * sizeof *p); if(!p){ l->oom = 1; return 1; } l->ids = p; l->cap = nc; }
    l->ids[l->n++] = id; return 0;
}
static int idl_cb(void *ud, const mdb_row_t *r){ return idl_add(ud, r->id); }
static int idl_track_cb(void *ud, const mdb_row_t *r){ idlist_t *l = ud; if((r->is_cue || r->is_iso) && r->track == l->want_track) return idl_add(l, r->id); return 0; }
static int uri_ids(char *uri_in, idlist_t *l){         /* 0 ok (l->n may be 0), -1 database error */
    memset(l, 0, sizeof *l);
    const char *uri = strip_slashes(uri_in);
    if(!uri[0]) return mdb_rows(MDB_ROWS_ALL, NULL, 0, idl_cb, l) < 0 ? -1 : 0;
    if(!uri_ok(uri)) return 0;
    char p[600]; abs_of(uri, p, sizeof p);
    if(mdb_rows(MDB_ROWS_PATH, p, 0, idl_cb, l) < 0) return -1;
    if(l->n) return 0;
    const char *sl = strrchr(p, '/'); int tr = 0; char extra;
    if(sl && sscanf(sl + 1, "track%d%c", &tr, &extra) == 1 && tr > 0){
        char base[600]; snprintf(base, sizeof base, "%.*s", (int)(sl - p), p);
        l->want_track = tr;
        if(mdb_rows(MDB_ROWS_PATH, base, 0, idl_track_cb, l) < 0) return -1;
        if(l->n) return 0;
    }
    return mdb_rows(MDB_ROWS_UNDER, p, 0, idl_cb, l) < 0 ? -1 : 0;
}

/* ---------------------------------------------------------------- a new list (clear, then add) */
typedef struct { int id, dur_ms; char path[512], title[MDB_STR], artist[MDB_STR]; } sitem_t;
static struct { int on, n, cap; sitem_t *it; unsigned gen; unsigned path_seq; } g_st;
static void stage_clear(int on){ g_st.on = on; g_st.n = 0; g_st.gen++; track_state_t st; ipc_get_state(&st); g_st.path_seq = st.path_seq; }
static int stage_has(int id){ for(int i = 0; i < g_st.n; i++) if(g_st.it[i].id == id) return 1; return 0; }
static int stage_cb(void *ud, const mdb_row_t *r){
    (void)ud;
    if(stage_has(r->id)) return 0;                      /* one row per song: the slot refuses duplicates */
    if(g_st.n >= STAGE_MAX) return 1;
    if(g_st.n == g_st.cap){ int nc = g_st.cap ? g_st.cap * 2 : 64; sitem_t *p = realloc(g_st.it, (size_t)nc * sizeof *p); if(!p) return 1; g_st.it = p; g_st.cap = nc; }
    sitem_t *s = &g_st.it[g_st.n++];
    s->id = r->id; s->dur_ms = r->dur_ms;
    snprintf(s->path, sizeof s->path, "%s", r->path);
    snprintf(s->title, sizeof s->title, "%s", r->title[0] ? r->title : r->name);
    snprintf(s->artist, sizeof s->artist, "%s", r->artist);
    return 0;
}

/* ---------------------------------------------------------------- the queue view */
static ventry_t *g_view; static int g_nview;
static mdb_qrow_t *g_rows;
static struct { unsigned path_seq, qgen, sgen; int pos_id, mode, have, sig_src; char radio[16]; uint32_t at; int valid; } g_vkey;
static unsigned g_plver = 1, g_vsig;
static uint32_t g_ev_pending;                         /* events found while building (sent by the pump) */
static void view_invalidate(void){ g_vkey.valid = 0; }
static ventry_t *vadd(int kind){
    if(g_nview >= VIEW_MAX + 400 + 2) return NULL;
    ventry_t *v = &g_view[g_nview++]; memset(v, 0, sizeof *v); v->kind = kind; return v;
}
static unsigned fnv(unsigned h, const void *p, size_t n){ const unsigned char *c = p; while(n--){ h ^= *c++; h *= 16777619u; } return h; }
static void view_build(const rmt_state_t *s){
    g_nview = 0;
    if(!g_view) g_view = calloc(VIEW_MAX + 400 + 2, sizeof *g_view);
    if(!g_rows) g_rows = calloc(VIEW_MAX + 1, sizeof *g_rows);
    if(!g_view || !g_rows) return;
    if(g_st.on){
        for(int i = 0; i < g_st.n; i++){
            ventry_t *v = vadd(V_STAGED); if(!v) break;
            v->id = ID_STAGED + i; v->idx = i; v->dur_ms = g_st.it[i].dur_ms; v->song_id = g_st.it[i].id;
            snprintf(v->path, sizeof v->path, "%s", g_st.it[i].path);
            snprintf(v->title, sizeof v->title, "%s", g_st.it[i].title); snprintf(v->artist, sizeof v->artist, "%s", g_st.it[i].artist);
        }
        return;
    }
    int local = !strcmp(s->source, "local");
    if(!local){                                          /* web radio, Music Assistant, another mode: the one thing playing */
        if(!s->has_track && strcmp(s->source, "radio")) return;
        ventry_t *v = vadd(V_OTHER);
        v->id = ID_OTHER; v->dur_ms = (int)s->duration_ms;
        snprintf(v->path, sizeof v->path, "%s", s->station_url[0] ? s->station_url : "");
        snprintf(v->title, sizeof v->title, "%.*s", (int)sizeof v->title - 1, s->stream_title[0] ? s->stream_title : s->title);
        snprintf(v->artist, sizeof v->artist, "%s", s->station[0] ? s->station : s->artist);
        return;
    }
    track_state_t st; ipc_get_state(&st);
    int n = 0, more = 0, mode = ui_play_mode_now();
    if(st.have_track && st.path[0] && !mdb_is_book_path(st.path) && mode >= 0)
        n = mdb_upnext(mode == 1, st.pos_id, st.path, g_rows, VIEW_MAX + 1, &more);
    if(n > 0){
        ventry_t *v = vadd(V_CUR);
        v->id = g_rows[0].base_id; v->base_id = g_rows[0].base_id; v->ord = g_rows[0].ord; v->dur_ms = g_rows[0].dur_ms;
        snprintf(v->path, sizeof v->path, "%s", g_rows[0].path);
        snprintf(v->title, sizeof v->title, "%s", st.title[0] ? st.title : g_rows[0].title);
        snprintf(v->artist, sizeof v->artist, "%s", st.artist[0] ? st.artist : g_rows[0].artist);
    } else if(st.have_track){
        ventry_t *v = vadd(V_CUR);
        v->id = st.pos_id > 0 ? st.pos_id : ID_OTHER + 1; v->base_id = st.pos_id; v->dur_ms = (int)st.duration_ms;
        snprintf(v->path, sizeof v->path, "%s", st.path);
        snprintf(v->title, sizeof v->title, "%s", st.title); snprintf(v->artist, sizeof v->artist, "%s", st.artist);
    }
    int qn = queue_count(), skip = 1;
    char qpath[256];
    int active = queue_active();
    for(int i = 0; i < qn; i++){
        ventry_t *v = vadd(V_QUEUE); if(!v) break;
        long dur = 0;
        queue_item(i, qpath, sizeof qpath, v->title, sizeof v->title, v->artist, sizeof v->artist, &dur);
        snprintf(v->path, sizeof v->path, "%s", qpath);
        v->id = ID_QUEUE + i; v->idx = i; v->dur_ms = (int)dur;
        /* while the player plays the queue's copy, the queued songs are already in its list right after the current
         * one: show them once, as queue entries */
        if(active && skip < n && !strcmp(g_rows[skip].path, qpath)) skip++;
    }
    for(int i = skip; i < n; i++){
        ventry_t *v = vadd(V_NEXT); if(!v) break;
        v->id = g_rows[i].base_id; v->base_id = g_rows[i].base_id; v->ord = g_rows[i].ord; v->dur_ms = g_rows[i].dur_ms;
        snprintf(v->path, sizeof v->path, "%s", g_rows[i].path);
        snprintf(v->title, sizeof v->title, "%s", g_rows[i].title); snprintf(v->artist, sizeof v->artist, "%s", g_rows[i].artist);
    }
}
/* The view, rebuilt when what it depends on changed (or after 3 s: Up Next can change under us). Bumps the playlist
 * version when its content differs. */
static void view_get(const rmt_state_t *s, int fresh){
    track_state_t st; ipc_get_state(&st);
    int mode = ui_play_mode_now();
    const char *rs = s->radio_state ? s->radio_state : "off";
    if(!fresh && g_vkey.valid && g_vkey.path_seq == st.path_seq && g_vkey.pos_id == st.pos_id && g_vkey.mode == mode
       && g_vkey.qgen == queue_generation() && g_vkey.sgen == g_st.gen && g_vkey.have == s->has_track
       && !strcmp(g_vkey.radio, rs) && lv_tick_elaps(g_vkey.at) < 3000) return;
    view_build(s);
    g_vkey.valid = 1; g_vkey.path_seq = st.path_seq; g_vkey.pos_id = st.pos_id; g_vkey.mode = mode;
    g_vkey.qgen = queue_generation(); g_vkey.sgen = g_st.gen; g_vkey.have = s->has_track; g_vkey.at = lv_tick_get();
    snprintf(g_vkey.radio, sizeof g_vkey.radio, "%s", rs);
    unsigned h = 2166136261u;
    for(int i = 0; i < g_nview; i++){
        h = fnv(h, &g_view[i].kind, sizeof g_view[i].kind); h = fnv(h, &g_view[i].id, sizeof g_view[i].id);
        h = fnv(h, g_view[i].path, strlen(g_view[i].path)); h = fnv(h, g_view[i].title, strlen(g_view[i].title));
    }
    h = fnv(h, &g_nview, sizeof g_nview);
    if(h != g_vsig){ g_vsig = h; g_plver++; g_ev_pending |= MPD_EV_PLAYLIST; }
}
static int cur_pos(void){ return g_nview > 0 && (g_view[0].kind == V_CUR || g_view[0].kind == V_OTHER) ? 0 : -1; }
static void print_entry(rmt_buf_t *o, int pos){
    const ventry_t *v = &g_view[pos];
    int printed = 0;
    if(v->kind != V_OTHER && v->path[0] && pos < FULL_TAGS){   /* the library's full tags when it has the file */
        one_t x = { v->path, 0, o };
        if((v->song_id > 0 ? mdb_rows(MDB_ROWS_ID, NULL, v->song_id, one_cb, &x) : mdb_rows(MDB_ROWS_PATH, v->path, 0, one_cb, &x)) > 0 && x.found) printed = 1;
    }
    if(!printed){
        char uri[600];
        if(v->kind == V_OTHER) snprintf(uri, sizeof uri, "%s", v->path[0] ? v->path : "stream");
        else if(to_uri(v->path, 0, 0, uri, sizeof uri)) snprintf(uri, sizeof uri, "%s", v->path);
        mpd_kv(o, "file", uri);
        kv_if(o, "Title", v->title); kv_if(o, "Artist", v->artist);
        if(v->kind == V_OTHER) kv_if(o, "Name", v->artist);
        print_time(o, v->dur_ms);
    }
    rmt_buf_printf(o, "Pos: %d\nId: %d\n", pos, v->id);
}
static int pos_of_id(long id){ for(int i = 0; i < g_nview; i++) if(g_view[i].id == id) return i; return -1; }

/* ---------------------------------------------------------------- the player */
static int rexec(rmt_cmd_t *c, const char **msg){
    const char *why = NULL; char *body = NULL;
    int rc = remote_exec(c, &why, &body);
    free(body);
    if(rc){ *msg = why ? why : "The player refused - see the Disc"; return MPD_ACK_SYSTEM; }
    return 0;
}
static int rsimple(rmt_cmd_kind_t k, long arg, const char **msg){
    rmt_cmd_t c; memset(&c, 0, sizeof c); c.kind = k; c.arg = arg; return rexec(&c, msg);
}
static const char *const NOT_LOCAL = "The Disc is not in local playback (e.g. USB DAC, Bluetooth or AirPlay mode)";
static int play_staged(int idx, const char **msg){
    if(idx < 0 || idx >= g_st.n){ *msg = "Bad song index"; return MPD_ACK_ARG; }
    if(!ui_local_playback_allowed()){ *msg = NOT_LOCAL; return MPD_ACK_SYSTEM; }
    mdb_plan_t plan; memset(&plan, 0, sizeof plan);
    plan.list_type = 5; plan.count = g_st.n; plan.ids = malloc((size_t)g_st.n * sizeof *plan.ids);
    if(!plan.ids){ *msg = "Out of memory"; return MPD_ACK_SYSTEM; }
    for(int i = 0; i < g_st.n; i++) plan.ids[i] = g_st.it[i].id;
    int ok = ui_play_plan(&plan, g_st.it[idx].id);
    mdb_plan_free(&plan);
    if(!ok){ *msg = "The player refused the list - see the Disc"; return MPD_ACK_SYSTEM; }
    stage_clear(0);
    view_invalidate();
    return 0;
}
static int play_pos(int pos, const char **msg){
    if(pos < 0 || pos >= g_nview){ *msg = "Bad song index"; return MPD_ACK_ARG; }
    const ventry_t *v = &g_view[pos];
    switch(v->kind){
    case V_STAGED: return play_staged(v->idx, msg);
    case V_CUR: case V_OTHER: return rsimple(RMT_CMD_PLAY, 0, msg);
    case V_QUEUE:
        if(!ui_local_playback_allowed()){ *msg = NOT_LOCAL; return MPD_ACK_SYSTEM; }
        queue_jump(v->idx); view_invalidate(); return 0;
    case V_NEXT: {
        if(!ui_local_playback_allowed()){ *msg = NOT_LOCAL; return MPD_ACK_SYSTEM; }
        int ord = mdb_queue_ord(v->base_id, v->path);
        if(ord < 0){ *msg = "The player is busy - try again"; return MPD_ACK_SYSTEM; }
        if(ord == 0 || (v->ord && ord != v->ord)){ view_invalidate(); *msg = "The list changed - read it again"; return MPD_ACK_NO_EXIST; }
        if(ui_queue_jump(ord)){ *msg = "The player refused - see the Disc"; return MPD_ACK_SYSTEM; }
        view_invalidate(); return 0; }
    }
    *msg = "Bad song index"; return MPD_ACK_ARG;
}

/* ---------------------------------------------------------------- library walks */
typedef struct {
    rmt_buf_t *o; const mpd_filter_t *f;
    int mode;                      /* W_* */
    char prefix[600]; size_t plen; /* lsinfo / listall: the directory's absolute path + '/' */
    char urip[600];                /* ... and its URI + '/' ("" at the root) */
    char lastdir[600];             /* the last directory printed, relative to prefix */
    long start, end, k;            /* window */
    idlist_t *ids;                 /* findadd / searchadd */
    long long songs, ms;           /* count (ms of a whole card overflows a 32-bit long) */
} walk_t;
enum { W_FIND = 1, W_IDS, W_COUNT, W_LS, W_LISTALL, W_LISTALLINFO };
static int walk_cb(void *ud, const mdb_row_t *r){
    walk_t *w = ud;
    if(w->o && w->o->n > MPD_OUT_MAX) return 1;
    if(w->mode == W_LS || w->mode == W_LISTALL || w->mode == W_LISTALLINFO){
        if(strncmp(r->path, w->prefix, w->plen)) return 0;
        const char *rest = r->path + w->plen;
        /* Rows come in PATH order, so a directory's songs are contiguous: a level is new unless it is (a parent of)
         * the last directory printed. lsinfo prints only the first level and skips the songs below it. */
        const char *last = strrchr(rest, '/');
        if(last){
            size_t upto = (size_t)((w->mode == W_LS ? strchr(rest, '/') : last) - rest);
            for(size_t i = 1; i <= upto; i++){
                if(i < upto && rest[i] != '/') continue;
                int printed = !strncmp(w->lastdir, rest, i) && (w->lastdir[i] == 0 || w->lastdir[i] == '/');
                if(!printed){ char uri[1200]; snprintf(uri, sizeof uri, "%s%.*s", w->urip, (int)i, rest); mpd_kv(w->o, "directory", uri); }
            }
            snprintf(w->lastdir, sizeof w->lastdir, "%.*s", (int)upto, rest);
            if(w->mode == W_LS) return 0;
        }
        if(w->mode == W_LISTALL){ char uri[600]; if(!to_uri(r->path, r->is_cue || r->is_iso, r->track, uri, sizeof uri)) mpd_kv(w->o, "file", uri); }
        else print_row(w->o, r);
        return 0;
    }
    tagbuf_t tb; row_tags(r, &tb);
    if(!tb.file[0] || (w->f && !mpd_filter_match(w->f, &tb.t))) return 0;
    if(w->mode == W_COUNT){ w->songs++; w->ms += r->dur_ms; return 0; }
    if(w->mode == W_IDS) return idl_add(w->ids, r->id);
    long k = w->k++;
    if(k < w->start || (w->end >= 0 && k >= w->end)) return 0;
    print_row(w->o, r);
    return 0;
}
/* lsinfo / listall / listallinfo of a directory URI (or a file URI: that song) */
static int list_dir(char *uri_in, int mode, rmt_buf_t *o, const char **msg){
    const char *uri = strip_slashes(uri_in);
    if(uri[0] && !uri_ok(uri)){ *msg = "No such directory"; return MPD_ACK_NO_EXIST; }
    char p[580]; abs_of(uri, p, sizeof p);
    if(uri[0]){                                          /* a file: that song */
        one_t x = { p, 0, o };
        if(mdb_rows(MDB_ROWS_PATH, p, 0, one_cb, &x) < 0){ *msg = "Database error"; return MPD_ACK_SYSTEM; }
        if(x.found) return 0;
        const char *sl = strrchr(p, '/'); int tr = 0; char extra;    /* a CUE/ISO sub-track */
        if(sl && sscanf(sl + 1, "track%d%c", &tr, &extra) == 1 && tr > 0){
            char base[580]; snprintf(base, sizeof base, "%.*s", (int)(sl - p), p);
            sub_t y = { tr, 0, o };
            if(mdb_rows(MDB_ROWS_PATH, base, 0, sub_cb, &y) < 0){ *msg = "Database error"; return MPD_ACK_SYSTEM; }
            if(y.found) return 0;
        }
    }
    walk_t w; memset(&w, 0, sizeof w);
    w.o = o; w.mode = mode;
    snprintf(w.prefix, sizeof w.prefix, "%s/", p); w.plen = strlen(w.prefix);
    if(uri[0]) snprintf(w.urip, sizeof w.urip, "%s/", uri);
    int n = mdb_rows(uri[0] ? MDB_ROWS_UNDER : MDB_ROWS_ALL, p, 0, walk_cb, &w);
    if(n < 0){ *msg = "Database error"; return MPD_ACK_SYSTEM; }
    if(n == 0 && uri[0]){ *msg = "No such directory"; return MPD_ACK_NO_EXIST; }
    return 0;
}
/* trailing "sort TAG" / "window A:B" / "group TAG" options of find & co.: cut off argv */
static int cut_opts(int *argc, char **argv, long *ws, long *we, const char **msg){
    *ws = 0; *we = -1;
    while(*argc >= 2){
        const char *k = argv[*argc - 2];
        if(!strcmp(k, "window")){ if(mpd_parse_range(argv[*argc - 1], ws, we)){ *msg = "Bad window"; return MPD_ACK_ARG; } *argc -= 2; }
        else if(!strcmp(k, "sort") || !strcmp(k, "position")) *argc -= 2;   /* PATH order is kept */
        else break;
    }
    return 0;
}

/* list TAG [filter] [group TAG ...]: distinct values (with their groups) */
typedef struct { char *g[2]; char *v; } lval_t;
typedef struct { int tag, ng, gt[2]; const mpd_filter_t *f; lval_t *a; int n, cap, oom; } lwalk_t;
static int lval_cmp(const void *x, const void *y){
    const lval_t *a = x, *b = y;
    for(int i = 0; i < 2; i++){ int c = strcmp(a->g[i] ? a->g[i] : "", b->g[i] ? b->g[i] : ""); if(c) return c; }
    return strcmp(a->v, b->v);
}
static int list_cb(void *ud, const mdb_row_t *r){
    lwalk_t *w = ud;
    tagbuf_t tb; row_tags(r, &tb);
    if(!tb.file[0] || (w->f && !mpd_filter_match(w->f, &tb.t))) return 0;
    const char *v = tb.t.v[w->tag]; if(!v) v = "";
    if(w->n == w->cap){ int nc = w->cap ? w->cap * 2 : 256; lval_t *p = realloc(w->a, (size_t)nc * sizeof *p); if(!p){ w->oom = 1; return 1; } w->a = p; w->cap = nc; }
    lval_t *e = &w->a[w->n]; memset(e, 0, sizeof *e);
    e->v = strdup(v);
    for(int i = 0; i < w->ng; i++){ const char *g = tb.t.v[w->gt[i]]; e->g[i] = strdup(g ? g : ""); }
    if(!e->v){ w->oom = 1; return 1; }
    w->n++;
    return 0;
}
static int cmd_list(int argc, char **argv, rmt_buf_t *o, const char **msg){
    if(argc < 2){ *msg = "too few arguments for \"list\""; return MPD_ACK_ARG; }
    lwalk_t w; memset(&w, 0, sizeof w);
    w.tag = mpd_tag_from_name(argv[1]);
    if(w.tag < 0 || w.tag == MPD_TAG_ANY || w.tag == MPD_TAG_BASE){ *msg = "Unknown tag type"; return MPD_ACK_ARG; }
    int n = argc;
    while(n >= 4 && !strcmp(argv[n - 2], "group")){
        if(w.ng == 2){ *msg = "Too many groups"; return MPD_ACK_ARG; }
        int g = mpd_tag_from_name(argv[n - 1]);
        if(g < 0 || g == MPD_TAG_ANY || g == MPD_TAG_BASE){ *msg = "Unknown tag type"; return MPD_ACK_ARG; }
        w.gt[w.ng++] = g; n -= 2;
    }
    if(w.ng == 2){ int t = w.gt[0]; w.gt[0] = w.gt[1]; w.gt[1] = t; }   /* outermost group first */
    mpd_filter_t f; const char *err;
    if(n == 3 && w.tag == MPD_TAG_ALBUM && argv[2][0] != '('){         /* old form: list album ARTIST */
        char *fa[2] = { (char *)"artist", argv[2] };
        if(mpd_filter_parse(&f, 2, fa, 0, &err)){ *msg = err; return MPD_ACK_ARG; }
        w.f = &f;
    } else if(n > 2){
        if(mpd_filter_parse(&f, n - 2, argv + 2, 0, &err)){ *msg = err; return MPD_ACK_ARG; }
        w.f = &f;
    }
    int rc = mdb_rows(MDB_ROWS_ALL, NULL, 0, list_cb, &w);
    int ack = 0;
    if(rc < 0){ *msg = "Database error"; ack = MPD_ACK_SYSTEM; }
    else if(w.oom){ *msg = "Out of memory"; ack = MPD_ACK_SYSTEM; }
    else {
        if(w.n > 1) qsort(w.a, (size_t)w.n, sizeof *w.a, lval_cmp);
        const char *lastg[2] = { NULL, NULL }; const char *lastv = NULL;
        for(int i = 0; i < w.n; i++){
            int gchanged = 0;
            for(int g = 0; g < w.ng; g++){
                if(gchanged || !lastg[g] || strcmp(lastg[g], w.a[i].g[g])){ mpd_kv(o, mpd_tag_name(w.gt[g]), w.a[i].g[g]); lastg[g] = w.a[i].g[g]; gchanged = 1; }
            }
            if(!gchanged && lastv && !strcmp(lastv, w.a[i].v)) continue;
            if(w.a[i].v[0] || w.ng) mpd_kv(o, mpd_tag_name(w.tag), w.a[i].v);
            lastv = w.a[i].v;
        }
    }
    for(int i = 0; i < w.n; i++){ free(w.a[i].v); free(w.a[i].g[0]); free(w.a[i].g[1]); }
    free(w.a);
    return ack;
}

/* ---------------------------------------------------------------- stored playlists (the Disc's playlists) */
static long playlist_by_name(const char *name){
    int n = mdb_playlist_num(); if(n <= 0) return 0;
    char (*names)[MDB_STR] = calloc((size_t)n + 1, MDB_STR); long *ids = calloc((size_t)n + 1, sizeof *ids);
    long id = 0;
    if(names && ids){
        int got = mdb_playlists(names, ids, n);
        for(int i = 0; i < got && i < n; i++) if(!strcmp(names[i], name)){ id = ids[i]; break; }
    }
    free(names); free(ids);
    return id;
}
static int playlist_ids(long pid, idlist_t *l){
    memset(l, 0, sizeof *l);
    int n = mdb_playlist_count(pid); if(n < 0) return -1;
    if(n == 0) return 0;
    l->ids = calloc((size_t)n, sizeof *l->ids); if(!l->ids) return -1;
    l->cap = n;
    int got = mdb_playlist_song_ids(pid, l->ids, n);
    int k = 0; for(int i = 0; i < got && i < n; i++) if(l->ids[i] > 0) l->ids[k++] = l->ids[i];   /* 0: not in the library */
    l->n = k;
    return 0;
}
typedef struct { rmt_buf_t *o; int full; } plp_t;
static int plp_cb(void *ud, const mdb_row_t *r){
    plp_t *x = ud;
    if(x->full) print_row(x->o, r);
    else { char uri[600]; if(!to_uri(r->path, r->is_cue || r->is_iso, r->track, uri, sizeof uri)) mpd_kv(x->o, "file", uri); }
    return 1;
}

/* ---------------------------------------------------------------- adding */
static int add_ids(idlist_t *l, const char **msg){
    if(l->oom){ *msg = "Out of memory"; return MPD_ACK_SYSTEM; }
    if(l->n == 0){ *msg = "No such song"; return MPD_ACK_NO_EXIST; }
    rmt_state_t s; remote_state(&s);
    if(!g_st.on && !s.has_track && queue_count() == 0) stage_clear(1);   /* nothing loaded: a new list */
    if(g_st.on){
        for(int i = 0; i < l->n; i++){
            if(g_st.n >= STAGE_MAX){ *msg = "The list is full"; return MPD_ACK_PLAYLIST_MAX; }
            if(mdb_rows(MDB_ROWS_ID, NULL, l->ids[i], stage_cb, NULL) < 0){ *msg = "Database error"; return MPD_ACK_SYSTEM; }
        }
        g_st.gen++; view_invalidate();
        return 0;
    }
    if(!ui_local_playback_allowed()){ *msg = NOT_LOCAL; return MPD_ACK_SYSTEM; }
    int r = queue_add_ids(l->ids, l->n);
    view_invalidate();
    if(r == -1){ *msg = "The Queue is full (400 songs)"; return MPD_ACK_PLAYLIST_MAX; }
    if(r == -2){ *msg = "The library is busy - try again"; return MPD_ACK_SYSTEM; }
    if(r == 0){ *msg = "Nothing could be added"; return MPD_ACK_NO_EXIST; }
    return 0;
}

/* ---------------------------------------------------------------- commands */
static const char *const COMMANDS[] = { "add", "addid", "clear", "commands", "count", "currentsong", "decoders", "delete", "deleteid",
    "disableoutput", "enableoutput", "find", "findadd", "getvol", "list", "listall", "listallinfo", "listplaylist", "listplaylistinfo",
    "listplaylists", "load", "lsinfo", "move", "moveid", "next", "notcommands", "outputs", "pause", "play", "playid", "playlist",
    "playlistid", "playlistinfo", "plchanges", "plchangesposid", "previous", "random", "repeat", "replay_gain_status", "rescan",
    "rm", "rename", "save", "search", "searchadd", "searchcount", "seek", "seekcur", "seekid", "setvol", "shuffle", "single", "stats",
    "status", "stop", "toggleoutput", "update", "urlhandlers", "volume", "consume", "crossfade", NULL };
static int argn(int argc, int lo, int hi, const char *cmd, const char **msg){
    if(argc - 1 < lo || argc - 1 > hi){ snprintf(g_msg, sizeof g_msg, "wrong number of arguments for \"%s\"", cmd); *msg = g_msg; return MPD_ACK_ARG; }
    return 0;
}
#define ARGS(lo, hi) do { int _e = argn(argc, lo, hi, c, msg); if(_e) return _e; } while(0)
static int bad_arg(const char **msg, const char *what){ snprintf(g_msg, sizeof g_msg, "%s", what); *msg = g_msg; return MPD_ACK_ARG; }

static int cmd_status(rmt_buf_t *o, const rmt_state_t *s){
    view_get(s, 0);
    int random, repeat, single; mpd_flags_from_mode(s->play_mode < 0 ? 0 : s->play_mode, &random, &repeat, &single);
    rmt_buf_printf(o, "volume: %d\nrepeat: %d\nrandom: %d\nsingle: %d\nconsume: 0\n", mpd_vol_to_mpd(s->volume, s->volume_max), repeat, random, single);
    rmt_buf_printf(o, "playlist: %u\nplaylistlength: %d\nmixrampdb: 0.000000\n", g_plver, g_nview);
    int cp = cur_pos();
    const char *state = cp < 0 ? "stop" : s->playing ? "play" : "pause";
    rmt_buf_printf(o, "state: %s\n", state);
    if(cp >= 0){
        long el = s->position_ms < 0 ? 0 : s->position_ms, du = s->duration_ms > 0 ? s->duration_ms : g_view[cp].dur_ms;
        rmt_buf_printf(o, "song: %d\nsongid: %d\n", cp, g_view[cp].id);
        rmt_buf_printf(o, "time: %ld:%ld\nelapsed: %ld.%03ld\n", (el + 500) / 1000, (du + 500) / 1000, el / 1000, el % 1000);
        if(du > 0) rmt_buf_printf(o, "duration: %ld.%03ld\n", du / 1000, du % 1000);
        if(s->kbps > 0) rmt_buf_printf(o, "bitrate: %d\n", s->kbps);
        if(s->sample_rate > 0){
            char bits[12]; if(s->bits > 0) snprintf(bits, sizeof bits, "%d", s->bits); else snprintf(bits, sizeof bits, "*");
            rmt_buf_printf(o, "audio: %d:%s:%d\n", s->sample_rate, bits, s->channels > 0 ? s->channels : 2);
        }
        if(cp + 1 < g_nview) rmt_buf_printf(o, "nextsong: %d\nnextsongid: %d\n", cp + 1, g_view[cp + 1].id);
    } else if(g_nview > 0) rmt_buf_printf(o, "nextsong: 0\nnextsongid: %d\n", g_view[0].id);
    if(scanner_active()) rmt_buf_put(o, "updating_db: 1\n");
    if(s->radio_state && !strcmp(s->radio_state, "error") && s->radio_error[0]) mpd_kv(o, "error", s->radio_error);
    return 0;
}

static int handler(void *ud, int argc, char **argv, rmt_buf_t *o, const char **msg, const mpd_sess_t *sess){
    (void)ud; (void)sess;
    const char *c = argv[0];
    rmt_state_t s;
    /* ---- status */
    if(!strcmp(c, "status")){ ARGS(0, 0); remote_state(&s); return cmd_status(o, &s); }
    if(!strcmp(c, "currentsong")){
        ARGS(0, 0); remote_state(&s); view_get(&s, 0);
        int cp = cur_pos(); if(cp >= 0) print_entry(o, cp);
        return 0;
    }
    if(!strcmp(c, "stats")){
        ARGS(0, 0);
        long long ms = 0; int n = mdb_song_count();
        for(int i = 0; i < n; i++){ const mdb_song_t *x = mdb_song(i); if(x && x->dur_ms > 0) ms += x->dur_ms; }
        rmt_buf_printf(o, "artists: %d\nalbums: %d\nsongs: %d\nuptime: %ld\ndb_playtime: %lld\ndb_update: 0\nplaytime: 0\n",
                       mdb_artist_count(), mdb_album_count(), n < 0 ? 0 : n, (long)(time(NULL) - g_started), ms / 1000);
        return 0;
    }
    if(!strcmp(c, "commands") || !strcmp(c, "notcommands")){
        if(!strcmp(c, "notcommands")) return 0;
        for(int i = 0; COMMANDS[i]; i++) mpd_kv(o, "command", COMMANDS[i]);
        for(int i = 0; MPD_SESSION_COMMANDS[i]; i++) mpd_kv(o, "command", MPD_SESSION_COMMANDS[i]);
        return 0;
    }
    if(!strcmp(c, "urlhandlers")) return 0;
    if(!strcmp(c, "decoders")){
        static const char *const SUF[] = { "flac", "mp3", "wav", "aiff", "ape", "m4a", "aac", "ogg", "opus", "wma", "dsf", "dff", "iso", "cue", NULL };
        mpd_kv(o, "plugin", "disc");
        for(int i = 0; SUF[i]; i++) mpd_kv(o, "suffix", SUF[i]);
        return 0;
    }
    if(!strcmp(c, "outputs")){ rmt_buf_put(o, "outputid: 0\noutputname: Disc\nplugin: disc\noutputenabled: 1\n"); return 0; }
    if(!strcmp(c, "enableoutput") || !strcmp(c, "disableoutput") || !strcmp(c, "toggleoutput")){
        ARGS(1, 1);
        if(strcmp(argv[1], "0")){ *msg = "No such audio output"; return MPD_ACK_NO_EXIST; }
        if(c[0] != 'e'){ *msg = "The Disc's output can't be turned off"; return MPD_ACK_SYSTEM; }
        return 0;
    }
    if(!strcmp(c, "replay_gain_status")){ rmt_buf_put(o, "replay_gain_mode: off\n"); return 0; }
    /* ---- options */
    if(!strcmp(c, "random") || !strcmp(c, "repeat") || !strcmp(c, "single")){
        ARGS(1, 1);
        int v;
        if(!strcmp(c, "single") && !strcmp(argv[1], "oneshot")){ *msg = "single oneshot is not supported"; return MPD_ACK_ARG; }
        if(mpd_parse_bool(argv[1], &v)) return bad_arg(msg, "Boolean (0/1) expected");
        remote_state(&s);
        if(s.play_mode < 0){ *msg = "The play mode is not known (Music Assistant or another mode plays)"; return MPD_ACK_SYSTEM; }
        int random, repeat, single; mpd_flags_from_mode(s.play_mode, &random, &repeat, &single);
        if(c[1] == 'a') random = v; else if(c[1] == 'e') repeat = v; else single = v;
        if(c[1] != 'a' && v && random){ *msg = "The Disc can't combine shuffle with repeat or single - turn random off first"; return MPD_ACK_SYSTEM; }
        int mode = mpd_mode_from_flags(random, c[1] == 'a' && v ? 0 : repeat, c[1] == 'a' && v ? 0 : single);
        if(mode < 0){ *msg = "The Disc has no such play mode"; return MPD_ACK_SYSTEM; }
        if(mode == s.play_mode) return 0;
        return rsimple(RMT_CMD_PLAY_MODE, mode, msg);
    }
    if(!strcmp(c, "consume")){ ARGS(1, 1); if(!strcmp(argv[1], "0")) return 0; *msg = "consume is not supported"; return MPD_ACK_SYSTEM; }
    if(!strcmp(c, "crossfade")){ ARGS(1, 1); if(!strcmp(argv[1], "0")) return 0; *msg = "crossfade is not supported"; return MPD_ACK_SYSTEM; }
    /* ---- volume */
    if(!strcmp(c, "getvol")){
        ARGS(0, 0); remote_state(&s);
        int v = mpd_vol_to_mpd(s.volume, s.volume_max); if(v >= 0) rmt_buf_printf(o, "volume: %d\n", v);
        return 0;
    }
    if(!strcmp(c, "setvol")){
        ARGS(1, 1); long v; if(mpd_parse_int(argv[1], 0, 100, &v)) return bad_arg(msg, "Invalid volume value");
        return rsimple(RMT_CMD_VOLUME, mpd_vol_from_mpd((int)v, VOL_MAX), msg);
    }
    if(!strcmp(c, "volume")){
        ARGS(1, 1); long d; if(mpd_parse_int(argv[1], -100, 100, &d)) return bad_arg(msg, "Invalid volume value");
        remote_state(&s);
        int cur = mpd_vol_to_mpd(s.volume, s.volume_max);
        if(cur < 0){ *msg = "The volume is not known yet - set it once"; return MPD_ACK_SYSTEM; }
        int t = cur + (int)d; if(t < 0) t = 0; if(t > 100) t = 100;
        return rsimple(RMT_CMD_VOLUME, mpd_vol_from_mpd(t, VOL_MAX), msg);
    }
    /* ---- transport */
    if(!strcmp(c, "pause")){
        ARGS(0, 1);
        if(argc == 1) return rsimple(RMT_CMD_PLAY_PAUSE, 0, msg);
        int v; if(mpd_parse_bool(argv[1], &v)) return bad_arg(msg, "Boolean (0/1) expected");
        return rsimple(v ? RMT_CMD_PAUSE : RMT_CMD_PLAY, 0, msg);
    }
    if(!strcmp(c, "stop")){ ARGS(0, 0); if(g_st.on) return 0; return rsimple(RMT_CMD_PAUSE, 0, msg); }   /* no stop on the Disc */
    if(!strcmp(c, "next")){ ARGS(0, 0); view_invalidate(); return rsimple(RMT_CMD_NEXT, 0, msg); }
    if(!strcmp(c, "previous")){ ARGS(0, 0); view_invalidate(); return rsimple(RMT_CMD_PREV, 0, msg); }
    if(!strcmp(c, "play") || !strcmp(c, "playid")){
        ARGS(0, 1);
        remote_state(&s); view_get(&s, 1);
        if(argc == 1 || !strcmp(argv[1], "-1")){
            if(g_st.on){ if(!g_st.n) return 0; return play_staged(0, msg); }
            if(cur_pos() >= 0 || queue_count() > 0) return rsimple(RMT_CMD_PLAY, 0, msg);
            return 0;                                        /* nothing to play, as MPD */
        }
        long v; if(mpd_parse_int(argv[1], 0, 1L << 30, &v)) return bad_arg(msg, "Integer expected");
        int pos = c[4] == 'i' ? pos_of_id(v) : (int)v;
        if(pos < 0 || pos >= g_nview){ *msg = c[4] == 'i' ? "No such song" : "Bad song index"; return c[4] == 'i' ? MPD_ACK_NO_EXIST : MPD_ACK_ARG; }
        return play_pos(pos, msg);
    }
    if(!strcmp(c, "seek") || !strcmp(c, "seekid") || !strcmp(c, "seekcur")){
        int cur = !strcmp(c, "seekcur");
        ARGS(cur ? 1 : 2, cur ? 1 : 2);
        long ms; int rel = 0;
        if(mpd_parse_time(argv[cur ? 1 : 2], &ms, cur ? &rel : NULL)) return bad_arg(msg, "Bad time");
        remote_state(&s); view_get(&s, 0);
        int cp = cur_pos();
        if(cp < 0){ *msg = "Not playing"; return MPD_ACK_PLAYER_SYNC; }
        if(!cur){
            long v; if(mpd_parse_int(argv[1], 0, 1L << 30, &v)) return bad_arg(msg, "Integer expected");
            int pos = c[4] == 'i' ? pos_of_id(v) : (int)v;
            if(pos != cp){ *msg = "Only the song playing now can be sought (play it first)"; return MPD_ACK_SYSTEM; }
        }
        if(rel) ms = s.position_ms + rel * ms;
        if(ms < 0) ms = 0;
        if(s.duration_ms > 0 && ms > s.duration_ms) ms = s.duration_ms;
        return rsimple(RMT_CMD_SEEK, ms, msg);
    }
    /* ---- the queue */
    if(!strcmp(c, "playlistinfo") || !strcmp(c, "playlistid") || !strcmp(c, "plchanges") || !strcmp(c, "plchangesposid") || !strcmp(c, "playlist")){
        remote_state(&s); view_get(&s, 0);
        long a = 0, b = -1;
        if(!strcmp(c, "plchanges") || !strcmp(c, "plchangesposid")){
            ARGS(1, 2);
            /* the version is unsigned 32 bit: 0xFFFFFFFF does not fit a 32-bit long (MIPS), so parse it unsigned */
            char *end; errno = 0; unsigned long long ver = strtoull(argv[1], &end, 10);
            if(!argv[1][0] || argv[1][0] == '-' || *end || errno || ver > 0xFFFFFFFFULL) return bad_arg(msg, "Integer expected");
            if(argc == 3 && mpd_parse_range(argv[2], &a, &b)) return bad_arg(msg, "Bad range");
            if((unsigned)ver == g_plver) return 0;           /* nothing changed since */
            if(c[9] == 'p'){ for(int i = (int)a; i < g_nview && (b < 0 || i < b); i++) rmt_buf_printf(o, "cpos: %d\nId: %d\n", i, g_view[i].id); return 0; }
        } else if(!strcmp(c, "playlistid")){
            ARGS(0, 1);
            if(argc == 2){ long id; if(mpd_parse_int(argv[1], 0, 1L << 30, &id)) return bad_arg(msg, "Integer expected");
                int p = pos_of_id(id); if(p < 0){ *msg = "No such song"; return MPD_ACK_NO_EXIST; } a = p; b = p + 1; }
        } else if(!strcmp(c, "playlist")){
            ARGS(0, 0);
            for(int i = 0; i < g_nview; i++){ char uri[600]; if(to_uri(g_view[i].path, 0, 0, uri, sizeof uri)) snprintf(uri, sizeof uri, "%s", g_view[i].path); rmt_buf_printf(o, "%d:", i); mpd_kv(o, "file", uri); }
            return 0;
        } else {
            ARGS(0, 1);
            if(argc == 2 && mpd_parse_range(argv[1], &a, &b)) return bad_arg(msg, "Bad range");
            if(argc == 2 && a >= g_nview && !(a == 0 && g_nview == 0)){ *msg = "Bad song index"; return MPD_ACK_ARG; }
        }
        for(int i = (int)a; i < g_nview && (b < 0 || i < b); i++) print_entry(o, i);
        return 0;
    }
    if(!strcmp(c, "add") || !strcmp(c, "addid")){
        ARGS(1, 2);
        if(argc == 3){ *msg = "Adding at a position is not supported: songs play after the current one"; return MPD_ACK_ARG; }
        if(strstr(argv[1], "://")){ *msg = "Streams can't be added (use the Radio)"; return MPD_ACK_NO_EXIST; }
        idlist_t l;
        if(uri_ids(argv[1], &l) < 0){ free(l.ids); *msg = "Database error"; return MPD_ACK_SYSTEM; }
        int staged_before = g_st.on, qn_before = queue_count(), sn_before = g_st.n;
        int r = add_ids(&l, msg);
        free(l.ids);
        if(r) return r;
        if(c[3]){                                            /* addid: the new song's id */
            if(g_st.on) rmt_buf_printf(o, "Id: %d\n", ID_STAGED + (staged_before ? sn_before : 0));
            else rmt_buf_printf(o, "Id: %d\n", ID_QUEUE + qn_before);
        }
        return 0;
    }
    if(!strcmp(c, "clear")){
        ARGS(0, 0);
        if(queue_count() > 0) queue_clear();
        remote_state(&s);
        if(s.playing && !strcmp(s.source, "local")){ int r = rsimple(RMT_CMD_PAUSE, 0, msg); if(r) return r; }
        stage_clear(1); view_invalidate();
        return 0;
    }
    if(!strcmp(c, "delete") || !strcmp(c, "deleteid")){
        ARGS(1, 1);
        remote_state(&s); view_get(&s, 1);
        long a, b;
        if(c[6] == 'i'){ long id; if(mpd_parse_int(argv[1], 0, 1L << 30, &id)) return bad_arg(msg, "Integer expected");
            int p = pos_of_id(id); if(p < 0){ *msg = "No such song"; return MPD_ACK_NO_EXIST; } a = p; b = p + 1; }
        else if(mpd_parse_range(argv[1], &a, &b)) return bad_arg(msg, "Bad song index");
        if(b < 0) b = g_nview;
        if(a >= g_nview || b > g_nview){ *msg = "Bad song index"; return MPD_ACK_ARG; }
        for(long i = a; i < b; i++) if(g_view[i].kind != V_QUEUE && g_view[i].kind != V_STAGED){
            *msg = g_view[i].kind == V_CUR ? "The song playing now can't be removed" : "Only songs added from an MPD client or the Disc's Queue can be removed";
            return MPD_ACK_SYSTEM;
        }
        for(long i = b - 1; i >= a; i--){                    /* from the end: indexes stay right */
            if(g_view[i].kind == V_QUEUE) queue_remove_at(g_view[i].idx);
            else { int k = g_view[i].idx; memmove(&g_st.it[k], &g_st.it[k + 1], (size_t)(g_st.n - k - 1) * sizeof g_st.it[0]); g_st.n--; g_st.gen++; }
        }
        view_invalidate();
        return 0;
    }
    if(!strcmp(c, "move") || !strcmp(c, "moveid")){
        ARGS(2, 2);
        remote_state(&s); view_get(&s, 1);
        long from, to;
        if(c[4] == 'i'){ long id; if(mpd_parse_int(argv[1], 0, 1L << 30, &id)) return bad_arg(msg, "Integer expected");
            from = pos_of_id(id); if(from < 0){ *msg = "No such song"; return MPD_ACK_NO_EXIST; } }
        else { long e; if(mpd_parse_range(argv[1], &from, &e) || e != from + 1){ *msg = "Moving ranges is not supported"; return MPD_ACK_ARG; } }
        if(mpd_parse_int(argv[2], 0, 1L << 30, &to)) return bad_arg(msg, "Integer expected");
        if(from >= g_nview || to >= g_nview){ *msg = "Bad song index"; return MPD_ACK_ARG; }
        const ventry_t *f = &g_view[from], *t = &g_view[to];
        if(f->kind != t->kind || (f->kind != V_QUEUE && f->kind != V_STAGED)){ *msg = "Only songs added from an MPD client or the Disc's Queue can be moved, and only among themselves"; return MPD_ACK_SYSTEM; }
        if(f->kind == V_QUEUE){ if(f->idx != t->idx) queue_move(f->idx, t->idx); }
        else if(f->idx != t->idx){
            sitem_t tmp = g_st.it[f->idx];
            if(f->idx < t->idx) memmove(&g_st.it[f->idx], &g_st.it[f->idx + 1], (size_t)(t->idx - f->idx) * sizeof tmp);
            else memmove(&g_st.it[t->idx + 1], &g_st.it[t->idx], (size_t)(f->idx - t->idx) * sizeof tmp);
            g_st.it[t->idx] = tmp; g_st.gen++;
        }
        view_invalidate();
        return 0;
    }
    if(!strcmp(c, "shuffle")){
        ARGS(0, 1);
        if(g_st.on){ for(int i = g_st.n - 1; i > 0; i--){ int j = rand() % (i + 1); sitem_t t = g_st.it[i]; g_st.it[i] = g_st.it[j]; g_st.it[j] = t; } g_st.gen++; }
        else queue_shuffle();
        view_invalidate();
        return 0;
    }
    /* ---- the library */
    if(!strcmp(c, "lsinfo") || !strcmp(c, "listall") || !strcmp(c, "listallinfo")){
        ARGS(0, 1);
        char empty[1] = "";
        return list_dir(argc == 2 ? argv[1] : empty, c[1] == 's' ? W_LS : c[7] ? W_LISTALLINFO : W_LISTALL, o, msg);
    }
    if(!strcmp(c, "find") || !strcmp(c, "search") || !strcmp(c, "findadd") || !strcmp(c, "searchadd") || !strcmp(c, "count") || !strcmp(c, "searchcount")){
        int fold = c[0] == 's', add = strstr(c, "add") != NULL, count = strstr(c, "count") != NULL;
        long ws, we; int n = argc - 1;
        if(count){                                           /* count FILTER [group TAG] */
            int gt = -1;
            if(n >= 3 && !strcmp(argv[argc - 2], "group")){ gt = mpd_tag_from_name(argv[argc - 1]); if(gt < 0){ *msg = "Unknown tag type"; return MPD_ACK_ARG; } n -= 2; }
            if(gt >= 0){ *msg = "count with group is not supported"; return MPD_ACK_ARG; }
        } else { int r = cut_opts(&n, argv + 1, &ws, &we, msg); if(r) return r; }
        mpd_filter_t f; const char *err;
        if(mpd_filter_parse(&f, n, argv + 1, fold, &err)) return bad_arg(msg, err);
        walk_t w; memset(&w, 0, sizeof w);
        w.o = o; w.f = &f;
        idlist_t l; memset(&l, 0, sizeof l);
        if(add){ w.mode = W_IDS; w.ids = &l; }
        else if(count) w.mode = W_COUNT;
        else { w.mode = W_FIND; w.start = ws; w.end = we; }
        if(mdb_rows(MDB_ROWS_ALL, NULL, 0, walk_cb, &w) < 0){ free(l.ids); *msg = "Database error"; return MPD_ACK_SYSTEM; }
        if(count){ rmt_buf_printf(o, "songs: %lld\nplaytime: %lld\n", w.songs, w.ms / 1000); return 0; }
        if(add){ int r = l.n ? add_ids(&l, msg) : 0; free(l.ids); return r; }
        return 0;
    }
    if(!strcmp(c, "list")) return cmd_list(argc, argv, o, msg);
    if(!strcmp(c, "update") || !strcmp(c, "rescan")){
        ARGS(0, 1);
        if(!scanner_active()){ int r = rsimple(RMT_CMD_RESCAN, 1, msg); if(r) return r; msrv_events(MPD_EV_UPDATE); }
        rmt_buf_put(o, "updating_db: 1\n");
        return 0;
    }
    /* ---- stored playlists */
    if(!strcmp(c, "listplaylists")){
        ARGS(0, 0);
        int n = mdb_playlist_num(); if(n <= 0) return 0;
        char (*names)[MDB_STR] = calloc((size_t)n + 1, MDB_STR); long *ids = calloc((size_t)n + 1, sizeof *ids);
        if(!names || !ids){ free(names); free(ids); *msg = "Out of memory"; return MPD_ACK_SYSTEM; }
        int got = mdb_playlists(names, ids, n);
        for(int i = 0; i < got && i < n; i++){ mpd_kv(o, "playlist", names[i]); rmt_buf_put(o, "Last-Modified: 1970-01-01T00:00:00Z\n"); }
        free(names); free(ids);
        return 0;
    }
    if(!strcmp(c, "listplaylist") || !strcmp(c, "listplaylistinfo") || !strcmp(c, "load")){
        ARGS(1, 2);
        long pid = playlist_by_name(argv[1]);
        if(!pid){ *msg = "No such playlist"; return MPD_ACK_NO_EXIST; }
        idlist_t l; if(playlist_ids(pid, &l)){ free(l.ids); *msg = "Couldn't read the playlist"; return MPD_ACK_SYSTEM; }
        long a = 0, b = -1;
        if(argc == 3 && mpd_parse_range(argv[2], &a, &b)){ free(l.ids); return bad_arg(msg, "Bad range"); }
        if(b < 0 || b > l.n) b = l.n;
        if(a > b) a = b;
        if(c[0] == 'l' && c[1] == 'o'){                      /* load */
            idlist_t part = { l.ids + a, (int)(b - a), 0, 0, 0 };
            int r = part.n ? add_ids(&part, msg) : 0;
            free(l.ids); return r;
        }
        plp_t x = { o, c[12] == 'i' };
        for(long i = a; i < b; i++) mdb_rows(MDB_ROWS_ID, NULL, l.ids[i], plp_cb, &x);
        free(l.ids);
        return 0;
    }
    if(!strcmp(c, "save")){
        ARGS(1, 2);
        if(argc == 3 && strcmp(argv[2], "create")){ *msg = "Only save NAME (create) is supported"; return MPD_ACK_ARG; }
        if(!argv[1][0] || strlen(argv[1]) >= RMT_PL_NAME_MAX){ *msg = "Bad playlist name"; return MPD_ACK_ARG; }
        if(playlist_by_name(argv[1])){ *msg = "Playlist already exists"; return MPD_ACK_EXIST; }
        remote_state(&s); view_get(&s, 1);
        idlist_t l; memset(&l, 0, sizeof l);
        for(int i = 0; i < g_nview; i++){
            if(g_view[i].kind == V_OTHER) continue;
            int id = g_view[i].kind == V_STAGED ? g_st.it[g_view[i].idx].id : mdb_song_id_by_path(g_view[i].path);
            if(id > 0 && idl_add(&l, id)) break;
        }
        if(l.oom){ free(l.ids); *msg = "Out of memory"; return MPD_ACK_SYSTEM; }
        long pid = mdb_playlist_create(argv[1]);
        if(pid <= 0){ free(l.ids); *msg = "Couldn't create the playlist"; return MPD_ACK_SYSTEM; }
        int r = l.n ? mdb_playlist_add_ids(pid, l.ids, l.n) : 0;
        free(l.ids);
        library_refresh();
        msrv_events(MPD_EV_STORED_PLAYLIST);
        if(r < 0){ *msg = "Couldn't write the playlist"; return MPD_ACK_SYSTEM; }
        return 0;
    }
    if(!strcmp(c, "rm") || !strcmp(c, "rename")){
        ARGS(c[1] == 'm' ? 1 : 2, c[1] == 'm' ? 1 : 2);
        long pid = playlist_by_name(argv[1]);
        if(!pid){ *msg = "No such playlist"; return MPD_ACK_NO_EXIST; }
        int ok;
        if(c[1] == 'm') ok = mdb_playlist_delete(pid);
        else {
            if(!argv[2][0] || strlen(argv[2]) >= RMT_PL_NAME_MAX){ *msg = "Bad playlist name"; return MPD_ACK_ARG; }
            if(playlist_by_name(argv[2])){ *msg = "Playlist already exists"; return MPD_ACK_EXIST; }
            ok = mdb_playlist_rename(pid, argv[2]);
        }
        if(!ok){ *msg = "Couldn't change the playlist"; return MPD_ACK_SYSTEM; }
        library_refresh();
        msrv_events(MPD_EV_STORED_PLAYLIST);
        return 0;
    }
    snprintf(g_msg, sizeof g_msg, "unknown command \"%.60s\"", c);
    *msg = g_msg;
    return MPD_ACK_UNKNOWN;
}

/* ---------------------------------------------------------------- the main thread's pump: events and commands */
static struct { int have, playing, volume, mode, scanning; unsigned path_seq; int pos_id; long pos; uint32_t at;
                char radio[16], station[96], stream[330]; } g_last;
static void detect(void){
    rmt_state_t s; remote_state(&s);
    unsigned ev = g_ev_pending; g_ev_pending = 0;
    track_state_t st; ipc_get_state(&st);
    const char *rs = s.radio_state ? s.radio_state : "off";
    if(g_last.at){
        if(s.has_track != g_last.have || s.playing != g_last.playing || st.path_seq != g_last.path_seq || st.pos_id != g_last.pos_id
           || strcmp(rs, g_last.radio) || strcmp(s.station, g_last.station) || strcmp(s.stream_title, g_last.stream)) ev |= MPD_EV_PLAYER;
        else if(s.has_track){                            /* a seek: the position moved more than playing explains */
            long expect = g_last.pos + (s.playing ? (long)lv_tick_elaps(g_last.at) : 0);
            if(labs(s.position_ms - expect) > 3000) ev |= MPD_EV_PLAYER;
        }
        if(s.volume != g_last.volume) ev |= MPD_EV_MIXER;
        if(s.play_mode != g_last.mode) ev |= MPD_EV_OPTIONS;
        int scanning = scanner_active();
        if(scanning != g_last.scanning){ ev |= MPD_EV_UPDATE; if(!scanning) ev |= MPD_EV_DATABASE; }
    }
    /* a new list waits for play: anything the Disc starts itself ends it */
    if(g_st.on && s.playing && st.path_seq != g_st.path_seq){ stage_clear(0); view_invalidate(); }
    g_last.have = s.has_track; g_last.playing = s.playing; g_last.path_seq = st.path_seq; g_last.pos_id = st.pos_id;
    g_last.pos = s.position_ms; g_last.at = lv_tick_get() | 1; g_last.volume = s.volume; g_last.mode = s.play_mode;
    g_last.scanning = scanner_active();
    snprintf(g_last.radio, sizeof g_last.radio, "%s", rs); snprintf(g_last.station, sizeof g_last.station, "%s", s.station);
    snprintf(g_last.stream, sizeof g_last.stream, "%s", s.stream_title);
    view_get(&s, 0);                                     /* the playlist version (an Up Next change shows up here) */
    ev |= g_ev_pending; g_ev_pending = 0;
    if(ev) msrv_events(ev);
}
void mpd_pump(void){
    if(!msrv_running()) return;
    static uint32_t last;
    if(msrv_clients() > 0 && (!last || lv_tick_elaps(last) >= 500)){ detect(); last = lv_tick_get() | 1; }
    if(msrv_pump()) last = 0;                             /* a command ran: look for its effects at once */
}

/* ---------------------------------------------------------------- Settings: the access overlay */
static lv_obj_t *g_ov;
static void ov_close(void){ if(g_ov){ lv_obj_delete(g_ov); g_ov = NULL; } }
static void ov_close_cb(lv_event_t *e){ (void)e; ov_close(); }
int mpd_access_shown(void){ return g_ov != NULL; }
static lv_obj_t *ov_label(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t col, int y, int w){
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);             /* wraps; never scrolls vertically */
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}
static void show_access(const char *ip, const char *pass){
    ov_close();
    g_ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_ov);
    lv_obj_set_size(g_ov, 360, 360);
    lv_obj_set_style_bg_color(g_ov, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(g_ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_ov, LV_OBJ_FLAG_SCROLLABLE);
    ov_label(g_ov, "MPD Server", TH_F_DETAIL, TC(TEXT_PRIMARY), -120, 220);
    char t[160];
    snprintf(t, sizeof t, "Host\n%s\nPort %d", ip, MPD_PORT);
    ov_label(g_ov, t, TF(UI_16), TC(TEXT_SECONDARY), -54, 280);
    snprintf(t, sizeof t, "Password\n%s", pass);
    ov_label(g_ov, t, TF(UI_16), TC(TEXT_SECONDARY), 22, 280);
    ov_label(g_ov, "mpc: MPD_HOST=password@host", TF(UI_14), TC(TEXT_MUTED), 74, 280);
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
void mpd_show_access(void){
    if(!cfg_get_int(MPD_CFG_ON, 0)){ ui_toast("Turn on MPD Server first"); return; }
    server_sync();
    if(!msrv_running()){ ui_toast("Connect to Wi-Fi first"); return; }
    const char *t = remote_token(); if(!t) return;
    show_access(g_ip, t);
}
void mpd_test_access(void){ show_access("192.168.1.23", "ABCDEFGHIJKLMNOPQRST"); }   /* host renders */

/* ---------------------------------------------------------------- host tests */
char *mpd_test_request(const char *lines){
    mpd_sess_t s; mpd_sess_init(&s, 1);
    int n = 0; for(const char *p = lines; *p; p++) if(*p == '\n') n++;
    rmt_buf_t out = { 0 };
    if(strchr(lines, '\n') && n > 0){ s.list = 2; rmt_buf_put(&s.batch, lines); if(lines[strlen(lines) - 1] != '\n') rmt_buf_put(&s.batch, "\n"); }
    else { rmt_buf_put(&s.batch, lines); rmt_buf_put(&s.batch, "\n"); }
    mpd_run_batch(&s, NULL, handler, NULL, &out);
    mpd_sess_free(&s);
    return rmt_buf_take(&out);
}
/* host tests: serve real clients on 127.0.0.1:port (no password) with the real handler for `seconds` */
void mpd_test_serve(int port, int seconds){
    msrv_set_handler(handler, NULL);
    if(msrv_start("127.0.0.1", port, NULL)){ fprintf(stderr, "mpd: can't listen\n"); return; }
    printf("serving on 127.0.0.1:%d\n", msrv_port()); fflush(stdout);
    for(long i = 0; i < (long)seconds * 200; i++){ mpd_pump(); lv_tick_inc(5); lv_timer_handler(); usleep(5000); }
    msrv_stop();
}
