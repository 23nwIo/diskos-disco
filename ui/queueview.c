/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The play order as one list, for the MPD server and Remote Control (queueview.h). Was mpd.c's view_build. */
#include "queueview.h"
#include "screens.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static qv_view_t g_v;
static mdb_qrow_t *g_rows;
static struct { unsigned path_seq, qgen; int pos_id, mode, have, valid; char src[24], radio[16]; uint32_t at; } g_key;
static unsigned g_sig;

void qv_invalidate(void){ g_key.valid = 0; }

static qv_entry_t *add(int kind){
    if(g_v.n >= QV_CAP) return NULL;
    qv_entry_t *v = &g_v.e[g_v.n++]; memset(v, 0, sizeof *v); v->kind = kind; return v;
}
static unsigned fnv(unsigned h, const void *p, size_t n){ const unsigned char *c = p; while(n--){ h ^= *c++; h *= 16777619u; } return h; }

static void build(const rmt_state_t *s){
    g_v.n = 0; g_v.more = 0; g_v.shuffle = 0;
    const char *src = s->source ? s->source : "other";
    if(strcmp(src, "local")){                            /* web radio, Music Assistant, another mode: the one thing playing */
        int radio = !strcmp(src, "radio");
        if(!s->has_track && !radio) return;
        qv_entry_t *v = add(QV_OTHER);
        v->ord = radio;
        v->dur_ms = radio ? 0 : (int)s->duration_ms;     /* a stream has no length (the player reports a made-up one) */
        snprintf(v->path, sizeof v->path, "%s", s->station_url);
        /* a station: the song the stream names ("Artist - Title") or nothing; the player's own tags are only our
         * station name again (radio.c sets them for the Disc's screens) */
        snprintf(v->title, sizeof v->title, "%.*s", (int)sizeof v->title - 1, radio ? s->stream_title : s->title);
        snprintf(v->artist, sizeof v->artist, "%s", radio ? s->station : s->artist);
        return;
    }
    track_state_t st; ipc_get_state(&st);
    int n = 0, more = 0, mode = ui_play_mode_now();
    g_v.shuffle = mode == 1;
    if(st.have_track && st.path[0] && !mdb_is_book_path(st.path) && mode >= 0)
        n = mdb_upnext(mode == 1, st.pos_id, st.path, g_rows, QV_NEXT_MAX + 1, &more);
    if(n > 0){
        qv_entry_t *v = add(QV_CUR);
        v->base_id = g_rows[0].base_id; v->ord = g_rows[0].ord; v->dur_ms = g_rows[0].dur_ms;
        snprintf(v->path, sizeof v->path, "%s", g_rows[0].path);
        snprintf(v->title, sizeof v->title, "%s", st.title[0] ? st.title : g_rows[0].title);
        snprintf(v->artist, sizeof v->artist, "%s", st.artist[0] ? st.artist : g_rows[0].artist);
    } else if(st.have_track){
        qv_entry_t *v = add(QV_CUR);
        v->base_id = st.pos_id; v->dur_ms = (int)st.duration_ms;
        snprintf(v->path, sizeof v->path, "%s", st.path);
        snprintf(v->title, sizeof v->title, "%s", st.title); snprintf(v->artist, sizeof v->artist, "%s", st.artist);
    }
    int qn = queue_count(), skip = 1, active = queue_active();
    char qpath[256];
    for(int i = 0; i < qn; i++){
        qv_entry_t *v = add(QV_QUEUE); if(!v) break;
        long dur = 0;
        queue_item(i, qpath, sizeof qpath, v->title, sizeof v->title, v->artist, sizeof v->artist, &dur);
        snprintf(v->path, sizeof v->path, "%s", qpath);
        v->idx = i; v->dur_ms = (int)dur;
        /* while the player plays the queue's copy, the queued songs are already in its list right after the current
         * one: show them once, as queue entries */
        if(active && skip < n && !strcmp(g_rows[skip].path, qpath)) skip++;
    }
    for(int i = skip; i < n; i++){
        qv_entry_t *v = add(QV_NEXT); if(!v) break;
        v->base_id = g_rows[i].base_id; v->ord = g_rows[i].ord; v->dur_ms = g_rows[i].dur_ms;
        snprintf(v->path, sizeof v->path, "%s", g_rows[i].path);
        snprintf(v->title, sizeof v->title, "%s", g_rows[i].title); snprintf(v->artist, sizeof v->artist, "%s", g_rows[i].artist);
    }
    g_v.more = more;
}

const qv_view_t *qv_view(const rmt_state_t *s, int fresh){
    if(!g_v.e) g_v.e = calloc(QV_CAP, sizeof *g_v.e);
    if(!g_rows) g_rows = calloc(QV_NEXT_MAX + 1, sizeof *g_rows);
    if(!g_v.e || !g_rows) return NULL;
    track_state_t st; ipc_get_state(&st);
    int mode = ui_play_mode_now();
    const char *rs = s->radio_state ? s->radio_state : "off", *src = s->source ? s->source : "other";
    if(!fresh && g_key.valid && g_key.path_seq == st.path_seq && g_key.pos_id == st.pos_id && g_key.mode == mode
       && g_key.qgen == queue_generation() && g_key.have == s->has_track && !strcmp(g_key.src, src)
       && !strcmp(g_key.radio, rs) && lv_tick_elaps(g_key.at) < 3000) return &g_v;
    build(s);
    g_key.valid = 1; g_key.path_seq = st.path_seq; g_key.pos_id = st.pos_id; g_key.mode = mode;
    g_key.qgen = queue_generation(); g_key.have = s->has_track; g_key.at = lv_tick_get();
    snprintf(g_key.src, sizeof g_key.src, "%s", src); snprintf(g_key.radio, sizeof g_key.radio, "%s", rs);
    unsigned h = 2166136261u;
    for(int i = 0; i < g_v.n; i++){
        const qv_entry_t *v = &g_v.e[i];
        h = fnv(h, &v->kind, sizeof v->kind); h = fnv(h, &v->base_id, sizeof v->base_id); h = fnv(h, &v->idx, sizeof v->idx);
        h = fnv(h, v->path, strlen(v->path)); h = fnv(h, v->title, strlen(v->title)); h = fnv(h, v->artist, strlen(v->artist));
    }
    h = fnv(h, &g_v.n, sizeof g_v.n); h = fnv(h, &g_v.more, sizeof g_v.more);
    if(h != g_sig || !g_v.ver){ g_sig = h; g_v.ver++; if(!g_v.ver) g_v.ver = 1; }
    return &g_v;
}

int qv_play(const qv_entry_t *v, const char **why){
    if(v->kind != QV_QUEUE && v->kind != QV_NEXT){ *why = "Only a queued or upcoming song can be played from the list"; return QV_E_KIND; }
    if(!ui_local_playback_allowed()){ *why = "The Disc is not in local playback (e.g. USB DAC, Bluetooth or AirPlay mode)"; return QV_E_REFUSED; }
    if(v->kind == QV_QUEUE){ queue_jump(v->idx); qv_invalidate(); return 0; }
    int ord = mdb_queue_ord(v->base_id, v->path);
    if(ord < 0){ *why = "The player is busy - try again"; return QV_E_BUSY; }
    if(ord == 0 || (v->ord && ord != v->ord)){ qv_invalidate(); *why = "The list changed - read it again"; return QV_E_CHANGED; }
    if(ui_queue_jump(ord)){ *why = "The player refused - see the Disc"; return QV_E_REFUSED; }
    qv_invalidate();
    return 0;
}
